#include "Settings.h"
#include "EffectsManager.h"
#include "MyMatrix.h"
#include "LocalDNS.h"
#include "MqttClient.h"
#include "LampWebServer.h"

#include <ESPAsyncWebServer.h>

#if defined(ESP32)
#include <SPIFFS.h>
#define FLASHFS SPIFFS
#else
#include <LittleFS.h>
#define FLASHFS LittleFS
#endif
#include "effects/Effect.h"

namespace {

    const size_t serializeEffectsSize = 512 * 22;
    const size_t serializeSettingsSize = 512 * 2;

    Settings* object = nullptr;

    bool settingsChanged = false;
    uint32_t settingsSaveTimer = 0;
    uint32_t settingsSaveInterval = 5000;

    const char* settingsFileName PROGMEM = "/settings.json";
    const char* effectsFileName PROGMEM = "/effects.json";

    const char* settingsFileNameSave PROGMEM = "/settings.json.save";
    const char* effectsFileNameSave PROGMEM = "/effects.json.save";

    std::vector<String> pendingConfig;
    std::vector<String> pendingCommand;

    String GetUniqueID()
    {
#if defined(ESP32)
        return String((uint32_t)ESP.getEfuseMac(), HEX);
#else
        return String((uint32_t)ESP.getChipId(), HEX);
#endif
    }

    bool copyFile(String fileFrom, String fileTo)
    {
        mySettings->busy = true;
#ifdef USE_DEBUG
        Serial.printf_P(PSTR("Copying file from %s to %s\n"), fileFrom.c_str(), fileTo.c_str());
#endif

        File source = FLASHFS.open(fileFrom, "r");
        if (!source) {
#ifdef USE_DEBUG
            Serial.print(F("FLASHFS Error reading file: "));
            Serial.println(fileFrom);
#endif
            mySettings->busy = false;
            return false;
        }

        if (FLASHFS.exists(fileTo)) {
            FLASHFS.remove(fileTo);
        }

        File dest = FLASHFS.open(fileTo, "w");
        if (!dest) {
#ifdef USE_DEBUG
            Serial.print(F("FLASHFS Error opening file: "));
            Serial.println(fileTo);
#endif
            source.close();
            mySettings->busy = false;
            return false;
        }

        size_t blockSize = 64;
        uint8_t buf[blockSize];
        while (size_t n = source.read(buf, blockSize)) {
            if (dest.write(buf, n) == 0) {
#ifdef USE_DEBUG
                Serial.print(F("FLASHFS Error writing to file: "));
                Serial.println(fileTo);
#endif
                source.close();
                dest.close();
                FLASHFS.remove(fileTo);
                mySettings->busy = false;
                return false;
            }
        }

        dest.close();
        source.close();

        mySettings->busy = false;
        return true;
    }

    void restoreSettingsAndReboot()
    {
#ifdef USE_DEBUG
        Serial.println(F("Restoring settings json save and rebooting"));
#endif
        copyFile(settingsFileName, settingsFileNameSave);
        ESP.restart();
        delay(5000);
    }

    void restoreEffectsAndReboot()
    {
#ifdef USE_DEBUG
        Serial.println(F("Restoring effects json save and rebooting"));
#endif
        copyFile(effectsFileName, effectsFileNameSave);
        ESP.restart();
        delay(5000);
    }

} // namespace

Settings* Settings::instance()
{
    return object;
}

void Settings::Initialize(uint32_t saveInterval)
{
    if (object) {
        return;
    }

#ifdef USE_DEBUG
    Serial.println(F("Initializing Settings"));
#endif
    object = new Settings(saveInterval);
}

size_t Settings::jsonSerializeSize()
{
    return serializeEffectsSize;
}

void Settings::loop()
{
    if (settingsChanged && settingsSaveTimer > 0 && (millis() - settingsSaveTimer) > settingsSaveInterval) {
        settingsChanged = false;
        settingsSaveTimer = millis();
        saveSettings();
        saveEffects();

        if (pendingConfig.size()) {
            for (const String& config : pendingConfig) {
                processConfig(config);
            }
            pendingConfig.clear();
        }

        if (pendingCommand.size()) {
            for (const String& command : pendingCommand) {
                processCommandMqtt(command);
            }
            pendingCommand.clear();
        }
    }
}

void Settings::saveLater()
{
    settingsChanged = true;
    settingsSaveTimer = millis();
}

void Settings::saveSettings()
{
    busy = true;

#ifdef USE_DEBUG
    Serial.print(F("Saving settings... "));
#endif

    File file = FLASHFS.open(settingsFileNameSave, "w");
    if (!file) {
#ifdef USE_DEBUG
        Serial.println(F("Error opening settings file from FLASHFS!"));
#endif
        return;
    }

    DynamicJsonDocument json(serializeSettingsSize);
    JsonObject root = json.to<JsonObject>();
    buildSettingsJson(root);

    if (serializeJson(json, file) == 0) {
#ifdef USE_DEBUG
        Serial.println(F("Failed to serialize settings"));
#endif
        saveLater();
    }

    if (file) {
        file.close();
    }
#ifdef USE_DEBUG
    Serial.println(F("Done!"));
#endif

    busy = false;
}

void Settings::saveEffects()
{
    busy = true;

#ifdef USE_DEBUG
    Serial.print(F("Saving effects... "));
#endif

    File file = FLASHFS.open(effectsFileNameSave, "w");
    if (!file) {
#ifdef USE_DEBUG
        Serial.println(F("Error opening effects file from FLASHFS!"));
#endif
        return;
    }

    DynamicJsonDocument json(serializeEffectsSize);
    JsonArray root = json.to<JsonArray>();
    buildEffectsJson(root);

    if (serializeJson(json, file) == 0) {
#ifdef USE_DEBUG
        Serial.println(F("Failed to serialize effects"));
#endif
        saveLater();
    }
    if (file) {
        file.close();
    }
#ifdef USE_DEBUG
    Serial.println(F("Done!"));
#endif

    busy = false;
}

void Settings::writeEffectsMqtt(JsonArray& array)
{
    for (Effect* effect : effectsManager->effects) {
        array.add(effect->settings.name);
    }
}

void Settings::processConfig(const String& message)
{
    if (busy) {
#ifdef USE_DEBUG
        Serial.println(F("\nSaving in progress! Delaying operation.\n"));
#endif
        pendingConfig.push_back(message);
        lampWebServer->update();
        return;
    }

#ifdef USE_DEBUG
    Serial.print(F("<< "));
    Serial.println(message);
#endif

    {
        DynamicJsonDocument doc(512);
        if (DeserializationError err = deserializeJson(doc, message)) {
#ifdef USE_DEBUG
            Serial.print(F("[processConfig] Error parsing json: "));
            Serial.println(err.c_str());
#endif
            return;
        }

        const String event = doc[F("event")];
        if (event == F("WORKING")) {
            const bool working = doc[F("data")];
#ifdef USE_DEBUG
            Serial.printf_P(PSTR("working: %s\n"), working ? PSTR("true") : PSTR("false"));
#endif
            mySettings->generalSettings.working = working;
            saveLater();
        }
        else if (event == F("ACTIVE_EFFECT")) {
            //            const int index = doc[F("data")];
            //            effectsManager->activateEffect(static_cast<uint8_t>(index));
            return;
        }
        else if (event == F("EFFECTS_CHANGED")) {
            const JsonObject effect = doc[F("data")];
            const String id = effect[F("i")];
            if (id == effectsManager->activeEffect()->settings.id) {
                effectsManager->updateCurrentSettings(effect);
            }
            else {
                effectsManager->updateSettingsById(id, effect);
            }
            saveLater();
        }
        else if (event == F("ALARMS_CHANGED")) {

        }
    }

    if (mqtt) {
        mqtt->update();
    }
    lampWebServer->update();
}

void Settings::processCommandMqtt(const String& message)
{
    if (busy) {
#ifdef USE_DEBUG
        Serial.println(F("\nSaving in progress! Delaying operation.\n"));
#endif
        pendingCommand.push_back(message);
        return;
    }

#ifdef USE_DEBUG
    Serial.println(message);
    Serial.println();
#endif

    {
        DynamicJsonDocument doc(1024);
        if (DeserializationError err = deserializeJson(doc, message)) {
#ifdef USE_DEBUG
            Serial.print(F("[processCommandMqtt] Error parsing json: "));
            Serial.println(err.c_str());
#endif
        }
        JsonObject json = doc.as<JsonObject>();

        if (json.containsKey(F("state"))) {
            const String state = json[F("state")];
            mySettings->generalSettings.working = state == F("ON");

            if (json.containsKey(F("effect"))) {
                const String effect = json[F("effect")];
                effectsManager->changeEffectByName(effect);
            }
            else if (json.containsKey(F("switchTo"))) {
                const String switchTo = json[F("switchTo")];
                if (switchTo.equals(F("prev"))) {
                    effectsManager->previous();
                }
                else if (switchTo.equals(F("next"))) {
                    effectsManager->next();
                }
            }
            if (json.containsKey(F("color"))) {
                effectsManager->changeEffectById(F("Color"));
            }
        }
        effectsManager->updateCurrentSettings(json);
        saveLater();
    }

    if (mqtt) {
        mqtt->update();
    }
    lampWebServer->update();
}

bool Settings::readSettings()
{
    bool settingsExists = FLASHFS.exists(settingsFileName);
#ifdef USE_DEBUG
    Serial.printf_P(PSTR("FLASHFS Settings file exists: %s\n"), settingsExists ? PSTR("true") : PSTR("false"));
#endif
    if (!settingsExists) {
        saveSettings();
        copyFile(settingsFileNameSave, settingsFileName);
        return false;
    }

    bool settingsSaveExists = FLASHFS.exists(settingsFileNameSave);
#ifdef USE_DEBUG
    Serial.printf_P(PSTR("FLASHFS Settings save file exists: %s\n"), settingsSaveExists ? PSTR("true") : PSTR("false"));
#endif
    if (!settingsSaveExists) {
        copyFile(settingsFileName, settingsFileNameSave);
    }

    File settings = FLASHFS.open(settingsFileNameSave, "r");
#ifdef USE_DEBUG
    Serial.printf_P(PSTR("FLASHFS Settings file size: %zu\n"), settings.size());
#endif
    if (!settings) {
#ifdef USE_DEBUG
        Serial.println(F("FLASHFS Error reading settings file"));
#endif
        restoreSettingsAndReboot();
        return false;
    }

#ifdef USE_DEBUG
    Serial.println("reading settings.json.save");
#endif
    while (settings.available()) {
        String buffer = settings.readStringUntil('\n');
#ifdef USE_DEBUG
        Serial.println(buffer);
#endif
    }
    settings.seek(0);

    DynamicJsonDocument json(serializeSettingsSize);
    DeserializationError err = deserializeJson(json, settings);
    settings.close();
    if (err) {
#ifdef USE_DEBUG
        Serial.print(F("FLASHFS Error parsing settings json file: "));
        Serial.println(err.c_str());
#endif

        restoreSettingsAndReboot();
        return false;
    }

    JsonObject root = json.as<JsonObject>();
    if (root.size() == 0) {
        restoreSettingsAndReboot();
        return false;
    }

    if (root.containsKey(F("matrix"))) {
        JsonObject matrixObject = root[F("matrix")];
        if (matrixObject.containsKey(F("pin"))) {
            matrixSettings.pin = matrixObject[F("pin")];
        }
        if (matrixObject.containsKey(F("width"))) {
            matrixSettings.width = matrixObject[F("width")];
        }
        if (matrixObject.containsKey(F("height"))) {
            matrixSettings.height = matrixObject[F("height")];
        }
        if (matrixObject.containsKey(F("segments"))) {
            matrixSettings.segments = matrixObject[F("segments")];
        }
        if (matrixObject.containsKey(F("type"))) {
            matrixSettings.type = matrixObject[F("type")];
        }
        if (matrixObject.containsKey(F("maxBrightness"))) {
            matrixSettings.maxBrightness = matrixObject[F("maxBrightness")];
        }
        if (matrixObject.containsKey(F("currentLimit"))) {
            matrixSettings.currentLimit = matrixObject[F("currentLimit")];
        }
        if (matrixObject.containsKey(F("rotation"))) {
            matrixSettings.rotation = matrixObject[F("rotation")];
        }
        if (matrixObject.containsKey(F("dither"))) {
            matrixSettings.dither = matrixObject[F("dither")];
        }
        if (matrixObject.containsKey(F("order"))) {
            matrixSettings.order = matrixObject[F("order")].as<String>();
        }
    }

    if (root.containsKey(F("connection"))) {
        JsonObject connectionObject = root[F("connection")];
        if (connectionObject.containsKey(F("mdns"))) {
            connectionSettings.mdns = connectionObject[F("mdns")].as<String>();
        }
        if (connectionObject.containsKey(F("apName"))) {
            connectionSettings.apName = connectionObject[F("apName")].as<String>();
        }
        if (connectionObject.containsKey(F("apPassword"))) {
            connectionSettings.apPassword = connectionObject[F("apPassword")].as<String>();
        }
        if (connectionObject.containsKey(F("ntpServer"))) {
            connectionSettings.ntpServer = connectionObject[F("ntpServer")].as<String>();
        }
        if (connectionObject.containsKey(F("ntpOffset"))) {
            connectionSettings.ntpOffset = connectionObject[F("ntpOffset")];
        }
        if (connectionObject.containsKey(F("hostname"))) {
            connectionSettings.hostname = connectionObject[F("hostname")].as<String>();
        }
        if (connectionObject.containsKey(F("ssid"))) {
            connectionSettings.ssid = connectionObject[F("ssid")].as<String>();
        }
        if (connectionObject.containsKey(F("bssid"))) {
            connectionSettings.bssid = connectionObject[F("bssid")].as<String>();
        }
        if (connectionObject.containsKey(F("password"))) {
            connectionSettings.password = connectionObject[F("password")].as<String>();
        }
        if (connectionObject.containsKey(F("login"))) {
            connectionSettings.login = connectionObject[F("login")].as<String>();
        }
    }

    if (root.containsKey(F("mqtt"))) {
        JsonObject mqttObject = root[F("mqtt")];
        if (mqttObject.containsKey(F("host"))) {
            mqttSettings.host = mqttObject[F("host")].as<String>();
        }
        if (mqttObject.containsKey(F("port"))) {
            mqttSettings.port = mqttObject[F("port")];
        }
        if (mqttObject.containsKey(F("username"))) {
            mqttSettings.username = mqttObject[F("username")].as<String>();
        }
        if (mqttObject.containsKey(F("password"))) {
            mqttSettings.password = mqttObject[F("password")].as<String>();
        }
        if (mqttObject.containsKey(F("uniqueId"))) {
            mqttSettings.uniqueId = mqttObject[F("uniqueId")].as<String>();
        }
        if (mqttObject.containsKey(F("name"))) {
            mqttSettings.name = mqttObject[F("name")].as<String>();
        }
        if (mqttObject.containsKey(F("model"))) {
            mqttSettings.model = mqttObject[F("model")].as<String>();
        }
    }

    if (root.containsKey(F("spectrometer"))) {
        JsonObject spectrometerObject = root[F("spectrometer")];
        if (spectrometerObject.containsKey(F("active"))) {
            generalSettings.soundControl = spectrometerObject[F("active")];
        }
    }

    if (root.containsKey(F("button"))) {
        JsonObject buttonObject = root[F("button")];
        if (buttonObject.containsKey(F("pin"))) {
            uint8_t btnPin = buttonObject[F("pin")];
            buttonSettings.pin = btnPin;
        }
        if (buttonObject.containsKey(F("type"))) {
            buttonSettings.type = buttonObject[F("type")];
        }
        if (buttonObject.containsKey(F("state"))) {
            buttonSettings.state = buttonObject[F("state")];
        }
    }

    if (root.containsKey(F("logInterval"))) {
        generalSettings.logInterval = root[F("logInterval")];
    }

    if (root.containsKey(F("activeEffect"))) {
        generalSettings.activeEffect = root[F("activeEffect")];
    }

    if (root.containsKey(F("working"))) {
        generalSettings.working = root[F("working")];
    }

    copyFile(settingsFileNameSave, settingsFileName);

    return true;
}

bool Settings::readEffects()
{
    bool effectsExists = FLASHFS.exists(effectsFileName);
#ifdef USE_DEBUG
    Serial.printf_P(PSTR("FLASHFS Effects file exists: %s\n"), effectsExists ? PSTR("true") : PSTR("false"));
#endif
    if (!effectsExists) {
        effectsManager->processAllEffects();
        saveEffects();
        copyFile(effectsFileNameSave, effectsFileName);
        return false;
    }
    bool effectsSaveExists = FLASHFS.exists(effectsFileNameSave);
#ifdef USE_DEBUG
    Serial.printf_P(PSTR("FLASHFS Effects save file exists: %s\n"), effectsSaveExists ? PSTR("true") : PSTR("false"));
#endif
    if (!effectsSaveExists) {
        copyFile(effectsFileName, effectsFileNameSave);
    }

    File effects = FLASHFS.open(effectsFileNameSave, "r");
#ifdef USE_DEBUG
    Serial.printf_P(PSTR("FLASHFS Effects file size: %zu\n"), effects.size());
#endif
    if (!effects) {
#ifdef USE_DEBUG
        Serial.println(F("FLASHFS Error reading effects file"));
#endif
        restoreEffectsAndReboot();
        return false;
    }

#ifdef USE_DEBUG
    Serial.println("reading effects.json");
#endif
    while (effects.available()) {
        String buffer = effects.readStringUntil('\n');
#ifdef USE_DEBUG
        Serial.println(buffer);
#endif
    }
    effects.seek(0);

    DynamicJsonDocument json(serializeEffectsSize);
    DeserializationError err = deserializeJson(json, effects);
    effects.close();
    if (err) {
#ifdef USE_DEBUG
        Serial.print(F("FLASHFS Error parsing effects json file: "));
        Serial.println(err.c_str());
#endif
        restoreEffectsAndReboot();
        return false;
    }

    JsonArray root = json.as<JsonArray>();
    if (root.size() == 0) {
        restoreEffectsAndReboot();
        return false;
    }
    else {
#ifdef USE_DEBUG
        Serial.printf_P(PSTR("Effects count: %zu\n"), root.size());
#endif
    }

    for (JsonObject effect : root) {
        effectsManager->processEffectSettings(effect);
    }

    copyFile(effectsFileNameSave, effectsFileName);

    return true;
}

void Settings::buildSettingsJson(JsonObject& root)
{
    root[F("activeEffect")] = effectsManager->activeEffectIndex();
    root[F("logInterval")] = generalSettings.logInterval;
    root[F("working")] = generalSettings.working;

    JsonObject matrixObject = root.createNestedObject(F("matrix"));
    matrixObject[F("pin")] = matrixSettings.pin;
    matrixObject[F("width")] = matrixSettings.width;
    matrixObject[F("height")] = matrixSettings.height;
    matrixObject[F("segments")] = matrixSettings.segments;
    matrixObject[F("type")] = matrixSettings.type;
    matrixObject[F("maxBrightness")] = matrixSettings.maxBrightness;
    matrixObject[F("currentLimit")] = matrixSettings.currentLimit;
    matrixObject[F("rotation")] = matrixSettings.rotation;
    matrixObject[F("dither")] = matrixSettings.dither;
    matrixObject[F("order")] = matrixSettings.order;

    JsonObject connectionObject = root.createNestedObject(F("connection"));
    connectionObject[F("mdns")] = connectionSettings.mdns;
    connectionObject[F("apName")] = connectionSettings.apName;
    connectionObject[F("apPassword")] = connectionSettings.apPassword;
    connectionObject[F("ntpServer")] = connectionSettings.ntpServer;
    connectionObject[F("ntpOffset")] = connectionSettings.ntpOffset;
    connectionObject[F("hostname")] = connectionSettings.hostname;
    connectionObject[F("ssid")] = connectionSettings.ssid;
    connectionObject[F("bssid")] = connectionSettings.bssid;
    connectionObject[F("password")] = connectionSettings.password;
    connectionObject[F("login")] = connectionSettings.login;

    JsonObject mqttObject = root.createNestedObject(F("mqtt"));
    mqttObject[F("host")] = mqttSettings.host;
    mqttObject[F("port")] = mqttSettings.port;
    mqttObject[F("username")] = mqttSettings.username;
    mqttObject[F("password")] = mqttSettings.password;
    mqttObject[F("uniqueId")] = mqttSettings.uniqueId;
    mqttObject[F("model")] = mqttSettings.model;
    mqttObject[F("name")] = mqttSettings.name;

    JsonObject buttonObject = root.createNestedObject(F("button"));
    buttonObject[F("pin")] = buttonSettings.pin;
    buttonObject[F("type")] = buttonSettings.type;
    buttonObject[F("state")] = buttonSettings.state;

    JsonObject spectrometerObject = root.createNestedObject(F("spectrometer"));
    spectrometerObject[F("active")] = generalSettings.soundControl;
}

void Settings::buildEffectsJson(JsonArray& effects)
{
    for (Effect* effect : effectsManager->effects) {
        JsonObject effectObject = effects.createNestedObject();
        effectObject[F("i")] = effect->settings.id;
        effectObject[F("n")] = effect->settings.name;
        effectObject[F("s")] = effect->settings.speed;
        effectObject[F("l")] = effect->settings.scale;
        effectObject[F("b")] = effect->settings.brightness;
        effect->writeSettings(effectObject);
    }
}

void Settings::buildJsonMqtt(JsonObject& root)
{
    root[F("state")] = generalSettings.working ? F("ON") : F("OFF");
    root[F("brightness")] = effectsManager->activeEffect()->settings.brightness;
    root[F("speed")] = effectsManager->activeEffect()->settings.speed;
    root[F("scale")] = effectsManager->activeEffect()->settings.scale;
    root[F("effect")] = effectsManager->activeEffect()->settings.name;
    root[F("localIp")] = WiFi.localIP().toString();
    root[F("webui")] = String(F("http://")) + WiFi.localIP().toString();
#if defined(ESP8266)
    root[F("device")] = F("esp8266");
#else
    root[F("device")] = F("esp32");
#endif
    effectsManager->activeEffect()->writeSettings(root);
}

Settings::Settings(uint32_t saveInterval)
{
    settingsSaveInterval = saveInterval;

    matrixSettings.order = F("grb");

    connectionSettings.mdns = F("firelamp");
    connectionSettings.apName = F("Fire Lamp");
    connectionSettings.ntpServer = F("europe.pool.ntp.org");
    connectionSettings.hostname = F("firelamp");

    mqttSettings.uniqueId = GetUniqueID();
    mqttSettings.manufacturer = F("coderus");
}
