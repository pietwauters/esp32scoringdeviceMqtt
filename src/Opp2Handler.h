#ifndef OPP2HANDLER_H
#define OPP2HANDLER_H

#include "CyranoHandler.h"
#include "EventDefinitions.h"
#include "FencingStateMachine.h"
#include "SubjectObserverTemplate.h"
#include <AtlasAsyncMqttClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <opp2.h>

class UDPIOHandler;

/**
 * @brief OPP2 (OpenPiste Protocol Level 2) handler
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
                    public Observer<CyranoHandler>,
                    public Subject<Opp2Handler>,
                    public SingletonMixin<Opp2Handler> {
public:
  virtual ~Opp2Handler();

  /**
   * Initialize the handler: load preferences, setup MQTT topics,
   * configure Last Will and Testament, initialize state.
   */
  void Begin();

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
   * Observer pattern: receive EFP1 string messages from CyranoHandler.
   */
  void update(CyranoHandler *subject,
              const std::string &strEFP1Message) override;

  /**
   * Observer pattern: receive event notifications from CyranoHandler.
   */
  void update(CyranoHandler *subject, uint32_t eventtype) override;

  /**
   * Check MQTT connection status and reconnect if needed.
   * Should be called periodically from main loop.
   */
  void CheckConnection();

  /**
   * Set the piste identifier.
   */
  void SetPisteID(const char *pisteId);

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
   * MQTT_Cyrano/* → CyranoHandler (temporary for step 1)
   */
  static void OnMqttMessageStatic(const char *topic, const char *payload,
                                  unsigned int length);

protected:
  friend class SingletonMixin<Opp2Handler>;
  Opp2Handler();

private:
  // ── State management ──────────────────────────────────────────────────

  OPP2::SystemState m_State; ///< Complete OPP2 piste state
  OPP2::Dispatcher
      m_Dispatcher;      ///< OPP2 message dispatcher for incoming messages
  uint32_t m_SeqCounter; ///< Global sequence counter for all QoS 1 messages

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

  /**
   * Publish a match message.
   */
  void PublishMatch();

  /**
   * Publish a UW2F message (passivity timer and P-cards).
   */
  void PublishUW2F();

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

  // ── Utility ───────────────────────────────────────────────────────────

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

  // ── MQTT configuration ────────────────────────────────────────────────

  Preferences m_Preferences;

  bool m_bConnected;
  bool m_bWifiConnected;
  bool m_bConnectionAttempted; ///< Track if we've called mqttClient.begin()
};

#endif // OPP2HANDLER_H
