#include "Opp2Handler.h"
#include "AbsoluteTime.h"
#include "CyranoHandler.h"
#include "EFP1Message.h"
#include "MDNSResolver.h"
#include <cstring>
#include <esp_log.h>

static const char *OPP2_TAG = "OPP2";
extern const char *mdnsName;             // Defined in CyranoHandler.cpp
extern AtlasAsyncMqttClient &mqttClient; // Shared MQTT client singleton

// Boot recovery state — used by CheckConnection() and OnMqttMessageStatic()
// to intercept the apparatus's own retained topics on the first MQTT connect
// and restore m_State from broker before publishing anything.
static bool     s_bFirstConnect       = true;   // false after first connect completes
static bool     s_bBootRecoveryActive = false;  // true during the 300ms recovery window
static uint32_t s_BootRecoveryStartMs = 0;      // millis() when the window opened

// ── Constructor / Destructor ────────────────────────────────────────────────

Opp2Handler::Opp2Handler()
    : m_SeqCounter(0), m_NextPeriodicUpdate(0), m_TimeToShowClock(0),
      m_bConnected(false), m_bWifiConnected(false),
      m_bConnectionAttempted(false), m_StateMutex(nullptr),
      m_ActiveInputProtocol(InputProtocol::NONE), m_AutoDetectProtocol(true) {
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

  // Clean up mutex
  if (m_StateMutex != nullptr) {
    vSemaphoreDelete(m_StateMutex);
    m_StateMutex = nullptr;
  }
}

// ── Initialization ──────────────────────────────────────────────────────────

void Opp2Handler::Begin() {
  ESP_LOGI(OPP2_TAG, "[OPP2] Begin() called - taking MQTT ownership");

  // ── Create mutex for thread-safe state access ────────────────────────
  m_StateMutex = xSemaphoreCreateRecursiveMutex();
  if (m_StateMutex == nullptr) {
    ESP_LOGE(OPP2_TAG, "[OPP2] FATAL: Failed to create state mutex!");
    return;
  }
  ESP_LOGI(OPP2_TAG, "[OPP2] State mutex created successfully");

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
    ESP_LOGI(OPP2_TAG, "[OPP2] Received Control command from %s: %d",
             topic.publisher == OPP2::Publisher::SOFTWARE ? "software"
                                                          : "remote",
             static_cast<int>(msg.command));
    // Control commands (ACK/NAK/BEGIN/HALT/RESET) trigger actions, not state
    // updates They're allowed from OPP2 regardless of active input protocol
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

    // Route through external update with protocol tracking
    updateFencersExternal(msg, InputProtocol::OPP2);
  };

  m_Dispatcher.onMatch = [this](const OPP2::Topic &topic,
                                const OPP2::Match &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] ✓ Received COMPLETE Match message (seq=%u)",
             msg.seq);
    ESP_LOGI(OPP2_TAG, "[OPP2]   Weapon=%d Type=%d Phase=%d Round=%u",
             static_cast<int>(msg.weapon), static_cast<int>(msg.type),
             static_cast<int>(msg.phase_type), msg.round);

    // Route through external update with protocol tracking
    updateMatchExternal(msg, InputProtocol::OPP2);
  };

  m_Dispatcher.onClock = [this](const OPP2::Topic &topic,
                                const OPP2::Clock &msg) {
    ESP_LOGI(
        OPP2_TAG, "[OPP2] Received Clock update from %s: time=%u running=%d",
        topic.publisher == OPP2::Publisher::SOFTWARE ? "software" : "remote",
        msg.time_ms, msg.running);
    // Route through external update with protocol tracking
    updateClockExternal(msg, InputProtocol::OPP2);
  };

  m_Dispatcher.onScore = [this](const OPP2::Topic &topic,
                                const OPP2::Score &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] Received Score update from %s: L=%u R=%u",
             topic.publisher == OPP2::Publisher::SOFTWARE ? "software"
                                                          : "remote",
             msg.left.score, msg.right.score);
    // Route through external update with protocol tracking
    updateScoreExternal(msg, InputProtocol::OPP2);
  };

  m_Dispatcher.onLights = [this](const OPP2::Topic &topic,
                                 const OPP2::Lights &msg) {
    ESP_LOGI(OPP2_TAG, "[OPP2] Received Lights update from %s",
             topic.publisher == OPP2::Publisher::SOFTWARE ? "software"
                                                          : "remote");
    // Route through external update with protocol tracking
    updateLightsExternal(msg, InputProtocol::OPP2);
  };

  m_Dispatcher.onApparatusState = [this](const OPP2::Topic &topic,
                                         const OPP2::ApparatusStateMsg &msg) {
    ESP_LOGI(
        OPP2_TAG, "[OPP2] Received ApparatusState update from %s: state=%d",
        topic.publisher == OPP2::Publisher::SOFTWARE ? "software" : "remote",
        static_cast<int>(msg.state));
    // Route through external update with protocol tracking
    updateApparatusStateExternal(msg, InputProtocol::OPP2);
  };

  m_Dispatcher.onUW2F = [this](const OPP2::Topic &topic,
                               const OPP2::UW2F &msg) {
    ESP_LOGI(OPP2_TAG,
             "[OPP2] Received UW2F update from %s: time=%ums L_P=%d R_P=%d",
             topic.publisher == OPP2::Publisher::SOFTWARE ? "software"
                                                          : "remote",
             msg.time_ms, msg.left.p_card, msg.right.p_card);
    // Route through external update with protocol tracking
    updateUW2FExternal(msg, InputProtocol::OPP2);
  };

  ESP_LOGI(OPP2_TAG, "[OPP2] Dispatcher callbacks registered");

  // ── Initialize CyranoHandler cache and notify observers of boot state ──
  PushCachedStatusToCyrano();
  notify(EVENT_CYRANO_STATE_W); // Broadcast WAITING to FPA422Handler and others
}

// ── MQTT Callback Implementations ───────────────────────────────────────────

void Opp2Handler::OnMqttConnectStatic(bool sessionPresent) {
  ESP_LOGI(OPP2_TAG, "[OPP2] MQTT Connected (sessionPresent=%d)",
           sessionPresent);

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

  // Do NOT publish here — boot recovery (CheckConnection) will restore state
  // from retained broker topics first, then publish once the window closes.
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

    // Apparatus topics are our own publishes — never process as incoming events.
    // Exception: during boot recovery, intercept them to restore m_State from
    // retained broker data before we overwrite with boot-state zeros.
    if (strstr(topic, "/apparatus/") != nullptr) {
      if (s_bBootRecoveryActive) {
        Opp2Handler::getInstance().ProcessBootRecovery(topic, payload, length);
      }
      // After recovery (or if not from apparatus publisher) — drop silently.
      return;
    }

    // Level 1: raw EFP1.1 from software over MQTT
    if (strstr(topic, "/software/efp1") != nullptr) {
      ESP_LOGD(OPP2_TAG, "[L1] Routing software/efp1 to CyranoHandler");
      CyranoHandler::getInstance().ProcessMessageFromSoftware(
          EFP1Message((char *)payload), false);
    } else {
      // OPP2 protocol message
      ESP_LOGD(OPP2_TAG, "[OPP2] Routing to Opp2Handler");
      Opp2Handler::getInstance().ProcessIncomingMessage(topic, payload, length);
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

bool Opp2Handler::isProtocolAllowed(InputProtocol source) {
  // Hard override (m_AutoDetectProtocol=false): locked to whatever was set at boot.
  if (!m_AutoDetectProtocol) {
    return (m_ActiveInputProtocol == source);
  }

  // Cyrano-priority auto-detect: Cyrano can always take over from NONE or OPP2.
  if (source == InputProtocol::CYRANO) {
    if (m_ActiveInputProtocol != InputProtocol::CYRANO) {
      ESP_LOGI(OPP2_TAG, "[Protocol] Cyrano detected: locking to Cyrano (was %s)",
               m_ActiveInputProtocol == InputProtocol::NONE ? "NONE" : "OPP2");
      m_ActiveInputProtocol = InputProtocol::CYRANO;
    }
    return true;
  }

  // OPP2: provisional — accepted only if Cyrano has not yet been detected.
  if (source == InputProtocol::OPP2) {
    if (m_ActiveInputProtocol == InputProtocol::CYRANO) {
      ESP_LOGV(OPP2_TAG, "[Guard] OPP2 rejected: Cyrano is active");
      return false;
    }
    if (m_ActiveInputProtocol == InputProtocol::NONE) {
      ESP_LOGI(OPP2_TAG, "[Protocol] OPP2 provisional (no Cyrano detected yet)");
      m_ActiveInputProtocol = InputProtocol::OPP2;
    }
    return true;
  }

  return false;
}

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

  OPP2::Connection snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    m_State.connection.online = online;
    if (online) {
      m_State.connection.device_present = true;
      strncpy(m_State.connection.device, "ESP32-Scoring",
              sizeof(m_State.connection.device) - 1);
      m_State.connection.fw_version_present = true;
      strncpy(m_State.connection.fw_version, "1.0.0",
              sizeof(m_State.connection.fw_version) - 1);
    }
    snap = m_State.connection;
    snap.seq = NextSeq();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishConnection() timeout");
    return;
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
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

  OPP2::ApparatusStateMsg snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.apparatus_state;
    snap.seq = NextSeq();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishApparatusState() timeout");
    return;
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::APPARATUS_STATE, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  const char *stateNames[] = {"FENCING", "HALT",   "PAUSE",
                              "WAITING", "ENDING", "UNKNOWN"};
  int stateIdx = static_cast<int>(snap.state);
  if (stateIdx < 0 || stateIdx > 5)
    stateIdx = 5;

  ESP_LOGI(OPP2_TAG, "Published state %s to %s", stateNames[stateIdx],
           topicBuf);
}

void Opp2Handler::PublishLights() {
  if (!mqttClient.isConnected()) {
    return;
  }

  OPP2::Lights snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.lights;
    snap.seq = NextSeq();
    snap.ts = CreateTimestamp();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishLights() timeout");
    return;
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::LIGHTS, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published lights L(%s,%s) R(%s,%s) to %s",
           snap.left.on_target ? "red" : "off",
           snap.left.white ? "white" : "off",
           snap.right.on_target ? "green" : "off",
           snap.right.white ? "white" : "off", topicBuf);
}

void Opp2Handler::PublishClock() {
  if (!mqttClient.isConnected()) {
    return;
  }

  // Clock is QoS 0, no sequence number
  OPP2::Clock snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.clock;
    snap.ts = CreateTimestamp();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishClock() timeout");
    return;
  }

  char payloadBuf[512];
  char topicBuf[64];

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::CLOCK, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 0, true, payloadBuf);

  ESP_LOGD(OPP2_TAG, "Published clock: %s %ums to %s",
           snap.running ? "running" : "stopped", snap.time_ms, topicBuf);
}

void Opp2Handler::PublishScore() {
  if (!mqttClient.isConnected()) {
    return;
  }

  OPP2::Score snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.score;
    snap.seq = NextSeq();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishScore() timeout");
    return;
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::SCORE, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published score L:%d R:%d to %s",
           snap.left.score, snap.right.score, topicBuf);
}

void Opp2Handler::PublishFencers() {
  if (!mqttClient.isConnected()) {
    return;
  }

  OPP2::Fencers snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.fencers;
    snap.seq = NextSeq();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishFencers() timeout");
    return;
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::FENCERS, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published fencers L:%s R:%s to %s",
           snap.left.fencer.name, snap.right.fencer.name, topicBuf);
}

void Opp2Handler::PublishMatch() {
  if (!mqttClient.isConnected()) {
    ESP_LOGW(OPP2_TAG, "Cannot publish match: MQTT not connected");
    return;
  }

  OPP2::Match snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.match;
    snap.seq = NextSeq();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishMatch() timeout");
    return;
  }

  ESP_LOGI(OPP2_TAG, "PublishMatch: weapon=%d type=%d phase_type=%d round=%d",
           static_cast<int>(snap.weapon), static_cast<int>(snap.type),
           static_cast<int>(snap.phase_type), snap.round);

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::SerializeError err =
      OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));

  if (err != OPP2::SerializeError::OK) {
    ESP_LOGE(OPP2_TAG,
             "Failed to serialize Match: error=%d (weapon=%d type=%d)",
             static_cast<int>(err), static_cast<int>(snap.weapon),
             static_cast<int>(snap.type));
    return;
  }

  BuildTopic(OPP2::MessageType::MATCH, topicBuf, sizeof(topicBuf));
  ESP_LOGI(OPP2_TAG, "Payload: %s", payloadBuf);

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  const char *weaponNames[] = {"FOIL", "EPEE", "SABRE", "UNKNOWN"};
  int weaponIdx = static_cast<int>(snap.weapon);
  if (weaponIdx < 0 || weaponIdx > 3)
    weaponIdx = 3;

  ESP_LOGI(OPP2_TAG, "Published match: weapon=%s round=%d to %s",
           weaponNames[weaponIdx], snap.round, topicBuf);
}

void Opp2Handler::PublishUW2F() {
  if (!mqttClient.isConnected()) {
    return;
  }

  OPP2::UW2F snap;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    snap = m_State.uw2f;
    snap.seq = NextSeq();
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PublishUW2F() timeout");
    return;
  }

  char payloadBuf[512] = {0};
  char topicBuf[64] = {0};

  OPP2::Serializer::serialize(snap, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::UW2F, topicBuf, sizeof(topicBuf));

  mqttClient.publish(topicBuf, 1, true, payloadBuf);

  ESP_LOGI(OPP2_TAG, "Published UW2F: time=%ums L_P=%d R_P=%d to %s",
           snap.time_ms, snap.left.p_card, snap.right.p_card, topicBuf);
}

// ── Event Processing ────────────────────────────────────────────────────────

void Opp2Handler::PublishBladeContact(bool active) {
  if (!mqttClient.isConnected()) return;

  OPP2::BladeContact msg;
  msg.active = active;
  msg.ts = CreateTimestamp();

  char payloadBuf[96];
  char topicBuf[64];
  OPP2::Serializer::serialize(msg, payloadBuf, sizeof(payloadBuf));
  BuildTopic(OPP2::MessageType::BLADE_CONTACT, topicBuf, sizeof(topicBuf));
  mqttClient.publish(topicBuf, 0, false, payloadBuf); // QoS 0, not retained

  ESP_LOGD(OPP2_TAG, "Published blade_contact: active=%d", active);
}

void Opp2Handler::ProcessLightsChange(uint32_t eventtype) {
  uint32_t event_data = eventtype & SUB_TYPE_MASK;

  // Build lights state from event data
  OPP2::Lights lights = m_State.lights; // Copy current state to preserve seq/ts
  lights.left.on_target = (event_data & MASK_RED) != 0;
  lights.right.on_target = (event_data & MASK_GREEN) != 0;
  lights.left.white = (event_data & MASK_WHITE_L) != 0;
  lights.right.white = (event_data & MASK_WHITE_R) != 0;

  // Update through internal method (thread-safe, change detection, publish)
  updateLightsInternal(lights);

  // Blade contact: publish on transition only (QoS 0, momentary event)
  bool parry = (event_data & MASK_PARRY) != 0;
  if (parry != m_LastParryState) {
    m_LastParryState = parry;
    PublishBladeContact(parry);
  }
}

void Opp2Handler::ProcessUIEvents(uint32_t event) {
  // Handle UI button presses from UDPIOHandler
  // Remote control buttons update canonical state here, then notify Cyrano
  ESP_LOGD(OPP2_TAG, "ProcessUIEvents: 0x%08X", event);

  uint32_t event_data = event & SUB_TYPE_MASK;

  switch (event_data) {
  case UI_SWAP_FENCERS: {
    ESP_LOGI(OPP2_TAG, "[UI] SWAP FENCERS");

    // Snapshots for FSM sync — populated inside mutex
    uint8_t snapScoreL = 0, snapScoreR = 0;
    bool    snapYcL = false, snapYcR = false;
    uint8_t snapRcL = 0, snapRcR = 0;
    Priority_t snapPrio = NO_PRIO;

    if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      // Swap fencer identities
      OPP2::FencerSide tmpFencer = m_State.fencers.left;
      m_State.fencers.left  = m_State.fencers.right;
      m_State.fencers.right = tmpFencer;

      // Swap scores and cards
      OPP2::ScoreState tmpScore = m_State.score.left;
      m_State.score.left  = m_State.score.right;
      m_State.score.right = tmpScore;

      // Flip priority
      if (m_State.score.priority == OPP2::Priority::LEFT)
        m_State.score.priority = OPP2::Priority::RIGHT;
      else if (m_State.score.priority == OPP2::Priority::RIGHT)
        m_State.score.priority = OPP2::Priority::LEFT;

      // Swap lights
      OPP2::LightState tmpLight = m_State.lights.left;
      m_State.lights.left  = m_State.lights.right;
      m_State.lights.right = tmpLight;

      // Swap UW2F (P-cards)
      OPP2::UW2FSide tmpUw2f = m_State.uw2f.left;
      m_State.uw2f.left  = m_State.uw2f.right;
      m_State.uw2f.right = tmpUw2f;

      // Capture FSM sync snapshot (swapped values are now in m_State)
      snapScoreL = m_State.score.left.score;
      snapScoreR = m_State.score.right.score;
      snapYcL    = m_State.score.left.yellow_card;
      snapYcR    = m_State.score.right.yellow_card;
      snapRcL    = m_State.score.left.red_cards;
      snapRcR    = m_State.score.right.red_cards;
      switch (m_State.score.priority) {
        case OPP2::Priority::LEFT:  snapPrio = PRIO_LEFT;  break;
        case OPP2::Priority::RIGHT: snapPrio = PRIO_RIGHT; break;
        default:                    snapPrio = NO_PRIO;    break;
      }

      xSemaphoreGiveRecursive(m_StateMutex);
    } else {
      ESP_LOGW(OPP2_TAG, "[SWAP] mutex timeout — swap dropped");
      break;
    }

    PublishFencers();
    PublishScore();
    PublishLights();
    PublishUW2F();
    PushCachedStatusToCyrano();
    StateChanged(EVENT_STATE_CHANGED);

    // Sync FSM so its next event doesn't overwrite the swapped canonical state
    if (m_pFSM) {
      m_pFSM->SetScoreLeft(snapScoreL);
      m_pFSM->SetScoreRight(snapScoreR);
      m_pFSM->SetYellowCardLeft(snapYcL ? 1 : 0);
      m_pFSM->SetYellowCardRight(snapYcR ? 1 : 0);
      m_pFSM->SetRedCardLeft(snapRcL);
      m_pFSM->SetRedCardRight(snapRcR);
      m_pFSM->SetPriority(snapPrio);
    }
    break;
  }

  case UI_INPUT_RESET:
    if (m_AutoDetectProtocol) {
      ESP_LOGI(OPP2_TAG, "[Protocol] Reset: releasing protocol lock (NONE)");
      m_ActiveInputProtocol = InputProtocol::NONE;
    }
    break;

  case UI_INPUT_CYRANO_NEXT:
    ESP_LOGI(OPP2_TAG, "[UI] NEXT button pressed");
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_NEXT);
    if (mqttClient.isConnected()) {
      OPP2::Control ctrl;
      ctrl.seq     = NextSeq();
      ctrl.ts      = CreateTimestamp();
      ctrl.command = OPP2::Command::NEXT;
      char payloadBuf[160];
      char topicBuf[64];
      OPP2::Serializer::serialize(ctrl, payloadBuf, sizeof(payloadBuf));
      BuildTopic(OPP2::MessageType::CONTROL, topicBuf, sizeof(topicBuf));
      mqttClient.publish(topicBuf, 1, false, payloadBuf);
      ESP_LOGI(OPP2_TAG, "[OPP2] Published control NEXT to %s", topicBuf);
    }
    break;

  case UI_INPUT_CYRANO_PREV:
    ESP_LOGI(OPP2_TAG, "[UI] PREV button pressed");
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_PREV);
    if (mqttClient.isConnected()) {
      OPP2::Control ctrl;
      ctrl.seq     = NextSeq();
      ctrl.ts      = CreateTimestamp();
      ctrl.command = OPP2::Command::PREV;
      char payloadBuf[160];
      char topicBuf[64];
      OPP2::Serializer::serialize(ctrl, payloadBuf, sizeof(payloadBuf));
      BuildTopic(OPP2::MessageType::CONTROL, topicBuf, sizeof(topicBuf));
      mqttClient.publish(topicBuf, 1, false, payloadBuf);
      ESP_LOGI(OPP2_TAG, "[OPP2] Published control PREV to %s", topicBuf);
    }
    break;

  case UI_INPUT_CYRANO_BEGIN:
    // BEGIN button: Change state WAITING → HALT, then send INFO
    ESP_LOGI(OPP2_TAG, "[UI] BEGIN button pressed");
    {
      OPP2::ApparatusStateMsg newState;
      newState.state = OPP2::ApparatusState::HALT;
      updateApparatusStateInternal(newState); // push + notify + send INFO
    }
    notify(EVENT_CYRANO_STATE_UNLOCKED); // Unlock FSM (separate from state change)
    break;

  case UI_INPUT_CYRANO_END:
    ESP_LOGI(OPP2_TAG, "[UI] END button pressed");
    if (m_State.apparatus_state.state == OPP2::ApparatusState::WAITING) {
      // W→E is not a valid transition (spec §13). After a reboot the referee
      // must press BEGIN first (W→H), then END (H→E).
      ESP_LOGW(OPP2_TAG, "[UI] END ignored in W state — press BEGIN first");
      break;
    }
    {
      OPP2::ApparatusStateMsg newState;
      newState.state = OPP2::ApparatusState::ENDING;
      updateApparatusStateInternal(newState); // push + notify + send INFO
    }
    if (mqttClient.isConnected()) {
      OPP2::Control ctrl;
      ctrl.seq     = NextSeq();
      ctrl.ts      = CreateTimestamp();
      ctrl.command = OPP2::Command::END;
      char payloadBuf[160];
      char topicBuf[64];
      OPP2::Serializer::serialize(ctrl, payloadBuf, sizeof(payloadBuf));
      BuildTopic(OPP2::MessageType::CONTROL, topicBuf, sizeof(topicBuf));
      mqttClient.publish(topicBuf, 1, false, payloadBuf);
      ESP_LOGI(OPP2_TAG, "[OPP2] Published control END to %s", topicBuf);
    }
    break;

  default:
    // Other UI events not handled here
    break;
  }
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

void Opp2Handler::ProcessBootRecovery(const char *topic, const char *payload,
                                      unsigned int length) {
  // Called only during the 300ms boot recovery window (first MQTT connect).
  // Deserializes retained apparatus messages directly into m_State without
  // triggering publish or observer notifications.  State is published once,
  // in bulk, when the window closes.
  //
  // Note: FSM is NOT synced here — the apparatus was rebooted, so button
  // behaviour will be as if in W regardless of restored bout state.  This is
  // acceptable: restored display state gives the referee a reference; a spare
  // unit or manual referee action handles the rest.

  OPP2::Topic parsedTopic;
  if (!OPP2::TopicParser::parse(topic, parsedTopic)) {
    ESP_LOGW(OPP2_TAG, "[Boot] Unparseable topic: %s", topic);
    return;
  }

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGW(OPP2_TAG, "[Boot] Mutex timeout during recovery");
    return;
  }

  switch (parsedTopic.message_type) {
    case OPP2::MessageType::SCORE: {
      OPP2::Score msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        m_State.score = msg;
        ESP_LOGI(OPP2_TAG, "[Boot] Restored score L=%d R=%d", msg.left.score, msg.right.score);
      }
      break;
    }
    case OPP2::MessageType::LIGHTS: {
      OPP2::Lights msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        m_State.lights = msg;
        ESP_LOGI(OPP2_TAG, "[Boot] Restored lights L(%d,%d) R(%d,%d)",
                 (int)msg.left.on_target, (int)msg.left.white,
                 (int)msg.right.on_target, (int)msg.right.white);
      }
      break;
    }
    case OPP2::MessageType::APPARATUS_STATE: {
      OPP2::ApparatusStateMsg msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        // Always revert to W after a reboot. The FSM starts fresh regardless of
        // broker state, and external updates (NEXT/PREV/DISP) are only accepted
        // in W. Score/lights/fencers are restored as a display reference; the
        // referee presses BEGIN to start or re-confirm the match.
        if (msg.state != OPP2::ApparatusState::WAITING) {
          ESP_LOGW(OPP2_TAG, "[Boot] Restored state=%d: reverting to W (FSM reset on reboot)",
                   static_cast<int>(msg.state));
        }
        msg.state = OPP2::ApparatusState::WAITING;
        m_State.apparatus_state = msg;
      }
      break;
    }
    case OPP2::MessageType::CLOCK: {
      OPP2::Clock msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        msg.running = false; // timer is always stopped after a reboot
        m_State.clock = msg;
        ESP_LOGI(OPP2_TAG, "[Boot] Restored clock: %ums (stopped)", msg.time_ms);
      }
      break;
    }
    case OPP2::MessageType::UW2F: {
      OPP2::UW2F msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        m_State.uw2f = msg;
        ESP_LOGI(OPP2_TAG, "[Boot] Restored UW2F L_P=%d R_P=%d", msg.left.p_card, msg.right.p_card);
      }
      break;
    }
    case OPP2::MessageType::FENCERS: {
      OPP2::Fencers msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        m_State.fencers = msg;
        ESP_LOGI(OPP2_TAG, "[Boot] Restored fencers L:%s R:%s",
                 msg.left.fencer.name, msg.right.fencer.name);
      }
      break;
    }
    case OPP2::MessageType::MATCH: {
      OPP2::Match msg;
      if (OPP2::Deserializer::deserialize(payload, length, msg) == OPP2::DeserializeError::OK) {
        m_State.match = msg;
        ESP_LOGI(OPP2_TAG, "[Boot] Restored match weapon=%d round=%d",
                 static_cast<int>(msg.weapon), msg.round);
      }
      break;
    }
    default:
      // connection: LWT-managed, not restored here.
      // apparatus/fencers and apparatus/match are retained and handled above.
      // software/fencers and software/match are NOT retained per §4.5 — CMS re-pushes on reconnect.
      break;
  }

  xSemaphoreGiveRecursive(m_StateMutex);
}

void Opp2Handler::ProcessIncomingControl(const OPP2::Control &msg) {
  ESP_LOGI(OPP2_TAG, "[OPP2] ProcessIncomingControl: command=%d",
           static_cast<int>(msg.command));

  // Handle control commands similar to CyranoHandler's DISP/ACK/NAK handling
  switch (msg.command) {
  case OPP2::Command::ACK:
    ESP_LOGI(OPP2_TAG, "[OPP2] ACK received - transitioning to WAITING");
    {
      OPP2::ApparatusStateMsg waiting;
      waiting.state = OPP2::ApparatusState::WAITING;
      updateApparatusStateInternal(waiting); // push + notify + send INFO
    }
    ClearIdentifyingData();
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

void Opp2Handler::ProcessCyranoACK() {
  OPP2::ApparatusStateMsg waiting;
  waiting.state = OPP2::ApparatusState::WAITING;
  updateApparatusStateInternal(waiting); // push + notify + send INFO
  ClearIdentifyingData();
}

void Opp2Handler::ClearIdentifyingData() {
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    m_State.fencers = OPP2::Fencers{};
    m_State.match   = OPP2::Match{};
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] ClearIdentifyingData() timeout");
    return;
  }
  ESP_LOGI(OPP2_TAG, "[ACK] Cleared fencer/match identifying data");
  PublishFencers();
  PublishMatch();
  PushCachedStatusToCyrano();
  notify(EVENT_CYRANO_SEND_INFO);
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
    {
      OPP2::Match match = m_State.match; // Copy current state
      switch (event_data) {
      case WEAPON_MASK_EPEE:
        match.weapon = OPP2::Weapon::EPEE;
        break;
      case WEAPON_MASK_SABRE:
        match.weapon = OPP2::Weapon::SABRE;
        break;
      case WEAPON_MASK_FOIL:
        match.weapon = OPP2::Weapon::FOIL;
        break;
      default:
        // Unknown weapon mask - keep current valid weapon
        ESP_LOGW(OPP2_TAG,
                 "Unknown weapon mask: 0x%08X - keeping current weapon",
                 event_data);
        bTransmit = false; // Don't publish for unknown weapon
        break;
      }
      if (bTransmit) {
        ESP_LOGI(OPP2_TAG, "Weapon AFTER update: %d",
                 static_cast<int>(match.weapon));
        updateMatchInternal(match);
      }
    }
    bTransmit = false;
    break;

  case EVENT_SCORE_LEFT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.left.score = event_data;
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_SCORE_RIGHT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.right.score = event_data;
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_TIMER_STATE:
    // Update running state
    {
      OPP2::Clock clock = m_State.clock; // Copy current state
      OPP2::ApparatusStateMsg apparatusState = m_State.apparatus_state;

      if (eventtype & DATA_24BIT_MASK) {
        clock.running = true;
        if (apparatusState.state != OPP2::ApparatusState::ENDING &&
            apparatusState.state != OPP2::ApparatusState::WAITING) {
          apparatusState.state = OPP2::ApparatusState::FENCING;
          updateApparatusStateInternal(apparatusState); // push + notify + send INFO
        }
      } else {
        clock.running = false;
        if (apparatusState.state != OPP2::ApparatusState::ENDING &&
            apparatusState.state != OPP2::ApparatusState::WAITING) {
          apparatusState.state = OPP2::ApparatusState::HALT;
          updateApparatusStateInternal(apparatusState); // push + notify + send INFO
        }
      }
      updateClockInternal(clock);
    }
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
    // centiseconds == 100 is FencingTimer's "top of second" sentinel; treat as 0
    uint32_t minutes = TimeInfo.theBytes[2];
    uint32_t seconds = TimeInfo.theBytes[1];
    uint32_t centiseconds = TimeInfo.theBytes[0];
    if (centiseconds > 99) centiseconds = 0;

    OPP2::Clock clock = m_State.clock; // Copy current state
    clock.time_ms = (minutes * 60000) + (seconds * 1000) + (centiseconds * 10);

    if (bTransmit) {
      updateClockInternal(clock);
    }
    bTransmit = false;
    break;
  }

  case EVENT_ROUND: {
    uint8_t currentRound = event_data & DATA_BYTE0_MASK;
    uint8_t nrOfRounds = (event_data & DATA_BYTE1_MASK) >> 8;

    OPP2::Match match = m_State.match; // Copy current state
    match.round = currentRound;

    // Derive phase_type and type from nrOfRounds
    if (nrOfRounds == 1) {
      // Pools: 1 round
      match.phase_type = OPP2::PhaseType::POOL;
      match.type = OPP2::MatchType::INDIVIDUAL;
    } else if (nrOfRounds == 2 || nrOfRounds == 3) {
      // Direct Elimination: 2 or 3 rounds
      match.phase_type = OPP2::PhaseType::DE;
      match.type = OPP2::MatchType::INDIVIDUAL;
    } else if (nrOfRounds == 9) {
      // Team match: 9 rounds
      match.phase_type = OPP2::PhaseType::DE; // Teams are typically DE format
      match.type = OPP2::MatchType::TEAM;
    }

    ESP_LOGI(OPP2_TAG, "EVENT_ROUND: round=%d/%d, phase_type=%d, type=%d",
             currentRound, nrOfRounds, static_cast<int>(match.phase_type),
             static_cast<int>(match.type));

    updateMatchInternal(match);
    bTransmit = false;
    break;
  }

  case EVENT_YELLOW_CARD_LEFT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.left.yellow_card = (event_data > 0);
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_YELLOW_CARD_RIGHT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.right.yellow_card = (event_data > 0);
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_RED_CARD_LEFT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.left.red_cards = event_data;
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_RED_CARD_RIGHT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.right.red_cards = event_data;
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_BLACK_CARD_LEFT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.left.black_card = (event_data != 0);
    if (event_data) {
      score.right.status = OPP2::FencerStatus::EXCLUSION;
    } else {
      score.right.status = OPP2::FencerStatus::UNDEFINED;
    }
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_BLACK_CARD_RIGHT: {
    OPP2::Score score = m_State.score; // Copy current state
    score.right.black_card = (event_data != 0);
    if (event_data) {
      score.left.status = OPP2::FencerStatus::EXCLUSION;
    } else {
      score.left.status = OPP2::FencerStatus::UNDEFINED;
    }
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_P_CARD: {
    mix_t PCardInfo;
    PCardInfo.theDWord = eventtype & DATA_24BIT_MASK;
    OPP2::UW2F uw2f = m_State.uw2f; // Copy current state
    uw2f.right.p_card = PCardInfo.theBytes[1];
    uw2f.left.p_card = PCardInfo.theBytes[0];
    updateUW2FInternal(uw2f);
    bTransmit = false;
    break;
  }

  case EVENT_PRIO: {
    OPP2::Score score = m_State.score; // Copy current state
    switch (event_data) {
    case 2:
      score.priority = OPP2::Priority::RIGHT;
      break;
    case 1:
      score.priority = OPP2::Priority::LEFT;
      break;
    default:
      score.priority = OPP2::Priority::NONE;
    }
    updateScoreInternal(score);
  }
    bTransmit = false;
    break;

  case EVENT_UW2F_TIMER: {
    // Extract UW2F timer value
    mix_t TimeInfo;
    TimeInfo.theDWord = eventtype & DATA_24BIT_MASK;
    uint32_t minutes = TimeInfo.theBytes[2];
    uint32_t seconds = TimeInfo.theBytes[1];
    uint32_t centiseconds = TimeInfo.theBytes[0];

    OPP2::UW2F uw2f = m_State.uw2f; // Copy current state
    uw2f.time_ms = (minutes * 60000) + (seconds * 1000) + (centiseconds * 10);
    updateUW2FInternal(uw2f);
    bTransmit = false;
    break;
  }

  default:
    bTransmit = false;
  }
  (void)bTransmit;
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
    m_bConnected = true;

    if (s_bFirstConnect) {
      // First boot: subscribe to our own retained apparatus topics so the broker
      // delivers last-known state back to us.  ProcessBootRecovery() will write
      // them into m_State.  We publish nothing until the window closes.
      s_bBootRecoveryActive = true;
      s_BootRecoveryStartMs = millis();
      char topicBuf[80];
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/score",   m_State.piste_id);
      mqttClient.subscribe(topicBuf, 1);
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/lights",  m_State.piste_id);
      mqttClient.subscribe(topicBuf, 1);
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/state",   m_State.piste_id);
      mqttClient.subscribe(topicBuf, 1);
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/clock",   m_State.piste_id);
      mqttClient.subscribe(topicBuf, 0);
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/uw2f",    m_State.piste_id);
      mqttClient.subscribe(topicBuf, 1);
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/fencers", m_State.piste_id);
      mqttClient.subscribe(topicBuf, 1);
      snprintf(topicBuf, sizeof(topicBuf), "openpiste/%s/apparatus/match",   m_State.piste_id);
      mqttClient.subscribe(topicBuf, 1);
      ESP_LOGI(OPP2_TAG, "[OPP2] Boot recovery: subscribed to retained apparatus topics, holding 300ms");
    } else {
      // WiFi glitch reconnect — RAM state is valid; republish it.
      ESP_LOGI(OPP2_TAG, "[OPP2] MQTT reconnect: republishing RAM state for piste %s", m_State.piste_id);
      PublishConnection(true);
      PublishApparatusState();
      PublishScore();
      PublishLights();
      PublishMatch();
      PublishFencers();
    }

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

  // Close the boot recovery window after 300ms and publish restored state.
  if (s_bBootRecoveryActive && (millis() - s_BootRecoveryStartMs >= 300)) {
    s_bBootRecoveryActive = false;
    s_bFirstConnect       = false;
    ESP_LOGI(OPP2_TAG, "[OPP2] Boot recovery complete — state=W score=%d:%d fencers L:%s R:%s",
             m_State.score.left.score, m_State.score.right.score,
             m_State.fencers.left.fencer.name, m_State.fencers.right.fencer.name);
    // Sync FSM from restored state so all FSM observers (FPA422, LED strip,
    // TimeScoreDisplay) receive the correct values on the next FSM tick.
    if (m_pFSM) {
      weapon_t snapWeapon = UNKNOWN;
      switch (m_State.match.weapon) {
        case OPP2::Weapon::EPEE:  snapWeapon = EPEE;  break;
        case OPP2::Weapon::FOIL:  snapWeapon = FOIL;  break;
        case OPP2::Weapon::SABRE: snapWeapon = SABRE; break;
        default: break;
      }
      m_pFSM->SetScoreLeft(m_State.score.left.score);
      m_pFSM->SetScoreRight(m_State.score.right.score);
      m_pFSM->SetYellowCardLeft(m_State.score.left.yellow_card ? 1 : 0);
      m_pFSM->SetYellowCardRight(m_State.score.right.yellow_card ? 1 : 0);
      m_pFSM->SetRedCardLeft(m_State.score.left.red_cards);
      m_pFSM->SetRedCardRight(m_State.score.right.red_cards);
      m_pFSM->SetClockFromMs(m_State.clock.time_ms);
      if (snapWeapon != UNKNOWN)
        m_pFSM->SetMachineWeapon(snapWeapon);
      m_pFSM->SetUW2FSecondsFromMs(m_State.uw2f.time_ms);
      m_pFSM->SetPCardLeft(m_State.uw2f.left.p_card);
      m_pFSM->SetPCardRight(m_State.uw2f.right.p_card);
    }
    // Publish recovered state to broker.
    PublishConnection(true);
    PublishApparatusState();
    PublishScore();
    PublishLights();
    PublishMatch();
    PublishFencers();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_STATE_W);
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

// ────────────────────────────────────────────────────────────────────────────
// ZERO-COPY Cyrano Update (Phase 6 stack safety)
// ────────────────────────────────────────────────────────────────────────────

bool Opp2Handler::updateFromCyranoMessage(const EFP1Message &EFP1Input,
                                          OPP2::ApparatusState &outApparatusState) {
  // CRITICAL: This is called from UDP callback context (async_udp task)
  // NO stack allocations allowed! EFP1Input passed by const reference.

  ESP_LOGI(OPP2_TAG, "[Cyrano→OPP2] (zero-copy) Received command: %s",
           EFP1Input[Command].c_str());

  // ── Only process DISP/INFO commands (match setup) ───────────────────
  if (EFP1Input[Command] != "DISP" && EFP1Input[Command] != "INFO") {
    return false; // Not a match setup command
  }

  bool fencersChanged = false;
  bool scoreChanged = false;
  bool matchChanged = false;
  bool lightsChanged = false;
  bool uw2fChanged = false;
  bool clockChanged = false;
  bool isDisp = (EFP1Input[Command] == "DISP");
  // Snapshot for FSM sync — captured inside mutex before release
  uint8_t  snapScoreLeft = 0, snapScoreRight = 0;
  bool     snapYcLeft = false, snapYcRight = false;
  uint8_t  snapRcLeft = 0, snapRcRight = 0;
  uint32_t snapClockMs = 0;
  weapon_t snapWeapon = UNKNOWN;

  // ── Acquire mutex for state modification ────────────────────────────
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGW(OPP2_TAG,
             "[MUTEX] updateFromCyranoMessage() timeout - dropping update");
    return false;
  }

  // ── Fencers information (goes to FENCERS message, not SCORE) ────────

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
    strncpy(m_State.fencers.left.fencer.name, EFP1Input[LeftFencerName].c_str(),
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

  // ── Scores (goes to SCORE message) ──────────────────────────────────

  if (EFP1Input[RightScore] != "") {
    m_State.score.right.score = std::atoi(EFP1Input[RightScore].c_str());
    scoreChanged = true;
  }
  if (EFP1Input[LeftScore] != "") {
    m_State.score.left.score = std::atoi(EFP1Input[LeftScore].c_str());
    scoreChanged = true;
  }

  // ── Cards ────────────────────────────────────────────────────────────

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

  // ── P-Cards (UW2F) ───────────────────────────────────────────────────

  if (EFP1Input[RightPCards] != "") {
    m_State.uw2f.right.p_card = std::atoi(EFP1Input[RightPCards].c_str());
    uw2fChanged = true;
  }
  if (EFP1Input[LeftPCards] != "") {
    m_State.uw2f.left.p_card = std::atoi(EFP1Input[LeftPCards].c_str());
    uw2fChanged = true;
  }

  // ── Priority ─────────────────────────────────────────────────────────

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

  // ── Weapon ───────────────────────────────────────────────────────────

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

  // ── Round number ─────────────────────────────────────────────────────

  if (EFP1Input[RoundNumber] != "") {
    m_State.match.round = std::atoi(EFP1Input[RoundNumber].c_str());
    matchChanged = true;
  }

  // ── Match identification fields (echoed back in INFO) ────────────────
  if (EFP1Input[PhaseNumber] != "") {
    strncpy(m_State.match.phase, EFP1Input[PhaseNumber].c_str(),
            sizeof(m_State.match.phase) - 1);
    m_State.match.phase[sizeof(m_State.match.phase) - 1] = '\0';
    matchChanged = true;
  }
  if (EFP1Input[Poule_Tableau_Id] != "") {
    strncpy(m_State.match.poule, EFP1Input[Poule_Tableau_Id].c_str(),
            sizeof(m_State.match.poule) - 1);
    m_State.match.poule[sizeof(m_State.match.poule) - 1] = '\0';
    matchChanged = true;
  }
  if (EFP1Input[MatchNumber] != "") {
    m_State.match.match_num =
        (uint16_t)std::atoi(EFP1Input[MatchNumber].c_str());
    matchChanged = true;
  }
  if (EFP1Input[CompetitionType] != "") {
    if (EFP1Input[CompetitionType] == "T")
      m_State.match.type = OPP2::MatchType::TEAM;
    else
      m_State.match.type = OPP2::MatchType::INDIVIDUAL;
    matchChanged = true;
  }

  // ── StopWatch (initial clock value from DISP) ────────────────────────
  if (EFP1Input[StopWatch] != "") {
    uint32_t minutes = 0, seconds = 0;
    sscanf(EFP1Input[StopWatch].c_str(), "%u:%u", &minutes, &seconds);
    uint32_t new_time_ms = (minutes * 60000) + (seconds * 1000);
    if (m_State.clock.time_ms != new_time_ms) {
      m_State.clock.time_ms = new_time_ms;
      clockChanged = true;
    }
  }

  // ── Lights (if present in INFO messages) ────────────────────────────

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

  // Return current bout state to caller (DISP does not change it)
  outApparatusState = m_State.apparatus_state.state;

  // ── Capture FSM sync snapshot (inside mutex) ─────────────────────────
  if (isDisp) {
    snapScoreLeft  = m_State.score.left.score;
    snapScoreRight = m_State.score.right.score;
    snapYcLeft     = m_State.score.left.yellow_card;
    snapYcRight    = m_State.score.right.yellow_card;
    snapRcLeft     = m_State.score.left.red_cards;
    snapRcRight    = m_State.score.right.red_cards;
    snapClockMs    = m_State.clock.time_ms;
    if (EFP1Input[Weapon] != "") {
      switch (m_State.match.weapon) {
        case OPP2::Weapon::EPEE:  snapWeapon = EPEE;  break;
        case OPP2::Weapon::FOIL:  snapWeapon = FOIL;  break;
        case OPP2::Weapon::SABRE: snapWeapon = SABRE; break;
        default: break;
      }
    }
  }

  // ── Release mutex before publishing ──────────────────────────────────
  xSemaphoreGiveRecursive(m_StateMutex);

  // ── Publish updated state to OPP2 MQTT topics ───────────────────────

  if (fencersChanged || scoreChanged || matchChanged ||
      lightsChanged || uw2fChanged || clockChanged) {
    ESP_LOGI(OPP2_TAG, "[Cyrano→OPP2] Publishing updated state from %s",
             EFP1Input[Command].c_str());

    if (fencersChanged)
      PublishFencers();
    if (scoreChanged)
      PublishScore();
    if (matchChanged)
      PublishMatch();
    if (lightsChanged)
      PublishLights();
    if (uw2fChanged)
      PublishUW2F();
    if (clockChanged)
      PublishClock();

    StateChanged(EVENT_STATE_CHANGED);
    PushCachedStatusToCyrano();
  } else {
    if (EFP1Input[Command] == "INFO") {
      StateChanged(EVENT_STATE_CHANGED);
    }
  }

  // ── Sync FSM internal state from canonical (DISP only) ───────────────
  if (isDisp && m_pFSM) {
    m_pFSM->SetScoreLeft(snapScoreLeft);
    m_pFSM->SetScoreRight(snapScoreRight);
    m_pFSM->SetYellowCardLeft(snapYcLeft ? 1 : 0);
    m_pFSM->SetYellowCardRight(snapYcRight ? 1 : 0);
    m_pFSM->SetRedCardLeft(snapRcLeft);
    m_pFSM->SetRedCardRight(snapRcRight);
    m_pFSM->SetClockFromMs(snapClockMs);
    if (snapWeapon != UNKNOWN)
      m_pFSM->SetMachineWeapon(snapWeapon);
  }

  return true;
} // End of updateFromCyranoMessage()

// ════════════════════════════════════════════════════════════════════════════
// Thread-Safe State Access (Phase 1)
// ════════════════════════════════════════════════════════════════════════════

OPP2::SystemState Opp2Handler::getStateCopy() {
  OPP2::SystemState copy;

  // Acquire mutex with timeout
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    copy = m_State;
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG,
             "[MUTEX] getStateCopy() timeout - returning default state");
    // Return default-initialized state on timeout
  }

  return copy;
}

void Opp2Handler::getPisteId(char *buffer) {
  // Acquire mutex with timeout
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    strncpy(buffer, m_State.piste_id, OPP2::PISTE_ID_MAX - 1);
    buffer[OPP2::PISTE_ID_MAX - 1] = '\0';
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] getPisteId() timeout - returning empty");
    buffer[0] = '\0';
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Cache Synchronization (Phase 6 stack safety)
// ════════════════════════════════════════════════════════════════════════════

void Opp2Handler::PushCachedStatusToCyrano() {
  // Called after state updates (mutex already released)
  // Safe to take mutex again for read-only copy
  OPP2::SystemState stateCopy;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    stateCopy = m_State;
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] PushCachedStatusToCyrano() timeout");
    return;
  }

  // Convert to Cyrano format and push to CyranoHandler
  EFP1Message cyranoStatus = convertOpp2ToCyrano(stateCopy, stateCopy.piste_id);
  CyranoHandler::getInstance().updateCachedStatus(cyranoStatus);
}

// ════════════════════════════════════════════════════════════════════════════
// Internal State Updates (from FSM/Sensor - bypass all guards)
// ════════════════════════════════════════════════════════════════════════════

void Opp2Handler::updateLightsInternal(const OPP2::Lights &lights) {
  bool changed = false;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!lightsEqual(m_State.lights, lights)) {
      m_State.lights = lights;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateLightsInternal() timeout");
    return;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[Internal] Lights updated: L=%d/%d R=%d/%d",
             lights.left.on_target, lights.left.white, lights.right.on_target,
             lights.right.white);
    PublishLights();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

void Opp2Handler::updateScoreInternal(const OPP2::Score &score) {
  bool changed = false;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!scoreEqual(m_State.score, score)) {
      m_State.score = score;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateScoreInternal() timeout");
    return;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[Internal] Score updated: L=%u R=%u priority=%d",
             score.left.score, score.right.score,
             static_cast<int>(score.priority));
    PublishScore();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

void Opp2Handler::updateClockInternal(const OPP2::Clock &clock) {
  bool changed = false;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!clockEqual(m_State.clock, clock)) {
      m_State.clock = clock;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateClockInternal() timeout");
    return;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[Internal] Clock updated: time=%u running=%d",
             clock.time_ms, clock.running);
    PublishClock();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

void Opp2Handler::updateApparatusStateInternal(
    const OPP2::ApparatusStateMsg &apparatusState) {
  bool changed = false;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!apparatusStateEqual(m_State.apparatus_state, apparatusState)) {
      m_State.apparatus_state = apparatusState;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateApparatusStateInternal() timeout");
    return;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[Internal] ApparatusState updated: state=%d",
             static_cast<int>(apparatusState.state));
    PublishApparatusState();
    PushCachedStatusToCyrano();

    // Map new state to the correct Cyrano event for FPA422Handler/CyranoHandler
    static const uint32_t kCyranoStateEvent[] = {
        EVENT_CYRANO_STATE_F, // FENCING = 0
        EVENT_CYRANO_STATE_H, // HALT    = 1
        EVENT_CYRANO_STATE_P, // PAUSE   = 2
        EVENT_CYRANO_STATE_W, // WAITING = 3
        EVENT_CYRANO_STATE_E, // ENDING  = 4
        EVENT_CYRANO_STATE_W, // UNKNOWN = 5
    };
    int idx = static_cast<int>(apparatusState.state);
    if (idx < 0 || idx > 5) idx = 5;
    notify(kCyranoStateEvent[idx]);
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

void Opp2Handler::updateMatchInternal(const OPP2::Match &match) {
  bool changed = false;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!matchEqual(m_State.match, match)) {
      m_State.match = match;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateMatchInternal() timeout");
    return;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG,
             "[Internal] Match updated: weapon=%d type=%d phase=%d round=%u",
             static_cast<int>(match.weapon), static_cast<int>(match.type),
             static_cast<int>(match.phase_type), match.round);
    PublishMatch();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

void Opp2Handler::updateUW2FInternal(const OPP2::UW2F &uw2f) {
  bool changed = false;

  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!uw2fEqual(m_State.uw2f, uw2f)) {
      m_State.uw2f = uw2f;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateUW2FInternal() timeout");
    return;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[Internal] UW2F updated: time=%ums L_P=%d R_P=%d",
             uw2f.time_ms, uw2f.left.p_card, uw2f.right.p_card);
    PublishUW2F();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// External State Updates (from software - with protocol guards)
// ════════════════════════════════════════════════════════════════════════════

bool Opp2Handler::updateFencersExternal(const OPP2::Fencers &fencers,
                                        InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Guard: Only accept when apparatus is in WAITING state
  bool canUpdate = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    canUpdate =
        (m_State.apparatus_state.state == OPP2::ApparatusState::WAITING);
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateFencersExternal() guard check timeout");
    return false;
  }

  if (!canUpdate) {
    ESP_LOGV(OPP2_TAG,
             "[Guard] Fencers update rejected: apparatus not in WAITING");
    return false;
  }

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!fencersEqual(m_State.fencers, fencers)) {
      m_State.fencers = fencers;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateFencersExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[External] Fencers updated from %s",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano");
    PublishFencers();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }

  return true;
}

bool Opp2Handler::updateMatchExternal(const OPP2::Match &match,
                                      InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Guard 2: Only accept when apparatus is in WAITING state
  bool canUpdate = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    canUpdate =
        (m_State.apparatus_state.state == OPP2::ApparatusState::WAITING);
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateMatchExternal() guard check timeout");
    return false;
  }

  if (!canUpdate) {
    ESP_LOGV(OPP2_TAG,
             "[Guard] Match update rejected: apparatus not in WAITING");
    return false;
  }

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!matchEqual(m_State.match, match)) {
      m_State.match = match;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateMatchExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[External] Match updated from %s",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano");
    PublishMatch();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
    if (m_pFSM) {
      weapon_t w = UNKNOWN;
      switch (match.weapon) {
        case OPP2::Weapon::EPEE:  w = EPEE;  break;
        case OPP2::Weapon::FOIL:  w = FOIL;  break;
        case OPP2::Weapon::SABRE: w = SABRE; break;
        default: break;
      }
      if (w != UNKNOWN) m_pFSM->SetMachineWeapon(w);
    }
  }

  return true;
}

bool Opp2Handler::updateClockExternal(const OPP2::Clock &clock,
                                      InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Guard 2: Only accept when clock is not running
  bool canUpdate = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    canUpdate = !m_State.clock.running;
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateClockExternal() guard check timeout");
    return false;
  }

  if (!canUpdate) {
    ESP_LOGV(OPP2_TAG, "[Guard] Clock update rejected: clock is running");
    return false;
  }

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!clockEqual(m_State.clock, clock)) {
      m_State.clock = clock;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateClockExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[External] Clock updated from %s",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano");
    PublishClock();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
    if (m_pFSM) m_pFSM->SetClockFromMs(clock.time_ms);
  }

  return true;
}

bool Opp2Handler::updateScoreExternal(const OPP2::Score &score,
                                      InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Note: Score can be updated anytime (e.g., transferred matches, corrections)
  // No state guard needed - unlike fencers/match which require WAITING

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!scoreEqual(m_State.score, score)) {
      m_State.score = score;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateScoreExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[External] Score updated from %s: L=%u R=%u",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano",
             score.left.score, score.right.score);
    PublishScore();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
    if (m_pFSM) {
      m_pFSM->SetScoreLeft(score.left.score);
      m_pFSM->SetScoreRight(score.right.score);
      m_pFSM->SetYellowCardLeft(score.left.yellow_card ? 1 : 0);
      m_pFSM->SetYellowCardRight(score.right.yellow_card ? 1 : 0);
      m_pFSM->SetRedCardLeft(score.left.red_cards);
      m_pFSM->SetRedCardRight(score.right.red_cards);
    }
  }

  return true;
}

bool Opp2Handler::updateLightsExternal(const OPP2::Lights &lights,
                                       InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Note: Lights can be updated anytime (e.g., for testing, simulation)
  // No state guard needed

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!lightsEqual(m_State.lights, lights)) {
      m_State.lights = lights;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateLightsExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[External] Lights updated from %s",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano");
    PublishLights();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }

  return true;
}

bool Opp2Handler::updateApparatusStateExternal(
    const OPP2::ApparatusStateMsg &apparatusState, InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Guard 2: Software may only reset apparatus to WAITING.
  // FENCING/HALT/PAUSE/ENDING are driven by physical buttons and timer,
  // not by software commands. Reject stale retained MQTT messages that
  // could otherwise overwrite the device's own state on reconnect.
  if (apparatusState.state != OPP2::ApparatusState::WAITING) {
    ESP_LOGW(OPP2_TAG,
             "[Guard] ApparatusState external update rejected: software may "
             "only set WAITING (got %d)",
             static_cast<int>(apparatusState.state));
    return false;
  }

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!apparatusStateEqual(m_State.apparatus_state, apparatusState)) {
      m_State.apparatus_state = apparatusState;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateApparatusStateExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG, "[External] ApparatusState updated from %s: state=%d",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano",
             static_cast<int>(apparatusState.state));
    PublishApparatusState();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_STATE_W); // guard ensures only WAITING is accepted
    notify(EVENT_CYRANO_SEND_INFO);
  }

  return true;
}

bool Opp2Handler::updateUW2FExternal(const OPP2::UW2F &uw2f,
                                     InputProtocol source) {
  if (!isProtocolAllowed(source)) return false;

  // Note: UW2F (blade contact warnings) can be updated anytime
  // Used for penalty card tracking during matches

  // Update state
  bool changed = false;
  if (xSemaphoreTakeRecursive(m_StateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!uw2fEqual(m_State.uw2f, uw2f)) {
      m_State.uw2f = uw2f;
      changed = true;
    }
    xSemaphoreGiveRecursive(m_StateMutex);
  } else {
    ESP_LOGW(OPP2_TAG, "[MUTEX] updateUW2FExternal() update timeout");
    return false;
  }

  if (changed) {
    ESP_LOGI(OPP2_TAG,
             "[External] UW2F updated from %s: time=%ums L_P=%d R_P=%d",
             source == InputProtocol::OPP2 ? "OPP2" : "Cyrano", uw2f.time_ms,
             uw2f.left.p_card, uw2f.right.p_card);
    PublishUW2F();
    PushCachedStatusToCyrano();
    notify(EVENT_CYRANO_SEND_INFO);
  }

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Equality Helpers (for change detection)
// ════════════════════════════════════════════════════════════════════════════

bool Opp2Handler::lightsEqual(const OPP2::Lights &a, const OPP2::Lights &b) {
  return (
      a.left.on_target == b.left.on_target && a.left.white == b.left.white &&
      a.right.on_target == b.right.on_target && a.right.white == b.right.white);
}

bool Opp2Handler::clockEqual(const OPP2::Clock &a, const OPP2::Clock &b) {
  return (a.time_ms == b.time_ms && a.running == b.running);
}

bool Opp2Handler::scoreEqual(const OPP2::Score &a, const OPP2::Score &b) {
  return (
      a.left.score == b.left.score && a.left.red_cards == b.left.red_cards &&
      a.left.yellow_card == b.left.yellow_card &&
      a.left.black_card == b.left.black_card &&
      a.left.status == b.left.status &&
      a.right.score == b.right.score &&
      a.right.red_cards == b.right.red_cards &&
      a.right.yellow_card == b.right.yellow_card &&
      a.right.black_card == b.right.black_card &&
      a.right.status == b.right.status && a.priority == b.priority);
}

static bool personEqual(const OPP2::Person &a, const OPP2::Person &b) {
  if (a.present != b.present) return false;
  if (!a.present) return true;  // both absent
  return (strcmp(a.name, b.name) == 0 &&
          strcmp(a.id, b.id) == 0 &&
          strcmp(a.nation, b.nation) == 0);
}

bool Opp2Handler::fencersEqual(const OPP2::Fencers &a, const OPP2::Fencers &b) {
  return personEqual(a.left.fencer,   b.left.fencer)   &&
         personEqual(a.left.coach,    b.left.coach)     &&
         personEqual(a.right.fencer,  b.right.fencer)   &&
         personEqual(a.right.coach,   b.right.coach)    &&
         personEqual(a.referee,       b.referee)        &&
         personEqual(a.video_official, b.video_official);
}

bool Opp2Handler::matchEqual(const OPP2::Match &a, const OPP2::Match &b) {
  return (a.weapon == b.weapon && a.type == b.type &&
          a.phase_type == b.phase_type && a.round == b.round &&
          a.match_num == b.match_num &&
          strcmp(a.competition, b.competition) == 0 &&
          strcmp(a.phase, b.phase) == 0 &&
          strcmp(a.poule, b.poule) == 0);
}

bool Opp2Handler::apparatusStateEqual(const OPP2::ApparatusStateMsg &a,
                                      const OPP2::ApparatusStateMsg &b) {
  return (a.state == b.state);
}

bool Opp2Handler::uw2fEqual(const OPP2::UW2F &a, const OPP2::UW2F &b) {
  return (a.time_ms == b.time_ms && a.left.p_card == b.left.p_card &&
          a.right.p_card == b.right.p_card);
}

// ════════════════════════════════════════════════════════════════════════════
// OPP2 to Cyrano Conversion (Phase 3)
// ════════════════════════════════════════════════════════════════════════════

EFP1Message Opp2Handler::convertOpp2ToCyrano(const OPP2::SystemState &state,
                                             const char *pisteId) {
  EFP1Message cyrano;

  // ── Header fields ──────────────────────────────────────────────────────

  cyrano[Protocol] =
      "EFP1.1"; // Cyrano protocol identifier (required by software!)
  cyrano[PisteId] = pisteId ? pisteId : state.piste_id;

  // Weapon: OPP2::Weapon to Cyrano (E/F/S)
  switch (state.match.weapon) {
  case OPP2::Weapon::EPEE:
    cyrano[Weapon] = "E";
    break;
  case OPP2::Weapon::FOIL:
    cyrano[Weapon] = "F";
    break;
  case OPP2::Weapon::SABRE:
    cyrano[Weapon] = "S";
    break;
  default:
    cyrano[Weapon] = "F"; // Default to foil
    break;
  }

  // State: OPP2::ApparatusState to Cyrano (W/F/H/P/E)
  switch (state.apparatus_state.state) {
  case OPP2::ApparatusState::WAITING:
    cyrano[State] = "W";
    break;
  case OPP2::ApparatusState::FENCING:
    cyrano[State] = "F";
    break;
  case OPP2::ApparatusState::HALT:
    cyrano[State] = "H";
    break;
  case OPP2::ApparatusState::PAUSE:
    cyrano[State] = "P";
    break;
  case OPP2::ApparatusState::ENDING:
    cyrano[State] = "E";
    break;
  default:
    cyrano[State] = "W";
    break;
  }

  // Clock time: convert milliseconds to MM:SS format
  uint32_t total_seconds = state.clock.time_ms / 1000;
  uint32_t minutes = total_seconds / 60;
  uint32_t seconds = total_seconds % 60;
  char time_buf[8];
  snprintf(time_buf, sizeof(time_buf), "%02u:%02u", minutes, seconds);
  cyrano[StopWatch] = time_buf;

  // Round number
  char round_buf[8];
  snprintf(round_buf, sizeof(round_buf), "%u", state.match.round);
  cyrano[RoundNumber] = round_buf;

  // Match identification fields (echoed back from DISP)
  if (state.match.phase[0] != '\0')
    cyrano[PhaseNumber] = state.match.phase;
  if (state.match.poule[0] != '\0')
    cyrano[Poule_Tableau_Id] = state.match.poule;
  char match_num_buf[8];
  snprintf(match_num_buf, sizeof(match_num_buf), "%u", state.match.match_num);
  cyrano[MatchNumber] = match_num_buf;
  switch (state.match.type) {
  case OPP2::MatchType::TEAM:
    cyrano[CompetitionType] = "T";
    break;
  default:
    cyrano[CompetitionType] = "I";
    break;
  }

  // Priority: OPP2::Priority to Cyrano (L/R/N)
  switch (state.score.priority) {
  case OPP2::Priority::LEFT:
    cyrano[Priority] = "L";
    break;
  case OPP2::Priority::RIGHT:
    cyrano[Priority] = "R";
    break;
  default:
    cyrano[Priority] = "N";
    break;
  }

  // ── Right fencer fields ────────────────────────────────────────────────

  if (state.fencers.right.fencer.present) {
    cyrano[RightFencerId] = state.fencers.right.fencer.id;
    cyrano[RightFencerName] = state.fencers.right.fencer.name;
    cyrano[RightFencerNation] = state.fencers.right.fencer.nation;
  }

  // Right score
  char right_score_buf[8];
  snprintf(right_score_buf, sizeof(right_score_buf), "%d",
           state.score.right.score);
  cyrano[RightScore] = right_score_buf;

  // Right status (U=undefined/active, V=victory, D=defeat, A=abandonment, E=exclusion)
  switch (state.score.right.status) {
  case OPP2::FencerStatus::VICTORY:     cyrano[RightStatus] = "V"; break;
  case OPP2::FencerStatus::DEFEAT:      cyrano[RightStatus] = "D"; break;
  case OPP2::FencerStatus::ABANDONMENT: cyrano[RightStatus] = "A"; break;
  case OPP2::FencerStatus::EXCLUSION:   cyrano[RightStatus] = "E"; break;
  case OPP2::FencerStatus::DNS:         cyrano[RightStatus] = "DNS"; break;
  default:                              cyrano[RightStatus] = "U"; break;
  }

  // Right cards
  cyrano[RightYCard] = state.score.right.yellow_card ? "1" : "0";
  char right_red_buf[8];
  snprintf(right_red_buf, sizeof(right_red_buf), "%u",
           state.score.right.red_cards);
  cyrano[RightRCard] = right_red_buf;

  // Right lights
  cyrano[RightLight] = state.lights.right.on_target ? "1" : "0";
  cyrano[RightWhiteLight] = state.lights.right.white ? "1" : "0";

  // Right medical and reserve (known gaps — 0/N for individual competitions)
  cyrano[RightMedicalIntervention] = "0";
  cyrano[RightReserveIntroduction] = "N";

  // Right P-cards
  char right_pcard_buf[8];
  snprintf(right_pcard_buf, sizeof(right_pcard_buf), "%u",
           state.uw2f.right.p_card);
  cyrano[RightPCards] = right_pcard_buf;

  // ── Left fencer fields ─────────────────────────────────────────────────

  if (state.fencers.left.fencer.present) {
    cyrano[LeftFencerId] = state.fencers.left.fencer.id;
    cyrano[LeftFencerName] = state.fencers.left.fencer.name;
    cyrano[LeftFencerNation] = state.fencers.left.fencer.nation;
  }

  // Left score
  char left_score_buf[8];
  snprintf(left_score_buf, sizeof(left_score_buf), "%d",
           state.score.left.score);
  cyrano[LeftScore] = left_score_buf;

  // Left status
  switch (state.score.left.status) {
  case OPP2::FencerStatus::VICTORY:     cyrano[LeftStatus] = "V"; break;
  case OPP2::FencerStatus::DEFEAT:      cyrano[LeftStatus] = "D"; break;
  case OPP2::FencerStatus::ABANDONMENT: cyrano[LeftStatus] = "A"; break;
  case OPP2::FencerStatus::EXCLUSION:   cyrano[LeftStatus] = "E"; break;
  case OPP2::FencerStatus::DNS:         cyrano[LeftStatus] = "DNS"; break;
  default:                              cyrano[LeftStatus] = "U"; break;
  }

  // Left cards
  cyrano[LeftYCard] = state.score.left.yellow_card ? "1" : "0";
  char left_red_buf[8];
  snprintf(left_red_buf, sizeof(left_red_buf), "%u",
           state.score.left.red_cards);
  cyrano[LeftRCard] = left_red_buf;

  // Left lights
  cyrano[LeftLight] = state.lights.left.on_target ? "1" : "0";
  cyrano[LeftWhiteLight] = state.lights.left.white ? "1" : "0";

  // Left medical and reserve (known gaps — 0/N for individual competitions)
  cyrano[LeftMedicalIntervention] = "0";
  cyrano[LeftReserveIntroduction] = "N";

  // Left P-cards
  char left_pcard_buf[8];
  snprintf(left_pcard_buf, sizeof(left_pcard_buf), "%u",
           state.uw2f.left.p_card);
  cyrano[LeftPCards] = left_pcard_buf;

  return cyrano;
}

// ════════════════════════════════════════════════════════════════════════════
// Cyrano to OPP2 Conversion (Phase 4)
// ════════════════════════════════════════════════════════════════════════════

OPP2::Fencers
Opp2Handler::convertCyranoToOpp2Fencers(const EFP1Message &cyrano) {
  OPP2::Fencers fencers = {};

  // ── Right fencer ──────────────────────────────────────────────────────
  const std::string &rightId = cyrano[RightFencerId];
  const std::string &rightName = cyrano[RightFencerName];
  const std::string &rightNation = cyrano[RightFencerNation];

  if (!rightId.empty() || !rightName.empty() || !rightNation.empty()) {
    fencers.right.fencer.present = true;
    strncpy(fencers.right.fencer.id, rightId.c_str(),
            sizeof(fencers.right.fencer.id) - 1);
    strncpy(fencers.right.fencer.name, rightName.c_str(),
            sizeof(fencers.right.fencer.name) - 1);
    strncpy(fencers.right.fencer.nation, rightNation.c_str(),
            sizeof(fencers.right.fencer.nation) - 1);
  } else {
    fencers.right.fencer.present = false;
  }

  // ── Left fencer ───────────────────────────────────────────────────────
  const std::string &leftId = cyrano[LeftFencerId];
  const std::string &leftName = cyrano[LeftFencerName];
  const std::string &leftNation = cyrano[LeftFencerNation];

  if (!leftId.empty() || !leftName.empty() || !leftNation.empty()) {
    fencers.left.fencer.present = true;
    strncpy(fencers.left.fencer.id, leftId.c_str(),
            sizeof(fencers.left.fencer.id) - 1);
    strncpy(fencers.left.fencer.name, leftName.c_str(),
            sizeof(fencers.left.fencer.name) - 1);
    strncpy(fencers.left.fencer.nation, leftNation.c_str(),
            sizeof(fencers.left.fencer.nation) - 1);
  } else {
    fencers.left.fencer.present = false;
  }

  return fencers;
}

OPP2::Match Opp2Handler::convertCyranoToOpp2Match(const EFP1Message &cyrano) {
  OPP2::Match match = {};

  // ── Weapon conversion: E/F/S → OPP2::Weapon ───────────────────────────
  const std::string &weaponStr = cyrano[Weapon];
  if (weaponStr == "E") {
    match.weapon = OPP2::Weapon::EPEE;
  } else if (weaponStr == "F") {
    match.weapon = OPP2::Weapon::FOIL;
  } else if (weaponStr == "S") {
    match.weapon = OPP2::Weapon::SABRE;
  } else {
    match.weapon = OPP2::Weapon::EPEE; // Default
  }

  // ── Round number ──────────────────────────────────────────────────────
  const std::string &roundStr = cyrano[RoundNumber];
  if (!roundStr.empty()) {
    match.round = static_cast<uint8_t>(std::stoi(roundStr));
  } else {
    match.round = 1; // Default
  }

  // Note: type and phase_type not present in Cyrano, leave as default
  return match;
}

OPP2::Clock Opp2Handler::convertCyranoToOpp2Clock(const EFP1Message &cyrano) {
  OPP2::Clock clock = {};
  clock.running = false; // Cyrano doesn't indicate if clock is running

  // ── Parse MM:SS format → milliseconds ─────────────────────────────────
  const std::string &stopwatchStr = cyrano[StopWatch];
  if (!stopwatchStr.empty()) {
    size_t colonPos = stopwatchStr.find(':');
    if (colonPos != std::string::npos) {
      // Format is "MM:SS"
      int minutes = std::stoi(stopwatchStr.substr(0, colonPos));
      int seconds = std::stoi(stopwatchStr.substr(colonPos + 1));
      clock.time_ms = (minutes * 60 + seconds) * 1000;
    } else {
      // If no colon, assume it's just seconds
      int seconds = std::stoi(stopwatchStr);
      clock.time_ms = seconds * 1000;
    }
  } else {
    clock.time_ms = 0;
  }

  return clock;
}