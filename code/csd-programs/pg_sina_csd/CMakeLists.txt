add_csd_program(pg-sina-heap-count heap_count.c)
target_include_directories(pg-sina-heap-count PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/pg_sina_csd/include")
if(BUILD_TESTING)
    add_executable(pg-sina-heap-count-test tests/heap_count.c heap_count.c)
    target_include_directories(pg-sina-heap-count-test PRIVATE "${CSD_FEMU_INCLUDE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/pg_sina_csd/include")
    target_compile_options(pg-sina-heap-count-test PRIVATE -UNDEBUG)
    add_test(NAME pgSinaHeapCount COMMAND pg-sina-heap-count-test)
endif()
