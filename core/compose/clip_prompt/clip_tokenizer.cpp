#include "core/compose/clip_prompt/clip_tokenizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <utility>

namespace irs3 {

namespace {

std::string Trim(const std::string &value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string ToLowerAscii(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

} // namespace

bool ClipTokenizer::Load(const std::string &vocab_path, const std::string &merges_path, std::string *error) {
    vocab_.clear();
    merges_.clear();

    std::ifstream vocab_file(vocab_path);
    if (!vocab_file.is_open()) {
        if (error != nullptr) {
            *error = "failed to open vocab file: " + vocab_path;
        }
        return false;
    }

    nlohmann::json vocab_json;
    try {
        vocab_file >> vocab_json;
    } catch (const nlohmann::json::exception &ex) {
        if (error != nullptr) {
            *error = std::string("failed to parse vocab json: ") + ex.what();
        }
        return false;
    }

    for (const auto &entry : vocab_json.items()) {
        if (!entry.value().is_number_integer()) {
            continue;
        }
        vocab_.emplace(entry.key(), entry.value().get<int64_t>());
    }

    auto start_it = vocab_.find("<|startoftext|>");
    auto end_it = vocab_.find("<|endoftext|>");
    if (start_it == vocab_.end() || end_it == vocab_.end()) {
        if (error != nullptr) {
            *error = "clip vocab missing start/end tokens";
        }
        return false;
    }
    start_token_id_ = start_it->second;
    end_token_id_ = end_it->second;

    std::ifstream merges_file(merges_path);
    if (!merges_file.is_open()) {
        if (error != nullptr) {
            *error = "failed to open merges file: " + merges_path;
        }
        return false;
    }

    std::string line;
    if (!std::getline(merges_file, line)) {
        if (error != nullptr) {
            *error = "merges file is empty";
        }
        return false;
    }

    while (std::getline(merges_file, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        merges_.emplace_back(line.substr(0, space), line.substr(space + 1));
    }

    if (merges_.empty()) {
        if (error != nullptr) {
            *error = "merges file contained no pairs";
        }
        return false;
    }

    return true;
}

std::string ClipTokenizer::CleanText(const std::string &text) const {
    std::string cleaned = ToLowerAscii(text);
    cleaned = std::regex_replace(cleaned, std::regex(R"(\s+)"), " ");
    cleaned = Trim(cleaned);
    return cleaned;
}

std::vector<std::string> ClipTokenizer::SplitWords(const std::string &text) const {
    std::vector<std::string> words;
    std::istringstream stream(text);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

std::vector<std::string> ClipTokenizer::BytePairEncode(const std::string &token) const {
    std::vector<std::string> pieces;
    if (token.empty()) {
        return pieces;
    }

    for (std::size_t i = 0; i < token.size(); ++i) {
        std::string piece = token.substr(i, 1);
        if (i + 1 == token.size()) {
            piece += "</w>";
        }
        pieces.push_back(std::move(piece));
    }

    auto join_pieces = [&pieces]() {
        std::ostringstream joined;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            if (i > 0) {
                joined << ' ';
            }
            joined << pieces[i];
        }
        return joined.str();
    };

    while (pieces.size() > 1) {
        std::size_t best_rank = merges_.size();
        std::size_t best_index = pieces.size();

        for (std::size_t i = 0; i + 1 < pieces.size(); ++i) {
            for (std::size_t rank = 0; rank < merges_.size(); ++rank) {
                if (merges_[rank].first == pieces[i] && merges_[rank].second == pieces[i + 1]) {
                    if (rank < best_rank) {
                        best_rank = rank;
                        best_index = i;
                    }
                    break;
                }
            }
        }

        if (best_index >= pieces.size()) {
            break;
        }

        pieces[best_index] = pieces[best_index] + pieces[best_index + 1];
        pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(best_index + 1));
        (void)join_pieces;
    }

    return pieces;
}

int64_t ClipTokenizer::LookupToken(const std::string &token) const {
    const auto it = vocab_.find(token);
    if (it == vocab_.end()) {
        return 0;
    }
    return it->second;
}

std::vector<int64_t> ClipTokenizer::Encode(const std::string &text) const {
    std::vector<int64_t> token_ids;
    token_ids.reserve(kMaxLength);
    token_ids.push_back(start_token_id_);

    for (const std::string &word : SplitWords(CleanText(text))) {
        for (const std::string &piece : BytePairEncode(word)) {
            token_ids.push_back(LookupToken(piece));
            if (token_ids.size() >= kMaxLength - 1) {
                break;
            }
        }
        if (token_ids.size() >= kMaxLength - 1) {
            break;
        }
    }

    token_ids.push_back(end_token_id_);
    if (token_ids.size() > kMaxLength) {
        token_ids.resize(kMaxLength);
        token_ids[kMaxLength - 1] = end_token_id_;
    }

    while (token_ids.size() < kMaxLength) {
        token_ids.push_back(0);
    }

    return token_ids;
}

} // namespace irs3
