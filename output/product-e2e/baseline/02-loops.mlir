module attributes {cachescope.origins_assigned} {
  func.func private @matmul_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>, %arg2: memref<16x16xf32>) {
    %c0 = arith.constant 0 : index
    %c16 = arith.constant 16 : index
    %c1 = arith.constant 1 : index
    scf.for %arg3 = %c0 to %c16 step %c1 {
      scf.for %arg4 = %c0 to %c16 step %c1 {
        scf.for %arg5 = %c0 to %c16 step %c1 {
          %0 = memref.load %arg0[%arg3, %arg5] : memref<16x16xf32>
          %1 = memref.load %arg1[%arg5, %arg4] : memref<16x16xf32>
          %2 = memref.load %arg2[%arg3, %arg4] : memref<16x16xf32>
          %3 = arith.mulf %0, %1 : f32
          %4 = arith.addf %2, %3 : f32
          memref.store %4, %arg2[%arg3, %arg4] : memref<16x16xf32>
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
