add_llvm_library(CacheScopePassPlugin
  MODULE BUILDTREE_ONLY
  lib/InstrumentMemory.cpp
  lib/InstrumentLLVMFallback.cpp
  lib/SourceMapping.cpp
  plugin/CacheScopePassPlugin.cpp

  PLUGIN_TOOL
  mlir-opt

  LINK_LIBS
  MLIRArithDialect
  MLIRFuncDialect
  MLIRIR
  MLIRMemRefDialect
  MLIRPass
  MLIRSupport
)

target_include_directories(CacheScopePassPlugin
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(CacheScopePassPlugin PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

install(TARGETS CacheScopePassPlugin
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/cachescope
)
