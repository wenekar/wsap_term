sources += files('backend.c', 'desktop.c', 'events.c', 'input.c', 'native.c', 'timing.c', 'uri.c', 'utf8.c', 'surface.c', 'gl_options.c', 'controls.c', 'move_trace.c')
if has_vulkan
    sources += files('vulkan.c')
    # The application passes PFN_vkGetInstanceProcAddr, so yasl needs only the
    # Vulkan headers and never links the loader.
    deps += vulkan_dep.partial_dependency(compile_args: true, includes: true)
endif
