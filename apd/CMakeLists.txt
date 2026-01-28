cmake_minimum_required(VERSION 3.14)
project(apd VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -fvisibility=hidden")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections -Wl,--strip-all")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")

set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
  COMMAND git rev-list --count HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../
  OUTPUT_VARIABLE APD_GIT_COUNT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND git describe --tags --always
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../
  OUTPUT_VARIABLE APD_GIT_DESC
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(APD_GIT_COUNT STREQUAL "")
  set(APD_GIT_COUNT "0")
endif()
if(APD_GIT_DESC STREQUAL "")
  set(APD_GIT_DESC "0.0.0")
endif()

math(EXPR APD_VERSION_CODE "10000 + 200 + ${APD_GIT_COUNT}")
string(REGEX REPLACE "^v" "" APD_VERSION_NAME "${APD_GIT_DESC}")

configure_file(
  ${CMAKE_SOURCE_DIR}/src/version.hpp.in
  ${GENERATED_DIR}/version.hpp
  @ONLY
)

set(SOURCES
  src/main.cpp
  src/cli.cpp
  src/defs.cpp
  src/log.cpp
  src/utils.cpp
  src/assets.cpp
  src/supercall.cpp
  src/package.cpp
  src/module.cpp
  src/metamodule.cpp
  src/event.cpp
  src/sepolicy.cpp
  src/restorecon.cpp
  src/pty.cpp
  src/apd.cpp
)

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${GENERATED_DIR})

add_executable(apd ${SOURCES})

if(NOT ANDROID)
  target_link_libraries(apd PRIVATE pthread)
endif()
if(ANDROID)
  target_link_libraries(apd PRIVATE log)
endif()

if(DEFINED APD_OUTPUT_DIR)
  set_target_properties(apd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${APD_OUTPUT_DIR}")
endif()
