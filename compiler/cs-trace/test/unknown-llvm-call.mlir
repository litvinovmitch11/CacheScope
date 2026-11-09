// RUN: not mlir-opt %s --load-pass-plugin=%cachescope_plugin \
// RUN:   --pass-pipeline='builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})' \
// RUN:   2>&1 | FileCheck %s

module {
  llvm.func @unknown(!llvm.ptr)
  llvm.func @kernel(%pointer: !llvm.ptr) {
    // CHECK: error: call may hide memory accesses
    llvm.call @unknown(%pointer) : (!llvm.ptr) -> ()
    llvm.return
  }
}
