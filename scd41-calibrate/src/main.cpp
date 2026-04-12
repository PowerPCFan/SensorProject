#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>

SensirionI2cScd4x scd4x;

const uint16_t CALIBRATION_TARGET_PPM = 420; // global co2 level outside
const unsigned long STABILIZE_MS = 10 * 60 * 1000; // 10 mins

void setup() {
    Serial.begin(115200);
    delay(10000);
    Serial.println("SCD41 Forced Recalibration");
    Serial.println("Make sure the sensor is outside or near an open window.");
    Serial.println("Target: " + String(CALIBRATION_TARGET_PPM) + " ppm");
    Serial.println();

    Wire.begin(21, 22);
    Wire.setClock(100000);

    uint16_t error = 0;
    char errorMessage[128];
    uint64_t serialNumber = 0;

    scd4x.begin(Wire, 0x62);
    delay(30);

    scd4x.wakeUp();
    scd4x.stopPeriodicMeasurement();
    delay(500);
    scd4x.reinit();
    delay(1000);

    error = scd4x.getSerialNumber(serialNumber);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("Failed to read serial number: ") + errorMessage);
        Serial.println("Check wiring and restart.");
        while (true) delay(1000);
    }
    Serial.println(String("SCD41 detected, serial: ") + String((unsigned long)(serialNumber >> 32), HEX) + String((unsigned long)(serialNumber & 0xFFFFFFFF), HEX));

    error = scd4x.startPeriodicMeasurement();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("Failed to start measurement: ") + errorMessage);
        while (true) delay(1000);
    }

    Serial.println("Stabilizing for " + String(STABILIZE_MS / 60000) + " minutes, keep the sensor outside...");
    Serial.println();

    unsigned long startMs = millis();
    const char* spinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    uint8_t spidx = 0;

    while ((millis() - startMs) < STABILIZE_MS) {
        unsigned long remaining = (STABILIZE_MS - (millis() - startMs)) / 1000;
        Serial.print("\r");
        Serial.print(spinner[spidx]);
        Serial.print(" Waiting... ");
        Serial.print(remaining);
        Serial.print("s remaining   ");
        spidx = (spidx + 1) % 10;

        bool dataReady = false;
        scd4x.getDataReadyStatus(dataReady);
        if (dataReady) {
            uint16_t co2 = 0;
            float temp = 0, hum = 0;
            if (!scd4x.readMeasurement(co2, temp, hum) && co2 > 0) {
                Serial.print("| Current CO2: ");
                Serial.print(co2);
                Serial.print(" ppm   ");
            }
        }

        delay(1000);
    }

    Serial.println();
    Serial.println();
    Serial.println("Stabilization complete. Running forced recalibration...");

    scd4x.stopPeriodicMeasurement();
    delay(500);

    uint16_t frcCorrection = 0;
    error = scd4x.performForcedRecalibration(CALIBRATION_TARGET_PPM, frcCorrection);
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("Calibration failed: ") + errorMessage);
        while (true) delay(1000);
    }

    int16_t correctionPpm = (int16_t)frcCorrection - 0x8000;
    Serial.println("Calibration successful!");
    Serial.println("Correction applied: " + String(correctionPpm) + " ppm");

    error = scd4x.persistSettings();
    if (error) {
        errorToString(error, errorMessage, sizeof(errorMessage));
        Serial.println(String("Failed to persist settings: ") + errorMessage);
        Serial.println("Calibration was applied but will be lost on power cycle.");
    } else {
        Serial.println("Settings saved to flash.");
    }

    Serial.println();
    Serial.println("Done. Flash your main firmware now.");
}

void loop() {}
