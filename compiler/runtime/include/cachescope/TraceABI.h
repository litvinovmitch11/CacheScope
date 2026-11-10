#ifndef CACHESCOPE_TRACEABI_H
#define CACHESCOPE_TRACEABI_H

#include <cstdint>

#if defined(_WIN32)
#define CACHESCOPE_EXPORT __declspec(dllexport)
#else
#define CACHESCOPE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" CACHESCOPE_EXPORT void
__cachescope_trace_v1(std::int64_t accessId, std::int64_t memrefId,
                      std::int64_t byteOffset, std::int64_t sizeBytes,
                      std::int64_t accessKind);

// The compiler emits one registration call for every static access site in the
// target function. Registrations execute in the function prologue, so the
// access table does not depend on control flow or the dynamic trace limit.
// addressKind is 0 for a semantic memref offset and 1 for a process address.
extern "C" CACHESCOPE_EXPORT void
__cachescope_register_access_v1(std::int64_t accessId, std::int64_t memrefId,
                                std::int64_t accessKind,
                                std::int64_t addressKind);
extern "C" CACHESCOPE_EXPORT void
__cachescope_register_source_v1(std::int64_t accessId,
                                const char *sourceLocation);
// Source-map metadata crosses the ABI as typed fields. The runtime validates
// references and serializes the optional JSON extension atomically.
extern "C" CACHESCOPE_EXPORT void
__cachescope_register_source_map_v1(const char *payloadFingerprint);
extern "C" CACHESCOPE_EXPORT void
__cachescope_register_stage_v1(std::int64_t ordinal, const char *stageId,
                               const char *path);
extern "C" CACHESCOPE_EXPORT void
__cachescope_register_origin_v1(std::int64_t originId, const char *stageId,
                                std::int64_t line, std::int64_t column,
                                const char *operation);
extern "C" CACHESCOPE_EXPORT void
__cachescope_register_access_origin_v1(std::int64_t accessId,
                                       std::int64_t originId);
extern "C" CACHESCOPE_EXPORT void __cachescope_register_stage_location_v1(
    std::int64_t accessId, const char *stageId, std::int64_t line,
    std::int64_t column, const char *operation);

// LLVM fallback events have no stable memref identity. They carry the concrete
// process address so no explicit LLVM memory operation is silently omitted.
extern "C" CACHESCOPE_EXPORT void
__cachescope_trace_fallback_v1(std::int64_t accessId, std::int64_t address,
                               std::int64_t sizeBytes, std::int64_t accessKind);

// Explicit lifecycle hooks are useful for an instrumented shared library that
// is invoked from a long-lived process such as Python. The atexit flush remains
// a fallback for standalone executables.
extern "C" CACHESCOPE_EXPORT void __cachescope_trace_flush_v1();
extern "C" CACHESCOPE_EXPORT void __cachescope_trace_reset_v1();

#endif // CACHESCOPE_TRACEABI_H
