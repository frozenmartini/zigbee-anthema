cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.2")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
# build only main and what it requires, not every component in ../components
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_base)
