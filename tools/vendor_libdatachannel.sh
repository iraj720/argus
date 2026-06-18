#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${ROOT_DIR}/third_party/libdatachannel"
SOURCE_DIR_DEFAULT="${ROOT_DIR}/../libdatachannel"
SOURCE_DIR="${1:-${SOURCE_DIR_DEFAULT}}"

if [[ ! -d "${SOURCE_DIR}" ]]; then
    echo "missing source libdatachannel dir: ${SOURCE_DIR}" >&2
    exit 1
fi

mkdir -p "${ROOT_DIR}/third_party"
rm -rf "${DEST_DIR}"
cp -R "${SOURCE_DIR}" "${DEST_DIR}"

echo "vendored libdatachannel into: ${DEST_DIR}"
