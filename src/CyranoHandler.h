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
class CyranoHandler : public Observer<FencingStateMachine>,
                      public Observer<UDPIOHandler>,
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
  // Publish a minimal parry event JSON to MQTT
  void publishParryEvent(bool state, uint64_t timestamp_ms);
  void update(FencingStateMachine *subject, uint32_t eventtype);
  void update(UDPIOHandler *subject, uint32_t eventtype) {
    ProcessUIEvents(eventtype);
  };
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

  // Cyrano-specific fields not in OPP2::SystemState
  std::string m_CompetitionId; //!< Software-provided competition ID

  // ── Cached state for UDP callback stack safety ────────────────────────
  // Phase 6 lesson: getStateCopy() creates large stack allocations (~400-600
  // bytes) In async_udp callback context (limited stack ~4KB), this causes
  // overflow. Solution: Cache converted Cyrano message, update via observer
  // when state changes. Trades heap memory (acceptable) for stack safety
  // (critical).
  EFP1Message m_CachedStatus; //!< Updated when Opp2Handler state changes
  bool m_CachedStatusValid;   //!< True when cache is synchronized

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
