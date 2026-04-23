cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" WIFINDER_VERSION_RAW)
string(STRIP "${WIFINDER_VERSION_RAW}" WIFINDER_VERSION)
if(WIFINDER_VERSION STREQUAL "")
  message(FATAL_ERROR "VERSION file is empty")
endif()
set(PROJECT_VER "${WIFINDER_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifinder)
