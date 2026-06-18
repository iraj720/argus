#include "core/compose/compose_type.h"

namespace irs3 {

bool IsKnownComposeType(const std::string &compose_type) {
    return compose_type == kComposeTypeJpgSnapshot || compose_type == kComposeTypeSideBySide ||
           compose_type == kComposeTypeClipPrompt;
}

} // namespace irs3
