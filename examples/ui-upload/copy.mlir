module {
  func.func private @copy_kernel(%src: memref<4x4xf32>, %dst: memref<4x4xf32>) {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c4 = arith.constant 4 : index
    scf.for %i = %c0 to %c4 step %c1 {
      scf.for %j = %c0 to %c4 step %c1 {
        %v = memref.load %src[%i, %j] : memref<4x4xf32>
        memref.store %v, %dst[%i, %j] : memref<4x4xf32>
      }
    }
    return
  }
  func.func @unsupported(%x: i32) -> i32 {
    return %x : i32
  }
}
