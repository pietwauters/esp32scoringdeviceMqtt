#include "Opp2Handler.h"
#include "AbsoluteTime.h"
#include "CyranoConverter.h"
#include "CyranoHandler.h"
#include "EFP1Message.h"
#include "MDNSResolver.h"
#include <cstring>
#include <esp_log.h>

static const char *OPP2_TAG = "OPP2";
extern const char *mdnsName;             // Defined in CyranoHandler.cpp
extern char *mqttListenTopic;            // Cyrano listen topic (temporary)
extern char *mqttLastWillTopic;          // Cyrano LWT topic
extern AtlasAsyncMqttClient &mqttClient; // Shared MQTT client singleton

// ── Constructor / Destructor ────────────────────────────────────────────────

Opp2Handler::Opp2Handler()
    : m_SeqCounter(0), m_NextPeriodicUpdate(0), m_TimeToShowClock(0),
      m_bConnected(false), m_bWifiConnected(false),
      m_bConnectionAttempted(false) {
  // Initialize state with defaults
  strncpy(m_State.piste_id, "1", sizeof(m_State.piste_id) - 1);
  m_State.apparatus_state.state = OPP2::ApparatusState::WAITING;
  m_State.connection.online = false;
  m_State.clock.running = false;
  m_State.clock.time_ms = 0;
  m_State.score.priority = OPP2::Priority::NONE;
  // Match must have valid weapon/type/phase_type - UNKNOWN is not allowed by
  // OPP2 protocol
  m_State.match.weapon = OPP2::Weapon::FOIL;        // Default to FOIL
  m_State.match.type = OPP2::MatchType::INDIVIDUAL; // Default to INDIVIDUAL
  m_State.match.phase_type =
      OPP2::PhaseType::POOL; // Default to POOL (updated by EVENT_ROUND)
  m_State.match.round = 1;   // Default to round 1
}

Opp2Handler::~Opp2Handler() {
  // Publish offline status on graceful shutdown
  // Note: LWT handles ungraceful disconnects (power loss, crash)
  PublishConnection(false);
}

// ── Initialization ──────────────────────────────────────────────────────────

void Opp2Handler::Begin() {
  ESP_LOGI(OPP2_TAG, "[OPP2] Begin() called - taking MQTT ownership");

  m_Preferences.begin("credentials", false);
  uint32_t pisteNr = m_Preferences.getInt("pisteNr", 304);
  String pisteName = m_Preferences.getString("Pistename", "");
  String mqttBroker = m_Preferences.getString("MqttBroker", "10.154.1.130");
  m_Preferences.end();

  // Set piste ID
  if (pisteName != "") {
    strncpy(m_State.piste_id, pisteName.c_str(), sizeof(m_State.piste_id) - 1);
    ESP_LOGI(OPP2_TAG, "Using piste name from preferences: %s",
             m_State.piste_id);
  } else {
    snprintf(m_State.piste_id, sizeof(m_State.piste_id), "%u", pisteNr);
    ESP_LOGI(OPP2_TAG, "Using piste number: %u", pisteNr);
  }

  m_NextPeriodicUpdate = millis() + 10000;

  // ── Start NTP time service (now owned by Opp2Handler) ────────────────
  ESP_LOGI(OPP2_TAG, "[OPP2] Starting AbsoluteTime (NTP client)");
  AbsoluteTime::getInstance().begin(mdnsName, 10, mqttBroker.c_str());

  // ── Register MQTT callbacks (Opp2Handler becomes message router) ─────
  ESP_LOGI(OPP2_TAG, "[OPP2] Registering MQTT callbacks");
  mqttClient.onConnect(Opp2Handler::OnMqttConnectStatic);
  mqttClient.onDisconnect(Opp2Handler::OnMqttDisconnectStatic);
  mqttClient.onMessage(Opp2Handler::OnMqttMessageStatic);

  // ── Setup MQTT Last Will and Testament (LWT) ─────────────────────────
  // Create offline connection message
  OPP2::Connection offlineConn;
  offlineConn.seq = 0;
  offlineConn.online = false;
  offlineConn.device_present = true;
  strncpy(offlineConn.device, "ESP32-Scoring", sizeof(offlineConn.device) - 1);
  offlineConn.fw_version_present = true;
  strncpy(offlineConn.fw_version, "1.0.0", sizeof(offlineConn.fw_version) - 1);

  // Serialize to JSON
  char lwtPayload[256];
  OPP2::Serializer::serialize(offlineConn, lwtPayload, sizeof(lwtPayload));

  // Build LWT topic
  char lwtTopic[128];
  OPP2::TopicParser::buildFrom(m_State.piste_id, OPP2::Publisher::APPARATUS,
                               OPP2::MessageType::CONNECTION, lwtTopic,
                               sizeof(lwtTopic));

  // Set OPP2 LWT (primary protocol)
  mqttClient.setWill(lwtTopic, lwtPayload, 1, true);

  ESP_LOGI(OPP2_TAG, "[OPP2] Initialized for piste: %s", m_State.piste_id);
  ESP_LOGI(OPP2_TAG, "[OPP2] Topics: openpiste/%s/apparatus/*",
           m_State.piste_id);
  ESP_LOGI(OPP2_TAG, "[OPP2] LWT configured: %s", lwtTopic);

  // ── Setup OPP2 Message Dispatcher ─────────────────────────────────────
  // Register callbacks for incoming messages from software/remote

  m_Dispatcher.onControl = [this](const OPP2::Topic &topic,
                                  const OPP2::Control &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] Received Control command: %d",
             static_cast<int>(msg.command));
    ProcessIncomingControl(msg);
  };

  m_Dispatcher.onFencers = [this](const OPP2::Topic &topic,
                                  const OPP2::Fencers &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] ✓ Received COMPLETE Fencers message (seq=%u)",
             msg.seq);

    // Log left fencer details to prove full message was received
    if (msg.left.fencer.present) {
      ESP_LOGI(OPP2_TAG, "[OPP2]   LEFT: name='%s' id='%s'",
               msg.left.fencer.name, msg.left.fencer.id);
    }

    // Log right fencer details
    if (msg.right.fencer.present) {
      ESP_LOGI(OPP2_TAG, "[OPP2]   RIGHT: name='%s' id='%s'",
               msg.right.fencer.name, msg.right.fencer.id);
    }

    // Copy fencers data into our state
    m_State.fencers = msg;
    // TODO: Update local state and notify observers if needed
  };

  m_Dispatcher.onMatch = [this](const OPP2::Topic &topic,
                                const OPP2::Match &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] ✓ Received COMPLETE Match message (seq=%u)",
             msg.seq);
    ESP_LOGI(OPP2_TAG, "[OPP2]   Weapon=%d Type=%d Phase=%d Round=%u",
             static_cast<int>(msg.weapon), static_cast<int>(msg.type),
             static_cast<int>(msg.phase_type), msg.round);

    // Copy match data into our state
    m_State.match = msg;
    // TODO: Update local state and notify observers if needed
  };

  m_Dispatcher.onClock = [this](const OPP2::Topic &topic,
                                const OPP2::Clock &msg) {
    ESP_LOGI(
        OPP2_TAG, "[OPP2] Received Clock update from %s: time=%u running=%d",
        topic.publisher == OPP2::Publisher::SOFTWARE ? "software" : "remote",
        msg.time_ms, msg.running);
    // Copy clock data into our state
    m_State.clock = msg;
    // TODO: Sync with local timer if needed
  };

  m_Dispatcher.onScore = [this](const OPP2::Topic &topic,
                                const OPP2::Score &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] Received Score update from %s: L=%u R=%u",
             topic.publisher == OPP2::Publisher::SOFTWARE ? "software"
                                                          : "remote",
             msg.left.score, msg.right.score);
    // Copy score data into our state
    m_State.score = msg;
    // TODO: Update displays if needed
  };

  ESP_LOGI(OPP2_TAG, "[OPP2] Dispatcher callbacks registered");
}

// ── MQTT Callback Implementations ───────────────────────────────────────────

void Opp2Handler::OnMqttConnectStatic(bool sessionPresent) {
  ESP_LOGI(OPP2_TAG, "[OPP2] MQTT Connected (sessionPresent=%d)",
           sessionPresent);

  // Publish Cyrano online status (temporary for step 1)
  mqttClient.publish(mqttLastWillTopic, 1, true, "online");

  // Subscribe to Cyrano topic (temporary for step 1)
  mqttClient.subscribe(mqttListenTopic, 1);
  ESP_LOGI(OPP2_TAG, "[Cyrano] Subscribed to: %s", mqttListenTopic);

  // Subscribe to OPP2 control topics
  Opp2Handler &handler = Opp2Handler::getInstance();
  char topicBuf[64];

  ESP_LOGI(OPP2_TAG, "[OPP2] Current piste_id: '%s'", handler.m_State.piste_id);

  snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/software/#",
           handler.m_State.piste_id);
  mqttClient.subscribe(topicBuf, 1);
  ESP_LOGI(OPP2_TAG, "[OPP2] *** SUBSCRIBING TO: %s ***", topicBuf);

  snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/remote/#",
           handler.m_State.piste_id);
  mqttClient.subscribe(topicBuf, 1);
  ESP_LOGI(OPP2_TAG, "[OPP2] *** SUBSCRIBING TO: %s ***", topicBuf);
}

void Opp2Handler::OnMqttDisconnectStatic() {
  ESP_LOGW(OPP2_TAG, "[OPP2] MQTT Disconnected");
}

void Opp2Handler::OnMqttMessageStatic(const char *topic, const char *payload,
                                      unsigned int length) {
  ESP_LOGI(OPP2_TAG, "[MQTT] *** MESSAGE RECEIVED *** Topic: %s, Length: %u",
           topic, length);

  // Log payload (up to 256 chars for debugging, but full payload is processed)
  char payloadPreview[257];
  size_t previewLen = length < 256 ? length : 256;
  memcpy(payloadPreview, payload, previewLen);
  payloadPreview[previewLen] = '\0';
  ESP_LOGI(OPP2_TAG, "[MQTT] Full payload (%u bytes): %s%s", length,
           payloadPreview, length > 256 ? "..." : "");

  // Route based on topic prefix
  if (strncmp(topic, "openpiste/", 10) == 0) {
    // OPP2 protocol message
    ESP_LOGD(OPP2_TAG, "[OPP2] Routing to Opp2Handler");
    Opp2Handler::getInstance().ProcessIncomingMessage(topic, payload, length);
  } else if (strncmp(topic, "MQTT_Cyrano/", 12) == 0) {
    // Cyrano protocol message (temporary routing for step 1)
    ESP_LOGD(OPP2_TAG, "[Cyrano] Routing to CyranoHandler");
    std::string cyranoStr = convert_json_to_cyrano_string((char *)payload);
    if (cyranoStr != "") {
      CyranoHandler::getInstance().ProcessMessageFromSoftware(
          EFP1Message(cyranoStr), false);
    }
  } else {
    ESP_LOGW(OPP2_TAG, "[MQTT] Unknown topic prefix: %s", topic);
  }
}

void Opp2Handler::SetPisteID(const char *pisteId) {
  strncpy(m_State.piste_id, pisteId, sizeof(m_State.piste_id) - 1);
  m_State.piste_id[sizeof(m_State.piste_id) - 1] = '\0';
}

// ── Topic Management ────────────────────────────────────────────────────────

void Opp2Handler::BuildTopic(OPP2::MessageType msgType, char *topicBuf,
                             size_t topicBufSize) {
  OPP2::TopicParser::buildFrom(m_State.piste_id, OPP2::Publisher::APPARATUS,
                               msgType, topicBuf, topicBufSize);
}

OPP2::Timestamp Opp2Handler::CreateTimestamp() {
  AbsoluteTime &absTime = AbsoluteTime::getInstance();
  uint64_t timestamp = absTime.getTimestamp();

  // Check if we're synced to NTP to set correct ClockSource flag
  if (absTime.isSynced()) {
    // NTP synced - timestamp is epoch milliseconds
    return OPP2::Timestamp::fromEpochMs(timestamp);
  } else {
    // Fallback mode - timestamp is still valid, just not NTP synced
    return OPP2::Timestamp::fromMillis(static_cast<uint32_t>(timestamp));
  }
}

// ── Publishing Methods ──────────────────────────────────────────────────────

void Opp2Handler::PublishConnection(bool online) {
  if (!mqttClient.isConnected()) {
    ESP_LOGW(OPP2_TAG, "Cannot publish connection: MQTT not connected");
    return;
  }

  m_State.connection.seq = NextSeq();
  m_State.connection.online = online;

  if (online) {
    m_State.connection.device_present = true;
    strncpy(m_State.connection.device, "ESP32-Scoring",
            sizeof(m_State.connection.device) - 1);
    m_State.connection.fw_version_present = true;
    strncpy(m_State.connection.fw_version, "1.0.0",
            sizeof(m_State.connection.fw_version) - 1);
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(m_State.connection, payloadBuf,
                              sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::CONNECTION, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published connection %s to %s",
           online ? "online" : "offline", topicBuf);
  ESP_LOGD(OPP2_TAG, "Payload: %s", payloadBuf);
}

void Opp2Handler::PublishApparatusState() {
  if (!mqttClient.isConnected()) {
    ESP_LOGW(OPP2_TAG, "Cannot publish apparatus state: MQTT not connected");
    return;
  }

  m_State.apparatus_state.seq = NextSeq();

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(m_State.apparatus_state, payloadBuf,
                              sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::APPARATUS_STATE, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  const char *stateNames[] = {"FENCING", "HALT",   "PAUSE",
                              "WAITING", "ENDING", "UNKNOWN"};
  int stateIdx = static_cast<int>(m_State.apparatus_state.state);
  if (stateIdx < 0 || stateIdx > 5)
    stateIdx = 5;

  ESP_LOGI(OPP2_TAG, "Published state %s to %s", stateNames[stateIdx],
           topicBuf);
}

void Opp2Handler::PublishLights() {
  if (!mqttClient.isConnected()) {
    return;
  }

  m_State.lights.seq = NextSeq();
  m_State.lights.ts = CreateTimestamp();

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(m_State.lights, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::LIGHTS, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published lights L(%s,%s) R(%s,%s) to %s",
           m_State.lights.left.on_target ? "red" : "off",
           m_State.lights.left.white ? "white" : "off",
           m_State.lights.right.on_target ? "green" : "off",
           m_State.lights.right.white ? "white" : "off", topicBuf);
}

void Opp2Handler::PublishClock() {
  if (!mqttClient.isConnected()) {
    return;
  }

  // Clock is QoS 0, no sequence number
  m_State.clock.ts = CreateTimestamp();

  char payloadBuf[512];
  char topicBuf[64];

  OPP2::Serializer::serialize(m_State.clock, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::CLOCK, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 0, true, payloadBuf);

  ESP_LOGD(OPP2_TAG, "Published clock: %s %ums to %s",
           m_State.clock.running ? "running" : "stopped", m_State.clock.time_ms,
           topicBuf);
}

void Opp2Handler::PublishScore() {
  if (!mqttClient.isConnected()) {
    return;
  }

  m_State.score.seq = NextSeq();

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(m_State.score, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::SCORE, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published score L:%d R:%d to %s",
           m_State.score.left.score, m_State.score.right.score, topicBuf);
}

void Opp2Handler::PublishFencers() {
  if (!mqttClient.isConnected()) {
    return;
  }

  m_State.fencers.seq = NextSeq();

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(m_State.fencers, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::FENCERS, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published fencers L:%s R:%s to %s",
           m_State.fencers.left.fencer.name, m_State.fencers.right.fencer.name,
           topicBuf);
}

void Opp2Handler::PublishMatch() {
  if (!mqttClient.isConnected()) {
    ESP_LOGW(OPP2_TAG, "Cannot publish match: MQTT not connected");
    return;
  }

  ESP_LOGI(OPP2_TAG,
           "PublishMatch START: weapon=%d type=%d phase_type=%d round=%d",
           static_cast<int>(m_State.match.weapon),
           static_cast<int>(m_State.match.type),
           static_cast<int>(m_State.match.phase_type), m_State.match.round);

  char payloadBuf[512] = {0}; // Initialize to zeros
  char topicBuf[64] = {0};

  m_State.match.seq = NextSeq();

  ESP_LOGI(OPP2_TAG,
           "About to serialize Match: weapon=%d type=%d phase_type=%d",
           static_cast<int>(m_State.match.weapon),
           static_cast<int>(m_State.match.type),
           static_cast<int>(m_State.match.phase_type));

  OPP2::SerializeError err = OPP2::Serializer::serialize(
      m_State.match, payloadBuf, sizeof(payloadBuf));

  ESP_LOGI(OPP2_TAG, "Serialize returned: %d", static_cast<int>(err));

  if (err != OPP2::SerializeError::OK) {
    ESP_LOGE(OPP2_TAG,
             "Failed to serialize Match: error=%d (weapon=%d type=%d)",
             static_cast<int>(err), static_cast<int>(m_State.match.weapon),
             static_cast<int>(m_State.match.type));
    return;
  }

  BuildTopic(OPP2::MessageType::MATCH, topicBuf, sizeof(topicBuf));

  ESP_LOGI(OPP2_TAG, "About to publish to topic: %s", topicBuf);
  ESP_LOGI(OPP2_TAG, "Payload: %s", payloadBuf);

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  // OPP2::Weapon enum order: FOIL=0, EPEE=1, SABRE=2, UNKNOWN=3
  const char *weaponNames[] = {"FOIL", "EPEE", "SABRE", "UNKNOWN"};
  int weaponIdx = static_cast<int>(m_State.match.weapon);
  if (weaponIdx < 0 || weaponIdx > 3)
    weaponIdx = 3; // Default to UNKNOWN

  ESP_LOGI(OPP2_TAG, "Published match: weapon=%s round=%d to %s",
           weaponNames[weaponIdx], m_State.match.round, topicBuf);
}

void Opp2Handler::PublishUW2F() {
  if (!mqttClient.isConnected()) {
    return;
  }

  m_State.uw2f.seq = NextSeq();

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(m_State.uw2f, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::UW2F, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published UW2F: time=%ums L_P=%d R_P=%d to %s",
           m_State.uw2f.time_ms, m_State.uw2f.left.p_card,
           m_State.uw2f.right.p_card, topicBuf);
}

// ── Event Processing ────────────────────────────────────────────────────────

void Opp2Handler::ProcessLightsChange(uint32_t eventtype) {
  uint32_t event_data = eventtype & SUB_TYPE_MASK;

  // Update lights state
  m_State.lights.left.on_target = (event_data & MASK_RED) != 0;
  m_State.lights.right.on_target = (event_data & MASK_GREEN) != 0;
  m_State.lights.left.white = (event_data & MASK_WHITE_L) != 0;
  m_State.lights.right.white = (event_data & MASK_WHITE_R) != 0;

  PublishLights();
}

void Opp2Handler::ProcessUIEvents(uint32_t event) {
  // Handle UI button presses from UDPIOHandler
  // Future: map to OPP2 control commands if needed
  ESP_LOGD(OPP2_TAG, "ProcessUIEvents: 0x%08X", event);
}

// ── MQTT Message Processing ─────────────────────────────────────────────────

void Opp2Handler::ProcessIncomingMessage(const char *topic, const char *payload,
                                         unsigned int length) {
  ESP_LOGI(OPP2_TAG, "[OPP2] Received MQTT message on topic: %s (%u bytes)",
           topic, length);

  // Use the OPP2 Dispatcher to parse, deserialize, and route the message
  OPP2::DispatchResult result = m_Dispatcher.dispatch(topic, payload, length);

  switch (result) {
  case OPP2::DispatchResult::OK:
    ESP_LOGD(OPP2_TAG, "[OPP2] Message dispatched successfully");
    break;
  case OPP2::DispatchResult::UNKNOWN_TOPIC:
    ESP_LOGW(OPP2_TAG, "[OPP2] Unknown topic format: %s", topic);
    break;
  case OPP2::DispatchResult::DESERIALIZE_ERROR:
    ESP_LOGE(OPP2_TAG, "[OPP2] Failed to deserialize payload: error=%d",
             static_cast<int>(m_Dispatcher.lastDeserializeError()));
    ESP_LOGE(OPP2_TAG, "[OPP2] Payload: %.*s", length, payload);
    break;
  case OPP2::DispatchResult::NO_CALLBACK:
    ESP_LOGD(OPP2_TAG, "[OPP2] No callback registered for this message type");
    break;
  case OPP2::DispatchResult::UNKNOWN_TYPE:
    ESP_LOGD(OPP2_TAG, "[OPP2] Unknown message type (future version?)");
    break;
  default:
    ESP_LOGW(OPP2_TAG, "[OPP2] Unexpected dispatch result: %d",
             static_cast<int>(result));
    break;
  }
}

void Opp2Handler::ProcessIncomingControl(const OPP2::Control &msg) {
  ESP_LOGI(OPP2_TAG, "[OPP2] ProcessIncomingControl: command=%d",
           static_cast<int>(msg.command));

  // Handle control commands similar to CyranoHandler's DISP/ACK/NAK handling
  switch (msg.command) {
  case OPP2::Command::ACK:
    // Software acknowledged - equivalent to Cyrano DISP or ACK
    // Unlocks the state machine to allow fencing
    ESP_LOGI(OPP2_TAG, "[OPP2] ACK received - notifying state machine");
    notify(EVENT_CYRANO_STATE_W);
    break;

  case OPP2::Command::NAK:
    // Software rejected - equivalent to Cyrano NAK
    // Puts machine in error/halt state
    ESP_LOGE(OPP2_TAG, "[OPP2] NAK received - notifying state machine");
    notify(EVENT_CYRANO_STATE_NAK);
    break;

  case OPP2::Command::BEGIN:
    // Remote control: start bout
    ESP_LOGI(OPP2_TAG, "[OPP2] BEGIN command (remote control)");
    // TODO: Map to appropriate FencingStateMachine event
    // notify(EVENT_UI_INPUT | UI_INPUT_BEGIN);
    break;

  case OPP2::Command::HALT:
    // Remote control: halt bout
    ESP_LOGI(OPP2_TAG, "[OPP2] HALT command (remote control)");
    // TODO: Map to appropriate FencingStateMachine event
    // notify(EVENT_UI_INPUT | UI_INPUT_HALT);
    break;

  case OPP2::Command::RESET:
    // Remote control: reset machine
    ESP_LOGI(OPP2_TAG, "[OPP2] RESET command (remote control)");
    // TODO: Map to appropriate FencingStateMachine event
    // notify(EVENT_UI_INPUT | UI_INPUT_RESET);
    break;

  case OPP2::Command::VALIDATE:
    // Remote control: validate/confirm hit
    ESP_LOGI(OPP2_TAG, "[OPP2] VALIDATE command (remote control)");
    // TODO: Implement if needed for remote hit validation
    break;

  case OPP2::Command::VIDEO_REVIEW_GRANTED:
    ESP_LOGI(OPP2_TAG, "[OPP2] Video review GRANTED");
    // TODO: Update state machine or display
    break;

  case OPP2::Command::VIDEO_REVIEW_DENIED:
    ESP_LOGI(OPP2_TAG, "[OPP2] Video review DENIED");
    // TODO: Update state machine or display
    break;

  default:
    ESP_LOGW(OPP2_TAG, "[OPP2] Unhandled control command: %d",
             static_cast<int>(msg.command));
    break;
  }
}

void Opp2Handler::update(CyranoHandler *subject, uint32_t eventtype) {

  // Handle event-based notifications from CyranoHandler
  // Currently not used - CyranoHandler mainly sends string messages
  ESP_LOGD(OPP2_TAG, "[Cyrano→OPP2] Event: 0x%08X", eventtype);
}

void Opp2Handler::update(FencingStateMachine *subject, uint32_t eventtype) {

  uint32_t event_data = eventtype & SUB_TYPE_MASK;
  uint32_t maineventtype = eventtype & MAIN_TYPE_MASK;
  bool bTransmit = true;

  ESP_LOGD(OPP2_TAG, "Received event: 0x%08X (main: 0x%08X, data: 0x%08X)",
           eventtype, maineventtype, event_data);

  switch (maineventtype) {

  case EVENT_LIGHTS:
    ProcessLightsChange(eventtype);
    bTransmit = false; // Already published in ProcessLightsChange
    break;

  case EVENT_WEAPON:
    ESP_LOGI(OPP2_TAG, "EVENT_WEAPON received: event_data=0x%08X", event_data);
    ESP_LOGI(OPP2_TAG, "Weapon BEFORE update: %d",
             static_cast<int>(m_State.match.weapon));
    switch (event_data) {
    case WEAPON_MASK_EPEE:
      m_State.match.weapon = OPP2::Weapon::EPEE;
      break;
    case WEAPON_MASK_SABRE:
      m_State.match.weapon = OPP2::Weapon::SABRE;
      break;
    case WEAPON_MASK_FOIL:
      m_State.match.weapon = OPP2::Weapon::FOIL;
      break;
    default:
      // Unknown weapon mask - keep current valid weapon, don't change to
      // UNKNOWN
      ESP_LOGW(OPP2_TAG, "Unknown weapon mask: 0x%08X - keeping current weapon",
               event_data);
      bTransmit = false; // Don't publish for unknown weapon
      break;
    }
    if (bTransmit) {
      ESP_LOGI(OPP2_TAG, "Weapon AFTER update: %d",
               static_cast<int>(m_State.match.weapon));
      PublishMatch();
    }
    bTransmit = false;
    break;

  case EVENT_SCORE_LEFT:
    m_State.score.left.score = event_data;
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_SCORE_RIGHT:
    m_State.score.right.score = event_data;
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_TIMER_STATE:
    // Update running state
    if (eventtype & DATA_24BIT_MASK) {
      m_State.clock.running = true;
      // Don't change apparatus state if already in ENDING or WAITING
      if (m_State.apparatus_state.state != OPP2::ApparatusState::ENDING &&
          m_State.apparatus_state.state != OPP2::ApparatusState::WAITING) {
        m_State.apparatus_state.state = OPP2::ApparatusState::FENCING;
        PublishApparatusState();
      }
    } else {
      m_State.clock.running = false;
      if (m_State.apparatus_state.state != OPP2::ApparatusState::ENDING &&
          m_State.apparatus_state.state != OPP2::ApparatusState::WAITING) {
        m_State.apparatus_state.state = OPP2::ApparatusState::HALT;
        PublishApparatusState();
      }
    }
    PublishClock();
    bTransmit = false;
    break;

  case EVENT_TIMER: {
    // Throttle clock updates to approximately 1 Hz
    uint32_t now = millis();
    if (now < m_TimeToShowClock) {
      bTransmit = false;
      break;
    }

    // Always publish transition to zero
    if (!event_data) {
      bTransmit = true;
    } else {
      bTransmit = true;
      if (m_TimeToShowClock > 0 && (now - m_TimeToShowClock) < 1000) {
        m_TimeToShowClock += 1000;
      } else {
        m_TimeToShowClock = now + 900;
      }
    }

    // Extract time components
    mix_t TimeInfo;
    TimeInfo.theDWord = eventtype & DATA_24BIT_MASK;

    // Convert to milliseconds
    // TimeInfo.theBytes[0] = centiseconds
    // TimeInfo.theBytes[1] = seconds
    // TimeInfo.theBytes[2] = minutes
    uint32_t minutes = TimeInfo.theBytes[2];
    uint32_t seconds = TimeInfo.theBytes[1];
    uint32_t centiseconds = TimeInfo.theBytes[0];

    m_State.clock.time_ms =
        (minutes * 60000) + (seconds * 1000) + (centiseconds * 10);

    if (bTransmit) {
      PublishClock();
    }
    bTransmit = false;
    break;
  }

  case EVENT_ROUND: {
    uint8_t currentRound = event_data & DATA_BYTE0_MASK;
    uint8_t nrOfRounds = (event_data & DATA_BYTE1_MASK) >> 8;

    m_State.match.round = currentRound;

    // Derive phase_type and type from nrOfRounds
    if (nrOfRounds == 1) {
      // Pools: 1 round
      m_State.match.phase_type = OPP2::PhaseType::POOL;
      m_State.match.type = OPP2::MatchType::INDIVIDUAL;
    } else if (nrOfRounds == 2 || nrOfRounds == 3) {
      // Direct Elimination: 2 or 3 rounds
      m_State.match.phase_type = OPP2::PhaseType::DE;
      m_State.match.type = OPP2::MatchType::INDIVIDUAL;
    } else if (nrOfRounds == 9) {
      // Team match: 9 rounds
      m_State.match.phase_type =
          OPP2::PhaseType::DE; // Teams are typically DE format
      m_State.match.type = OPP2::MatchType::TEAM;
    }

    ESP_LOGI(OPP2_TAG, "EVENT_ROUND: round=%d/%d, phase_type=%d, type=%d",
             currentRound, nrOfRounds,
             static_cast<int>(m_State.match.phase_type),
             static_cast<int>(m_State.match.type));

    PublishMatch();
    bTransmit = false;
    break;
  }

  case EVENT_YELLOW_CARD_LEFT:
    m_State.score.left.yellow_card = (event_data > 0);
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_YELLOW_CARD_RIGHT:
    m_State.score.right.yellow_card = (event_data > 0);
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_RED_CARD_LEFT:
    m_State.score.left.red_cards = event_data;
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_RED_CARD_RIGHT:
    m_State.score.right.red_cards = event_data;
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_BLACK_CARD_LEFT:
    m_State.score.left.black_card = (event_data != 0);
    if (event_data) {
      m_State.score.right.status = OPP2::FencerStatus::EXCLUSION;
    } else {
      m_State.score.right.status = OPP2::FencerStatus::UNDEFINED;
    }
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_BLACK_CARD_RIGHT:
    m_State.score.right.black_card = (event_data != 0);
    if (event_data) {
      m_State.score.left.status = OPP2::FencerStatus::EXCLUSION;
    } else {
      m_State.score.left.status = OPP2::FencerStatus::UNDEFINED;
    }
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_P_CARD: {
    mix_t PCardInfo;
    PCardInfo.theDWord = eventtype & DATA_24BIT_MASK;
    m_State.uw2f.right.p_card = PCardInfo.theBytes[1];
    m_State.uw2f.left.p_card = PCardInfo.theBytes[0];
    PublishUW2F();
    bTransmit = false;
    break;
  }

  case EVENT_PRIO:
    switch (event_data) {
    case 2:
      m_State.score.priority = OPP2::Priority::RIGHT;
      break;
    case 1:
      m_State.score.priority = OPP2::Priority::LEFT;
      break;
    default:
      m_State.score.priority = OPP2::Priority::NONE;
    }
    PublishScore();
    bTransmit = false;
    break;

  case EVENT_UW2F_TIMER: {
    // Extract UW2F timer value
    mix_t TimeInfo;
    TimeInfo.theDWord = eventtype & DATA_24BIT_MASK;
    uint32_t minutes = TimeInfo.theBytes[2];
    uint32_t seconds = TimeInfo.theBytes[1];
    uint32_t centiseconds = TimeInfo.theBytes[0];

    m_State.uw2f.time_ms =
        (minutes * 60000) + (seconds * 1000) + (centiseconds * 10);
    PublishUW2F();
    bTransmit = false;
    break;
  }

  default:
    bTransmit = false;
  }

  // For any unhandled events that set bTransmit = true, we would publish here
  // (Currently all events handle their own publishing)
}

// ── Connection Management ───────────────────────────────────────────────────

void Opp2Handler::CheckConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    if (m_bWifiConnected) {
      ESP_LOGW(OPP2_TAG, "[OPP2] WiFi disconnected");
      m_bWifiConnected = false;
      m_bConnected = false;
      m_bConnectionAttempted = false;
    }
    return;
  }

  if (!m_bWifiConnected) {
    ESP_LOGI(OPP2_TAG, "[OPP2] WiFi connected");
    m_bWifiConnected = true;
  }

  // ── Start MQTT connection (now owned by Opp2Handler) ─────────────────
  if (!mqttClient.isConnected() && !m_bConnectionAttempted) {
    ESP_LOGI(OPP2_TAG, "[OPP2] *** Starting MQTT connection to broker ***");
    ESP_LOGI(OPP2_TAG,
             "[OPP2] *** Will subscribe to openpiste/%s/software and "
             "openpiste/%s/remote ***",
             m_State.piste_id, m_State.piste_id);
    mqttClient.begin();
    m_bConnectionAttempted = true;
  }

  if (mqttClient.isConnected() && !m_bConnected) {
    // Just connected - publish initial state
    m_bConnected = true;

    ESP_LOGI(OPP2_TAG,
             "[OPP2] MQTT connected! Publishing initial state for piste: %s",
             m_State.piste_id);

    PublishConnection(true);
    PublishApparatusState();
    PublishScore();
    PublishLights();
    ESP_LOGI(OPP2_TAG, "[OPP2] About to publish initial Match state");
    PublishMatch();

    ESP_LOGI(OPP2_TAG, "[OPP2] Initial state published");
  } else if (!mqttClient.isConnected() && m_bConnected) {
    // Lost connection
    ESP_LOGW(OPP2_TAG, "[OPP2] MQTT connection lost");
    m_bConnected = false;
    m_bConnectionAttempted = false;
  } else if (!mqttClient.isConnected() && !m_bConnected) {
    // Still not connected
    static uint32_t lastLogTime = 0;
    if (millis() - lastLogTime > 5000) {
      ESP_LOGD(OPP2_TAG, "[OPP2] Waiting for MQTT connection...");
      lastLogTime = millis();
    }
  }
}

void Opp2Handler::update(CyranoHandler *subject,
                         const std::string &strEFP1Message) {

  EFP1Message EFP1Input(strEFP1Message);

  ESP_LOGI(OPP2_TAG, "[Cyrano→OPP2] Received command: %s",
           EFP1Input[Command].c_str());

  // ── Handle DISP/INFO commands (match setup) ────────────────────────
  if (EFP1Input[Command] == "DISP" || EFP1Input[Command] == "INFO") {
    bool fencersChanged = false;
    bool scoreChanged = false;
    bool matchChanged = false;
    bool apparatusStateChanged = false;
    bool lightsChanged = false;
    bool uw2fChanged = false;

    // ── Fencers information (goes to FENCERS message, not SCORE) ──

    // Right fencer
    if (EFP1Input[RightFencerId] != "") {
      strncpy(m_State.fencers.right.fencer.id, EFP1Input[RightFencerId].c_str(),
              sizeof(m_State.fencers.right.fencer.id) - 1);
      m_State.fencers.right.fencer.present = true;
      fencersChanged = true;
    }
    if (EFP1Input[RightFencerName] != "") {
      strncpy(m_State.fencers.right.fencer.name,
              EFP1Input[RightFencerName].c_str(),
              sizeof(m_State.fencers.right.fencer.name) - 1);
      m_State.fencers.right.fencer.present = true;
      fencersChanged = true;
    }
    if (EFP1Input[RightFencerNation] != "") {
      strncpy(m_State.fencers.right.fencer.nation,
              EFP1Input[RightFencerNation].c_str(),
              sizeof(m_State.fencers.right.fencer.nation) - 1);
      m_State.fencers.right.fencer.present = true;
      fencersChanged = true;
    }

    // Left fencer
    if (EFP1Input[LeftFencerId] != "") {
      strncpy(m_State.fencers.left.fencer.id, EFP1Input[LeftFencerId].c_str(),
              sizeof(m_State.fencers.left.fencer.id) - 1);
      m_State.fencers.left.fencer.present = true;
      fencersChanged = true;
    }
    if (EFP1Input[LeftFencerName] != "") {
      strncpy(m_State.fencers.left.fencer.name,
              EFP1Input[LeftFencerName].c_str(),
              sizeof(m_State.fencers.left.fencer.name) - 1);
      m_State.fencers.left.fencer.present = true;
      fencersChanged = true;
    }
    if (EFP1Input[LeftFencerNation] != "") {
      strncpy(m_State.fencers.left.fencer.nation,
              EFP1Input[LeftFencerNation].c_str(),
              sizeof(m_State.fencers.left.fencer.nation) - 1);
      m_State.fencers.left.fencer.present = true;
      fencersChanged = true;
    }

    // ── Scores (goes to SCORE message) ────────────────────────────

    if (EFP1Input[RightScore] != "") {
      m_State.score.right.score = std::atoi(EFP1Input[RightScore].c_str());
      scoreChanged = true;
    }
    if (EFP1Input[LeftScore] != "") {
      m_State.score.left.score = std::atoi(EFP1Input[LeftScore].c_str());
      scoreChanged = true;
    }

    // ── Cards ──────────────────────────────────────────────────────

    if (EFP1Input[RightYCard] != "") {
      m_State.score.right.yellow_card = (EFP1Input[RightYCard] == "1");
      scoreChanged = true;
    }
    if (EFP1Input[LeftYCard] != "") {
      m_State.score.left.yellow_card = (EFP1Input[LeftYCard] == "1");
      scoreChanged = true;
    }
    if (EFP1Input[RightRCard] != "") {
      m_State.score.right.red_cards = std::atoi(EFP1Input[RightRCard].c_str());
      scoreChanged = true;
    }
    if (EFP1Input[LeftRCard] != "") {
      m_State.score.left.red_cards = std::atoi(EFP1Input[LeftRCard].c_str());
      scoreChanged = true;
    }

    // ── P-Cards (UW2F) ─────────────────────────────────────────────

    if (EFP1Input[RightPCards] != "") {
      m_State.uw2f.right.p_card = std::atoi(EFP1Input[RightPCards].c_str());
      uw2fChanged = true;
    }
    if (EFP1Input[LeftPCards] != "") {
      m_State.uw2f.left.p_card = std::atoi(EFP1Input[LeftPCards].c_str());
      uw2fChanged = true;
    }

    // ── Priority ───────────────────────────────────────────────────

    if (EFP1Input[Priority] != "") {
      if (EFP1Input[Priority] == "R") {
        m_State.score.priority = OPP2::Priority::RIGHT;
      } else if (EFP1Input[Priority] == "L") {
        m_State.score.priority = OPP2::Priority::LEFT;
      } else {
        m_State.score.priority = OPP2::Priority::NONE;
      }
      scoreChanged = true;
    }

    // ── Weapon ─────────────────────────────────────────────────────

    if (EFP1Input[Weapon] != "") {
      if (EFP1Input[Weapon] == "E") {
        m_State.match.weapon = OPP2::Weapon::EPEE;
      } else if (EFP1Input[Weapon] == "S") {
        m_State.match.weapon = OPP2::Weapon::SABRE;
      } else if (EFP1Input[Weapon] == "F") {
        m_State.match.weapon = OPP2::Weapon::FOIL;
      }
      matchChanged = true;
    }

    // ── Round number ───────────────────────────────────────────────

    if (EFP1Input[RoundNumber] != "") {
      m_State.match.round = std::atoi(EFP1Input[RoundNumber].c_str());
      matchChanged = true;
    }

    // ── State (apparatus state) ────────────────────────────────────

    if (EFP1Input[State] != "") {
      // Cyrano state: W=WAITING, F=FENCING, H=HALT, P=PAUSE, E=ENDING
      if (EFP1Input[State] == "W") {
        m_State.apparatus_state.state = OPP2::ApparatusState::WAITING;
      } else if (EFP1Input[State] == "F") {
        m_State.apparatus_state.state = OPP2::ApparatusState::FENCING;
      } else if (EFP1Input[State] == "H") {
        m_State.apparatus_state.state = OPP2::ApparatusState::HALT;
      } else if (EFP1Input[State] == "P") {
        m_State.apparatus_state.state = OPP2::ApparatusState::PAUSE;
      } else if (EFP1Input[State] == "E") {
        m_State.apparatus_state.state = OPP2::ApparatusState::ENDING;
      }
      apparatusStateChanged = true;
    }

    // ── Lights (if present in INFO messages) ──────────────────────

    if (EFP1Input[RightLight] != "") {
      m_State.lights.right.on_target = (EFP1Input[RightLight] == "1");
      lightsChanged = true;
    }
    if (EFP1Input[LeftLight] != "") {
      m_State.lights.left.on_target = (EFP1Input[LeftLight] == "1");
      lightsChanged = true;
    }
    if (EFP1Input[RightWhiteLight] != "") {
      m_State.lights.right.white = (EFP1Input[RightWhiteLight] == "1");
      lightsChanged = true;
    }
    if (EFP1Input[LeftWhiteLight] != "") {
      m_State.lights.left.white = (EFP1Input[LeftWhiteLight] == "1");
      lightsChanged = true;
    }

    // ── Publish updated state to OPP2 MQTT topics ─────────────────

    if (fencersChanged || scoreChanged || matchChanged ||
        apparatusStateChanged || lightsChanged || uw2fChanged) {
      ESP_LOGI(OPP2_TAG, "[Cyrano→OPP2] Publishing updated state from %s",
               EFP1Input[Command].c_str());

      if (fencersChanged)
        PublishFencers();
      if (scoreChanged)
        PublishScore();
      if (matchChanged)
        PublishMatch();
      if (apparatusStateChanged)
        PublishApparatusState();
      if (lightsChanged)
        PublishLights();
      if (uw2fChanged)
        PublishUW2F();
    }
  }
  /*
    // ── Handle ACK command ──────────────────────────────────────────────
    else if (EFP1Input[Command] == "ACK") {
      ESP_LOGI(OPP2_TAG, "[Cyrano→OPP2] ACK received");
      // Already handled by ProcessIncomingControl for OPP2::Command::ACK
      // No action needed here - Cyrano ACK is informational
    }

    // ── Handle NAK command ──────────────────────────────────────────────
    else if (EFP1Input[Command] == "NAK") {
      ESP_LOGE(OPP2_TAG, "[Cyrano→OPP2] NAK received - error state");
      m_State.apparatus_state.state = OPP2::ApparatusState::HALT;
      PublishApparatusState();
    }

    // ── Handle HELLO command ────────────────────────────────────────────
    else if (EFP1Input[Command] == "HELLO") {
      ESP_LOGI(OPP2_TAG, "[Cyrano→OPP2] HELLO received - software connected");
      // No specific OPP2 action needed
    }
      */
}