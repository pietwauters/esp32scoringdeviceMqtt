// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
// Dedicated boot mode for reconfiguring WiFi -- replaces CaptivePortal's
// approach of scanning while the live app (AsyncWebServer, MQTT, FSM,
// everything) kept running. That approach was tried, fixed through
// several real bugs (a request race, an undersized scan timeout, a stuck
// safety net), and ultimately abandoned 2026-08-14: real-world testing
// still failed, and the remaining theory -- WiFi.scanNetworks() reliably
// conflicting with AsyncWebServer/AsyncTCP being alive, confirmed by
// elimination (calling task, blocking vs. async scan API, and MQTT
// activity were each individually tested and ruled out) -- meant the
// live app's own web server would need repeated end()/begin() cycling
// around every scan, which ESPAsyncWebServer likely isn't designed or
// tested for at all. Piet's call: reconfiguring WiFi is rare enough that
// it doesn't need to coexist with live operation -- do it as a clean
// reboot into a minimal, dedicated mode instead, matching the old
// WiFiManager flow's own core shape (full device takeover for the
// duration, reboot when done) but as an explicit boot-time specialization
// rather than trying to freeze/thaw pieces of the live app in place.
//
// This mode starts nothing else at all -- no FSM, no MQTT, no OPP2, no
// sensors, no AsyncWebServer -- just WiFi (AP only) and a plain
// synchronous WebServer (not AsyncWebServer/AsyncTCP) for the scan/
// connect form. WiFi.scanNetworks() (the ordinary blocking form) is
// exactly what NetWork::begin() already calls successfully at normal
// boot and what WiFiManager itself used for years without this problem
// -- both cases where nothing else was contending for the same
// resources, which this mode reproduces deliberately.
#ifndef WIFISETUPMODE_H
#define WIFISETUPMODE_H

namespace WifiSetupMode {

// True if the device should boot directly into Run() instead of the
// normal app -- check this as the very first thing in setup().
bool IsPending();

// Sets the pending flag and reboots. Called from the menu's WiFi button
// handler (on the live, normal AsyncWebServer) -- everything past this
// point happens after the reboot, in Run(). Never returns.
void RequestAndReboot();

// Runs the dedicated setup mode: AP-only WiFi (no STA connect attempt --
// nothing else running needs it, and this mode's whole purpose is
// picking a *different* network) plus a plain WebServer serving just a
// scan/connect form. Blocks forever in its own loop; always ends in
// ESP.restart() (successful or failed connect attempt, or this mode's
// own 5-minute idle timeout) -- never returns. Call as the very first
// thing in setup(), guarded by IsPending(), before anything else so
// there is nothing else running to conflict with the scan.
void Run();

} // namespace WifiSetupMode

#endif // WIFISETUPMODE_H
