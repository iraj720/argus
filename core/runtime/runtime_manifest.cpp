#include "core/runtime/runtime_manifest.h"

#include <utility>

namespace irs3 {

RuntimeManifestValidationResult ValidateRuntimeManifest(
    const RuntimeManifest &manifest,
    const std::unordered_set<std::string> &active_source_ids,
    bool require_active_sources
) {
    RuntimeManifestValidationResult result;
    std::unordered_set<std::string> source_ids;
    std::unordered_set<std::string> sink_ids;
    std::unordered_set<std::string> decoder_ids;
    std::unordered_set<std::string> compose_ids;

    for (const RuntimeSourceSpec &source : manifest.sources) {
        if (source.source_id.empty()) {
            result.error = "source_id must not be empty";
            return result;
        }
        if (!source_ids.insert(source.source_id).second) {
            result.error = "duplicate source_id: " + source.source_id;
            return result;
        }
    }

    for (const RuntimeSinkSpec &sink : manifest.sinks) {
        if (sink.sink_id.empty()) {
            result.error = "sink_id must not be empty";
            return result;
        }
        if (!sink_ids.insert(sink.sink_id).second) {
            result.error = "duplicate sink_id: " + sink.sink_id;
            return result;
        }
        if (sink.source_id.empty()) {
            result.error = "sink source_id must not be empty for sink_id: " + sink.sink_id;
            return result;
        }
        if (sink.output_root.empty()) {
            result.error = "sink output_root must not be empty for sink_id: " + sink.sink_id;
            return result;
        }
        if (sink.output_mode != IRS3_HLS_SINK_OUTPUT_RECORD &&
            sink.output_mode != IRS3_HLS_SINK_OUTPUT_LIVE) {
            result.error = "unsupported sink output mode for sink_id: " + sink.sink_id;
            return result;
        }
        if (source_ids.find(sink.source_id) == source_ids.end()) {
            result.error = "sink references unknown source_id in manifest: " + sink.source_id;
            return result;
        }
        if (require_active_sources &&
            active_source_ids.find(sink.source_id) == active_source_ids.end()) {
            result.error = "sink references inactive source_id: " + sink.source_id;
            return result;
        }
    }

    for (const RuntimeDecoderSpec &decoder : manifest.decoders) {
        if (decoder.decoder_id.empty()) {
            result.error = "decoder_id must not be empty";
            return result;
        }
        if (!decoder_ids.insert(decoder.decoder_id).second) {
            result.error = "duplicate decoder_id: " + decoder.decoder_id;
            return result;
        }
        if (decoder.source_id.empty()) {
            result.error = "decoder source_id must not be empty for decoder_id: " + decoder.decoder_id;
            return result;
        }
        if (source_ids.find(decoder.source_id) == source_ids.end()) {
            result.error = "decoder references unknown source_id in manifest: " + decoder.source_id;
            return result;
        }
        if (require_active_sources &&
            active_source_ids.find(decoder.source_id) == active_source_ids.end()) {
            result.error = "decoder references inactive source_id: " + decoder.source_id;
            return result;
        }
    }

    for (const RuntimeComposeSpec &compose : manifest.composes) {
        if (compose.compose_id.empty()) {
            result.error = "compose_id must not be empty";
            return result;
        }
        if (!compose_ids.insert(compose.compose_id).second) {
            result.error = "duplicate compose_id: " + compose.compose_id;
            return result;
        }
        if (compose.decoder_id.empty()) {
            result.error = "compose decoder_id must not be empty for compose_id: " + compose.compose_id;
            return result;
        }
        if (compose.output_root.empty()) {
            result.error = "compose output_root must not be empty for compose_id: " + compose.compose_id;
            return result;
        }
        if (compose.snapshot_interval == 0) {
            result.error = "compose snapshot_interval must be greater than zero for compose_id: " + compose.compose_id;
            return result;
        }
        if (!IsKnownComposeType(compose.compose_type)) {
            result.error = "compose references unknown compose_type for compose_id: " + compose.compose_id;
            return result;
        }
        if (compose.compose_type == kComposeTypeClipPrompt && compose.prompt.empty()) {
            result.error = "clip_prompt compose requires prompt for compose_id: " + compose.compose_id;
            return result;
        }
        if (decoder_ids.find(compose.decoder_id) == decoder_ids.end()) {
            result.error = "compose references unknown decoder_id in manifest: " + compose.decoder_id;
            return result;
        }
    }

    result.ok = true;
    return result;
}

RuntimeDesiredState BuildRuntimeDesiredState(const RuntimeManifest &manifest) {
    RuntimeDesiredState desired;

    for (const RuntimeSourceSpec &source : manifest.sources) {
        desired.sources_by_id.emplace(source.source_id, source);
        desired.sink_ids_by_source_id.try_emplace(source.source_id, std::vector<std::string>{});
        desired.decoder_ids_by_source_id.try_emplace(source.source_id, std::vector<std::string>{});
    }

    for (const RuntimeSinkSpec &sink : manifest.sinks) {
        desired.sinks_by_id.emplace(sink.sink_id, sink);
        desired.routes_by_sink_id.emplace(sink.sink_id, sink.source_id);
        desired.sink_ids_by_source_id[sink.source_id].push_back(sink.sink_id);
    }

    for (const RuntimeDecoderSpec &decoder : manifest.decoders) {
        desired.decoders_by_id.emplace(decoder.decoder_id, decoder);
        desired.routes_by_decoder_id.emplace(decoder.decoder_id, decoder.source_id);
        desired.decoder_ids_by_source_id[decoder.source_id].push_back(decoder.decoder_id);
        desired.compose_ids_by_decoder_id.try_emplace(decoder.decoder_id, std::vector<std::string>{});
    }

    for (const RuntimeComposeSpec &compose : manifest.composes) {
        desired.composes_by_id.emplace(compose.compose_id, compose);
        desired.routes_by_compose_id.emplace(compose.compose_id, compose.decoder_id);
        desired.compose_ids_by_decoder_id[compose.decoder_id].push_back(compose.compose_id);
    }

    return desired;
}

} // namespace irs3
