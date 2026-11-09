// RUN: not mlir-opt %s --load-pass-plugin=%cachescope_plugin \
// RUN:   --pass-pipeline='builtin.module(cachescope-instrument-memory{target-func=kernel})' \
// RUN:   2>&1 | FileCheck %s

module {
  func.func private @kernel(%buffer: memref<8xf32>) {
    %c0 = arith.constant 0 : index
    // CHECK: error: CacheScope MVP supports only rank-2 memref.load/store
    %value = memref.load %buffer[%c0] : memref<8xf32>
    return
  }
}
