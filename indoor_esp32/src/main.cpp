#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"
#include <SensirionI2CSgp40.h>
#include <SensirionI2cScd4x.h>
#include <VOCGasIndexAlgorithm.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <math.h>
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_mac.h"

unsigned long lastTime = 0;
const unsigned long timerDelay = POST_DELAY_MS;  // build flag
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectDelay = 30 * 60 * 1000;

#ifndef WARMUP_SECONDS
#define WARMUP_SECONDS 0
#endif

#ifndef USE_BLUETOOTH_SERIAL
#define USE_BLUETOOTH_SERIAL 0
#endif

const unsigned long warmupDelayMs = (unsigned long)WARMUP_SECONDS * 1000UL;
unsigned long warmupStartMs = 0;
unsigned long warmupLastLogMs = 0;

const bool USE_BME680 = true;
const bool USE_PMS5003 = true;
const bool USE_SCD41 = true;
const bool USE_SGP40 = true;

TwoWire secondaryWire(1);
Adafruit_BME680 bme(&secondaryWire);
SensirionI2CSgp40 sgp40;
VOCGasIndexAlgorithm vocAlgorithm;
SensirionI2cScd4x scd4x;
HardwareSerial pmsSerial(2);

#if USE_BLUETOOTH_SERIAL
#include <NimBLEDevice.h>

static const char* NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";

class BridgeServerCallbacks : public NimBLEServerCallbacks {
public:
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        (void)pServer;
        (void)connInfo;
        (void)reason;
        NimBLEDevice::startAdvertising();
    }
};

class SerialBridge : public Print {
public:
    explicit SerialBridge(HardwareSerial &usb) : usbSerial(usb) {}

    bool begin(unsigned long baudRate) {
        usbSerial.begin(baudRate);
        return true;
    }

    bool enableBluetooth(const char *deviceName) {
        if (bleEnabled) return true;

        NimBLEDevice::init(deviceName);
        NimBLEServer* pServer = NimBLEDevice::createServer();
        pServer -> setCallbacks(new BridgeServerCallbacks());
        pServer -> advertiseOnDisconnect(true);
        NimBLEService* pService = pServer -> createService(NUS_SERVICE_UUID);

        pTxChar = pService -> createCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", NIMBLE_PROPERTY::NOTIFY);

        pServer -> start();

        NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
        adv -> setName(deviceName);
        adv -> enableScanResponse(true);
        adv -> addServiceUUID(NUS_SERVICE_UUID);
        adv -> start();

        bleEnabled = true;
        return true;
    }

    void endBluetooth() {
        if (!bleEnabled) return;
        NimBLEDevice::deinit(true);
        bleEnabled = false;
    }

    size_t write(uint8_t byte) override {
        size_t written = usbSerial.write(byte);
        if (bleEnabled && pTxChar) {
            uint8_t b = byte;
            pTxChar -> setValue(&b, 1);
            pTxChar -> notify();
        }
        return written;
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        size_t written = usbSerial.write(buffer, size);
        if (bleEnabled && pTxChar) {
            pTxChar -> setValue(buffer, size);
            pTxChar -> notify();
        }
        return written;
    }

    using Print::write;

private:
    HardwareSerial &usbSerial;
    NimBLEServer* pServer = nullptr;
    NimBLEService* pService = nullptr;
    NimBLECharacteristic* pTxChar = nullptr;
    bool bleEnabled = false;
};

SerialBridge SerialBridgeInstance(::Serial);
#define MySerial SerialBridgeInstance
#endif

#ifndef MySerial
#define MySerial Serial
#endif

struct PMSData {
    float pm1_0 = 0, pm2_5 = 0, pm10_0 = 0;

    uint16_t p_03um = 0;
    uint16_t p_05um = 0;
    uint16_t p_10um = 0;
    uint16_t p_25um = 0;
    uint16_t p_50um = 0;
    uint16_t p_100um = 0;
};
PMSData pmsValues;

uint16_t safeSubtract(uint16_t a, uint16_t b) {
    return (a > b) ? (a - b) : 0;
}

bool readPMSdata(HardwareSerial *s, PMSData &data) {
    if (s -> available() < 32) return false;

    // Frame sync: consume bytes until we find header 0x42 0x4D.
    while (s -> available() >= 2) {
        uint8_t b = s -> read();
        if (b != 0x42) continue;

        if (s -> peek() != 0x4D) {
            continue;
        }

        uint8_t buffer[32];
        buffer[0] = 0x42;
        size_t got = s -> readBytes(&buffer[1], 31);
        if (got != 31) return false;

        uint16_t frameLen = ((uint16_t)buffer[2] << 8) | buffer[3];
        if (frameLen != 28) return false;

        uint16_t sum = 0;
        for (uint8_t i = 0; i < 30; i++) sum += buffer[i];
        uint16_t checksum = ((uint16_t)buffer[30] << 8) | buffer[31];

        if (sum != checksum) {
            MySerial.println("[PMS5003] checksum mismatch");
            return false;
        }

        data.pm1_0 = (float)((buffer[10] << 8) | buffer[11]);
        data.pm2_5 = (float)((buffer[12] << 8) | buffer[13]);
        data.pm10_0 = (float)((buffer[14] << 8) | buffer[15]);

        data.p_03um  = (buffer[16] << 8) | buffer[17];
        data.p_05um  = (buffer[18] << 8) | buffer[19];
        data.p_10um  = (buffer[20] << 8) | buffer[21];
        data.p_25um  = (buffer[22] << 8) | buffer[23];
        data.p_50um  = (buffer[24] << 8) | buffer[25];
        data.p_100um = (buffer[26] << 8) | buffer[27];

        return true;
    }

    return false;
}

bool bme680Ready = false;
uint8_t bme680Address = 0;
float lastBmeTemperature = 25.0f;
float lastBmeHumidity = 50.0f;
bool bme680HasReading = false;

bool sgp40Ready = false;
bool sgp40HasReading = false;
uint16_t sgp40LastRawSignal = 0;

bool isWarmupActive() {
    if (warmupDelayMs <= 0) return false;
    return (millis() - warmupStartMs) < warmupDelayMs;
}

bool similar(float a, float b, float eps = 0.001f) {
    return fabsf(a - b) <= eps;
}

bool isFallback(float temp, float hum, float press, float gas) {
    return similar(temp, 33.71479f) && similar(hum, 100.0f) && similar(press, 673.47f) && similar(gas, 0.0f);
}

uint16_t sgp40TemperatureToTicks(float temperature) {
    // ticks = (degC + 45) * 65535 / 175

    if (temperature < -45.0f) { temperature = -45.0f; }
    if (temperature > 130.0f) { temperature = 130.0f; }

    return (uint16_t)((temperature + 45.0f) * 65535.0f / 175.0f);
}

uint16_t sgp40HumidityToTicks(float humidity) {
    // ticks = %RH * 65535 / 100

    if (humidity < 0.0f) { humidity = 0.0f; }
    if (humidity > 100.0f) { humidity = 100.0f; }

    return (uint16_t)(humidity * 65535.0f / 100.0f);
}

void setupBME680() {
    secondaryWire.begin(25, 26, 100000);

    if (!bme.begin(0x76)) {
        // 0x76 is invalid, try 0x77
        if (!bme.begin(0x77)) {
            MySerial.println("[BME680] BME680 not found, disabling BME680 module");
            bme680Ready = false;
            return;
        } else {
            bme680Address = 0x77;
            MySerial.println("[BME680] BME680 found (0x77)");
        }
    } else {
        bme680Address = 0x76;
        MySerial.println("[BME680] BME680 found (0x76)");
    }

    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    bme680Ready = true;
    MySerial.println("[BME680] BME680 ready");
}

void addBME680Json(JsonDocument &doc) {
    if (!bme680Ready) return;

    if (!bme.performReading()) {
        MySerial.println("[BME680] Reading failed");
        return;
    }

    float temperature = bme.temperature;
    float humidity = bme.humidity;
    float pressure = bme.pressure / 100.0f;
    float gas = bme.gas_resistance / 1000.0f;

    if (isFallback(temperature, humidity, pressure, gas)) {
        MySerial.println("[BME680] Fallback values detected, retrying sensor read");
        delay(250);

        if (bme.begin(bme680Address) && bme.performReading()) {
            temperature = bme.temperature;
            humidity = bme.humidity;
            pressure = bme.pressure / 100.0f;
            gas = bme.gas_resistance / 1000.0f;

            if (isFallback(temperature, humidity, pressure, gas)) {
                MySerial.println("[BME680] Retry succeeded but returned fallback values again, giving up on this reading");
                return;
            } else {
                MySerial.println("[BME680] Retry successful");
            }
        } else {
            MySerial.println("[BME680] Retry failed");
            return;
        }
    }

    lastBmeTemperature = temperature;
    lastBmeHumidity = humidity;
    bme680HasReading = true;

    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["pressure"] = pressure;
    doc["gas"] = gas;
}

void setupSGP40() {
    sgp40.begin(secondaryWire);
    MySerial.println("[SGP40] SGP40 ready");
    sgp40Ready = true;

    // self test - uncomment to run
    // uint16_t selfTestResult = 0;
    // uint16_t error = sgp40.executeSelfTest(selfTestResult);
    // MySerial.println(String("[SGP40] Self test result (0xD400 = pass): 0x") + String(selfTestResult, HEX) + String(" (error occurred: ") + String(error) + String(")"));
}

void addSGP40Json(JsonDocument &doc) {
    if (!sgp40Ready) return;

    uint16_t humidityTicks = bme680HasReading ? sgp40HumidityToTicks(lastBmeHumidity) : 0x8000;
    uint16_t temperatureTicks = bme680HasReading ? sgp40TemperatureToTicks(lastBmeTemperature) : 0x6666;

    uint16_t rawSignal = 0;
    uint16_t error = sgp40.measureRawSignal(humidityTicks, temperatureTicks, rawSignal);

    if (error) {
        MySerial.println(String("[SGP40] Reading failed: ") + String(error));
        return;
    }

    sgp40LastRawSignal = rawSignal;
    sgp40HasReading = true;

    int32_t vocIndex = vocAlgorithm.process(rawSignal);
    if (vocIndex > 0) {
        doc["vocs"] = vocIndex;
    } else {
        MySerial.println(String("[SGP40] VOC algorithm returned invalid index: `") + String(vocIndex) + "`. This can either indicate the sensor warming up, an error in the algorithm, or unexpectedly clean air. Raw value: " + String(rawSignal));
    }
}

void setupPMS5003() {
    pmsSerial.begin(9600, SERIAL_8N1, 16, 17);
}

void pollPMS5003() {
    while (pmsSerial.available() >= 32) {
        readPMSdata(&pmsSerial, pmsValues);
    }
}

void addPMS5003Json(JsonDocument &doc) {
    doc["pm1_0"] = pmsValues.pm1_0;
    doc["pm2_5"] = pmsValues.pm2_5;
    doc["pm10_0"] = pmsValues.pm10_0;

    uint16_t bin_03_05 = safeSubtract(pmsValues.p_03um, pmsValues.p_05um);
    uint16_t bin_05_10 = safeSubtract(pmsValues.p_05um, pmsValues.p_10um);
    uint16_t bin_10_25 = safeSubtract(pmsValues.p_10um, pmsValues.p_25um);
    uint16_t bin_25_50 = safeSubtract(pmsValues.p_25um, pmsValues.p_50um);
    uint16_t bin_50_100 = safeSubtract(pmsValues.p_50um, pmsValues.p_100um);
    uint16_t bin_100_plus = pmsValues.p_100um;

    JsonObject pm_bins = doc.createNestedObject("pm_bins");

    pm_bins["bin_03_05"] = bin_03_05;
    pm_bins["bin_05_10"] = bin_05_10;
    pm_bins["bin_10_25"] = bin_10_25;
    pm_bins["bin_25_50"] = bin_25_50;
    pm_bins["bin_50_100"] = bin_50_100;
    pm_bins["bin_100_plus"] = bin_100_plus;
}

bool scd41Ready = false;
bool scd41HasReading = false;
uint16_t scd41LastCo2 = 0;
unsigned long scd41LastPollMs = 0;
unsigned long scd41LastNoDataLogMs = 0;
unsigned long scd41LastNotReadyLogMs = 0;
unsigned long scd41LastStallLogMs = 0;
unsigned long scd41LastReadingMs = 0;
unsigned long scd41StartMs = 0;

void setupSCD41() {
    uint16_t error = 0;
    char errorMessage[128];
    uint64_t serialNumber = 0;

    scd4x.begin(Wire, 0x62);

    delay(30);

    error = scd4x.wakeUp();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Failed to wake up: ") + errorMessage);
    }

    error = scd4x.stopPeriodicMeasurement();
    delay(500);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Failed to stop periodic measurement: ") + errorMessage);
    }

    error = scd4x.reinit();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Failed to reinitialize: ") + errorMessage);
        MySerial.println(String("[SCD41] The SCD41 module is now disabled."));
        scd41Ready = false;
        return;
    }

    error = scd4x.getSerialNumber(serialNumber);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Serial read failed: ") + errorMessage);
        MySerial.println(String("[SCD41] The SCD41 module is now disabled."));
        scd41Ready = false;
        return;
    }
    MySerial.println(String("[SCD41] SCD41 ready (serial: ") + String((unsigned long)(serialNumber >> 32), HEX) + String((unsigned long)(serialNumber & 0xFFFFFFFF), HEX) + String(")"));

    // self test - uncomment to run
    // uint16_t sensorStatus = 0;
    // error = scd4x.performSelfTest(sensorStatus);
    // MySerial.println(String("[SCD41] Self test result: 0x") + String(sensorStatus, HEX) + String(error ? String(", error: ") + String(error) : ", no error"));

    // disable ASC since this is an indoor sensor and it expects 400ppm like once a week or something which it wont get
    scd4x.setAutomaticSelfCalibrationEnabled(0);
    scd4x.persistSettings();

    delay(1000);
    error = scd4x.startPeriodicMeasurement();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Failed to start periodic measurement: ") + errorMessage);
        MySerial.println(String("[SCD41] The SCD41 module is now disabled."));
        scd41Ready = false;
        return;
    }

    scd41StartMs = millis();
    scd41LastPollMs = 0;
    scd41Ready = true;
}

void addSCD41Json(JsonDocument &doc) {
    if (!scd41Ready) return;
    if (!scd41HasReading) return;

    doc["carbon_dioxide"] = scd41LastCo2;
}

void pollSCD41() {
    if (!scd41Ready) {
        if ((millis() - scd41LastNotReadyLogMs) > 10000) {
            MySerial.println("[SCD41] Sensor not ready; skipping poll");
            scd41LastNotReadyLogMs = millis();
        }
        return;
    }

    if ((millis() - scd41StartMs) < 10000) return;
    if ((millis() - scd41LastPollMs) < 1000) return;
    scd41LastPollMs = millis();

    unsigned long now = millis();
    if ((now - scd41StartMs) > 90000) {
        unsigned long last = scd41HasReading ? scd41LastReadingMs : scd41StartMs;
        if ((now - last) > 90000 && (now - scd41LastStallLogMs) > 10000) {
            MySerial.println("[SCD41] No new data for 90s, likely a stall");
            scd41LastStallLogMs = now;
        }
    }

    uint16_t error = 0;
    char errorMessage[128];
    bool dataReady = false;

    error = scd4x.getDataReadyStatus(dataReady);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Ready check failed: ") + errorMessage);
        return;
    }

    if (!dataReady) {
        if ((millis() - scd41LastNoDataLogMs) > 10000) {
            MySerial.println("[SCD41] Data not ready");
            scd41LastNoDataLogMs = millis();
        }
        return;
    }

    uint16_t co2 = 0;
    float temperature = 0;
    float humidity = 0;

    error = scd4x.readMeasurement(co2, temperature, humidity);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        MySerial.println(String("[SCD41] Read failed: ") + errorMessage);
        return;
    }

    if (co2 == 0) return;

    scd41LastCo2 = co2;
    scd41HasReading = true;
    scd41LastReadingMs = millis();
}

void setup() {
    MySerial.begin(115200);
    delay(10000); // 10 second delay to open serial monitor
    MySerial.println("[SETUP] Starting setup...");

#if USE_BLUETOOTH_SERIAL
    if (MySerial.enableBluetooth("Indoor-ESP32")) {
        MySerial.println("[BT] Bluetooth serial ready as `Indoor-ESP32`");
    } else {
        MySerial.println("[BT] Failed to initialize Bluetooth serial");
    }
#endif

    Wire.begin(21, 22);
    Wire.setClock(100000);

    if (USE_BME680) setupBME680();
    if (USE_PMS5003) setupPMS5003();
    if (USE_SCD41) setupSCD41();
    if (USE_SGP40) setupSGP40();

    warmupStartMs = millis();
    if (warmupDelayMs > 0) {
        MySerial.println("[SETUP] Warmup enabled for " + String(WARMUP_SECONDS) + "s; API uploads will be paused during this time to stabilize measurements");
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const char* spinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    uint8_t spidx = 0;
    while (WiFi.status() != WL_CONNECTED) {
        MySerial.print("\r[SETUP] Connecting to Wi-Fi ");
        MySerial.print(spinner[spidx]);
        spidx = (spidx + 1) % 10;
        delay(100);
    }
    MySerial.println("\r[SETUP] Wi-Fi connected | Local IP: " + WiFi.localIP().toString());

    // Diagnostics block
    esp_chip_info_t chipInfo;
    esp_chip_info(&chipInfo);
    MySerial.println("[SETUP] ESP32 Model: " + String(ESP.getChipModel()) + "; Cores: " + String(chipInfo.cores) + "; Revision: " + String(chipInfo.revision) + "; Flash Size: " + String(ESP.getFlashChipSize() / (1024 * 1024)) + "MB");
    uint64_t chipid = ESP.getEfuseMac();
    MySerial.printf("[SETUP] Chip ID (Efuse MAC base): %04X%08X\n", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    uint8_t wifiMac[6];
    esp_read_mac(wifiMac, ESP_MAC_WIFI_STA);
    MySerial.printf("[SETUP] Wi-Fi MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", wifiMac[0], wifiMac[1], wifiMac[2], wifiMac[3], wifiMac[4], wifiMac[5]);
    uint8_t btMac[6];
    esp_read_mac(btMac, ESP_MAC_BT);
    MySerial.printf("[SETUP] BLE MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", btMac[0], btMac[1], btMac[2], btMac[3], btMac[4], btMac[5]);
}

void loop() {
    if (USE_PMS5003) pollPMS5003();
    if (USE_SCD41) pollSCD41();

    if (isWarmupActive()) {
        if ((millis() - warmupLastLogMs) > 5000) {
            warmupLastLogMs = millis();
            unsigned long warmupElapsedSeconds = (millis() - warmupStartMs) / 1000UL;
            MySerial.println("[LOOP] Warmup active (" + String(warmupElapsedSeconds) + "/" + String(WARMUP_SECONDS) + "s); skipping API upload");
        }
        delay(timerDelay / 2);
        return;
    }

    if ((millis() - lastTime) > timerDelay) {
        if (WiFi.status() == WL_CONNECTED) {
            JsonDocument doc;

            if (USE_BME680) addBME680Json(doc);
            if (USE_SGP40) addSGP40Json(doc);
            if (USE_PMS5003) addPMS5003Json(doc);
            if (USE_SCD41) addSCD41Json(doc);

            doc["wifi_strength"] = WiFi.RSSI();

            lastTime = millis();

            String payload;
            serializeJson(doc, payload);

            HTTPClient http;
            http.begin(SERVER_URL);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-API-Key", API_KEY);
            int code = http.POST(payload);
            http.end();

            MySerial.println("\n[LOOP] Payload sent (HTTP " + String(code) + "): " + payload);
        } else {
            MySerial.println("[LOOP] Wi-Fi disconnected, skipping upload");
            if ((millis() - lastReconnectAttempt) > reconnectDelay) {
                lastReconnectAttempt = millis();
                MySerial.println("[LOOP] Attempting to reconnect to Wi-Fi...");
                WiFi.disconnect();
                WiFi.begin(WIFI_SSID, WIFI_PASS);
            }
        }
    }
}