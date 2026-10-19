# SPDX-License-Identifier: MIT
#
# SoapySDR module for the W1700K, built the way SoapySDR modules usually are:
#
#   cmake -S . -B build && cmake --build build && sudo cmake --install build
#
# The Makefile builds the same module without CMake.
cmake_minimum_required(VERSION 3.10)
project(SoapyW1700K CXX)

find_package(SoapySDR CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The `w1700k` command the module runs by default (see README.md). A bare name is looked up in PATH.
set(W1700K_CMD "w1700k" CACHE STRING "default w1700k command: a path, or a name looked up in PATH")
set(W1700K_MODULE_VERSION "0.1.0")

# SoapySDR's module directory, from SoapySDRUtil: the first search path it reports.
find_program(SOAPY_SDR_UTIL SoapySDRUtil)
if(SOAPY_SDR_UTIL)
    execute_process(COMMAND ${SOAPY_SDR_UTIL} --info OUTPUT_VARIABLE SOAPY_INFO ERROR_QUIET)
    string(REGEX MATCH "Search path: *([^ \n]+)" SOAPY_SEARCH "${SOAPY_INFO}")
    set(SOAPY_SDR_MODULE_DIR "${CMAKE_MATCH_1}" CACHE PATH "where SoapySDR looks for modules")
endif()

file(GLOB W1700K_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(w1700kSupport MODULE ${W1700K_SOURCES})
target_include_directories(w1700kSupport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(w1700kSupport PRIVATE
    W1700K_CMD="${W1700K_CMD}" W1700K_MODULE_VERSION="${W1700K_MODULE_VERSION}")
target_link_libraries(w1700kSupport PRIVATE SoapySDR nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(w1700kSupport PRIVATE -Wall -Wextra)

if(NOT SOAPY_SDR_MODULE_DIR)
    message(FATAL_ERROR "cannot find SoapySDR's module directory: set SOAPY_SDR_MODULE_DIR")
endif()
install(TARGETS w1700kSupport LIBRARY DESTINATION ${SOAPY_SDR_MODULE_DIR})
