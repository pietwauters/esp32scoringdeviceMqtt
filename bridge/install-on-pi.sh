#!/bin/bash
# Run this on the Raspberry Pi to clone and install the bridge.

set -e

git clone --filter=blob:none --sparse \
  https://github.com/pietwauters/esp32scoringdeviceMqtt.git
cd esp32scoringdeviceMqtt
git sparse-checkout set bridge
git checkout opp2-canonical-state
cd bridge

npm install

if [ ! -f config.json ]; then
  cp config.example.json config.json
  echo ""
  echo "config.json created from example. Edit it before starting the bridge:"
  echo "  nano config.json"
  echo ""
else
  echo "config.json already exists — not overwritten."
fi

echo "Done. Start the bridge with: node bridge.js"
