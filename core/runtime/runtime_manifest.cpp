#include "core/runtime/runtime_manifest.h"

#include "core/sources/packet_typing.h"

#include <sstream>
#include <utility>

namespace irs3 {

namespace {

bool has_duplicate_input(const RuntimeSinkSpec &sink) {
    for (std::size_t i = 0; i < sink.inputs.size(); ++i) {
        for (std::size_t j = i + 1; j < sink.inputs.size(); ++j) {
            const RuntimeSinkInputSpec &a = sink.inputs[i];
            const RuntimeSinkInputSpec &b = sink.inputs[j];
            if (a.id == b.id && a.stream_id == b.stream_id && a.packet_type == b.packet_type) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool SinkSpecsEqual(const RuntimeSinkSpec &left, const RuntimeSinkSpec &right) {
    if (left.sink_id != right.sink_id ||
        left.output_root != right.output_root ||
        left.output_mode != right.output_mode ||
        left.inputs.size() != right.inputs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.inputs.size(); ++i) {
        const RuntimeSinkInputSpec &a = left.inputs[i];
        const RuntimeSinkInputSpec &b = right.inputs[i];
        if (a.kind != b.kind || a.id != b.id || a.stream_type != b.stream_type ||
            a.packet_type != b.packet_type || a.stream_id != b.stream_id) {
            return false;
        }
    }
    return true;
}

RuntimeManifestValidationResult ValidateRuntimeManifest(
    const RuntimeManifest &manifest,
    const std::unordered_set<std::string> &active_source_ids,
    bool require_active_sources
) {
    RuntimeManifestValidationResult result;

    if (manifest.manifest_version != kRuntimeManifestVersion) {
        result.error = "manifest_version must be 2";
        return result;
    }

    std::unordered_set<std::string> source_ids;
    std::unordered_set<std::string> sink_ids;

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
        if (sink.output_root.empty()) {
            result.error = "sink output_root must not be empty for sink_id: " + sink.sink_id;
            return result;
        }
        if (sink.output_mode != IRS3_HLS_SINK_OUTPUT_RECORD &&
            sink.output_mode != IRS3_HLS_SINK_OUTPUT_LIVE) {
            result.error = "unsupported sink output mode for sink_id: " + sink.sink_id;
            return result;
        }
        if (sink.inputs.empty()) {
            result.error = "sink inputs must not be empty for sink_id: " + sink.sink_id;
            return result;
        }
        if (has_duplicate_input(sink)) {
            result.error = "duplicate sink input for sink_id: " + sink.sink_id;
            return result;
        }

        bool has_video = false;
        for (const RuntimeSinkInputSpec &input : sink.inputs) {
            if (input.kind != "source") {
                result.error = "sink input kind must be source for sink_id: " + sink.sink_id;
                return result;
            }
            if (input.id.empty() || input.stream_type.empty() || input.packet_type.empty() ||
                input.stream_id.empty()) {
                result.error = "sink input requires kind, id, stream_type, packet_type, stream_id for sink_id: " + sink.sink_id;
                return result;
            }
            if (source_ids.find(input.id) == source_ids.end()) {
                result.error = "sink input references unknown source id: " + input.id;
                return result;
            }
            if (!IsRemuxAllowedStreamType(input.stream_type)) {
                result.error = "sink input stream_type must be video or voice for sink_id: " + sink.sink_id;
                return result;
            }
            if (!PacketTypeMatchesStreamType(input.packet_type, input.stream_type)) {
                result.error = "sink input packet_type does not match stream_type for sink_id: " + sink.sink_id;
                return result;
            }
            if (input.stream_type == "video") {
                has_video = true;
            }
            if (require_active_sources &&
                active_source_ids.find(input.id) == active_source_ids.end()) {
                result.error = "sink input references inactive source id: " + input.id;
                return result;
            }
        }
        if (!has_video) {
            result.error = "sink must include at least one video input for sink_id: " + sink.sink_id;
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
    }

    for (const RuntimeSinkSpec &sink : manifest.sinks) {
        desired.sinks_by_id.emplace(sink.sink_id, sink);
        std::vector<std::string> source_ids;
        for (const RuntimeSinkInputSpec &input : sink.inputs) {
            if (std::find(source_ids.begin(), source_ids.end(), input.id) == source_ids.end()) {
                source_ids.push_back(input.id);
            }
            desired.sink_ids_by_source_id[input.id].push_back(sink.sink_id);
        }
        desired.source_ids_by_sink_id.emplace(sink.sink_id, std::move(source_ids));
    }

    return desired;
}

} // namespace irs3
