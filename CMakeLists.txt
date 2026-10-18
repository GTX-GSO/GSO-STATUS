cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.7.23")
set(SDKCONFIG_DEFAULTS "config/gss-hosted-reference.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gso_status_screen)
# 禁止组件构造器在GUI/板级初始化前复位C5；由无线工作任务显式启动。
idf_component_get_property(hosted_library espressif__esp_hosted COMPONENT_LIB)
get_target_property(hosted_sources ${hosted_library} SOURCES)
list(FILTER hosted_sources EXCLUDE REGEX "port_esp_hosted_host_init[.]c$")
set_property(TARGET ${hosted_library} PROPERTY SOURCES "${hosted_sources}")
