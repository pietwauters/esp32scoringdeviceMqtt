// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#ifndef EFP1Message_H
#define EFP1Message_H
#include <cstddef>
#include <cstdint>
#include <cstring>

#define MAX_NR_FIELDS 41

enum EPF1SubMessage {
  Protocol = 0,
  Command,
  PisteId,
  CompetitionId,
  PhaseNumber,
  Poule_Tableau_Id,
  MatchNumber,
  RoundNumber,
  Start_time,
  StopWatch,
  CompetitionType,
  Weapon,
  Priority,
  State,
  RefereeId,
  RefereeName,
  RefereeNation,
  RightFencerId,
  RightFencerName,
  RightFencerNation,
  RightScore,
  RightStatus,
  RightYCard,
  RightRCard,
  RightLight,
  RightWhiteLight,
  RightMedicalIntervention,
  RightReserveIntroduction,
  RightPCards,
  LeftFencerId,
  LeftFencerName,
  LeftFencerNation,
  LeftScore,
  LeftStatus,
  LeftYCard,
  LeftRCard,
  LeftLight,
  LeftWhiteLight,
  LeftMedicalIntervention,
  LeftReserveIntroduction,
  LeftPCards
};

enum MessageType { HELLO, DISP, ACK, NAK, INFO, NEXT, PREV, ERROR };

// Get() always returns a valid, null-terminated pointer (never nullptr),
// so this is always safe to call on its result.
inline bool EFP1FieldEmpty(const char *field) { return field[0] == '\0'; }

// EFP1.1/Cyrano wire message. Field storage is 41 fixed-size char[] arrays
// -- no std::string, no std::vector, no heap allocation anywhere in this
// class. Field sizes are sourced from docs/CyranoProtocol-1-1.pdf's
// max-length column, with four fields (PisteId, CompetitionId, FencerId,
// FencerName) widened beyond the spec's own numbers to match OPP2's
// existing field widths (opp2_types.h) -- confirmed with Piet 2026-08-13,
// not guessed, to avoid silently truncating real data that already flows
// through those wider OPP2 fields. Status (R5/L5) is similarly widened
// beyond the spec's stated max of 1 to fit "DNS", which this codebase
// sends but which isn't in the official spec's status vocabulary. See
// docs/HEAP_FIX_IMPLEMENTATION_PLAN.md for the full derivation.
//
// Traced 2026-08-12/13: the previous std::vector<std::string>-based
// storage, and the O(n) std::string-concatenation in ToString(), were
// crashing the device (uncaught std::bad_alloc under this device's
// tight/fragmented heap) on ordinary state-change traffic -- not just
// under artificial load. This redesign removes every heap allocation from
// the class, not just the ones that had already crashed.
class EFP1Message {
public:
  EFP1Message();
  // Parses a raw EFP1.1 wire message directly from a C string (e.g.
  // straight from a UDP packet buffer) -- no std::string, no stringstream,
  // no heap. Every token is bounded to its field's own fixed capacity
  // (truncates on overlong input, never overflows).
  explicit EFP1Message(const char *Buffer);
  ~EFP1Message() = default;

  // Every field is a plain fixed char[] array, so the compiler-generated
  // copy constructor/assignment already do a correct, allocation-free
  // member-wise copy -- no custom implementation needed.
  EFP1Message(const EFP1Message &) = default;
  EFP1Message &operator=(const EFP1Message &) = default;

  // Direct pointer into the field's own buffer -- valid for this
  // EFP1Message's lifetime, never nullptr, always null-terminated.
  const char *Get(int field) const;
  // Bounded copy (strncpy + explicit null-termination) into the field's
  // own fixed buffer. Silently truncates an overlong value rather than
  // overflowing -- callers that need to know about truncation should check
  // strlen(value) against the field's known max length themselves.
  void Set(int field, const char *value);

  // Writes the full EFP1.1 wire string into out (bounded to outCap, always
  // null-terminated). Returns the number of bytes written, excluding the
  // null terminator.
  size_t ToString(char *out, size_t outCap) const;
  size_t MakeNextMessageString(char *out, size_t outCap) const;
  size_t MakePrevMessageString(char *out, size_t outCap) const;

  void CopyIfNotEmpty(const EFP1Message &Source);
  void Prune(const EFP1Message &Source);
  void TruncateToMaxLength(void) {
  } // no-op: Set() already bounds every field, nothing left to truncate

  MessageType GetType() const;

  void SetRed(bool value) { Set(LeftLight, value ? "1" : "0"); }
  void SetGreen(bool value) { Set(RightLight, value ? "1" : "0"); }
  void SetWhiteLeft(bool value) { Set(LeftWhiteLight, value ? "1" : "0"); }
  void SetWhiteRight(bool value) { Set(RightWhiteLight, value ? "1" : "0"); }

  void SwapFencersInclScoreCardsEtc();
  void HandleTeamReserve(bool left, bool value);
  uint8_t EFP1StatusString2Type10MessageStatus();
  void print() const;

  // Worst-case output sizes for ToString()/MakeNext/PrevMessageString(),
  // derived field-by-field in docs/HEAP_FIX_IMPLEMENTATION_PLAN.md (general
  // area content 171 bytes + right/left fencer areas 111 bytes each +
  // 48 bytes of '|'/'%|' delimiters = 441 bytes worst case for ToString();
  // rounded up with margin, not tuned to the exact byte).
  static constexpr size_t kMaxWireMessageLength = 512;
  static constexpr size_t kMaxNextPrevMessageLength = 160;

private:
  int GetNrOfGeneralFields() const { return 17; }
  // Always 12 in this codebase: the original PC implementation returned 11
  // for legacy "EFP1" (pre-1.1) messages, but both constructors here
  // unconditionally force Protocol to "EFP1.1" (CLAUDE.md invariant #5),
  // so that branch never fires -- confirmed by checking every call site,
  // not preserved as dead code.
  int GetNrOfFencerFields() const { return 12; }

  struct FieldSlot {
    char *ptr;
    size_t cap; // includes the null terminator
  };
  FieldSlot fieldSlot(int field);

  char mProtocol[7];
  char mCommand[7];
  char mPisteId[33];
  char mCompetitionId[65];
  char mPhaseNumber[3];
  char mPouleTableauId[9];
  char mMatchNumber[4];
  char mRoundNumber[3];
  char mStartTime[6];
  char mStopWatch[9];
  char mCompetitionType[2];
  char mWeapon[2];
  char mPriority[2];
  char mState[2];
  char mRefereeId[9];
  char mRefereeName[21];
  char mRefereeNation[4];

  char mRightFencerId[33];
  char mRightFencerName[65];
  char mRightFencerNation[4];
  char mRightScore[3];
  char mRightStatus[4];
  char mRightYCard[2];
  char mRightRCard[2];
  char mRightLight[2];
  char mRightWhiteLight[2];
  char mRightMedicalIntervention[2];
  char mRightReserveIntroduction[2];
  char mRightPCards[2];

  char mLeftFencerId[33];
  char mLeftFencerName[65];
  char mLeftFencerNation[4];
  char mLeftScore[3];
  char mLeftStatus[4];
  char mLeftYCard[2];
  char mLeftRCard[2];
  char mLeftLight[2];
  char mLeftWhiteLight[2];
  char mLeftMedicalIntervention[2];
  char mLeftReserveIntroduction[2];
  char mLeftPCards[2];
};

#endif // EFP1Message_H
