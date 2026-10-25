cmake_minimum_required(VERSION 3.16)
project(snia VERSION 1.1 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CTest)
find_package(Threads REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
add_library(snia SHARED
    src/core/cs_discovery.cpp src/core/cs_access.cpp src/core/cs_afdm_management.cpp
    src/core/cs_storage_ios.cpp src/core/cs_csf_scheduling.cpp src/core/cs_batch_scheduling.cpp
    src/core/cs_event_management.cpp src/core/cs_management.cpp src/core/cs_library_management.cpp
    src/core/internal/cs_handle_registry.cpp src/core/internal/cs_request_execution.cpp)
target_include_directories(snia PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> PRIVATE src/core/internal src/interface)
target_link_libraries(snia PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(snia PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
set_property(TARGET snia APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
add_subdirectory(src/plugins/femu)
install(TARGETS snia LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/cs.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
add_executable(cs-batch-sort-avg examples/cs_batch_sort_avg.c)
target_link_libraries(cs-batch-sort-avg PRIVATE snia)
