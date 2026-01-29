cmake_minimum_required(VERSION 3.14)
project(apd VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

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

FetchContent_Declare(
  lua
  URL https://www.lua.org/ftp/lua-5.5.0.tar.gz
  URL_HASH SHA256=57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(lua)

set(LUA_SOURCES
  ${lua_SOURCE_DIR}/src/lapi.c
  ${lua_SOURCE_DIR}/src/lcode.c
  ${lua_SOURCE_DIR}/src/lctype.c
  ${lua_SOURCE_DIR}/src/ldebug.c
  ${lua_SOURCE_DIR}/src/ldo.c
  ${lua_SOURCE_DIR}/src/ldump.c
  ${lua_SOURCE_DIR}/src/lfunc.c
  ${lua_SOURCE_DIR}/src/lgc.c
  ${lua_SOURCE_DIR}/src/llex.c
  ${lua_SOURCE_DIR}/src/lmem.c
  ${lua_SOURCE_DIR}/src/lobject.c
  ${lua_SOURCE_DIR}/src/lopcodes.c
  ${lua_SOURCE_DIR}/src/lparser.c
  ${lua_SOURCE_DIR}/src/lstate.c
  ${lua_SOURCE_DIR}/src/lstring.c
  ${lua_SOURCE_DIR}/src/ltable.c
  ${lua_SOURCE_DIR}/src/ltm.c
  ${lua_SOURCE_DIR}/src/lundump.c
  ${lua_SOURCE_DIR}/src/lvm.c
  ${lua_SOURCE_DIR}/src/lzio.c
  ${lua_SOURCE_DIR}/src/lauxlib.c
  ${lua_SOURCE_DIR}/src/lbaselib.c
  ${lua_SOURCE_DIR}/src/lcorolib.c
  ${lua_SOURCE_DIR}/src/ldblib.c
  ${lua_SOURCE_DIR}/src/liolib.c
  ${lua_SOURCE_DIR}/src/lmathlib.c
  ${lua_SOURCE_DIR}/src/loslib.c
  ${lua_SOURCE_DIR}/src/lstrlib.c
  ${lua_SOURCE_DIR}/src/ltablib.c
  ${lua_SOURCE_DIR}/src/lutf8lib.c
  ${lua_SOURCE_DIR}/src/loadlib.c
  ${lua_SOURCE_DIR}/src/linit.c
)

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR}/src)
target_compile_definitions(lua PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
if(ANDROID)
  target_link_libraries(lua PRIVATE dl)
endif()

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
target_compile_definitions(apd PRIVATE APD_USE_LUA)
target_link_libraries(apd PRIVATE lua)

if(NOT ANDROID)
  target_link_libraries(apd PRIVATE pthread)
endif()
if(ANDROID)
  target_link_libraries(apd PRIVATE log)
endif()

if(DEFINED APD_OUTPUT_DIR)
  set_target_properties(apd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${APD_OUTPUT_DIR}")
endif()
