// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Compact web remote control (Atlas-app-style Main/Penalties layout),
// served from SPIFFS. Every button route translates directly into the same
// UI_INPUT_* event physical buttons and the existing OPRCP UDP remote
// protocol already produce (UDPIOHandler::InputChanged()) -- no new input
// path, no OPP2 spec change. See CLAUDE.md before touching this file.
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
  // Mounts SPIFFS and registers routes on NetWork::GetServer(). Call once
  // at boot, after NetWork::GlobalStartWiFi() (so startCalibrationWebServer
  // ()'s server.reset() has already run and won't wipe these routes) and
  // after UDPIOHandler/Opp2Handler exist (routes call both). Not started
  // in repeater mode, matching FPA422Handler/CyranoHandler/Opp2Handler
  // (see main.cpp).
  void begin();

private:
  friend class SingletonMixin<WebRemoteHandler>;
  WebRemoteHandler() = default;

  // Registers a POST route that does nothing but inject one UI_INPUT_*
  // event -- every button on the page is one of these.
  void registerUiRoute(const char *path, uint32_t eventtype);
  // Loads one plain-text SPIFFS file ONCE (call from begin(), not
  // per-request) into a heap buffer that is intentionally never freed, and
  // registers a route that serves that same persistent buffer on every
  // request via request->send() (AsyncBasicResponse). NOT
  // AsyncFileResponse/serveStatic()/AsyncProgmemResponse, and NOT gzip --
  // see the long comment above this function's definition in the .cpp for
  // the full history of what actually went wrong with those on real
  // hardware (not assumed, decoded crash backtraces and byte-level
  // evidence throughout).
  void serveSpiffsFile(const char *routePath, const char *filePath,
                        const char *contentType);
  void handleState(AsyncWebServerRequest *request);
};

#endif // WEBREMOTEHANDLER_H
