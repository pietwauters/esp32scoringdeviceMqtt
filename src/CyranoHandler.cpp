#include "CyranoHandler.h"
#include "AbsoluteTime.h"
#include "EFP1Message.h"
#include "MDNSResolver.h"
#include "Opp2Handler.h"
#include <esp_log.h>
#include <string>

extern const char ca_cert_pem[];
extern const size_t ca_cert_pem_len;

static const char *CYRANO_TAG = "Cyrano";

// Set true to broadcast INFO on the subnet so CMS software can discover online
// pistes without prior configuration. Disabled: we send unicast only, which
// avoids unnecessary traffic. Some commercial CMS products rely on broadcast
// discovery — re-enable here if needed.
static constexpr bool kCyranoBroadcastEnabled = false;
const char *mdnsName = "openpiste";
CyranoHandler::CyranoHandler() : m_CachedStatusValid(false) {
  // ctor
  // Note: State now managed by OPP2::SystemState in Opp2Handler
  // Cached status will be populated on first state update
  m_CompetitionId[0] = '\0';
  m_CachedCyranoString[0] = '\0';
  m_CachedNextCyrano[0] = '\0';
  m_CachedPrevCyrano[0] = '\0';
}

auto &mqttClient = AtlasAsyncMqttClient::getInstance();
char mqttServer[16];           // MQTT Broker address
const int mqttPort = 1883;     // MQTT Broker port (testing: no TLS)
const char *mqttUser = "";     // MQTT username (optional)
const char *mqttPassword = ""; // MQTT password (optional)
char *mqttClientId;            // MQTT Client ID
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
  // Mutate m_CachedStatus in place rather than copying to a local EFP1Message
  // (~1.3KB on the stack) — this runs in UDP callback context (async_udp
  // task, ~4KB stack) via updateFromCyranoMessage()->updateCachedStatus().
  // Command/CompetitionId are unconditionally overwritten here on every
  // call, so nothing depends on their prior value.
  m_CachedStatus.Set(Command, "INFO");
  m_CachedStatus.Set(CompetitionId, m_CompetitionId);

  // Build and cache the INFO, NEXT, PREV Cyrano wire strings
  // CRITICAL: MakeNext/PrevMessageString() use CompetitionId, so it must be set above
  m_CachedStatus.ToString(m_CachedCyranoString, sizeof(m_CachedCyranoString));
  m_CachedStatus.MakeNextMessageString(m_CachedNextCyrano,
                                       sizeof(m_CachedNextCyrano));
  m_CachedStatus.MakePrevMessageString(m_CachedPrevCyrano,
                                       sizeof(m_CachedPrevCyrano));

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

  // NOTE: LWT set by Opp2Handler (OPP2 is primary protocol)
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

  // Logged here, not at the EVENT_CYRANO_SEND_INFO call site -- moved
  // 2026-08-14: that call site logged "Sending INFO message"
  // unconditionally, before either of the two early-returns above, so it
  // fired (misleadingly) on every state change even with no CMS ever
  // connected (bOKToSend stays false until a real HELLO arrives) --
  // nothing was actually going out despite the log claiming otherwise.
  ESP_LOGI(CYRANO_TAG, "[Opp2→Cyrano] Sending INFO message");

  // Use cached string directly - zero stack allocations
  const char *pCyranoMsg = m_CachedCyranoString;
  size_t cyranoLen = strlen(m_CachedCyranoString);

  CyranoHandlerudpRcv.writeTo((uint8_t *)pCyranoMsg, cyranoLen,
                              SoftwareIPAddress(), CyranoBroadcastPort,
                              TCPIP_ADAPTER_IF_STA);
  if (kCyranoBroadcastEnabled)
    CyranoHandlerudpBroadcast.broadcastTo((uint8_t *)pCyranoMsg, cyranoLen,
                                          CyranoBroadcastPort,
                                          TCPIP_ADAPTER_IF_STA);

  // Level 1: mirror raw EFP1.1 payload to MQTT
  if (mqttClient.isConnected()) {
    char efp1Topic[64];
    char pisteId[OPP2::PISTE_ID_MAX];
    Opp2Handler::getInstance().getPisteId(pisteId);
    snprintf(efp1Topic, sizeof(efp1Topic), "openpiste/%s/apparatus/efp1", pisteId);
    mqttClient.publish(efp1Topic, 0, false, pCyranoMsg, cyranoLen);
  }

  return;
}

void CyranoHandler::ProcessMessageFromSoftware(const EFP1Message &input,
                                               bool bVerifyPisteID) {
  if (bVerifyPisteID) {
    // Get piste ID from canonical OPP2 state (stack-efficient)
    char pisteId[OPP2::PISTE_ID_MAX];
    Opp2Handler::getInstance().getPisteId(pisteId);
    if (strcmp(input.Get(PisteId), pisteId) != 0)
      return; // wrong Piste
  }
  switch (input.GetType()) {
  case HELLO:
    bOKToSend = true;
    bSoftwareIsLive = true;
    LastHelloReception = millis();
    strncpy(m_CompetitionId, input.Get(CompetitionId),
            sizeof(m_CompetitionId) - 1);
    m_CompetitionId[sizeof(m_CompetitionId) - 1] = '\0';

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
    // Send INFO message with current state -- SendInfoMessage() itself
    // logs, only once it knows it's actually past the bOKToSend gate.
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
      const char *pCyranoMsg = m_CachedNextCyrano;
      size_t cyranoLen = strlen(m_CachedNextCyrano);
      CyranoHandlerudpRcv.writeTo((uint8_t *)pCyranoMsg, cyranoLen,
                                  SoftwareIPAddress(), CyranoBroadcastPort,
                                  TCPIP_ADAPTER_IF_STA);
      if (mqttClient.isConnected()) {
        char efp1Topic[64];
        char pisteId[OPP2::PISTE_ID_MAX];
        Opp2Handler::getInstance().getPisteId(pisteId);
        snprintf(efp1Topic, sizeof(efp1Topic), "openpiste/%s/apparatus/efp1", pisteId);
        mqttClient.publish(efp1Topic, 0, false, pCyranoMsg, cyranoLen);
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
      const char *pCyranoMsg = m_CachedPrevCyrano;
      size_t cyranoLen = strlen(m_CachedPrevCyrano);
      CyranoHandlerudpRcv.writeTo((uint8_t *)pCyranoMsg, cyranoLen,
                                  SoftwareIPAddress(), CyranoBroadcastPort,
                                  TCPIP_ADAPTER_IF_STA);
      if (mqttClient.isConnected()) {
        char efp1Topic[64];
        char pisteId[OPP2::PISTE_ID_MAX];
        Opp2Handler::getInstance().getPisteId(pisteId);
        snprintf(efp1Topic, sizeof(efp1Topic), "openpiste/%s/apparatus/efp1", pisteId);
        mqttClient.publish(efp1Topic, 0, false, pCyranoMsg, cyranoLen);
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
  // Detect WiFi drop and reset connection flags so sockets are re-bound on
  // reconnect.  Also restart the HELLO timeout clock so the 40 s countdown
  // does not fire for time the CMS was simply unreachable.
  if (WiFi.status() != WL_CONNECTED) {
    if (bWifiConnected) {
      ESP_LOGW(CYRANO_TAG, "[Cyrano] WiFi lost — resetting UDP/MQTT flags");
      bWifiConnected = false;
      budpCyranoConnected = false;
      bmqttCyranoConnected = false;
      bCyranoConnected = false;
      LastHelloReception = millis();
    }
    return;
  }

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

    if (!budpCyranoConnected) {
      // budpCyranoConnected is only set true on an actual successful
      // listen() -- traced 2026-08-13: it was previously set unconditionally
      // here regardless of listen()'s return value, so a single failed
      // attempt (e.g. a timing race right after boot, before the network
      // stack is fully ready) permanently blocked this retry guard for the
      // rest of the boot session -- CheckConnection() runs every main-loop
      // iteration, but this block would never be entered again. Confirmed
      // on real hardware: nothing was bound to CyranoPort at all (verified
      // via ICMP port-unreachable), consistent with this exact failure mode.
      if (CyranoHandlerudpRcv.listen(CyranoPort)) {
        ESP_LOGI(CYRANO_TAG, "%s", "Cyrano Listening on IP: ");
        ESP_LOGI(CYRANO_TAG, "%s", (WiFi.localIP().toString()).c_str());
        CyranoHandlerudpRcv.onPacket(
            [](AsyncUDPPacket packet) { ProcessCyranoPacket(packet); });

        // NOTE: MQTT connection now started by Opp2Handler

        budpCyranoConnected = true;
        bCyranoConnected = true;
      } else {
        ESP_LOGW(CYRANO_TAG, "[Cyrano] listen(%u) failed, will retry", CyranoPort);
      }
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

