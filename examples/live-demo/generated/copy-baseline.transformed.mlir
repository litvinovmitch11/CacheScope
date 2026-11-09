module {
  func.func private @copy_kernel(%arg0: memref<16x16xf32>, %arg1: memref<16x16xf32>) {
    linalg.copy ins(%arg0 : memref<16x16xf32>) outs(%arg1 : memref<16x16xf32>)
    return
  }
  func.func @main() -> i32 {
    %alloca = memref.alloca() : memref<16x16xf32>
    %alloca_0 = memref.alloca() : memref<16x16xf32>
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c15 = arith.constant 15 : index
    %c16 = arith.constant 16 : index
    %cst = arith.constant 0.000000e+00 : f32
    %cst_1 = arith.constant 7.000000e+00 : f32
    scf.for %arg0 = %c0 to %c16 step %c1 {
      scf.for %arg1 = %c0 to %c16 step %c1 {
        memref.store %cst_1, %alloca[%arg0, %arg1] : memref<16x16xf32>
        memref.store %cst, %alloca_0[%arg0, %arg1] : memref<16x16xf32>
      }
    }
    call @copy_kernel(%alloca, %alloca_0) : (memref<16x16xf32>, memref<16x16xf32>) -> ()
    %0 = memref.load %alloca_0[%c15, %c15] : memref<16x16xf32>
    %1 = arith.fptosi %0 : f32 to i32
    return %1 : i32
  }
}

