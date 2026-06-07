#!/usr/bin/env bash
# Install the companion GUI/TUI app-menu launchers for the current user.
# Idempotent: copies wrapper scripts to ~/.local/bin and .desktop files to
# ~/.local/share/applications, then refreshes the desktop database.
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin"
APPS="$HOME/.local/share/applications"
mkdir -p "$BIN" "$APPS"

install -m 0755 "$SELF/companion-gui" "$BIN/companion-gui"
install -m 0755 "$SELF/companion-tui" "$BIN/companion-tui"
install -m 0644 "$SELF/companion-gui.desktop" "$APPS/companion-gui.desktop"
install -m 0644 "$SELF/companion-tui.desktop" "$APPS/companion-tui.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" || true

echo "Installed:"
echo "  $BIN/companion-gui"
echo "  $BIN/companion-tui"
echo "  $APPS/companion-gui.desktop   -> 'Treecataje Companion (GUI)'"
echo "  $APPS/companion-tui.desktop   -> 'Treecataje Companion (TUI)'"
echo
echo "They should now appear in the application menu (Utility/System)."
echo "CLI: 'companion-gui' / 'companion-tui' (ensure ~/.local/bin is on PATH)."
