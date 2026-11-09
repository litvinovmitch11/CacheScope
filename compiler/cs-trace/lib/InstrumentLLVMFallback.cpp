#include "CacheScope/Passes.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mlir::cachescope {
namespace {

constexpr StringLiteral kFallbackTraceFunction =
    "__cachescope_trace_fallback_v1";
constexpr StringLiteral kAccessIdMetadata = "cachescope.access_id";
constexpr StringLiteral kInstrumentedAttribute =
    "cachescope.llvm_fallback_instrumented";
constexpr std::int64_t kRead = 0;
constexpr std::int64_t kWrite = 1;
constexpr std::int64_t kReadWrite = 2;
constexpr std::int64_t kFallbackAccessIdBase = std::int64_t{1} << 32;

struct LLVMAccess {
  Operation *operation;
  Value pointer;
  Type valueType;
  std::int64_t kind;
  std::int64_t accessId;
};

class InstrumentLLVMFallbackPass
    : public PassWrapper<InstrumentLLVMFallbackPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InstrumentLLVMFallbackPass)

  InstrumentLLVMFallbackPass() = default;
  InstrumentLLVMFallbackPass(const InstrumentLLVMFallbackPass &pass)
      : PassWrapper(pass) {}

  StringRef getArgument() const final {
    return "cachescope-instrument-llvm-fallback";
  }
  StringRef getDescription() const final {
    return "Instrument explicit LLVM memory accesses not covered at MemRef "
           "level and reject unsupported memory operations";
  }

  void getDependentDialects(DialectRegistry &registry) const final {
    registry.insert<LLVM::LLVMDialect>();
  }

protected:
  void runOnOperation() final;

private:
  Option<std::string> targetFunction{
      *this, "target-func",
      llvm::cl::desc("Exact LLVM function symbol to audit and instrument"),
      llvm::cl::init("")};
};

} // namespace

static bool hasSemanticMarker(Location location) {
  auto fused = dyn_cast<FusedLoc>(location);
  if (!fused)
    return false;

  if (auto metadata = dyn_cast_or_null<DictionaryAttr>(fused.getMetadata()))
    if (metadata.get(kAccessIdMetadata))
      return true;

  for (Location nested : fused.getLocations())
    if (hasSemanticMarker(nested))
      return true;
  return false;
}

static bool hasDirectMemoryEffect(Operation *operation) {
  auto interface = dyn_cast<MemoryEffectOpInterface>(operation);
  if (!interface)
    return false;
  SmallVector<MemoryEffects::EffectInstance> effects;
  interface.getEffects(effects);
  return llvm::any_of(effects, [](const MemoryEffects::EffectInstance &effect) {
    return isa<MemoryEffects::Read, MemoryEffects::Write>(effect.getEffect());
  });
}

static bool isAllowedRuntimeCall(LLVM::CallOp call) {
  std::optional<StringRef> callee = call.getCallee();
  if (!callee)
    return false;
  return *callee == "malloc" || *callee == "calloc" ||
         *callee == "aligned_alloc" || *callee == "free" ||
         *callee == "__cachescope_trace_v1" ||
         *callee == kFallbackTraceFunction;
}

static LLVM::LLVMFuncOp getOrCreateFallbackDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto type = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(module.getContext()), {i64, i64, i64, i64});

  if (auto existing =
          module.lookupSymbol<LLVM::LLVMFuncOp>(kFallbackTraceFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kFallbackTraceFunction, type,
                                  LLVM::Linkage::External);
}

static FailureOr<std::uint64_t> getFixedSizeBytes(Type type,
                                                  const DataLayout &layout) {
  llvm::TypeSize size = layout.getTypeSize(type);
  if (size.isScalable())
    return failure();
  return size.getFixedValue();
}

static void insertFallbackCall(const LLVMAccess &access,
                               std::uint64_t sizeBytes,
                               LLVM::LLVMFuncOp traceFunction) {
  OpBuilder builder(access.operation);
  Location location = access.operation->getLoc();
  Type i64 = builder.getI64Type();
  Value address =
      LLVM::PtrToIntOp::create(builder, location, i64, access.pointer);
  auto constant = [&](std::int64_t value) -> Value {
    return LLVM::ConstantOp::create(builder, location, i64, value);
  };
  SmallVector<Value, 4> arguments{
      constant(access.accessId), address,
      constant(static_cast<std::int64_t>(sizeBytes)), constant(access.kind)};
  LLVM::CallOp::create(builder, location, traceFunction, arguments);
}

void InstrumentLLVMFallbackPass::runOnOperation() {
  ModuleOp module = getOperation();
  if (module->hasAttr(kInstrumentedAttribute)) {
    module.emitError("module is already instrumented by LLVM fallback");
    return signalPassFailure();
  }
  if (targetFunction.empty()) {
    module.emitError(
        "--cachescope-instrument-llvm-fallback requires target-func=<name>");
    return signalPassFailure();
  }

  auto function = module.lookupSymbol<LLVM::LLVMFuncOp>(targetFunction);
  if (!function || function.isExternal()) {
    module.emitError() << "cannot find target LLVM function @"
                       << targetFunction;
    return signalPassFailure();
  }

  SmallVector<LLVMAccess> fallbackAccesses;
  std::int64_t nextAccessId = kFallbackAccessIdBase;
  WalkResult result = function.walk([&](Operation *operation) -> WalkResult {
    Value pointer;
    Type valueType;
    std::int64_t kind;

    if (auto load = dyn_cast<LLVM::LoadOp>(operation)) {
      pointer = load.getAddr();
      valueType = load.getType();
      kind = kRead;
    } else if (auto store = dyn_cast<LLVM::StoreOp>(operation)) {
      pointer = store.getAddr();
      valueType = store.getValue().getType();
      kind = kWrite;
    } else if (auto atomic = dyn_cast<LLVM::AtomicRMWOp>(operation)) {
      pointer = atomic.getPtr();
      valueType = atomic.getVal().getType();
      kind = kReadWrite;
    } else if (auto atomic = dyn_cast<LLVM::AtomicCmpXchgOp>(operation)) {
      pointer = atomic.getPtr();
      valueType = atomic.getVal().getType();
      kind = kReadWrite;
    } else {
      if (auto call = dyn_cast<LLVM::CallOp>(operation)) {
        if (!isAllowedRuntimeCall(call)) {
          operation->emitError(
              "call may hide memory accesses; inline/lower the callee or add "
              "an explicit CacheScope call policy");
          return WalkResult::interrupt();
        }
        return WalkResult::advance();
      }
      // Calls are interprocedural boundaries. Allocations and deallocations are
      // intentionally not modeled as data accesses. Every other explicit LLVM
      // operation with a direct read/write effect must be handled or rejected.
      if (!isa<LLVM::AllocaOp>(operation) && hasDirectMemoryEffect(operation)) {
        operation->emitError(
            "unsupported explicit LLVM memory operation; CacheScope refuses "
            "to produce an incomplete trace");
        return WalkResult::interrupt();
      }
      return WalkResult::advance();
    }

    if (!hasSemanticMarker(operation->getLoc()))
      fallbackAccesses.push_back(
          {operation, pointer, valueType, kind, nextAccessId++});
    return WalkResult::advance();
  });
  if (result.wasInterrupted())
    return signalPassFailure();

  LLVM::LLVMFuncOp traceFunction;
  if (!fallbackAccesses.empty()) {
    traceFunction = getOrCreateFallbackDeclaration(module);
    if (!traceFunction) {
      module.emitError() << "symbol @" << kFallbackTraceFunction
                         << " exists with an incompatible type";
      return signalPassFailure();
    }
  }

  DataLayout layout(module);
  for (const LLVMAccess &access : fallbackAccesses) {
    FailureOr<std::uint64_t> size = getFixedSizeBytes(access.valueType, layout);
    if (failed(size)) {
      access.operation->emitError(
          "fallback instrumentation requires a fixed access size");
      return signalPassFailure();
    }
    insertFallbackCall(access, *size, traceFunction);
  }
  module->setAttr(kInstrumentedAttribute, UnitAttr::get(&getContext()));
}

void registerCacheScopeLLVMFallbackPass() {
  PassRegistration<InstrumentLLVMFallbackPass>();
}

} // namespace mlir::cachescope
