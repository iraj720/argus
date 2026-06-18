#include "core/compose/clip_prompt/mobileclip2_s2_engine.h"

#include "core/compose/clip_prompt/clip_tokenizer.h"
#include "core/compose/clip_prompt/model_path.h"

#include <onnxruntime_cxx_api.h>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace irs3 {

namespace {

constexpr int kImageSize = 256;
constexpr int kEmbedDim = 512;
constexpr std::size_t kExpectedVisionModelBytes = 143044797;
constexpr std::size_t kExpectedTextModelBytes = 254053669;

struct OrtDeleter {
    void operator()(Ort::Session *session) const {
        delete session;
    }
};

std::string JoinPath(const std::string &root, const char *leaf) {
    if (root.empty()) {
        return leaf;
    }
    if (root.back() == '/') {
        return root + leaf;
    }
    return root + "/" + leaf;
}

bool FileSizeMatches(const std::string &path, std::size_t expected_bytes, std::string *error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.good()) {
        if (error != nullptr) {
            *error = "missing file: " + path;
        }
        return false;
    }
    const std::size_t actual_bytes = static_cast<std::size_t>(file.tellg());
    if (actual_bytes != expected_bytes) {
        if (error != nullptr) {
            *error = "truncated file: " + path + " (" + std::to_string(actual_bytes) + "/" +
                     std::to_string(expected_bytes) +
                     " bytes). Re-run tools/download_mobileclip2_s2_models.sh";
        }
        return false;
    }
    return true;
}

void NormalizeInPlace(std::vector<float> *values) {
    double sum_sq = 0.0;
    for (float value : *values) {
        sum_sq += static_cast<double>(value) * static_cast<double>(value);
    }
    if (sum_sq <= 0.0) {
        return;
    }
    const float inv_norm = static_cast<float>(1.0 / std::sqrt(sum_sq));
    for (float &value : *values) {
        value *= inv_norm;
    }
}

double DotProduct(const std::vector<float> &left, const std::vector<float> &right) {
    if (left.size() != right.size()) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        sum += static_cast<double>(left[i]) * static_cast<double>(right[i]);
    }
    return sum;
}

bool BuildPixelValues(const VideoFrame &frame, std::vector<float> *pixel_values, std::string *error) {
    if (frame.native == nullptr || pixel_values == nullptr) {
        if (error != nullptr) {
            *error = "missing decoded frame";
        }
        return false;
    }

    const int src_width = frame.native->width;
    const int src_height = frame.native->height;
    if (src_width <= 0 || src_height <= 0) {
        if (error != nullptr) {
            *error = "invalid frame dimensions";
        }
        return false;
    }

    SwsContext *sws_ctx = sws_getContext(
        src_width,
        src_height,
        static_cast<AVPixelFormat>(frame.native->format),
        src_width,
        src_height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );
    if (sws_ctx == nullptr) {
        if (error != nullptr) {
            *error = "failed to create rgb conversion context";
        }
        return false;
    }

    std::vector<uint8_t> rgb(static_cast<std::size_t>(src_width * src_height * 3));
    uint8_t *dst_slices[4] = {rgb.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {src_width * 3, 0, 0, 0};
    sws_scale(
        sws_ctx,
        frame.native->data,
        frame.native->linesize,
        0,
        src_height,
        dst_slices,
        dst_linesize
    );
    sws_freeContext(sws_ctx);

    const int shortest_edge = std::min(src_width, src_height);
    const float scale = static_cast<float>(kImageSize) / static_cast<float>(shortest_edge);
    const int resized_width = std::max(1, static_cast<int>(std::lround(src_width * scale)));
    const int resized_height = std::max(1, static_cast<int>(std::lround(src_height * scale)));
    const int crop_x = std::max(0, (resized_width - kImageSize) / 2);
    const int crop_y = std::max(0, (resized_height - kImageSize) / 2);

    std::vector<uint8_t> resized(static_cast<std::size_t>(resized_width * resized_height * 3));
    for (int y = 0; y < resized_height; ++y) {
        const int src_y = std::min(src_height - 1, static_cast<int>(y / scale));
        for (int x = 0; x < resized_width; ++x) {
            const int src_x = std::min(src_width - 1, static_cast<int>(x / scale));
            const std::size_t src_index =
                static_cast<std::size_t>((src_y * src_width + src_x) * 3);
            const std::size_t dst_index = static_cast<std::size_t>((y * resized_width + x) * 3);
            resized[dst_index + 0] = rgb[src_index + 0];
            resized[dst_index + 1] = rgb[src_index + 1];
            resized[dst_index + 2] = rgb[src_index + 2];
        }
    }

    pixel_values->assign(static_cast<std::size_t>(3 * kImageSize * kImageSize), 0.0f);
    for (int y = 0; y < kImageSize; ++y) {
        const int src_y = std::min(resized_height - 1, crop_y + y);
        for (int x = 0; x < kImageSize; ++x) {
            const int src_x = std::min(resized_width - 1, crop_x + x);
            const std::size_t src_index = static_cast<std::size_t>((src_y * resized_width + src_x) * 3);
            for (int c = 0; c < 3; ++c) {
                const std::size_t dst_index =
                    static_cast<std::size_t>(c * kImageSize * kImageSize + y * kImageSize + x);
                (*pixel_values)[dst_index] = static_cast<float>(resized[src_index + c]) / 255.0f;
            }
        }
    }

    return true;
}

std::vector<float> RunSession(
    Ort::Session *session,
    const char *input_name,
    const std::vector<int64_t> &input_shape,
    const std::vector<float> &input_data,
    const char *output_name,
    std::string *error
) {
    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        const_cast<float *>(input_data.data()),
        input_data.size(),
        input_shape.data(),
        input_shape.size()
    );

    const char *input_names[] = {input_name};
    const char *output_names[] = {output_name};
    auto outputs = session->Run(
        Ort::RunOptions{nullptr},
        input_names,
        &input_tensor,
        1,
        output_names,
        1
    );

    if (outputs.empty() || !outputs.front().IsTensor()) {
        if (error != nullptr) {
            *error = "onnx session returned no tensor output";
        }
        return {};
    }

    float *output_data = outputs.front().GetTensorMutableData<float>();
    const auto type_info = outputs.front().GetTensorTypeAndShapeInfo();
    const std::size_t element_count = type_info.GetElementCount();
    return std::vector<float>(output_data, output_data + element_count);
}

std::vector<float> RunTextSession(
    Ort::Session *session,
    const char *input_name,
    const std::vector<int64_t> &token_ids,
    const char *output_name,
    std::string *error
) {
    std::vector<int64_t> input_shape{1, static_cast<int64_t>(token_ids.size())};
    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(
        memory_info,
        const_cast<int64_t *>(token_ids.data()),
        token_ids.size(),
        input_shape.data(),
        input_shape.size()
    );

    const char *input_names[] = {input_name};
    const char *output_names[] = {output_name};
    auto outputs = session->Run(
        Ort::RunOptions{nullptr},
        input_names,
        &input_tensor,
        1,
        output_names,
        1
    );

    if (outputs.empty() || !outputs.front().IsTensor()) {
        if (error != nullptr) {
            *error = "text onnx session returned no tensor output";
        }
        return {};
    }

    float *output_data = outputs.front().GetTensorMutableData<float>();
    const auto type_info = outputs.front().GetTensorTypeAndShapeInfo();
    const std::size_t element_count = type_info.GetElementCount();
    return std::vector<float>(output_data, output_data + element_count);
}

} // namespace

struct MobileClip2S2Engine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "argus"};
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session, OrtDeleter> vision_session;
    std::unique_ptr<Ort::Session, OrtDeleter> text_session;
    ClipTokenizer tokenizer;
    std::vector<float> text_embedding;
    bool ready = false;
};

MobileClip2S2Engine::MobileClip2S2Engine() : impl_(std::make_unique<Impl>()) {
    impl_->session_options.SetIntraOpNumThreads(1);
    impl_->session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
}

MobileClip2S2Engine::~MobileClip2S2Engine() {
    Close();
}

bool MobileClip2S2Engine::Prepare(const std::string &model_root, const std::string &prompt, std::string *error) {
    Close();

    const std::string resolved_root = ResolveMobileClip2ModelRoot(model_root);
    const std::string vision_path = JoinPath(resolved_root, "vision_model.onnx");
    const std::string text_path = JoinPath(resolved_root, "text_model.onnx");
    const std::string vocab_path = JoinPath(resolved_root, "vocab.json");
    const std::string merges_path = JoinPath(resolved_root, "merges.txt");

    if (!FileSizeMatches(vision_path, kExpectedVisionModelBytes, error)) {
        return false;
    }
    if (!FileSizeMatches(text_path, kExpectedTextModelBytes, error)) {
        return false;
    }

    if (!impl_->tokenizer.Load(vocab_path, merges_path, error)) {
        return false;
    }

    try {
        impl_->vision_session = std::unique_ptr<Ort::Session, OrtDeleter>(
            new Ort::Session(impl_->env, vision_path.c_str(), impl_->session_options)
        );
        impl_->text_session = std::unique_ptr<Ort::Session, OrtDeleter>(
            new Ort::Session(impl_->env, text_path.c_str(), impl_->session_options)
        );
    } catch (const Ort::Exception &ex) {
        if (error != nullptr) {
            *error = std::string("failed to load onnx models: ") + ex.what();
        }
        Close();
        return false;
    }

    const std::vector<int64_t> token_ids = impl_->tokenizer.Encode(prompt);
    impl_->text_embedding = RunTextSession(
        impl_->text_session.get(),
        "input_ids",
        token_ids,
        "text_embeds",
        error
    );
    if (impl_->text_embedding.size() != kEmbedDim) {
        if (error != nullptr) {
            *error = "unexpected text embedding size";
        }
        Close();
        return false;
    }
    NormalizeInPlace(&impl_->text_embedding);
    impl_->ready = true;
    return true;
}

void MobileClip2S2Engine::Close() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->vision_session.reset();
    impl_->text_session.reset();
    impl_->text_embedding.clear();
    impl_->ready = false;
}

bool MobileClip2S2Engine::ScoreFrame(
    const VideoFrame &frame,
    MobileClipPromptScore *score,
    std::string *error
) const {
    if (score == nullptr) {
        return false;
    }
    if (impl_ == nullptr || !impl_->ready || impl_->vision_session == nullptr) {
        if (error != nullptr) {
            *error = "mobileclip engine is not prepared";
        }
        return false;
    }

    const auto started = std::chrono::steady_clock::now();

    std::vector<float> pixel_values;
    if (!BuildPixelValues(frame, &pixel_values, error)) {
        return false;
    }

    const std::vector<int64_t> input_shape{1, 3, kImageSize, kImageSize};
    std::vector<float> image_embedding = RunSession(
        impl_->vision_session.get(),
        "pixel_values",
        input_shape,
        pixel_values,
        "image_embeds",
        error
    );
    if (image_embedding.size() != kEmbedDim) {
        if (error != nullptr) {
            *error = "unexpected image embedding size";
        }
        return false;
    }
    NormalizeInPlace(&image_embedding);

    const auto finished = std::chrono::steady_clock::now();
    score->score = DotProduct(image_embedding, impl_->text_embedding);
    score->latency_ms = std::chrono::duration<double, std::milli>(finished - started).count();
    return true;
}

} // namespace irs3
