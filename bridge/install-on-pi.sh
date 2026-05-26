#!/bin/bash
# Run this on the Raspberry Pi to clone and install the bridge.

git clone --filter=blob:none --sparse \
  https://github.com/pietwauters/esp32scoringdeviceMqtt.git
cd esp32scoringdeviceMqtt
git sparse-checkout set bridge
git checkout opp2-canonical-state
cd bridge
npm install
