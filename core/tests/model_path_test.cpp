#include "core/compose/clip_prompt/model_path.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    const std::string resolved = irs3::ResolveMobileClip2ModelRoot("./models/mobileclip2_s2");
    expect(
        resolved.find("mobileclip2_s2") != std::string::npos,
        "resolved model root should point at mobileclip2_s2 directory"
    );

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "model_path_test passed resolved=%s\n", resolved.c_str());
    return 0;
}
