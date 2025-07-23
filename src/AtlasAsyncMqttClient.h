//Copyright (c) Piet Wauters 2025 <piet.wauters@gmail.com>
#ifndef ATLAS_ASYNC_MQTT_CLIENT_H
#define ATLAS_ASYNC_MQTT_CLIENT_H
#include "Singleton.h"
#include <WiFi.h>
#include "esp_event.h"
#include "esp_tls.h"
#include "mqtt_client.h"
#include <functional>
#include <string>
#include "IPAddress.h" // ESP-IDF's IPAddress or your wrapper
#include <Preferences.h>
#include "esp_log.h"
#include "MDNSResolver.h"

typedef std::function<void(bool sessionPresent)> mqtt_connect_cb_t;
typedef std::function<void()> mqtt_disconnect_cb_t;
typedef std::function<void(const char* topic, const char* payload, size_t len)> mqtt_message_cb_t;
typedef std::function<void(uint16_t packetId, uint8_t qos)> mqtt_subscribe_cb_t;

class AtlasAsyncMqttClient : public SingletonMixin<AtlasAsyncMqttClient> {
public:
    virtual ~AtlasAsyncMqttClient();

    void begin();
    void setServer(IPAddress ip, uint16_t port);
    void setServer(const std::string host, uint16_t port);
    void setCredentials(const char* username, const char* password = nullptr);
    void setClientId(const char* clientId);

    void onConnect(mqtt_connect_cb_t);
    void onDisconnect(mqtt_disconnect_cb_t);
    void onMessage(mqtt_message_cb_t);
    void onSubscribe(mqtt_subscribe_cb_t);

    void publish(const char* topic, int qos, bool retain, const char* payload);
    void publish(const char* topic, int qos, bool retain, const void* payload, size_t length);

    // Optional: for safer string handling
    void publishString(const char* topic, int qos, bool retain, const std::string& payload);

    void subscribe(const char* topic, int qos = 0);

    void disconnect();
    void reconnectWithNewSettings();

    void setWill(const char* topic, const char* message, int qos = 0, bool retain = false);
    void clearWill();

    void setTlsCerts(const char* cert_pem, const char* key_pem = nullptr, const char* ca_pem = nullptr);
    void setTLS(bool enable, const std::string& caCert = "") {
    m_tlsEnabled = enable;
    m_ca_cert = caCert;
    m_port = 8883;
  };


private:
    AtlasAsyncMqttClient(); // singleton protected

    friend class SingletonMixin<AtlasAsyncMqttClient>;

    bool m_HasBegun = false;
    esp_mqtt_client_handle_t client = nullptr;

    std::string m_host;
    uint16_t m_port = 1883;
    bool m_tlsEnabled = false;
    std::string m_username;
    std::string m_password;
    std::string m_clientId;

    mqtt_connect_cb_t connectCb;
    mqtt_disconnect_cb_t disconnectCb;
    mqtt_message_cb_t messageCb;
    mqtt_subscribe_cb_t subscribeCb;

    // --- Last Will and Testament (LWT) ---
    std::string m_lwtTopic;
    std::string m_lwtMessage;
    int m_lwtQos = 0;
    bool m_lwtRetain = false;
    bool m_lwtEnabled = false;

    std::string m_ca_cert;  // For TLS server verification (PEM format)


    static esp_err_t mqttEventHandlerCb(esp_mqtt_event_handle_t event);
    static void eventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    void handleEvent(esp_mqtt_event_handle_t event);

};

#endif // ATLAS_ASYNC_MQTT_CLIENT_H
