#ifndef ARGUS_CORE_RUNTIME_RUNTIME_HTTP_H
#define ARGUS_CORE_RUNTIME_RUNTIME_HTTP_H

#include "core/runtime/runtime_manifest.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace irs3 {

struct RuntimeHttpConfig {
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 8090;
    bool enabled = true;
};

class RuntimeHttpServer {
public:
    using ManifestHandler = std::function<bool(const RuntimeManifest &, std::string *)>;

    RuntimeHttpServer(RuntimeHttpConfig config, ManifestHandler handler);
    ~RuntimeHttpServer();

    RuntimeHttpServer(const RuntimeHttpServer &) = delete;
    RuntimeHttpServer &operator=(const RuntimeHttpServer &) = delete;

    bool Start();
    void Close();
    std::uint16_t BoundPort() const;

private:
    void Serve();
    int CreateListener();

    RuntimeHttpConfig config_;
    ManifestHandler handler_;
    std::atomic<bool> closing_{false};
    std::atomic<int> listener_fd_{-1};
    std::atomic<std::uint16_t> bound_port_{0};
    std::thread thread_;
};

} // namespace irs3

#endif
