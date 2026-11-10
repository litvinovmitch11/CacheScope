module {
  func.func @kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>,
                    %c: memref<4x4xf32>) {
    %c0 = arith.constant 0 : index
    %c4 = arith.constant 4 : index
    scf.for %i = %c0 to %c4 step %c4 {
      linalg.matmul
        ins(%a, %b : memref<4x4xf32>, memref<4x4xf32>)
        outs(%c : memref<4x4xf32>)
    }
    return
  }
}
