cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(CacheScopeSimulator LANGUAGES CXX)
  include(CTest)
endif()

add_library(cachescope_simulator
  lib/SetAssociativeCacheLevel.cpp
  lib/LRU.cpp
)
target_compile_features(cachescope_simulator PUBLIC cxx_std_17)
target_include_directories(cachescope_simulator PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(BUILD_TESTING)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  )
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_executable(cachescope_cache_level_tests
    tests/SetAssociativeCacheLevelTest.cpp
    tests/LRUTest.cpp
  )
  target_link_libraries(cachescope_cache_level_tests PRIVATE cachescope_simulator GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(cachescope_cache_level_tests)
endif()
