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
                      std::int64_t accessKind, std::int64_t index0,
                      std::int64_t index1);

// LLVM fallback events have no stable memref/indices. They carry the concrete
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
