// Copyright (c) Piet Wauters 2026 <piet.wauters@gmail.com>
#ifndef BROKER_DISCOVERY_H
#define BROKER_DISCOVERY_H

#include "Singleton.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <string>

// Races the MQTT client's own static-IP reconnect against a background mDNS
// lookup for as long as the client isn't connected, and switches the client
// onto the mDNS-resolved address if that wins the race. Stops entirely once
// connected -- no periodic re-check while connected, by design (connected is
// connected, full stop). Resumes the race on the next disconnect.
class BrokerDiscovery : public SingletonMixin<BrokerDiscovery> {
public:
  // Called once from CyranoHandler::Begin(), right after the client has
  // already been pointed at the static/configured broker IP -- starts the
  // background mDNS race immediately, since nothing is connected yet.
  void Begin(const char *mdnsHostname, uint16_t port);

  // Hook into the MQTT client's onConnect/onDisconnect callbacks.
  void OnConnected();
  void OnDisconnected();

private:
  friend class SingletonMixin<BrokerDiscovery>;
  BrokerDiscovery();

  void StartSearching();
  static void searchTask(void *param);

  std::string m_MdnsHostname;
  uint16_t m_Port;
  volatile bool m_Searching;
  TaskHandle_t m_TaskHandle;
};

#endif // BROKER_DISCOVERY_H
