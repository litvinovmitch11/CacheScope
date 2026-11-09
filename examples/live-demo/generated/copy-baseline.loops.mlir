module {
  func.func private @copy_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>) {
    %c0 = arith.constant 0 : index
    %c16 = arith.constant 16 : index
    %c1 = arith.constant 1 : index
    scf.for %arg2 = %c0 to %c16 step %c1 {
      scf.for %arg3 = %c0 to %c16 step %c1 {
        %0 = memref.load %arg0[%arg2, %arg3] : memref<16x16xf32>
        memref.store %0, %arg1[%arg2, %arg3] : memref<16x16xf32>
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

