#ifndef ARGUS_CORE_COMPOSE_ICOMPOSE_H
#define ARGUS_CORE_COMPOSE_ICOMPOSE_H

#include "core/decoder/decoded_video_consumer.h"

namespace irs3 {

class ICompose : public IDecodedVideoConsumer {
public:
    ~ICompose() override = default;

    virtual bool Prepare() = 0;
    virtual void Close() = 0;
};

} // namespace irs3

#endif
