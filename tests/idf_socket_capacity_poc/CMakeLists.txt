cmake_minimum_required(VERSION 3.16)

# lwIP VFS uses this for both fd_set and its socket descriptor range.
add_compile_definitions(FD_SETSIZE=80)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(woodland_socket_capacity_poc)
