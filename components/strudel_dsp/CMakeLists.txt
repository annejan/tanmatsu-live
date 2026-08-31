idf_component_register(
	SRCS
		"sd_synth.c"
	INCLUDE_DIRS
		"include"
)

# The project builds at -Og for debuggability, which leaves the per sample
# helpers un-inlined. This is the one component where that is not affordable.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-lm")
