set(SOURCE_LIST
include/cpu/state.h
include/cpu/common.h
include/cpu/functions.h
include/cpu/impl/dynarmic_cpu.h
include/cpu/impl/interface.h
include/cpu/disasm/functions.h
include/cpu/disasm/state.h

src/disasm.cpp
src/cpu.cpp
src/dynarmic_cpu.cpp
)

add_library(
cpu
STATIC
${SOURCE_LIST}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})

target_include_directories(cpu PUBLIC include)
target_link_libraries(cpu PUBLIC mem util)
target_link_libraries(cpu PRIVATE dynarmic capstone merry::mcl)

if(NOT ANDROID)
    add_executable(
        cpu-tests
        tests/context_tests.cpp
    )

    target_include_directories(cpu-tests PRIVATE include)
    target_link_libraries(cpu-tests PRIVATE cpu googletest util)
    add_test(NAME cpu COMMAND cpu-tests)
endif()
