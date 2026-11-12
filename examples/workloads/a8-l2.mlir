// Two passes over 128 KiB: one 32-bit read per 64-byte cache line.
module {
  func.func private @a8_l2_kernel(%data: memref<32768xi32>) -> i32 {
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c2 = arith.constant 2 : index
    %c16 = arith.constant 16 : index
    %end = arith.constant 32768 : index
    %zero = arith.constant 0 : i32
    %sum = scf.for %pass = %c0 to %c2 step %c1 iter_args(%acc = %zero) -> (i32) {
      %next = scf.for %i = %c0 to %end step %c16 iter_args(%partial = %acc) -> (i32) {
        %value = memref.load %data[%i] : memref<32768xi32>
        %added = arith.addi %partial, %value : i32
        scf.yield %added : i32
      }
      scf.yield %next : i32
    }
    return %sum : i32
  }

  func.func @main() -> i32 {
    %data = memref.alloca() : memref<32768xi32>
    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %end = arith.constant 32768 : index
    %one = arith.constant 1 : i32
    scf.for %i = %c0 to %end step %c1 {
      memref.store %one, %data[%i] : memref<32768xi32>
    }
    %result = call @a8_l2_kernel(%data) : (memref<32768xi32>) -> i32
    return %result : i32
  }
}
