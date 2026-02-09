#This is primarily a cpp code but will use log4c to log both the C and C++ aspects
include(FindPkgConfig)
pkg_check_modules(MERCURY REQUIRED IMPORTED_TARGET mercury)
pkg_check_modules(LOG4C REQUIRED IMPORTED_TARGET log4c)
include_directories("$ENV{CONDA_PREFIX}/include")
link_directories("$ENV{CONDA_PREFIX}/lib")

#Dynamic Target
add_library(fitcache_client SHARED fitcache_client.cpp fitcache_comm.cpp fitcache_comm_client.cpp wrappers.c fitcache_logging.c fitcache_multi_source_read.cpp fitcache_real_functions.cpp)
target_compile_definitions(fitcache_client PUBLIC FitCache_CLIENT)
target_compile_definitions(fitcache_client PUBLIC FitCache_PRELOAD)
target_include_directories(fitcache_client PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(fitcache_client PRIVATE pthread dl PkgConfig::LOG4C PkgConfig::MERCURY)

#Server Daemon
add_executable(fitcache_server fitcache_server.cpp fitcache_data_mover.cpp fitcache_comm.cpp fitcache_comm_server.cpp fitcache_logging.c ) # fitcache_cache_policy.cpp
target_compile_definitions(fitcache_server PUBLIC FitCache_SERVER)
target_include_directories(fitcache_server PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(fitcache_server PRIVATE pthread PkgConfig::LOG4C rt PkgConfig::MERCURY)

install(TARGETS fitcache_client DESTINATION lib)
install(TARGETS fitcache_server DESTINATION bin)
