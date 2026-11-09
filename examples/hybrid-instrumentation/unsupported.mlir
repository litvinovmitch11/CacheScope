// This is intentionally rejected until memcpy receives an explicit policy.
module {
  llvm.func @kernel(%destination: !llvm.ptr, %source: !llvm.ptr, %size: i64) {
    // CHECK: error: unsupported explicit LLVM memory operation
    "llvm.intr.memcpy"(%destination, %source, %size) <{isVolatile = false}>
      : (!llvm.ptr, !llvm.ptr, i64) -> ()
    llvm.return
  }
}
