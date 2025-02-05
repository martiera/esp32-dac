#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <IRremote.hpp>
#include <MCP41xxx.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <MQTT.h>
#include <esp_task_wdt.h>

// Include Configuration
#include "esp32-config.h"

// Global state structure
struct State {
    byte volume = 20;
    int wifiSignalBars = 0;
    bool screenOn = true;
    unsigned long screenTimeOn = 0;
    uint32_t lastIRCode = 0;
    unsigned long lastIRTime = 0;
    String displayText;
    bool hasDisplayMessage = false;
    volatile bool displayBusy = false;
    bool displayUpdatePending = false;
    
    enum DisplayMode {
        VOLUME,
        MINIMAL,
        TEXT
    } currentMode = VOLUME;
    
    void resetScreen() {
        screenTimeOn = millis();
        screenOn = true;
    }
};

// Global objects
MCP41xxx dacVolume(Config::DAC_SS_PIN);
U8G2_SSD1322_NHD_256X64_F_4W_SW_SPI u8g2(U8G2_R2, 14, 12, 27, 13, 35);
WiFiClient net;
MQTTClient mqttClient;
State state;

// Function prototypes
void updateVolume();
void requestDisplayUpdate();
void handleDisplayUpdate();
void sendVolumeMQTT();

class WiFiManager {
public:
    static bool connect() {
        WiFi.mode(WIFI_STA);
        WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
        
        int attempts = 0;
        while (WiFi.waitForConnectResult() != WL_CONNECTED && attempts < 3) {
            Serial.println("Connection Failed! Retrying...");
            delay(5000);
            attempts++;
            WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
        }
        
        return WiFi.status() == WL_CONNECTED;
    }
    
    static int getBarsSignal(long rssi) {
        if (rssi > -55) return 5;
        if (rssi > -65) return 4;
        if (rssi > -75) return 3;
        if (rssi > -85) return 2;
        if (rssi > -96) return 1;
        return 0;
    }
};

void setupOTA() {
    ArduinoOTA.setHostname(Config::HOSTNAME);
    ArduinoOTA.setPassword(Config::OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        IrReceiver.stop();
        Serial.println("Starting OTA update");
    });

    ArduinoOTA.onEnd([]() {
        IrReceiver.start();
        Serial.println("\nOTA update completed");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        IrReceiver.start();
        Serial.printf("Error[%u]: ", error);
        const char* errorMsg = "Unknown Error";
        switch (error) {
            case OTA_AUTH_ERROR: errorMsg = "Auth Failed"; break;
            case OTA_BEGIN_ERROR: errorMsg = "Begin Failed"; break;
            case OTA_CONNECT_ERROR: errorMsg = "Connect Failed"; break;
            case OTA_RECEIVE_ERROR: errorMsg = "Receive Failed"; break;
            case OTA_END_ERROR: errorMsg = "End Failed"; break;
        }
        Serial.println(errorMsg);
    });

    ArduinoOTA.begin();
}

void messageReceived(String &topic, String &payload) {
    if (topic == Config::TOPIC_VOLUME_SET) {
        int tempVolume = payload.toInt();
        if (tempVolume >= 0 && tempVolume <= 255) {
            state.volume = static_cast<byte>(tempVolume);
            state.resetScreen();
            updateVolume();
            requestDisplayUpdate();
            sendVolumeMQTT();
        }
    }
    else if (topic == Config::TOPIC_DISPLAY_SET) {
        state.displayText = payload;
        state.hasDisplayMessage = !payload.isEmpty();
        if (state.hasDisplayMessage) {
            state.currentMode = State::DisplayMode::TEXT;
            requestDisplayUpdate();
        } else {
            state.currentMode = State::DisplayMode::MINIMAL;
            requestDisplayUpdate();
        }
    }
}

void setupMQTT() {
    mqttClient.begin(Config::MQTT_SERVER, net);
    mqttClient.onMessage(messageReceived);

    while (!mqttClient.connect("ESP-DAC", Config::MQTT_USER, Config::MQTT_PASSWORD)) {
        Serial.print(".");
        delay(1000);
    }
    
    mqttClient.subscribe(Config::TOPIC_DISPLAY_SET);
    mqttClient.subscribe(Config::TOPIC_VOLUME_SET);
}

void handleMQTT() {
    mqttClient.loop();
    delay(10);  // Stability delay
    
    if (!mqttClient.connected()) {
        setupMQTT();
    }
}

void sendVolumeMQTT() {
    if (!mqttClient.connected()) {
        setupMQTT();
    }
    mqttClient.publish(Config::TOPIC_VOLUME_STATE, String(state.volume));
}

void updateVolume() {
    int dacValue = min(255, static_cast<int>(Config::TWICE_LOUD_STEPS / Config::TWICE_LOUD_DB * state.volume + 0.5f));
    dacVolume.analogWrite(0, dacValue);
    Serial.printf("Volume: %d, DAC Value: %d\n", state.volume, dacValue);
    state.currentMode = State::DisplayMode::VOLUME;
    requestDisplayUpdate();
}

void handleIRCommand() {
    if (!IrReceiver.decode()) return;
    
    uint32_t receivedCode = IrReceiver.decodedIRData.decodedRawData;
    Serial.printf("Received IR Code: 0x%08X\n", receivedCode);
    
    if (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) {
        receivedCode = state.lastIRCode;
    } else {
        state.lastIRCode = receivedCode;
    }
    
    bool volumeChanged = false;
    
    switch (static_cast<IRCommand>(receivedCode)) {
        case IRCommand::UP_SONY:
        case IRCommand::UP_APPLE:
            if (state.volume < 255) {
                state.volume++;
                volumeChanged = true;
            }
            break;
            
        case IRCommand::DOWN_SONY:
        case IRCommand::DOWN_APPLE:
            if (state.volume > 0) {
                state.volume--;
                volumeChanged = true;
            }
            break;
    }

    if (volumeChanged) {
        state.resetScreen();
        updateVolume();
        requestDisplayUpdate();
        sendVolumeMQTT();
    }

    IrReceiver.resume();
}

void requestDisplayUpdate() {
    state.displayUpdatePending = true;
}

void handleDisplayUpdate() {
    static uint8_t errorCount = 0;
    if (state.displayBusy) {
        errorCount++;
        if (errorCount > 10) {  // If display stays busy for too long
            state.displayBusy = false;
            errorCount = 0;
        }
        return;
    }
    
    if (!state.displayUpdatePending) {
        return;
    }
    
    errorCount = 0;
    state.displayBusy = true;
    
    u8g2.clearBuffer();
    
    static char buffer[12];
    switch (state.currentMode) {
        case State::DisplayMode::VOLUME:
            u8g2.setFont(u8g2_font_logisoso18_tf);
            u8g2.drawStr(0, 18, "VOL");
            
            u8g2.setFont(u8g2_font_logisoso58_tf);
            sprintf(buffer, "-%2d", state.volume);
            u8g2.drawStr(0, 61, buffer);
            u8g2.drawStr(130, 61, "dB");
            break;
            
        case State::DisplayMode::MINIMAL:
            u8g2.setFont(u8g2_font_logisoso18_tf);
            sprintf(buffer, "VOL -%2d dB", state.volume);
            u8g2.drawStr(0, 18, buffer);
            break;
            
        case State::DisplayMode::TEXT:
            u8g2.setFont(u8g2_font_logisoso18_tf);
            u8g2.drawStr(0, 32, state.displayText.c_str());
            break;
    }
    
    // Always draw WiFi bars
    for (int b = 0; b <= state.wifiSignalBars; b++) {
        u8g2.drawBox(228 + (b*5), 64 - (b*5), 3, b*5);
    }
    
    u8g2.sendBuffer();
    state.displayBusy = false;
    state.displayUpdatePending = false;
    state.screenTimeOn = millis();
}

void setup() {
    Serial.begin(115200);
    Serial.println("Booting ESP32 DAC Controller");

    // Initialize watchdog
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 8000,              // 8 seconds
        .idle_core_mask = (1 << 0),      // Watch core 0
        .trigger_panic = true            // Trigger panic on timeout
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);

    if (!WiFiManager::connect()) {
        ESP.restart();
    }
    
    setupOTA();
    dacVolume.begin();
    u8g2.begin();
    updateVolume();
    
    IrReceiver.begin(Config::IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
    
    // Startup animation
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso32_tf);
    u8g2.drawStr(0, 48, "ESP32 SABRE");
    u8g2.sendBuffer();
    delay(1000);

    u8g2.clearBuffer();
    u8g2.drawStr(10, 48, "ES9038 DAC");
    u8g2.sendBuffer();
    delay(1000);

    state.wifiSignalBars = WiFiManager::getBarsSignal(WiFi.RSSI());
    requestDisplayUpdate();
    setupMQTT();
    sendVolumeMQTT();
}

void loop() {
    esp_task_wdt_reset();

    if ((millis() - state.screenTimeOn > Config::SCREEN_TIMEOUT) && state.screenOn) {
        state.screenOn = false;
        state.currentMode = state.hasDisplayMessage ? State::DisplayMode::TEXT : State::DisplayMode::MINIMAL;
        requestDisplayUpdate();
    }

    handleDisplayUpdate();
    handleIRCommand();
    handleMQTT();
    ArduinoOTA.handle();
    
    // Update WiFi signal strength periodically
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 60000) {  // Check every 60 seconds
        state.wifiSignalBars = WiFiManager::getBarsSignal(WiFi.RSSI());
        requestDisplayUpdate();
        lastWifiCheck = millis();
    }
}