// No MemRef provenance is available, so the LLVM pass inserts pointer events.
module {
  llvm.func @kernel(%pointer: !llvm.ptr, %value: i32) {
    // CHECK: llvm.call @__cachescope_trace_fallback_v1
    // CHECK: llvm.load
    %loaded = llvm.load %pointer : !llvm.ptr -> i32
    // CHECK: llvm.call @__cachescope_trace_fallback_v1
    // CHECK: llvm.store
    llvm.store %value, %pointer : i32, !llvm.ptr
    llvm.return
  }
}
