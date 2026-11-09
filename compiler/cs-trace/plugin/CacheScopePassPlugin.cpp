#include "CacheScope/Passes.h"

#include "mlir/Tools/Plugins/PassPlugin.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/Compiler.h"

using namespace mlir;

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
  return {MLIR_PLUGIN_API_VERSION, "CacheScopePasses", LLVM_VERSION_STRING,
          [] { cachescope::registerCacheScopePasses(); }};
}
