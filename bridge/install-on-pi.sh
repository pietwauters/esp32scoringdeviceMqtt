#!/bin/bash
# Run this on the Raspberry Pi to clone, install, and enable the bridge service.

set -e

REPO_DIR="$HOME/esp32scoringdeviceMqtt"
BRIDGE_DIR="$REPO_DIR/bridge"
SERVICE_NAME="openpiste-bridge"

# ── Clone ─────────────────────────────────────────────────────────────────────

git clone --filter=blob:none --sparse \
  https://github.com/pietwauters/esp32scoringdeviceMqtt.git "$REPO_DIR"
cd "$REPO_DIR"
git sparse-checkout set bridge
git checkout opp2-canonical-state

# ── Install dependencies ──────────────────────────────────────────────────────

cd "$BRIDGE_DIR"
npm install

# ── Config ────────────────────────────────────────────────────────────────────

if [ ! -f config.json ]; then
  cp config.example.json config.json
  echo ""
  echo "config.json created. Edit it before starting the bridge:"
  echo "  nano $BRIDGE_DIR/config.json"
  echo ""
else
  echo "config.json already exists — not overwritten."
fi

# ── Systemd service ───────────────────────────────────────────────────────────

# Patch WorkingDirectory in the service file to match the actual install path
sed "s|WorkingDirectory=.*|WorkingDirectory=$BRIDGE_DIR|" \
  "$BRIDGE_DIR/openpiste-bridge.service" \
  > /tmp/${SERVICE_NAME}.service

sudo cp /tmp/${SERVICE_NAME}.service /etc/systemd/system/${SERVICE_NAME}.service
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"

echo ""
echo "Service installed and enabled."
echo "After editing config.json, start it with:"
echo "  sudo systemctl start $SERVICE_NAME"
echo ""
echo "Check status with:"
echo "  sudo systemctl status $SERVICE_NAME"
echo "  journalctl -u $SERVICE_NAME -f"
