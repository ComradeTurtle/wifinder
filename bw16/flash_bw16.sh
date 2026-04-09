#!/usr/bin/env bash
set -euo pipefail

FQBN="${FQBN:-realtek:AmebaD:Ai-Thinker_BW16}"
PORT="${PORT:-/dev/ttyUSB0}"
SKETCH_DIR="${SKETCH_DIR:-/tmp/bw16_node}"
SKETCH_NAME="${SKETCH_NAME:-bw16_node}"
BUILD_DIR="${BUILD_DIR:-/tmp/bw16_build}"
BUILD_EXTRA_FLAGS="${BUILD_EXTRA_FLAGS:-}"
BUILD_BIN_NAME="${BUILD_BIN_NAME:-km0_km4_image2.bin}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--port /dev/ttyUSB0] [--fqbn FQBN] [--sketch-dir DIR]

Environment overrides:
  PORT        Serial port (default: /dev/ttyUSB0)
  FQBN        Board FQBN (default: realtek:AmebaD:Ai-Thinker_BW16)
  SKETCH_DIR  Temporary Arduino sketch directory (default: /tmp/bw16_node)
  SKETCH_NAME Sketch file basename without extension (default: bw16_node)
  BUILD_DIR   Compile output directory (default: /tmp/bw16_build)
  BUILD_BIN_NAME
              Firmware filename inside BUILD_DIR (default: km0_km4_image2.bin)
  BUILD_EXTRA_FLAGS
              Optional build.extra_flags override (default: unset).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port)
      PORT="$2"
      shift 2
      ;;
    -f|--fqbn)
      FQBN="$2"
      shift 2
      ;;
    -d|--sketch-dir)
      SKETCH_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found in PATH." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_MAIN="${SCRIPT_DIR}/src/main.cpp"
TARGET_SKETCH="${SKETCH_DIR}/${SKETCH_NAME}.ino"

if [[ ! -f "$SOURCE_MAIN" ]]; then
  echo "Source file not found: $SOURCE_MAIN" >&2
  exit 1
fi

mkdir -p "$SKETCH_DIR"
cp "$SOURCE_MAIN" "$TARGET_SKETCH"
mkdir -p "$BUILD_DIR"

echo "Compiling (${FQBN})..."
if [[ -n "$BUILD_EXTRA_FLAGS" ]]; then
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    --build-property "build.extra_flags=${BUILD_EXTRA_FLAGS}" \
    "$SKETCH_DIR"
else
  arduino-cli compile \
    --fqbn "$FQBN" \
    --build-path "$BUILD_DIR" \
    "$SKETCH_DIR"
fi

BUILD_BIN_PATH="${BUILD_DIR}/${BUILD_BIN_NAME}"
if [[ ! -f "$BUILD_BIN_PATH" ]]; then
  echo "Build artifact not found: $BUILD_BIN_PATH" >&2
  exit 1
fi

echo "Uploading to ${PORT} (${FQBN})..."
arduino-cli upload \
  -p "$PORT" \
  --fqbn "$FQBN" \
  -i "$BUILD_BIN_PATH" \
  "$SKETCH_DIR"

echo "Flash complete."
