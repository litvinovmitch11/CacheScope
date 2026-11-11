#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace mlir;

static std::string unsupported(func::FuncOp function) {
  if (function.isDeclaration())
    return "Function must have a body";
  if (function.getNumResults() != 0)
    return "Returned values are unsupported; use output memref arguments";
  for (Type type : function.getArgumentTypes()) {
    const auto memref = dyn_cast<MemRefType>(type);
    if (!memref || !memref.hasStaticShape() ||
        !memref.getElementType().isF32() || !memref.getLayout().isIdentity() ||
        memref.getMemorySpaceAsInt() != 0)
      return "Arguments must be static identity-layout memref<f32> in memory "
             "space 0";
  }
  bool calls = false;
  function.walk([&](Operation *operation) {
    if (isa<func::CallOp, func::CallIndirectOp>(operation))
      calls = true;
  });
  return calls ? "Calls to other functions are unsupported" : "";
}

int main(int argc, char **argv) {
  if (argc != 3 && argc != 5) {
    llvm::errs() << "usage: cachescope-program inspect INPUT | wrap INPUT "
                    "FUNCTION OUTPUT\n";
    return 2;
  }
  DialectRegistry registry;
  registry
      .insert<affine::AffineDialect, arith::ArithDialect,
              cf::ControlFlowDialect, func::FuncDialect, linalg::LinalgDialect,
              memref::MemRefDialect, scf::SCFDialect>();
  MLIRContext context(registry);
  auto module = parseSourceFile<ModuleOp>(argv[2], &context);
  if (!module || failed(verify(*module)))
    return 1;
  const std::string mode = argv[1];
  if (mode == "inspect" && argc == 3) {
    llvm::json::Array functions;
    for (auto function : module->getOps<func::FuncOp>()) {
      const auto reason = unsupported(function);
      llvm::json::Array arguments;
      for (auto [index, type] : llvm::enumerate(function.getArgumentTypes())) {
        llvm::json::Array shape;
        if (const auto memref = dyn_cast<MemRefType>(type))
          for (auto dimension : memref.getShape())
            shape.push_back(dimension);
        std::string spelling;
        llvm::raw_string_ostream out(spelling);
        type.print(out);
        arguments.push_back(
            llvm::json::Object{{"index", static_cast<int64_t>(index)},
                               {"type", spelling},
                               {"shape", std::move(shape)}});
      }
      functions.push_back(
          llvm::json::Object{{"name", function.getSymName()},
                             {"supported", reason.empty()},
                             {"reason", reason},
                             {"arguments", std::move(arguments)}});
    }
    llvm::outs() << llvm::formatv("{0:2}\n",
                                  llvm::json::Value(std::move(functions)));
    return 0;
  }
  if (mode != "wrap" || argc != 5)
    return 2;
  auto kernel = module->lookupSymbol<func::FuncOp>(argv[3]);
  if (!kernel) {
    llvm::errs() << "Unknown function\n";
    return 1;
  }
  if (const auto reason = unsupported(kernel); !reason.empty()) {
    llvm::errs() << reason << '\n';
    return 1;
  }
  // Only the selected function is executable; unrelated main/declarations do
  // not become unresolved symbols in the generated shared library.
  for (auto function :
       llvm::make_early_inc_range(module->getOps<func::FuncOp>()))
    if (function != kernel)
      function.erase();
  kernel.setName("__cachescope_kernel");
  kernel.setPrivate();
  OpBuilder builder(&context);
  builder.setInsertionPointToEnd(module->getBody());
  auto wrapper = func::FuncOp::create(
      builder, kernel.getLoc(), "cachescope_entry", kernel.getFunctionType());
  wrapper->setAttr("llvm.emit_c_interface", builder.getUnitAttr());
  builder.setInsertionPointToStart(wrapper.addEntryBlock());
  func::CallOp::create(builder, kernel.getLoc(), kernel,
                       wrapper.getArguments());
  func::ReturnOp::create(builder, kernel.getLoc());
  if (failed(verify(*module)))
    return 1;
  std::error_code error;
  llvm::raw_fd_ostream output(argv[4], error, llvm::sys::fs::OF_None);
  if (error) {
    llvm::errs() << error.message() << '\n';
    return 1;
  }
  module->print(output, OpPrintingFlags().enableDebugInfo());
  return 0;
}
