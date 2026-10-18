idf_component_register(SRCS "../../vendor/cjson/cJSON.c" INCLUDE_DIRS "../../vendor/cjson")
target_compile_definitions(${COMPONENT_LIB} PRIVATE CJSON_NESTING_LIMIT=32)
