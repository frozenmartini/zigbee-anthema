cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
# build only main and what it requires
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_sniff)
