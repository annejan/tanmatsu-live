idf_component_register(
	SRCS
		"sd_frac.c"
		"sd_pattern.c"
		"sd_mini.c"
	INCLUDE_DIRS
		"include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
