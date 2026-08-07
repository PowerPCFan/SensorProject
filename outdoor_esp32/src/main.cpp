#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_AHTX0.h>
#include <WiFi.h>
#include <math.h>

#ifdef VOLTAGE_PIN
float readESP32Voltage() {
    uint32_t raw_mv_sum = 0;
    const int samples = 10;

    for (int i = 0; i < samples; i++) {
        raw_mv_sum += analogReadMilliVolts(VOLTAGE_PIN);
        delay(2);
    }
    float pin_volts = (raw_mv_sum / (float)samples) / 1000.0f;

    // using 10k and 100k resistors, so i need to multiply by 11
    return pin_volts * 11.0f;
}
#else
float readESP32Voltage() {
    return 0.0f;
}
#endif

#if !defined(ESPNOW_ENABLED) && defined(DEBUG_SERVER)
#include <WebServer.h>

WebServer debugServer(80);
bool serverStarted = false;
String logBuffer = "";
const size_t MAX_LOG_SIZE = 4096;

void appendLogBuffer(const String& str) {
    logBuffer += str;
    if (logBuffer.length() > MAX_LOG_SIZE) {
        logBuffer = logBuffer.substring(logBuffer.length() - (MAX_LOG_SIZE / 2));
    }
}

void logMsg(const String &msg) {
    Serial.println(msg);
    appendLogBuffer(msg + "\n");
}

void logPrintf(const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Serial.print(buf);
    appendLogBuffer(String(buf));
}

void setupDebugServer() {
    if (!serverStarted) {
        debugServer.on("/", []() {
            debugServer.send(200, "text/plain", logBuffer.length() == 0 ? "No logs recorded yet.\n" : logBuffer);
        });
        debugServer.begin();
        serverStarted = true;
        logMsg("[DEBUG_SERVER] Server started (http://" + WiFi.localIP().toString() + "/)");
    }
}
#else
#define logMsg(msg) Serial.println(msg)
#define logPrintf(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#endif

#ifdef ESPNOW_ENABLED
#include <esp_now.h>
#include <esp_wifi.h>

const uint64_t TIME_TO_SLEEP_US = (uint64_t)POST_DELAY_MS * 1000ULL;
#else
#include <HTTPClient.h>
#include <ArduinoJson.h>

unsigned long lastTime = 0;
const unsigned long timerDelay = POST_DELAY_MS;

void goToDeepSleep(uint64_t seconds, const char* reason) {
    logPrintf("\n[DEEP_SLEEP] %s. Sleeping for %llu seconds...\n", reason, seconds);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    esp_deep_sleep_start();
}

bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    const int maxAttempts = 10;
    const unsigned long attemptTimeoutMs = 15000; // 15 secs per attempt
    const char* spinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        if (WiFi.status() == WL_CONNECTED) {
            return true;
        }

        logPrintf("\n[WIFI] Connection attempt %d of %d...\n", attempt, maxAttempts);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS, 6);

        unsigned long startAttempt = millis();
        uint8_t spidx = 0;

        while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < attemptTimeoutMs) {
            Serial.print("\r[WIFI] Connecting ");
            Serial.print(spinner[spidx]);
            spidx = (spidx + 1) % 10;
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            logMsg("\r[WIFI] Connected! | Local IP: " + WiFi.localIP().toString());
#if !defined(ESPNOW_ENABLED) && defined(DEBUG_SERVER)
            setupDebugServer();
#endif
            return true;
        }

        logPrintf("\r[WIFI] Attempt %d failed or timed out after 15s.\n", attempt);
        if (attempt < maxAttempts) {
            delay(1000); // 1-second pause between attempts
        }
    }

    return false;
}
#endif

Adafruit_BME280 bme;
Adafruit_AHTX0 aht;
TwoWire internalBus = TwoWire(1);
bool bmeReady = false;
bool ahtReady = false;

#ifdef ESPNOW_ENABLED
uint8_t targetAddress[6];

typedef struct __attribute__((packed)) struct_message {
    float temperature;
    float humidity;
    float pressure;
    float internal_temperature;
    float internal_humidity;
    float esp32_temperature;
    float esp32_voltage;
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

volatile bool deliveryComplete = false;
volatile bool deliverySuccess = false;

// RTC_DATA_ATTR uint8_t lastWorkingChannel = 0;

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    deliverySuccess = (status == ESP_NOW_SEND_SUCCESS);
    deliveryComplete = true;
}

bool sendOnChannel(uint8_t channel) {
    peerInfo.channel = channel;

    if (esp_now_add_peer(&peerInfo) == ESP_ERR_ESPNOW_EXIST) {
        esp_now_mod_peer(&peerInfo);
    }

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    deliveryComplete = false;
    deliverySuccess = false;

    esp_now_send(targetAddress, (uint8_t *) &myData, sizeof(myData));

    int wait_ms = 0;
    while (!deliveryComplete && wait_ms < 50) {
        delay(1);
        wait_ms++;
    }

    return deliverySuccess;
}
#endif

void setup() {
#if defined(SLOWEST_CLOCK)
    constexpr int CPU_MHZ = 80;
#elif defined(SLOW_CLOCK)
    constexpr int CPU_MHZ = 160;
#else
    constexpr int CPU_MHZ = 240;
#endif

    if (!setCpuFrequencyMhz(CPU_MHZ)) {
        logMsg("[SETUP] Failed to set CPU frequency to " + String(CPU_MHZ) + " MHz");
    } else {
        logMsg("[SETUP] CPU frequency set to " + String(CPU_MHZ) + " MHz" + (CPU_MHZ == 240 ? " (default)" : "") + (CPU_MHZ == getCpuFrequencyMhz() ? " (confirmed)" : " (WARN: mismatch between getCpuFrequencyMhz() and requested frequency)"));
    }

    Serial.begin(115200);
    logMsg("\n[SETUP] Starting setup...");
    logPrintf("[SETUP] CPU Frequency: %u MHz\n", getCpuFrequencyMhz());

#ifdef VOLTAGE_PIN
    pinMode(VOLTAGE_PIN, INPUT);
#endif

    Wire.begin(21, 22);
    Wire.setClock(100000);

    internalBus.begin(25, 26);
    internalBus.setClock(100000);

    if (!bme.begin(0x76)) {
        if (!bme.begin(0x77)) {
            logMsg("[BME280] Could not find a valid BME280 sensor, check wiring!");
        } else {
            bmeReady = true;
            logMsg("[BME280] BME280 found at 0x77");
        }
    } else {
        bmeReady = true;
        logMsg("[BME280] BME280 found at 0x76");
    }

    if (!aht.begin(&internalBus, 0, 38U)) {
        logMsg("[AHT20] Could not find a valid AHT20 sensor on pins 25/26 with address 0x38");
    } else {
        ahtReady = true;
        logMsg("[AHT20] AHT20 found on internal bus");
    }

#ifdef ESPNOW_ENABLED
    WiFi.mode(WIFI_STA);
    if (esp_wifi_set_max_tx_power(84) != ESP_OK) {
        int8_t currentTx;
        esp_wifi_get_max_tx_power(&currentTx);
        logMsg("[ESP-NOW] Failed to set ESP-NOW Wi-Fi transmit power to 84 (20dBm); currently set to " + String(currentTx * 0.25f) + " dBm");
    }
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        logMsg("[ESP-NOW] Error initializing ESP-NOW");
    } else {
        esp_now_register_send_cb(OnDataSent);

        // parse mac address
        int mac[6];
        if (6 == sscanf(ESPNOW_NODE, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5])) {
            for (int i = 0; i < 6; ++i) {
                targetAddress[i] = (uint8_t) mac[i];
            }

            logPrintf(
                "[ESP-NOW] Parsed Receiver Node MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                targetAddress[0], targetAddress[1], targetAddress[2],
                targetAddress[3], targetAddress[4], targetAddress[5]
            );
        } else {
            logMsg("[ESP-NOW] ERROR: Failed to parse MAC address from ESPNOW_NODE build flag");
        }

        memcpy(peerInfo.peer_addr, targetAddress, 6);
        peerInfo.encrypt = false;

        if (bmeReady) {
            myData.temperature = bme.readTemperature();
            myData.humidity = bme.readHumidity();
            myData.pressure = bme.readPressure() / 100.0f;
            if (ahtReady) {
                sensors_event_t humidityEvent;
                sensors_event_t tempEvent;
                aht.getEvent(&humidityEvent, &tempEvent);
                myData.internal_temperature = tempEvent.temperature;
                myData.internal_humidity = humidityEvent.relative_humidity;
            } else {
                myData.internal_temperature = NAN;
                myData.internal_humidity = NAN;
            }
            myData.esp32_temperature = temperatureRead();
            myData.esp32_voltage = readESP32Voltage();

            bool success = false;

            const uint8_t CHANNEL = 6;
            logPrintf("[ESP-NOW] Sending on channel %d...\n", CHANNEL);
            if (sendOnChannel(CHANNEL)) {
                logPrintf("[ESP-NOW] Delivery Success on channel %d\n", CHANNEL);
                success = true;
            }

            // if (lastWorkingChannel >= 1 && lastWorkingChannel <= 11) {
            //     logPrintf("[ESP-NOW] Trying cached channel %d...\n", lastWorkingChannel);
            //     if (sendOnChannel(lastWorkingChannel)) {
            //         logPrintf("[ESP-NOW] Delivery Success on cached channel %d\n", lastWorkingChannel);
            //         success = true;
            //     } else {
            //         logPrintf("[ESP-NOW] Delivery failed on cached channel %d, starting channel sweep...\n", lastWorkingChannel);
            //     }
            // }
            // 
            // if (!success) {
            //     for (uint8_t channel = 1; channel <= 11; channel++) {
            //         if (channel == lastWorkingChannel) continue;
            // 
            //         if (sendOnChannel(channel)) {
            //             logPrintf("[ESP-NOW] Delivery Success on channel %d, saving to RTC memory\n", channel);
            //             lastWorkingChannel = channel;
            //             success = true;
            //             break;
            //         }
            //     }
            // }

            if (!success) {
                // logMsg("[ESP-NOW] Delivery Failed on all channels.");
                logPrintf("[ESP-NOW] Delivery Failed on channel %d.\n", CHANNEL);
            }
        }
    }

    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_US);
    logMsg("[DEEP_SLEEP] Going to sleep for " + String((uint32_t)TIME_TO_SLEEP_US) + " microseconds.");
    Serial.flush(); 
    esp_deep_sleep_start();
#else
    if (!connectWiFi()) {
        goToDeepSleep(600, "All 10 Wi-Fi connection attempts failed");
    }
#endif
}

void loop() {
#ifdef ESPNOW_ENABLED
    // we are using setup as loop
    // since it deep sleeps at the end meaning itll reset and then rerun setup
#else
#ifdef DEBUG_SERVER
    debugServer.handleClient();
#endif

    if ((millis() - lastTime) > timerDelay || lastTime == 0) {
        lastTime = millis();

        if (WiFi.status() != WL_CONNECTED) {
            logMsg("[LOOP] Wi-Fi disconnected. Attempting to reconnect...");
            if (!connectWiFi()) {
                goToDeepSleep(600, "Wi-Fi reconnection failed after 10 attempts");
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            JsonDocument doc;

            if (bmeReady) {
                // measurements:
                // - temp: celsius (c)
                // - humidity: relative humidity (%)
                // - pressure: hectopascals (hPa)

                doc["temperature"] = bme.readTemperature();
                doc["humidity"] = bme.readHumidity();
                doc["pressure"] = bme.readPressure() / 100.0f;
            }

            if (ahtReady) {
                sensors_event_t humidityEvent;
                sensors_event_t tempEvent;
                aht.getEvent(&humidityEvent, &tempEvent);
                doc["internal_temperature"] = tempEvent.temperature;
                doc["internal_humidity"] = humidityEvent.relative_humidity;
            }

            doc["esp32_temperature"] = temperatureRead();
            doc["esp32_voltage"] = readESP32Voltage();
            doc["wifi_strength"] = WiFi.RSSI();

            String payload;
            serializeJson(doc, payload);

            HTTPClient http;
            http.begin(SERVER_URL);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-API-Key", API_KEY);

            int code = http.POST(payload);
            http.end();

            logMsg("\n[LOOP] Payload sent (HTTP " + String(code) + "): " + payload);
        }
    }
#endif
}
