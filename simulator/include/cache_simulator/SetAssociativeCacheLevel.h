#pragma once

#include "cache_simulator/CacheLevel.h"
#include "cache_simulator/ReplacementPolicy.h"
#include <functional>
#include <memory>
#include <vector>

namespace cache_simulator {

struct CacheLevelConfig {
  std::size_t capacity_bytes;
  std::size_t line_size_bytes;
  std::size_t associativity;
};

// Called once per set; each invocation must return an independent policy.
using ReplacementPolicyFactory =
    std::function<std::unique_ptr<IReplacementPolicy>()>;

class SetAssociativeCacheLevel final : public ICacheLevel {
public:
  SetAssociativeCacheLevel(CacheLevelConfig config,
                           const ReplacementPolicyFactory &policy_factory);

  LookupResult lookup(std::uint64_t line_address) const override;
  void touch(std::uint64_t line_address, AccessType type) override;
  InsertResult insert(CacheLine line) override;
  std::optional<CacheLine> invalidate(std::uint64_t line_address) override;
  void clear() override;

private:
  struct Set {
    std::vector<std::optional<CacheLine>> lines;
    std::unique_ptr<IReplacementPolicy> policy;
  };

  std::size_t compute_set_index(std::uint64_t line_address) const;

  CacheLevelConfig config_;
  std::vector<Set> sets_;
};

} // namespace cache_simulator
