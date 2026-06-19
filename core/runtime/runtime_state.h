#ifndef ARGUS_CORE_RUNTIME_RUNTIME_STATE_H
#define ARGUS_CORE_RUNTIME_RUNTIME_STATE_H

#include "core/runtime/runtime_manifest.h"
#include "core/runtime/sink_handle.h"
#include "core/sources/isource.h"

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
};

struct RuntimeActiveSinkState {
    RuntimeSinkSpec spec;
};

struct RuntimeActiveRouteState {
    std::string sink_id;
    std::vector<std::string> source_ids;
    std::vector<SourceSubscriptionPtr> subscriptions;
    std::unique_ptr<SinkHandle> sink_handle;
};

struct RuntimeState {
    std::mutex mutex;
    RuntimeDesiredState desired;
    std::unordered_map<std::string, RuntimeActiveSourceState> active_sources;
    std::unordered_map<std::string, RuntimeActiveSinkState> active_sinks;
    std::unordered_map<std::string, RuntimeActiveRouteState> active_routes;
};

} // namespace irs3

#endif
