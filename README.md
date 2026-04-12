# SensorProject
Monorepo for different scripts and thingies for my sensor project

Note that the projects, although contained in their own directories, aren't independent. They all depend on each other to some extent, and must stay together in the SensorProject directory.

## Data
All data is stored in TimescaleDB.

### Indoor Metrics
* **Timestamp**: Timestamp of the reading
* **Temperature**: Stored in Celsius (°C)
* **Humidity**: The Relative Humidity (%RH) of the air
* **Barometric Pressure**: Stored as Hectopascals (hPa) / Millibars (mb)
* **Gas**: Gas Resistance (KΩ)
* **PM1.0, PM2.5, PM10.0**: Particulate Matter (in µg/m³)
* **Particulate Matter Bins**: Raw particle count per 0.1L of air
* **VOCs**: Volatile Organic Compounds Index
* **Carbon Monoxide**: Carbon Monoxide (ppm)
* **Carbon Dioxide**: Carbon Dioxide (ppm)
* **Wi-Fi Strength**: Wi-Fi RSSI Strength (dBm)

### Outdoor Metrics
* **Timestamp**: Timestamp of the reading
* **Temperature**: Stored in Celsius (°C)
* **Humidity**: The Relative Humidity (%RH) of the air
* **Barometric Pressure**: Stored as Hectopascals (hPa) / Millibars (mb)
* **Light Level**: Illuminance (Lux)
* **Battery Voltage**: ESP32 Battery Level (in volts)
* **ESP-NOW Strength**: ESP-NOW Signal Strength (dBm)
