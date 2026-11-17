# Vulkan peak RSS

This report covers the resident memory of the Vulkan renderer at startup.
Date: 2026-11-17. GPU: AMD Navi 44 (RADV, Mesa with LLVM 23). Kernel 7.4.

## Method

A private headless sway runs with the GLES2 renderer on the render node, so
the swapchain gets dmabuf. The app runs `sh -c 'sleep 4'`, or a flood of
`seq` and header files. A sampler reads `/proc/PID/status` and
`/proc/PID/smaps` every 30 ms. `VmHWM` is the peak. The breakdown comes from
the sample with the highest `VmRSS`.

## Where the memory goes

A dlopen of `libvulkan_radeon.so` alone, in an empty process, adds 29 MiB.
RADV links `libLLVM.so` (DT_NEEDED). The static constructors of LLVM
(`cl::opt` registration) run in many translation units. Kernel fault-around
maps 64 KiB for each fault. The result is 21.6 MiB of clean, shared code and
read-only data of libLLVM in the RSS. RADV compiles with ACO, so it does not
use those pages again.

The loader also opens every other installed driver (intel, intel_hasvk,
nouveau) to ask for devices. Each one maps its library and its dependencies.
They add about 6 MiB to the peak.

The atlas and the swapchain images are device-local. They are not in the RSS
on a discrete GPU. The host-visible upload and instance buffers are about
2.5 MiB (`RssShmem`).

## Changes

1. `driver_filter()` reads the kernel driver of each `/dev/dri/renderD*` node.
   It sets `VK_LOADER_DRIVERS_SELECT` to the matching manifests while the
   instance is created. A user loader setting or an unknown kernel driver
   disables the filter. If the filtered open fails, the renderer opens again
   with no filter.
2. `driver_pages_release()` runs after instance creation, before the device
   and the pipelines allocate. It applies `MADV_DONTNEED` to the libLLVM
   mappings that are read-only and hold no anonymous pages. It keeps the
   RELRO and writable mappings: their pages are private copies, and
   `MADV_DONTNEED` would reset them to the file contents. A later access to a
   released page maps it again from the page cache. One smaps read costs
   about 0.3 ms.

## Results

| Case | VmHWM | Steady VmRSS |
| --- | --- | --- |
| Before, idle | 48.8 MiB | 47.0 MiB |
| Release only (filter disabled) | 46.3 MiB | 25.7 MiB |
| Filter and release, idle | 40.3 MiB | 28.1 MiB |
| Before, flood | 48.8 MiB | 48.8 MiB |
| Filter and release, flood | 40.1 MiB | 30.0 MiB |
| CPU renderer, idle (reference) | 28.5 MiB | 28.5 MiB |

The steady difference of about 2 MiB between the two "release" rows comes
from libLLVM destructors at exit, which the sampler sees. It is not a
refault during the run (checked with `perf record -e page-faults:u -d`).

## The device before the font system

The peak is the RSS before instance creation plus the driver load. Before
this change, the device started in `window_open`. At that point the process
held 11.0 MiB: 5.5 MiB at exec, and 4.1 MiB from the font system. Now
`main` calls `app_create_gpu_first()`. It creates the device right after it
reads the configuration, at 5.7 MiB. The configuration must come first,
because CPU mode and `--lightweight` must not load the driver.
`app_create()` keeps the device lazy. The tests use it, and some of them
open no window.

Three interleaved runs on 2026-11-17 (morning). All values are lower by
about 2 MiB than in the first table, also for the old build:

| Build | VmHWM |
| --- | --- |
| Before any change | 46.2 to 47.3 MiB |
| Filter and release, device in `window_open` | 38.4 to 38.5 MiB |
| Filter and release, device before the font system | 35.0 to 35.2 MiB |

The steady RSS stays at 26 to 28 MiB. CPU mode (`--renderer cpu`,
`--lightweight`) loads no Vulkan library and stays at 28.2 MiB.

## Floor: a minimal Vulkan window

A minimal client opens one xdg toplevel. It clears the window and presents
120 frames (FIFO, 5 images, 1280x720). It has no fonts, no pipelines, and no
buffers. The values are RSS in MiB. USS is private memory only.

| Step | RSS | Peak | USS |
| --- | --- | --- | --- |
| Process start | 2.4 | 2.4 | 0.2 |
| Wayland window, no buffer | 2.6 | 2.6 | 0.2 |
| `vkCreateInstance` | 39.3 | 39.3 | 12.4 |
| Enumerate devices | 36.9 | 42.3 | 9.6 |
| `vkCreateDevice` | 37.7 | 42.3 | 9.9 |
| Swapchain | 37.9 | 42.3 | 10.0 |
| After 120 frames | 39.4 | 42.3 | 11.0 |
| After 120 frames, filter and release | 16.1 | 32.3 | 7.9 |

Instance creation adds most of the memory: it loads the drivers.
The device, the swapchain, and the frames add 2.5 MiB. The swapchain images
are in VRAM. The loader unloads the other drivers after enumeration, but the
peak already includes them. The compositor (radeonsi) maps the same libLLVM
pages, so the release changes RSS and PSS but not USS. The app adds about
12 MiB over this floor (fonts, shaping, terminal state, UI).

## What remains

The peak is still set during instance creation, when all LLVM constructor
pages are resident at once. The app cannot prevent this: the constructors
run inside dlopen, and fault-around is a global kernel setting. A Mesa build
without LLVM, or a RADV that loads LLVM on demand, removes it.
