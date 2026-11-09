// RUN: mlir-opt %s --load-pass-plugin=%cachescope_plugin \
// RUN:   --pass-pipeline='builtin.module(cachescope-instrument-llvm-fallback{target-func=kernel})' \
// RUN:   | FileCheck %s

module {
  llvm.func @kernel(%pointer: !llvm.ptr, %value: i32) {
    // CHECK: %[[READ_ADDRESS:.*]] = llvm.ptrtoint %{{.*}} : !llvm.ptr to i64
    // CHECK-NEXT: %[[READ_ID:.*]] = llvm.mlir.constant(4294967296 : i64) : i64
    // CHECK: llvm.call @__cachescope_trace_fallback_v1(%[[READ_ID]], %[[READ_ADDRESS]], %{{.*}}, %{{.*}})
    // CHECK-NEXT: %{{.*}} = llvm.load
    %loaded = llvm.load %pointer : !llvm.ptr -> i32

    // CHECK: %[[WRITE_ADDRESS:.*]] = llvm.ptrtoint %{{.*}} : !llvm.ptr to i64
    // CHECK-NEXT: %[[WRITE_ID:.*]] = llvm.mlir.constant(4294967297 : i64) : i64
    // CHECK: llvm.call @__cachescope_trace_fallback_v1(%[[WRITE_ID]], %[[WRITE_ADDRESS]], %{{.*}}, %{{.*}})
    // CHECK-NEXT: llvm.store
    llvm.store %value, %pointer : i32, !llvm.ptr
    llvm.return
  }
}
