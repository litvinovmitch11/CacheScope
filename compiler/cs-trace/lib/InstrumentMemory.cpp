#include "CacheScope/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace mlir::cachescope {
namespace {

constexpr StringLiteral kTraceFunction = "__cachescope_trace_v1";
constexpr StringLiteral kInstrumentedAttribute = "cachescope.instrumented";
constexpr StringLiteral kAccessIdMetadata = "cachescope.access_id";
constexpr StringLiteral kMemrefIdMetadata = "cachescope.memref_id";
constexpr std::int64_t kRead = 0;
constexpr std::int64_t kWrite = 1;

struct AccessKey {
  unsigned memrefId;
  std::int64_t kind;
  unsigned ordinal;

  auto asTuple() const { return std::tie(memrefId, kind, ordinal); }
  bool operator<(const AccessKey &other) const {
    return asTuple() < other.asTuple();
  }
};

struct AccessSite {
  Operation *operation;
  Value memref;
  SmallVector<Value> indices;
  AccessKey key;
  std::int64_t accessId = -1;
};

class InstrumentMemoryPass
    : public PassWrapper<InstrumentMemoryPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(InstrumentMemoryPass)

  InstrumentMemoryPass() = default;
  InstrumentMemoryPass(const InstrumentMemoryPass &pass) : PassWrapper(pass) {}

  StringRef getArgument() const final { return "cachescope-instrument-memory"; }
  StringRef getDescription() const final {
    return "Insert CacheScope tracing calls before rank-2 memref loads/stores";
  }

  void getDependentDialects(DialectRegistry &registry) const final {
    registry.insert<arith::ArithDialect, func::FuncDialect,
                    memref::MemRefDialect>();
  }

protected:
  void runOnOperation() final;

private:
  Option<std::string> targetFunction{
      *this, "target-func",
      llvm::cl::desc("Exact function symbol to instrument"),
      llvm::cl::init("")};
};

using RootIdMap = llvm::DenseMap<Value, unsigned>;

} // namespace

static RootIdMap collectRootIds(func::FuncOp function) {
  RootIdMap roots;
  for (BlockArgument argument : function.getArguments())
    roots.try_emplace(argument, argument.getArgNumber());

  unsigned nextId = function.getNumArguments();
  function.walk([&](Operation *operation) {
    if (auto alloc = dyn_cast<memref::AllocOp>(operation))
      roots.try_emplace(alloc.getResult(), nextId++);
    else if (auto alloca = dyn_cast<memref::AllocaOp>(operation))
      roots.try_emplace(alloca.getResult(), nextId++);
  });
  return roots;
}

static std::optional<unsigned> findRootId(Value value, const RootIdMap &roots) {
  while (true) {
    if (auto root = roots.find(value); root != roots.end())
      return root->second;

    Operation *definingOp = value.getDefiningOp();
    if (!definingOp)
      return std::nullopt;
    auto view = dyn_cast<ViewLikeOpInterface>(definingOp);
    if (!view)
      return std::nullopt;
    value = view.getViewSource();
  }
}

static FailureOr<std::int64_t> getElementSizeBytes(MemRefType type) {
  Type elementType = type.getElementType();
  if (!elementType.isIntOrFloat() || elementType.isInteger(1))
    return failure();
  const unsigned width = elementType.getIntOrFloatBitWidth();
  if (width == 0 || width % 8 != 0)
    return failure();
  return width / 8;
}

static func::FuncOp getOrCreateTraceDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  SmallVector<Type, 7> arguments(7, i64);
  auto expectedType = FunctionType::get(module.getContext(), arguments, {});

  if (auto existing = module.lookupSymbol<func::FuncOp>(kTraceFunction)) {
    if (existing.getFunctionType() == expectedType)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  auto declaration = func::FuncOp::create(builder, module.getLoc(),
                                          kTraceFunction, expectedType);
  declaration.setPrivate();
  return declaration;
}

static LogicalResult collectAccessSites(func::FuncOp function,
                                        SmallVectorImpl<AccessSite> &sites) {
  RootIdMap rootIds = collectRootIds(function);
  std::map<std::pair<unsigned, std::int64_t>, unsigned> nextOrdinal;
  WalkResult result = function.walk([&](Operation *operation) -> WalkResult {
    Value memref;
    ValueRange indices;
    std::int64_t kind;

    if (auto load = dyn_cast<memref::LoadOp>(operation)) {
      memref = load.getMemRef();
      indices = load.getIndices();
      kind = kRead;
    } else if (auto store = dyn_cast<memref::StoreOp>(operation)) {
      memref = store.getMemRef();
      indices = store.getIndices();
      kind = kWrite;
    } else {
      return WalkResult::advance();
    }

    auto memrefType = dyn_cast<MemRefType>(memref.getType());
    if (!memrefType || memrefType.getRank() != 2 || indices.size() != 2) {
      operation->emitError(
          "CacheScope MVP supports only rank-2 memref.load/store");
      return WalkResult::interrupt();
    }

    SmallVector<int64_t> strides;
    int64_t offset;
    if (failed(memrefType.getStridesAndOffset(strides, offset))) {
      operation->emitError("CacheScope requires a strided memref layout");
      return WalkResult::interrupt();
    }
    if (failed(getElementSizeBytes(memrefType))) {
      operation->emitError(
          "CacheScope requires a byte-addressable scalar int/float element");
      return WalkResult::interrupt();
    }

    std::optional<unsigned> memrefId = findRootId(memref, rootIds);
    if (!memrefId) {
      operation->emitError(
          "cannot resolve the accessed memref to a function argument or "
          "allocation");
      return WalkResult::interrupt();
    }

    auto counterKey = std::make_pair(*memrefId, kind);
    const unsigned ordinal = nextOrdinal[counterKey]++;
    sites.push_back({operation,
                     memref,
                     SmallVector<Value>(indices),
                     {*memrefId, kind, ordinal}});
    return WalkResult::advance();
  });
  return success(!result.wasInterrupted());
}

static void assignStableAccessIds(SmallVectorImpl<AccessSite> &sites) {
  std::map<AccessKey, std::int64_t> ids;
  for (const AccessSite &site : sites)
    ids.try_emplace(site.key, -1);

  std::int64_t nextId = 0;
  for (auto &[key, id] : ids)
    id = nextId++;
  for (AccessSite &site : sites)
    site.accessId = ids.at(site.key);
}

static void attachAccessMetadata(const AccessSite &site) {
  Builder builder(site.operation->getContext());
  auto metadata = builder.getDictionaryAttr({
      builder.getNamedAttr(kAccessIdMetadata,
                           builder.getI64IntegerAttr(site.accessId)),
      builder.getNamedAttr(kMemrefIdMetadata,
                           builder.getI64IntegerAttr(site.key.memrefId)),
  });
  site.operation->setLoc(
      builder.getFusedLoc({site.operation->getLoc()}, metadata));
}

static void instrumentAccess(const AccessSite &site,
                             std::int64_t elementSizeBytes) {
  OpBuilder builder(site.operation);
  Location location = site.operation->getLoc();
  auto i64 = builder.getI64Type();

  auto metadata =
      memref::ExtractStridedMetadataOp::create(builder, location, site.memref);
  Value elementOffset = metadata.getOffset();
  for (auto [index, stride] :
       llvm::zip_equal(site.indices, metadata.getStrides())) {
    Value scaledIndex = arith::MulIOp::create(builder, location, index, stride);
    elementOffset =
        arith::AddIOp::create(builder, location, elementOffset, scaledIndex);
  }

  Value elementSize =
      arith::ConstantIndexOp::create(builder, location, elementSizeBytes);
  Value byteOffsetIndex =
      arith::MulIOp::create(builder, location, elementOffset, elementSize);
  Value byteOffset =
      arith::IndexCastOp::create(builder, location, i64, byteOffsetIndex);
  Value index0 =
      arith::IndexCastOp::create(builder, location, i64, site.indices[0]);
  Value index1 =
      arith::IndexCastOp::create(builder, location, i64, site.indices[1]);

  auto constantI64 = [&](std::int64_t value) -> Value {
    return arith::ConstantIntOp::create(builder, location, value, 64);
  };
  SmallVector<Value, 7> arguments{constantI64(site.accessId),
                                  constantI64(site.key.memrefId),
                                  byteOffset,
                                  constantI64(elementSizeBytes),
                                  constantI64(site.key.kind),
                                  index0,
                                  index1};
  func::CallOp::create(builder, location, kTraceFunction, TypeRange{},
                       arguments);
}

void InstrumentMemoryPass::runOnOperation() {
  ModuleOp module = getOperation();
  if (module->hasAttr(kInstrumentedAttribute)) {
    module.emitError("module is already instrumented by CacheScope");
    return signalPassFailure();
  }
  if (targetFunction.empty()) {
    module.emitError(
        "--cachescope-instrument-memory requires target-func=<name>");
    return signalPassFailure();
  }

  auto function = module.lookupSymbol<func::FuncOp>(targetFunction);
  if (!function || function.isDeclaration()) {
    module.emitError() << "cannot find target function @" << targetFunction;
    return signalPassFailure();
  }

  SmallVector<AccessSite> sites;
  if (failed(collectAccessSites(function, sites)))
    return signalPassFailure();
  if (sites.empty()) {
    function.emitError(
        "target contains no supported memref.load/store operations");
    return signalPassFailure();
  }

  if (!getOrCreateTraceDeclaration(module)) {
    module.emitError() << "symbol @" << kTraceFunction
                       << " exists with an incompatible type";
    return signalPassFailure();
  }

  assignStableAccessIds(sites);
  for (const AccessSite &site : sites) {
    attachAccessMetadata(site);
    auto type = cast<MemRefType>(site.memref.getType());
    instrumentAccess(site, *getElementSizeBytes(type));
  }
  module->setAttr(kInstrumentedAttribute, UnitAttr::get(&getContext()));
}

void registerCacheScopePasses() {
  PassRegistration<InstrumentMemoryPass>();
  registerCacheScopeLLVMFallbackPass();
}

} // namespace mlir::cachescope
