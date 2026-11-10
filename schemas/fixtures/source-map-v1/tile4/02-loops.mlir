module {
  func.func @kernel(%a: memref<4x4xf32>, %b: memref<4x4xf32>,
                    %c: memref<4x4xf32>) {
    %c0 = arith.constant 0 : index
    %a0 = memref.load %a[%c0, %c0] : memref<4x4xf32>
    %b0 = memref.load %b[%c0, %c0] : memref<4x4xf32>
    %c0_value = memref.load %c[%c0, %c0] : memref<4x4xf32>
    %product = arith.mulf %a0, %b0 : f32
    %sum = arith.addf %c0_value, %product : f32
    memref.store %sum, %c[%c0, %c0] : memref<4x4xf32>
    return
  }
}
