// One payload for all matmul demo variants.
// Expected result: C[0, 0] == 16, so @main returns 16.
module {
  func.func private @matmul_kernel(
      %a: memref<16x16xf32>,
      %b: memref<16x16xf32>,
      %c: memref<16x16xf32>) {
    linalg.matmul
      ins(%a, %b : memref<16x16xf32>, memref<16x16xf32>)
      outs(%c : memref<16x16xf32>)
    return
  }

  func.func @main() -> i32 {
    %a = memref.alloca() : memref<16x16xf32>
    %b = memref.alloca() : memref<16x16xf32>
    %c = memref.alloca() : memref<16x16xf32>

    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c16 = arith.constant 16 : index
    %zero = arith.constant 0.0 : f32
    %one = arith.constant 1.0 : f32

    scf.for %i = %c0 to %c16 step %c1 {
      scf.for %j = %c0 to %c16 step %c1 {
        memref.store %one, %a[%i, %j] : memref<16x16xf32>
        memref.store %one, %b[%i, %j] : memref<16x16xf32>
        memref.store %zero, %c[%i, %j] : memref<16x16xf32>
      }
    }

    call @matmul_kernel(%a, %b, %c)
      : (memref<16x16xf32>, memref<16x16xf32>, memref<16x16xf32>) -> ()

    %result = memref.load %c[%c0, %c0] : memref<16x16xf32>
    %result_i32 = arith.fptosi %result : f32 to i32
    return %result_i32 : i32
  }
}
