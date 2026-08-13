// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Compact web remote control (Atlas-app-style Main/Penalties layout),
// served from flash-resident const arrays (src/web_assets_generated.h,
// regenerated from web_src/ by strip_web_assets.py at build time -- not
// SPIFFS; see that generated header's own comment and
// docs/HEAP_FIX_IMPLEMENTATION_PLAN.md's Branch 0 for why). Every button
// route translates directly into the same UI_INPUT_* event physical
// buttons and the existing OPRCP UDP remote protocol already produce
// (UDPIOHandler::InputChanged()) -- no new input path, no OPP2 spec
// change. See CLAUDE.md before touching this file.
//
// Registers its routes on NetWork::GetServer() (the device's one and only
// AsyncWebServer, port 80) -- NOT its own instance. A dedicated second
// AsyncWebServer was the original design and was flashed/tested on real
// hardware: it corrupted or hung every multi-packet response (>1 TCP
// segment) on BOTH servers, confirmed by disabling this one and re-testing
// -- the same route came back byte-perfect with only one AsyncWebServer
// instance active. See network.cpp's `server` declaration and
// startCalibrationWebServer() for the full writeup and the one known
// remaining gap (routes here get wiped by a runtime "Reconfigure WiFi",
// not just at boot).
#ifndef WEBREMOTEHANDLER_H
#define WEBREMOTEHANDLER_H
#include "Singleton.h"
// network.h (not <ESPAsyncWebServer.h> directly) -- it already pulls in
// <ESPAsyncWebServer.h> after <WiFiManager.h>, the only include order that
// doesn't collide (WiFiManager -> WebServer -> HTTP_Method.h vs.
// ESPAsyncWebServer.h both define HTTP_GET/HTTP_POST/etc., "conflicting
// declaration" if ESPAsyncWebServer.h wins the race). Including
// network.h here guarantees that order regardless of what else pulls this
// header in, or in what order.
#include "network.h"

class WebRemoteHandler : public SingletonMixin<WebRemoteHandler> {
public:
  // Registers routes on NetWork::GetServer(). Call once at boot, after
  // NetWork::GlobalStartWiFi() (so startCalibrationWebServer()'s
  // server.reset() has already run and won't wipe these routes) and after
  // UDPIOHandler/Opp2Handler exist (routes call both). Not started in
  // repeater mode, matching FPA422Handler/CyranoHandler/Opp2Handler (see
  // main.cpp). No filesystem to mount -- assets are compiled in.
  void begin();

private:
  friend class SingletonMixin<WebRemoteHandler>;
  WebRemoteHandler() = default;

  // Registers a POST route that does nothing but inject one UI_INPUT_*
  // event -- every button on the page is one of these.
  void registerUiRoute(const char *path, uint32_t eventtype);
  // Registers a GET route serving one flash-resident gzipped asset
  // (src/web_assets_generated.h's arrays -- .rodata, not heap, not
  // SPIFFS). request->beginResponse(code, type, const uint8_t*, len)
  // resolves to AsyncProgmemResponse, which reads via memcpy_P (plain
  // memcpy on ESP32) -- confirmed by reading ESPAsyncWebServer's own
  // source that it genuinely does not care whether that pointer is heap
  // or flash, so this needed no response-class change from the prior
  // SPIFFS-backed version, only where the pointer comes from.
  void serveFlashAsset(const char *routePath, const uint8_t *data,
                        size_t len, const char *contentType);
  void handleState(AsyncWebServerRequest *request);
};

#endif // WEBREMOTEHANDLER_H
