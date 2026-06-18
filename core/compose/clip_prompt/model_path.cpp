#include "core/compose/clip_prompt/model_path.h"

#include <cstdlib>
#include <fstream>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(__linux__)
#include <unistd.h>
#endif

#include <climits>
#include <cstring>

namespace irs3 {

namespace {

constexpr const char *kVisionModelFile = "vision_model.onnx";

std::string JoinPath(const std::string &root, const char *leaf) {
    if (root.empty()) {
        return leaf;
    }
    if (root.back() == '/') {
        return root + leaf;
    }
    return root + "/" + leaf;
}

bool HasVisionModel(const std::string &root) {
    if (root.empty()) {
        return false;
    }
    std::ifstream file(JoinPath(root, kVisionModelFile));
    return file.good();
}

std::string GetExecutableDir() {
#if defined(__APPLE__)
    char raw_path[PATH_MAX];
    uint32_t size = sizeof(raw_path);
    if (_NSGetExecutablePath(raw_path, &size) != 0) {
        return {};
    }
    char resolved[PATH_MAX];
    if (realpath(raw_path, resolved) == nullptr) {
        return {};
    }
    char *slash = std::strrchr(resolved, '/');
    if (slash == nullptr) {
        return {};
    }
    *slash = '\0';
    return resolved;
#elif defined(__linux__)
    char raw_path[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", raw_path, sizeof(raw_path) - 1);
    if (length <= 0) {
        return {};
    }
    raw_path[length] = '\0';
    char *slash = std::strrchr(raw_path, '/');
    if (slash == nullptr) {
        return {};
    }
    *slash = '\0';
    return raw_path;
#else
    return {};
#endif
}

std::vector<std::string> CandidateModelRoots(const std::string &configured_root) {
    std::vector<std::string> candidates;
    if (!configured_root.empty()) {
        candidates.push_back(configured_root);
    }

    const char *env_root = std::getenv("ARGUS_MOBILECLIP2_MODEL_DIR");
    if (env_root != nullptr && env_root[0] != '\0') {
        candidates.emplace_back(env_root);
    }

    candidates.emplace_back("./models/mobileclip2_s2");

    const std::string executable_dir = GetExecutableDir();
    if (!executable_dir.empty()) {
        candidates.push_back(JoinPath(executable_dir, "../models/mobileclip2_s2"));
        candidates.push_back(JoinPath(executable_dir, "../../models/mobileclip2_s2"));
    }

    return candidates;
}

} // namespace

std::string ResolveMobileClip2ModelRoot(const std::string &configured_root) {
    for (const std::string &candidate : CandidateModelRoots(configured_root)) {
        if (HasVisionModel(candidate)) {
            return candidate;
        }
    }
    return configured_root.empty() ? "./models/mobileclip2_s2" : configured_root;
}

} // namespace irs3
