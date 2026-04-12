#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <math.h>

#ifdef ESPNOW_ENABLED
#include <esp_now.h>
#include <esp_wifi.h>

const uint64_t uS_TO_S_FACTOR = 1000000ULL;
const uint64_t TIME_TO_SLEEP = POST_DELAY_MS / 1000;
#else
#include <HTTPClient.h>
#include <ArduinoJson.h>

unsigned long lastTime = 0;
const unsigned long timerDelay = POST_DELAY_MS;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectDelay = 30 * 60 * 1000;
#endif

Adafruit_BME280 bme;
bool bmeReady = false;

#ifdef ESPNOW_ENABLED
uint8_t targetAddress[6];

typedef struct __attribute__((packed)) struct_message {
    float temperature;
    float humidity;
    float pressure;
    float esp32_temperature;
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

volatile bool deliveryComplete = false;
volatile bool deliverySuccess = false;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    deliverySuccess = (status == ESP_NOW_SEND_SUCCESS);
    deliveryComplete = true;
}
#endif

void setup() {
    Serial.begin(115200);
    Serial.println("\n[SETUP] Starting setup...");

    Wire.begin(21, 22);
    Wire.setClock(100000);

    if (!bme.begin(0x76)) {
        if (!bme.begin(0x77)) {
            Serial.println("[BME280] Could not find a valid BME280 sensor, check wiring!");
        } else {
            bmeReady = true;
            Serial.println("[BME280] BME280 found at 0x77");
        }
    } else {
        bmeReady = true;
        Serial.println("[BME280] BME280 found at 0x76");
    }

#ifdef ESPNOW_ENABLED
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] Error initializing ESP-NOW");
    } else {
        esp_now_register_send_cb(OnDataSent);

        // parse mac address
        int mac[6];
        if (6 == sscanf(ESPNOW_NODE, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5])) {
            for (int i = 0; i < 6; ++i) {
                targetAddress[i] = (uint8_t) mac[i];
            }

            Serial.printf(
                "[ESP-NOW] Parsed Receiver Node MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                targetAddress[0], targetAddress[1], targetAddress[2],
                targetAddress[3], targetAddress[4], targetAddress[5]
            );
        } else {
            Serial.println("[ESP-NOW] ERROR: Failed to parse MAC address from ESPNOW_NODE build flag");
        }

        memcpy(peerInfo.peer_addr, targetAddress, 6);
        peerInfo.encrypt = false;

        if (bmeReady) {
            myData.temperature = bme.readTemperature() - 0.5;
            myData.humidity = bme.readHumidity() - 2;
            myData.pressure = bme.readPressure() / 100.0f;
            myData.esp32_temperature = temperatureRead();

            bool success = false;
            for (uint8_t channel = 1; channel <= 11; channel++) {
                peerInfo.channel = channel;

                if (esp_now_add_peer(&peerInfo) == ESP_ERR_ESPNOW_EXIST) {
                    esp_now_mod_peer(&peerInfo);
                }

                esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

                deliveryComplete = false;
                deliverySuccess = false;

                esp_now_send(targetAddress, (uint8_t *) &myData, sizeof(myData));

                int wait_ms = 0;
                while (!deliveryComplete && wait_ms < 100) {
                    delay(1);
                    wait_ms++;
                }

                if (deliverySuccess) {
                    Serial.printf("[ESP-NOW] Delivery Success on channel %d\n", channel);
                    success = true;
                    break;
                }
            }

            if (!success) {
                Serial.println("[ESP-NOW] Delivery Failed on all channels.");
            }
        }
    }

    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    Serial.println("[DEEP_SLEEP] Going to sleep for " + String((uint32_t)TIME_TO_SLEEP) + " seconds.");
    Serial.flush(); 
    esp_deep_sleep_start();
#else
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
#endif
}

void loop() {
#ifdef ESPNOW_ENABLED
    // we are using setup as loop
    // since it deep sleeps at the end meaning itll reset and then rerun setup
#else
    if ((millis() - lastTime) > timerDelay || lastTime == 0) {
        lastTime = millis();

        if (WiFi.status() == WL_CONNECTED) {
            JsonDocument doc;

            if (bmeReady) {
                doc["temperature"] = bme.readTemperature() - 0.5f;
                doc["humidity"] = bme.readHumidity() - 2.0f;
                doc["pressure"] = bme.readPressure() / 100.0f;
            }

            doc["esp32_temperature"] = temperatureRead();
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
#endif
}
