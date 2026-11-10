#include "cachescope/TraceABI.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
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

struct StageLocation {
  std::string stageId;
  std::int64_t line;
  std::int64_t column;
  std::string operation;

  auto asTuple() const { return std::tie(stageId, line, column, operation); }
  bool operator==(const StageLocation &other) const {
    return asTuple() == other.asTuple();
  }
};

struct AccessDescription {
  std::int64_t memrefId;
  std::int64_t accessKind;
  std::int64_t addressKind;
  std::string sourceLocation;
  std::vector<std::int64_t> originIds;
  std::vector<StageLocation> stageLocations;
};

struct StageDescription {
  std::string stageId;
  std::string path;

  bool operator==(const StageDescription &other) const {
    return stageId == other.stageId && path == other.path;
  }
};

struct OriginDescription {
  std::string stageId;
  std::int64_t line;
  std::int64_t column;
  std::string operation;

  auto asTuple() const { return std::tie(stageId, line, column, operation); }
  bool operator==(const OriginDescription &other) const {
    return asTuple() == other.asTuple();
  }
};

struct SourceMapDescription {
  bool registered = false;
  std::string payloadFingerprint;
  std::map<std::int64_t, StageDescription> stages;
  std::map<std::int64_t, OriginDescription> origins;
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
SourceMapDescription sourceMap;
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

static bool isPayloadFingerprint(std::string_view value) {
  constexpr std::string_view prefix = "sha256:";
  if (value.size() != prefix.size() + 64 ||
      value.substr(0, prefix.size()) != prefix)
    return false;
  for (char character : value.substr(prefix.size()))
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f')))
      return false;
  return true;
}

static bool isStageId(std::string_view value) {
  if (value.empty() || value.front() < 'a' || value.front() > 'z')
    return false;
  for (char character : value.substr(1))
    if (!((character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-'))
      return false;
  return true;
}

static bool isPortableArtifactPath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != path.npos)
    return false;
  while (!path.empty()) {
    const std::size_t separator = path.find('/');
    const std::string_view component = path.substr(0, separator);
    if (component.empty() || component == "." || component == "..")
      return false;
    if (separator == path.npos)
      break;
    path.remove_prefix(separator + 1);
  }
  return true;
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

static bool containsOriginId(const std::vector<std::int64_t> &origins,
                             std::int64_t originId) {
  for (std::int64_t candidate : origins)
    if (candidate == originId)
      return true;
  return false;
}

static bool containsStageLocation(const std::vector<StageLocation> &locations,
                                  const StageLocation &location) {
  for (const StageLocation &candidate : locations)
    if (candidate == location)
      return true;
  return false;
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

static bool validateSourceMapping() {
  bool hasAccessMapping = false;
  for (const auto &[accessId, description] : accessTable) {
    hasAccessMapping |=
        !description.originIds.empty() || !description.stageLocations.empty();
    (void)accessId;
  }
  const bool hasSourceMapData = sourceMap.registered ||
                                !sourceMap.stages.empty() ||
                                !sourceMap.origins.empty();
  if (!hasSourceMapData) {
    if (hasAccessMapping)
      addError("access mapping was registered without a source map");
    return false;
  }

  bool mappingValid = true;
  auto invalidate = [&](std::string message) {
    addError(std::move(message));
    mappingValid = false;
  };
  if (!sourceMap.registered)
    invalidate("source map metadata was registered without a fingerprint");
  if (sourceMap.stages.empty())
    invalidate("source map has no stages");

  std::set<std::string> stageIds;
  std::set<std::string> stagePaths;
  std::int64_t expectedOrdinal = 0;
  for (const auto &[ordinal, stage] : sourceMap.stages) {
    if (ordinal != expectedOrdinal++)
      invalidate("source map stage ordinals must be contiguous");
    if (!stageIds.insert(stage.stageId).second)
      invalidate("source map stage IDs must be unique");
    if (!stagePaths.insert(stage.path).second)
      invalidate("source map stage paths must be unique");
  }
  if (!sourceMap.stages.empty() &&
      sourceMap.stages.begin()->second.stageId != "input")
    invalidate("source map must begin with the input stage");

  for (const auto &[originId, origin] : sourceMap.origins) {
    if (origin.stageId != "input" || stageIds.count(origin.stageId) == 0)
      invalidate("source map origin must reference the input stage");
    (void)originId;
  }

  for (const auto &[accessId, description] : accessTable) {
    const bool hasOrigins = !description.originIds.empty();
    const bool hasStageLocations = !description.stageLocations.empty();
    if (description.addressKind == kSemanticAddress &&
        (!hasOrigins || !hasStageLocations)) {
      invalidate("semantic access is missing source mapping metadata");
      continue;
    }
    for (std::int64_t originId : description.originIds)
      if (sourceMap.origins.count(originId) == 0)
        invalidate("access mapping references an unknown origin_id");
    for (const StageLocation &location : description.stageLocations)
      if (stageIds.count(location.stageId) == 0)
        invalidate("access mapping references an unknown stage_id");
    (void)accessId;
  }
  return mappingValid;
}

static void writeSourceMap(std::ostream &output) {
  output << "  \"source_map\": {\"schema\": \"cachescope.source-map.v1\", "
         << "\"payload_fingerprint\": \""
         << jsonEscape(sourceMap.payloadFingerprint) << "\", \"stages\": [";
  std::size_t stageIndex = 0;
  for (const auto &[ordinal, stage] : sourceMap.stages) {
    output << (stageIndex++ == 0 ? "" : ",") << "{\"stage_id\": \""
           << jsonEscape(stage.stageId) << "\", \"path\": \""
           << jsonEscape(stage.path) << "\"}";
    (void)ordinal;
  }
  output << "], \"origins\": [";
  std::size_t originIndex = 0;
  for (const auto &[originId, origin] : sourceMap.origins) {
    output << (originIndex++ == 0 ? "" : ",") << "{\"origin_id\": " << originId
           << ", \"stage_id\": \"" << jsonEscape(origin.stageId)
           << "\", \"line\": " << origin.line
           << ", \"column\": " << origin.column << ", \"operation\": \""
           << jsonEscape(origin.operation) << "\"}";
  }
  output << "]},\n";
}

static void writeAccessMapping(std::ostream &output,
                               const AccessDescription &description) {
  output << ", \"origin_ids\": [";
  for (std::size_t index = 0; index < description.originIds.size(); ++index) {
    if (index != 0)
      output << ',';
    output << description.originIds[index];
  }
  std::map<std::string, std::int64_t> stageOrdinals;
  for (const auto &[ordinal, stage] : sourceMap.stages)
    stageOrdinals.emplace(stage.stageId, ordinal);
  std::vector<const StageLocation *> locations;
  locations.reserve(description.stageLocations.size());
  for (const StageLocation &location : description.stageLocations)
    locations.push_back(&location);
  std::sort(locations.begin(), locations.end(),
            [&](const StageLocation *left, const StageLocation *right) {
              return std::tuple(stageOrdinals.at(left->stageId), left->line,
                                left->column, left->operation) <
                     std::tuple(stageOrdinals.at(right->stageId), right->line,
                                right->column, right->operation);
            });

  output << "], \"stage_locations\": [";
  for (std::size_t index = 0; index < locations.size(); ++index) {
    if (index != 0)
      output << ',';
    const StageLocation &location = *locations[index];
    output << "{\"stage_id\": \"" << jsonEscape(location.stageId)
           << "\", \"line\": " << location.line
           << ", \"column\": " << location.column << ", \"operation\": \""
           << jsonEscape(location.operation) << "\"}";
  }
  output << ']';
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

  const bool emitSourceMapping = validateSourceMapping();
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
  output << (errors.empty() ? "],\n" : "\n  ],\n");
  if (emitSourceMapping)
    writeSourceMap(output);
  output << "  \"accesses\": [\n";

  std::size_t accessIndex = 0;
  for (const auto &[accessId, description] : accessTable) {
    output << "    {\"access_id\": " << accessId
           << ", \"memref_id\": " << description.memrefId << ", \"kind\": \""
           << kindName(description.accessKind) << "\", \"address_kind\": \""
           << addressKindName(description.addressKind)
           << "\", \"source_location\": \""
           << jsonEscape(description.sourceLocation) << "\"";
    if (emitSourceMapping &&
        (!description.originIds.empty() || !description.stageLocations.empty()))
      writeAccessMapping(output, description);
    output << "}";
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

  AccessDescription description{memrefId,  accessKind, addressKind,
                                "unknown", {},         {}};
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
__cachescope_register_source_map_v1(const char *payloadFingerprint) {
  initializeRuntime();
  const std::string fingerprint = payloadFingerprint ? payloadFingerprint : "";
  if (!isPayloadFingerprint(fingerprint)) {
    addError("source map payload fingerprint is invalid");
    return;
  }
  if (sourceMap.registered && sourceMap.payloadFingerprint != fingerprint) {
    addError("conflicting source map registrations");
    return;
  }
  sourceMap.registered = true;
  sourceMap.payloadFingerprint = fingerprint;
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_register_stage_v1(std::int64_t ordinal, const char *stageId,
                               const char *path) {
  initializeRuntime();
  const std::string id = stageId ? stageId : "";
  const std::string artifactPath = path ? path : "";
  if (ordinal < 0 || !isStageId(id) || !isPortableArtifactPath(artifactPath)) {
    addError("source map stage registration is invalid");
    return;
  }
  StageDescription description{id, artifactPath};
  auto [entry, inserted] = sourceMap.stages.try_emplace(ordinal, description);
  if (!inserted && !(entry->second == description))
    addError("conflicting source map stage registrations");
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_register_origin_v1(std::int64_t originId, const char *stageId,
                                std::int64_t line, std::int64_t column,
                                const char *operation) {
  initializeRuntime();
  const std::string id = stageId ? stageId : "";
  const std::string name = operation ? operation : "";
  if (originId < 0 || !isStageId(id) || line < 1 || column < 0 ||
      name.empty()) {
    addError("source map origin registration is invalid");
    return;
  }
  OriginDescription description{id, line, column, name};
  auto [entry, inserted] = sourceMap.origins.try_emplace(originId, description);
  if (!inserted && !(entry->second == description))
    addError("conflicting source map origin registrations");
}

extern "C" CACHESCOPE_EXPORT void
__cachescope_register_access_origin_v1(std::int64_t accessId,
                                       std::int64_t originId) {
  initializeRuntime();
  auto access = accessTable.find(accessId);
  if (access == accessTable.end()) {
    addError("access origin references an unregistered access_id");
    return;
  }
  if (originId < 0) {
    addError("access origin_id must be non-negative");
    return;
  }
  auto &origins = access->second.originIds;
  if (!containsOriginId(origins, originId)) {
    origins.push_back(originId);
    std::sort(origins.begin(), origins.end());
  }
}

extern "C" CACHESCOPE_EXPORT void __cachescope_register_stage_location_v1(
    std::int64_t accessId, const char *stageId, std::int64_t line,
    std::int64_t column, const char *operation) {
  initializeRuntime();
  auto access = accessTable.find(accessId);
  if (access == accessTable.end()) {
    addError("stage location references an unregistered access_id");
    return;
  }
  const std::string id = stageId ? stageId : "";
  const std::string name = operation ? operation : "";
  if (!isStageId(id) || line < 1 || column < 0 || name.empty()) {
    addError("stage location registration is invalid");
    return;
  }
  StageLocation location{id, line, column, name};
  auto &locations = access->second.stageLocations;
  if (!containsStageLocation(locations, location)) {
    locations.push_back(std::move(location));
    std::sort(locations.begin(), locations.end(),
              [](const StageLocation &left, const StageLocation &right) {
                return left.asTuple() < right.asTuple();
              });
  }
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
  sourceMap = {};
  eventCount = 0;
  flushed = false;
  readTraceLimit();
}
