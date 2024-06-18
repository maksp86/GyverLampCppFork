#include "MqttClient.h"

#include <Arduino.h>
#define ARDUINOJSON_ENABLE_PROGMEM 1
#include <ArduinoJson.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

#include <Ticker.h>
#include <AsyncMqttClient.h>

#include "Settings.h"

namespace
{

    Ticker mqttReconnectTimer;
    Ticker updateTimer;

    MqttClient* object = nullptr;
    AsyncMqttClient* client = nullptr;

    String commonTopic;

    String availabilityTopic;
    String configTopic;
    String setTopic;
    String stateTopic;

    String clientId;

    void subscribe()
    {
        if (!client->connected()) {
            return;
        }
#ifdef USE_DEBUG
        Serial.print(F("Subscribing to topic: "));
        Serial.println(setTopic);
#endif
        client->subscribe(setTopic.c_str(), 0);
    }

    void sendString(String topic, String message, uint8_t qos = 2, bool retain = false)
    {
        if (!client->connected()) {
            return;
        }

        client->publish(topic.c_str(), qos, retain, message.c_str(), message.length(), false);
    }

    void sendState()
    {
        if (!client->connected()) {
            return;
        }

        String buffer;
        {
            DynamicJsonDocument doc(1024);
            JsonObject json = doc.to<JsonObject>();

            mySettings->buildJsonMqtt(json);

#ifdef USE_DEBUG
            Serial.println(F("Sending state"));
            Serial.println(stateTopic);
            serializeJsonPretty(doc, Serial);
            Serial.println();

#endif
            if (!serializeJson(doc, buffer)) {
#ifdef USE_DEBUG
                Serial.println(F("writing payload: wrong size!"));
#endif
            }
        }
        if (buffer.length() == 0) {
            return;
        }
        sendString(stateTopic, buffer, 2, true);
    }

    void staticUpdate()
    {
        if (!mqtt) {
            return;
        }

        sendState();
    }

    void sendAvailability()
    {
        if (!client->connected()) {
            return;
        }

#ifdef USE_DEBUG
        Serial.println(F("Sending availability"));
        Serial.println(availabilityTopic);
#endif
        sendString(availabilityTopic, F("true"), 2, true);
    }

    void sendDiscovery()
    {
        if (!client->connected()) {
            return;
        }

        String buffer;
        {
            DynamicJsonDocument doc(1024 * 5);
            doc[F("~")] = commonTopic;
            doc[F("name")] = mySettings->mqttSettings.name;
            doc[F("uniq_id")] = mySettings->mqttSettings.uniqueId;
            doc[F("cmd_t")] = F("~/set");
            doc[F("stat_t")] = F("~/state");
            doc[F("avty_t")] = F("~/available");
            doc[F("pl_avail")] = F("true");
            doc[F("pl_not_avail")] = F("false");
            doc[F("schema")] = F("json");
            doc[F("brightness")] = true;
            doc[F("effect")] = true;
            doc[F("rgb")] = true;
            doc[F("json_attr_t")] = F("~/state");

            JsonObject dev = doc.createNestedObject(F("dev"));
            dev[F("mf")] = mySettings->mqttSettings.manufacturer;
            dev[F("name")] = mySettings->mqttSettings.name;
            dev[F("mdl")] = mySettings->mqttSettings.model;
            JsonArray ids = dev.createNestedArray(F("ids"));
            ids.add(mySettings->mqttSettings.uniqueId);

            JsonArray effects = doc.createNestedArray(F("effect_list"));
            mySettings->writeEffectsMqtt(effects);

#ifdef USE_DEBUG
            Serial.println(F("Sending discovery"));
            Serial.println(configTopic);
            serializeJsonPretty(doc, Serial);
            Serial.println();
#endif


            if (!serializeJson(doc, buffer)) {
#ifdef USE_DEBUG
                Serial.println(F("writing payload: wrong size!"));
#endif
            }
        }
        if (buffer.length() == 0) {
            return;
        }
        sendString(configTopic, buffer, 2, true);
    }

    void callback(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
    {
#ifdef USE_DEBUG
        Serial.println(topic);
#endif
        char* buffer = new char[len + 1]();
        memcpy(buffer, payload, len);
        String message = buffer;

        mySettings->processCommandMqtt(message);
    }

    void onMqttConnect(bool sessionPresent)
    {
#ifdef USE_DEBUG
        Serial.println(F("Connected to MQTT."));
        Serial.print(F("Session present: "));
        Serial.println(sessionPresent);
#endif

        sendDiscovery();
        sendState();
        sendAvailability();
        subscribe();
    }

    void connectToMqtt() {
        if (!WiFi.isConnected()) {
            return;
        }

        if (!client) {
            return;
        }

#ifdef USE_DEBUG
        Serial.println(F("Connecting to MQTT..."));
#endif
        client->connect();
    }

    void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
    {
#ifdef USE_DEBUG
        Serial.print(F("MQTT disconnect reason: "));
        switch (reason) {
        case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
            Serial.println(F("TCP_DISCONNECTED"));
            break;
        case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
            Serial.println(F("MQTT_SERVER_UNAVAILABLE"));
            break;
        case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
            Serial.println(F("MQTT_UNACCEPTABLE_PROTOCOL_VERSION"));
            break;
        case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT:
            Serial.println(F("TLS_BAD_FINGERPRINT"));
            break;
        case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
            Serial.println(F("MQTT_IDENTIFIER_REJECTED"));
            break;
        case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
            Serial.println(F("MQTT_MALFORMED_CREDENTIALS"));
            break;
        case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
            Serial.println(F("MQTT_NOT_AUTHORIZED"));
            break;
        case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE:
            Serial.println(F("ESP8266_NOT_ENOUGH_SPACE"));
            break;
        default:
            Serial.printf_P(PSTR("unknown %d\n"), reason);
        }
#endif
        mqttReconnectTimer.once(10, connectToMqtt);
    }

#if defined(ESP8266)
    WiFiEventHandler wifiConnectHandler;
    WiFiEventHandler wifiDisconnectHandler;

    void onWifiConnect(const WiFiEventStationModeGotIP& event) {
#ifdef USE_DEBUG
        Serial.println("Connected to Wi-Fi.");
#endif
        mqttReconnectTimer.once(2, connectToMqtt);
    }

    void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
#ifdef USE_DEBUG
        Serial.println("Disconnected from Wi-Fi.");
#endif
        mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
    }

#else
    void WiFiEvent(WiFiEvent_t event) {
        switch (event) {
        case SYSTEM_EVENT_STA_GOT_IP:
#ifdef USE_DEBUG
            Serial.println("Connected to Wi-Fi.");
#endif
            mqttReconnectTimer.once(2, connectToMqtt);
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
#ifdef USE_DEBUG
            Serial.println("Disconnected from Wi-Fi.");
#endif
            mqttReconnectTimer.detach(); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
            break;
        default:
            break;
        }
    }
#endif

}

MqttClient* MqttClient::instance()
{
    return object;
}

void MqttClient::Initialize()
{
    if (object) {
        return;
    }

#ifdef USE_DEBUG
    Serial.println(F("Initializing MqttClient"));
#endif
    object = new MqttClient();
}

void MqttClient::update()
{
    if (!client) {
        return;
    }

    updateTimer.once(1, staticUpdate);
}

MqttClient::MqttClient()
{
    if (mySettings->mqttSettings.host.isEmpty()) {
#ifdef USE_DEBUG
        Serial.println(F("Empty host for MqttClient"));
#endif
        return;
    }

#ifdef ARDUINO_ARCH_ESP8266
    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
#else
    WiFi.onEvent(WiFiEvent);
#endif

    commonTopic = String(F("homeassistant/light/")) + mySettings->mqttSettings.uniqueId;
    setTopic = commonTopic + String(F("/set"));
    stateTopic = commonTopic + String(F("/state"));
    configTopic = commonTopic + String(F("/config"));
    availabilityTopic = commonTopic + String(F("/available"));
    clientId = String(F("FireLampClient-")) + mySettings->mqttSettings.name;

    client = new AsyncMqttClient;
    client->onConnect(onMqttConnect);
    client->onDisconnect(onMqttDisconnect);
    client->onMessage(callback);
    client->setClientId(clientId.c_str());
    client->setWill(availabilityTopic.c_str(),
        2,
        true,
        "false");
    client->setCredentials(mySettings->mqttSettings.username.c_str(),
        mySettings->mqttSettings.password.c_str());
    client->setServer(mySettings->mqttSettings.host.c_str(),
        mySettings->mqttSettings.port);

    mqttReconnectTimer.once(2, connectToMqtt);
}
