// RUN: mlir-opt %s --load-pass-plugin=%cachescope_plugin \
// RUN:   --pass-pipeline='builtin.module(cachescope-instrument-memory{target-func=kernel})' \
// RUN:   | FileCheck %s

module {
  func.func private @kernel(
      %scalar: memref<f32>,
      %vector: memref<?xf32>,
      %tensor: memref<2x3x4xf32>,
      %strided: memref<?xf32, strided<[?], offset: ?>>,
      %i: index) {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c2 = arith.constant 2 : index

    // CHECK-LABEL: func.func private @kernel
    // CHECK: memref.extract_strided_metadata %{{.*}} : memref<f32>
    // CHECK: call @__cachescope_trace_v1
    // CHECK-NEXT: %[[SCALAR:.*]] = memref.load %{{.*}}[] : memref<f32>
    %scalar_value = memref.load %scalar[] : memref<f32>

    // CHECK: memref.extract_strided_metadata %{{.*}} : memref<?xf32>
    // CHECK: call @__cachescope_trace_v1
    // CHECK-NEXT: %[[VECTOR:.*]] = memref.load %{{.*}}[%{{.*}}] : memref<?xf32>
    %vector_value = memref.load %vector[%i] : memref<?xf32>

    // CHECK: memref.extract_strided_metadata %{{.*}} : memref<2x3x4xf32>
    // CHECK: call @__cachescope_trace_v1
    // CHECK-NEXT: %[[TENSOR:.*]] = memref.load %{{.*}}[%{{.*}}, %{{.*}}, %{{.*}}] : memref<2x3x4xf32>
    %tensor_value = memref.load %tensor[%c0, %c1, %c2]
      : memref<2x3x4xf32>

    // Dynamic offsets and strides are taken from runtime metadata rather than
    // reconstructed from the static type.
    // CHECK: memref.extract_strided_metadata %{{.*}} : memref<?xf32, strided<[?], offset: ?>>
    // CHECK: call @__cachescope_trace_v1
    // CHECK-NEXT: memref.store %[[TENSOR]], %{{.*}}[%{{.*}}] : memref<?xf32, strided<[?], offset: ?>>
    memref.store %tensor_value, %strided[%i]
      : memref<?xf32, strided<[?], offset: ?>>
    return
  }
}
