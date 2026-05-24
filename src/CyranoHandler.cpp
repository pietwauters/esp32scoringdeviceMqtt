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
  ESP_LOGI(CYRANO_TAG, "Connected to MQTT broker");
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
    bOKToSend = true;
    bSoftwareIsLive = true;
    LastHelloReception = millis();
    m_CompetitionId = input[CompetitionId];

    if (m_CachedStatusValid) {
      RebuildCachedStrings();
    }
    SendInfoMessage();
    break;

  case DISP: {

    OPP2::ApparatusState apparatusState;
    Opp2Handler::getInstance().updateFromCyranoMessage(input, apparatusState);
    StateChanged(EVENT_CYRANO_STATE_LOCKED); // Lock FSM — only BEGIN can unlock
    SendInfoMessage();                       // Reply to CMS with current state
  } break;

  case ACK:
    ClearOnACK();
    Opp2Handler::getInstance().ProcessCyranoACK(); // ENDING → WAITING
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
  // ────────────────────────────────────────────────────────────────────────
  // Button handling moved to Opp2Handler (canonical state owner)
  // Opp2Handler fires EVENT_CYRANO_SEND_XXX events to trigger message sends
  // This method kept for compatibility but does nothing
  // ────────────────────────────────────────────────────────────────────────
}

void CyranoHandler::update(Opp2Handler *subject, uint32_t eventtype) {
  // Handle message send requests from Opp2Handler
  switch (eventtype) {
  case EVENT_CYRANO_SEND_INFO:
    // Send INFO message with current state
    ESP_LOGI(CYRANO_TAG, "[Opp2→Cyrano] Sending INFO message");
    SendInfoMessage();
    break;

  case EVENT_CYRANO_SEND_NEXT:
    // Send NEXT message (WAITING state only per Cyrano spec)
    ESP_LOGI(CYRANO_TAG, "[Opp2→Cyrano] Sending NEXT message");
    if (!m_CachedStatusValid) {
      ESP_LOGW(CYRANO_TAG, "[Opp2→Cyrano] NEXT: Cache not valid yet");
      return;
    }
    {
      const char *pCyranoMsg = m_CachedNextCyrano.c_str();
      size_t cyranoLen = m_CachedNextCyrano.length();
      const char *pJsonMsg = m_CachedNextJson.c_str();
      size_t jsonLen = m_CachedNextJson.length();

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
    }
    break;

  case EVENT_CYRANO_SEND_PREV:
    // Send PREV message (WAITING state only per Cyrano spec)
    ESP_LOGI(CYRANO_TAG, "[Opp2→Cyrano] Sending PREV message");
    if (!m_CachedStatusValid) {
      ESP_LOGW(CYRANO_TAG, "[Opp2→Cyrano] PREV: Cache not valid yet");
      return;
    }
    {
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
    }
    break;

  case EVENT_CYRANO_STATE_H:
    m_State = HALT;
    break;
  case EVENT_CYRANO_STATE_W:
    m_State = WAITING;
    break;
  case EVENT_CYRANO_STATE_F:
    m_State = FENCING;
    break;
  case EVENT_CYRANO_STATE_P:
    m_State = PAUSE;
    break;
  case EVENT_CYRANO_STATE_E:
    m_State = ENDING;
    break;

  case EVENT_CYRANO_STATE_UNLOCKED:
    // FSM observes CyranoHandler (not Opp2Handler), so relay unlock to FSM
    StateChanged(EVENT_CYRANO_STATE_UNLOCKED);
    break;

  default:
    // Other events not handled
    break;
  }
}

#define MASK_ANY_ORANGE (MASK_ORANGE_L | MASK_ORANGE_R)

void CyranoHandler::ProcessLightsChange(uint32_t eventtype) {
  // Note: Lights already updated in Opp2Handler via updateLightsInternal
  // (Phase 2) No need to duplicate state here - just trigger Cyrano message
  // send (Handled in update() method which calls SendInfoMessage())
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