// Copyright (c) Piet Wauters 2022 <piet.wauters@gmail.com>

/*! \mainpage ESP32 Scoring Machine for fencing
 *
 * \section intro_sec Introduction
 *
 * For more information about this project pls. go to
 * https://github.com/pietwauters/esp32-scoring-device/wiki
 *
 * \section main_structure_sec Main structure
 * The esp32 has 2 cores. One core is used for the physical scoring device
 * sensor. The other core is used for everything else: State machine, timers,
 * network, Cyrano, Configuration Portal, ...
 *
 * \subsection The sensor
 *
 *
 */
// #include "LedMatrix.h"
#include "3WeaponSensor.h"
#include "AutoRef.h"
#include "CyranoHandler.h"
#include "FPA422Handler.h"
#include "FastADC1.h"
#include "FencingStateMachine.h"
#include "FlashWriteGuard.h"
#include "RepeaterReceiver.h"
#include "RepeaterSender.h"
#include "ResetHandler.h"
#include "TimeScoreDisplay.h"
#include "UDPIOHandler.h"
#include "WS2812BLedStrip.h"
#include "driver/adc.h"
#include "esp_clk.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "network.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

/*#include "ESP32Button.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <mqtt_client.h>
// Define the OLED display dimensions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32  // Adjust for your display's size
// Define custom I2C pins
#define SDA_PIN 4
#define SCL_PIN 2
// Create an instance of the display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1); // -1 for
default I2C address (0x3C) esp_mqtt_client_handle_t mqttClient;
*/

static const char *SET_UP_TAG = "SetUp";
static const char *LOOP_TAG = "Loop";

WS2812B_LedStrip *MyLedStrip;
TimeScoreDisplay *MyTimeScoreDisplay;
MultiWeaponSensor *MySensor;
NetWork *MyNetWork;
FencingStateMachine *MyStatemachine;
FPA422Handler *MyFPA422Handler;
UDPIOHandler *MyUDPIOHandler;
CyranoHandler *MyCyranoHandler;
RepeaterReceiver *MyRepeaterReiver;
RepeaterSender *MyRepeaterSender;

bool bIsRepeater = false;
bool bEnableDeepSleep = false;
int FactoryResetCounter = 50;

void setup() {
  esp_task_wdt_init(20, false);
  Serial.begin(115200);

  MyTimeScoreDisplay = new TimeScoreDisplay();
  MyTimeScoreDisplay->begin(); // this also powers up the led panels
  MyTimeScoreDisplay->LaunchStartupDisplay();

  MyLedStrip = &WS2812B_LedStrip::getInstance();
  MyLedStrip->begin();

  MyLedStrip->ClearAll();
  MyLedStrip->attach(*MyTimeScoreDisplay);
  /*
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason != ESP_RST_POWERON) {
      MyTimeScoreDisplay->DisplayResetReason(reset_reason);
      while (true) {
        esp_task_wdt_reset();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        MyTimeScoreDisplay->DisplayResetReason(reset_reason);
      }
    */
  MySensor = &MultiWeaponSensor::getInstance();
  MyLedStrip->ClearAll();
  MyStatemachine = &FencingStateMachine::getInstance();
  // put your setup code here, to run once:

  MyLedStrip->ClearAll();
  MyLedStrip->ClearAll();

  MyFPA422Handler = new FPA422Handler();
  Preferences mypreferences;
  mypreferences.begin("scoringdevice", RO_MODE);
  bIsRepeater = mypreferences.getBool("RepeaterMode", false);
  bEnableDeepSleep = mypreferences.getBool("Powersave", false);
  bool DisableBrownOut = mypreferences.getBool("DisableBrownout", true);
  FlashWriteGuard::init(DisableBrownOut); // capture brownout register, disable
                                          // detection globally
  mypreferences.end();

  MyNetWork = &NetWork::getInstance();
  printf("[setup] NetWork begin\n");
  MyNetWork->begin();
  printf("[setup] NetWork begin done\n");

  printf("[setup] GlobalStartWiFi\n");
  MyNetWork->GlobalStartWiFi();
  printf("[setup] GlobalStartWiFi done\n");

  ESP_LOGI(SET_UP_TAG, "%s", "Wifi started");

  MyUDPIOHandler = &UDPIOHandler::getInstance();
  printf("[setup] ConnectToAP\n");
  MyUDPIOHandler->ConnectToAP();
  printf("[setup] ConnectToAP done\n");
  MyUDPIOHandler->attach(*MyNetWork);

  // In repeater mode don't start these 2 tasks

  if (!bIsRepeater) {
    ESP_LOGI(SET_UP_TAG, "%s", "Bwahahaaha I am the master!");
    MyCyranoHandler = &CyranoHandler::getInstance();
    MyStatemachine->ResetAll();
    MyFPA422Handler->StartWiFi();
    MyStatemachine->attach(*MyFPA422Handler);
    MyUDPIOHandler->attach(*MyStatemachine);
    MyStatemachine->attach(*MyUDPIOHandler);
    MyStatemachine->attach(*MyCyranoHandler);
    MyUDPIOHandler->attach(*MyCyranoHandler);
    MyCyranoHandler->attach(*MyStatemachine);
    MyCyranoHandler->attach(*MyFPA422Handler);
    MySensor->attach(*MyStatemachine);
    MyStatemachine->RegisterMultiWeaponSensor(MySensor);
    printf("[setup] Statemachine begin\n");
    MyStatemachine->begin();
    printf("[setup] Statemachine begin done\n");
    printf("[setup] Sensor begin\n");
    MySensor->begin();
    printf("[setup] Sensor begin done\n");
    MyStatemachine->attach(*MyTimeScoreDisplay);
    printf("[setup] CyranoHandler Begin\n");
    MyCyranoHandler->Begin();
    printf("[setup] CyranoHandler Begin done\n");
    esp_task_wdt_add(
        NULL); // register main task with WDT only after slow mDNS lookup
    MyRepeaterSender = &RepeaterSender::getInstance();
    MyRepeaterSender->begin();
    MyStatemachine->attach(*MyRepeaterSender);
    MyStatemachine->attach(*MyLedStrip);
    MyStatemachine->SetMachineWeapon(MySensor->GetActualWeapon());
    printf("[setup] AutoRef begin\n");
    MyStatemachine->attach(AutoRef::getInstance());
    AutoRef::getInstance().begin();
    AutoRef::getInstance().setEnabled(false);
    printf("[setup] AutoRef begin done\n");
    MySensor->getLongHitDetector().attach(AutoRef::getInstance());
    MySensor->getDoubleHitDetector().attach(AutoRef::getInstance());
    switch (MySensor->GetActualWeapon()) {
    case FOIL:
      MyStatemachine->StateChanged(EVENT_WEAPON | WEAPON_MASK_FOIL);
      break;
    case EPEE:
      MyStatemachine->StateChanged(EVENT_WEAPON | WEAPON_MASK_EPEE);
      break;
    case SABRE:
      MyStatemachine->StateChanged(EVENT_WEAPON | WEAPON_MASK_SABRE);
      break;
    }
  } else {
    // When running in repeater mode
    MyRepeaterReiver = &RepeaterReceiver::getInstance();
    ESP_LOGI(SET_UP_TAG, "%s", "Ouch! I am a repeater!");
    MyRepeaterReiver->begin();
    MyRepeaterReiver->attach(*MyLedStrip);
    MyRepeaterReiver->attach(*MyTimeScoreDisplay);
    MyRepeaterReiver->StartWatchDog();
    MyLedStrip->SetMirroring(MyRepeaterReiver->Mirror());
  }
  ESP_LOGI(SET_UP_TAG, "%s", (WiFi.localIP().toString()).c_str());
  ESP_LOGI(SET_UP_TAG, "%s", "MAC address: ");
  ESP_LOGI(SET_UP_TAG, "%s", WiFi.macAddress().c_str());
  /*
  button = ESP32Button::getInstance(15);
  button->begin();
  // Initialize I2C with custom SDA and SCL pins
  Wire.begin(SDA_PIN, SCL_PIN);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.display();  // Display the initial content
    delay(2000);  // Pause for 2 seconds

    // Clear the display buffer
    display.clearDisplay();
    display.setTextSize(1);      // Normal 1:1 pixel scale
    display.setTextColor(SSD1306_WHITE); // Draw white text
    display.setCursor(0, 0);     // Start at top left corner
    display.print(F("Hello, Claude!"));
    display.setCursor(0, 8);     // Start at top left corner
    display.print(F("The I2C interface"));
    display.setCursor(0, 16);     // Start at top left corner
    display.print(F("is working!!!!"));
    display.setCursor(0, 25);     // Start at top left corner
    display.print(F("Greetings. Piet"));
    display.display();  // Show the content on the screen
    */
  int freq_mhz = esp_clk_cpu_freq() / 1000000;
  printf("CPU frequency: %d MHz\n", freq_mhz);

  MyStatemachine->update(MyUDPIOHandler, EVENT_UI_INPUT | UI_INPUT_RESET);
  printf("Setup complete\n");
}

// extern HardwareSerial MySerial;

void loop() {

  /*  // put your main code here, to run repeatedly:
    button->doUpdate();
    if (button->stateHasChanged()) {
        MySerial.println(button->isPressed() ? "Button Pressed" : "Button
    Released");
    }
  */
  vTaskDelay(1 / portTICK_PERIOD_MS);
  if (!bIsRepeater) {
    MyFPA422Handler->WifiPeriodicalUpdate();
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);
    MyTimeScoreDisplay->ProcessEvents();
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);
    MyFPA422Handler->WifiPeriodicalUpdate();
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);
    if (MyNetWork->IsExternalWifiAvailable()) {
      // MyCyranoHandler->PeriodicallyBroadcastStatus();
      esp_task_wdt_reset();
      vTaskDelay(1 / portTICK_PERIOD_MS);
      MyCyranoHandler->CheckConnection();
      // MyFPA422Handler->WifiPeriodicalUpdate();  // Not really needed because
      // already done above
    }
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);

    if (MyStatemachine->IsConnectedToRemote()) {
      MyTimeScoreDisplay->CycleScoreMatchAndTimeWhenNotFighting();
      esp_task_wdt_reset();
      vTaskDelay(1 / portTICK_PERIOD_MS);
      MyFPA422Handler->WifiPeriodicalUpdate();
      MyFPA422Handler->WifiPeriodicalUpdate();
    }

    MyStatemachine->PeriodicallyBroadcastFullState(
        MyRepeaterSender, FULL_STATUS_REPETITION_PERIOD);
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);
    MyRepeaterSender->RepeatLastMessage();
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);
    // MyRepeaterSender->BroadcastHeartBeat();

    /*if (bEnableDeepSleep && MyStatemachine->GoToSleep()) {
      // prepareforDeepSleep();
    }*/
  } else { // when in repeater mode
    MyTimeScoreDisplay->ProcessEvents();
    MyTimeScoreDisplay->CycleScoreMatchAndTimeWhenNotFighting();
    esp_task_wdt_reset();
    vTaskDelay(1 / portTICK_PERIOD_MS);

    if (MyRepeaterReiver->IsWatchDogTriggered()) { // We lost connection with
                                                   // the master scoring device
      // clear displays and start looking for MasterId
      MyTimeScoreDisplay->DisplayPisteId();
      MyLedStrip->ClearAll();
      MyNetWork->FindAndSetMasterChannel(1, false);
    }
  }
  esp_task_wdt_reset();
  if (!digitalRead(0)) {
    if (!FactoryResetCounter) {
      ESP_LOGI(LOOP_TAG, "%s", "Bootpin pressed");
      Serial.println("Bootpin pressed");
      MyNetWork->DoFactoryReset();
      ESP.restart();
      FactoryResetCounter = 200;
    } else
      FactoryResetCounter--;
  } else {
    FactoryResetCounter = 200;
  }
}

extern "C" void app_main() {
  // Call Arduino setup and loop
  initArduino(); // Initialize Arduino if needed
  setup();       // Call the Arduino setup function
  printf("At start of loop\n");
  esp_task_wdt_init(20, false);

  while (true) {
    loop(); // Call the Arduino loop function
  }
}
