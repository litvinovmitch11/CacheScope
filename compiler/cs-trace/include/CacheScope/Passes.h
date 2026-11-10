#ifndef CACHESCOPE_PASSES_H
#define CACHESCOPE_PASSES_H

namespace mlir::cachescope {

void registerCacheScopePasses();
void registerCacheScopeLLVMFallbackPass();
void registerCacheScopeSourceMappingPasses();

} // namespace mlir::cachescope

#endif // CACHESCOPE_PASSES_H
