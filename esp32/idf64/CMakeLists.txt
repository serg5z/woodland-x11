cmake_minimum_required(VERSION 3.16)

# ESP-IDF's lwIP VFS uses the same FD_SETSIZE for fd_set and socket descriptor allocation.
add_compile_definitions(FD_SETSIZE=80)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(woodland_x11_64)
