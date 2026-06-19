#ifndef ARGUS_CORE_RUNTIME_RUNTIME_H
#define ARGUS_CORE_RUNTIME_RUNTIME_H

#include "core/runtime/iserver.h"
#include "core/runtime/runtime_http.h"
#include "core/runtime/runtime_manifest.h"
#include "core/runtime/runtime_state.h"
#include "core/sources/source_queue.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace irs3 {

struct RuntimeConfig {
    RuntimeHttpConfig http;
    RuntimeManifest initial_manifest;
};

enum class RuntimeManifestApplyMode {
    kBootstrap,
    kStrictUpsert,
};

struct ApplyManifestResult {
    bool ok = false;
    std::string error;
};

class Runtime {
public:
    Runtime(RuntimeConfig config, std::vector<ServerPtr> servers);
    ~Runtime();

    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    int Start();
    void Close();
    ApplyManifestResult ApplyManifest(const RuntimeManifest &manifest);

private:
    ApplyManifestResult ApplyManifestWithMode(const RuntimeManifest &manifest, RuntimeManifestApplyMode mode);
    void StartServerThreads();
    void JoinThreads();
    void HandleSource(SourcePtr source);
    void CloseActiveState();
    void CloseRouteState(RuntimeActiveRouteState *route_state);
    bool AllSinkSourcesActiveLocked(const RuntimeSinkSpec &sink_spec) const;
    bool StartRouteLocked(const std::string &sink_id, const RuntimeSinkSpec &sink_spec);
    void TryStartPendingSinksLocked();

    RuntimeState state_;
    RuntimeConfig config_;
    std::vector<ServerPtr> servers_;
    std::vector<IServer *> server_ptrs_;
    SourceQueue source_queue_;
    std::atomic<int> remaining_watchers_{0};
    std::atomic<int> server_result_{0};
    std::atomic<bool> closed_{false};
    std::unique_ptr<RuntimeHttpServer> http_server_;
    std::vector<std::thread> server_threads_;
    std::vector<std::thread> watcher_threads_;
};

} // namespace irs3

#endif
