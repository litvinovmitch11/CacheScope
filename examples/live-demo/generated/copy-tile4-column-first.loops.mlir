module {
  func.func private @copy_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>) {
    %c1 = arith.constant 1 : index
    %c0 = arith.constant 0 : index
    %c16 = arith.constant 16 : index
    %c4 = arith.constant 4 : index
    scf.for %arg2 = %c0 to %c16 step %c4 {
      scf.for %arg3 = %c0 to %c16 step %c4 {
        %subview = memref.subview %arg0[%arg3, %arg2] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
        %subview_0 = memref.subview %arg1[%arg3, %arg2] [4, 4] [1, 1] : memref<16x16xf32> to memref<4x4xf32, strided<[16, 1], offset: ?>>
        scf.for %arg4 = %c0 to %c4 step %c1 {
          scf.for %arg5 = %c0 to %c4 step %c1 {
            %0 = memref.load %subview[%arg4, %arg5] : memref<4x4xf32, strided<[16, 1], offset: ?>>
            memref.store %0, %subview_0[%arg4, %arg5] : memref<4x4xf32, strided<[16, 1], offset: ?>>
          }
        }
      }
    }
    return
  }
  func.func @main() -> i32 {
    %cst = arith.constant 7.000000e+00 : f32
    %cst_0 = arith.constant 0.000000e+00 : f32
    %c16 = arith.constant 16 : index
    %c15 = arith.constant 15 : index
    %c1 = arith.constant 1 : index
    %c0 = arith.constant 0 : index
    %alloca = memref.alloca() : memref<16x16xf32>
    %alloca_1 = memref.alloca() : memref<16x16xf32>
    scf.for %arg0 = %c0 to %c16 step %c1 {
      scf.for %arg1 = %c0 to %c16 step %c1 {
        memref.store %cst, %alloca[%arg0, %arg1] : memref<16x16xf32>
        memref.store %cst_0, %alloca_1[%arg0, %arg1] : memref<16x16xf32>
      }
    }
    call @copy_kernel(%alloca, %alloca_1) : (memref<16x16xf32>, memref<16x16xf32>) -> ()
    %0 = memref.load %alloca_1[%c15, %c15] : memref<16x16xf32>
    %1 = arith.fptosi %0 : f32 to i32
    return %1 : i32
  }
}

