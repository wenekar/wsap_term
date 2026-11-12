# Resize Latency

This report covers the time from a configure event to the commit of a frame at
the new size, during a 1000 Hz resize drag.

## Findings

The Vulkan path asked the driver for the present modes and the formats of the
surface on each frame. On Wayland, Mesa answers each of these queries with a
new registry and three waits for the compositor. A frame at the same size made
two queries. A frame at a new size made four. The queries were 61% of the time
of a resize frame, and almost all the time of a frame at the same size.

The CPU path made a new zeroed canvas on each size change, copied the old
pixels into it, and freed the old canvas. The allocation was a new mapping each
time, so each step paid a page fault for each page and one `munmap`.

## What changed

- `src/render/gpu/vulkan/swapchain.c`: `vulkan_swapchain_ensure()` returns at
  once when the surface, the requested size, and the pacing are the same as
  for the last frame. It asks the driver nothing. `traits_ensure()`
  reads the present modes and the format one time for each surface. A failed
  swapchain creation clears the stored answers.
- The driver result `VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR`, from an
  acquire or from a present, sets `recheck`. The next call then reads the
  surface capabilities again. X11 needs this, because the extent of an X11
  swapchain comes from the window and not from the request.
- `subprojects/yasl/src/common/surface.c`: `canvas_fit()` keeps an allocation
  that is half as large again as the canvas. It moves the rows in place when
  the size changes. The public contract is the same: a resize keeps the
  overlap and zeroes the rest.
- `src/platform/linux/platform.c`: `platform_reserve_fds()` expands the
  file descriptor table of the process to 1024 entries at start-up.
  This avoids an `expand_fdtable()` RCU synchronization stall during the
  first swapchain recreation.

## Method

`scripts/wsap-resize-latency`, 3000 ticks at 1000 Hz, in a private headless
sway 1.12. Host: Ryzen 7 9700X, Radeon RX 9060 XT, RADV of Mesa 27.0-devel,
Linux 7.4, release build with LTO. One run of each configuration before and
after the change. sway logs the protocol, and the log makes each registry
more expensive than it is in normal use.

To find the cause, count the `get_registry` requests between an
`ack_configure` and the next `commit` in `sway.log`. The count must be zero.

## Results

Configure to resized, in milliseconds:

| Configuration            | p50 before | p50 after | p99 before | p99 after |
|--------------------------|-----------:|----------:|-----------:|----------:|
| Vulkan, empty scrollback |      0.582 |     0.186 |      1.096 |     0.361 |
| Vulkan, 50000 lines      |      0.598 |     0.225 |      1.282 |     0.493 |
| CPU, empty scrollback    |      0.193 |     0.143 |      0.904 |     0.359 |
| CPU, 50000 lines         |      0.209 |     0.170 |      0.969 |     0.409 |

Tick to shown, in milliseconds:

| Configuration            | p50 before | p50 after | p99 before | p99 after |
|--------------------------|-----------:|----------:|-----------:|----------:|
| Vulkan, empty scrollback |      0.802 |     0.268 |      1.807 |     0.478 |
| Vulkan, 50000 lines      |      0.968 |     0.354 |      1.990 |     0.607 |
| CPU, empty scrollback    |      0.275 |     0.159 |      1.346 |     0.330 |
| CPU, 50000 lines         |      0.286 |     0.180 |      1.872 |     0.393 |

A Vulkan frame at an unchanged size took 245 µs from the acknowledgement to
the commit before the change, and 5 µs after it. This applies to each frame of
normal output, not only to a resize.

## What remains

A Vulkan resize frame now takes about 210 µs. `vkCreateSwapchainKHR` uses about
150 µs of it: Mesa frees four images, allocates four images, and waits for the
compositor one time.

An experiment rounded the swapchain extent up to a multiple of 256 pixels, so
most resize steps kept the swapchain. Configure to resized fell to 0.081 ms at
p50 and 0.166 ms at p99. This is not in the tree. A correct version must crop
the buffer with `wp_viewport.set_source` in the same commit as the buffer. A
source rectangle that is larger than the attached buffer is a fatal protocol
error. The gain is 0.1 ms on a frame that a display shows for 4 ms or more, so
the risk is larger than the gain.

## Resolved issues

- The first resize took 6 ms to 8 ms in `vkCreateSwapchainKHR`.
  The cause was kernel descriptor table expansion in `expand_fdtable()`,
  which waited for `synchronize_rcu()` in multithreaded processes.
  The process reached 64 descriptors during the first resize when the old
  and new swapchains coexisted.
  Calling `platform_reserve_fds()` expands the table to 1024 descriptors
  at start-up.
  Maximum configure-to-resized time across nine runs fell from 5.9 ms - 8.3 ms
  to 0.77 ms - 1.47 ms. Percentiles did not change.
- A resize frame occasionally waited 16.7 ms during high terminal output.
  Rapid content submissions exhausted all frame slots before the resize
  frame arrived. `render_gpu_draw()` dropped the frame, triggering a
  display-paced retry.
  `window_vulkan` now marks resize transactions when the surface size changes.
  When a resize transaction is in progress and all slots are busy,
  `slot_acquire()` waits up to 20 ms on the oldest slot fence instead of
  dropping the frame.
- When `vkAcquireNextImageKHR` returned `VK_SUBOPTIMAL_KHR` and the swapchain
  did not change, the renderer acquired a second image from the same swapchain.
  The first image was not presented and the semaphore was reused with a
  pending signal.
  `vulkan_swapchain_acquire()` now unifies image acquisition.
  On `VK_SUBOPTIMAL_KHR`, the renderer keeps the acquired image if the
  swapchain does not change.
  If the swapchain changes, it recreates the slot semaphore and acquires
  from the new swapchain.
  On `VK_ERROR_OUT_OF_DATE_KHR`, it forces swapchain recreation even when
  extent and mode match.
- On X11, the Vulkan surface extent comes from window geometry.
  During fast resizes, the window geometry can lag behind the requested size.
  `vulkan_swapchain_ensure()` now records the actual swapchain extent.
  It sets `recheck` to query capabilities again on the next frame when
  the extent differs from the requested size.
