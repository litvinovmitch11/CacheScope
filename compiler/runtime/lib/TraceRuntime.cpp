#include "cachescope/TraceABI.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CACHESCOPE_PROJECT_VERSION
#define CACHESCOPE_PROJECT_VERSION "unknown"
#endif
#ifndef CACHESCOPE_LLVM_PACKAGE_VERSION
#define CACHESCOPE_LLVM_PACKAGE_VERSION "unknown"
#endif
#ifndef CACHESCOPE_LLVM_REVISION
#define CACHESCOPE_LLVM_REVISION "unknown"
#endif

namespace {

constexpr std::int64_t kSemanticAddress = 0;
constexpr std::int64_t kProcessAddress = 1;

struct AccessDescription {
  std::int64_t memrefId;
  std::int64_t accessKind;
  std::int64_t addressKind;
  std::string sourceLocation;
};

struct Event {
  std::uint64_t sequence;
  std::int64_t accessId;
  std::int64_t memrefId;
  std::int64_t byteOffset;
  std::int64_t sizeBytes;
  std::int64_t accessKind;
  bool processAddress;
};

std::map<std::int64_t, AccessDescription> accessTable;
std::vector<Event> events;
std::vector<std::string> errors;
std::uint64_t eventCount = 0;
std::uint64_t traceLimit = 1'000'000;
bool flushRegistered = false;
bool flushed = false;

} // namespace

static void addError(std::string message) {
  if (errors.empty() || errors.back() != message)
    errors.push_back(std::move(message));
}

static void readTraceLimit() {
  traceLimit = 1'000'000;
  if (const char *limit = std::getenv("CACHESCOPE_TRACE_LIMIT")) {
    std::uint64_t parsed = 0;
    const std::string_view text(limit);
    const auto [end, error] =
        std::from_chars(text.begin(), text.end(), parsed, 10);
    if (error == std::errc{} && end == text.end() && !text.empty()) {
      traceLimit = parsed;
      return;
    }
    addError("CACHESCOPE_TRACE_LIMIT is not an unsigned integer");
  }
}

static std::string jsonEscape(std::string_view value) {
  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (unsigned char ch : value) {
    switch (ch) {
    case '\\':
      result << "\\\\";
      break;
    case '"':
      result << "\\\"";
      break;
    case '\b':
      result << "\\b";
      break;
    case '\f':
      result << "\\f";
      break;
    case '\n':
      result << "\\n";
      break;
    case '\r':
      result << "\\r";
      break;
    case '\t':
      result << "\\t";
      break;
    default:
      if (ch < 0x20)
        result << "\\u" << std::setw(4) << static_cast<unsigned>(ch);
      else
        result << static_cast<char>(ch);
      break;
    }
  }
  return result.str();
}

static const char *kindName(std::int64_t kind) {
  if (kind == 0)
    return "read";
  if (kind == 1)
    return "write";
  if (kind == 2)
    return "read_write";
  return "invalid";
}

static const char *addressKindName(std::int64_t kind) {
  if (kind == kSemanticAddress)
    return "memref_offset";
  if (kind == kProcessAddress)
    return "process_address";
  return "invalid";
}

static const char *getenvOr(const char *name, const char *fallback) {
  if (const char *value = std::getenv(name))
    return value;
  return fallback;
}

static void validateEvent(std::int64_t accessId, std::int64_t memrefId,
                          std::int64_t sizeBytes, std::int64_t accessKind,
                          std::int64_t addressKind) {
  if (sizeBytes <= 0)
    addError("event size_bytes must be positive");
  if (accessKind < 0 || accessKind > 2)
    addError("event access kind is invalid");
  auto access = accessTable.find(accessId);
  if (access == accessTable.end()) {
    addError("dynamic event references an unregistered access_id");
    return;
  }
  const AccessDescription &description = access->second;
  if (description.memrefId != memrefId ||
      description.accessKind != accessKind ||
      description.addressKind != addressKind)
    addError("dynamic event conflicts with its static access description");
}

static void flushTrace() {
  if (flushed)
    return;
  flushed = true;

  const char *outputPath = getenvOr("CACHESCOPE_TRACE_FILE", "trace.json");
  const char *variant = getenvOr("CACHESCOPE_VARIANT", "unknown");
  const char *pipelineFingerprint =
      getenvOr("CACHESCOPE_PIPELINE_FINGERPRINT", "unspecified");
  const std::string temporaryPath = std::string(outputPath) + ".tmp";
  std::ofstream output(temporaryPath, std::ios::out | std::ios::trunc);
  if (!output) {
    std::cerr << "CacheScope runtime: cannot open temporary trace file "
              << temporaryPath << '\n';
    return;
  }

  const bool truncated = eventCount > events.size();
  const bool valid = errors.empty();
  output << "{\n"
         << "  \"schema\": \"cachescope.trace.v1\",\n"
         << "  \"producer\": {\n"
         << "    \"name\": \"CacheScope\",\n"
         << "    \"version\": \"" << CACHESCOPE_PROJECT_VERSION << "\",\n"
         << "    \"llvm_version\": \"" << CACHESCOPE_LLVM_PACKAGE_VERSION
         << "\",\n"
         << "    \"llvm_revision\": \"" << CACHESCOPE_LLVM_REVISION << "\",\n"
         << "    \"pipeline_fingerprint\": \""
         << jsonEscape(pipelineFingerprint) << "\"\n"
         << "  },\n"
         << "  \"variant\": \"" << jsonEscape(variant) << "\",\n"
         << "  \"valid\": " << (valid ? "true" : "false") << ",\n"
         << "  \"complete\": " << (valid && !truncated ? "true" : "false")
         << ",\n"
         << "  \"total_events\": " << eventCount << ",\n"
         << "  \"recorded_events\": " << events.size() << ",\n"
         << "  \"truncated\": " << (truncated ? "true" : "false") << ",\n"
         << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    output << (i == 0 ? "\n" : ",\n") << "    \"" << jsonEscape(errors[i])
           << "\"";
  }
  output << (errors.empty() ? "],\n" : "\n  ],\n") << "  \"accesses\": [\n";

  std::size_t accessIndex = 0;
  for (const auto &[accessId, description] : accessTable) {
    output << "    {\"access_id\": " << accessId
           << ", \"memref_id\": " << description.memrefId << ", \"kind\": \""
           << kindName(description.accessKind) << "\", \"address_kind\": \""
           << addressKindName(description.addressKind)
           << "\", \"source_location\": \""
           << jsonEscape(description.sourceLocation) << "\"}";
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
           << kindName(event.accessKind) << "\", \"address_kind\": \""
           << (event.processAddress ? "process_address" : "memref_offset")
           << "\"}";
    output << (i + 1 == events.size() ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  output.close();
  if (!output) {
    std::cerr << "CacheScope runtime: failed to write trace file "
              << temporaryPath << '\n';
    return;
  }
  if (std::rename(temporaryPath.c_str(), outputPath) != 0)
    std::cerr << "CacheScope runtime: cannot publish trace file " << outputPath
              << '\n';
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
__cachescope_register_access_v1(std::int64_t accessId, std::int64_t memrefId,
                                std::int64_t accessKind,
                                std::int64_t addressKind) {
  initializeRuntime();
  bool descriptionValid = true;
  if (accessId < 0) {
    addError("static access_id must be non-negative");
    descriptionValid = false;
  }
  if (accessKind < 0 || accessKind > 2) {
    addError("static access kind is invalid");
    descriptionValid = false;
  }
  if (addressKind != kSemanticAddress && addressKind != kProcessAddress) {
    addError("static address kind is invalid");
    descriptionValid = false;
  }
  if ((addressKind == kSemanticAddress && memrefId < 0) ||
      (addressKind == kProcessAddress && memrefId != -1)) {
    addError("static memref_id conflicts with address kind");
    descriptionValid = false;
  }
  if (!descriptionValid)
    return;

  AccessDescription description{memrefId, accessKind, addressKind, "unknown"};
  auto [entry, inserted] = accessTable.try_emplace(accessId, description);
  if (!inserted && (entry->second.memrefId != memrefId ||
                    entry->second.accessKind != accessKind ||
                    entry->second.addressKind != addressKind))
    addError("conflicting static descriptions for one access_id");
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_register_source_v1(std::int64_t accessId,
                                const char *sourceLocation) {
  initializeRuntime();
  auto access = accessTable.find(accessId);
  if (access == accessTable.end()) {
    addError("source location references an unregistered access_id");
    return;
  }
  const std::string source = sourceLocation ? sourceLocation : "unknown";
  if (access->second.sourceLocation != "unknown" &&
      access->second.sourceLocation != source) {
    addError("conflicting source locations for one access_id");
    return;
  }
  access->second.sourceLocation = source;
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_trace_v1(std::int64_t accessId, std::int64_t memrefId,
                      std::int64_t byteOffset, std::int64_t sizeBytes,
                      std::int64_t accessKind) {
  initializeRuntime();
  validateEvent(accessId, memrefId, sizeBytes, accessKind, kSemanticAddress);
  const std::uint64_t sequence = eventCount++;
  if (events.size() >= traceLimit)
    return;
  events.push_back(
      {sequence, accessId, memrefId, byteOffset, sizeBytes, accessKind, false});
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_trace_fallback_v1(std::int64_t accessId, std::int64_t address,
                               std::int64_t sizeBytes,
                               std::int64_t accessKind) {
  initializeRuntime();
  validateEvent(accessId, -1, sizeBytes, accessKind, kProcessAddress);
  const std::uint64_t sequence = eventCount++;
  if (events.size() >= traceLimit)
    return;
  events.push_back(
      {sequence, accessId, -1, address, sizeBytes, accessKind, true});
}

extern "C" CACHESCOPE_EXPORT void __cachescope_trace_flush_v1() {
  initializeRuntime();
  flushTrace();
}

extern "C" CACHESCOPE_EXPORT void __cachescope_trace_reset_v1() {
  initializeRuntime();
  accessTable.clear();
  events.clear();
  errors.clear();
  eventCount = 0;
  flushed = false;
  readTraceLimit();
}
