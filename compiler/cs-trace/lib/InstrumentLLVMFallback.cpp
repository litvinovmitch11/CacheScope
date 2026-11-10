#include "CacheScope/Passes.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace mlir::cachescope {
namespace {

constexpr StringLiteral kFallbackTraceFunction =
    "__cachescope_trace_fallback_v1";
constexpr StringLiteral kRegisterAccessFunction =
    "__cachescope_register_access_v1";
constexpr StringLiteral kRegisterSourceFunction =
    "__cachescope_register_source_v1";
constexpr StringLiteral kAccessIdMetadata = "cachescope.access_id";
constexpr StringLiteral kMemrefIdMetadata = "cachescope.memref_id";
constexpr StringLiteral kInstrumentedAttribute =
    "cachescope.llvm_fallback_instrumented";
constexpr std::int64_t kRead = 0;
constexpr std::int64_t kWrite = 1;
constexpr std::int64_t kReadWrite = 2;
constexpr std::int64_t kSemanticAddress = 0;
constexpr std::int64_t kProcessAddress = 1;
constexpr std::int64_t kFallbackAccessIdBase = std::int64_t{1} << 32;

struct LLVMAccess {
  Operation *operation;
  Value pointer;
  Type valueType;
  std::int64_t kind;
  std::int64_t accessId;
};

struct StaticAccess {
  std::int64_t accessId;
  std::int64_t memrefId;
  std::int64_t kind;
  std::int64_t addressKind;
  std::string sourceLocation;
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

static DictionaryAttr findSemanticMetadata(Location location) {
  auto fused = dyn_cast<FusedLoc>(location);
  if (!fused)
    return {};

  if (auto metadata = dyn_cast_or_null<DictionaryAttr>(fused.getMetadata()))
    if (metadata.get(kAccessIdMetadata))
      return metadata;

  for (Location nested : fused.getLocations())
    if (DictionaryAttr metadata = findSemanticMetadata(nested))
      return metadata;
  return {};
}

static std::string formatSourceLocation(Location location) {
  while (auto fused = dyn_cast<FusedLoc>(location)) {
    if (fused.getLocations().empty())
      break;
    location = fused.getLocations().front();
  }
  std::string result;
  llvm::raw_string_ostream stream(result);
  location.print(stream);
  return result;
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
         *callee == kRegisterAccessFunction ||
         *callee == kRegisterSourceFunction ||
         *callee == kFallbackTraceFunction;
}

static LLVM::LLVMFuncOp getOrCreateRegistrationDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto type = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(module.getContext()), {i64, i64, i64, i64});

  if (auto existing =
          module.lookupSymbol<LLVM::LLVMFuncOp>(kRegisterAccessFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterAccessFunction, type,
                                  LLVM::Linkage::External);
}

static LLVM::LLVMFuncOp getOrCreateSourceDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto pointer = LLVM::LLVMPointerType::get(module.getContext());
  auto type = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(module.getContext()), {i64, pointer});

  if (auto existing =
          module.lookupSymbol<LLVM::LLVMFuncOp>(kRegisterSourceFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterSourceFunction, type,
                                  LLVM::Linkage::External);
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

static LLVM::GlobalOp createSourceLocationGlobal(ModuleOp module,
                                                 const StaticAccess &access) {
  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  SmallVector<std::uint8_t> bytes(access.sourceLocation.begin(),
                                  access.sourceLocation.end());
  bytes.push_back('\0');
  auto tensorType = RankedTensorType::get(
      {static_cast<std::int64_t>(bytes.size())}, builder.getI8Type());
  auto data = DenseElementsAttr::get(tensorType, ArrayRef(bytes));
  auto arrayType = LLVM::LLVMArrayType::get(builder.getI8Type(), bytes.size());
  std::string symbol = "__cachescope_source_" + std::to_string(access.accessId);
  while (module.lookupSymbol(symbol))
    symbol.push_back('_');
  return LLVM::GlobalOp::create(builder, module.getLoc(), arrayType,
                                /*isConstant=*/true, LLVM::Linkage::Private,
                                symbol, data);
}

static void insertStaticRegistrations(ModuleOp module,
                                      LLVM::LLVMFuncOp function,
                                      SmallVectorImpl<StaticAccess> &accesses,
                                      LLVM::LLVMFuncOp registrationFunction,
                                      LLVM::LLVMFuncOp sourceFunction) {
  llvm::sort(accesses, [](const StaticAccess &left, const StaticAccess &right) {
    return left.accessId < right.accessId;
  });

  SmallVector<std::pair<StaticAccess *, LLVM::GlobalOp>> globals;
  for (StaticAccess &access : accesses)
    globals.emplace_back(&access, createSourceLocationGlobal(module, access));

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(&function.getBody().front());
  auto i64 = builder.getI64Type();
  auto pointer = LLVM::LLVMPointerType::get(module.getContext());
  auto constant = [&](std::int64_t value) -> Value {
    return LLVM::ConstantOp::create(builder, function.getLoc(), i64, value);
  };
  for (auto &[access, global] : globals) {
    Value address = LLVM::AddressOfOp::create(builder, function.getLoc(),
                                              pointer, global.getName());
    auto arrayType = cast<LLVM::LLVMArrayType>(global.getType());
    SmallVector<LLVM::GEPArg> indices(1, 0);
    Value source = LLVM::GEPOp::create(builder, function.getLoc(), pointer,
                                       arrayType, address, indices);
    SmallVector<Value, 4> arguments{
        constant(access->accessId), constant(access->memrefId),
        constant(access->kind), constant(access->addressKind)};
    LLVM::CallOp::create(builder, function.getLoc(), registrationFunction,
                         arguments);
    SmallVector<Value, 2> sourceArguments{constant(access->accessId), source};
    LLVM::CallOp::create(builder, function.getLoc(), sourceFunction,
                         sourceArguments);
  }
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
  std::map<std::int64_t, StaticAccess> staticAccesses;
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

    if (DictionaryAttr metadata = findSemanticMetadata(operation->getLoc())) {
      auto accessId = metadata.getAs<IntegerAttr>(kAccessIdMetadata);
      auto memrefId = metadata.getAs<IntegerAttr>(kMemrefIdMetadata);
      if (!accessId || !memrefId) {
        operation->emitError("incomplete CacheScope semantic metadata");
        return WalkResult::interrupt();
      }
      StaticAccess access{accessId.getInt(), memrefId.getInt(), kind,
                          kSemanticAddress,
                          formatSourceLocation(operation->getLoc())};
      auto [entry, inserted] =
          staticAccesses.try_emplace(access.accessId, access);
      if (!inserted && (entry->second.memrefId != access.memrefId ||
                        entry->second.kind != access.kind ||
                        entry->second.addressKind != access.addressKind)) {
        operation->emitError("conflicting CacheScope semantic access metadata");
        return WalkResult::interrupt();
      }
    } else {
      fallbackAccesses.push_back(
          {operation, pointer, valueType, kind, nextAccessId++});
    }
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
    staticAccesses.try_emplace(
        access.accessId,
        StaticAccess{access.accessId, -1, access.kind, kProcessAddress,
                     formatSourceLocation(access.operation->getLoc())});
  }

  LLVM::LLVMFuncOp registrationFunction =
      getOrCreateRegistrationDeclaration(module);
  LLVM::LLVMFuncOp sourceFunction = getOrCreateSourceDeclaration(module);
  if (!registrationFunction || !sourceFunction) {
    module.emitError() << "symbol @" << kRegisterAccessFunction << " or @"
                       << kRegisterSourceFunction
                       << " exists with an incompatible type";
    return signalPassFailure();
  }
  SmallVector<StaticAccess> registrations;
  registrations.reserve(staticAccesses.size());
  for (auto &[id, access] : staticAccesses)
    registrations.push_back(std::move(access));
  insertStaticRegistrations(module, function, registrations,
                            registrationFunction, sourceFunction);
  module->setAttr(kInstrumentedAttribute, UnitAttr::get(&getContext()));
}

void registerCacheScopeLLVMFallbackPass() {
  PassRegistration<InstrumentLLVMFallbackPass>();
}

} // namespace mlir::cachescope
