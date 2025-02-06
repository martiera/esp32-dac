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
    volatile bool displayBusy = false;
    bool displayUpdatePending = false;
    String moodeSource;
    String moodeDetails;
    
    bool hasMoodeInfo() const {
        return !moodeSource.isEmpty();
    }

    bool hasDisplayMessage() const {
        return !displayText.isEmpty();
    }

    enum DisplayMode {
        VOLUME,
        MINIMAL,
        TEXT,
        MOODE
    } currentMode = VOLUME;
    
    enum InputSource {
        OPTICAL,
        COAX,
        I2S
    } currentSource = I2S;

    void resetScreen() {
        screenTimeOn = millis();
        screenOn = true;
    }

    String inputSourceToString() {
        switch(currentSource) {
            case OPTICAL:   return Config::DISPLAY_OPTICAL_TEXT;
            case COAX:      return Config::DISPLAY_COAX_TEXT;
            case I2S:       return Config::DISPLAY_I2S_TEXT;
            default:        return "";
        }
    }

    InputSource stringToInputSource(const String &s) {
        if (s.equalsIgnoreCase(Config::DISPLAY_OPTICAL_TEXT)) {
            return InputSource::OPTICAL;
        } else if (s.equalsIgnoreCase(Config::DISPLAY_COAX_TEXT)) {
            return InputSource::COAX;
        } else if (s.equalsIgnoreCase(Config::DISPLAY_I2S_TEXT)) {
            return InputSource::I2S;
        } else {
            // A default value if no match is found.
            return InputSource::I2S;
        }
}

};

// Global objects
MCP41xxx dacVolume(Config::DAC_SS_PIN);
U8G2_SSD1322_NHD_256X64_F_4W_SW_SPI u8g2(U8G2_R2, 14, 12, 27, 13, 35);
WiFiClient net;
MQTTClient mqttClient;
State state;

// Function prototypes
void handleVolumeChange();
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
        if (rssi > -55) return 4;
        if (rssi > -65) return 3;
        if (rssi > -75) return 2;
        if (rssi > -85) return 1;
        return 0;
    }
};

void setupOTA() {
    ArduinoOTA.setHostname(Config::HOSTNAME);
    ArduinoOTA.setPassword(Config::OTA_PASSWORD);
    ArduinoOTA.setTimeout(30000); // 30 seconds

    ArduinoOTA.onStart([]() {
        IrReceiver.stop();
        esp_task_wdt_delete(NULL);  // Unsubscribe current task from watchdog
        Serial.println("Starting OTA update");
    });

    ArduinoOTA.onEnd([]() {
        IrReceiver.start();
        esp_task_wdt_add(NULL);  // Resubscribe the task to the watchdog
        Serial.println("\nOTA update completed");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        IrReceiver.start();
        esp_task_wdt_add(NULL);  // Resubscribe the task to the watchdog
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
            handleVolumeChange();
        }
    }
    else if (topic == Config::TOPIC_MOODE_SOURCE) {
        state.moodeSource = payload;
        requestDisplayUpdate();
    }
    else if (topic == Config::TOPIC_MOODE_DETAILS) {
        state.moodeDetails = payload;
        requestDisplayUpdate();
    }
    else if (topic == Config::TOPIC_DISPLAY_SET) {
        state.displayText = payload;
        requestDisplayUpdate();
    }
    else if (topic == Config::TOPIC_SOURCE_SET) {
        state.currentSource = state.stringToInputSource(payload);
        requestDisplayUpdate();
    }
}

void handleVolumeChange() {
    updateVolume();
    state.resetScreen();
    state.currentMode = State::DisplayMode::VOLUME;
    requestDisplayUpdate();
    sendVolumeMQTT();
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
    mqttClient.subscribe(Config::TOPIC_MOODE_SOURCE);
    mqttClient.subscribe(Config::TOPIC_MOODE_DETAILS);
    mqttClient.subscribe(Config::TOPIC_SOURCE_SET);
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

uint8_t convertVolumeToDac(uint8_t volume) {
    // Invert the volume percentage
    uint8_t invertedVolume = Config::VOLUME_STEPS - volume;

    float db = Config::MIN_DB + (static_cast<float>(invertedVolume) / Config::VOLUME_STEPS) * 
               (Config::MAX_DB - Config::MIN_DB);
    float linear = pow(Config::TWICE_LOUD_DB, db / Config::TWICE_LOUD_STEPS);
    return static_cast<uint8_t>(linear * 255.0f + 0.5f);
}

void updateVolume() {
    uint8_t dacValue = convertVolumeToDac(state.volume);
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
        handleVolumeChange();
    }

    IrReceiver.resume();
}

void requestDisplayUpdate() {
    state.displayUpdatePending = true;
}

// Helper function for text centering and truncation
void drawCenteredText(const String& text, uint8_t y, const uint8_t* font) {
    u8g2.setFont(font);
    String displayText = text;
    // Remove whitespaces
    displayText.trim();
    if (displayText.length() > Config::MAX_LINE_LENGTH) {
        displayText = displayText.substring(0, Config::MAX_LINE_LENGTH);
    }
    uint8_t width = u8g2.getStrWidth(displayText.c_str());
    uint8_t x = max((u8g2.getDisplayWidth() - width) / 2, 0);
    u8g2.drawStr(x, y, displayText.c_str());
}

void drawMinimalDisplay() {
    drawCenteredText(state.inputSourceToString(), 42, u8g2_font_logisoso42_tf);
}

void handleDisplayUpdate() {
    if (!state.displayUpdatePending || state.displayBusy) return;

    state.displayBusy = true;
    
    u8g2.clearBuffer();
    
    static char buffer[12];
    switch (state.currentMode) {
        case State::DisplayMode::VOLUME:
            u8g2.setFont(u8g2_font_logisoso18_tf);
            u8g2.drawStr(0, 18, "VOLUME:");
            
            if (state.volume < 100) {
                sprintf(buffer, "%2d", state.volume);
            } else {
                sprintf(buffer, "%3d", state.volume);
            }
            drawCenteredText(buffer, 42, u8g2_font_logisoso42_tf);
            break;

        case State::DisplayMode::MINIMAL:
            drawMinimalDisplay();
            break;
            
        case State::DisplayMode::TEXT:
            u8g2.setFont(u8g2_font_logisoso18_tf);
            u8g2.drawStr(0, 32, state.displayText.c_str());
            break;

        case State::DisplayMode::MOODE:
            if (state.currentSource == State::InputSource::I2S) {
                const uint8_t* font = u8g2_font_logisoso16_tf;
                if (!state.moodeDetails.isEmpty() && !state.moodeSource.isEmpty()) {
                    // Two lines of Moode info
                    drawCenteredText(state.moodeDetails, 18, font);
                    drawCenteredText(state.moodeSource, 44, font);
                } else if (!state.moodeSource.isEmpty()) {
                    // Single line in middle
                    drawCenteredText(state.moodeSource, 44, font);  // 32
                } else {
                    drawMinimalDisplay();
                }
            } else {
                drawMinimalDisplay();
            }
            break;
    }

    // Draw Volume bar when no VOLUME mode
    if (state.currentMode != State::DisplayMode::VOLUME) {
        u8g2.setFont(u8g2_font_t0_12_tf);
        u8g2.drawStr(48, 63, "VOLUME:"); // 32
        u8g2.drawFrame(93, 55, 100, 8);  // 77
        u8g2.drawBox(93, 56, (state.volume * 100) / 100, 6);
    }

    // Always draw WiFi bars
    for (int b = 0; b <= state.wifiSignalBars; b++) {
        u8g2.drawBox(242 + (b*3), 64 - (b*3), 2, b*3);
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
        state.currentMode = state.hasMoodeInfo() 
            ? State::DisplayMode::MOODE 
            : state.hasDisplayMessage() 
                ? State::DisplayMode::TEXT 
                : State::DisplayMode::MINIMAL;
        requestDisplayUpdate();
    } else if (!state.screenOn) {
        state.currentMode = state.hasMoodeInfo() 
            ? State::DisplayMode::MOODE 
            : state.hasDisplayMessage() 
                ? State::DisplayMode::TEXT 
                : State::DisplayMode::MINIMAL;
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