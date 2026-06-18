#ifndef ARGUS_CORE_RUNTIME_RUNTIME_STATE_H
#define ARGUS_CORE_RUNTIME_RUNTIME_STATE_H

#include "core/compose/compose_handle.h"
#include "core/decoder/decoder_handle.h"
#include "core/sources/isource.h"
#include "core/runtime/runtime_manifest.h"
#include "core/runtime/sink_handle.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace irs3 {

struct RuntimeActiveSourceState {
    SourcePtr source;
    SourceDescriptor descriptor;
    std::vector<std::string> route_sink_ids;
    std::vector<std::string> route_decoder_ids;
};

struct RuntimeActiveSinkState {
    RuntimeSinkSpec spec;
};

struct RuntimeActiveDecoderState {
    RuntimeDecoderSpec spec;
};

struct RuntimeActiveComposeState {
    RuntimeComposeSpec spec;
};

struct RuntimeActiveRouteState {
    std::string sink_id;
    std::string source_id;
    SourceSubscriptionPtr subscription;
    std::unique_ptr<SinkHandle> sink_handle;
};

struct RuntimeActiveDecoderRouteState {
    std::string decoder_id;
    std::string source_id;
    SourceSubscriptionPtr subscription;
    std::unique_ptr<DecoderHandle> decoder_handle;
};

struct RuntimeActiveComposeRouteState {
    std::string compose_id;
    std::string decoder_id;
    std::unique_ptr<ComposeHandle> compose_handle;
};

struct RuntimeState {
    std::mutex mutex;
    RuntimeDesiredState desired;
    std::unordered_map<std::string, RuntimeActiveSourceState> active_sources;
    std::unordered_map<std::string, RuntimeActiveSinkState> active_sinks;
    std::unordered_map<std::string, RuntimeActiveDecoderState> active_decoders;
    std::unordered_map<std::string, RuntimeActiveComposeState> active_composes;
    std::unordered_map<std::string, RuntimeActiveRouteState> active_routes;
    std::unordered_map<std::string, RuntimeActiveDecoderRouteState> active_decoder_routes;
    std::unordered_map<std::string, RuntimeActiveComposeRouteState> active_compose_routes;
};

} // namespace irs3

#endif
