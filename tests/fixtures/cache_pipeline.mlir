module {
  func.func private @matmul_kernel(%a: memref<8x8xf32>) {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c2 = arith.constant 2 : index
    %c4 = arith.constant 4 : index
    %one = arith.constant 1.0 : f32
    memref.store %one, %a[%c0, %c0] : memref<8x8xf32>
    %x = memref.load %a[%c0, %c0] : memref<8x8xf32>
    memref.store %x, %a[%c2, %c0] : memref<8x8xf32>
    %y = memref.load %a[%c4, %c0] : memref<8x8xf32>
    %z = memref.load %a[%c2, %c0] : memref<8x8xf32>
    %sum = arith.addf %y, %z : f32
    memref.store %sum, %a[%c0, %c1] : memref<8x8xf32>
    return
  }
  func.func @main() -> i32 {
    %a = memref.alloca() : memref<8x8xf32>
    %c4 = arith.constant 4 : index
    %c0 = arith.constant 0 : index
    %initial = arith.constant 15.0 : f32
    memref.store %initial, %a[%c4, %c0] : memref<8x8xf32>
    call @matmul_kernel(%a) : (memref<8x8xf32>) -> ()
    %c1 = arith.constant 1 : index
    %out = memref.load %a[%c0, %c1] : memref<8x8xf32>
    %result = arith.fptosi %out : f32 to i32
    return %result : i32
  }
}
