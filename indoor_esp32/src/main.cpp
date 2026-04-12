#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"
#include <SensirionI2cScd4x.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <math.h>

unsigned long lastTime = 0;
const unsigned long timerDelay = POST_DELAY_MS;  // build flag
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectDelay = 30 * 60 * 1000;

const bool USE_BME680 = true;
const bool USE_PMS5003 = true;
const bool USE_SCD41 = true;

TwoWire bmeWire(1);
Adafruit_BME680 bme(&bmeWire);
SensirionI2cScd4x scd4x;
HardwareSerial pmsSerial(2);

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
    if (s->available() < 32) return false;

    while (s->available() && s->peek() != 0x42) s->read();
    if (s->available() < 32) return false;

    uint8_t buffer[32];
    s->readBytes(buffer, 32);

    if (buffer[0] != 0x42 || buffer[1] != 0x4D) return false;

    uint16_t sum = 0;
    for (uint8_t i = 0; i < 30; i++) sum += buffer[i];
    uint16_t checksum = ((uint16_t)buffer[30] << 8) | buffer[31];

    if (sum != checksum) {
        Serial.println("[PMS5003] checksum mismatch");
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

bool bme680Ready = false;
uint8_t bme680Address = 0;

// fb = fallback btw
const float FB_TEMP = 33.71479f;
const float FB_HUM = 100.0f;
const float FB_PRESS = 673.47f;
const float FB_GAS = 0.0f;

bool sim(float a, float b, float eps = 0.001f) { return fabsf(a - b) <= eps; }

bool isFallback(float temp, float hum, float press, float gas) {
    return sim(temp, FB_TEMP) && sim(hum, FB_HUM) && sim(press, FB_PRESS) && sim(gas, FB_GAS);
}

void setupBME680() {
    bmeWire.begin(25, 26, 100000);

    if (!bme.begin(0x76)) {
        // 0x76 is invalid, try 0x77
        if (!bme.begin(0x77)) {
            Serial.println("[BME680] BME680 not found, disabling BME680 module");
            bme680Ready = false;
            return;
        } else {
            bme680Address = 0x77;
            Serial.println("[BME680] BME680 found at address 0x77");
        }
    } else {
        bme680Address = 0x76;
        Serial.println("[BME680] BME680 found at address 0x76");
    }

    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    bme680Ready = true;
    Serial.println("[BME680] BME680 ready");
}

void addBME680Json(JsonDocument &doc) {
    if (!bme680Ready) return;

    if (!bme.performReading()) {
        Serial.println("[BME680] Reading failed");
        return;
    }

    float temperature = bme.temperature;
    float humidity = bme.humidity;
    float pressure = bme.pressure / 100.0f;
    float gas = bme.gas_resistance / 1000.0f;

    if (isFallback(temperature, humidity, pressure, gas)) {
        Serial.println("[BME680] Fallback values detected, retrying sensor read");
        delay(250);

        if (bme.begin(bme680Address) && bme.performReading()) {
            temperature = bme.temperature;
            humidity = bme.humidity;
            pressure = bme.pressure / 100.0f;
            gas = bme.gas_resistance / 1000.0f;

            if (isFallback(temperature, humidity, pressure, gas)) {
                Serial.println("[BME680] Retry succeeded but returned fallback values again, giving up on this reading");
                return;
            } else {
                Serial.println("[BME680] Retry successful");
            }
        } else {
            Serial.println("[BME680] Retry failed");
            return;
        }
    }

    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["pressure"] = pressure;
    doc["gas"] = gas;
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
        Serial.println(String("[SCD41] Failed to wake up: ") + errorMessage);
    }

    error = scd4x.stopPeriodicMeasurement();
    delay(500);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("[SCD41] Failed to stop periodic measurement: ") + errorMessage);
    }

    error = scd4x.reinit();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("[SCD41] Failed to reinitialize: ") + errorMessage);
        Serial.println(String("[SCD41] The SCD41 module is now disabled."));
        scd41Ready = false;
        return;
    }

    error = scd4x.getSerialNumber(serialNumber);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("[SCD41] Serial read failed: ") + errorMessage);
        Serial.println(String("[SCD41] The SCD41 module is now disabled."));
        scd41Ready = false;
        return;
    }
    Serial.println(String("[SCD41] SCD41 detected, serial: ") + String((unsigned long)(serialNumber >> 32), HEX) + String((unsigned long)(serialNumber & 0xFFFFFFFF), HEX));

    // self test - uncomment to run

    // uint16_t sensorStatus = 0;
    // error = scd4x.performSelfTest(sensorStatus);
    // Serial.println(String("[SCD41] Self test result: 0x") + String(sensorStatus, HEX) + String(error ? String(", error: ") + String(error) : ", no error"));

    // disable ASC since this is an indoor sensor and it expects 400ppm like once a week or something which it wont get
    scd4x.setAutomaticSelfCalibrationEnabled(0);
    scd4x.persistSettings();

    delay(1000);
    error = scd4x.startPeriodicMeasurement();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("[SCD41] Failed to start periodic measurement: ") + errorMessage);
        Serial.println(String("[SCD41] The SCD41 module is now disabled."));
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
    if (!scd41Ready) return;
    if ((millis() - scd41StartMs) < 10000) return;
    if ((millis() - scd41LastPollMs) < 1000) return;
    scd41LastPollMs = millis();

    uint16_t error = 0;
    char errorMessage[128];
    bool dataReady = false;

    error = scd4x.getDataReadyStatus(dataReady);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("[SCD41] Ready check failed: ") + errorMessage);
        return;
    }

    if (!dataReady) return;

    uint16_t co2 = 0;
    float temperature = 0;
    float humidity = 0;

    error = scd4x.readMeasurement(co2, temperature, humidity);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("[SCD41] Read failed: ") + errorMessage);
        return;
    }

    if (co2 == 0) return;

    scd41LastCo2 = co2;
    scd41HasReading = true;
}

void setup() {
    Serial.begin(115200);
    delay(10000); // 10 second delay to open serial monitor
    Serial.println("[SETUP] Starting setup...");

    Wire.begin(21, 22);
    Wire.setClock(100000);

    if (USE_BME680) setupBME680();
    if (USE_PMS5003) setupPMS5003();
    if (USE_SCD41) setupSCD41();

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
}

void loop() {
    if (USE_PMS5003) pollPMS5003();
    if (USE_SCD41) pollSCD41();

    if ((millis() - lastTime) > timerDelay) {
        lastTime = millis();

        if (WiFi.status() == WL_CONNECTED) {
            JsonDocument doc;

            if (USE_BME680) addBME680Json(doc);
            if (USE_PMS5003) addPMS5003Json(doc);
            if (USE_SCD41) addSCD41Json(doc);

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