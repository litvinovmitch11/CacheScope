module {
  func.func private @matmul_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>, %arg2: memref<16x16xf32>) {
    %c0 = arith.constant 0 : index
    %c0_0 = arith.constant 0 : index
    %c0_1 = arith.constant 0 : index
    %c16 = arith.constant 16 : index
    %c16_2 = arith.constant 16 : index
    %c16_3 = arith.constant 16 : index
    %c4 = arith.constant 4 : index
    %c4_4 = arith.constant 4 : index
    %c4_5 = arith.constant 4 : index
    scf.for %arg3 = %c0 to %c16 step %c4 {
      scf.for %arg4 = %c0_0 to %c16_2 step %c4_4 {
        scf.for %arg5 = %c0_1 to %c16_3 step %c4_5 {
          %subview = memref.subview %arg0[%arg3, %arg4] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
          %subview_6 = memref.subview %arg1[%arg4, %arg5] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
          %subview_7 = memref.subview %arg2[%arg3, %arg5] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
          linalg.matmul ins(%subview, %subview_6 : memref<4x4xf32, strided<[16, 1], offset: ?>>, memref<4x4xf32, strided<[16, 1], offset: ?>>) outs(%subview_7 : memref<4x4xf32, strided<[16, 1], offset: ?>>)
        }
      }
    }
    return
  }
  func.func @main() -> i32 {
    %alloca = memref.alloca() : memref<16x16xf32>
    %alloca_0 = memref.alloca() : memref<16x16xf32>
    %alloca_1 = memref.alloca() : memref<16x16xf32>
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c16 = arith.constant 16 : index
    %cst = arith.constant 0.000000e+00 : f32
    %cst_2 = arith.constant 1.000000e+00 : f32
    scf.for %arg0 = %c0 to %c16 step %c1 {
      scf.for %arg1 = %c0 to %c16 step %c1 {
        memref.store %cst_2, %alloca[%arg0, %arg1] : memref<16x16xf32>
        memref.store %cst_2, %alloca_0[%arg0, %arg1] : memref<16x16xf32>
        memref.store %cst, %alloca_1[%arg0, %arg1] : memref<16x16xf32>
      }
    }
    call @matmul_kernel(%alloca, %alloca_0, %alloca_1) : (memref<16x16xf32>, memref<16x16xf32>, memref<16x16xf32>) -> ()
    %0 = memref.load %alloca_1[%c0, %c0] : memref<16x16xf32>
    %1 = arith.fptosi %0 : f32 to i32
    return %1 : i32
  }
}

