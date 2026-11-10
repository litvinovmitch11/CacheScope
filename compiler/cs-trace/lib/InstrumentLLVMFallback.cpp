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
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace mlir::cachescope {
namespace {

constexpr StringLiteral kFallbackTraceFunction =
    "__cachescope_trace_fallback_v1";
constexpr StringLiteral kRegisterAccessFunction =
    "__cachescope_register_access_v1";
constexpr StringLiteral kRegisterSourceFunction =
    "__cachescope_register_source_v1";
constexpr StringLiteral kRegisterSourceMapFunction =
    "__cachescope_register_source_map_v1";
constexpr StringLiteral kRegisterStageFunction =
    "__cachescope_register_stage_v1";
constexpr StringLiteral kRegisterOriginFunction =
    "__cachescope_register_origin_v1";
constexpr StringLiteral kRegisterAccessOriginFunction =
    "__cachescope_register_access_origin_v1";
constexpr StringLiteral kRegisterStageLocationFunction =
    "__cachescope_register_stage_location_v1";
constexpr StringLiteral kAccessIdMetadata = "cachescope.access_id";
constexpr StringLiteral kMemrefIdMetadata = "cachescope.memref_id";
constexpr StringLiteral kOriginIdMetadata = "cachescope.origin_id";
constexpr StringLiteral kOriginOperationMetadata =
    "cachescope.origin_operation";
constexpr StringLiteral kStageIdMetadata = "cachescope.stage_id";
constexpr StringLiteral kStageOperationMetadata = "cachescope.stage_operation";
constexpr StringLiteral kStageTagPrefix = "cachescope.stage.";
constexpr StringLiteral kInstrumentedAttribute =
    "cachescope.llvm_fallback_instrumented";
constexpr std::int64_t kRead = 0;
constexpr std::int64_t kWrite = 1;
constexpr std::int64_t kReadWrite = 2;
constexpr std::int64_t kSemanticAddress = 0;
constexpr std::int64_t kProcessAddress = 1;
constexpr std::int64_t kFallbackAccessIdBase = std::int64_t{1} << 32;

constexpr std::array<StringLiteral, 5> kCanonicalStages = {
    "input", "after-transform", "after-linalg-to-loops",
    "after-instrumentation", "after-llvm-lowering"};

struct SourcePosition {
  std::string path;
  std::int64_t line;
  std::int64_t column;

  auto asTuple() const { return std::tie(path, line, column); }
  bool operator==(const SourcePosition &other) const {
    return asTuple() == other.asTuple();
  }
};

struct OriginDescription {
  std::int64_t originId;
  std::string operation;
  SourcePosition position;

  bool operator==(const OriginDescription &other) const {
    return originId == other.originId && operation == other.operation &&
           position == other.position;
  }
};

struct StageLocation {
  std::string stageId;
  std::string operation;
  SourcePosition position;

  bool operator==(const StageLocation &other) const {
    return stageId == other.stageId && operation == other.operation &&
           position == other.position;
  }
};

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
  SmallVector<OriginDescription> origins;
  SmallVector<StageLocation> stageLocations;
};

struct SourceMapRegistration {
  std::string payloadFingerprint;
  SmallVector<std::pair<std::string, std::string>> stages;
  std::map<std::int64_t, OriginDescription> origins;
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
  Option<std::string> payloadFingerprint{
      *this, "payload-fingerprint",
      llvm::cl::desc("sha256 fingerprint enabling source-map registration"),
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

static void collectTaggedPositions(Location location, StringRef expectedTag,
                                   SmallVectorImpl<SourcePosition> &positions) {
  if (auto name = dyn_cast<NameLoc>(location)) {
    if (name.getName().getValue() == expectedTag) {
      if (auto file = dyn_cast<FileLineColLoc>(name.getChildLoc())) {
        positions.push_back({file.getFilename().getValue().str(),
                             static_cast<std::int64_t>(file.getLine()),
                             static_cast<std::int64_t>(file.getColumn())});
      }
    }
    collectTaggedPositions(name.getChildLoc(), expectedTag, positions);
    return;
  }
  if (auto fused = dyn_cast<FusedLoc>(location)) {
    for (Location nested : fused.getLocations())
      collectTaggedPositions(nested, expectedTag, positions);
  }
}

static FailureOr<SourcePosition> findUniqueTaggedPosition(Location location,
                                                          StringRef stageId) {
  SmallVector<SourcePosition> positions;
  const std::string tag = (kStageTagPrefix + stageId).str();
  collectTaggedPositions(location, tag, positions);
  llvm::sort(positions,
             [](const SourcePosition &left, const SourcePosition &right) {
               return left.asTuple() < right.asTuple();
             });
  positions.erase(llvm::unique(positions), positions.end());
  if (positions.size() != 1)
    return failure();
  return positions.front();
}

static void collectSourceMapping(Location location,
                                 SmallVectorImpl<OriginDescription> &origins,
                                 SmallVectorImpl<StageLocation> &stages) {
  if (auto name = dyn_cast<NameLoc>(location)) {
    collectSourceMapping(name.getChildLoc(), origins, stages);
    return;
  }
  auto fused = dyn_cast<FusedLoc>(location);
  if (!fused)
    return;

  if (auto metadata = dyn_cast_or_null<DictionaryAttr>(fused.getMetadata())) {
    if (auto originId = metadata.getAs<IntegerAttr>(kOriginIdMetadata)) {
      if (auto operation =
              metadata.getAs<StringAttr>(kOriginOperationMetadata)) {
        if (FailureOr<SourcePosition> position =
                findUniqueTaggedPosition(fused, "input");
            succeeded(position)) {
          origins.push_back(
              {originId.getInt(), operation.getValue().str(), *position});
        }
      }
    }
    if (auto stageId = metadata.getAs<StringAttr>(kStageIdMetadata)) {
      if (auto operation =
              metadata.getAs<StringAttr>(kStageOperationMetadata)) {
        if (FailureOr<SourcePosition> position =
                findUniqueTaggedPosition(fused, stageId.getValue());
            succeeded(position)) {
          stages.push_back({stageId.getValue().str(),
                            operation.getValue().str(), *position});
        }
      }
    }
  }
  for (Location nested : fused.getLocations())
    collectSourceMapping(nested, origins, stages);
}

static bool sameOrigin(const OriginDescription &left,
                       const OriginDescription &right) {
  return left.originId == right.originId && left.operation == right.operation &&
         left.position == right.position;
}

static bool sameStageLocation(const StageLocation &left,
                              const StageLocation &right) {
  return left.stageId == right.stageId && left.operation == right.operation &&
         left.position == right.position;
}

static void normalizeSourceMapping(StaticAccess &access) {
  llvm::sort(access.origins, [](const OriginDescription &left,
                                const OriginDescription &right) {
    return std::tie(left.originId, left.operation, left.position.path,
                    left.position.line, left.position.column) <
           std::tie(right.originId, right.operation, right.position.path,
                    right.position.line, right.position.column);
  });
  access.origins.erase(llvm::unique(access.origins, sameOrigin),
                       access.origins.end());
  llvm::sort(access.stageLocations, [](const StageLocation &left,
                                       const StageLocation &right) {
    return std::tie(left.stageId, left.position.path, left.position.line,
                    left.position.column, left.operation) <
           std::tie(right.stageId, right.position.path, right.position.line,
                    right.position.column, right.operation);
  });
  access.stageLocations.erase(
      llvm::unique(access.stageLocations, sameStageLocation),
      access.stageLocations.end());
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
         *callee == kRegisterSourceMapFunction ||
         *callee == kRegisterStageFunction ||
         *callee == kRegisterOriginFunction ||
         *callee == kRegisterAccessOriginFunction ||
         *callee == kRegisterStageLocationFunction ||
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

static LLVM::LLVMFuncOp getOrCreateSourceMapDeclaration(ModuleOp module) {
  auto pointer = LLVM::LLVMPointerType::get(module.getContext());
  auto type = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(module.getContext()), {pointer});

  if (auto existing =
          module.lookupSymbol<LLVM::LLVMFuncOp>(kRegisterSourceMapFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterSourceMapFunction, type,
                                  LLVM::Linkage::External);
}

static LLVM::LLVMFuncOp getOrCreateStageDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto pointer = LLVM::LLVMPointerType::get(module.getContext());
  auto type = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(module.getContext()), {i64, pointer, pointer});

  if (auto existing =
          module.lookupSymbol<LLVM::LLVMFuncOp>(kRegisterStageFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterStageFunction, type,
                                  LLVM::Linkage::External);
}

static LLVM::LLVMFuncOp getOrCreateOriginDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto pointer = LLVM::LLVMPointerType::get(module.getContext());
  auto type =
      LLVM::LLVMFunctionType::get(LLVM::LLVMVoidType::get(module.getContext()),
                                  {i64, pointer, i64, i64, pointer});

  if (auto existing =
          module.lookupSymbol<LLVM::LLVMFuncOp>(kRegisterOriginFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterOriginFunction, type,
                                  LLVM::Linkage::External);
}

static LLVM::LLVMFuncOp getOrCreateAccessOriginDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto type = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(module.getContext()), {i64, i64});

  if (auto existing = module.lookupSymbol<LLVM::LLVMFuncOp>(
          kRegisterAccessOriginFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterAccessOriginFunction, type,
                                  LLVM::Linkage::External);
}

static LLVM::LLVMFuncOp getOrCreateStageLocationDeclaration(ModuleOp module) {
  auto i64 = IntegerType::get(module.getContext(), 64);
  auto pointer = LLVM::LLVMPointerType::get(module.getContext());
  auto type =
      LLVM::LLVMFunctionType::get(LLVM::LLVMVoidType::get(module.getContext()),
                                  {i64, pointer, i64, i64, pointer});

  if (auto existing = module.lookupSymbol<LLVM::LLVMFuncOp>(
          kRegisterStageLocationFunction)) {
    if (existing.getFunctionType() == type)
      return existing;
    return {};
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  return LLVM::LLVMFuncOp::create(builder, module.getLoc(),
                                  kRegisterStageLocationFunction, type,
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

static bool isPayloadFingerprint(StringRef value) {
  if (!value.consume_front("sha256:") || value.size() != 64)
    return false;
  return llvm::all_of(value, [](char character) {
    return std::isdigit(static_cast<unsigned char>(character)) ||
           (character >= 'a' && character <= 'f');
  });
}

static std::optional<unsigned> stageOrdinal(StringRef stageId) {
  for (auto [index, candidate] : llvm::enumerate(kCanonicalStages))
    if (candidate == stageId)
      return index;
  return std::nullopt;
}

static bool isPortableArtifactPath(StringRef path) {
  if (path.empty() || path.starts_with('/') || path.contains('\\'))
    return false;
  SmallVector<StringRef> components;
  path.split(components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  return llvm::all_of(components, [](StringRef component) {
    return !component.empty() && component != "." && component != "..";
  });
}

static FailureOr<SourceMapRegistration>
buildSourceMapRegistration(const std::map<std::int64_t, StaticAccess> &accesses,
                           StringRef payloadFingerprint) {
  if (!isPayloadFingerprint(payloadFingerprint))
    return failure();

  std::map<std::int64_t, OriginDescription> origins;
  std::map<std::string, std::string> stagePaths;
  std::set<std::string> uniquePaths;
  for (const auto &[accessId, access] : accesses) {
    if (access.addressKind != kSemanticAddress)
      continue;
    if (access.origins.empty() || access.stageLocations.empty())
      return failure();
    for (const OriginDescription &origin : access.origins) {
      auto [entry, inserted] = origins.try_emplace(origin.originId, origin);
      if (!inserted && !sameOrigin(entry->second, origin))
        return failure();
    }
    std::set<std::string> accessStages;
    for (const StageLocation &stage : access.stageLocations) {
      if (!stageOrdinal(stage.stageId))
        return failure();
      accessStages.insert(stage.stageId);
      auto [entry, inserted] =
          stagePaths.try_emplace(stage.stageId, stage.position.path);
      if (!inserted && entry->second != stage.position.path)
        return failure();
    }
    for (StringRef stageId : kCanonicalStages)
      if (accessStages.find(stageId.str()) == accessStages.end())
        return failure();
    (void)accessId;
  }
  if (origins.empty() || stagePaths.find("input") == stagePaths.end())
    return failure();
  for (const auto &[stageId, path] : stagePaths) {
    if (!isPortableArtifactPath(path) || !uniquePaths.insert(path).second)
      return failure();
    (void)stageId;
  }
  for (const auto &[originId, origin] : origins) {
    if (origin.position.path != stagePaths.at("input"))
      return failure();
    (void)originId;
  }

  SourceMapRegistration registration;
  registration.payloadFingerprint = payloadFingerprint.str();
  for (StringRef stageId : kCanonicalStages) {
    auto path = stagePaths.find(stageId.str());
    if (path == stagePaths.end())
      continue;
    registration.stages.emplace_back(stageId.str(), path->second);
  }
  registration.origins = std::move(origins);
  return registration;
}

static LLVM::GlobalOp createStringGlobal(ModuleOp module, StringRef value,
                                         StringRef prefix) {
  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBody());
  SmallVector<std::uint8_t> bytes(value.bytes_begin(), value.bytes_end());
  bytes.push_back('\0');
  auto tensorType = RankedTensorType::get(
      {static_cast<std::int64_t>(bytes.size())}, builder.getI8Type());
  auto data = DenseElementsAttr::get(tensorType, ArrayRef(bytes));
  auto arrayType = LLVM::LLVMArrayType::get(builder.getI8Type(), bytes.size());
  std::string symbol = prefix.str();
  while (module.lookupSymbol(symbol))
    symbol.push_back('_');
  return LLVM::GlobalOp::create(builder, module.getLoc(), arrayType,
                                /*isConstant=*/true, LLVM::Linkage::Private,
                                symbol, data);
}

static Value getStringAddress(OpBuilder &builder, Location location,
                              LLVM::GlobalOp global) {
  auto pointer = LLVM::LLVMPointerType::get(builder.getContext());
  Value address =
      LLVM::AddressOfOp::create(builder, location, pointer, global.getName());
  auto arrayType = cast<LLVM::LLVMArrayType>(global.getType());
  SmallVector<LLVM::GEPArg> indices(1, 0);
  return LLVM::GEPOp::create(builder, location, pointer, arrayType, address,
                             indices);
}

static void insertStaticRegistrations(
    ModuleOp module, LLVM::LLVMFuncOp function,
    SmallVectorImpl<StaticAccess> &accesses,
    LLVM::LLVMFuncOp registrationFunction, LLVM::LLVMFuncOp sourceFunction,
    const SourceMapRegistration *sourceMap, LLVM::LLVMFuncOp sourceMapFunction,
    LLVM::LLVMFuncOp stageFunction, LLVM::LLVMFuncOp originFunction,
    LLVM::LLVMFuncOp accessOriginFunction,
    LLVM::LLVMFuncOp stageLocationFunction) {
  llvm::sort(accesses, [](const StaticAccess &left, const StaticAccess &right) {
    return left.accessId < right.accessId;
  });

  struct StageLocationGlobals {
    StageLocation *location;
    LLVM::GlobalOp stageId;
    LLVM::GlobalOp operation;
  };
  struct RegistrationGlobals {
    StaticAccess *access;
    LLVM::GlobalOp source;
    SmallVector<StageLocationGlobals> stageLocations;
  };
  SmallVector<RegistrationGlobals> globals;
  for (StaticAccess &access : accesses) {
    const std::string id = std::to_string(access.accessId);
    LLVM::GlobalOp source = createStringGlobal(module, access.sourceLocation,
                                               "__cachescope_source_" + id);
    SmallVector<StageLocationGlobals> locationGlobals;
    for (auto indexed : llvm::enumerate(access.stageLocations)) {
      StageLocation &location = indexed.value();
      const std::string suffix = id + "_" + std::to_string(indexed.index());
      LLVM::GlobalOp stageId = createStringGlobal(
          module, location.stageId, "__cachescope_location_stage_" + suffix);
      LLVM::GlobalOp operation = createStringGlobal(
          module, location.operation, "__cachescope_location_op_" + suffix);
      locationGlobals.push_back({&location, stageId, operation});
    }
    globals.push_back({&access, source, std::move(locationGlobals)});
  }

  struct StageGlobals {
    std::int64_t ordinal;
    LLVM::GlobalOp stageId;
    LLVM::GlobalOp path;
  };
  struct OriginGlobals {
    std::int64_t originId;
    const OriginDescription *origin;
    LLVM::GlobalOp stageId;
    LLVM::GlobalOp operation;
  };
  LLVM::GlobalOp fingerprintGlobal;
  SmallVector<StageGlobals> stageGlobals;
  SmallVector<OriginGlobals> originGlobals;
  if (sourceMap) {
    fingerprintGlobal = createStringGlobal(
        module, sourceMap->payloadFingerprint, "__cachescope_fingerprint");
    for (auto [ordinal, stage] : llvm::enumerate(sourceMap->stages)) {
      const std::string suffix = std::to_string(ordinal);
      stageGlobals.push_back(
          {static_cast<std::int64_t>(ordinal),
           createStringGlobal(module, stage.first,
                              "__cachescope_stage_id_" + suffix),
           createStringGlobal(module, stage.second,
                              "__cachescope_stage_path_" + suffix)});
    }
    for (auto &[originId, origin] : sourceMap->origins) {
      const std::string suffix = std::to_string(originId);
      originGlobals.push_back(
          {originId, &origin,
           createStringGlobal(module, "input",
                              "__cachescope_origin_stage_" + suffix),
           createStringGlobal(module, origin.operation,
                              "__cachescope_origin_op_" + suffix)});
    }
  }

  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(&function.getBody().front());
  auto i64 = builder.getI64Type();
  auto constant = [&](std::int64_t value) -> Value {
    return LLVM::ConstantOp::create(builder, function.getLoc(), i64, value);
  };

  if (fingerprintGlobal) {
    Value fingerprint =
        getStringAddress(builder, function.getLoc(), fingerprintGlobal);
    LLVM::CallOp::create(builder, function.getLoc(), sourceMapFunction,
                         ValueRange{fingerprint});
    for (const StageGlobals &stage : stageGlobals) {
      Value stageId =
          getStringAddress(builder, function.getLoc(), stage.stageId);
      Value path = getStringAddress(builder, function.getLoc(), stage.path);
      SmallVector<Value, 3> arguments{constant(stage.ordinal), stageId, path};
      LLVM::CallOp::create(builder, function.getLoc(), stageFunction,
                           arguments);
    }
    for (const OriginGlobals &entry : originGlobals) {
      Value stageId =
          getStringAddress(builder, function.getLoc(), entry.stageId);
      Value operation =
          getStringAddress(builder, function.getLoc(), entry.operation);
      SmallVector<Value, 5> arguments{constant(entry.originId), stageId,
                                      constant(entry.origin->position.line),
                                      constant(entry.origin->position.column),
                                      operation};
      LLVM::CallOp::create(builder, function.getLoc(), originFunction,
                           arguments);
    }
  }
  for (RegistrationGlobals &entry : globals) {
    StaticAccess *access = entry.access;
    Value source = getStringAddress(builder, function.getLoc(), entry.source);
    SmallVector<Value, 4> arguments{
        constant(access->accessId), constant(access->memrefId),
        constant(access->kind), constant(access->addressKind)};
    LLVM::CallOp::create(builder, function.getLoc(), registrationFunction,
                         arguments);
    SmallVector<Value, 2> sourceArguments{constant(access->accessId), source};
    LLVM::CallOp::create(builder, function.getLoc(), sourceFunction,
                         sourceArguments);
    if (sourceMap && access->addressKind == kSemanticAddress) {
      for (const OriginDescription &origin : access->origins) {
        SmallVector<Value, 2> originArguments{constant(access->accessId),
                                              constant(origin.originId)};
        LLVM::CallOp::create(builder, function.getLoc(), accessOriginFunction,
                             originArguments);
      }
      for (const StageLocationGlobals &location : entry.stageLocations) {
        Value stageId =
            getStringAddress(builder, function.getLoc(), location.stageId);
        Value operation =
            getStringAddress(builder, function.getLoc(), location.operation);
        SmallVector<Value, 5> locationArguments{
            constant(access->accessId), stageId,
            constant(location.location->position.line),
            constant(location.location->position.column), operation};
        LLVM::CallOp::create(builder, function.getLoc(), stageLocationFunction,
                             locationArguments);
      }
    }
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
      StaticAccess access{accessId.getInt(),
                          memrefId.getInt(),
                          kind,
                          kSemanticAddress,
                          formatSourceLocation(operation->getLoc()),
                          {},
                          {}};
      if (!payloadFingerprint.empty()) {
        collectSourceMapping(operation->getLoc(), access.origins,
                             access.stageLocations);
        normalizeSourceMapping(access);
      }
      auto [entry, inserted] =
          staticAccesses.try_emplace(access.accessId, access);
      if (!inserted &&
          (entry->second.memrefId != access.memrefId ||
           entry->second.kind != access.kind ||
           entry->second.addressKind != access.addressKind ||
           entry->second.origins != access.origins ||
           entry->second.stageLocations != access.stageLocations)) {
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
        StaticAccess{access.accessId,
                     -1,
                     access.kind,
                     kProcessAddress,
                     formatSourceLocation(access.operation->getLoc()),
                     {},
                     {}});
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
  std::optional<SourceMapRegistration> sourceMap;
  LLVM::LLVMFuncOp sourceMapFunction;
  LLVM::LLVMFuncOp stageFunction;
  LLVM::LLVMFuncOp originFunction;
  LLVM::LLVMFuncOp accessOriginFunction;
  LLVM::LLVMFuncOp stageLocationFunction;
  if (!payloadFingerprint.empty()) {
    FailureOr<SourceMapRegistration> registration =
        buildSourceMapRegistration(staticAccesses, payloadFingerprint);
    if (failed(registration)) {
      module.emitError(
          "cannot build a complete source map from the tagged locations");
      return signalPassFailure();
    }
    sourceMap.emplace(std::move(*registration));
    sourceMapFunction = getOrCreateSourceMapDeclaration(module);
    stageFunction = getOrCreateStageDeclaration(module);
    originFunction = getOrCreateOriginDeclaration(module);
    accessOriginFunction = getOrCreateAccessOriginDeclaration(module);
    stageLocationFunction = getOrCreateStageLocationDeclaration(module);
    if (!sourceMapFunction || !stageFunction || !originFunction ||
        !accessOriginFunction || !stageLocationFunction) {
      module.emitError(
          "source mapping runtime symbol has an incompatible type");
      return signalPassFailure();
    }
  }
  SmallVector<StaticAccess, 0> registrations;
  registrations.reserve(staticAccesses.size());
  for (auto &[id, access] : staticAccesses)
    registrations.push_back(std::move(access));
  insertStaticRegistrations(
      module, function, registrations, registrationFunction, sourceFunction,
      sourceMap ? &*sourceMap : nullptr, sourceMapFunction, stageFunction,
      originFunction, accessOriginFunction, stageLocationFunction);
  module->setAttr(kInstrumentedAttribute, UnitAttr::get(&getContext()));
}

void registerCacheScopeLLVMFallbackPass() {
  PassRegistration<InstrumentLLVMFallbackPass>();
}

} // namespace mlir::cachescope
