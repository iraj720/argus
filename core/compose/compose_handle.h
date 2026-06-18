#ifndef ARGUS_CORE_COMPOSE_COMPOSE_HANDLE_H
#define ARGUS_CORE_COMPOSE_COMPOSE_HANDLE_H

#include "core/compose/icompose.h"
#include "core/decoder/decoder_handle.h"
#include "core/runtime/runtime_manifest.h"

#include <memory>

namespace irs3 {

class ComposeHandle {
public:
    ComposeHandle(DecoderHandle *decoder_handle, RuntimeComposeSpec spec);
    ~ComposeHandle();

    ComposeHandle(const ComposeHandle &) = delete;
    ComposeHandle &operator=(const ComposeHandle &) = delete;

    bool Start();
    void Close();

private:
    RuntimeComposeSpec spec_;
    DecoderHandle *decoder_handle_ = nullptr;
    std::shared_ptr<ICompose> compose_;
    bool closed_ = false;
};

} // namespace irs3

#endif
