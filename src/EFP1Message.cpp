// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "EFP1Message.h"
#include "esp_log.h"

static const char *EFP1_TAG = "EFP1Message";

EFP1Message::EFP1Message() {
  // Zero every field, then set the two that always have a default value.
  // No allocation -- these are all fixed member arrays.
  mProtocol[0] = '\0';
  mCommand[0] = '\0';
  mPisteId[0] = '\0';
  mCompetitionId[0] = '\0';
  mPhaseNumber[0] = '\0';
  mPouleTableauId[0] = '\0';
  mMatchNumber[0] = '\0';
  mRoundNumber[0] = '\0';
  mStartTime[0] = '\0';
  mStopWatch[0] = '\0';
  mCompetitionType[0] = '\0';
  mWeapon[0] = '\0';
  mPriority[0] = '\0';
  mState[0] = '\0';
  mRefereeId[0] = '\0';
  mRefereeName[0] = '\0';
  mRefereeNation[0] = '\0';
  mRightFencerId[0] = '\0';
  mRightFencerName[0] = '\0';
  mRightFencerNation[0] = '\0';
  mRightScore[0] = '\0';
  mRightStatus[0] = '\0';
  mRightYCard[0] = '\0';
  mRightRCard[0] = '\0';
  mRightLight[0] = '\0';
  mRightWhiteLight[0] = '\0';
  mRightMedicalIntervention[0] = '\0';
  mRightReserveIntroduction[0] = '\0';
  mRightPCards[0] = '\0';
  mLeftFencerId[0] = '\0';
  mLeftFencerName[0] = '\0';
  mLeftFencerNation[0] = '\0';
  mLeftScore[0] = '\0';
  mLeftStatus[0] = '\0';
  mLeftYCard[0] = '\0';
  mLeftRCard[0] = '\0';
  mLeftLight[0] = '\0';
  mLeftWhiteLight[0] = '\0';
  mLeftMedicalIntervention[0] = '\0';
  mLeftReserveIntroduction[0] = '\0';
  mLeftPCards[0] = '\0';

  Set(Protocol, "EFP1.1");
  Set(Command, "INFO");
}

EFP1Message::FieldSlot EFP1Message::fieldSlot(int field) {
  switch (field) {
  case Protocol:
    return {mProtocol, sizeof(mProtocol)};
  case Command:
    return {mCommand, sizeof(mCommand)};
  case PisteId:
    return {mPisteId, sizeof(mPisteId)};
  case CompetitionId:
    return {mCompetitionId, sizeof(mCompetitionId)};
  case PhaseNumber:
    return {mPhaseNumber, sizeof(mPhaseNumber)};
  case Poule_Tableau_Id:
    return {mPouleTableauId, sizeof(mPouleTableauId)};
  case MatchNumber:
    return {mMatchNumber, sizeof(mMatchNumber)};
  case RoundNumber:
    return {mRoundNumber, sizeof(mRoundNumber)};
  case Start_time:
    return {mStartTime, sizeof(mStartTime)};
  case StopWatch:
    return {mStopWatch, sizeof(mStopWatch)};
  case CompetitionType:
    return {mCompetitionType, sizeof(mCompetitionType)};
  case Weapon:
    return {mWeapon, sizeof(mWeapon)};
  case Priority:
    return {mPriority, sizeof(mPriority)};
  case State:
    return {mState, sizeof(mState)};
  case RefereeId:
    return {mRefereeId, sizeof(mRefereeId)};
  case RefereeName:
    return {mRefereeName, sizeof(mRefereeName)};
  case RefereeNation:
    return {mRefereeNation, sizeof(mRefereeNation)};
  case RightFencerId:
    return {mRightFencerId, sizeof(mRightFencerId)};
  case RightFencerName:
    return {mRightFencerName, sizeof(mRightFencerName)};
  case RightFencerNation:
    return {mRightFencerNation, sizeof(mRightFencerNation)};
  case RightScore:
    return {mRightScore, sizeof(mRightScore)};
  case RightStatus:
    return {mRightStatus, sizeof(mRightStatus)};
  case RightYCard:
    return {mRightYCard, sizeof(mRightYCard)};
  case RightRCard:
    return {mRightRCard, sizeof(mRightRCard)};
  case RightLight:
    return {mRightLight, sizeof(mRightLight)};
  case RightWhiteLight:
    return {mRightWhiteLight, sizeof(mRightWhiteLight)};
  case RightMedicalIntervention:
    return {mRightMedicalIntervention, sizeof(mRightMedicalIntervention)};
  case RightReserveIntroduction:
    return {mRightReserveIntroduction, sizeof(mRightReserveIntroduction)};
  case RightPCards:
    return {mRightPCards, sizeof(mRightPCards)};
  case LeftFencerId:
    return {mLeftFencerId, sizeof(mLeftFencerId)};
  case LeftFencerName:
    return {mLeftFencerName, sizeof(mLeftFencerName)};
  case LeftFencerNation:
    return {mLeftFencerNation, sizeof(mLeftFencerNation)};
  case LeftScore:
    return {mLeftScore, sizeof(mLeftScore)};
  case LeftStatus:
    return {mLeftStatus, sizeof(mLeftStatus)};
  case LeftYCard:
    return {mLeftYCard, sizeof(mLeftYCard)};
  case LeftRCard:
    return {mLeftRCard, sizeof(mLeftRCard)};
  case LeftLight:
    return {mLeftLight, sizeof(mLeftLight)};
  case LeftWhiteLight:
    return {mLeftWhiteLight, sizeof(mLeftWhiteLight)};
  case LeftMedicalIntervention:
    return {mLeftMedicalIntervention, sizeof(mLeftMedicalIntervention)};
  case LeftReserveIntroduction:
    return {mLeftReserveIntroduction, sizeof(mLeftReserveIntroduction)};
  case LeftPCards:
    return {mLeftPCards, sizeof(mLeftPCards)};
  default:
    return {nullptr, 0};
  }
}

const char *EFP1Message::Get(int field) const {
  FieldSlot s = const_cast<EFP1Message *>(this)->fieldSlot(field);
  return s.ptr ? s.ptr : "";
}

void EFP1Message::Set(int field, const char *value) {
  FieldSlot s = fieldSlot(field);
  if (!s.ptr || s.cap == 0 || !value)
    return;
  strncpy(s.ptr, value, s.cap - 1);
  s.ptr[s.cap - 1] = '\0';
}

// Appends s to out at position pos, bounded to outCap (always leaves room
// for the null terminator, always null-terminates). Returns the new
// position. No allocation, no temporaries -- this is the replacement for
// the old ToString()'s "Buffer = Buffer + field + '|'" pattern, which
// built two throwaway std::string temporaries per field, 41 times, on
// every call.
static size_t appendBounded(char *out, size_t outCap, size_t pos,
                            const char *s) {
  if (outCap == 0 || pos >= outCap - 1)
    return pos;
  size_t avail = outCap - 1 - pos;
  size_t len = strlen(s);
  size_t n = (len < avail) ? len : avail;
  memcpy(out + pos, s, n);
  pos += n;
  out[pos] = '\0';
  return pos;
}

size_t EFP1Message::ToString(char *out, size_t outCap) const {
  if (outCap == 0)
    return 0;
  out[0] = '\0';
  size_t pos = 0;

  pos = appendBounded(out, outCap, pos, "|");
  for (int i = 0; i < GetNrOfGeneralFields(); i++) {
    pos = appendBounded(out, outCap, pos, Get(i));
    pos = appendBounded(out, outCap, pos, "|");
  }
  pos = appendBounded(out, outCap, pos, "%|");

  int rightBase = GetNrOfGeneralFields();
  for (int i = 0; i < GetNrOfFencerFields(); i++) {
    pos = appendBounded(out, outCap, pos, Get(rightBase + i));
    pos = appendBounded(out, outCap, pos, "|");
  }
  pos = appendBounded(out, outCap, pos, "%|");

  int leftBase = rightBase + GetNrOfFencerFields();
  for (int i = 0; i < GetNrOfFencerFields(); i++) {
    pos = appendBounded(out, outCap, pos, Get(leftBase + i));
    pos = appendBounded(out, outCap, pos, "|");
  }
  pos = appendBounded(out, outCap, pos, "%|");

  return pos;
}

size_t EFP1Message::MakeNextMessageString(char *out, size_t outCap) const {
  if (outCap == 0)
    return 0;
  out[0] = '\0';
  size_t pos = 0;
  pos = appendBounded(out, outCap, pos, "|");
  pos = appendBounded(out, outCap, pos, Get(Protocol));
  pos = appendBounded(out, outCap, pos, "|NEXT|");
  pos = appendBounded(out, outCap, pos, Get(PisteId));
  pos = appendBounded(out, outCap, pos, "|");
  pos = appendBounded(out, outCap, pos, Get(CompetitionId));
  pos = appendBounded(out, outCap, pos, "|%|");
  return pos;
}

size_t EFP1Message::MakePrevMessageString(char *out, size_t outCap) const {
  if (outCap == 0)
    return 0;
  out[0] = '\0';
  size_t pos = 0;
  pos = appendBounded(out, outCap, pos, "|");
  pos = appendBounded(out, outCap, pos, Get(Protocol));
  pos = appendBounded(out, outCap, pos, "|PREV|");
  pos = appendBounded(out, outCap, pos, Get(PisteId));
  pos = appendBounded(out, outCap, pos, "|");
  pos = appendBounded(out, outCap, pos, Get(CompetitionId));
  pos = appendBounded(out, outCap, pos, "|%|");
  return pos;
}

// Consumes one '|'- or '%'-terminated token starting at *pp, copying it
// (bounded to outCap, null-terminated) into out. Advances *pp past the
// token and its trailing '|' (a trailing '%' is left for the caller to
// detect area-end, matching the wire format's 3-area structure). Returns
// false without consuming anything if the area has already ended (current
// char is '%' or the string ended) -- this is what lets a message that
// omits trailing empty fields (explicitly permitted by the EFP1.1 spec)
// leave the rest of an EFP1Message's fields at their constructor defaults.
static bool nextToken(const char **pp, char *out, size_t outCap) {
  const char *p = *pp;
  if (*p == '\0' || *p == '%')
    return false;
  size_t len = strcspn(p, "|%");
  size_t n = (outCap > 0 && len < outCap - 1) ? len : (outCap > 0 ? outCap - 1 : 0);
  if (outCap > 0) {
    memcpy(out, p, n);
    out[n] = '\0';
  }
  p += len;
  if (*p == '|')
    p++;
  *pp = p;
  return true;
}

EFP1Message::EFP1Message(const char *Buffer) : EFP1Message() {
  // Delegates to the default ctor first (Protocol="EFP1.1", Command="INFO",
  // everything else empty) so any field not present in Buffer -- the
  // EFP1.1 spec explicitly allows omitting trailing empty fields, even
  // whole trailing areas -- keeps a safe default instead of stale content.
  if (!Buffer)
    return;

  const char *p = Buffer;
  if (*p == '|')
    p++;

  for (int i = 0; i < GetNrOfGeneralFields(); i++) {
    FieldSlot s = fieldSlot(i);
    char dummy[1];
    if (!nextToken(&p, s.ptr ? s.ptr : dummy, s.ptr ? s.cap : sizeof(dummy)))
      break;
  }
  if (*p == '%')
    p++;
  // Each area after the first still has its own leading '|' immediately
  // after the '%' (the wire format is "...|%|firstField|..." -- the '|'
  // belongs to the next area's field list, not the separator). Traced via
  // the standalone host test 2026-08-13: without this skip, every
  // right/left-fencer field parses one position early -- RightFencerId
  // comes out empty and every subsequent field silently holds the
  // *previous* field's value, all the way through the message. Matches the
  // old stringstream-based parser's explicit `RightFencerFields >> dummy;`
  // / `LeftFencerFields >> dummy;` (both areas, not the first -- the first
  // area's leading '|' is the one stripped above, before this loop).
  if (*p == '|')
    p++;

  int rightBase = GetNrOfGeneralFields();
  for (int i = 0; i < GetNrOfFencerFields(); i++) {
    FieldSlot s = fieldSlot(rightBase + i);
    char dummy[1];
    if (!nextToken(&p, s.ptr ? s.ptr : dummy, s.ptr ? s.cap : sizeof(dummy)))
      break;
  }
  if (*p == '%')
    p++;
  if (*p == '|')
    p++;

  int leftBase = rightBase + GetNrOfFencerFields();
  for (int i = 0; i < GetNrOfFencerFields(); i++) {
    FieldSlot s = fieldSlot(leftBase + i);
    char dummy[1];
    if (!nextToken(&p, s.ptr ? s.ptr : dummy, s.ptr ? s.cap : sizeof(dummy)))
      break;
  }

  // Matches the old behavior: Protocol is unconditionally forced to
  // "EFP1.1" after parsing, regardless of what the wire message actually
  // contained (CLAUDE.md invariant #5 -- this device never treats an
  // incoming message as bare legacy "EFP1").
  Set(Protocol, "EFP1.1");
}

void EFP1Message::CopyIfNotEmpty(const EFP1Message &Source) {
  for (int i = 0; i < MAX_NR_FIELDS; i++) {
    const char *v = Source.Get(i);
    if (!EFP1FieldEmpty(v))
      Set(i, v);
  }
}

void EFP1Message::Prune(const EFP1Message &Source) {
  for (int i = 0; i < MAX_NR_FIELDS; i++) {
    const char *mine = Get(i);
    const char *theirs = Source.Get(i);
    if (strcmp(mine, theirs) == 0)
      Set(i, "");
    else
      Set(i, theirs);
  }
}

void EFP1Message::SwapFencersInclScoreCardsEtc() {
  static const int kRightFields[] = {
      RightFencerId, RightFencerName,     RightFencerNation, RightScore,
      RightStatus,   RightYCard,          RightRCard,        RightLight,
      RightWhiteLight, RightMedicalIntervention, RightReserveIntroduction,
      RightPCards};
  static const int kLeftFields[] = {
      LeftFencerId, LeftFencerName,     LeftFencerNation, LeftScore,
      LeftStatus,   LeftYCard,          LeftRCard,        LeftLight,
      LeftWhiteLight, LeftMedicalIntervention, LeftReserveIntroduction,
      LeftPCards};
  // Right/left corresponding fields always have identical capacities (both
  // sides use the same widths) -- a plain fixed-size byte swap via the
  // wider field's own capacity (FencerName, 65) is safe and simpler than
  // per-field strncpy bookkeeping.
  char temp[65];
  for (size_t k = 0; k < sizeof(kRightFields) / sizeof(kRightFields[0]); k++) {
    FieldSlot r = fieldSlot(kRightFields[k]);
    FieldSlot l = fieldSlot(kLeftFields[k]);
    memcpy(temp, r.ptr, r.cap);
    memcpy(r.ptr, l.ptr, r.cap);
    memcpy(l.ptr, temp, r.cap);
  }
}

void EFP1Message::HandleTeamReserve(bool left, bool value) {
  Set(left ? LeftReserveIntroduction : RightReserveIntroduction,
      value ? "R" : "N");
}

uint8_t EFP1Message::EFP1StatusString2Type10MessageStatus() {
  const char *s = Get(State);
  if (strcmp(s, "F") == 0)
    return 'F';
  if (strcmp(s, "H") == 0)
    return 'H';
  if (strcmp(s, "P") == 0)
    return 'P';
  if (strcmp(s, "W") == 0)
    return 'W';
  if (strcmp(s, "E") == 0)
    return 'E';
  return 'U';
}

MessageType EFP1Message::GetType() const {
  const char *cmd = Get(Command);
  if (strcmp(cmd, "HELLO") == 0)
    return HELLO;
  if (strcmp(cmd, "DISP") == 0)
    return DISP;
  if (strcmp(cmd, "ACK") == 0)
    return ACK;
  if (strcmp(cmd, "NAK") == 0)
    return NAK;
  if (strcmp(cmd, "INFO") == 0)
    return INFO;
  if (strcmp(cmd, "NEXT") == 0)
    return NEXT;
  if (strcmp(cmd, "PREV") == 0)
    return PREV;
  return ERROR;
}

void EFP1Message::print() const {
  ESP_LOGI(EFP1_TAG, "Protocol: %s", Get(Protocol));
  ESP_LOGI(EFP1_TAG, "Command: %s", Get(Command));
  ESP_LOGI(EFP1_TAG, "PisteId: %s", Get(PisteId));
  ESP_LOGI(EFP1_TAG, "CompetitionId: %s", Get(CompetitionId));
  ESP_LOGI(EFP1_TAG, "PhaseNumber: %s", Get(PhaseNumber));
  ESP_LOGI(EFP1_TAG, "Poule_Tableau_Id: %s", Get(Poule_Tableau_Id));
  ESP_LOGI(EFP1_TAG, "MatchNumber: %s", Get(MatchNumber));
  ESP_LOGI(EFP1_TAG, "RoundNumber: %s", Get(RoundNumber));
  ESP_LOGI(EFP1_TAG, "Start_time: %s", Get(Start_time));
  ESP_LOGI(EFP1_TAG, "StopWatch: %s", Get(StopWatch));
  ESP_LOGI(EFP1_TAG, "CompetitionType: %s", Get(CompetitionType));
  ESP_LOGI(EFP1_TAG, "Weapon: %s", Get(Weapon));
  ESP_LOGI(EFP1_TAG, "Priority: %s", Get(Priority));
  ESP_LOGI(EFP1_TAG, "State: %s", Get(State));
  ESP_LOGI(EFP1_TAG, "RefereeId: %s", Get(RefereeId));
  ESP_LOGI(EFP1_TAG, "RefereeName: %s", Get(RefereeName));
  ESP_LOGI(EFP1_TAG, "RefereeNation: %s", Get(RefereeNation));
  ESP_LOGI(EFP1_TAG, "RightFencerId: %s", Get(RightFencerId));
  ESP_LOGI(EFP1_TAG, "RightFencerName: %s", Get(RightFencerName));
  ESP_LOGI(EFP1_TAG, "RightFencerNation: %s", Get(RightFencerNation));
  ESP_LOGI(EFP1_TAG, "RightScore: %s", Get(RightScore));
  ESP_LOGI(EFP1_TAG, "RightStatus: %s", Get(RightStatus));
  ESP_LOGI(EFP1_TAG, "RightYCard: %s", Get(RightYCard));
  ESP_LOGI(EFP1_TAG, "RightRCard: %s", Get(RightRCard));
  ESP_LOGI(EFP1_TAG, "RightLight: %s", Get(RightLight));
  ESP_LOGI(EFP1_TAG, "RightWhiteLight: %s", Get(RightWhiteLight));
  ESP_LOGI(EFP1_TAG, "RightMedicalIntervention: %s",
           Get(RightMedicalIntervention));
  ESP_LOGI(EFP1_TAG, "RightReserveIntroduction: %s",
           Get(RightReserveIntroduction));
  ESP_LOGI(EFP1_TAG, "RightPCards: %s", Get(RightPCards));
  ESP_LOGI(EFP1_TAG, "LeftFencerId: %s", Get(LeftFencerId));
  ESP_LOGI(EFP1_TAG, "LeftFencerName: %s", Get(LeftFencerName));
  ESP_LOGI(EFP1_TAG, "LeftFencerNation: %s", Get(LeftFencerNation));
  ESP_LOGI(EFP1_TAG, "LeftScore: %s", Get(LeftScore));
  ESP_LOGI(EFP1_TAG, "LeftStatus: %s", Get(LeftStatus));
  ESP_LOGI(EFP1_TAG, "LeftYCard: %s", Get(LeftYCard));
  ESP_LOGI(EFP1_TAG, "LeftRCard: %s", Get(LeftRCard));
  ESP_LOGI(EFP1_TAG, "LeftLight: %s", Get(LeftLight));
  ESP_LOGI(EFP1_TAG, "LeftWhiteLight: %s", Get(LeftWhiteLight));
  ESP_LOGI(EFP1_TAG, "LeftMedicalIntervention: %s",
           Get(LeftMedicalIntervention));
  ESP_LOGI(EFP1_TAG, "LeftReserveIntroduction: %s",
           Get(LeftReserveIntroduction));
  ESP_LOGI(EFP1_TAG, "LeftPCards: %s", Get(LeftPCards));
}
