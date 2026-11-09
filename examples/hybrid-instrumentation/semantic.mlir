// A supported semantic access: CacheScope retains memref identity and indices.
module {
  func.func @kernel(%input: memref<4x4xf32>, %output: memref<4x4xf32>) {
    %c0 = arith.constant 0 : index
    %value = memref.load %input[%c0, %c0] : memref<4x4xf32>
    memref.store %value, %output[%c0, %c0] : memref<4x4xf32>
    return
  }
}
