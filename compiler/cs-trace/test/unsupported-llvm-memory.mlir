// RUN: not mlir-opt %s --load-pass-plugin=%cachescope_plugin \
// RUN:   --pass-pipeline='builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})' \
// RUN:   2>&1 | FileCheck %s

module {
  llvm.func @kernel(%destination: !llvm.ptr, %source: !llvm.ptr, %size: i64) {
    // CHECK: error: unsupported explicit LLVM memory operation
    "llvm.intr.memcpy"(%destination, %source, %size) <{isVolatile = false}>
      : (!llvm.ptr, !llvm.ptr, i64) -> ()
    llvm.return
  }
}
