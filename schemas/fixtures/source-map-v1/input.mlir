module {
  func.func @kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>,
                    %c: memref<4x4xf32>) {
    linalg.matmul
      ins(%a, %b : memref<4x4xf32>, memref<4x4xf32>)
      outs(%c : memref<4x4xf32>)
    return
  }
}
