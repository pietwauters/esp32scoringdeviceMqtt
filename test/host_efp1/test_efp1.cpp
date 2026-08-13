// Standalone host-side test for EFP1Message -- no ESP32, no network. Compiled
// directly against the real src/EFP1Message.cpp (only stubbed dependency is
// esp_log.h, used solely by print()). Run under ASan/UBSan so any buffer
// overflow or undefined behavior is caught immediately and precisely,
// something live-device testing can't give us.
#include "EFP1Message.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <random>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
      g_failures++;                                                          \
      printf("  FAIL: %s (line %d)\n", msg, __LINE__);                      \
    }                                                                        \
  } while (0)

#define CHECK_STREQ(a, b, msg)                                               \
  do {                                                                       \
    g_checks++;                                                              \
    if (strcmp((a), (b)) != 0) {                                             \
      g_failures++;                                                         \
      printf("  FAIL: %s (line %d)\n", msg, __LINE__);                       \
      printf("        expected=%.80s\n", (b));                              \
      printf("        got     =%.80s\n", (a));                              \
    }                                                                        \
  } while (0)

static const char *FIELD_NAMES[] = {
    "Protocol", "Command", "PisteId", "CompetitionId", "PhaseNumber",
    "Poule_Tableau_Id", "MatchNumber", "RoundNumber", "Start_time",
    "StopWatch", "CompetitionType", "Weapon", "Priority", "State",
    "RefereeId", "RefereeName", "RefereeNation", "RightFencerId",
    "RightFencerName", "RightFencerNation", "RightScore", "RightStatus",
    "RightYCard", "RightRCard", "RightLight", "RightWhiteLight",
    "RightMedicalIntervention", "RightReserveIntroduction", "RightPCards",
    "LeftFencerId", "LeftFencerName", "LeftFencerNation", "LeftScore",
    "LeftStatus", "LeftYCard", "LeftRCard", "LeftLight", "LeftWhiteLight",
    "LeftMedicalIntervention", "LeftReserveIntroduction", "LeftPCards"};

// -----------------------------------------------------------------------
void test_roundtrip_full() {
  printf("=== TEST: full roundtrip, all 41 fields, valid boundary-length values ===\n");
  EFP1Message msg;

  // FencerName cap is 65 (64 usable) -- exactly 64 'A's should survive intact.
  std::string name64(64, 'A');
  // CompetitionId cap is 65 (64 usable).
  std::string compe64(64, 'C');
  // PisteId cap is 33 (32 usable).
  std::string piste32(32, 'P');
  // FencerId cap is 33 (32 usable).
  std::string id32(32, 'I');

  msg.Set(Protocol, "EFP1.1");
  msg.Set(Command, "DISP");
  msg.Set(PisteId, piste32.c_str());
  msg.Set(CompetitionId, compe64.c_str());
  msg.Set(PhaseNumber, "2");
  msg.Set(Poule_Tableau_Id, "A16");
  msg.Set(MatchNumber, "7");
  msg.Set(RoundNumber, "1");
  msg.Set(Start_time, "14:30");
  msg.Set(StopWatch, "3:00");
  msg.Set(CompetitionType, "I");
  msg.Set(Weapon, "E");
  msg.Set(Priority, "N");
  msg.Set(State, "H");
  msg.Set(RefereeId, "77");
  msg.Set(RefereeName, "Referee Name");
  msg.Set(RefereeNation, "GBR");
  msg.Set(RightFencerId, id32.c_str());
  msg.Set(RightFencerName, name64.c_str());
  msg.Set(RightFencerNation, "FRA");
  msg.Set(RightScore, "3");
  msg.Set(RightStatus, "DNS"); // the field agreed to widen for exactly this
  msg.Set(RightYCard, "0");
  msg.Set(RightRCard, "0");
  msg.Set(RightLight, "0");
  msg.Set(RightWhiteLight, "0");
  msg.Set(RightMedicalIntervention, "0");
  msg.Set(RightReserveIntroduction, "N");
  msg.Set(RightPCards, "0");
  msg.Set(LeftFencerId, "L-ID-02");
  msg.Set(LeftFencerName, "Left Fencer");
  msg.Set(LeftFencerNation, "ITA");
  msg.Set(LeftScore, "5");
  msg.Set(LeftStatus, "U");
  msg.Set(LeftYCard, "1");
  msg.Set(LeftRCard, "0");
  msg.Set(LeftLight, "1");
  msg.Set(LeftWhiteLight, "0");
  msg.Set(LeftMedicalIntervention, "0");
  msg.Set(LeftReserveIntroduction, "N");
  msg.Set(LeftPCards, "2");

  // Verify every field reads back exactly what was set, before any
  // serialize/parse round trip.
  CHECK_STREQ(msg.Get(RightFencerName), name64.c_str(), "64-char name stored intact (Get before roundtrip)");
  CHECK_STREQ(msg.Get(CompetitionId), compe64.c_str(), "64-char competitionId stored intact");
  CHECK_STREQ(msg.Get(PisteId), piste32.c_str(), "32-char pisteId stored intact");
  CHECK_STREQ(msg.Get(RightStatus), "DNS", "DNS status stored intact (3 chars, the widened field)");

  char wire[EFP1Message::kMaxWireMessageLength];
  size_t len = msg.ToString(wire, sizeof(wire));
  CHECK(len > 0, "ToString produced non-empty output");
  CHECK(len < sizeof(wire) - 1, "ToString output comfortably within buffer (no silent truncation of the whole message)");
  printf("  wire length: %zu bytes\n", len);
  printf("  wire: %s\n", wire);

  // Parse it back and confirm every field survives the roundtrip.
  EFP1Message parsed(wire);
  int mismatches = 0;
  for (int i = 0; i < MAX_NR_FIELDS; i++) {
    if (i == Command) continue; // Command intentionally not re-verified (DISP is what we set)
    const char *orig = msg.Get(i);
    const char *back = parsed.Get(i);
    if (strcmp(orig, back) != 0) {
      mismatches++;
      printf("  MISMATCH field[%d]=%s: sent=%.70s got=%.70s\n", i,
             FIELD_NAMES[i], orig, back);
    }
  }
  CHECK(mismatches == 0, "all 41 fields (except Command) roundtrip through ToString()+parse ctor");
  CHECK_STREQ(parsed.Get(Command), "DISP", "Command roundtrips too");
}

// -----------------------------------------------------------------------
void test_truncation_on_overlong_set() {
  printf("=== TEST: Set() truncates overlong values instead of overflowing ===\n");
  EFP1Message msg;

  // FencerName cap 65 (64 usable) -- 100 'B's should truncate to exactly 64.
  std::string overlong(100, 'B');
  msg.Set(RightFencerName, overlong.c_str());
  CHECK(strlen(msg.Get(RightFencerName)) == 64, "100-char input into 64-cap field truncates to exactly 64");
  CHECK(strncmp(msg.Get(RightFencerName), overlong.c_str(), 64) == 0, "truncated content matches the first 64 source bytes");

  // A 1-char field (e.g. Weapon, cap 2 / 1 usable) with a long value.
  msg.Set(Weapon, "EPEEEPEEEPEE");
  CHECK(strlen(msg.Get(Weapon)) == 1, "12-char input into 1-cap field truncates to exactly 1");
  CHECK(msg.Get(Weapon)[0] == 'E', "truncated single char is the first source char");

  // Extremely long input (10KB) into the widest field (65 cap) -- this is
  // the case most likely to reveal a buffer overflow if Set()'s bounding
  // were wrong; ASan will catch it if so.
  std::string huge(10000, 'X');
  msg.Set(CompetitionId, huge.c_str());
  CHECK(strlen(msg.Get(CompetitionId)) == 64, "10000-char input into 64-cap field truncates to exactly 64, no overflow");
}

// -----------------------------------------------------------------------
void test_malformed_wire_parsing() {
  printf("=== TEST: malformed/adversarial wire input to the parsing constructor ===\n");

  // Empty string.
  {
    EFP1Message m("");
    CHECK(EFP1FieldEmpty(m.Get(PisteId)), "empty input -> PisteId empty, no crash");
    CHECK_STREQ(m.Get(Protocol), "EFP1.1", "empty input -> Protocol still forced to EFP1.1 (matches old behavior)");
  }

  // Just a pipe.
  {
    EFP1Message m("|");
    CHECK_STREQ(m.Get(Command), "INFO", "single '|' -> parses nothing, Command keeps default ctor's \"INFO\", no crash");
  }

  // No '%' separators at all (single blob, real EFP1.1 always has them).
  {
    EFP1Message m("|EFP1.1|HELLO|6|rtchk|");
    CHECK_STREQ(m.Get(Protocol), "EFP1.1", "no '%' separators -> general fields still parse up to string end");
    CHECK_STREQ(m.Get(Command), "HELLO", "no '%' separators -> Command parses correctly");
  }

  // Missing leading '|'.
  {
    EFP1Message m("EFP1.1|HELLO|6|rtchk|%|");
    CHECK_STREQ(m.Get(Protocol), "EFP1.1", "missing leading '|' -> still parses Protocol (first char isn't '|', so it's treated as part of area 0 directly)");
  }

  // Many consecutive empty fields (spec explicitly allows omitted/empty fields).
  {
    EFP1Message m("|EFP1.1|INFO||||||||3:00||||W|%||||0|U|0|1|1|0|0|N|%||||0|U|0|1|0|0|0|N|%|");
    CHECK_STREQ(m.Get(Protocol), "EFP1.1", "many empty fields -> Protocol still correct");
    CHECK_STREQ(m.Get(Command), "INFO", "many empty fields -> Command still correct");
    CHECK_STREQ(m.Get(StopWatch), "3:00", "many empty fields -> a populated field mid-run still parses correctly");
    CHECK(EFP1FieldEmpty(m.Get(PisteId)), "empty field in the run really is empty, not garbage");
  }

  // Wildly overlong single field within an otherwise-valid message (10KB in
  // one slot) -- exercises the parsing constructor's own bounding, not
  // Set()'s (different code path: nextToken() writing into fieldSlot().ptr
  // directly).
  {
    std::string huge_field(10000, 'Z');
    std::string msg_str = "|EFP1.1|HELLO|" + huge_field + "|rtchk|%|";
    EFP1Message m(msg_str.c_str());
    CHECK(strlen(m.Get(PisteId)) == 32, "10000-char field in raw wire input truncates to PisteId's 32-char cap, no overflow");
  }

  // Message far longer than the spec's ~210-char real-world max (100KB
  // total), to stress the whole parse loop, not just one field.
  {
    std::string massive;
    massive = "|EFP1.1|DISP|6|c|";
    for (int i = 0; i < 2000; i++) {
      massive += std::string(50, 'M') + "|";
    }
    massive += "%|%|";
    EFP1Message m(massive.c_str());
    // Just must not crash/overflow; field 4 (PhaseNumber) onward will have
    // absorbed some of the flood, general area only has 17 fields so most
    // of `massive` is simply never consumed by the field loop.
    CHECK_STREQ(m.Get(Protocol), "EFP1.1", "100KB adversarial message -> still parses Protocol correctly, no crash");
  }

  // No message at all (nullptr) -- explicitly guarded in the constructor.
  {
    EFP1Message m(nullptr);
    CHECK_STREQ(m.Get(Protocol), "EFP1.1", "nullptr input -> default-constructed state (Protocol=EFP1.1), no crash");
    CHECK_STREQ(m.Get(Command), "INFO", "nullptr input -> Command keeps default ctor's \"INFO\", no crash");
  }
}

// -----------------------------------------------------------------------
void test_get_set_invalid_index() {
  printf("=== TEST: Get()/Set() with out-of-range indices ===\n");
  EFP1Message msg;
  CHECK_STREQ(msg.Get(-1), "", "Get(-1) returns empty string, not garbage/crash");
  CHECK_STREQ(msg.Get(999), "", "Get(999) returns empty string, not garbage/crash");
  msg.Set(-1, "should be ignored");
  msg.Set(999, "should also be ignored");
  CHECK(g_checks > 0, "Set() with invalid index does not crash (reaching this line proves it)");
}

// -----------------------------------------------------------------------
void test_set_null_value() {
  printf("=== TEST: Set() with a null value pointer ===\n");
  EFP1Message msg;
  msg.Set(Weapon, "E");
  msg.Set(Weapon, nullptr); // should be a no-op per the guard, not a crash
  CHECK_STREQ(msg.Get(Weapon), "E", "Set(field, nullptr) is a no-op, previous value preserved");
}

// -----------------------------------------------------------------------
void test_swap_fencers() {
  printf("=== TEST: SwapFencersInclScoreCardsEtc() ===\n");
  EFP1Message msg;
  msg.Set(RightFencerName, "Right Person");
  msg.Set(RightScore, "9");
  msg.Set(LeftFencerName, "Left Person");
  msg.Set(LeftScore, "3");
  msg.SwapFencersInclScoreCardsEtc();
  CHECK_STREQ(msg.Get(RightFencerName), "Left Person", "swap: right name now holds former left name");
  CHECK_STREQ(msg.Get(LeftFencerName), "Right Person", "swap: left name now holds former right name");
  CHECK_STREQ(msg.Get(RightScore), "3", "swap: right score now holds former left score");
  CHECK_STREQ(msg.Get(LeftScore), "9", "swap: left score now holds former right score");
}

// -----------------------------------------------------------------------
void test_copy_if_not_empty_and_prune() {
  printf("=== TEST: CopyIfNotEmpty() and Prune() ===\n");
  EFP1Message target;
  target.Set(RightScore, "1");
  target.Set(LeftScore, "2");

  EFP1Message source;
  source.Set(RightScore, "9"); // non-empty -> should overwrite
  // LeftScore left empty in source -> should NOT overwrite target's "2"

  target.CopyIfNotEmpty(source);
  CHECK_STREQ(target.Get(RightScore), "9", "CopyIfNotEmpty: non-empty source field overwrites target");
  CHECK_STREQ(target.Get(LeftScore), "2", "CopyIfNotEmpty: empty source field leaves target unchanged");

  EFP1Message a, b;
  a.Set(RightScore, "5");
  a.Set(LeftScore, "5");
  b.Set(RightScore, "5");  // same as a -> Prune should blank this
  b.Set(LeftScore, "7");   // different from a -> Prune should keep b's value
  a.Prune(b);
  CHECK(EFP1FieldEmpty(a.Get(RightScore)), "Prune: field equal in both -> blanked");
  CHECK_STREQ(a.Get(LeftScore), "7", "Prune: field differing -> takes source's value");
}

// -----------------------------------------------------------------------
void test_get_type() {
  printf("=== TEST: GetType() ===\n");
  struct { const char *cmd; MessageType expect; } cases[] = {
      {"HELLO", HELLO}, {"DISP", DISP}, {"ACK", ACK}, {"NAK", NAK},
      {"INFO", INFO}, {"NEXT", NEXT}, {"PREV", PREV}, {"GARBAGE", ERROR},
      {"", ERROR},
  };
  for (auto &c : cases) {
    EFP1Message m;
    m.Set(Command, c.cmd);
    CHECK(m.GetType() == c.expect, c.cmd);
  }
}

// -----------------------------------------------------------------------
void test_status_conversion() {
  printf("=== TEST: EFP1StatusString2Type10MessageStatus() ===\n");
  struct { const char *state; uint8_t expect; } cases[] = {
      {"F", 'F'}, {"H", 'H'}, {"P", 'P'}, {"W", 'W'}, {"E", 'E'},
      {"?", 'U'}, {"", 'U'},
  };
  for (auto &c : cases) {
    EFP1Message m;
    m.Set(State, c.state);
    CHECK(m.EFP1StatusString2Type10MessageStatus() == c.expect, c.state);
  }
}

// -----------------------------------------------------------------------
void test_make_next_prev() {
  printf("=== TEST: MakeNextMessageString() / MakePrevMessageString() ===\n");
  EFP1Message msg;
  msg.Set(Protocol, "EFP1.1");
  msg.Set(PisteId, "6");
  msg.Set(CompetitionId, "rtchk");

  char next[EFP1Message::kMaxNextPrevMessageLength];
  size_t n = msg.MakeNextMessageString(next, sizeof(next));
  CHECK(n > 0, "MakeNextMessageString produces output");
  CHECK_STREQ(next, "|EFP1.1|NEXT|6|rtchk|%|", "MakeNextMessageString exact expected wire format");

  char prev[EFP1Message::kMaxNextPrevMessageLength];
  msg.MakePrevMessageString(prev, sizeof(prev));
  CHECK_STREQ(prev, "|EFP1.1|PREV|6|rtchk|%|", "MakePrevMessageString exact expected wire format");

  // Tiny output buffer -- must not overflow, should truncate gracefully.
  char tiny[5];
  size_t tn = msg.MakeNextMessageString(tiny, sizeof(tiny));
  CHECK(tn == 4, "MakeNextMessageString into a 5-byte buffer writes exactly 4 chars + null");
  CHECK(tiny[4] == '\0', "5-byte buffer result is null-terminated at the last byte");
}

// -----------------------------------------------------------------------
void test_tostring_tiny_buffer() {
  printf("=== TEST: ToString() into a too-small buffer ===\n");
  EFP1Message msg;
  msg.Set(Protocol, "EFP1.1");
  msg.Set(RightFencerName, "A very long fencer name that will not fit");

  char tiny[10];
  size_t n = msg.ToString(tiny, sizeof(tiny));
  CHECK(n == 9, "ToString into 10-byte buffer writes exactly 9 chars + null, no overflow");
  CHECK(tiny[9] == '\0', "result is null-terminated within bounds");

  // Zero-capacity buffer -- must not crash or write anything.
  char zero[1];
  size_t zn = msg.ToString(zero, 0);
  CHECK(zn == 0, "ToString with outCap=0 returns 0, does not touch the buffer");
}

// -----------------------------------------------------------------------
void test_fuzz_random_input() {
  printf("=== TEST: randomized fuzz input to the parsing constructor (ASan/UBSan-checked) ===\n");
  std::mt19937 rng(0xE5F1); // fixed seed -- reproducible
  std::uniform_int_distribution<int> lenDist(0, 2000);
  // Bias toward '|' and '%' since those are the bytes that actually drive
  // control flow in the parser; pure-random bytes would mostly just become
  // inert field content.
  const char *alphabet = "|%|%|%abcXYZ019 \t\"'\\";
  int alphabetLen = (int)strlen(alphabet);
  std::uniform_int_distribution<int> charDist(0, alphabetLen - 1);

  const int kIterations = 20000;
  for (int iter = 0; iter < kIterations; iter++) {
    int len = lenDist(rng);
    std::string s;
    s.reserve(len);
    for (int i = 0; i < len; i++) {
      s += alphabet[charDist(rng)];
    }
    EFP1Message m(s.c_str()); // ASan/UBSan will abort the whole test on any real violation
    // Cheap sanity check reachable only if nothing crashed: every field
    // must still be a valid, null-terminated, in-bounds C string.
    for (int f = 0; f < MAX_NR_FIELDS; f++) {
      const char *v = m.Get(f);
      g_checks++;
      if (v == nullptr) {
        g_failures++;
        printf("  FAIL: Get(%d) returned nullptr after fuzz input (iter %d)\n", f, iter);
      }
    }
    // Also exercise ToString() on the resulting (possibly maximally-full)
    // object -- the combination of fuzz-parsed content feeding back into
    // the bounded serializer is its own interesting stress case.
    char wire[EFP1Message::kMaxWireMessageLength];
    m.ToString(wire, sizeof(wire));
  }
  printf("  %d fuzz iterations completed with no crash/ASan/UBSan violation\n", kIterations);
}

// -----------------------------------------------------------------------
int main() {
  test_roundtrip_full();
  test_truncation_on_overlong_set();
  test_malformed_wire_parsing();
  test_get_set_invalid_index();
  test_set_null_value();
  test_swap_fencers();
  test_copy_if_not_empty_and_prune();
  test_get_type();
  test_status_conversion();
  test_make_next_prev();
  test_tostring_tiny_buffer();
  test_fuzz_random_input();

  printf("\n=== %d checks, %d failures ===\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
