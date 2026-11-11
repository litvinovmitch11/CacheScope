module attributes {cachescope.instrumented, cachescope.origins_assigned} {
  func.func private @__cachescope_trace_v1(i64, i64, i64, i64, i64)
  func.func private @matmul_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>, %arg2: memref<16x16xf32>) {
    %c3_i64 = arith.constant 3 : i64
    %c2_i64 = arith.constant 2 : i64
    %c1_i64 = arith.constant 1 : i64
    %c4_i64 = arith.constant 4 : i64
    %c0_i64 = arith.constant 0 : i64
    %c1 = arith.constant 1 : index
    %c0 = arith.constant 0 : index
    %c16 = arith.constant 16 : index
    %c4 = arith.constant 4 : index
    scf.for %arg3 = %c0 to %c16 step %c4 {
      scf.for %arg4 = %c0 to %c16 step %c4 {
        scf.for %arg5 = %c0 to %c16 step %c4 {
          %subview = memref.subview %arg0[%arg3, %arg5] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
          %subview_0 = memref.subview %arg1[%arg5, %arg4] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
          %subview_1 = memref.subview %arg2[%arg3, %arg4] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
          scf.for %arg6 = %c0 to %c4 step %c1 {
            scf.for %arg7 = %c0 to %c4 step %c1 {
              scf.for %arg8 = %c0 to %c4 step %c1 {
                %base_buffer, %offset, %sizes:2, %strides:2 = memref.extract_strided_metadata %subview : memref<4x4xf32, strided<[16, 1], offset: ?>> -> memref<f32>, index, index, index, index, index
                %0 = arith.muli %arg6, %c16 : index
                %1 = arith.addi %offset, %0 : index
                %2 = arith.addi %1, %arg8 : index
                %3 = arith.muli %2, %c4 : index
                %4 = arith.index_cast %3 : index to i64
                func.call @__cachescope_trace_v1(%c0_i64, %c0_i64, %4, %c4_i64, %c0_i64) : (i64, i64, i64, i64, i64) -> ()
                %5 = memref.load %subview[%arg6, %arg8] : memref<4x4xf32, strided<[16, 1], offset: ?>>
                %base_buffer_2, %offset_3, %sizes_4:2, %strides_5:2 = memref.extract_strided_metadata %subview_0 : memref<4x4xf32, strided<[16, 1], offset: ?>> -> memref<f32>, index, index, index, index, index
                %6 = arith.muli %arg8, %c16 : index
                %7 = arith.addi %offset_3, %6 : index
                %8 = arith.addi %7, %arg7 : index
                %9 = arith.muli %8, %c4 : index
                %10 = arith.index_cast %9 : index to i64
                func.call @__cachescope_trace_v1(%c1_i64, %c1_i64, %10, %c4_i64, %c0_i64) : (i64, i64, i64, i64, i64) -> ()
                %11 = memref.load %subview_0[%arg8, %arg7] : memref<4x4xf32, strided<[16, 1], offset: ?>>
                %base_buffer_6, %offset_7, %sizes_8:2, %strides_9:2 = memref.extract_strided_metadata %subview_1 : memref<4x4xf32, strided<[16, 1], offset: ?>> -> memref<f32>, index, index, index, index, index
                %12 = arith.addi %offset_7, %0 : index
                %13 = arith.addi %12, %arg7 : index
                %14 = arith.muli %13, %c4 : index
                %15 = arith.index_cast %14 : index to i64
                func.call @__cachescope_trace_v1(%c2_i64, %c2_i64, %15, %c4_i64, %c0_i64) : (i64, i64, i64, i64, i64) -> ()
                %16 = memref.load %subview_1[%arg6, %arg7] : memref<4x4xf32, strided<[16, 1], offset: ?>>
                %17 = arith.mulf %5, %11 : f32
                %18 = arith.addf %16, %17 : f32
                func.call @__cachescope_trace_v1(%c3_i64, %c2_i64, %15, %c4_i64, %c1_i64) : (i64, i64, i64, i64, i64) -> ()
                memref.store %18, %subview_1[%arg6, %arg7] : memref<4x4xf32, strided<[16, 1], offset: ?>>
              }
            }
          }
        }
      }
    }
    return
  }
  func.func @main() -> i32 {
    %cst = arith.constant 1.000000e+00 : f32
    %cst_0 = arith.constant 0.000000e+00 : f32
    %c16 = arith.constant 16 : index
    %c1 = arith.constant 1 : index
    %c0 = arith.constant 0 : index
    %alloca = memref.alloca() : memref<16x16xf32>
    %alloca_1 = memref.alloca() : memref<16x16xf32>
    %alloca_2 = memref.alloca() : memref<16x16xf32>
    scf.for %arg0 = %c0 to %c16 step %c1 {
      scf.for %arg1 = %c0 to %c16 step %c1 {
        memref.store %cst, %alloca[%arg0, %arg1] : memref<16x16xf32>
        memref.store %cst, %alloca_1[%arg0, %arg1] : memref<16x16xf32>
        memref.store %cst_0, %alloca_2[%arg0, %arg1] : memref<16x16xf32>
      }
    }
    call @matmul_kernel(%alloca, %alloca_1, %alloca_2) : (memref<16x16xf32>, memref<16x16xf32>, memref<16x16xf32>) -> ()
    %0 = memref.load %alloca_2[%c0, %c0] : memref<16x16xf32>
    %1 = arith.fptosi %0 : f32 to i32
    return %1 : i32
  }
}
