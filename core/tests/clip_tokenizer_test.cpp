#include "core/compose/clip_prompt/clip_tokenizer.h"

#include <cstdio>

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
    irs3::ClipTokenizer tokenizer;
    std::string error;
    expect(
        tokenizer.Load("models/mobileclip2_s2/vocab.json", "models/mobileclip2_s2/merges.txt", &error),
        "tokenizer should load bundled clip vocab files"
    );

    const std::vector<int64_t> tokens = tokenizer.Encode("a minion character");
    expect(tokens.size() == irs3::ClipTokenizer::kMaxLength, "encoded tokens should be padded to 77");
    expect(tokens.front() == 49406, "first token should be startoftext");
    expect(tokens[1] != 0, "prompt should produce non-padding tokens");

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "clip_tokenizer_test passed\n");
    return 0;
}
