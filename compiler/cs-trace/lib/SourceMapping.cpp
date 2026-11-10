#include "CacheScope/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Pass/Pass.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>
#include <string>

namespace mlir::cachescope {
namespace {

constexpr StringLiteral kOriginsAssignedAttribute =
    "cachescope.origins_assigned";
constexpr StringLiteral kOriginIdMetadata = "cachescope.origin_id";
constexpr StringLiteral kOriginOperationMetadata =
    "cachescope.origin_operation";
constexpr StringLiteral kStageIdMetadata = "cachescope.stage_id";
constexpr StringLiteral kStageOperationMetadata = "cachescope.stage_operation";

class AssignOriginsPass
    : public PassWrapper<AssignOriginsPass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AssignOriginsPass)

  AssignOriginsPass() = default;
  AssignOriginsPass(const AssignOriginsPass &pass) : PassWrapper(pass) {}

  StringRef getArgument() const final { return "cachescope-assign-origins"; }
  StringRef getDescription() const final {
    return "Assign deterministic CacheScope origins to high-level memory ops";
  }

protected:
  void runOnOperation() final;

private:
  Option<std::string> targetFunction{
      *this, "target-func",
      llvm::cl::desc("Exact function symbol whose origins should be assigned"),
      llvm::cl::init("")};
};

class TagStagePass : public PassWrapper<TagStagePass, OperationPass<ModuleOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TagStagePass)

  TagStagePass() = default;
  TagStagePass(const TagStagePass &pass) : PassWrapper(pass) {}

  StringRef getArgument() const final { return "cachescope-tag-stage"; }
  StringRef getDescription() const final {
    return "Attach a stage ID and operation name to snapshot locations";
  }

protected:
  void runOnOperation() final;

private:
  Option<std::string> targetFunction{
      *this, "target-func",
      llvm::cl::desc("Exact function symbol whose operations should be tagged"),
      llvm::cl::init("")};
  Option<std::string> stageId{*this, "stage-id",
                              llvm::cl::desc("Stable source-map stage ID"),
                              llvm::cl::init("")};
};

} // namespace

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

static bool isValidStageId(StringRef value) {
  if (value.empty() || value.front() < 'a' || value.front() > 'z')
    return false;
  return llvm::all_of(value.drop_front(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-';
  });
}

static Operation *findTarget(ModuleOp module, StringRef targetFunction) {
  if (targetFunction.empty())
    return nullptr;
  return SymbolTable::lookupSymbolIn(module, targetFunction);
}

void AssignOriginsPass::runOnOperation() {
  ModuleOp module = getOperation();
  if (module->hasAttr(kOriginsAssignedAttribute)) {
    module.emitError("CacheScope origins are already assigned");
    return signalPassFailure();
  }
  Operation *target = findTarget(module, targetFunction);
  if (!target) {
    module.emitError() << "cannot find target function @" << targetFunction;
    return signalPassFailure();
  }

  std::int64_t nextOriginId = 0;
  target->walk([&](Operation *operation) {
    if (!hasDirectMemoryEffect(operation))
      return;
    Builder builder(operation->getContext());
    auto metadata = builder.getDictionaryAttr({
        builder.getNamedAttr(kOriginIdMetadata,
                             builder.getI64IntegerAttr(nextOriginId++)),
        builder.getNamedAttr(
            kOriginOperationMetadata,
            builder.getStringAttr(operation->getName().getStringRef())),
    });
    operation->setLoc(builder.getFusedLoc({operation->getLoc()}, metadata));
  });
  if (nextOriginId == 0) {
    target->emitError("target contains no high-level memory operations");
    return signalPassFailure();
  }
  module->setAttr(kOriginsAssignedAttribute, UnitAttr::get(&getContext()));
}

void TagStagePass::runOnOperation() {
  ModuleOp module = getOperation();
  if (!isValidStageId(stageId)) {
    module.emitError() << "invalid CacheScope stage ID '" << stageId << "'";
    return signalPassFailure();
  }
  Operation *target = findTarget(module, targetFunction);
  if (!target) {
    module.emitError() << "cannot find target function @" << targetFunction;
    return signalPassFailure();
  }

  target->walk([&](Operation *operation) {
    Builder builder(operation->getContext());
    auto metadata = builder.getDictionaryAttr({
        builder.getNamedAttr(kStageIdMetadata, builder.getStringAttr(stageId)),
        builder.getNamedAttr(
            kStageOperationMetadata,
            builder.getStringAttr(operation->getName().getStringRef())),
    });
    operation->setLoc(builder.getFusedLoc({operation->getLoc()}, metadata));
  });
}

void registerCacheScopeSourceMappingPasses() {
  PassRegistration<AssignOriginsPass>();
  PassRegistration<TagStagePass>();
}

} // namespace mlir::cachescope
