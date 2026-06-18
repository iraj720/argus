#ifndef ARGUS_CORE_RUNTIME_RUNTIME_MANIFEST_H
#define ARGUS_CORE_RUNTIME_RUNTIME_MANIFEST_H

#include "core/sinks/hls_sink.h"
#include "core/compose/compose_type.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace irs3 {

struct RuntimeSourceSpec {
    std::string source_id;
};

struct RuntimeSinkSpec {
    std::string sink_id;
    std::string source_id;
    std::string output_root;
    irs3_hls_sink_output_mode output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
};

struct RuntimeDecoderSpec {
    std::string decoder_id;
    std::string source_id;
};

struct RuntimeComposeSpec {
    std::string compose_id;
    std::string decoder_id;
    std::string compose_type = kComposeTypeJpgSnapshot;
    std::string output_root;
    std::string prompt;
    std::string model_root;
    std::size_t snapshot_interval = 50;
};

struct RuntimeManifest {
    std::vector<RuntimeSourceSpec> sources;
    std::vector<RuntimeSinkSpec> sinks;
    std::vector<RuntimeDecoderSpec> decoders;
    std::vector<RuntimeComposeSpec> composes;
};

struct RuntimeManifestValidationResult {
    bool ok = false;
    std::string error;
};

struct RuntimeDesiredState {
    std::unordered_map<std::string, RuntimeSourceSpec> sources_by_id;
    std::unordered_map<std::string, RuntimeSinkSpec> sinks_by_id;
    std::unordered_map<std::string, RuntimeDecoderSpec> decoders_by_id;
    std::unordered_map<std::string, RuntimeComposeSpec> composes_by_id;
    std::unordered_map<std::string, std::string> routes_by_sink_id;
    std::unordered_map<std::string, std::string> routes_by_decoder_id;
    std::unordered_map<std::string, std::string> routes_by_compose_id;
    std::unordered_map<std::string, std::vector<std::string>> sink_ids_by_source_id;
    std::unordered_map<std::string, std::vector<std::string>> decoder_ids_by_source_id;
    std::unordered_map<std::string, std::vector<std::string>> compose_ids_by_decoder_id;
};

RuntimeManifestValidationResult ValidateRuntimeManifest(
    const RuntimeManifest &manifest,
    const std::unordered_set<std::string> &active_source_ids,
    bool require_active_sources
);

RuntimeDesiredState BuildRuntimeDesiredState(const RuntimeManifest &manifest);

} // namespace irs3

#endif
