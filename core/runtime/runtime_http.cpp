#include "core/runtime/runtime_http.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace irs3 {

namespace {

constexpr size_t kMaxRequestBytes = 1024 * 1024;
constexpr size_t kMaxResponseBytes = 1024 * 1024;

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string trim(const std::string &value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end &&
           (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) {
        ++start;
    }
    while (end > start &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lowercase(const std::string &value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool read_full_request(int fd, HttpRequest *request) {
    std::string buffer;
    char chunk[4096];
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    while (buffer.size() < kMaxRequestBytes) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
        header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
    }

    if (header_end == std::string::npos) {
        return false;
    }

    std::string head = buffer.substr(0, header_end);
    std::istringstream stream(head);
    std::string line;

    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    {
        std::istringstream request_line(line);
        if (!(request_line >> request->method >> request->path)) {
            return false;
        }
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = lowercase(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        request->headers[key] = value;
        if (key == "content-length") {
            content_length = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
        }
    }

    request->body = buffer.substr(header_end + 4);
    while (request->body.size() < content_length && request->body.size() < kMaxRequestBytes) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        request->body.append(chunk, static_cast<size_t>(n));
    }
    return request->body.size() == content_length;
}

bool write_full(int fd, const std::string &payload) {
    size_t offset = 0;
    while (offset < payload.size()) {
        ssize_t n = send(fd, payload.data() + offset, payload.size() - offset, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool write_response(int fd, const HttpResponse &response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << response.reason << "\r\n";
    for (const auto &entry : response.headers) {
        out << entry.first << ": " << entry.second << "\r\n";
    }
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << response.body;
    const std::string payload = out.str();
    if (payload.size() > kMaxResponseBytes) {
        return false;
    }
    return write_full(fd, payload);
}

HttpResponse make_json_response(int status, const std::string &reason, const nlohmann::json &body) {
    HttpResponse response;
    response.status = status;
    response.reason = reason;
    response.body = body.dump();
    response.headers["Content-Type"] = "application/json";
    return response;
}

HttpResponse make_error_response(int status, const std::string &reason, const std::string &error) {
    nlohmann::json body;
    body["ok"] = false;
    body["error"] = error;
    return make_json_response(status, reason, body);
}

bool parse_mode(const nlohmann::json &value, irs3_hls_sink_output_mode *out) {
    if (!value.is_string() || out == nullptr) {
        return false;
    }
    const std::string mode = value.get<std::string>();
    if (mode == "record") {
        *out = IRS3_HLS_SINK_OUTPUT_RECORD;
        return true;
    }
    if (mode == "live") {
        *out = IRS3_HLS_SINK_OUTPUT_LIVE;
        return true;
    }
    return false;
}

bool parse_manifest_json(const std::string &body, RuntimeManifest *manifest, std::string *error) {
    if (manifest == nullptr || error == nullptr) {
        return false;
    }

    try {
        nlohmann::json root = nlohmann::json::parse(body);
        if (!root.is_object()) {
            *error = "manifest must be a JSON object";
            return false;
        }

        auto sources_it = root.find("sources");
        auto sinks_it = root.find("sinks");
        if (sources_it == root.end() || !sources_it->is_array()) {
            *error = "manifest.sources must be an array";
            return false;
        }
        if (sinks_it == root.end() || !sinks_it->is_array()) {
            *error = "manifest.sinks must be an array";
            return false;
        }
        if (root.contains("decoders") || root.contains("composes")) {
            *error = "manifest_version 2 does not support decoders or composes";
            return false;
        }

        RuntimeManifest parsed;
        parsed.manifest_version = kRuntimeManifestVersion;
        if (root.contains("manifest_version")) {
            if (!root["manifest_version"].is_number_integer()) {
                *error = "manifest_version must be an integer";
                return false;
            }
            parsed.manifest_version = root["manifest_version"].get<int>();
        }
        if (parsed.manifest_version != kRuntimeManifestVersion) {
            *error = "manifest_version must be 2";
            return false;
        }
        for (const auto &source_json : *sources_it) {
            if (!source_json.is_object() || !source_json.contains("source_id") ||
                !source_json["source_id"].is_string()) {
                *error = "each source must contain string source_id";
                return false;
            }
            parsed.sources.push_back(RuntimeSourceSpec{source_json["source_id"].get<std::string>()});
        }

        for (const auto &sink_json : *sinks_it) {
            if (!sink_json.is_object()) {
                *error = "each sink must be an object";
                return false;
            }

            if (!sink_json.contains("sink_id") || !sink_json["sink_id"].is_string()) {
                *error = "each sink must contain string sink_id";
                return false;
            }
            if (!sink_json.contains("output_root") || !sink_json["output_root"].is_string()) {
                *error = "each sink must contain string output_root";
                return false;
            }
            if (!sink_json.contains("inputs") || !sink_json["inputs"].is_array() || sink_json["inputs"].empty()) {
                *error = "each sink must contain non-empty inputs array";
                return false;
            }

            RuntimeSinkSpec sink;
            sink.sink_id = sink_json["sink_id"].get<std::string>();
            sink.output_root = sink_json["output_root"].get<std::string>();
            sink.output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
            if (sink_json.contains("mode") && !parse_mode(sink_json["mode"], &sink.output_mode)) {
                *error = "sink mode must be \"record\" or \"live\"";
                return false;
            }

            for (const auto &input_json : sink_json["inputs"]) {
                if (!input_json.is_object()) {
                    *error = "each sink input must be an object";
                    return false;
                }
                RuntimeSinkInputSpec input;
                input.kind = input_json.value("kind", "source");
                if (!input_json.contains("id") || !input_json["id"].is_string()) {
                    *error = "each sink input must contain string id";
                    return false;
                }
                if (!input_json.contains("stream_type") || !input_json["stream_type"].is_string()) {
                    *error = "each sink input must contain string stream_type";
                    return false;
                }
                if (!input_json.contains("packet_type") || !input_json["packet_type"].is_string()) {
                    *error = "each sink input must contain string packet_type";
                    return false;
                }
                if (!input_json.contains("stream_id") || !input_json["stream_id"].is_string()) {
                    *error = "each sink input must contain string stream_id";
                    return false;
                }
                input.id = input_json["id"].get<std::string>();
                input.stream_type = input_json["stream_type"].get<std::string>();
                input.packet_type = input_json["packet_type"].get<std::string>();
                input.stream_id = input_json["stream_id"].get<std::string>();
                sink.inputs.push_back(std::move(input));
            }
            parsed.sinks.push_back(std::move(sink));
        }

        *manifest = std::move(parsed);
        return true;
    } catch (const std::exception &ex) {
        *error = ex.what();
        return false;
    }
}

HttpResponse handle_request(
    const RuntimeHttpServer::ManifestHandler &handler,
    const HttpRequest &request
) {
    if (request.method == "OPTIONS" && request.path == "/upsert_manifest") {
        HttpResponse response;
        response.status = 204;
        response.reason = "No Content";
        response.headers["Allow"] = "OPTIONS, POST";
        response.headers["Access-Control-Allow-Methods"] = "OPTIONS, POST";
        response.headers["Access-Control-Allow-Headers"] = "Content-Type";
        return response;
    }

    if (request.path != "/upsert_manifest") {
        return make_error_response(404, "Not Found", "not found");
    }

    if (request.method != "POST") {
        return make_error_response(405, "Method Not Allowed", "method not allowed");
    }

    auto content_type_it = request.headers.find("content-type");
    if (content_type_it == request.headers.end() ||
        lowercase(trim(content_type_it->second)).find("application/json") != 0) {
        return make_error_response(400, "Bad Request", "invalid Content-Type");
    }

    RuntimeManifest manifest;
    std::string error;
    if (!parse_manifest_json(request.body, &manifest, &error)) {
        return make_error_response(400, "Bad Request", error);
    }

    if (!handler(manifest, &error)) {
        return make_error_response(400, "Bad Request", error);
    }

    nlohmann::json body;
    body["ok"] = true;
    return make_json_response(200, "OK", body);
}

} // namespace

RuntimeHttpServer::RuntimeHttpServer(RuntimeHttpConfig config, ManifestHandler handler)
    : config_(std::move(config)),
      handler_(std::move(handler)) {
}

RuntimeHttpServer::~RuntimeHttpServer() {
    Close();
}

bool RuntimeHttpServer::Start() {
    if (!config_.enabled) {
        return true;
    }

    int listener_fd = CreateListener();
    if (listener_fd < 0) {
        return false;
    }

    listener_fd_.store(listener_fd);
    thread_ = std::thread([this]() {
        Serve();
    });

    std::fprintf(
        stderr,
        "argus: runtime http listening on http://%s:%u\n",
        config_.bind_host.c_str(),
        static_cast<unsigned>(bound_port_.load())
    );
    return true;
}

void RuntimeHttpServer::Close() {
    if (!config_.enabled) {
        return;
    }
    if (closing_.exchange(true)) {
        return;
    }

    int listener_fd = listener_fd_.exchange(-1);
    if (listener_fd >= 0) {
        shutdown(listener_fd, SHUT_RDWR);
        close(listener_fd);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::uint16_t RuntimeHttpServer::BoundPort() const {
    return bound_port_.load();
}

void RuntimeHttpServer::Serve() {
    while (!closing_.load()) {
        int client_fd = accept(listener_fd_.load(), nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (closing_.load() || errno == EBADF || errno == EINVAL) {
                break;
            }
            std::perror("accept");
            break;
        }

        HttpRequest request;
        HttpResponse response;
        if (!read_full_request(client_fd, &request)) {
            response = make_error_response(400, "Bad Request", "failed to parse request");
        } else {
            response = handle_request(handler_, request);
        }
        (void)write_response(client_fd, response);
        close(client_fd);
    }
}

int RuntimeHttpServer::CreateListener() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::perror("setsockopt");
        close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.bind_host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "argus: invalid runtime bind host %s\n", config_.bind_host.c_str());
        close(fd);
        return -1;
    }

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        std::perror("listen");
        close(fd);
        return -1;
    }

    sockaddr_in bound_addr{};
    socklen_t bound_addr_len = sizeof(bound_addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&bound_addr), &bound_addr_len) == 0) {
        bound_port_.store(ntohs(bound_addr.sin_port));
    } else {
        bound_port_.store(config_.port);
    }

    return fd;
}

} // namespace irs3
