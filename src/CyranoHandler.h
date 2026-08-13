#ifndef CYRANOHANDLER_H
#define CYRANOHANDLER_H
#include "AsyncUDP.h"
#include "EFP1Message.h"
#include "EventDefinitions.h"
#include "FencingStateMachine.h"
#include "SubjectObserverTemplate.h"
#include <AsyncTCP.h>
#include <Preferences.h>
#include <WiFi.h>
#include <iostream>
// #include <AsyncMqttClient.h>
#include <AtlasAsyncMqttClient.h>

#define CYRANO_PORT 50101
#define CYRANO_BROADCAST_PORT 50100

enum CyranoState { FENCING, HALT, PAUSE, ENDING, WAITING };

/*enum EventType
{
    NextButtonPressed,
    PrevButtonPressed,
    BeginButtonPressed,
    EndButtonPressed
};*/

class UDPIOHandler;
class Opp2Handler;
class CyranoHandler : public Observer<UDPIOHandler>,
                      public Observer<Opp2Handler>,
                      public Subject<CyranoHandler>,
                      public SingletonMixin<CyranoHandler> {
public:
  /** Default destructor */
  virtual ~CyranoHandler();
  void ProcessMessageFromSoftware(const EFP1Message &input,
                                  bool bVerifyPisteID = true);
  void SendInfoMessage();
  void ProcessUIEvents(uint32_t const event);
  void SetPisteID(const std::string &ID);
  void update(UDPIOHandler *subject, uint32_t eventtype) {
    ProcessUIEvents(eventtype);
  };
  void update(Opp2Handler *subject, uint32_t eventtype);
  void StateChanged(uint32_t eventtype) { notify(eventtype); }
  void StateChanged(std::string eventtype) { notify(eventtype); }
  void ProcessLightsChange(uint32_t eventtype);
  void CheckConnection();
  void PeriodicallyBroadcastStatus();
  void Begin();
  bool SoftwareIsLive() { return bSoftwareIsLive; };
  IPAddress SoftwareIPAddress() { return mSoftwareIPAddress; };
  void SoftwareIPAddress(IPAddress theSoftwareIPAddress) {
    mSoftwareIPAddress = theSoftwareIPAddress;
  };
  void ClearOnACK();

  /**
   * Update cached Cyrano status message (push from Opp2Handler).
   * Called when OPP2 state changes - avoids mutex reads in UDP callbacks.
   * Thread-safe: only called from Core 0 contexts.
   */
  void updateCachedStatus(const EFP1Message &status);

protected:
private:
  friend class SingletonMixin<CyranoHandler>;
  /** Default constructor */
  CyranoHandler();

  // Cyrano-specific fields not in OPP2::SystemState. Fixed char[] (matches
  // EFP1Message::CompetitionId's own width) -- not std::string. Traced
  // 2026-08-12/13: this field and the three cached strings below were
  // std::string, rebuilt on every state change regardless of whether a
  // Cyrano CMS was even connected, and were part of what crashed this
  // device under ordinary traffic. See docs/HEAP_FIX_IMPLEMENTATION_PLAN.md.
  char m_CompetitionId[65]; //!< Software-provided competition ID

  // ── Cached strings for UDP callback stack safety ──────────────────────
  // Phase 6 lesson: String building (ToString(), JSON conversion) in
  // UDP callbacks causes stack overflow in async_udp task (~4KB stack).
  // Solution: Pre-build and cache ALL final strings (INFO, NEXT, PREV).
  // Rebuild only when state or CompetitionId changes.
  // SendInfoMessage() and ProcessUIEvents() use cached strings - NO stack,
  // no heap (fixed char[] sized from EFP1Message::kMax*Length -- see
  // EFP1Message.h for the derivation).
  EFP1Message m_CachedStatus; //!< Updated when Opp2Handler state changes
  char m_CachedCyranoString[EFP1Message::kMaxWireMessageLength]; //!< Pre-built INFO Cyrano string
  char m_CachedNextCyrano[EFP1Message::kMaxNextPrevMessageLength]; //!< Pre-built NEXT Cyrano string
  char m_CachedPrevCyrano[EFP1Message::kMaxNextPrevMessageLength]; //!< Pre-built PREV Cyrano string
  bool m_CachedStatusValid; //!< True when cache is synchronized

  /**
   * Rebuild cached strings from m_CachedStatus and m_CompetitionId.
   * Called when state changes (via updateCachedStatus) or CompetitionId
   * changes.
   */
  void RebuildCachedStrings();

  EFP1Message m_IncompleteMessage;
  CyranoState m_State = WAITING;
  int previous_seconds =
      99; // This will always result in setting a correct initial value
  uint32_t m_timeToShowTimer = 0;
  long NextTimeToCheckConnection = 0;
  long LastHelloReception = 0;
  bool bWifiConnected = false;
  bool bCyranoConnected = false;
  bool bmqttCyranoConnected = false;
  bool budpCyranoConnected = false;

  AsyncUDP CyranoHandlerudpRcv;
  AsyncUDP CyranoHandlerudpBroadcast;
  bool bOKToSend = false;
  Preferences networkpreferences;
  uint16_t CyranoPort = CYRANO_PORT;
  uint16_t CyranoBroadcastPort = CYRANO_BROADCAST_PORT;
  long NextPeriodicalUpdate;
  bool bSoftwareIsLive = false;
  IPAddress mSoftwareIPAddress;
};

#endif // CYRANOHANDLER_H
