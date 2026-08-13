// Standalone host-side test: does ArduinoJson v6's StaticJsonDocument<N>
// (used throughout opp2_serialize.h) genuinely avoid heap allocation, as
// claimed in docs/HEAP_MEMORY_ANALYSIS.md's Option A? Proven, not assumed --
// overrides global operator new/delete to count allocations around real
// calls to OPP2::Serializer::serialize() for every message type this
// project actually publishes.
#include <cstdio>
#include <cstdlib>
#include <cstring>

static long g_allocCount = 0;
static size_t g_allocBytes = 0;

void *operator new(size_t size) {
  g_allocCount++;
  g_allocBytes += size;
  return malloc(size);
}
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void *operator new[](size_t size) {
  g_allocCount++;
  g_allocBytes += size;
  return malloc(size);
}
void operator delete[](void *p) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

#include "opp2_serialize.h"

static int g_failures = 0;

#define CHECK_NO_ALLOC(label, callExpr)                                      \
  do {                                                                       \
    long before = g_allocCount;                                             \
    callExpr;                                                                \
    long after = g_allocCount;                                              \
    printf("%-40s allocations=%ld\n", label, after - before);               \
    if (after != before) {                                                   \
      g_failures++;                                                          \
      printf("  FAIL: %s allocated %ld time(s) via operator new -- v6's "   \
             "StaticJsonDocument<N> is not actually heap-free here\n",       \
             label, after - before);                                        \
    }                                                                        \
  } while (0)

int main() {
  char buf[1024];

  using namespace OPP2;

  {
    Lights msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Lights)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Clock msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Clock)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Score msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Score)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Connection msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Connection)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    ApparatusStateMsg msg{};
    CHECK_NO_ALLOC("Serializer::serialize(ApparatusStateMsg)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Fencers msg{};
    strncpy(msg.right.fencer.name, "Test Fencer Name With Some Length",
            sizeof(msg.right.fencer.name) - 1);
    CHECK_NO_ALLOC("Serializer::serialize(Fencers)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Match msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Match)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    UW2F msg{};
    CHECK_NO_ALLOC("Serializer::serialize(UW2F)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    BladeContact msg{};
    CHECK_NO_ALLOC("Serializer::serialize(BladeContact)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Medical msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Medical)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  {
    Control msg{};
    CHECK_NO_ALLOC("Serializer::serialize(Control)",
                    Serializer::serialize(msg, buf, sizeof(buf)));
  }
  // VideoReview deliberately excluded -- not called anywhere in the
  // consumer project (esp32scoringdeviceMqtt), matches CLAUDE.md's own
  // "Not started" note there. The v6-compat fix in opp2_serialize.h
  // (createNestedObject() vs v7-only add<JsonObject>()) was verified
  // separately by the fact this whole test compiles and links at all --
  // that function is in the same translation unit.

  printf("\ntotal operator-new calls across all serialize() calls above: %ld "
         "(%zu bytes)\n",
         g_allocCount, g_allocBytes);
  printf("%s\n", g_failures == 0
                     ? "PASS: zero heap allocation confirmed for every "
                       "message type this project actually publishes"
                     : "FAIL: see above");
  return g_failures == 0 ? 0 : 1;
}
