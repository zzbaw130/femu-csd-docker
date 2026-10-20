add_csd_program(csd-vadd csd-vadd.c)
add_csd_program(csd-original-kernels csd-original-kernels.cc)
target_link_libraries(csd-original-kernels PRIVATE lz4)
find_program(CLANG_EXECUTABLE clang)
if(CLANG_EXECUTABLE)
    add_custom_target(csd-vadd-bpf
        COMMAND "${CLANG_EXECUTABLE}" -target bpf -O2 -g -c
            "${CMAKE_CURRENT_SOURCE_DIR}/csd-vadd.bpf.c" -o "${CMAKE_BINARY_DIR}/csd-vadd.bpf.o"
        DEPENDS csd-vadd.bpf.c femu-csd-kernel.h)
endif()
add_csd_program(cs-example-sort sort.cpp)
add_csd_program(cs-example-avg avg.c)
target_include_directories(cs-example-sort PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
target_include_directories(cs-example-avg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
# Independent Guest CPU benchmark, sharing only the allocator and run-sort code.
add_executable(read-sort-avg tests/read_sort_avg.c)
target_include_directories(read-sort-avg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
target_link_libraries(read-sort-avg PRIVATE cs-workmem)
if(BUILD_TESTING)
    add_executable(sort-spill-merge tests/sort_spill_merge.cpp)
    target_include_directories(sort-spill-merge PRIVATE "${CSD_FEMU_INCLUDE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/snia-cs-api/examples")
    target_compile_options(sort-spill-merge PRIVATE -UNDEBUG)
    target_link_libraries(sort-spill-merge PRIVATE cs-workmem ${CMAKE_DL_LIBS})
    add_dependencies(sort-spill-merge cs-example-sort cs-example-avg)
    add_test(NAME sortSpillMergeAverage COMMAND sort-spill-merge $<TARGET_FILE:cs-example-sort> $<TARGET_FILE:cs-example-avg>)
endif()
