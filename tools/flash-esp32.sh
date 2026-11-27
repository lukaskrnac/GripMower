#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# GripMower ESP32 Firmware Flasher
# Downloads the latest ESP32 firmware from GitHub Releases (lukaskrnac/GripMower)
# and flashes it over USB. Automatically stops and restarts the mowgli-ros2
# Docker container to release the serial port safely.
# ==============================================================================

REPO="${GRIPMOWER_REPO:-lukaskrnac/GripMower}"
RELEASE_TAG="${GRIPMOWER_TAG:-firmware-latest}"
MOWGLI_CONTAINER="${GRIPMOWER_CONTAINER:-mowgli-ros2}"
BAUD="${GRIPMOWER_BAUD:-460800}"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/gripmower-flasher"
VENV_DIR="$DATA_DIR/venv"
STATE_FILE="$DATA_DIR/last_flashed.json"

PORT=""
LOCAL_BIN=""
MANAGE_DOCKER=true
ASSUME_YES=false
PAUSE_AT_END=false
SELF_UPDATE=false

TMP_DIR=""
STOPPED_CONTAINER=false
DOCKER_CMD=()
ESPTOOL_CMD=()

# Colors (only when connected to a terminal)
if [ -t 1 ]; then
  C_RESET='\033[0m'
  C_BOLD='\033[1m'
  C_RED='\033[0;31m'
  C_GREEN='\033[0;32m'
  C_YELLOW='\033[1;33m'
  C_CYAN='\033[0;36m'
  C_DIM='\033[2m'
else
  C_RESET='' C_BOLD='' C_RED='' C_GREEN='' C_YELLOW='' C_CYAN='' C_DIM=''
fi

info() { printf '%b[INFO]%b %s\n' "$C_CYAN" "$C_RESET" "$*"; }
ok()   { printf '%b[ OK ]%b %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn() { printf '%b[WARN]%b %s\n' "$C_YELLOW" "$C_RESET" "$*"; }
err()  { printf '%b[ERROR]%b %s\n' "$C_RED" "$C_RESET" "$*" >&2; }

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Downloads the latest ESP32 firmware from GitHub Releases (${REPO}) and flashes it over USB.

Options:
  -p, --port <path>    ESP32 serial port (e.g. /dev/mowgli or /dev/ttyUSB0)
  -t, --tag <tag>      GitHub Release tag (default: ${RELEASE_TAG})
  -b, --baud <rate>    Flashing baud rate (default: ${BAUD})
  -f, --file <file>    Flash a local merged .bin file instead of downloading
      --no-docker      Do not stop/restart the Docker container (${MOWGLI_CONTAINER})
  -y, --yes            Skip confirmation prompts
      --pause          Wait for Enter before exiting (used by Desktop shortcut)
      --self-update    Update this flasher script from GitHub
  -h, --help           Show this help message
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -p|--port)        PORT="${2:-}"; shift 2 ;;
    -t|--tag)         RELEASE_TAG="${2:-}"; shift 2 ;;
    -b|--baud)        BAUD="${2:-}"; shift 2 ;;
    -f|--file)        LOCAL_BIN="${2:-}"; shift 2 ;;
    --no-docker)      MANAGE_DOCKER=false; shift ;;
    -y|--yes)         ASSUME_YES=true; shift ;;
    --pause)          PAUSE_AT_END=true; shift ;;
    --self-update)    SELF_UPDATE=true; shift ;;
    -h|--help)        usage; exit 0 ;;
    *)                err "Unknown option: $1"; usage; exit 2 ;;
  esac
done

cleanup() {
  local exit_code=$?
  if [ "$STOPPED_CONTAINER" = true ] && [ "${#DOCKER_CMD[@]}" -gt 0 ]; then
    echo ""
    info "Waiting 2 s for ESP32 boot and starting container ${MOWGLI_CONTAINER} back up..."
    sleep 2
    if "${DOCKER_CMD[@]}" start "$MOWGLI_CONTAINER" >/dev/null 2>&1; then
      ok "Container ${MOWGLI_CONTAINER} is running again."
    else
      warn "Failed to restart container ${MOWGLI_CONTAINER}. Start it manually: docker start ${MOWGLI_CONTAINER}"
    fi
    STOPPED_CONTAINER=false
  fi

  if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
    rm -rf "$TMP_DIR"
  fi

  if [ "$PAUSE_AT_END" = true ]; then
    echo ""
    if [ "$exit_code" -eq 0 ]; then
      printf '%bDone.%b Press Enter to close this window... ' "$C_GREEN$C_BOLD" "$C_RESET"
    else
      printf '%bExited with error (code %d).%b Press Enter to close this window... ' "$C_RED$C_BOLD" "$exit_code" "$C_RESET"
    fi
    read -r _ || true
  fi
}
trap cleanup EXIT

do_self_update() {
  local self_path tmp_new url
  self_path="$(readlink -f "$0")"
  url="https://raw.githubusercontent.com/${REPO}/main/tools/flash-esp32.sh"
  tmp_new="$(mktemp)"
  info "Downloading latest script from ${url}..."
  if ! curl -fsSL "$url" -o "$tmp_new"; then
    rm -f "$tmp_new"
    err "Failed to download script update."
    exit 1
  fi
  if ! bash -n "$tmp_new"; then
    rm -f "$tmp_new"
    err "Downloaded script failed syntax validation."
    exit 1
  fi
  chmod +x "$tmp_new"
  mv "$tmp_new" "$self_path"
  ok "Script updated: ${self_path}"
  exit 0
}

if [ "$SELF_UPDATE" = true ]; then
  do_self_update
fi

# Check basic dependencies
for cmd in curl python3 sha256sum; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    err "Missing required tool: $cmd (install it e.g. via: sudo apt-get install -y $cmd)"
    exit 1
  fi
done

printf '%b============================================================%b\n' "$C_BOLD$C_CYAN" "$C_RESET"
printf '%b            GripMower — ESP32 Firmware Updater              %b\n' "$C_BOLD$C_CYAN" "$C_RESET"
printf '%b============================================================%b\n' "$C_BOLD$C_CYAN" "$C_RESET"
echo ""
warn "SAFETY: Make sure the mower is stationary, blades are stopped,"
warn "and nobody is standing near the mower (ideally press physical STOP)."
echo ""

mkdir -p "$DATA_DIR"
TMP_DIR="$(mktemp -d -t gripmower-esp32-XXXXXX)"

# ------------------------------------------------------------------------------
# 1. Fetch release metadata (or use local .bin)
# ------------------------------------------------------------------------------
RELEASE_BASE_URL="https://github.com/${REPO}/releases/download/${RELEASE_TAG}"
INFO_FILE="$TMP_DIR/firmware-info.json"
SUMS_FILE="$TMP_DIR/SHA256SUMS.txt"
BIN_FILE="$TMP_DIR/gripmower-esp32-merged.bin"

REMOTE_FW_VERSION="unknown"
REMOTE_PROTO="?"
REMOTE_COMMIT="?"
REMOTE_BUILT_AT="?"
REMOTE_SHA=""

json_get() {
  python3 - "$1" "$2" <<'PY'
import json, sys
try:
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    val = data.get(sys.argv[2], "")
    print("" if val is None else val)
except Exception:
    print("")
PY
}

if [ -n "$LOCAL_BIN" ]; then
  if [ ! -f "$LOCAL_BIN" ]; then
    err "Local file does not exist: $LOCAL_BIN"
    exit 1
  fi
  BIN_FILE="$LOCAL_BIN"
  REMOTE_FW_VERSION="local file ($(basename "$LOCAL_BIN"))"
  REMOTE_SHA="$(sha256sum "$LOCAL_BIN" | awk '{print $1}')"
  info "Using local firmware file: $LOCAL_BIN"
else
  info "Checking available firmware on GitHub (${REPO} @ ${RELEASE_TAG})..."
  if ! curl -fsSL "${RELEASE_BASE_URL}/firmware-info.json" -o "$INFO_FILE"; then
    err "Failed to download firmware-info.json from:"
    err "  ${RELEASE_BASE_URL}/firmware-info.json"
    err "Check your internet connection and verify that the GitHub Actions build has published a release."
    exit 1
  fi

  REMOTE_FW_VERSION="$(json_get "$INFO_FILE" fw_version)"
  REMOTE_PROTO="$(json_get "$INFO_FILE" protocol_version)"
  REMOTE_COMMIT="$(json_get "$INFO_FILE" commit_short)"
  REMOTE_BUILT_AT="$(json_get "$INFO_FILE" built_at_utc)"
  REMOTE_SHA="$(json_get "$INFO_FILE" merged_sha256)"

  printf '  %bAvailable version:%b v%s (protocol v%s, commit %s)\n' "$C_BOLD" "$C_RESET" "$REMOTE_FW_VERSION" "$REMOTE_PROTO" "$REMOTE_COMMIT"
  printf '  %bBuild date (UTC):%b  %s\n' "$C_BOLD" "$C_RESET" "$REMOTE_BUILT_AT"

  if [ -f "$STATE_FILE" ]; then
    PREV_FW="$(json_get "$STATE_FILE" fw_version)"
    PREV_COMMIT="$(json_get "$STATE_FILE" commit_short)"
    PREV_SHA="$(json_get "$STATE_FILE" merged_sha256)"
    printf '  %bLast flashed:%b      v%s (commit %s)\n' "$C_BOLD" "$C_RESET" "${PREV_FW:-?}" "${PREV_COMMIT:-?}"
    if [ -n "$REMOTE_SHA" ] && [ "$REMOTE_SHA" = "$PREV_SHA" ]; then
      ok "According to local records, this exact build (${REMOTE_COMMIT}) is already flashed."
    fi
  fi
  echo ""
fi

# ------------------------------------------------------------------------------
# 2. Auto-detect ESP32 USB serial port
# ------------------------------------------------------------------------------
describe_port() {
  local dev="$1"
  local real_dev vid pid model desc tag=""
  real_dev="$(readlink -f "$dev" 2>/dev/null || echo "$dev")"
  vid="" pid="" model=""
  if command -v udevadm >/dev/null 2>&1; then
    while IFS='=' read -r k v; do
      case "$k" in
        ID_VENDOR_ID) vid="$v" ;;
        ID_MODEL_ID)  pid="$v" ;;
        ID_MODEL_FROM_DATABASE|ID_MODEL) [ -z "$model" ] && model="$v" ;;
      esac
    done < <(udevadm info --query=property --name="$real_dev" 2>/dev/null || true)
  fi
  desc="${model:-USB serial device}"
  [ -n "$vid" ] && [ -n "$pid" ] && desc="${desc} [${vid}:${pid}]"

  if [ -n "$GPS_REAL" ] && [ "$real_dev" = "$GPS_REAL" ]; then
    tag=" (GPS - do not select)"
  elif [ -n "$LIDAR_REAL" ] && [ "$real_dev" = "$LIDAR_REAL" ]; then
    tag=" (LiDAR - do not select)"
  elif [ -n "$MOWGLI_REAL" ] && [ "$real_dev" = "$MOWGLI_REAL" ]; then
    tag=" (Mowgli /dev/mowgli)"
  fi

  if [ "$dev" != "$real_dev" ]; then
    printf '%s -> %s  —  %s%s' "$dev" "$real_dev" "$desc" "$tag"
  else
    printf '%s  —  %s%s' "$dev" "$desc" "$tag"
  fi
}

is_likely_esp32_port() {
  local real_dev="$1"
  local vid=""
  if command -v udevadm >/dev/null 2>&1; then
    vid="$(udevadm info --query=property --name="$real_dev" 2>/dev/null | awk -F= '$1=="ID_VENDOR_ID"{print tolower($2); exit}')"
  fi
  case "$vid" in
    10c4|1a86|303a|0403) return 0 ;; # Silicon Labs CP210x, WCH CH340/9102, Espressif, FTDI
    *) return 1 ;;
  esac
}

GPS_REAL=""
LIDAR_REAL=""
MOWGLI_REAL=""
[ -e /dev/gps ]    && GPS_REAL="$(readlink -f /dev/gps 2>/dev/null || true)"
[ -e /dev/lidar ]  && LIDAR_REAL="$(readlink -f /dev/lidar 2>/dev/null || true)"
[ -e /dev/mowgli ] && MOWGLI_REAL="$(readlink -f /dev/mowgli 2>/dev/null || true)"

if [ -z "$PORT" ]; then
  ALL_PORTS=()
  ESP_CANDIDATES=()

  if [ -e /dev/mowgli ]; then
    ALL_PORTS+=("/dev/mowgli")
  fi

  for dev in /dev/ttyUSB* /dev/ttyACM*; do
    [ -c "$dev" ] || continue
    real_dev="$(readlink -f "$dev" 2>/dev/null || echo "$dev")"
    # Skip duplicate if already added as /dev/mowgli
    if [ -n "$MOWGLI_REAL" ] && [ "$real_dev" = "$MOWGLI_REAL" ]; then
      continue
    fi
    ALL_PORTS+=("$dev")
    if [ "$real_dev" != "$GPS_REAL" ] && [ "$real_dev" != "$LIDAR_REAL" ] && is_likely_esp32_port "$real_dev"; then
      ESP_CANDIDATES+=("$dev")
    fi
  done

  if [ "${#ALL_PORTS[@]}" -eq 0 ]; then
    err "No USB serial port found (/dev/mowgli, /dev/ttyUSB*, /dev/ttyACM*)."
    err "Check that the ESP32 is connected via USB to the Mini PC."
    exit 1
  fi

  DEFAULT_IDX=""
  if [ -e /dev/mowgli ]; then
    DEFAULT_IDX=1
  elif [ "${#ESP_CANDIDATES[@]}" -eq 1 ]; then
    for i in "${!ALL_PORTS[@]}"; do
      if [ "${ALL_PORTS[$i]}" = "${ESP_CANDIDATES[0]}" ]; then
        DEFAULT_IDX=$((i + 1))
        break
      fi
    done
  elif [ "${#ALL_PORTS[@]}" -eq 1 ]; then
    DEFAULT_IDX=1
  fi

  if [ "$ASSUME_YES" = true ] && [ -n "$DEFAULT_IDX" ]; then
    PORT="${ALL_PORTS[$((DEFAULT_IDX - 1))]}"
  elif [ "${#ALL_PORTS[@]}" -eq 1 ]; then
    PORT="${ALL_PORTS[0]}"
    info "Detected port: $(describe_port "$PORT")"
  else
    info "Detected serial ports:"
    for i in "${!ALL_PORTS[@]}"; do
      idx=$((i + 1))
      mark=" "
      [ -n "$DEFAULT_IDX" ] && [ "$idx" -eq "$DEFAULT_IDX" ] && mark="*"
      printf '  %s [%d] %s\n' "$mark" "$idx" "$(describe_port "${ALL_PORTS[$i]}")"
    done
    echo ""
    if [ -n "$DEFAULT_IDX" ]; then
      printf 'Select port number [Enter = %d (%s)]: ' "$DEFAULT_IDX" "${ALL_PORTS[$((DEFAULT_IDX - 1))]}"
      read -r choice
      choice="${choice:-$DEFAULT_IDX}"
    else
      printf 'Select port number (1-%d): ' "${#ALL_PORTS[@]}"
      read -r choice
    fi
    if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt "${#ALL_PORTS[@]}" ]; then
      err "Invalid port selection."
      exit 1
    fi
    PORT="${ALL_PORTS[$((choice - 1))]}"
  fi
fi

if [ ! -e "$PORT" ]; then
  err "Port does not exist: $PORT"
  exit 1
fi

PORT_REAL="$(readlink -f "$PORT" 2>/dev/null || echo "$PORT")"

# ------------------------------------------------------------------------------
# 3. Check mowgli-ros2 Docker container status (do not stop yet)
# ------------------------------------------------------------------------------
CONTAINER_RUNNING=false
if [ "$MANAGE_DOCKER" = true ] && command -v docker >/dev/null 2>&1; then
  if docker info >/dev/null 2>&1; then
    DOCKER_CMD=(docker)
  elif sudo -n docker info >/dev/null 2>&1 || sudo docker info >/dev/null 2>&1; then
    DOCKER_CMD=(sudo docker)
  fi

  if [ "${#DOCKER_CMD[@]}" -gt 0 ]; then
    if "${DOCKER_CMD[@]}" ps --format '{{.Names}}' 2>/dev/null | grep -qx "$MOWGLI_CONTAINER"; then
      CONTAINER_RUNNING=true
    fi
  fi
fi

# ------------------------------------------------------------------------------
# 4. Confirm before flashing
# ------------------------------------------------------------------------------
echo ""
printf '%bFlashing summary:%b\n' "$C_BOLD" "$C_RESET"
printf '  ESP32 port: %s\n' "$(describe_port "$PORT")"
printf '  Firmware:   %s (commit %s)\n' "$REMOTE_FW_VERSION" "$REMOTE_COMMIT"
printf '  Baud rate:  %s\n' "$BAUD"
if [ "$CONTAINER_RUNNING" = true ]; then
  printf '  Docker:     will temporarily stop %s and restart it afterwards\n' "$MOWGLI_CONTAINER"
fi
echo ""

if [ "$ASSUME_YES" != true ]; then
  printf 'Proceed with flashing ESP32? [Y/n]: '
  read -r answer
  case "${answer:-y}" in
    y|Y|yes|YES|a|A) ;;
    *) info "Flashing cancelled by user."; exit 0 ;;
  esac
fi

# ------------------------------------------------------------------------------
# 5. Download merged binary and verify SHA-256
# ------------------------------------------------------------------------------
if [ -z "$LOCAL_BIN" ]; then
  echo ""
  info "Downloading gripmower-esp32-merged.bin..."
  if ! curl -fL --progress-bar "${RELEASE_BASE_URL}/gripmower-esp32-merged.bin" -o "$BIN_FILE"; then
    err "Failed to download gripmower-esp32-merged.bin."
    exit 1
  fi

  info "Verifying SHA-256 checksum..."
  if curl -fsSL "${RELEASE_BASE_URL}/SHA256SUMS.txt" -o "$SUMS_FILE"; then
    EXPECTED_SHA="$(awk '$2 == "gripmower-esp32-merged.bin" {print $1; exit}' "$SUMS_FILE")"
  else
    EXPECTED_SHA="$REMOTE_SHA"
  fi
  ACTUAL_SHA="$(sha256sum "$BIN_FILE" | awk '{print $1}')"
  if [ -z "$EXPECTED_SHA" ] || [ "$ACTUAL_SHA" != "$EXPECTED_SHA" ]; then
    err "SHA-256 checksum mismatch! Aborting flash."
    err "  Expected: ${EXPECTED_SHA:-missing}"
    err "  Actual:   ${ACTUAL_SHA}"
    exit 1
  fi
  ok "SHA-256 verified (${ACTUAL_SHA:0:16}...)."
fi

# ------------------------------------------------------------------------------
# 6. Ensure esptool is installed
# ------------------------------------------------------------------------------
detect_esptool() {
  if [ -x "$VENV_DIR/bin/esptool" ]; then
    ESPTOOL_CMD=("$VENV_DIR/bin/esptool")
    return 0
  elif [ -x "$VENV_DIR/bin/esptool.py" ]; then
    ESPTOOL_CMD=("$VENV_DIR/bin/esptool.py")
    return 0
  elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL_CMD=("$(command -v esptool)")
    return 0
  elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL_CMD=("$(command -v esptool.py)")
    return 0
  elif python3 -c "import esptool" >/dev/null 2>&1; then
    ESPTOOL_CMD=(python3 -m esptool)
    return 0
  fi
  return 1
}

if ! detect_esptool; then
  info "esptool not found — installing into isolated Python venv (${VENV_DIR})..."
  if ! python3 -m venv "$VENV_DIR" >/dev/null 2>&1; then
    warn "python3-venv missing, attempting to install via apt-get..."
    sudo apt-get update -qq
    if sudo apt-get install -y -qq python3-venv >/dev/null 2>&1; then
      python3 -m venv "$VENV_DIR"
    else
      warn "Falling back to system esptool apt package..."
      sudo apt-get install -y -qq esptool
    fi
  fi

  if [ -x "$VENV_DIR/bin/pip" ]; then
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip
    "$VENV_DIR/bin/pip" install --quiet esptool
  fi

  if ! detect_esptool; then
    err "Failed to install esptool. Install manually: sudo apt-get install -y esptool"
    exit 1
  fi
  ok "esptool ready: ${ESPTOOL_CMD[*]}"
fi

# Detect esptool v5 (hyphenated flags) vs v3/v4 (underscored flags)
if "${ESPTOOL_CMD[@]}" --help 2>&1 | grep -q 'write-flash'; then
  ESP_WRITE_CMD="write-flash"
  ESP_BEFORE="default-reset"
  ESP_AFTER="hard-reset"
  ESP_FLASH_MODE_FLAG="--flash-mode"
  ESP_FLASH_FREQ_FLAG="--flash-freq"
  ESP_FLASH_SIZE_FLAG="--flash-size"
else
  ESP_WRITE_CMD="write_flash"
  ESP_BEFORE="default_reset"
  ESP_AFTER="hard_reset"
  ESP_FLASH_MODE_FLAG="--flash_mode"
  ESP_FLASH_FREQ_FLAG="--flash_freq"
  ESP_FLASH_SIZE_FLAG="--flash_size"
fi

# ------------------------------------------------------------------------------
# 7. Stop mowgli-ros2 (release serial port)
# ------------------------------------------------------------------------------
if [ "$CONTAINER_RUNNING" = true ]; then
  echo ""
  info "Stopping container ${MOWGLI_CONTAINER} to release serial port..."
  "${DOCKER_CMD[@]}" stop "$MOWGLI_CONTAINER" >/dev/null
  STOPPED_CONTAINER=true
  sleep 1
  ok "Container ${MOWGLI_CONTAINER} stopped."
fi

if command -v fuser >/dev/null 2>&1 && fuser "$PORT_REAL" >/dev/null 2>&1; then
  warn "Port ${PORT_REAL} is still held open by another process:"
  fuser -v "$PORT_REAL" || true
fi

# ------------------------------------------------------------------------------
# 8. Flash ESP32 over USB
# ------------------------------------------------------------------------------
run_esptool() {
  local rate="$1"
  local prefix=()
  if [ ! -r "$PORT_REAL" ] || [ ! -w "$PORT_REAL" ]; then
    warn "No direct read/write permission on ${PORT_REAL} — running esptool with sudo."
    prefix=(sudo)
  fi
  "${prefix[@]}" "${ESPTOOL_CMD[@]}" \
    --chip esp32 \
    --port "$PORT_REAL" \
    --baud "$rate" \
    --before "$ESP_BEFORE" \
    --after "$ESP_AFTER" \
    "$ESP_WRITE_CMD" \
    -z \
    "$ESP_FLASH_MODE_FLAG" keep \
    "$ESP_FLASH_FREQ_FLAG" keep \
    "$ESP_FLASH_SIZE_FLAG" detect \
    0x0 "$BIN_FILE"
}

echo ""
info "Flashing firmware to ESP32 on ${PORT_REAL} (${BAUD} baud)..."
info "If it stalls on 'Connecting...', hold the BOOT button on the ESP32."
echo ""

if ! run_esptool "$BAUD"; then
  if [ "$BAUD" != "115200" ]; then
    echo ""
    warn "Flashing at ${BAUD} baud failed — retrying at a slower 115200 baud..."
    echo ""
    if ! run_esptool 115200; then
      err "Flashing ESP32 failed even at 115200 baud."
      err "Check the USB cable, make sure no other process holds the port, or hold the BOOT button on the ESP32."
      exit 1
    fi
  else
    err "Flashing ESP32 failed."
    exit 1
  fi
fi

# Save record of last flashed version
if [ -f "$INFO_FILE" ]; then
  cp "$INFO_FILE" "$STATE_FILE"
fi

echo ""
ok "Firmware successfully flashed to ESP32!"
