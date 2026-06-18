#ifndef ARGUS_CORE_COMPOSE_COMPOSE_TYPE_H
#define ARGUS_CORE_COMPOSE_COMPOSE_TYPE_H

#include <string>

namespace irs3 {

inline constexpr const char *kComposeTypeJpgSnapshot = "jpg_snapshot";
inline constexpr const char *kComposeTypeSideBySide = "side_by_side";
inline constexpr const char *kComposeTypeClipPrompt = "clip_prompt";

bool IsKnownComposeType(const std::string &compose_type);

} // namespace irs3

#endif
