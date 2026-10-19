add_csd_program(csd-vadd csd-vadd.c)
add_csd_program(csd-original-kernels csd-original-kernels.cc)
target_link_libraries(csd-original-kernels PRIVATE lz4)
add_csd_program(cs-example-functions snia_compute_functions.c)
find_program(CLANG_EXECUTABLE clang)
if(CLANG_EXECUTABLE)
    add_custom_target(csd-vadd-bpf
        COMMAND "${CLANG_EXECUTABLE}" -target bpf -O2 -g -c
            "${CMAKE_CURRENT_SOURCE_DIR}/csd-vadd.bpf.c" -o "${CMAKE_BINARY_DIR}/csd-vadd.bpf.o"
        DEPENDS csd-vadd.bpf.c femu-csd-kernel.h)
endif()
add_csd_program(cs-example-sort sort.cpp)
add_csd_program(cs-example-limit limit.c)
target_include_directories(cs-example-sort PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
target_include_directories(cs-example-limit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
if(BUILD_TESTING)
    add_executable(sort-spill-merge tests/sort_spill_merge.cpp)
    target_include_directories(sort-spill-merge PRIVATE "${CSD_FEMU_INCLUDE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
    target_compile_options(sort-spill-merge PRIVATE -UNDEBUG)
    target_link_libraries(sort-spill-merge PRIVATE cs-workmem ${CMAKE_DL_LIBS})
    add_dependencies(sort-spill-merge cs-example-sort cs-example-limit)
    add_test(NAME sortSpillMergeLimit COMMAND sort-spill-merge $<TARGET_FILE:cs-example-sort> $<TARGET_FILE:cs-example-limit>)
endif()
