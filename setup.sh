#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ANDROID_LOCAL_PROPERTIES="${ROOT_DIR}/android/local.properties"

ANDROID_SDK_DIR="${ANDROID_SDK_DIR:-}"
ESP_IDF_DIR="${ESP_IDF_DIR:-$HOME/esp/esp-idf}"
ESP_TARGET="${ESP_TARGET:-esp32c6}"
SKIP_APT=0

usage() {
  cat <<'EOF'
Usage: ./setup.sh [options]

Options:
  --skip-apt                 Skip apt package installation.
  --android-sdk-dir <path>   Force Android SDK path (default: auto-detect).
  --esp-idf-dir <path>       ESP-IDF install directory (default: $HOME/esp/esp-idf).
  --esp-target <target>      ESP-IDF target to install tools for (default: esp32c6).
  -h, --help                 Show this help text.

Environment variable equivalents:
  ANDROID_SDK_DIR, ESP_IDF_DIR, ESP_TARGET
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-apt)
      SKIP_APT=1
      shift
      ;;
    --android-sdk-dir)
      ANDROID_SDK_DIR="${2:-}"
      shift 2
      ;;
    --esp-idf-dir)
      ESP_IDF_DIR="${2:-}"
      shift 2
      ;;
    --esp-target)
      ESP_TARGET="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

ensure_command() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Missing required command: $cmd" >&2
    exit 1
  fi
}

install_apt_packages() {
  if [[ "$SKIP_APT" -eq 1 ]]; then
    echo "[setup] Skipping apt install (--skip-apt)."
    return
  fi

  ensure_command sudo
  ensure_command apt-get

  echo "[setup] Installing OS packages with apt..."
  sudo apt-get update
  sudo apt-get install -y \
    git curl unzip zip python3 python3-pip python3-venv \
    cmake ninja-build ccache libffi-dev libssl-dev \
    dfu-util libusb-1.0-0 openjdk-17-jdk adb android-sdk
}

detect_android_sdk_dir() {
  if [[ -n "${ANDROID_SDK_DIR}" && -d "${ANDROID_SDK_DIR}" ]]; then
    return
  fi

  local candidates=(
    "${ANDROID_SDK_DIR:-}"
    "${ANDROID_HOME:-}"
    "${ANDROID_SDK_ROOT:-}"
    "/usr/lib/android-sdk"
    "/usr/share/android-sdk"
    "$HOME/Android/Sdk"
  )

  local c
  for c in "${candidates[@]}"; do
    if [[ -n "${c}" && -d "${c}" ]]; then
      ANDROID_SDK_DIR="${c}"
      return
    fi
  done

  echo "Android SDK directory not found." >&2
  echo "Install via apt (android-sdk) or Android Studio, then re-run with --android-sdk-dir <path>." >&2
  exit 1
}

write_android_local_properties() {
  mkdir -p "${ROOT_DIR}/android"
  printf 'sdk.dir=%s\n' "${ANDROID_SDK_DIR}" > "${ANDROID_LOCAL_PROPERTIES}"
  echo "[setup] Wrote ${ANDROID_LOCAL_PROPERTIES}"
}

install_esp_idf() {
  ensure_command git
  ensure_command python3

  if [[ ! -d "${ESP_IDF_DIR}" ]]; then
    echo "[setup] Cloning ESP-IDF into ${ESP_IDF_DIR}..."
    mkdir -p "$(dirname "${ESP_IDF_DIR}")"
    git clone --recursive https://github.com/espressif/esp-idf.git "${ESP_IDF_DIR}"
  else
    echo "[setup] ESP-IDF already exists at ${ESP_IDF_DIR}"
  fi

  if [[ ! -x "${ESP_IDF_DIR}/install.sh" ]]; then
    echo "ESP-IDF install script not found at ${ESP_IDF_DIR}/install.sh" >&2
    exit 1
  fi

  echo "[setup] Installing ESP-IDF tools for target ${ESP_TARGET}..."
  "${ESP_IDF_DIR}/install.sh" "${ESP_TARGET}"
}

print_next_steps() {
  cat <<EOF

[setup] Complete.

1) In each new shell, load ESP-IDF:
   source "${ESP_IDF_DIR}/export.sh"

2) Build Android app:
   GRADLE_USER_HOME=/tmp/gradle-home ./android/gradlew -p android test
   GRADLE_USER_HOME=/tmp/gradle-home ./android/gradlew -p android assembleDebug

3) Flash ESP (${ESP_TARGET}):
   source "${ESP_IDF_DIR}/export.sh"
   idf.py set-target ${ESP_TARGET}
   idf.py build
   idf.py -p /dev/ttyACM0 flash monitor

Notes:
- This script uses apt package 'android-sdk' when not run with --skip-apt.
- If Android build still complains about missing SDK components, install extra platforms/build-tools
  using Android Studio SDK Manager or sdkmanager.
EOF
}

main() {
  echo "[setup] Repository root: ${ROOT_DIR}"
  install_apt_packages
  detect_android_sdk_dir
  echo "[setup] Using Android SDK directory: ${ANDROID_SDK_DIR}"
  write_android_local_properties
  install_esp_idf
  print_next_steps
}

main
