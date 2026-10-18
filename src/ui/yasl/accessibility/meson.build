ui_yasl_accessibility_deps = []
if accessibility_enabled
    ui_yasl_accessibility_src = files('accessibility.c')
    ui_yasl_accessibility_deps += accesskit_dep
else
    ui_yasl_accessibility_src = files('disabled.c')
endif
