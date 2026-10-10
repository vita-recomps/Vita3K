set(SOURCE_LIST
include/cpu/state.h
include/cpu/aot.h
include/cpu/aot_abi.h
include/cpu/common.h
include/cpu/functions.h
include/cpu/impl/aot_hooks.h
include/cpu/impl/aot_owner.h
include/cpu/impl/interface.h
include/cpu/impl/native_cpu.h
include/cpu/disasm/functions.h
include/cpu/disasm/state.h

src/aot.cpp
src/aot_hooks.cpp
src/disasm.cpp
src/cpu.cpp
src/native_cpu.cpp
)

add_library(
cpu
STATIC
${SOURCE_LIST}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})

target_include_directories(cpu PUBLIC include)
target_link_libraries(cpu PUBLIC mem util)
target_link_libraries(cpu PRIVATE capstone)
if(VITA3K_DYNARMIC)
	target_sources(cpu PRIVATE include/cpu/impl/dynarmic_cpu.h src/dynarmic_cpu.cpp)
	target_link_libraries(cpu PRIVATE dynarmic merry::mcl)
	target_compile_definitions(cpu PRIVATE VITA3K_DYNARMIC=1)
endif()
