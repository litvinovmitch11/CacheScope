function(cachescope_add_clang_tools)
  file(GLOB_RECURSE CACHESCOPE_FORMAT_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/compiler/*.cpp"
    "${PROJECT_SOURCE_DIR}/compiler/*.h"
  )
  file(GLOB_RECURSE CACHESCOPE_TIDY_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/compiler/*.cpp"
  )

  set(CACHESCOPE_CLANG_TOOLS_DIR "${LLVM_TOOLS_BINARY_DIR}" CACHE PATH
      "Directory containing clang-format and clang-tidy")
  find_program(CACHESCOPE_CLANG_FORMAT
    NAMES clang-format
    HINTS "${CACHESCOPE_CLANG_TOOLS_DIR}"
  )
  find_program(CACHESCOPE_CLANG_TIDY
    NAMES clang-tidy
    HINTS "${CACHESCOPE_CLANG_TOOLS_DIR}"
  )

  if(CACHESCOPE_CLANG_FORMAT)
    add_custom_target(format
      COMMAND ${CACHESCOPE_CLANG_FORMAT} -i ${CACHESCOPE_FORMAT_FILES}
      COMMENT "Formatting CacheScope C++ sources with LLVM style"
      VERBATIM
    )
    add_custom_target(format-check
      COMMAND ${CACHESCOPE_CLANG_FORMAT} --dry-run --Werror
              ${CACHESCOPE_FORMAT_FILES}
      COMMENT "Checking LLVM C++ formatting"
      VERBATIM
    )
  else()
    message(STATUS
      "clang-format not found; build the LLVM clang-format target to enable "
      "the format and format-check targets")
  endif()

  if(CACHESCOPE_CLANG_TIDY)
    add_custom_target(tidy
      COMMAND ${CACHESCOPE_CLANG_TIDY}
              -p=${CMAKE_BINARY_DIR}
              --config-file=${PROJECT_SOURCE_DIR}/.clang-tidy
              --quiet
              ${CACHESCOPE_TIDY_FILES}
      DEPENDS CacheScopePassPlugin cachescope_trace_runtime
      COMMENT "Running clang-tidy with build/compile_commands.json"
      VERBATIM
    )
  else()
    message(STATUS
      "clang-tidy not found; build the LLVM clang-tidy target to enable the "
      "tidy target")
  endif()
endfunction()
