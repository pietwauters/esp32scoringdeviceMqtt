#include "CyranoHandler.h"
#include "AbsoluteTime.h"
#include "CyranoConverter.h"
#include "EFP1Message.h"
#include "MDNSResolver.h"
#include "Opp2Handler.h"
#include <esp_log.h>
#include <sstream>
#include <string>

extern const char ca_cert_pem[];
extern const size_t ca_cert_pem_len;

static const char *CYRANO_TAG = "Cyrano";
const char *mdnsName = "openpiste";
CyranoHandler::CyranoHandler() : m_CachedStatusValid(false) {
  // ctor
  // Note: State now managed by OPP2::SystemState in Opp2Handler
  // Cached status will be populated on first state update
}

auto &mqttClient = AtlasAsyncMqttClient::getInstance();
char mqttServer[16];           // MQTT Broker address
const int mqttPort = 1883;     // MQTT Broker port (testing: no TLS)
const char *mqttUser = "";     // MQTT username (optional)
const char *mqttPassword = ""; // MQTT password (optional)
char *mqttClientId;            // MQTT Client ID
char *mqttPublishTopic; // = "MQTTCyrano/Piste_001/FromDevice";  // Topic to
                        // subscribe and publish to
char *mqttListenTopic;  // = "MQTTCyrano/Piste_001/FromSoftware";  // Topic to
                        // subscribe and publish to
char *mqttLastWillTopic;

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT broker");
  mqttClient.publish(mqttLastWillTopic, 1, true, "online");

  // Subscribe to the topic
  mqttClient.subscribe(mqttListenTopic, 1);
}

void onMqttDisconnect() {}

void onMqttMessage(const char *topic, const char *payload,
                   unsigned int length) {
  // Null-terminate the payload to make it a string
  // payload[length] = '\0';
  CyranoHandler &MyCyranoHandler = CyranoHandler::getInstance();
  std::string CyranoStr = convert_json_to_cyrano_string((char *)payload);
  if (CyranoStr != "") {
    MyCyranoHandler.ProcessMessageFromSoftware((EFP1Message(CyranoStr)), false);
    // std::cout << "Reveiced mqtt message: " << CyranoStr << std::endl;
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Cache Management (Phase 6 stack safety fix)
// ════════════════════════════════════════════════════════════════════════════

void CyranoHandler::updateCachedStatus(const EFP1Message &status) {
  // Store the base state from Opp2Handler
  m_CachedStatus = status;

  // Rebuild the cached strings with current CompetitionId
  RebuildCachedStrings();
}

void CyranoHandler::RebuildCachedStrings() {
  // Build EFP1 message with Cyrano-specific fields
  EFP1Message msg = m_CachedStatus;
  msg[Command] = "INFO";
  msg[CompetitionId] = m_CompetitionId;

  // Build and cache the INFO Cyrano protocol string and JSON
  msg.ToString(m_CachedCyranoString);
  m_CachedJsonString = convert_cyrano_to_json_string(m_CachedCyranoString);

  // Build and cache NEXT message strings
  // CRITICAL: MakeNextMessageString() uses msg[CompetitionId], so msg must have
  // it set
  m_CachedNextCyrano = msg.MakeNextMessageString();
  m_CachedNextJson = convert_cyrano_to_json_string(m_CachedNextCyrano);

  // Build and cache PREV message strings
  // CRITICAL: MakePrevMessageString() uses msg[CompetitionId], so msg must have
  // it set
  m_CachedPrevCyrano = msg.MakePrevMessageString();
  m_CachedPrevJson = convert_cyrano_to_json_string(m_CachedPrevCyrano);

  // Mark cache as valid
  m_CachedStatusValid = true;
}

void CyranoHandler::Begin() {

  networkpreferences.begin("credentials", false);
  uint32_t PisteNr = networkpreferences.getInt("pisteNr", 304);
  String pisteName = networkpreferences.getString("Pistename", "");
  CyranoPort = networkpreferences.getUShort("CyranoPort", CYRANO_PORT);
  CyranoBroadcastPort =
      networkpreferences.getUShort("CyranoBcPort", CYRANO_BROADCAST_PORT);
  // Note: Piste ID now managed by Opp2Handler, loaded from preferences there

  NextPeriodicalUpdate = millis() + 10000;
  strncpy(mqttServer,
          networkpreferences.getString("MqttBroker", "10.154.1.130").c_str(),
          16);
  networkpreferences.end();

  mqttClientId = (char *)malloc(sizeof("Piste_001") + 1);
  sprintf(mqttClientId, "Piste_%.3d", PisteNr);
  mqttPublishTopic =
      (char *)malloc(sizeof("MQTT_Cyrano/Piste_001/FromDevice") + 1);
  sprintf(mqttPublishTopic, "MQTT_Cyrano/Piste_%.3d/FromDevice", PisteNr);
  mqttListenTopic =
      (char *)malloc(sizeof("MQTT_Cyrano/Piste_001/FromSoftware") + 1);
  sprintf(mqttListenTopic, "MQTT_Cyrano/Piste_%.3d/FromSoftware", PisteNr);

  // NOTE: MQTT callbacks, connection, and NTP now managed by Opp2Handler
  // CyranoHandler still uses the shared mqttClient for publishing

  IPAddress theBroker;
  uint16_t resolvedPort = mqttPort; // Default port

  theBroker.fromString(mqttServer);

  IPAddress resolvedBroker =
      MDNSResolver::getInstance().resolveHostname(mdnsName, theBroker);
  mqttClient.setServer(resolvedBroker, resolvedPort);
  mqttClient.setTLS(false);
  mqttClient.setCredentials(mqttUser, mqttPassword);
  mqttClient.setClientId(mqttClientId);

  mqttLastWillTopic =
      (char *)malloc(sizeof("MQTT_Cyrano/Piste_001/Connection") + 1);
  sprintf(mqttLastWillTopic, "MQTT_Cyrano/Piste_%.3d/Connection", PisteNr);
  // NOTE: LWT now set by Opp2Handler (OPP2 is primary protocol)
  // CyranoHandler publishes online/offline via Cyrano topics if needed
}

CyranoHandler::~CyranoHandler() {
  // dtor
}

void CyranoHandler::SetPisteID(const std::string &ID) {
  // Update canonical state in Opp2Handler
  Opp2Handler::getInstance().SetPisteID(ID.c_str());
}

void CyranoHandler::ClearOnACK() {
  // Note: State now managed by OPP2::SystemState in Opp2Handler
  // Clearing is handled when software sends new fencer data via DISP
}

void CyranoHandler::SendInfoMessage() {
  if (!bOKToSend)
    return;

  // ── Use cached strings - NO string building in callback ───────────────
  // Strings pre-built by RebuildCachedStrings() when state/CompetitionId
  // changes
  if (!m_CachedStatusValid) {
    // Cache not yet initialized - skip this send
    return;
  }

  // Use cached strings directly - zero stack allocations
  const char *pCyranoMsg = m_CachedCyranoString.c_str();
  size_t cyranoLen = m_CachedCyranoString.length();
  const char *pJsonMsg = m_CachedJsonString.c_str();
  size_t jsonLen = m_CachedJsonString.length();

  // ── Send message ──────────────────────────────────────────────────────
  if (false)
    CyranoHandlerudpBroadcast.broadcastTo((uint8_t *)pCyranoMsg, cyranoLen,
                                          CyranoBroadcastPort,
                                          TCPIP_ADAPTER_IF_STA);
  else {
    CyranoHandlerudpRcv.writeTo((uint8_t *)pCyranoMsg, cyranoLen,
                                SoftwareIPAddress(), CyranoBroadcastPort,
                                TCPIP_ADAPTER_IF_STA);
    mqttClient.publish(mqttPublishTopic, 0, true, pJsonMsg, jsonLen);
    // Do both mqtt and standard Cyrano
    CyranoHandlerudpBroadcast.broadcastTo((uint8_t *)pCyranoMsg, cyranoLen,
                                          CyranoBroadcastPort,
                                          TCPIP_ADAPTER_IF_STA);
  }

  return;
}

void CyranoHandler::ProcessMessageFromSoftware(const EFP1Message &input,
                                               bool bVerifyPisteID) {
  if (bVerifyPisteID) {
    // Get piste ID from canonical OPP2 state (stack-efficient)
    char pisteId[OPP2::PISTE_ID_MAX];
    Opp2Handler::getInstance().getPisteId(pisteId);
    if (input[PisteId] != std::string(pisteId))
      return; // wrong Piste
  }
  switch (input.GetType()) {
  case HELLO:
    if (m_State != WAITING) {
      m_State = WAITING;
      StateChanged(EVENT_CYRANO_STATE_W);
    }

    bOKToSend = true;
    bSoftwareIsLive = true;
    LastHelloReception = millis();
    m_CompetitionId = input[CompetitionId]; // Store Cyrano-specific field

    // Rebuild cached strings since CompetitionId changed
    if (m_CachedStatusValid) {
      RebuildCachedStrings();
    }

    SendInfoMessage();

    break;

  case DISP:
    if (WAITING == m_State) {
      // ── Phase 6: Route Cyrano input through OPP2 canonical state ───────
      // Notify Opp2Handler with the full message to parse ALL fields
      // (scores, fencers, match, clock, cards, priority, state, etc.)
      std::string msg;
      EFP1Message temp = input; // Copy needed for non-const ToString()
      temp.ToString(msg);
      StateChanged(msg); // → Opp2Handler::update(CyranoHandler*, string)
      // ────────────────────────────────────────────────────────────────────

      // Per Cyrano state transition table: DISP in WAITING → stay WAITING
      m_State = WAITING;
      StateChanged(EVENT_CYRANO_STATE_W);

      // Lock Machine (only BEGIN button can unlock)
      StateChanged(EVENT_CYRANO_STATE_LOCKED);
    }

    break;

  case ACK:
    if (WAITING != m_State) {
      m_State = WAITING;
      ClearOnACK();
      StateChanged(EVENT_CYRANO_STATE_W);
      SendInfoMessage();
    }

    break;

  case NAK:
    // The software doesn't accept the "END" message
    StateChanged(EVENT_CYRANO_STATE_NAK);
    m_State = HALT;
    SendInfoMessage();
    break;
    ESP_LOGE(CYRANO_TAG, "%s", "Interesting, I should never ever get here");
  }
  // Note: Cache is push-updated by Opp2Handler when state changes
}

void CyranoHandler::ProcessUIEvents(uint32_t const event) {
  uint32_t event_data = event & SUB_TYPE_MASK;

  switch (event_data) {
  case UI_INPUT_CYRANO_NEXT:
    ESP_LOGI(CYRANO_TAG,
             "[DEBUG] NEXT button pressed, m_State=%d, bOKToSend=%d, "
             "m_CachedStatusValid=%d",
             m_State, bOKToSend, m_CachedStatusValid);
    bOKToSend = true;
    if (WAITING == m_State) {
      // Use cached NEXT strings - NO stack allocations
      if (!m_CachedStatusValid) {
        ESP_LOGW(CYRANO_TAG, "[DEBUG] NEXT: Cache not valid yet!");
        return; // Cache not initialized yet
      }
      const char *pCyranoMsg = m_CachedNextCyrano.c_str();
      size_t cyranoLen = m_CachedNextCyrano.length();
      const char *pJsonMsg = m_CachedNextJson.c_str();
      size_t jsonLen = m_CachedNextJson.length();

      ESP_LOGI(CYRANO_TAG, "[DEBUG] NEXT: Sending %d bytes: %s", cyranoLen,
               pCyranoMsg);

      if (false)
        CyranoHandlerudpBroadcast.broadcastTo((uint8_t *)pCyranoMsg, cyranoLen,
                                              CyranoBroadcastPort,
                                              TCPIP_ADAPTER_IF_STA);
      else {
        CyranoHandlerudpRcv.writeTo((uint8_t *)pCyranoMsg, cyranoLen,
                                    SoftwareIPAddress(), CyranoBroadcastPort,
                                    TCPIP_ADAPTER_IF_STA);
        mqttClient.publish(mqttPublishTopic, 0, true, pJsonMsg, jsonLen);
      }
      StateChanged(EVENT_CYRANO_STATE_W);
    } else {
      ESP_LOGW(CYRANO_TAG, "[DEBUG] NEXT: Not in WAITING state (m_State=%d)",
               m_State);
    }
    break;

  case UI_INPUT_CYRANO_PREV:
    bOKToSend = true;
    if (WAITING == m_State) {
      // Use cached PREV strings - NO stack allocations
      if (!m_CachedStatusValid) {
        return; // Cache not initialized yet
      }
      const char *pCyranoMsg = m_CachedPrevCyrano.c_str();
      size_t cyranoLen = m_CachedPrevCyrano.length();
      const char *pJsonMsg = m_CachedPrevJson.c_str();
      size_t jsonLen = m_CachedPrevJson.length();

      if (false)
        CyranoHandlerudpBroadcast.broadcastTo((uint8_t *)pCyranoMsg, cyranoLen,
                                              CyranoBroadcastPort,
                                              TCPIP_ADAPTER_IF_STA);
      else {
        CyranoHandlerudpRcv.writeTo((uint8_t *)pCyranoMsg, cyranoLen,
                                    SoftwareIPAddress(), CyranoBroadcastPort,
                                    TCPIP_ADAPTER_IF_STA);
        mqttClient.publish(mqttPublishTopic, 0, true, pJsonMsg, jsonLen);
      }
      StateChanged(EVENT_CYRANO_STATE_W);
    }
    break;

  case UI_INPUT_CYRANO_BEGIN:
    bOKToSend = true;
    if (WAITING == m_State) {
      m_State = HALT;
      StateChanged(EVENT_CYRANO_STATE_H);
      SendInfoMessage();

      // Unlock Machine
      StateChanged(EVENT_CYRANO_STATE_UNLOCKED);
    }
    break;

  case UI_INPUT_CYRANO_END:
    bOKToSend = true;
    if (WAITING != m_State) {
      m_State = ENDING;
      StateChanged(EVENT_CYRANO_STATE_E);
      SendInfoMessage();
    }
    break;

  case UI_SWAP_FENCERS:
    bOKToSend = true;
    {
      // TODO Phase 7: Implement fencer swap via OPP2 state update
      // For now, just send current state
      SendInfoMessage();
    }

    break;

  case UI_RESERVE_LEFT:
    bOKToSend = true;
    {
      // TODO Phase 7: Check team match and handle reserve
      // For now, just send current state
      SendInfoMessage();
    }
    break;
  case UI_RESERVE_RIGHT:
    bOKToSend = true;
    {
      // TODO Phase 7: Check team match and handle reserve
      // For now, just send current state
      SendInfoMessage();
    }
    break;

  case UI_ABANDON_LEFT:
    bOKToSend = true;
    {
      // TODO Phase 7: Set left fencer abandoned status
      // For now, just send current state
      SendInfoMessage();
    }

  case UI_ABANDON_RIGHT:
    bOKToSend = true;
    {
      // TODO Phase 7: Set right fencer abandoned status
      // For now, just send current state
      SendInfoMessage();
    }
  }
}

#define MASK_ANY_ORANGE (MASK_ORANGE_L | MASK_ORANGE_R)

void CyranoHandler::ProcessLightsChange(uint32_t eventtype) {
  // Note: Lights already updated in Opp2Handler via updateLightsInternal
  // (Phase 2) No need to duplicate state here - just trigger Cyrano message
  // send (Handled in update() method which calls SendInfoMessage())
}

void CyranoHandler::update(FencingStateMachine *subject, uint32_t eventtype) {
  uint32_t event_data = eventtype & SUB_TYPE_MASK;
  uint32_t maineventtype = eventtype & MAIN_TYPE_MASK;
  bool bTransmit = true;

  // ── Phase 6: State now managed by Opp2Handler ──────────────────────────
  // FSM events already update OPP2::SystemState via updateXxxInternal (Phase
  // 2) This method only needs to:
  // 1. Trigger Cyrano message sends via SendInfoMessage()
  // 2. Update local Cyrano-specific state (m_State)
  // 3. Emit state change notifications

  switch (maineventtype) {

  case EVENT_LIGHTS:
    ProcessLightsChange(eventtype);
    break;

  case EVENT_WEAPON:
  case EVENT_SCORE_LEFT:
  case EVENT_SCORE_RIGHT:
  case EVENT_ROUND:
  case EVENT_YELLOW_CARD_LEFT:
  case EVENT_YELLOW_CARD_RIGHT:
  case EVENT_RED_CARD_LEFT:
  case EVENT_RED_CARD_RIGHT:
  case EVENT_BLACK_CARD_LEFT:
  case EVENT_BLACK_CARD_RIGHT:
  case EVENT_P_CARD:
  case EVENT_PRIO:
    // All state updates handled by Opp2Handler (Phase 2)
    // Just trigger Cyrano message send
    break;

  case EVENT_TIMER_STATE: {
    // Check current state using cached status (avoid stack allocation)
    if (!m_CachedStatusValid) {
      break; // Cache not initialized yet
    }

    if ((m_CachedStatus[State] == "E") || (m_CachedStatus[State] == "W"))
      break;

    if (eventtype & DATA_24BIT_MASK) {
      StateChanged(EVENT_CYRANO_STATE_F);
    } else {
      StateChanged(EVENT_CYRANO_STATE_H);
    }
  } break;

  case EVENT_TIMER: {
    uint32_t temp = millis();
    if (temp < m_timeToShowTimer) {
      bTransmit = false;
    } else {
      bTransmit = true;
      if (m_timeToShowTimer - temp < 1000)
        m_timeToShowTimer += 1000;
      else
        m_timeToShowTimer = temp + 900;
    }
    // Always show transition to zero
    if (!event_data)
      bTransmit = true;
  } break;

  case EVENT_UW2F_TIMER:
    // Unknown to Cyrano -> build UW2F_Timer JSON and publish directly
    bTransmit = false;
    {
      char pisteId[OPP2::PISTE_ID_MAX];
      Opp2Handler::getInstance().getPisteId(pisteId);
      std::string uw2f_json = make_uw2f_timer_from_event_string(
          eventtype, pisteId, (long long)millis());
      if (!uw2f_json.empty()) {
        mqttClient.publish(mqttPublishTopic, 0, true, uw2f_json.c_str(),
                           uw2f_json.length());
      }
    }
    break;

  default:
    bTransmit = false;
  }

  if (bTransmit) {
    SendInfoMessage();
  }
  // Note: Cache is push-updated by Opp2Handler when state changes
}

void ProcessCyranoPacket(AsyncUDPPacket packet) {

  // If Software is live, we know the IP address. If the received packet does
  // come from the Software, we can ignore it.
  CyranoHandler &MyCyranoHandler = CyranoHandler::getInstance();
  if (MyCyranoHandler.SoftwareIsLive()) {
    if (MyCyranoHandler.SoftwareIPAddress() != packet.remoteIP())
      return;
  } else {
    if (!strncmp((char *)packet.data() + 8, "HELLO", 5)) {
      MyCyranoHandler.SoftwareIPAddress(packet.remoteIP());
    } else
      return;
  }
  // ESP_LOGE(CYRANO_TAG, "%s",(char*)packet.data());
  MyCyranoHandler.ProcessMessageFromSoftware(
      (EFP1Message((char *)packet.data())));
}

void CyranoHandler::CheckConnection() {
  if (bCyranoConnected) {
    if (bSoftwareIsLive) {
      if (LastHelloReception + 40000 < millis()) {
        bSoftwareIsLive = false;
        StateChanged(EVENT_CYRANO_STATE_W);
      }
    }
    if (budpCyranoConnected && bmqttCyranoConnected)
      return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!bWifiConnected) {
      bWifiConnected = true;
    }
    // NOTE: MQTT connection now managed by Opp2Handler
    // CyranoHandler just checks if MQTT is available
    if (!bCyranoConnected && mqttClient.isConnected()) {
      bSoftwareIsLive = true;
      bmqttCyranoConnected = true;
      bCyranoConnected = true;
    }

    if (!budpCyranoConnected) { // Somehow we should call this only once. It
                                // will
                                // keep on trying for ever.

      if (CyranoHandlerudpRcv.listen(CyranoPort)) {
        ESP_LOGI(CYRANO_TAG, "%s", "Cyrano Listening on IP: ");
        ESP_LOGI(CYRANO_TAG, "%s", (WiFi.localIP().toString()).c_str());
        CyranoHandlerudpRcv.onPacket(
            [](AsyncUDPPacket packet) { ProcessCyranoPacket(packet); });
      }

      // NOTE: MQTT connection now started by Opp2Handler

      budpCyranoConnected = true;
      bCyranoConnected = true;
    }
  }
}

void CyranoHandler::PeriodicallyBroadcastStatus() {
  if (!bOKToSend)
    return;
  if (NextPeriodicalUpdate > millis())
    return;
  NextPeriodicalUpdate = millis() + 17000;
  SendInfoMessage();
  return;
}

// Publish a minimal parry event JSON to MQTT
void CyranoHandler::publishParryEvent(bool state, uint64_t timestamp_ms) {
  if (!mqttClient.isConnected())
    return;
  std::ostringstream oss;
  oss << "{\"event\":\"parry\",\"ts\":" << timestamp_ms
      << ",\"state\":" << (state ? 1 : 0) << "}";
  // Use a dedicated topic for parry events
  std::string topic = std::string(mqttPublishTopic) + "/Parry";
  mqttClient.publish(topic.c_str(), 0, false, oss.str().c_str());
}