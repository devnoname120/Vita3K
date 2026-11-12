add_library(
	shader
	STATIC
	src/translator/alu.cpp
	src/translator/ialu.cpp
	src/translator/branch_cond.cpp
	src/translator/data.cpp
	src/translator/special.cpp
	src/translator/illegal.cpp
	src/translator/texture.cpp
	src/translator/utils.cpp
	src/gxp_parser.cpp
	src/usse_disasm.cpp
	src/usse_program_analyzer.cpp
	src/usse_decode_helpers.cpp
	src/usse_translator_entry.cpp
	src/usse_utilities.cpp
	src/spirv_recompiler.cpp
)

target_include_directories(shader PUBLIC include)
target_link_libraries(shader PUBLIC features gxm util)
target_link_libraries(shader PRIVATE SPIRV spirv-cross-glsl)

# Marshmallow Tracy linking
if(TRACY_ENABLE_ON_CORE_COMPONENTS)
	target_link_libraries(shader PRIVATE tracy)
endif()


if(NOT ANDROID)
	find_package(Python3 QUIET COMPONENTS Interpreter)
	if(Python3_Interpreter_FOUND)
		add_test(
			NAME shader-literal-input-bit-pattern
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/tests/literal_input_bit_pattern_test.py
				${CMAKE_CURRENT_SOURCE_DIR}/include/shader/usse_types.h
				${CMAKE_CURRENT_SOURCE_DIR}/src/gxp_parser.cpp
				${CMAKE_CURRENT_SOURCE_DIR}/src/spirv_recompiler.cpp
		)
		add_test(
			NAME shader-indexed-register-wrap
			COMMAND ${Python3_EXECUTABLE}
				${CMAKE_CURRENT_SOURCE_DIR}/tests/indexed_register_wrap_test.py
				${CMAKE_SOURCE_DIR}
		)
	endif()
endif()
