//Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>
#include "UDPIOHandler.h"

#include <WiFi.h>
#include "AsyncUDP.h"
#include "esp_log.h"
static const char* UDPIO_HANDLER_TAG = "UDPIOHandler";



static bool bWifiConnected = false;
static bool bUDPConnected = false;
static bool bFirstBind = true; // UI_INPUT_RESET fires only on first bind, not on reconnects

// CRC-8/SMBUS: poly 0x07, init 0x00, no reflection (OPRCP spec Section 8.1)
static uint8_t crc8_smbus(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

// Translate an OPRCP 4-byte big-endian command frame (OPRCP spec Section 5) to
// a firmware UI event word (EVENT_UI_INPUT | UI_INPUT_xxx).
// Returns 0 for unimplemented or unrecognised commands.
static uint32_t oprcp_translate(uint32_t frame)
{
    const uint8_t  cls   = (frame >> 24) & 0xFF;
    const bool     undo  = (frame >> 23) & 0x01;
    const bool     cycle = (frame >> 22) & 0x01;
    const uint8_t  cmd   = (frame >> 16) & 0x3F;
    const uint16_t param = frame & 0xFFFF;
    const uint8_t  side  = (param >> 8) & 0xFF; // 0x01=left 0x02=right

    switch (cls) {

    case 0x01: // CLOCK
        if (cmd == 0x00) { // START / STOP / TOGGLE
            if (cycle) return EVENT_UI_INPUT | UI_INPUT_TOGGLE_TIMER;
            return EVENT_UI_INPUT | (undo ? UI_INPUT_STOP_TIMER : UI_INPUT_START_TIMER);
        }
        // 0x01 RESET and 0x02 SET not yet implemented in firmware
        ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP CLOCK cmd=0x%02X not implemented", cmd);
        return 0;

    case 0x02: // SCORE — INCREMENT (only cmd; UNDO = decrement)
        if (cmd == 0x00) {
            if (side == 0x01) return EVENT_UI_INPUT | (undo ? UI_INPUT_DECR_SCORE_LEFT  : UI_INPUT_INCR_SCORE_LEFT);
            if (side == 0x02) return EVENT_UI_INPUT | (undo ? UI_INPUT_DECR_SCORE_RIGHT : UI_INPUT_INCR_SCORE_RIGHT);
        }
        break;

    case 0x03: // CARD
        switch (cmd) {
        case 0x00: // YELLOW
            if (side == 0x01) return EVENT_UI_INPUT | (undo ? UI_INPUT_YELLOW_CARD_LEFT_DECR  : UI_INPUT_YELLOW_CARD_LEFT);
            if (side == 0x02) return EVENT_UI_INPUT | (undo ? UI_INPUT_YELLOW_CARD_RIGHT_DECR : UI_INPUT_YELLOW_CARD_RIGHT);
            break;
        case 0x01: // RED
            if (side == 0x01) return EVENT_UI_INPUT | (undo ? UI_INPUT_RED_CARD_LEFT_DECR  : UI_INPUT_RED_CARD_LEFT);
            if (side == 0x02) return EVENT_UI_INPUT | (undo ? UI_INPUT_RED_CARD_RIGHT_DECR : UI_INPUT_RED_CARD_RIGHT);
            break;
        case 0x02: // BLACK
            if (side == 0x01) return EVENT_UI_INPUT | (undo ? UI_INPUT_BLACK_CARD_LEFT_DECR  : UI_INPUT_BLACK_CARD_LEFT);
            if (side == 0x02) return EVENT_UI_INPUT | (undo ? UI_INPUT_BLACK_CARD_RIGHT_DECR : UI_INPUT_BLACK_CARD_RIGHT);
            break;
        case 0x03: // P_YELLOW — deprecated in OPRCP spec
            ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP CARD P_YELLOW is deprecated, ignored");
            return 0;
        case 0x04: // P_RED — P-card (side not encoded in current firmware event)
            return EVENT_UI_INPUT | (undo ? UI_INPUT_P_CARD_UNDO : UI_INPUT_P_CARD);
        case 0x05: // P_BLACK
            if (side == 0x01) return EVENT_UI_INPUT | (undo ? UI_INPUT_BLACK_PCARD_LEFT_DECR  : UI_INPUT_BLACK_PCARD_LEFT);
            if (side == 0x02) return EVENT_UI_INPUT | (undo ? UI_INPUT_BLACK_PCARD_RIGHT_DECR : UI_INPUT_BLACK_PCARD_RIGHT);
            break;
        case 0x06: // UNWILLINGNESS → UW2F buzz; UNDO → restore timer
            return EVENT_UI_INPUT | (undo ? UI_INPUT_RESTORE_UW2F_TIMER : UI_INPUT_BUZZ);
        }
        break;

    case 0x04: // MATCH
        switch (cmd) {
        case 0x00: // NEXT_PERIOD / PREV_PERIOD
            if (undo) { ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP MATCH PREV_PERIOD not implemented"); return 0; }
            return EVENT_UI_INPUT | UI_NEXT_PERIOD;
        case 0x01: // RESET
            return EVENT_UI_INPUT | UI_INPUT_RESET;
        case 0x02: // PRIORITY_LEFT — UI_INPUT_PRIO does not encode side yet
        case 0x03: // PRIORITY_RIGHT
        case 0x04: // PRIORITY_AUTO — random draw not implemented; toggles priority
            return EVENT_UI_INPUT | UI_INPUT_PRIO;
        }
        break;

    case 0x05: // BREAK — not yet implemented in firmware
        ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP BREAK cmd=0x%02X not implemented", cmd);
        return 0;

    case 0x06: // COMPETITION
        switch (cmd) {
        case 0x00: return EVENT_UI_INPUT | UI_INPUT_CYRANO_BEGIN;
        case 0x01: return EVENT_UI_INPUT | UI_INPUT_CYRANO_END;
        case 0x02: return EVENT_UI_INPUT | (undo ? UI_INPUT_CYRANO_PREV : UI_INPUT_CYRANO_NEXT);
        case 0x03: // RESERVE
            if (side == 0x01) return EVENT_UI_INPUT | UI_RESERVE_LEFT;
            if (side == 0x02) return EVENT_UI_INPUT | UI_RESERVE_RIGHT;
            break;
        case 0x04: return EVENT_UI_INPUT | UI_SWAP_FENCERS;
        }
        break;

    case 0x07: // SYSTEM — handled by caller
        return 0;

    case 0x08: // SETTINGS
        if (cmd == 0x00) { // WEAPON
            if (cycle) return EVENT_UI_INPUT | UI_INPUT_CYCLE_WEAPON;
            ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP SETTINGS WEAPON explicit value not implemented");
            return 0;
        }
        ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP SETTINGS cmd=0x%02X not implemented", cmd);
        return 0;
    }

    ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP unhandled: cls=0x%02X cmd=0x%02X U=%d C=%d param=0x%04X",
             cls, cmd, (int)undo, (int)cycle, (unsigned)param);
    return 0;
}

static void ProcessOPRCPPacket(AsyncUDPPacket packet)
{
    if (packet.length() != 6) {
        ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP: bad length %u (expected 6)", (unsigned)packet.length());
        return;
    }

    const uint8_t *buf = packet.data();

    if (crc8_smbus(buf, 5) != buf[5]) {
        ESP_LOGW(UDPIO_HANDLER_TAG, "OPRCP: CRC mismatch, discarding");
        return;
    }

    const uint32_t frame = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] <<  8) |  (uint32_t)buf[3];
    const uint8_t  seq   = buf[4];

    // Deduplicate burst copies: same (frame, seq) within 500 ms window
    static uint32_t s_dedupFrame = 0;
    static uint8_t  s_dedupSeq   = 0xFF;
    static uint32_t s_dedupMs    = 0;
    const uint32_t  now = (uint32_t)millis();
    if (frame == s_dedupFrame && seq == s_dedupSeq && (now - s_dedupMs) < 500U)
        return;
    s_dedupFrame = frame;
    s_dedupSeq   = seq;
    s_dedupMs    = now;

    const uint8_t cls = (frame >> 24) & 0xFF;
    if (cls == 0x07) {
        const uint8_t cmd = (frame >> 16) & 0x3F;
        ESP_LOGI(UDPIO_HANDLER_TAG, "OPRCP SYSTEM cmd=0x%02X (not yet implemented)", cmd);
        return;
    }

    const uint32_t uiEvent = oprcp_translate(frame);
    if (uiEvent == 0)
        return;

    ESP_LOGI(UDPIO_HANDLER_TAG, "OPRCP frame=0x%08X seq=%u -> UI 0x%08X",
             (unsigned)frame, (unsigned)seq, (unsigned)uiEvent);
    UDPIOHandler::getInstance().InputChanged(uiEvent);
}

void ProcessUDPPacket (AsyncUDPPacket packet)
{
  UDPIOHandler &MyUDPIOHandler = UDPIOHandler::getInstance();
  mix_t Event;
  Event.theBytes[0] = packet.data()[0];
  Event.theBytes[1] = packet.data()[1];
  Event.theBytes[2] = packet.data()[2];
  Event.theBytes[3] = packet.data()[3];
  MyUDPIOHandler.InputChanged(Event.theDWord);
}

void UDPIOHandler::UDPCheck()
{
static long NextTimeToCheckUDP = millis() + 2500;
UDPIOHandler &MyUDPIOHandler = UDPIOHandler::getInstance();
  /*if(millis() < NextTimeToCheckUDP)
    return;*/
  NextTimeToCheckUDP = millis() + 2500;
#ifdef HOMENETWORK
  if(WiFi.status() != WL_CONNECTED)
  {
    if(bWifiConnected)
    {
      ESP_LOGW(UDPIO_HANDLER_TAG, "WiFi lost — resetting UDP connection flags");
      bWifiConnected = false;
      bUDPConnected = false;
    }
    return;
  }

  if(WiFi.status() == WL_CONNECTED)
  {
    if(!bWifiConnected)
    {
      bWifiConnected = true;
    }

    if(!bUDPConnected)
    {// Somewhere we should call this only once. It will keep on trying for ever.

      if(Commandudp.listen(1234))
      {
        ESP_LOGI(UDPIO_HANDLER_TAG, "UDP Listening on IP: %s",(WiFi.localIP().toString()).c_str());

        Commandudp.onPacket([](AsyncUDPPacket packet) {
          ProcessUDPPacket (packet);
        });
      }
      if(m_OPRCPudp.listen(4242))
      {
        ESP_LOGI(UDPIO_HANDLER_TAG, "OPRCP listening on port 4242");
        m_OPRCPudp.onPacket([](AsyncUDPPacket packet) {
          ProcessOPRCPPacket(packet);
        });
      }
      if (bFirstBind) {
        MyUDPIOHandler.InputChanged(UI_INPUT_RESET | EVENT_UI_INPUT);
        bFirstBind = false;
      }
      bUDPConnected = true;
    }

  }

#else
    //if(bWifiConnected)
    {
      if(!bUDPConnected)
      {// Somehow we should call this only once. It will keep on trying for ever.

        if(Commandudp.listen(1234))
        {
          ESP_LOGI(UDPIO_HANDLER_TAG, "UDP Listening on IP: %s",(WiFi.softAPIP().toString()).c_str());

          Commandudp.onPacket([](AsyncUDPPacket packet) {
            ProcessUDPPacket (packet);
          });
        }
        if(m_OPRCPudp.listen(4242))
        {
          ESP_LOGI(UDPIO_HANDLER_TAG, "OPRCP listening on port 4242");
          m_OPRCPudp.onPacket([](AsyncUDPPacket packet) {
            ProcessOPRCPPacket(packet);
          });
        }
        MyUDPIOHandler.InputChanged(UI_INPUT_RESET | EVENT_UI_INPUT);
        MyUDPIOHandler.InputChanged(UI_INPUT_RESET | EVENT_UI_INPUT);
        bUDPConnected = true;
      }

    }
#endif
}

void UDPIOHandler::ConnectToAP()
{
  UDPIOHandler &MyUDPIOHandler = UDPIOHandler::getInstance();
  if(!bUDPConnected)
  {// We should call this only once.
    bUDPConnected = true;
    if(Commandudp.listen(1234))
    {
      ESP_LOGI(UDPIO_HANDLER_TAG, "UDP Listening on IP: %s",(WiFi.softAPIP().toString()).c_str());
        Commandudp.onPacket([](AsyncUDPPacket packet) {
          ProcessUDPPacket (packet);
        });
    }
    if(m_OPRCPudp.listen(4242))
    {
      ESP_LOGI(UDPIO_HANDLER_TAG, "OPRCP listening on port 4242");
      m_OPRCPudp.onPacket([](AsyncUDPPacket packet) {
        ProcessOPRCPPacket(packet);
      });
    }
    MyUDPIOHandler.InputChanged(UI_INPUT_RESET | EVENT_UI_INPUT);
    MyUDPIOHandler.InputChanged(UI_INPUT_RESET | EVENT_UI_INPUT);
  }
}

void UDPIOHandler::run()
{
  UDPCheck();
  //if(UDP.connected())
    //UDP.run();

};

UDPIOHandler::UDPIOHandler()
{
    //ctor

}

void UDPIOHandler::Start()
{
  /*WiFi.hostname(espHostName);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);*/
}

UDPIOHandler::~UDPIOHandler()
{
    //dtor
}

// For now, below function is not used. It could be used to send back Lights
// Information to the app
void UDPIOHandler::ProcessLightsChange(uint32_t eventtype)
{
  uint32_t event_data = eventtype & SUB_TYPE_MASK;
/*  if(event_data & MASK_RED )
    ledRed.on();
  else
    ledRed.off();
  if(event_data & MASK_GREEN )
    ledGreen.on();
  else
    ledGreen.off();
  if(event_data & MASK_WHITE_L )
    ledWhiteL.on();
  else
    ledWhiteL.off();
  if(event_data & MASK_WHITE_R )
    ledWhiteR.on();
  else
    ledWhiteR.off();
  if(event_data & MASK_ORANGE_L )
    ledOrangeL.on();
  else
    ledOrangeL.off();
  if(event_data & MASK_ORANGE_R )
    ledOrangeR.on();
  else
        ledOrangeR.off();
        */
}


void UDPIOHandler::update (FencingStateMachine *subject, uint32_t eventtype)
{
  uint32_t event_data = eventtype & SUB_TYPE_MASK;
  uint32_t maineventtype = eventtype & MAIN_TYPE_MASK ;
  char chrono[16];
  char strRound[8];
  int newseconds;
  int currentRound, nrOfRounds;

  switch(maineventtype)
  {

    case EVENT_LIGHTS:
      ProcessLightsChange(eventtype);
    break;
    case EVENT_WEAPON:

    break;

    case EVENT_SCORE_LEFT:
      //UDP.virtualWrite(V3, eventtype & DATA_24BIT_MASK);
    break;

    case EVENT_SCORE_RIGHT:
      //UDP.virtualWrite(V4, eventtype & DATA_24BIT_MASK);
    break;

    case EVENT_TIMER_STATE:
      if(eventtype & DATA_24BIT_MASK)
      {
        //UDP.setProperty(V0,"color",UDP_GREEN);
        //UDP.virtualWrite(V1,1);
      }
      else
      {
        //UDP.setProperty(V0,"color",UDP_RED);
        //UDP.virtualWrite(V1,0);
      }
    break;
    case EVENT_TIMER:
    newseconds = event_data & (DATA_BYTE1_MASK |DATA_BYTE2_MASK);

      if(previous_seconds != newseconds)
      {
        subject->GetFormattedStringTime(chrono,2,0);
        //UDP.virtualWrite(V0, chrono);
        previous_seconds = newseconds;
      }

    break;

    case EVENT_ROUND:
      currentRound = event_data & DATA_BYTE0_MASK;
      nrOfRounds = (event_data & DATA_BYTE1_MASK)>>8;
      if(!currentRound || !nrOfRounds)
      {
        sprintf(strRound," ");

      }
      else
        sprintf(strRound,"%d/%d",currentRound,nrOfRounds);

      //UDP.virtualWrite(V11, strRound);

    break;



  }

}
