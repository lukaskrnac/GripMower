#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Desktop Shortcut Installer for GripMower ESP32 Flasher (Onboard Mini PC)
# One-line installation:
#   curl -fsSL https://raw.githubusercontent.com/lukaskrnac/GripMower/main/tools/install-flasher.sh | bash
# ==============================================================================

REPO="${GRIPMOWER_REPO:-lukaskrnac/GripMower}"
BRANCH="${GRIPMOWER_BRANCH:-main}"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"

FLASH_BIN="$BIN_DIR/gripmower-flash-esp32"
PAUSE_BIN="$BIN_DIR/gripmower-flash-esp32-pause"
DESKTOP_LAUNCHER="$BIN_DIR/gripmower-flash-esp32-desktop"

mkdir -p "$BIN_DIR" "$APP_DIR"

# If run from a local clone, copy local flash-esp32.sh; otherwise fetch from GitHub.
SCRIPT_DIR=""
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

if [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/flash-esp32.sh" ]; then
  echo "[INFO] Installing flasher from local file: $SCRIPT_DIR/flash-esp32.sh"
  cp "$SCRIPT_DIR/flash-esp32.sh" "$FLASH_BIN"
else
  URL="https://raw.githubusercontent.com/${REPO}/${BRANCH}/tools/flash-esp32.sh"
  echo "[INFO] Downloading flasher from ${URL}..."
  TMP_FILE="$(mktemp)"
  trap 'rm -f "$TMP_FILE"' EXIT
  curl -fsSL "$URL" -o "$TMP_FILE"
  bash -n "$TMP_FILE"
  mv "$TMP_FILE" "$FLASH_BIN"
fi
chmod +x "$FLASH_BIN"

# Zero-arg wrapper for terminal emulators (avoids differing -e quoting rules)
cat > "$PAUSE_BIN" <<EOF
#!/usr/bin/env bash
exec "${FLASH_BIN}" --pause
EOF
chmod +x "$PAUSE_BIN"

# Launcher that opens whichever graphical terminal emulator is installed
cat > "$DESKTOP_LAUNCHER" <<EOF
#!/usr/bin/env bash
set -euo pipefail

TARGET="${PAUSE_BIN}"
TITLE="GripMower — ESP32 Firmware Flash"

if command -v lxterminal >/dev/null 2>&1; then
  exec lxterminal -t "\$TITLE" -e "\$TARGET"
elif command -v xfce4-terminal >/dev/null 2>&1; then
  exec xfce4-terminal --title="\$TITLE" -e "\$TARGET"
elif command -v mate-terminal >/dev/null 2>&1; then
  exec mate-terminal --title="\$TITLE" -e "\$TARGET"
elif command -v gnome-terminal >/dev/null 2>&1; then
  exec gnome-terminal --title="\$TITLE" -- "\$TARGET"
elif command -v konsole >/dev/null 2>&1; then
  exec konsole -e "\$TARGET"
elif command -v x-terminal-emulator >/dev/null 2>&1; then
  exec x-terminal-emulator -e "\$TARGET"
elif command -v xterm >/dev/null 2>&1; then
  exec xterm -T "\$TITLE" -e "\$TARGET"
else
  exec "\$TARGET"
fi
EOF
chmod +x "$DESKTOP_LAUNCHER"

# Resolve localized Desktop directory (works for ~/Desktop, ~/Plocha, etc.)
DESKTOP_DIR=""
if command -v xdg-user-dir >/dev/null 2>&1; then
  DESKTOP_DIR="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
fi
if [ -z "$DESKTOP_DIR" ] || [ "$DESKTOP_DIR" = "$HOME" ]; then
  if [ -d "$HOME/Desktop" ]; then
    DESKTOP_DIR="$HOME/Desktop"
  elif [ -d "$HOME/Plocha" ]; then
    DESKTOP_DIR="$HOME/Plocha"
  else
    DESKTOP_DIR="$HOME/Desktop"
  fi
fi
mkdir -p "$DESKTOP_DIR"

write_desktop_entry() {
  local target="$1"
  cat > "$target" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=GripMower ESP32 Flash
Comment=Download latest GripMower ESP32 firmware from GitHub and flash over USB
Exec=${DESKTOP_LAUNCHER}
Icon=utilities-terminal
Terminal=false
Categories=System;Utility;
StartupNotify=true
EOF
  chmod +x "$target"
  if command -v gio >/dev/null 2>&1; then
    gio set "$target" metadata::trusted true 2>/dev/null || true
  fi
}

DESKTOP_FILE="$DESKTOP_DIR/GripMower-ESP32-Flash.desktop"
APP_FILE="$APP_DIR/gripmower-esp32-flash.desktop"

write_desktop_entry "$DESKTOP_FILE"
write_desktop_entry "$APP_FILE"

echo ""
echo "[ OK ] Installed:"
echo "  - Flasher CLI:      ${FLASH_BIN}"
echo "  - Desktop shortcut: ${DESKTOP_FILE}"
echo "  - App menu entry:   ${APP_FILE}"
echo ""
echo "Usage:"
echo "  1. Remote Desktop: double-click 'GripMower ESP32 Flash' on the Desktop."
echo "  2. Terminal / SSH: ${FLASH_BIN}"
if [[ ":$PATH:" != *":$BIN_DIR:"* ]]; then
  echo ""
  echo "[TIP] Add ~/.local/bin to PATH (e.g. in ~/.bashrc) to run 'gripmower-flash-esp32' directly."
fi
