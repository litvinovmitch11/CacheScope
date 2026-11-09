# Hybrid instrumentation example

This example isolates the compiler part of CacheScope without PyTorch or the
JIT runner. It demonstrates the three coverage outcomes.

1. [`semantic.mlir`](semantic.mlir) is instrumented at MemRef level. The LLVM
   pass recognizes its provenance marker and does not emit duplicate events.
2. [`raw-llvm.mlir`](raw-llvm.mlir) has no MemRef provenance. Its load and store
   receive `__cachescope_trace_fallback_v1` calls.
3. [`unsupported.mlir`](unsupported.mlir) contains `llvm.intr.memcpy`. The
   strict coverage pass rejects it instead of silently producing a partial
   trace.

Run from the repository root:

```bash
examples/hybrid-instrumentation/run.sh
```

Generated IR is written to `output/hybrid-instrumentation`. The example is
deliberately small enough to use as a template when adding support for another
memory operation.
