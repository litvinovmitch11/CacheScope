cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0219)
  cmake_policy(SET CMP0219 NEW)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Export compile_commands.json for clang tooling" FORCE)
project(CacheScope VERSION 0.1.0 LANGUAGES CXX C)

include(CTest)
include(GNUInstallDirs)
include(cmake/LLVMVersion.cmake)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(MLIR REQUIRED CONFIG)

if(NOT LLVM_PACKAGE_VERSION STREQUAL CACHESCOPE_LLVM_PACKAGE_VERSION)
  message(FATAL_ERROR
    "CacheScope requires LLVM ${CACHESCOPE_LLVM_PACKAGE_VERSION}, but "
    "MLIR_DIR points to LLVM ${LLVM_PACKAGE_VERSION}")
endif()

set(CACHESCOPE_LLVM_SOURCE_DIR "${PROJECT_SOURCE_DIR}/llvm-project")
if(EXISTS "${CACHESCOPE_LLVM_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git -C "${CACHESCOPE_LLVM_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE CACHESCOPE_LLVM_ACTUAL_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE CACHESCOPE_LLVM_GIT_RESULT
  )
  if(NOT CACHESCOPE_LLVM_GIT_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot read the llvm-project submodule revision")
  endif()
  if(NOT CACHESCOPE_LLVM_ACTUAL_REVISION STREQUAL CACHESCOPE_LLVM_REVISION)
    message(FATAL_ERROR
      "CacheScope requires llvm-project ${CACHESCOPE_LLVM_REVISION}, but the "
      "submodule is at ${CACHESCOPE_LLVM_ACTUAL_REVISION}")
  endif()
endif()

message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS
  "Using pinned llvm-project revision: ${CACHESCOPE_LLVM_REVISION}")

set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/lib)
set(MLIR_BINARY_DIR ${CMAKE_BINARY_DIR})

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/compiler/cs-trace/include)
link_directories(${LLVM_BUILD_LIBRARY_DIR})

add_subdirectory(compiler/runtime)
add_subdirectory(compiler/cs-trace)

include(cmake/ClangTools.cmake)
cachescope_add_clang_tools()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
