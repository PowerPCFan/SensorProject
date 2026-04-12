#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const unsigned long reconnectDelay = 30 * 60 * 1000;
unsigned long lastReconnectAttempt = 0;

// Structure to receive data
typedef struct __attribute__((packed)) struct_message {
    float temperature;
    float humidity;
    float pressure;
    float esp32_temperature;
} struct_message;

struct_message myData;
volatile bool hasDataToSend = false;

// Callback function that will be executed when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    if (len == sizeof(myData)) {
        memcpy(&myData, incomingData, sizeof(myData));
        Serial.print("[ESP-NOW] Bytes received: ");
        Serial.println(len);
        hasDataToSend = true;
    } else {
        Serial.println("[ESP-NOW] Warning: Received packet of unexpected size");
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000); 
    Serial.println("[SETUP] Starting ESP-NOW node setup...");
    Serial.println(String("[ESP-IDF] Version: ") + esp_get_idf_version());

    // Set device in AP_STA mode so it can connect to WiFi and receive ESP-NOW
    WiFi.mode(WIFI_AP_STA);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const char* spinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    uint8_t spidx = 0;
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print("\r[SETUP] Connecting to Wi-Fi ");
        Serial.print(spinner[spidx]);
        spidx = (spidx + 1) % 10;
        delay(100);
    }
    Serial.println("\r[SETUP] Wi-Fi connected | Local IP: " + WiFi.localIP().toString());

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Error initializing ESP-NOW");
        return;
    }

    esp_now_register_recv_cb(OnDataRecv);
    Serial.println("[SETUP] Setup complete. Waiting for incoming data...");
}

unsigned long lastPostTime = 0;
const unsigned long msgCooldownMs = 5000; // 5 second cooldown to prevent duplicates

void loop() {
    if (hasDataToSend) {
        hasDataToSend = false;

        // Prevent duplicate DB entries from overlapping radio channels
        if (millis() - lastPostTime < msgCooldownMs) {
            return; 
        }
        lastPostTime = millis();

        if (WiFi.status() == WL_CONNECTED) {
            JsonDocument doc;
            doc["temperature"] = myData.temperature;
            doc["humidity"] = myData.humidity;
            doc["pressure"] = myData.pressure;
            doc["esp32_temperature"] = myData.esp32_temperature;
            doc["wifi_strength"] = WiFi.RSSI();

            String payload;
            serializeJson(doc, payload);

            HTTPClient http;
            http.begin(SERVER_URL);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-API-Key", API_KEY);
            int code = http.POST(payload);
            http.end();

            Serial.println("\n[LOOP] Payload sent (HTTP " + String(code) + "): " + payload);
        } else {
            Serial.println("[LOOP] Wi-Fi disconnected, skipping upload");
            if ((millis() - lastReconnectAttempt) > reconnectDelay) {
                lastReconnectAttempt = millis();
                Serial.println("[LOOP] Attempting to reconnect to Wi-Fi...");
                WiFi.disconnect();
                WiFi.begin(WIFI_SSID, WIFI_PASS);
            }
        }
    }
}
