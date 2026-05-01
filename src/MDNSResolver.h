#ifndef MDNS_RESOLVER_H
#define MDNS_RESOLVER_H

#include "Singleton.h"
#include <Arduino.h>
#include <WiFi.h>
class MDNSResolver : public SingletonMixin<MDNSResolver> {
public:
  // virtual ~MDNSResolver();
  void begin(const char *espHostname = "esp32");
  IPAddress lookupService(const char *service, const char *proto,
                          IPAddress fallback, const char *targetHostname);
  // Resolve a hostname (e.g., "Cyrano.local") to IP, fallback if not found
IPAddress resolveHostname(const char *hostname, IPAddress fallback);

private:
  // private methods
  /** Default constructor */
  friend class SingletonMixin<MDNSResolver>;
  MDNSResolver(){}; // tickPeriod in miliseconds
};

#endif
