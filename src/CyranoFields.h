// ---- CyranoFields.h ----
#pragma once
#include <string>
#include <vector>
constexpr const char* header_fields[] = {
    "Protocol", "Com", "Piste", "Compe", "Phase", "PoulTab", "Match",
    "Round", "Time", "Stopwatch", "Type", "Weapon", "Priority",
    "State", "RefId", "RefName", "RefNat"
};

constexpr const char* right_fencer_fields[] = {
    "RightId", "RightName", "RightNat", "Rscore", "Rstatus",
    "RYcard", "RRcard", "RLight", "RWlight", "RMedical", "RReserve", "RP-card"
};

constexpr const char* left_fencer_fields[] = {
    "LeftId", "LeftName", "LeftNat", "Lscore", "Lstatus",
    "LYcard", "LRcard", "LLight", "LWlight", "LMedical", "LReserve", "LP-card"
};
