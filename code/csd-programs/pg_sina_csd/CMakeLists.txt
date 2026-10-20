add_csd_program(pg-sina-heap-count heap_count.c)
target_include_directories(pg-sina-heap-count PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/pg_sina_csd/include")
if(BUILD_TESTING)
    add_executable(pg-sina-heap-count-test tests/heap_count.c heap_count.c)
    target_include_directories(pg-sina-heap-count-test PRIVATE "${CSD_FEMU_INCLUDE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/pg_sina_csd/include")
    target_compile_options(pg-sina-heap-count-test PRIVATE -UNDEBUG)
    add_test(NAME pgSinaHeapCount COMMAND pg-sina-heap-count-test)
endif()

foreach(operator scan filter aggregate sort limit)
    add_csd_program(pg-sina-${operator} ${operator}.c)
    target_include_directories(pg-sina-${operator} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/pg_sina_csd/include")
endforeach()
if(BUILD_TESTING)
    add_executable(pg-sina-query-test tests/query.c)
    target_include_directories(pg-sina-query-test PRIVATE "${CSD_FEMU_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/pg_sina_csd/include")
    target_compile_options(pg-sina-query-test PRIVATE -UNDEBUG)
    target_link_libraries(pg-sina-query-test PRIVATE cs-workmem ${CMAKE_DL_LIBS})
    add_dependencies(pg-sina-query-test pg-sina-filter pg-sina-aggregate pg-sina-sort pg-sina-limit)
    add_test(NAME pgSinaVectorAggregate COMMAND pg-sina-query-test $<TARGET_FILE:pg-sina-filter>
        $<TARGET_FILE:pg-sina-aggregate> $<TARGET_FILE:pg-sina-sort> $<TARGET_FILE:pg-sina-limit>)
endif()
