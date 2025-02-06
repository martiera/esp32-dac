// esp32-config.h
#ifndef ESP32_CONFIG_H
#define ESP32_CONFIG_H

#include <Arduino.h>  // Needed for uint8_t on Arduino

// Configuration structure for better organization
struct Config {
    // Network settings
    static constexpr const char* HOSTNAME = "ESP32-DAC";
    static constexpr const char* WIFI_SSID = "ssidname";
    static constexpr const char* WIFI_PASSWORD = "wifipassword";
    static constexpr const char* OTA_PASSWORD = "otapassword";
    static constexpr const char* MQTT_SERVER = "192.168.00.00";
    static constexpr const char* MQTT_USER = "someuser";
    static constexpr const char* MQTT_PASSWORD = "somepassword";
    
   // MQTT topics
    static constexpr const char* TOPIC_VOLUME_SET = "tele/esp-dac/volume/set";
    static constexpr const char* TOPIC_VOLUME_STATE = "tele/esp-dac/volume";
    static constexpr const char* TOPIC_DISPLAY_SET = "tele/esp-dac/display/set";
    static constexpr const char* TOPIC_SOURCE_SET = "tele/esp-dac/source/set";
    static constexpr const char* TOPIC_SOURCE_STATE = "tele/esp-dac/source";

    // MQTT topics for Moode Monitor
    static constexpr const char* TOPIC_MOODE_SOURCE = "moode/audio/source";
    static constexpr const char* TOPIC_MOODE_DETAILS = "moode/audio/details";

    // Hardware pins
    static constexpr uint8_t IR_RECEIVE_PIN = 36;
    static constexpr uint8_t DAC_SS_PIN = 5;
    
    // Display settings
    static constexpr unsigned long SCREEN_TIMEOUT = 3000;
    static constexpr uint8_t MAX_LINE_LENGTH = 26; // Adjust based on font size
    static constexpr const char* DISPLAY_OPTICAL_TEXT = "TV";
    static constexpr const char* DISPLAY_COAX_TEXT = "COAX";
    static constexpr const char* DISPLAY_I2S_TEXT = "MOODE";
    
    // Volume settings
    static constexpr float TWICE_LOUD_STEPS = 20.0f;
    static constexpr float TWICE_LOUD_DB = 10.0f;
    static constexpr float MIN_DB = -60.0f;      // Minimum volume in dB
    static constexpr float MAX_DB = 0.0f;        // Maximum volume in dB
    static constexpr uint8_t VOLUME_STEPS = 100; // Number of steps
};

// IR codes in an enum class for type safety
enum class IRCommand : uint32_t {
    UP_SONY = 0x00004BA5,
    DOWN_SONY = 0x00004BA4,
    UP_APPLE = 0xDCC8CD06,    // rolling code note
    DOWN_APPLE = 0x671A1C02   // rolling code note
};

esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 8000,              // 8 seconds
    .idle_core_mask = (1 << 0),      // Watch core 0
    .trigger_panic = true            // Trigger panic on timeout
};

#endif // ESP32_CONFIG_H
