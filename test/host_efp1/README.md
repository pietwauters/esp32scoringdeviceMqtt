# EFP1Message host-side test

Standalone, no-device, no-network test for `src/EFP1Message.cpp`/`.h`. Compiles the
real source directly (not a reimplementation), stubbing only `esp_log.h` (the class's
one ESP-IDF dependency, used solely by `print()`). Not wired into PlatformIO's test
runner — just a plain host binary, run directly.

Added 2026-08-13 after this test caught a real bug on its first run: the parsing
constructor (`EFP1Message(const char*)`) was silently misaligning every right/left-fencer
field by one position (missing a leading-`|` skip when transitioning into each fencer
area after its `%` separator) — every field held the *previous* field's value, with
`RightFencerId` ending up empty. This is exactly the kind of silent-data-corruption bug
that's hard to catch on live hardware (looks like garbled state, not a crash) and easy
to catch here (`ToString()` → parse → field-by-field diff). Fixed in the same commit as
this test's addition.

## Run it

```sh
g++ -std=c++14 -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I test/host_efp1 -I src \
  test/host_efp1/test_efp1.cpp src/EFP1Message.cpp \
  -o /tmp/test_efp1
/tmp/test_efp1
```

ASan/UBSan are load-bearing here, not decoration — they're what actually proves no
buffer overflow happens on the truncation/overlong-input/fuzz cases, not just that the
program didn't visibly crash.

## What it covers

- Full 41-field roundtrip (`Set()` → `ToString()` → parse ctor → field-by-field compare),
  including boundary-length values (exactly at each field's cap).
- `Set()` truncation on overlong input (100-char and 10000-char values into small/medium
  fields) — confirms bounded, no overflow.
- Malformed/adversarial wire input to the parsing constructor: empty string, bare `|`,
  missing `%` separators, missing leading `|`, many consecutive empty fields (spec
  explicitly allows this), a 10000-char value crammed into one field, a ~100KB
  adversarial message, `nullptr`.
- Out-of-range field indices to `Get()`/`Set()`.
- `Set()` with a `nullptr` value.
- `SwapFencersInclScoreCardsEtc()`, `CopyIfNotEmpty()`, `Prune()`, `GetType()`,
  `EFP1StatusString2Type10MessageStatus()`, `MakeNextMessageString()`/
  `MakePrevMessageString()` (including into a too-small output buffer), `ToString()`
  into a too-small/zero-size output buffer.
- 20,000-iteration randomized fuzz pass feeding the parsing constructor
  `|`/`%`-heavy random strings up to 2000 chars, checking every field stays a valid
  in-bounds C string afterward and that `ToString()` doesn't misbehave on
  fuzz-parsed content. Fixed seed (reproducible).

Current status: 820,063 checks, 0 failures, 0 ASan/UBSan violations.
