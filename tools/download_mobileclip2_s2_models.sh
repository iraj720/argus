#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL_DIR="${1:-${ROOT_DIR}/models/mobileclip2_s2}"
BASE_URL="https://huggingface.co"

VISION_URL="${BASE_URL}/plhery/mobileclip2-onnx/resolve/main/onnx/s2/vision_model.onnx"
TEXT_URL="${BASE_URL}/plhery/mobileclip2-onnx/resolve/main/onnx/s2/text_model.onnx"
VOCAB_URL="${BASE_URL}/openai/clip-vit-base-patch32/resolve/main/vocab.json"
MERGES_URL="${BASE_URL}/openai/clip-vit-base-patch32/resolve/main/merges.txt"

VISION_BYTES=143044797
TEXT_BYTES=254053669

mkdir -p "${MODEL_DIR}"

file_size() {
    wc -c < "$1" | tr -d ' '
}

download_if_needed() {
    local url="$1"
    local dest="$2"
    local expected_bytes="$3"

    if [[ -f "${dest}" ]]; then
        local actual_bytes
        actual_bytes="$(file_size "${dest}")"
        if [[ "${actual_bytes}" == "${expected_bytes}" ]]; then
            echo "ok: ${dest} (${actual_bytes} bytes)"
            return 0
        fi
        echo "re-downloading truncated file: ${dest} (${actual_bytes}/${expected_bytes} bytes)"
        rm -f "${dest}"
    else
        echo "downloading: ${url}"
    fi

    curl -L --fail --retry 5 --continue-at - --output "${dest}" "${url}"

    local final_bytes
    final_bytes="$(file_size "${dest}")"
    if [[ "${final_bytes}" != "${expected_bytes}" ]]; then
        echo "error: ${dest} size mismatch (${final_bytes}/${expected_bytes} bytes)" >&2
        exit 1
    fi
    echo "ok: ${dest} (${final_bytes} bytes)"
}

download_small() {
    local url="$1"
    local dest="$2"
    if [[ -f "${dest}" ]]; then
        echo "exists: ${dest}"
        return 0
    fi
    echo "downloading: ${url}"
    curl -L --fail --retry 3 --output "${dest}" "${url}"
}

download_if_needed "${VISION_URL}" "${MODEL_DIR}/vision_model.onnx" "${VISION_BYTES}"
download_if_needed "${TEXT_URL}" "${MODEL_DIR}/text_model.onnx" "${TEXT_BYTES}"
download_small "${VOCAB_URL}" "${MODEL_DIR}/vocab.json"
download_small "${MERGES_URL}" "${MODEL_DIR}/merges.txt"

echo "MobileCLIP2-S2 assets ready in ${MODEL_DIR}"
