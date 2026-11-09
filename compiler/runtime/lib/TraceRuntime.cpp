#include "cachescope/TraceABI.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Event {
  std::uint64_t sequence;
  std::int64_t accessId;
  std::int64_t memrefId;
  std::int64_t byteOffset;
  std::int64_t sizeBytes;
  std::int64_t accessKind;
  std::int64_t index0;
  std::int64_t index1;
  bool processAddress;
};

std::vector<Event> events;
std::uint64_t eventCount = 0;
std::uint64_t traceLimit = 1'000'000;
bool flushRegistered = false;
bool flushed = false;

} // namespace

static void readTraceLimit() {
  traceLimit = 1'000'000;
  if (const char *limit = std::getenv("CACHESCOPE_TRACE_LIMIT")) {
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(limit, &end, 10);
    if (end != limit && *end == '\0')
      traceLimit = parsed;
  }
}

static std::string jsonEscape(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result += ch;
      break;
    }
  }
  return result;
}

static const char *kindName(std::int64_t kind) {
  if (kind == 0)
    return "read";
  if (kind == 1)
    return "write";
  return "read_write";
}

static const char *getenvOr(const char *name, const char *fallback) {
  if (const char *value = std::getenv(name))
    return value;
  return fallback;
}

static void flushTrace() {
  if (flushed)
    return;
  flushed = true;

  const char *outputPath = getenvOr("CACHESCOPE_TRACE_FILE", "trace.json");
  const char *variant = getenvOr("CACHESCOPE_VARIANT", "unknown");
  std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
  if (!output) {
    std::cerr << "CacheScope runtime: cannot open trace file " << outputPath
              << '\n';
    return;
  }

  // The runtime intentionally derives only information visible at the ABI.
  // Source locations and symbolic names belong to compiler metadata, not to
  // the hot dynamic event path.
  std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> accessTable;
  for (const Event &event : events)
    accessTable.try_emplace(event.accessId, event.memrefId, event.accessKind);

  output << "{\n"
         << "  \"schema\": \"cachescope.trace.v1\",\n"
         << "  \"variant\": \"" << jsonEscape(variant) << "\",\n"
         << "  \"producer\": \"instrumented-mlir-execution\",\n"
         << "  \"total_events\": " << eventCount << ",\n"
         << "  \"recorded_events\": " << events.size() << ",\n"
         << "  \"truncated\": "
         << (eventCount > events.size() ? "true" : "false") << ",\n"
         << "  \"accesses\": [\n";

  std::size_t accessIndex = 0;
  for (const auto &[accessId, description] : accessTable) {
    output << "    {\"access_id\": " << accessId
           << ", \"memref_id\": " << description.first << ", \"kind\": \""
           << kindName(description.second) << "\"}";
    output << (++accessIndex == accessTable.size() ? "\n" : ",\n");
  }

  output << "  ],\n"
         << "  \"events\": [\n";
  for (std::size_t i = 0; i < events.size(); ++i) {
    const Event &event = events[i];
    output << "    {\"sequence\": " << event.sequence
           << ", \"access_id\": " << event.accessId
           << ", \"memref_id\": " << event.memrefId
           << ", \"byte_offset\": " << event.byteOffset
           << ", \"size_bytes\": " << event.sizeBytes << ", \"kind\": \""
           << kindName(event.accessKind) << "\", \"indices\": [" << event.index0
           << ", " << event.index1 << "], \"address_kind\": \""
           << (event.processAddress ? "process_address" : "memref_offset")
           << "\"}";
    output << (i + 1 == events.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
}

static void initializeRuntime() {
  if (flushRegistered)
    return;
  flushRegistered = true;

  readTraceLimit();
  events.reserve(static_cast<std::size_t>(
      traceLimit < 16384 ? traceLimit : std::uint64_t{16384}));
  std::atexit(flushTrace);
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_trace_v1(std::int64_t accessId, std::int64_t memrefId,
                      std::int64_t byteOffset, std::int64_t sizeBytes,
                      std::int64_t accessKind, std::int64_t index0,
                      std::int64_t index1) {
  initializeRuntime();
  const std::uint64_t sequence = eventCount++;
  if (events.size() >= traceLimit)
    return;
  events.push_back({sequence, accessId, memrefId, byteOffset, sizeBytes,
                    accessKind, index0, index1, false});
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_trace_fallback_v1(std::int64_t accessId, std::int64_t address,
                               std::int64_t sizeBytes,
                               std::int64_t accessKind) {
  initializeRuntime();
  const std::uint64_t sequence = eventCount++;
  if (events.size() >= traceLimit)
    return;
  events.push_back(
      {sequence, accessId, -1, address, sizeBytes, accessKind, -1, -1, true});
}

extern "C" CACHESCOPE_EXPORT void __cachescope_trace_flush_v1() {
  initializeRuntime();
  flushTrace();
}

extern "C" CACHESCOPE_EXPORT void __cachescope_trace_reset_v1() {
  initializeRuntime();
  events.clear();
  eventCount = 0;
  flushed = false;
  readTraceLimit();
}
