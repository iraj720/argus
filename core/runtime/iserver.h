#ifndef ARGUS_CORE_RUNTIME_ISERVER_H
#define ARGUS_CORE_RUNTIME_ISERVER_H

#include "core/sources/isource.h"

#include <memory>

namespace irs3 {

class IServer {
public:
    virtual ~IServer() = default;

    virtual int Start() = 0;
    virtual void Close() = 0;
    virtual SourcePtr NextSource() = 0;
};

using ServerPtr = std::unique_ptr<IServer>;

} // namespace irs3

#endif
