// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#include "WebRemoteHandler.h"
#include "EventDefinitions.h"
#include "Opp2Handler.h"
#include "UDPIOHandler.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "network.h"
#include "web_assets_generated.h"
#include <WiFi.h>
#include <cstdio>
#include <cstdlib>

static const char *WEB_REMOTE_TAG = "WebRemote";

// Concurrency guard -- traced 2026-08-12 to a real crash: several genuinely
// concurrent requests (a phone's poll() overlapping a button POST, or
// several browser-initiated connections at once) each build an
// AsyncWebServerResponse, and AsyncWebServerResponse::addHeader() allocates
// a std::list<AsyncWebHeader> node via operator new. Under this device's
// tight/fragmented heap (confirmed via /heap: free ~9-18KB, largest block
// sometimes down to ~3.5KB), that allocation can fail; ESPAsyncWebServer
// doesn't catch the resulting std::bad_alloc, so it propagates to
// std::terminate() -> abort() -> full panic/reboot. Reproduced directly:
// 5-6 concurrent requests survived one round, crashed the device on the
// next. Capping how many of THIS handler's requests are in flight at once
// keeps concurrent header/body allocations low enough to stay clear of
// that ceiling. Scoped to WebRemoteHandler's own routes only -- doesn't
// touch network.cpp's calibration server routes or LWIP's global
// CONFIG_LWIP_MAX_ACTIVE_TCP, which also gates the MQTT client's socket.
static constexpr int kMaxConcurrentRequests = 3;
static volatile int s_activeRequests = 0;

// Returns true if the caller should proceed handling the request. Returns
// false if a 503 has already been sent and the caller must return
// immediately without touching state. Only call once per request, before
// any allocation-heavy work.
static bool AcquireRequestSlot(AsyncWebServerRequest *request) {
  if (s_activeRequests >= kMaxConcurrentRequests) {
    ESP_LOGW(WEB_REMOTE_TAG, "[slot] REJECT %s (active=%d)",
             request->url().c_str(), s_activeRequests);
    request->send(503, "text/plain", "busy, retry shortly");
    return false;
  }
  s_activeRequests++;
  request->onDisconnect([]() { s_activeRequests--; });
  return true;
}

// NOT an exception-safety boundary, despite the name -- currently a
// passthrough. 2026-08-13: tried making this a try/catch wrapper so an
// allocation failure anywhere in a handler's call chain (a second crash
// site found in AsyncWebServerRequest::send() itself, distinct from the
// EFP1Message one) would be caught instead of reaching std::terminate().
// Doesn't compile: this project builds with CONFIG_COMPILER_CXX_EXCEPTIONS
// unset (sdkconfig.esp32dev), a project-wide setting, not a per-file one --
// "exception handling disabled, use -fexceptions to enable". The library
// code that throws (libstdc++, ESPAsyncWebServer) was built separately
// with exceptions on, which is why it throws at all; nothing in our own
// reachable call chain can catch it as things stand. Left as an identity
// wrapper (and every route left calling it) so flipping this in later only
// needs the try/catch put back here, not every call site revisited. See
// conversation with Piet 2026-08-13 for the options that don't require
// enabling exceptions project-wide.
static ArRequestHandlerFunction
Guarded(std::function<void(AsyncWebServerRequest *)> inner) {
  return inner;
}

// Registers a route serving one flash-resident gzipped asset (a
// static const uint8_t[] array in src/web_assets_generated.h, .rodata --
// no SPIFFS, no heap buffer, nothing to load at boot). beginResponse(code,
// type, const uint8_t*, len) -> AsyncProgmemResponse (explicit length,
// binary-safe -- gzip output contains embedded NUL bytes, confirmed by
// scanning it, which rules out the NUL-terminated-string response class
// used everywhere else in this file).
//
// This response class/ceiling combination was found the hard way, on real
// hardware, before this function existed in this form -- this device has
// a hard reliable single-response-size ceiling around 4KB (confirmed with
// a binary-search diagnostic route returning a configurable-size plain
// payload: 4000 bytes always came back intact, 5000+ came back empty or
// truncated), independent of which AsyncWebServer response class serves
// it or whether the content is compressed. The fix that matters is
// keeping every individual response comfortably under that ceiling:
// index.html's inline <script> lives in its own app.js (see
// WebRemoteHandler::begin()), and app.js is minified with terser before
// gzip (strip_web_assets.py) -- comment-stripping alone wasn't enough
// (JS alone gzipped to ~5.2KB, still over; terser's mangling + dead-code
// elimination got it to ~2.4KB). Switching from a SPIFFS-loaded heap
// buffer to this flash-resident array (2026-08-13, Branch 0 of
// docs/HEAP_FIX_IMPLEMENTATION_PLAN.md) didn't touch any of that -- same
// response class, same ceiling, only where the pointer comes from changed.
void WebRemoteHandler::serveFlashAsset(const char *routePath,
                                        const uint8_t *data, size_t len,
                                        const char *contentType) {
  NetWork::getInstance().GetServer().on(
      routePath, HTTP_GET,
      Guarded([data, len, contentType](AsyncWebServerRequest *request) {
        if (!AcquireRequestSlot(request))
          return;
        AsyncWebServerResponse *response =
            request->beginResponse(200, contentType, data, len);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
      }));
}

void WebRemoteHandler::registerUiRoute(const char *path, uint32_t eventtype) {
  NetWork::getInstance().GetServer().on(
      path, HTTP_POST, Guarded([eventtype](AsyncWebServerRequest *request) {
        if (!AcquireRequestSlot(request))
          return;
        // Same call physical buttons and the OPRCP UDP remote already make
        // (UDPIOHandler.cpp's oprcp_translate() decodes external UDP
        // frames into these exact same UI_INPUT_* events) -- this HTTP
        // route is just another local input source feeding the same,
        // already-hardened path.
        UDPIOHandler::getInstance().InputChanged(EVENT_UI_INPUT | eventtype);
        request->send(200, "text/plain", "OK");
      }));
}

// getStateCopy() (~500-600 bytes on the stack) is safe here -- this handler
// runs on AsyncTCP's own service task, not the constrained ~4KB async_udp
// task CLAUDE.md warns about (AsyncTCP.cpp: CONFIG_ASYNC_TCP_STACK_SIZE
// defaults to 16KB). Builds the JSON with a fixed stack buffer + snprintf,
// not ArduinoJson or std::string concatenation, matching this project's
// "no dynamic allocation in hot paths" convention even though this path
// isn't as constrained as async_udp.
// Minimal JSON string escaping for OPP2::Person::name/nation -- CMS-sourced
// text, not attacker-controlled, but still raw char[] that could in
// principle carry a '"' or '\' and break the hand-built JSON below (every
// other field here is numeric/bool, this handler had no string fields
// before fencer names). Strips control characters rather than \u-escaping
// them -- names don't legitimately contain any.
static void jsonEscape(const char *in, char *out, size_t outSize) {
  size_t o = 0;
  for (size_t i = 0; in[i] != '\0' && o + 2 < outSize; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
    } else if (c >= 0x20) {
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
}

// state.piste_id (Opp2Handler::Begin()) is either the Cyrano "fancy name"
// (a fixed color word -- Red/Blue/Yellow/Green/Podium, see AppSettings.cpp's
// WriteCyranoPisteName()) when one is configured, or just the bare piste
// number as a string otherwise -- never zero-padded, never "Piste_"-prefixed
// itself. This reproduces the same "Piste_XXX" display convention already
// used elsewhere (NetWork::GlobalStartWiFi()'s AP SSID, WifiSetupMode.cpp)
// for the bare-number case, purely for display here -- doesn't touch
// state.piste_id or the topic it's built from. A purely-numeric piste_id
// reliably means "no fancy name set": none of the fixed Cyrano color words
// are digit strings.
static void formatPisteLabel(const char *pisteId, char *out, size_t outSize) {
  bool allDigits = pisteId[0] != '\0';
  for (const char *p = pisteId; *p; p++) {
    if (*p < '0' || *p > '9') {
      allDigits = false;
      break;
    }
  }
  if (allDigits) {
    snprintf(out, outSize, "Piste_%03d", atoi(pisteId));
  } else {
    jsonEscape(pisteId, out, outSize);
  }
}

void WebRemoteHandler::handleState(AsyncWebServerRequest *request) {
  if (!AcquireRequestSlot(request))
    return;
  OPP2::SystemState state = Opp2Handler::getInstance().getStateCopy();

  char pisteLabel[40];
  formatPisteLabel(state.piste_id, pisteLabel, sizeof(pisteLabel));

  // Fencer name/NOC are only meaningful once a CMS has sent a DISP/fencers
  // message (CLAUDE.md's "Information state" domain) -- Person::present
  // is false until then, and the client only shows this when both sides
  // are present.
  char leftName[130], rightName[130], leftNoc[12], rightNoc[12];
  jsonEscape(state.fencers.left.fencer.name, leftName, sizeof(leftName));
  jsonEscape(state.fencers.right.fencer.name, rightName, sizeof(rightName));
  jsonEscape(state.fencers.left.fencer.nation, leftNoc, sizeof(leftNoc));
  jsonEscape(state.fencers.right.fencer.nation, rightNoc, sizeof(rightNoc));

  char buf[960];
  int len = snprintf(
      buf, sizeof(buf),
      "{\"apparatus_state\":%d,"
      "\"left\":{\"score\":%d,\"yellow_card\":%s,\"red_cards\":%u,\"black_card\":%s,\"p_card\":%u,"
      "\"fencer_present\":%s,\"fencer_name\":\"%s\",\"fencer_noc\":\"%s\"},"
      "\"right\":{\"score\":%d,\"yellow_card\":%s,\"red_cards\":%u,\"black_card\":%s,\"p_card\":%u,"
      "\"fencer_present\":%s,\"fencer_name\":\"%s\",\"fencer_noc\":\"%s\"},"
      "\"priority\":%d,"
      "\"clock\":{\"running\":%s,\"time_ms\":%u},"
      "\"round\":%u,"
      "\"weapon\":%d,"
      "\"match_num\":%u,"
      "\"phase_type\":%d,"
      "\"match_type\":%d,"
      "\"piste_label\":\"%s\"}",
      static_cast<int>(state.apparatus_state.state),
      state.score.left.score, state.score.left.yellow_card ? "true" : "false",
      state.score.left.red_cards, state.score.left.black_card ? "true" : "false",
      state.uw2f.left.p_card,
      state.fencers.left.fencer.present ? "true" : "false", leftName, leftNoc,
      state.score.right.score, state.score.right.yellow_card ? "true" : "false",
      state.score.right.red_cards, state.score.right.black_card ? "true" : "false",
      state.uw2f.right.p_card,
      state.fencers.right.fencer.present ? "true" : "false", rightName, rightNoc,
      static_cast<int>(state.score.priority),
      state.clock.running ? "true" : "false", state.clock.time_ms,
      state.match.round,
      static_cast<int>(state.match.weapon),
      state.match.match_num,
      static_cast<int>(state.match.phase_type),
      static_cast<int>(state.match.type),
      pisteLabel);

  if (len < 0 || static_cast<size_t>(len) >= sizeof(buf)) {
    request->send(500, "text/plain", "state too large");
    return;
  }
  request->send(200, "application/json", buf);
}

void WebRemoteHandler::begin() {
  // Not "/" -- network.cpp's startCalibrationWebServer() already owns the
  // exact path "/" (its own health-check handler, "Hi! I am ESP32."),
  // registered first at boot; a duplicate exact-match registration here
  // never won (confirmed: GET / kept returning the health-check text).
  serveFlashAsset("/remote", index_html_gz, index_html_gz_len, "text/html");
  serveFlashAsset("/style.css", style_css_gz, style_css_gz_len, "text/css");
  serveFlashAsset("/app.js", app_js_gz, app_js_gz_len, "application/javascript");

  registerUiRoute("/ui/toggle_timer", UI_INPUT_TOGGLE_TIMER);
  registerUiRoute("/ui/incr_score_left", UI_INPUT_INCR_SCORE_LEFT);
  registerUiRoute("/ui/decr_score_left", UI_INPUT_DECR_SCORE_LEFT);
  registerUiRoute("/ui/incr_score_right", UI_INPUT_INCR_SCORE_RIGHT);
  registerUiRoute("/ui/decr_score_right", UI_INPUT_DECR_SCORE_RIGHT);
  registerUiRoute("/ui/next_period", UI_NEXT_PERIOD);
  registerUiRoute("/ui/reset", UI_INPUT_RESET);
  registerUiRoute("/ui/yellow_card_left", UI_INPUT_YELLOW_CARD_LEFT);
  registerUiRoute("/ui/yellow_card_left_decr", UI_INPUT_YELLOW_CARD_LEFT_DECR);
  registerUiRoute("/ui/yellow_card_right", UI_INPUT_YELLOW_CARD_RIGHT);
  registerUiRoute("/ui/yellow_card_right_decr", UI_INPUT_YELLOW_CARD_RIGHT_DECR);
  registerUiRoute("/ui/red_card_left", UI_INPUT_RED_CARD_LEFT);
  registerUiRoute("/ui/red_card_left_decr", UI_INPUT_RED_CARD_LEFT_DECR);
  registerUiRoute("/ui/red_card_right", UI_INPUT_RED_CARD_RIGHT);
  registerUiRoute("/ui/red_card_right_decr", UI_INPUT_RED_CARD_RIGHT_DECR);
  registerUiRoute("/ui/black_card_left", UI_INPUT_BLACK_CARD_LEFT);
  registerUiRoute("/ui/black_card_left_decr", UI_INPUT_BLACK_CARD_LEFT_DECR);
  registerUiRoute("/ui/black_card_right", UI_INPUT_BLACK_CARD_RIGHT);
  registerUiRoute("/ui/black_card_right_decr", UI_INPUT_BLACK_CARD_RIGHT_DECR);
  registerUiRoute("/ui/p_card", UI_INPUT_P_CARD);
  registerUiRoute("/ui/p_card_undo", UI_INPUT_P_CARD_UNDO);
  registerUiRoute("/ui/prio", UI_INPUT_PRIO);
  registerUiRoute("/ui/restore_uw2f_timer", UI_INPUT_RESTORE_UW2F_TIMER);

  // Match/lifecycle (OPP2 view). Unlike the scoring/card events above,
  // these are handled directly in Opp2Handler::update(UDPIOHandler*), not
  // FencingStateMachine -- confirmed by reading both: FencingStateMachine
  // has no case for any of these, so they're not subject to its "ignore
  // everything except stop-timer while the clock runs" gate. They operate
  // on bout state (W/H/F/P/E, CLAUDE.md's "Bout state" domain), which is
  // independent of the physical clock. No client-side disable needed here
  // the way scoring buttons need it.
  registerUiRoute("/ui/cyrano_prev", UI_INPUT_CYRANO_PREV);
  registerUiRoute("/ui/cyrano_begin", UI_INPUT_CYRANO_BEGIN);
  registerUiRoute("/ui/cyrano_next", UI_INPUT_CYRANO_NEXT);
  registerUiRoute("/ui/cyrano_end", UI_INPUT_CYRANO_END);
  // UI_SWAP_FENCERS already had a complete handler in
  // Opp2Handler::ProcessUIEvents() (swaps fencers/score/lights/uw2f under
  // the mutex, flips priority, syncs FSM -- confirmed by reading it) and
  // is already reachable from the OPRCP UDP remote (UDPIOHandler.cpp), it
  // just had no web route or button before this.
  registerUiRoute("/ui/swap_fencers", UI_SWAP_FENCERS);
  // UI_RESERVE_LEFT/RIGHT, unlike swap above, have no handler anywhere in
  // Opp2Handler.cpp or FencingStateMachine.cpp -- CLAUDE.md already flags
  // this as a known gap (no reserve_active field on OPP2::FencerSide to
  // hold the flag yet). Routed anyway, at Piet's explicit request, so the
  // buttons exist consistently now and just need real wiring later --
  // tapping them currently has no visible effect.
  registerUiRoute("/ui/reserve_left", UI_RESERVE_LEFT);
  registerUiRoute("/ui/reserve_right", UI_RESERVE_RIGHT);
  // Cycle only (Foil -> Epee -> Sabre -> Foil...) -- FencingStateMachine
  // has no "set weapon to X" UI_INPUT event, only cycle (confirmed by
  // reading FencingStateMachine.cpp's UI_INPUT_CYCLE_WEAPON case).
  registerUiRoute("/ui/cycle_weapon", UI_INPUT_CYCLE_WEAPON);
  // Cycle only, same shape as weapon above. UI_INPUT_ROUND already existed
  // (FencingStateMachine.cpp:320, cycles m_nrOfRounds 1->2/3->3->9->1) and
  // already fed Opp2Handler's EVENT_ROUND derivation of match.phase_type/
  // match.type (Pool=1 round, DE=2or3, Team=9 -- Opp2Handler.cpp:1374) --
  // just never had a route or button before this.
  registerUiRoute("/ui/cycle_round", UI_INPUT_ROUND);
  // Cycle only (Low->Normal->High->UltraHigh->Low) -- UI_CYCLE_BRIGHTNESS
  // already existed and is already handled by both TimeScoreDisplay and
  // WS2812BLedStrip (confirmed by reading both), it just had no route or
  // physical/web trigger anywhere yet, same situation UI_INPUT_ROUND was
  // in before btnCycleRound got wired up.
  registerUiRoute("/ui/cycle_brightness", UI_CYCLE_BRIGHTNESS);

  // Menu page (WiFi/Settings/OTA/Full reset) -- WiFi and OTA route
  // through the exact same UI_INPUT_* -> NetWork::update(UDPIOHandler*)
  // path as every button above (UI_START_WIFI_PORTAL/UI_START_OTA_PORTAL
  // already existed as events, just had no live trigger before this).
  // Settings needs no route of its own -- AppSettings::begin() already
  // registers /settings directly. Full reset is a plain reboot
  // (UI_FULL_RESET's existing handler is just ESP.restart(), no NVS
  // wipe -- that's DoFactoryReset(), a different, boot-pin-triggered
  // path, deliberately not exposed here).
  registerUiRoute("/ui/start_wifi_portal", UI_START_WIFI_PORTAL);
  registerUiRoute("/ui/start_ota_portal", UI_START_OTA_PORTAL);
  registerUiRoute("/ui/full_reset", UI_FULL_RESET);

  NetWork::getInstance().GetServer().on(
      "/api/state", HTTP_GET,
      Guarded([this](AsyncWebServerRequest *request) { handleState(request); }));

  // /heap kept as permanent, cheap instrumentation (not removed like
  // /sizetest above it) -- this device's baseline free heap/largest block
  // turned out to be genuinely useful to be able to check live, given how
  // much tonight's debugging depended on it. No allocation of its own
  // (fixed stack buffer), safe regardless of what it finds.
  NetWork::getInstance().GetServer().on(
      "/heap", HTTP_GET, Guarded([](AsyncWebServerRequest *request) {
        if (!AcquireRequestSlot(request))
          return;
        char buf[128];
        snprintf(buf, sizeof(buf), "free=%u largest_block=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        request->send(200, "text/plain", buf);
      }));

  // No .begin() here -- NetWork::GetServer() is already started (and
  // periodically re-started after server.reset()) by
  // startCalibrationWebServer(); see network.cpp.
  ESP_LOGI(WEB_REMOTE_TAG, "Web remote routes registered");
}
