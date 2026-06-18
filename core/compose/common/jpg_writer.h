#ifndef ARGUS_CORE_COMPOSE_COMMON_JPG_WRITER_H
#define ARGUS_CORE_COMPOSE_COMMON_JPG_WRITER_H

#include "core/decoder/video_frame.h"

#include <string>

namespace irs3 {

bool WriteJpegFrame(const VideoFrame &frame, const std::string &path, int quality = 85);
bool EnsureOutputDirectory(const std::string &path);

} // namespace irs3

#endif
