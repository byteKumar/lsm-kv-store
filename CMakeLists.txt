cmake_minimum_required(VERSION 3.20)
project(lsm_kv_store LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# LSM_SANITIZER=address builds with ASan + UBSan, LSM_SANITIZER=thread with TSan.
set(LSM_SANITIZER "" CACHE STRING "Sanitizer to build with: address, thread, or empty")
if(LSM_SANITIZER STREQUAL "address")
  add_compile_options(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(LSM_SANITIZER STREQUAL "thread")
  add_compile_options(-O1 -fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT LSM_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown LSM_SANITIZER '${LSM_SANITIZER}'")
endif()

add_library(lsm
  src/crc32.cc
  src/file_util.cc
  src/memtable.cc
  src/sstable.cc
  src/wal.cc
)
target_include_directories(lsm PUBLIC src)
# Warnings for our code only (not for GoogleTest's sources).
target_compile_options(lsm PUBLIC -Wall -Wextra -Wpedantic)

# GoogleTest is downloaded at configure time and built with the same flags
# (including sanitizers) as the rest of the project.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(lsm_tests
  tests/skiplist_test.cc
  tests/memtable_test.cc
  tests/crc32_coding_test.cc
  tests/wal_test.cc
  tests/sstable_test.cc
)
target_link_libraries(lsm_tests PRIVATE lsm GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(lsm_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
