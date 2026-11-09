// A second, visually simple payload: copy one matrix into another.
// Expected result: dst[15, 15] == 7, so @main returns 7.
module {
  func.func private @copy_kernel(
      %src: memref<16x16xf32>,
      %dst: memref<16x16xf32>) {
    linalg.copy
      ins(%src : memref<16x16xf32>)
      outs(%dst : memref<16x16xf32>)
    return
  }

  func.func @main() -> i32 {
    %src = memref.alloca() : memref<16x16xf32>
    %dst = memref.alloca() : memref<16x16xf32>

    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c15 = arith.constant 15 : index
    %c16 = arith.constant 16 : index
    %zero = arith.constant 0.0 : f32
    %seven = arith.constant 7.0 : f32

    scf.for %i = %c0 to %c16 step %c1 {
      scf.for %j = %c0 to %c16 step %c1 {
        memref.store %seven, %src[%i, %j] : memref<16x16xf32>
        memref.store %zero, %dst[%i, %j] : memref<16x16xf32>
      }
    }

    call @copy_kernel(%src, %dst)
      : (memref<16x16xf32>, memref<16x16xf32>) -> ()

    %result = memref.load %dst[%c15, %c15] : memref<16x16xf32>
    %result_i32 = arith.fptosi %result : f32 to i32
    return %result_i32 : i32
  }
}
