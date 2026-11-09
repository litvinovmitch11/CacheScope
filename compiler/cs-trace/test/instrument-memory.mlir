// RUN: mlir-opt %s --load-pass-plugin=%cachescope_plugin \
// RUN:   --pass-pipeline='builtin.module(cachescope-instrument-memory{target-func=kernel})' \
// RUN:   | FileCheck %s

module {
  // CHECK: func.func private @__cachescope_trace_v1(i64, i64, i64, i64, i64, i64, i64)
  func.func private @kernel(%a: memref<8x8xf32>, %b: memref<8x8xf32>) {
    %c0 = arith.constant 0 : index
    %tile = memref.subview %a[4, 4] [4, 4] [1, 1]
      : memref<8x8xf32> to memref<4x4xf32, strided<[8, 1], offset: 36>>

    // CHECK: memref.extract_strided_metadata %{{.*}}
    // CHECK: call @__cachescope_trace_v1(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}})
    // CHECK-NEXT: %{{.*}} = memref.load
    %value = memref.load %tile[%c0, %c0]
      : memref<4x4xf32, strided<[8, 1], offset: 36>>

    // CHECK: call @__cachescope_trace_v1(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}})
    // CHECK-NEXT: memref.store
    memref.store %value, %b[%c0, %c0] : memref<8x8xf32>

    // Allocations receive stable IDs after all function arguments. Here the
    // two arguments are IDs 0 and 1, so this buffer is memref ID 2.
    %temporary = memref.alloc() : memref<8x8xf32>
    // CHECK: %[[ACCESS_ID:.*]] = arith.constant 2 : i64
    // CHECK-NEXT: %[[ALLOC_ID:.*]] = arith.constant 2 : i64
    // CHECK: call @__cachescope_trace_v1(%[[ACCESS_ID]], %[[ALLOC_ID]], %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}}, %{{.*}})
    // CHECK-NEXT: memref.store
    memref.store %value, %temporary[%c0, %c0] : memref<8x8xf32>
    memref.dealloc %temporary : memref<8x8xf32>
    return
  }

  // CHECK-LABEL: func.func @main
  // CHECK-NOT: __cachescope_trace_v1
  func.func @main() {
    %c0 = arith.constant 0 : index
    %buffer = memref.alloc() : memref<8x8xf32>
    %unused = memref.load %buffer[%c0, %c0] : memref<8x8xf32>
    memref.dealloc %buffer : memref<8x8xf32>
    return
  }
}
