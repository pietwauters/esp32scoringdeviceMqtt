# MQTT JSON Format Documentation

## Overview
This document describes the JSON payloads published by the ESP32 scoring device to MQTT topics. The messages encode fencing match data and special events.

---

## Main Match Data Message

### Structure
The main message contains header fields (match info), right fencer fields, and left fencer fields.

#### Header Fields
- `Protocol`
- `Com`
- `Piste`
- `Compe`
- `Phase`
- `PoulTab`
- `Match`
- `Round`
- `Time`
- `Stopwatch`
- `Type`
- `Weapon`
- `Priority`
- `State`
- `RefId`
- `RefName`
- `RefNat`

#### Right Fencer Fields
- `RightId`
- `RightName`
- `RightNat`
- `Rscore`
- `Rstatus`
- `RYcard`
- `RRcard`
- `RLight`
- `RWlight`
- `RMedical`
- `RReserve`
- `RP-card`

#### Left Fencer Fields
- `LeftId`
- `LeftName`
- `LeftNat`
- `Lscore`
- `Lstatus`
- `LYcard`
- `LRcard`
- `LLight`
- `LWlight`
- `LMedical`
- `LReserve`
- `LP-card`

### Example
```json
{
  "Protocol": "EFP2",
  "Com": "value",
  "Piste": "001",
  "Compe": "value",
  "Phase": "value",
  "PoulTab": "value",
  "Match": "value",
  "Round": "value",
  "Time": "value",
  "Stopwatch": "value",
  "Type": "value",
  "Weapon": "epee",
  "Priority": "value",
  "State": "value",
  "RefId": "123",
  "RefName": "Smith",
  "RefNat": "FRA",
  "RightId": "456",
  "RightName": "Doe",
  "RightNat": "GER",
  "Rscore": "5",
  "Rstatus": "active",
  "RYcard": "0",
  "RRcard": "0",
  "RLight": "1",
  "RWlight": "0",
  "RMedical": "0",
  "RReserve": "0",
  "RP-card": "0",
  "LeftId": "789",
  "LeftName": "Lee",
  "LeftNat": "ITA",
  "Lscore": "3",
  "Lstatus": "active",
  "LYcard": "0",
  "LRcard": "0",
  "LLight": "0",
  "LWlight": "1",
  "LMedical": "0",
  "LReserve": "0",
  "LP-card": "0"
}
```

---

## Parry Event Message

A minimal message for parry events:

```json
{
  "event": "parry",
  "ts": 123456789,
  "state": 1
}
```
- `event`: Always "parry"
- `ts`: Timestamp in milliseconds
- `state`: 1 (active) or 0 (inactive)

---

## EFP2 Unwillingness to Fight Timer Message

This message is published to indicate the state of the unwillingness to fight timer.

### Example (milliseconds format)
```json
{
  "Protocol": "EFP2",
  "Piste": "001",
  "UW2F_Timer": {
    "time_ms": 60000,
    "timestamp_ms": 1715539200000
  }
}
```

### Example (minutes:seconds format)
```json
{
  "Protocol": "EFP2",
  "Piste": "001",
  "UW2F_Timer": {
    "time": "1:00",
    "timestamp_ms": 1715539200000
  }
}
```

- `Protocol`: Always "EFP2"
- `Piste`: The piste identifier
- `UW2F_Timer`: Object containing:
  - `time_ms`: Timer value in milliseconds (optional)
  - `time`: Timer value as a string in `minutes:seconds` (optional)
  - `timestamp_ms`: Epoch milliseconds when the timer event was generated

---

For further details on field meanings, see the code in `CyranoFields.h` and `CyranoConverter.cpp`.
