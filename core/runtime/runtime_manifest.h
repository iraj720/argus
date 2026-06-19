#ifndef ARGUS_CORE_RUNTIME_RUNTIME_MANIFEST_H
#define ARGUS_CORE_RUNTIME_RUNTIME_MANIFEST_H

#include "core/sinks/hls_sink.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace irs3 {

inline constexpr int kRuntimeManifestVersion = 2;

struct RuntimeSourceSpec {
    std::string source_id;
};

struct RuntimeSinkInputSpec {
    std::string kind;
    std::string id;
    std::string stream_type;
    std::string packet_type;
    std::string stream_id;
};

struct RuntimeSinkSpec {
    std::string sink_id;
    std::string output_root;
    irs3_hls_sink_output_mode output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
    std::vector<RuntimeSinkInputSpec> inputs;
};

struct RuntimeManifest {
    int manifest_version = kRuntimeManifestVersion;
    std::vector<RuntimeSourceSpec> sources;
    std::vector<RuntimeSinkSpec> sinks;
};

struct RuntimeManifestValidationResult {
    bool ok = false;
    std::string error;
};

struct RuntimeDesiredState {
    std::unordered_map<std::string, RuntimeSourceSpec> sources_by_id;
    std::unordered_map<std::string, RuntimeSinkSpec> sinks_by_id;
    std::unordered_map<std::string, std::vector<std::string>> source_ids_by_sink_id;
    std::unordered_map<std::string, std::vector<std::string>> sink_ids_by_source_id;
};

RuntimeManifestValidationResult ValidateRuntimeManifest(
    const RuntimeManifest &manifest,
    const std::unordered_set<std::string> &active_source_ids,
    bool require_active_sources
);

RuntimeDesiredState BuildRuntimeDesiredState(const RuntimeManifest &manifest);

bool SinkSpecsEqual(const RuntimeSinkSpec &left, const RuntimeSinkSpec &right);

} // namespace irs3

#endif
