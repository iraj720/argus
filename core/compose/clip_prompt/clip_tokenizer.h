#ifndef ARGUS_CORE_COMPOSE_CLIP_PROMPT_CLIP_TOKENIZER_H
#define ARGUS_CORE_COMPOSE_CLIP_PROMPT_CLIP_TOKENIZER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace irs3 {

class ClipTokenizer {
public:
    static constexpr std::size_t kMaxLength = 77;

    bool Load(const std::string &vocab_path, const std::string &merges_path, std::string *error);
    std::vector<int64_t> Encode(const std::string &text) const;

private:
    std::unordered_map<std::string, int64_t> vocab_;
    std::vector<std::pair<std::string, std::string>> merges_;
    int64_t start_token_id_ = 49406;
    int64_t end_token_id_ = 49407;

    std::string CleanText(const std::string &text) const;
    std::vector<std::string> SplitWords(const std::string &text) const;
    std::vector<std::string> BytePairEncode(const std::string &token) const;
    int64_t LookupToken(const std::string &token) const;
};

} // namespace irs3

#endif
