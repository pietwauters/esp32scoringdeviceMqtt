#ifndef OPP2HANDLER_H
#define OPP2HANDLER_H

#include "CyranoHandler.h"
#include "EventDefinitions.h"
#include "FencingStateMachine.h"
#include "SubjectObserverTemplate.h"
#include <AtlasAsyncMqttClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <opp2.h>

class UDPIOHandler;

/**
 * @brief Protocol selection for canonical state source
 *
 * When in OPP2 mode: only OPP2 messages can update external state
 * (fencers/match) When in CYRANO mode: only Cyrano messages can update external
 * state Remote control commands are always accepted (conceptually part of
 * apparatus)
 */
enum class InputProtocol {
  NONE,   ///< Auto-detect mode: no protocol decided yet; first to send wins
  OPP2,
  CYRANO
};

/**
 * @brief OPP2 (OpenPiste Protocol Level 2) handler
 *
 * CANONICAL STATE HOLDER: This class contains the single source of truth
 * for all piste state (m_State). All other components read from this state.
 *
 * Thread Safety: m_State is protected by m_StateMutex for dual-core access.
 * - Core 0 (PRO_CPU): All protocol handlers, FSM, remote control
 * - Core 1 (APP_CPU): 3WeaponSensor (high-frequency hit detection)
 *
 * Observes FencingStateMachine events and publishes OPP2-formatted messages
 * to MQTT. Maintains complete piste state using OPP2::SystemState.
 *
 * Design:
 * - Uses shared AtlasAsyncMqttClient singleton (parallel with CyranoHandler)
 * - Publishes to openpiste/{piste_id}/apparatus/* topics
 * - Currently apparatus-to-software only (with placeholders for bidirectional)
 * - Sequence numbers managed per-message-type (QoS 1 messages only)
 */
class Opp2Handler : public Observer<FencingStateMachine>,
                    public Observer<UDPIOHandler>,
                    public Subject<Opp2Handler>,
                    public SingletonMixin<Opp2Handler> {
public:
  virtual ~Opp2Handler();

  /**
   * Initialize the handler: load preferences, setup MQTT topics,
   * configure Last Will and Testament, initialize state.
   */
  void Begin();
  void setFSM(FencingStateMachine *fsm) { m_pFSM = fsm; }

  /**
   * Observer pattern: receive events from FencingStateMachine.
   * Updates internal OPP2 state and publishes relevant messages.
   */
  void update(FencingStateMachine *subject, uint32_t eventtype) override;

  /**
   * Observer pattern: receive UI events from UDPIOHandler.
   */
  void update(UDPIOHandler *subject, uint32_t eventtype) override {
    ProcessUIEvents(eventtype);
  }

  /**
   * Update canonical state from Cyrano EFP1Message (DISP/INFO).
   * ZERO-COPY variant for UDP callback context - no string conversion.
   * Parses all fields: fencers, match, clock, scores, cards, priority, state.
   * Thread-safe with mutex protection.
   * @param msg EFP1Message by const reference (no copy)
   * @param outApparatusState Output parameter - receives the new apparatus state
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateFromCyranoMessage(const class EFP1Message &msg,
                               OPP2::ApparatusState &outApparatusState);

  /**
   * Called by CyranoHandler when Cyrano ACK is received.
   * Transitions apparatus state from ENDING → WAITING.
   */
  void ProcessCyranoACK();

  /**
   * Check MQTT connection status and reconnect if needed.
   * Should be called periodically from main loop.
   */
  void CheckConnection();

  /**
   * Set the piste identifier.
   */
  void SetPisteID(const char *pisteId);

  // ── Thread-Safe State Access ──────────────────────────────────────────

  /**
   * Get a thread-safe copy of the complete system state.
   * Uses mutex protection for dual-core safety.
   * @return Copy of OPP2::SystemState
   */
  OPP2::SystemState getStateCopy();

  /**
   * Get piste ID without copying entire state (stack-efficient).
   * Thread-safe with mutex protection.
   * @param buffer Output buffer (must be at least PISTE_ID_MAX bytes)
   */
  void getPisteId(char *buffer);

  // ── OPP2 to Cyrano Conversion ─────────────────────────────────────────

  /**
   * Convert OPP2::SystemState to Cyrano EFP1Message format.
   * This allows CyranoHandler to read from OPP2 canonical state.
   * Static method - no instance required.
   * @param state OPP2 system state to convert
   * @param pisteId Piste identifier string
   * @return EFP1Message in Cyrano format
   */
  static class EFP1Message convertOpp2ToCyrano(const OPP2::SystemState &state,
                                               const char *pisteId);

  /**
   * Convert Cyrano fencer fields to OPP2::Fencers.
   * Extracts fencer IDs, names, and nations from Cyrano message.
   * @param cyrano Cyrano EFP1Message
   * @return OPP2::Fencers struct
   */
  static OPP2::Fencers
  convertCyranoToOpp2Fencers(const class EFP1Message &cyrano);

  /**
   * Convert Cyrano match fields to OPP2::Match.
   * Extracts weapon and round number from Cyrano message.
   * @param cyrano Cyrano EFP1Message
   * @return OPP2::Match struct
   */
  static OPP2::Match convertCyranoToOpp2Match(const class EFP1Message &cyrano);

  /**
   * Convert Cyrano clock field to OPP2::Clock.
   * Parses MM:SS format and converts to milliseconds.
   * @param cyrano Cyrano EFP1Message
   * @return OPP2::Clock struct
   */
  static OPP2::Clock convertCyranoToOpp2Clock(const class EFP1Message &cyrano);

  // ── Internal State Updates (from FSM/Sensor - bypass guards) ─────────

  /**
   * Update lights from internal events (FSM/Sensor).
   * Bypasses all guards - internal events are authoritative.
   * Thread-safe with mutex protection.
   */
  void updateLightsInternal(const OPP2::Lights &lights);

  /**
   * Update score from internal events (FSM).
   * Bypasses all guards - internal events are authoritative.
   * Thread-safe with mutex protection.
   */
  void updateScoreInternal(const OPP2::Score &score);

  /**
   * Update clock from internal events (FSM).
   * Bypasses all guards - internal events are authoritative.
   * Thread-safe with mutex protection.
   */
  void updateClockInternal(const OPP2::Clock &clock);

  /**
   * Update apparatus state from internal events (FSM).
   * Bypasses all guards - internal events are authoritative.
   * Thread-safe with mutex protection.
   */
  void updateApparatusStateInternal(const OPP2::ApparatusStateMsg &state);

  /**
   * Update match configuration from internal events (FSM).
   * Bypasses all guards - internal events are authoritative.
   * Thread-safe with mutex protection.
   */
  void updateMatchInternal(const OPP2::Match &match);

  /**
   * Update UW2F (passivity timer) from internal events (FSM).
   * Bypasses all guards - internal events are authoritative.
   * Thread-safe with mutex protection.
   */
  void updateUW2FInternal(const OPP2::UW2F &uw2f);

  // ── External State Updates (from software - with guards) ─────────────

  /**
   * Update fencers from external source (software).
   * Only accepted when: active protocol matches AND apparatus in WAITING state.
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateFencersExternal(const OPP2::Fencers &fencers,
                             InputProtocol source);

  /**
   * Update match from external source (software).
   * Only accepted when: active protocol matches AND apparatus in WAITING state.
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateMatchExternal(const OPP2::Match &match, InputProtocol source);

  /**
   * Update clock from external source (software).
   * Only accepted when: active protocol matches AND clock not running.
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateClockExternal(const OPP2::Clock &clock, InputProtocol source);

  /**
   * Update score from external source (software).
   * Only accepted when: active protocol matches.
   * Can be updated anytime (e.g., match transfers, manual corrections).
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateScoreExternal(const OPP2::Score &score, InputProtocol source);

  /**
   * Update lights from external source (OPP2 or Cyrano).
   * Can be updated anytime (e.g., for testing, simulation).
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateLightsExternal(const OPP2::Lights &lights, InputProtocol source);

  /**
   * Update apparatus state from external source (OPP2 or Cyrano).
   * Can be updated anytime (software-driven START/STOP/RESET).
   * @return true if update was accepted, false if rejected by guards
   */
  bool
  updateApparatusStateExternal(const OPP2::ApparatusStateMsg &apparatusState,
                               InputProtocol source);

  /**
   * Update UW2F (blade contact warnings/penalty cards) from external source.
   * Can be updated anytime during matches.
   * @return true if update was accepted, false if rejected by guards
   */
  bool updateUW2FExternal(const OPP2::UW2F &uw2f, InputProtocol source);

  // ── MQTT Callbacks (static for C-style callback registration) ────────

  /**
   * MQTT connect callback - publishes online status and subscribes to topics.
   * Routes to both OPP2 and Cyrano (temporary for step 1).
   */
  static void OnMqttConnectStatic(bool sessionPresent);

  /**
   * MQTT disconnect callback.
   */
  static void OnMqttDisconnectStatic();

  /**
   * MQTT message callback - routes messages based on topic prefix.
   * openpiste/* → Opp2Handler::ProcessIncomingMessage
   */
  static void OnMqttMessageStatic(const char *topic, const char *payload,
                                  unsigned int length);

protected:
  friend class SingletonMixin<Opp2Handler>;
  Opp2Handler();

private:
  // ── State management ──────────────────────────────────────────────────

  FencingStateMachine *m_pFSM = nullptr;
  OPP2::SystemState
      m_State; ///< Complete OPP2 piste state (CANONICAL - protected by mutex)
  SemaphoreHandle_t m_StateMutex; ///< Mutex for thread-safe access to m_State
  OPP2::Dispatcher
      m_Dispatcher;      ///< OPP2 message dispatcher for incoming messages
  uint32_t m_SeqCounter; ///< Global sequence counter for all QoS 1 messages

  // Protocol selection (for external state updates only)
  InputProtocol
      m_ActiveInputProtocol; ///< Which protocol can update external state
  bool m_AutoDetectProtocol; ///< Auto-switch to OPP2 on first OPP2 message

  // Timing and throttling
  uint32_t m_NextPeriodicUpdate;
  uint32_t m_TimeToShowClock; ///< Throttle clock updates to ~1 Hz

  // ── Publishing ────────────────────────────────────────────────────────

  /**
   * Publish a lights message.
   */
  void PublishLights();

  /**
   * Publish a clock message.
   */
  void PublishClock();

  /**
   * Publish a score message.
   */
  void PublishScore();

  /**
   * Publish a fencers message (identity information).
   */
  void PublishFencers();

  /**
   * Publish an apparatus state message.
   */
  void PublishApparatusState();

  /**
   * Publish a connection message (online/offline).
   */
  void PublishConnection(bool online);

  /**   * Push cached Cyrano status to CyranoHandler (stack safety
   * optimization). Called after internal state updates to keep CyranoHandler
   * cache synchronized. Avoids mutex reads from UDP callback contexts.
   */
  void PushCachedStatusToCyrano();

  /**   * Publish a match message.
   */
  void PublishMatch();

  /**
   * Publish a UW2F message (passivity timer and P-cards).
   */
  void PublishUW2F();

  /**
   * Publish a blade_contact message (QoS 0, not retained).
   * Published on transition only — active=true on contact, false on release.
   */
  void PublishBladeContact(bool active);

  // ── Event processing ──────────────────────────────────────────────────

  /**
   * Process light change events from FencingStateMachine.
   */
  void ProcessLightsChange(uint32_t eventtype);

  /**
   * Process UI events (buttons, controls).
   */
  void ProcessUIEvents(uint32_t event);

  /**
   * Process incoming MQTT messages on OPP2 topics.
   * Parses topic, verifies piste_id, deserializes Control messages.
   */
  void ProcessIncomingMessage(const char *topic, const char *payload,
                              unsigned int length);

  /**
   * Process incoming OPP2 control messages from software/remote.
   * Handles ACK/NAK, video review, and remote control commands.
   */
  void ProcessIncomingControl(const OPP2::Control &msg);

  /**
   * Clear fencer and match identifying data on ACK (E→W).
   * Resets m_State.fencers and m_State.match, publishes cleared retained
   * messages to the broker, and notifies observers.
   */
  void ClearIdentifyingData();

  // ── Utility ───────────────────────────────────────────────────────────

  /**
   * Auto-detect helper for external update methods.
   * If auto-detect is enabled and no protocol is active yet, locks onto source.
   * Returns true if source is allowed to update, false if rejected.
   */
  bool isProtocolAllowed(InputProtocol source);

  /**
   * Generate next sequence number (QoS 1 messages only).
   */
  uint32_t NextSeq() { return ++m_SeqCounter; }

  /**
   * Build an OPP2 topic for the given message type.
   * Result written to provided buffer.
   */
  void BuildTopic(OPP2::MessageType msgType, char *topicBuf,
                  size_t topicBufSize);

  /**
   * Create an OPP2 timestamp using AbsoluteTime (NTP if available).
   * Returns NTP timestamp if synced, otherwise session timestamp.
   */
  OPP2::Timestamp CreateTimestamp();

  /**
   * Notify observers of state changes.
   */
  void StateChanged(uint32_t eventtype) { notify(eventtype); }

  // ── Equality Helpers (for change detection) ───────────────────────────

  /**
   * Compare two Lights messages for equality.
   */
  static bool lightsEqual(const OPP2::Lights &a, const OPP2::Lights &b);

  /**
   * Compare two Clock messages for equality.
   */
  static bool clockEqual(const OPP2::Clock &a, const OPP2::Clock &b);

  /**
   * Compare two Score messages for equality.
   */
  static bool scoreEqual(const OPP2::Score &a, const OPP2::Score &b);

  /**
   * Compare two Fencers messages for equality.
   */
  static bool fencersEqual(const OPP2::Fencers &a, const OPP2::Fencers &b);

  /**
   * Compare two Match messages for equality.
   */
  static bool matchEqual(const OPP2::Match &a, const OPP2::Match &b);

  /**
   * Compare two ApparatusStateMsg messages for equality.
   */
  static bool apparatusStateEqual(const OPP2::ApparatusStateMsg &a,
                                  const OPP2::ApparatusStateMsg &b);

  /**
   * Compare two UW2F messages for equality.
   */
  static bool uw2fEqual(const OPP2::UW2F &a, const OPP2::UW2F &b);

  // ── MQTT configuration ────────────────────────────────────────────────

  Preferences m_Preferences;

  bool m_bConnected;
  bool m_bWifiConnected;
  bool m_bConnectionAttempted; ///< Track if we've called mqttClient.begin()
  bool m_LastParryState = false; ///< Previous blade contact state for change detection
};

#endif // OPP2HANDLER_H
