module attributes {cachescope.instrumented, cachescope.origins_assigned} {
  func.func private @__cachescope_trace_v1(i64, i64, i64, i64, i64)
  func.func private @matmul_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>, %arg2: memref<16x16xf32>) {
    %c3_i64 = arith.constant 3 : i64
    %c2_i64 = arith.constant 2 : i64
    %c1_i64 = arith.constant 1 : i64
    %c4_i64 = arith.constant 4 : i64
    %c0_i64 = arith.constant 0 : i64
    %c4 = arith.constant 4 : index
    %c0 = arith.constant 0 : index
    %c16 = arith.constant 16 : index
    %c1 = arith.constant 1 : index
    scf.for %arg3 = %c0 to %c16 step %c1 {
      scf.for %arg4 = %c0 to %c16 step %c1 {
        scf.for %arg5 = %c0 to %c16 step %c1 {
          %0 = arith.muli %arg3, %c16 : index
          %1 = arith.addi %0, %arg5 : index
          %2 = arith.muli %1, %c4 : index
          %3 = arith.index_cast %2 : index to i64
          func.call @__cachescope_trace_v1(%c0_i64, %c0_i64, %3, %c4_i64, %c0_i64) : (i64, i64, i64, i64, i64) -> ()
          %4 = memref.load %arg0[%arg3, %arg5] : memref<16x16xf32>
          %5 = arith.muli %arg5, %c16 : index
          %6 = arith.addi %5, %arg4 : index
          %7 = arith.muli %6, %c4 : index
          %8 = arith.index_cast %7 : index to i64
          func.call @__cachescope_trace_v1(%c1_i64, %c1_i64, %8, %c4_i64, %c0_i64) : (i64, i64, i64, i64, i64) -> ()
          %9 = memref.load %arg1[%arg5, %arg4] : memref<16x16xf32>
          %10 = arith.addi %0, %arg4 : index
          %11 = arith.muli %10, %c4 : index
          %12 = arith.index_cast %11 : index to i64
          func.call @__cachescope_trace_v1(%c2_i64, %c2_i64, %12, %c4_i64, %c0_i64) : (i64, i64, i64, i64, i64) -> ()
          %13 = memref.load %arg2[%arg3, %arg4] : memref<16x16xf32>
          %14 = arith.mulf %4, %9 : f32
          %15 = arith.addf %13, %14 : f32
          func.call @__cachescope_trace_v1(%c3_i64, %c2_i64, %12, %c4_i64, %c1_i64) : (i64, i64, i64, i64, i64) -> ()
          memref.store %15, %arg2[%arg3, %arg4] : memref<16x16xf32>
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
