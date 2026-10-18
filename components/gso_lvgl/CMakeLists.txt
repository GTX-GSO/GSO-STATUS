file(GLOB_RECURSE LVGL_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../../vendor/lvgl/src/*.c")
idf_component_register(SRCS ${LVGL_SOURCES} INCLUDE_DIRS "../../vendor/lvgl" "../../simulator")
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)

# Optimize the renderer and shared model for the actual P4 rather than debug stepping.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
