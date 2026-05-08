# OpenPiste Scoring Device

**Part of the [OpenPiste](https://github.com/OpenPiste) open source fencing electronics and IT platform**

A complete, full-featured fencing scoring device — everything a club needs, built for less than €60, no proprietary remote control required.

---

## A real scoring device, not a simplified approximation

Commercial fencing scoring devices are expensive. The OpenPiste scoring device delivers the same functionality at a fraction of the cost:

| Feature | OpenPiste |
|---------|-----------|
| Score display | ✓ |
| Timer | ✓ |
| Lights (on-target / off-target) | ✓ |
| Penalty cards | ✓ |
| Period management | ✓ |
| Priority (drawing and sudden death) | ✓ |
| All three weapons (foil, épée, sabre) | ✓ |
| Wifi connectivity with Cyrano | ✓ |
| Wireless repeaters (esp-now) | ✓ |

**Build cost: under €60.** The remote control is a free Android app — no dedicated hardware to buy.

---

## Standout features

### Passivity tracking
A feature found on very few commercial devices at any price point. While the timer is running, the device tracks time elapsed without a touch:
- Every 10 seconds without a hit, an additional blue LED illuminates — giving the referee a clear, at-a-glance indication of passivity building up
- At one minute without a touch, a red LED signals that P-cards are due

Passivity tracking is active in all modes. In AutoRef mode (see below) it is fully automated — an especially useful feature for training sessions.

### Automatic weapon detection
The device detects the current weapon automatically. When both fencers make a simultaneous long hit, the device switches to the correct weapon mode without any manual selection. No menus, no buttons, no risk of scoring a bout under the wrong weapon rules.

### AutoRef — fully automatic refereeing
AutoRef mode runs a complete bout from start to finish with no operator input beyond the two fencers. It combines:

- **Rules-aware scoring** — in épée, double hits immediately award a point to both fencers; in foil and sabre, the device resolves right-of-way from the next single valid hit, with no button press or remote required
- **Automated passivity** — bout automatically stopped and P-cards issued at the one-minute threshold
- **Full bout lifecycle** — timer start/stop, 1-minute break between periods, random priority drawing at end of regulation time if scores are level, sudden-death extra period

AutoRef is particularly valuable for **club training**: two fencers can run a complete, properly refereed bout with no official present.

---

## Works with your existing setup

The scoring device speaks the industry-standard protocols used by professional fencing equipment:

| Protocol | Purpose |
|----------|---------|
| **Cyrano (UDP)** | Competition management — compatible with FencingTime, Engarde, FencingFox and others |
| **FPA (UDP)** | Real-time data — Android remote, CYD hardware remote, video refereeing via the free [SFS iOS app](https://superfencingsystem.com/) |
| **MQTT / JSON** | Modern open layer — piste monitor, and the foundation for future ecosystem expansion |

Drop it into your existing setup and it works. No changes to your competition software, no migration required.

---

## The OpenPiste ecosystem

The scoring device is the heart of a broader platform:

```
Scoring device
      │
      ├─── Cyrano (UDP) ──► FencingTime / Engarde / FencingFox
      │
      ├─── FPA (UDP) ─────► SFS video referee app (iOS)
      │                 ├─► Android remote control app
      │                 └─► CYD hardware remote
      │
      ├─── ESP-NOW ───────► Wireless repeater displays (no WiFi required)
      │
      └─── MQTT ──────────► Raspberry Pi hub
                                  └─── Browser-based piste monitor
                                       (single piste or multi-piste overview,
                                        with fencer photos)
```

| Repo | Description |
|------|-------------|
| [ImprovedTesterAfterGenova](https://github.com/pietwauters/ImprovedTesterAfterGenova) | Weapon and wire tester |
| [remotecontrolapp](https://github.com/pietwauters/remotecontrolapp) | Android remote control (Kotlin) |
| [CYDRemoteControl](https://github.com/pietwauters/CYDRemoteControl) | Hardware remote (ESP32 + touchscreen) |
| [CyranoPisteMonitor](https://github.com/pietwauters/CyranoPisteMonitor) | Browser-based piste monitor |
| [esp32_scoring_device_hardware](https://github.com/pietwauters/esp32_scoring_device_hardware) | PCB schematics and hardware designs |

> **Legacy repo:** The original scoring device is at [esp32-scoring-device](https://github.com/pietwauters/esp32-scoring-device) (15 ★). Kept for its wiki, documentation, and photos. This repo is the active successor.

---

## Hardware

The scoring device is built around a custom PCB (rev 1.2b — the same PCB is shared with the weapon tester). Design files are in the [esp32_scoring_device_hardware](https://github.com/pietwauters/esp32_scoring_device_hardware) repo.

### Bill of materials

> Items marked \* are optional. JST-XH 2.54mm connectors are recommended — keyed, so they can't be connected the wrong way. Header pins work too. Connectors are preferred over soldering directly for serviceability.

| Qty | Description | Value / Notes |
|-----|-------------|---------------|
| 1 | Custom PCB rev 1.2b | Alternatively: breadboard or perfboard |
| 1 | ESP32 DevKit V1 | **30-pin version only** (2×15). The 38-pin V4 will not fit |
| 2 | WS2812B 64-LED matrix panel | 65×65mm |
| 1* | MAX7219 4-in-1 dot matrix display module | Optional |
| 2 | Electrolytic capacitor 470µF 35V | Ø10mm, 5mm pitch |
| 2 | Polyester film capacitor 0.1µF | |
| 5 | Metal film resistor 470Ω 1% | 1/4W |
| 2 | Metal film resistor 33Ω 1% | 1/4W |
| 1 | Metal film resistor 3.3kΩ 1% | 1/4W |
| 1 | Metal film resistor 2.2kΩ 1% | 1/4W |
| 1 | Metal film resistor 150Ω 1% | 1/4W |
| 1 | Metal film resistor 10kΩ 1% | 1/4W |
| 1* | Metal film resistor 0Ω bridge | Can substitute a wire bridge |
| 2 | BC337 NPN transistor | TO-92 |
| 1* | FQP27P06 power MOSFET | TO-220 — reverse voltage protection, strongly recommended if powering externally |
| 7 | Female banana jack | 4mm, panel mount |
| 1 | Buzzer | Not on PCB silkscreen |
| 1 | USB socket | For 5V power input |
| 1* | JST connector 1×7 | 2.54mm pitch |
| 1* | JST connector 1×3 | 2.54mm pitch |
| 1* | JST connector 1×5 | 2.54mm pitch |
| 2* | JST connector 1×2 | 2.54mm pitch |
| 2* | Pin strip socket 1×15 | 2.54mm — header for ESP32 |
| 1* | Right-angle 6-pin socket | For HC-06 Bluetooth module |

**Hardware (fasteners):**

| Qty | Description |
|-----|-------------|
| 4 | M3×20 screw (cylinder or round head) |
| 4* | Hotmelt threaded insert M3 |
| 18 | M2×6–10 screw |
| 18* | Hotmelt threaded insert M2 |

**Total build cost: under €60.**

### Enclosure

Three options:
- **3D printed** — model available on [Printables](https://www.printables.com/model/1138815-enclosure-for-scoring-device)
- **Any plastic project box** — the PCB fits standard enclosures
- **A transparent lunchbox** — practical, cheap, and the display is perfectly visible through the lid. See the [legacy wiki](https://github.com/pietwauters/esp32-scoring-device/wiki) for photos.

---

## Getting started

### Prerequisites
- Custom PCB (see hardware repo) with components assembled
- [PlatformIO](https://platformio.org/) (recommended — VS Code extension). The project is built on **ESP-IDF** with some Arduino libraries; PlatformIO handles this combination cleanly. Arduino IDE is not actively supported and may not work reliably.

### Build and flash

```bash
git clone https://github.com/pietwauters/esp32scoringdeviceMqtt.git
cd esp32scoringdeviceMqtt
# Open in VS Code with PlatformIO extension installed
# Select your environment and click Upload
```

### First use

Flash the firmware and power up the device — it will work straight away with sensible defaults. No configuration required to get started.

Calibration is available for fine-tuning once you're up and running, but it is entirely optional. Full configuration and calibration instructions will be documented here as the project develops.

### Connecting to the OpenPiste ecosystem

To use the piste monitor and MQTT-based features, you will need:
- A Raspberry Pi (or any device) running an MQTT broker (e.g. [Mosquitto](https://mosquitto.org/))
- The [CyranoPisteMonitor](https://github.com/pietwauters/CyranoPisteMonitor) web server

> *Network configuration instructions — WiFi settings, MQTT broker address, device ID — will be added here.*

---

## Field-tested

The OpenPiste scoring device has been tested and used in clubs across **Belgium, Spain, and Hong Kong** over several years, and at small local competitions. It is developed by a member of the **FIE SEMI Commission** and **EFC SEMI Commission** — the technology commissions of the International and European Fencing Federations.

---

## Contributing

Contributions are welcome — bug reports, feature requests, documentation improvements, and pull requests. Please open an issue first for any significant changes.

---

## Licence

The firmware is released under the **GNU General Public License v3.0**. See [LICENSE](LICENSE) for details.

Hardware designs (schematics and PCB layouts) are released under the **CERN Open Hardware Licence v2 — Strongly Reciprocal (CERN-OHL-S)**. See the [esp32_scoring_device_hardware](https://github.com/pietwauters/esp32_scoring_device_hardware) repo for details.

**Contributors:** All contributions require signing a Contributor Licence Agreement (CLA) before a pull request can be merged. This allows the project to maintain licensing flexibility for future developments while keeping the open source release fully open. The CLA will be linked here when available.

---

*Part of the [OpenPiste](https://github.com/OpenPiste) platform — open source fencing electronics and IT, built by the fencing community, for the fencing community.*
