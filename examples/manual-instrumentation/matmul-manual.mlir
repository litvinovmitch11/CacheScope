// A deliberately hand-instrumented baseline matmul.
// This is the executable ABI prototype for the future instrumentation pass.
module {
  func.func private @__cachescope_trace_v1(
      i64, i64, i64, i64, i64, i64, i64)

  func.func private @matmul_kernel(
      %a: memref<16x16xf32>,
      %b: memref<16x16xf32>,
      %c: memref<16x16xf32>) {
    %a_base, %a_offset, %a_sizes:2, %a_strides:2 =
      memref.extract_strided_metadata %a
        : memref<16x16xf32> -> memref<f32>, index, index, index, index, index
    %b_base, %b_offset, %b_sizes:2, %b_strides:2 =
      memref.extract_strided_metadata %b
        : memref<16x16xf32> -> memref<f32>, index, index, index, index, index
    %c_base, %c_offset, %c_sizes:2, %c_strides:2 =
      memref.extract_strided_metadata %c
        : memref<16x16xf32> -> memref<f32>, index, index, index, index, index

    %c0 = arith.constant 0 : index
    %c1 = arith.constant 1 : index
    %c4 = arith.constant 4 : index
    %c16 = arith.constant 16 : index
    %read = arith.constant 0 : i64
    %write = arith.constant 1 : i64
    %size_f32 = arith.constant 4 : i64
    %access_a = arith.constant 0 : i64
    %access_b = arith.constant 1 : i64
    %access_c_read = arith.constant 2 : i64
    %access_c_write = arith.constant 3 : i64
    %memref_a = arith.constant 0 : i64
    %memref_b = arith.constant 1 : i64
    %memref_c = arith.constant 2 : i64

    scf.for %i = %c0 to %c16 step %c1 {
      scf.for %j = %c0 to %c16 step %c1 {
        scf.for %k = %c0 to %c16 step %c1 {
          %i64 = arith.index_cast %i : index to i64
          %j64 = arith.index_cast %j : index to i64
          %k64 = arith.index_cast %k : index to i64

          %a_i = arith.muli %i, %a_strides#0 : index
          %a_k = arith.muli %k, %a_strides#1 : index
          %a_with_offset = arith.addi %a_offset, %a_i : index
          %a_element_offset = arith.addi %a_with_offset, %a_k : index
          %a_byte_offset_index = arith.muli %a_element_offset, %c4 : index
          %a_byte_offset = arith.index_cast %a_byte_offset_index : index to i64
          func.call @__cachescope_trace_v1(
              %access_a, %memref_a, %a_byte_offset, %size_f32,
              %read, %i64, %k64)
            : (i64, i64, i64, i64, i64, i64, i64) -> ()
          %a_value = memref.load %a[%i, %k] : memref<16x16xf32>

          %b_k = arith.muli %k, %b_strides#0 : index
          %b_j = arith.muli %j, %b_strides#1 : index
          %b_with_offset = arith.addi %b_offset, %b_k : index
          %b_element_offset = arith.addi %b_with_offset, %b_j : index
          %b_byte_offset_index = arith.muli %b_element_offset, %c4 : index
          %b_byte_offset = arith.index_cast %b_byte_offset_index : index to i64
          func.call @__cachescope_trace_v1(
              %access_b, %memref_b, %b_byte_offset, %size_f32,
              %read, %k64, %j64)
            : (i64, i64, i64, i64, i64, i64, i64) -> ()
          %b_value = memref.load %b[%k, %j] : memref<16x16xf32>

          %c_i = arith.muli %i, %c_strides#0 : index
          %c_j = arith.muli %j, %c_strides#1 : index
          %c_with_offset = arith.addi %c_offset, %c_i : index
          %c_element_offset = arith.addi %c_with_offset, %c_j : index
          %c_byte_offset_index = arith.muli %c_element_offset, %c4 : index
          %c_byte_offset = arith.index_cast %c_byte_offset_index : index to i64
          func.call @__cachescope_trace_v1(
              %access_c_read, %memref_c, %c_byte_offset, %size_f32,
              %read, %i64, %j64)
            : (i64, i64, i64, i64, i64, i64, i64) -> ()
          %c_value = memref.load %c[%i, %j] : memref<16x16xf32>

          %product = arith.mulf %a_value, %b_value : f32
          %sum = arith.addf %c_value, %product : f32
          func.call @__cachescope_trace_v1(
              %access_c_write, %memref_c, %c_byte_offset, %size_f32,
              %write, %i64, %j64)
            : (i64, i64, i64, i64, i64, i64, i64) -> ()
          memref.store %sum, %c[%i, %j] : memref<16x16xf32>
        }
      }
    }
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

    func.call @matmul_kernel(%a, %b, %c)
      : (memref<16x16xf32>, memref<16x16xf32>, memref<16x16xf32>) -> ()
    %result = memref.load %c[%c0, %c0] : memref<16x16xf32>
    %result_i32 = arith.fptosi %result : f32 to i32
    return %result_i32 : i32
  }
}
