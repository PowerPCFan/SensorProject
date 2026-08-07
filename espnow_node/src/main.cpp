#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <time.h>

volatile int lastEspNowRssi = 0;

#if defined(DISCORD_WEBHOOK_URL) && defined(DISCORD_USER_ID) && defined(ALARM_THRESHOLD)
#define ENABLE_DISCORD_WEBHOOK 1
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define RUNNING_NEW_ESP_IDF_VERSION 1
#endif

const char* ntpTz = "EST5EDT,M3.2.0,M11.1.0";
const char* ntpServer = "pool.ntp.org";

#if defined(DEBUG_SERVER)
#include <WebServer.h>

WebServer debugServer(80);
bool serverStarted = false;
String logBuffer = "";
const size_t MAX_LOG_SIZE = 4096;
const size_t MAX_LOG_LINES = 60;

String htmlEscape(const String& str) {
    String escaped;
    escaped.reserve(str.length() + 32);
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        switch (c) {
            case '&':  escaped += "&amp;"; break;
            case '<':  escaped += "&lt;"; break;
            case '>':  escaped += "&gt;"; break;
            case '"':  escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default:   escaped += c; break;
        }
    }
    return escaped;
}

const char* TRUNC_HEADER = "...truncated\n";

void appendLogBuffer(const String& str) {
    logBuffer += str;

    bool wasTruncated = logBuffer.startsWith(TRUNC_HEADER);
    if (wasTruncated) {
        logBuffer = logBuffer.substring(13);
    }

    bool needsTruncation = false;

    int lineCount = 0;
    for (size_t i = 0; i < logBuffer.length(); i++) {
        if (logBuffer[i] == '\n') lineCount++;
    }
    while (lineCount > MAX_LOG_LINES) {
        int idx = logBuffer.indexOf('\n');
        if (idx >= 0) {
            logBuffer = logBuffer.substring(idx + 1);
            lineCount--;
            needsTruncation = true;
        } else {
            break;
        }
    }

    while (logBuffer.length() > MAX_LOG_SIZE) {
        int idx = logBuffer.indexOf('\n');
        if (idx >= 0) {
            logBuffer = logBuffer.substring(idx + 1);
            needsTruncation = true;
        } else {
            logBuffer = logBuffer.substring(logBuffer.length() - MAX_LOG_SIZE);
            needsTruncation = true;
            break;
        }
    }

    if (wasTruncated || needsTruncation) {
        logBuffer = String(TRUNC_HEADER) + logBuffer;
    }

    if (logBuffer.length() > MAX_LOG_SIZE) {
        logBuffer = logBuffer.substring(logBuffer.length() - MAX_LOG_SIZE);
    }
}
#endif

String getFormattedTimestamp() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) {
        time_t now = time(NULL);
        localtime_r(&now, &timeinfo);
    }

    char timeStr[32];
    char tzStr[8];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    strftime(tzStr, sizeof(tzStr), "%z", &timeinfo);

    char buf[64];
    if (strlen(tzStr) >= 5) {
        snprintf(buf, sizeof(buf), "[%s UTC%.3s:%s]", timeStr, tzStr, tzStr + 3);
    } else {
        snprintf(buf, sizeof(buf), "[%s UTC+00:00]", timeStr);
    }
    return String(buf);
}

void logMsg(const String &msg) {
    String ts = getFormattedTimestamp();
    String formattedLine = ts + " " + msg;
    Serial.println(formattedLine);
#if defined(DEBUG_SERVER)
    appendLogBuffer(formattedLine + "\n");
#endif
}

void logPrintf(const char *format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    String str = String(buf);
    if (str.length() == 0) return;

    size_t prefixLen = 0;
    while (prefixLen < str.length() && (str[prefixLen] == '\n' || str[prefixLen] == '\r')) {
        prefixLen++;
    }

    String leadingNewlines = str.substring(0, prefixLen);
    String body = str.substring(prefixLen);

    if (body.length() > 0) {
        String ts = getFormattedTimestamp();
        String formatted = leadingNewlines + ts + " " + body;
        Serial.print(formatted);
#if defined(DEBUG_SERVER)
        appendLogBuffer(formatted);
#endif
    } else {
        Serial.print(str);
#if defined(DEBUG_SERVER)
        appendLogBuffer(str);
#endif
    }
}

#if defined(DEBUG_SERVER)
const char PAGE_HEADER[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <link rel="icon" href="data:image/x-icon;base64,AAABAAEAEBAAAAEAIABoBAAAFgAAACgAAAAQAAAAIAAAAAEAIAAAAAAAQAQAAAAAAAAAAAAAAAAAAAAAAAD////////////////////////////////4+vv////////////////////////////////////////////////////////////////////////////l5uf/qLCz//H09v/////////////////////////////////////////////////////////////////X2Nn/YmRk/1tfYP+coKL/7vL0//////////////////////////////////////////////////n7+//Q1tf/cnV2/0xNTf8wMDD/V1la/5icnv/s8PH/////////////////////////////////7vHx/83R0v+ssbL/2Nzc/6Snp/87PDz/WFdW/3R0c/9maGn/l5ud/+ru7//+/v///////////////////////66xsf9DRUb/VlZW/5SWlv9LTEz/MDAw/zQzM/91dXX/Tk5O/1haWv+VmZv/5urs//7+/v/////////////////p6ur/X2Fi/zk4OP88PT3/KCcn/zM0NP8+Pj7/SUlJ/0RGR/8hICH/ZGdo/5OXmP/n7O3//v7+/////////////////+jo6P9oamv/VFRU/0JCQv8sLC3/LS0u/0ZGRv9RVFj/JCIi/1BSUv9cX1//j5OV/+Xr7P/+/v7/////////////////7e7u/3N2dv8oNTr/XH+X/0NHSP8aGBj/DwwM/yEiJP8+TFD/aHF1/1pdXv+MkZL/6e7v//7////////////////////w8fH/gpCW/0psgP8vMDL/EhAQ/zlCRv9UbXT/eI2U/4qbof9xfIH/Wl5f/4+Ulv/u8PL///////////////////////T09P+KjY7/SEtM/zxJTf9ogon/hJWZ/46bnf+MmZz/ipyh/3N/hP9bXl//t7i5////////////////////////////9vb2/5GUlP9QWlz/gY6S/4iUmP+Kl5n/i5ia/4yZm/+RoKT/Z2xv/9na2v/////////////////////////////////4+Pj/nqKi/1xiZP+EkZP/jJmb/4SRk/+CjpD/UldY/xoYGP+MjIz///////////////////////////////////////r6+v+nqqr/XmRl/11kZf9aXFz/LCws/xcUFP9qaWn/3t7e////////////////////////////////////////////+/v7/7a5uv+LjI3/LCkp/zo4OP+0s7P/+Pj4///////////////////////////////////////////////////////+/v7//v7+/9TU1P/m5ub//v7+////////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==" type="image/x-icon">
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Geist+Mono:ital,wght@0,100..900;1,100..900&family=Google+Sans+Flex:opsz,wght@6..144,1..1000&display=swap" rel="stylesheet">
    <title>ESP-NOW Node Logs</title>
    <style>
        body{font-family:'Google Sans Flex',system-ui,sans-serif;color:#ebebeb;padding:1rem;background:#1c1c1c;width:100%;max-width:1300px;padding:1rem;box-sizing:border-box;margin:0 auto;}
        h2{margin:0;font-size:1.25rem;font-weight:600;margin-bottom:0.5rem;}
        pre{background:#050505;color:#fff;padding:0.75rem;font-family:'Geist Mono',monospace;font-size:.875rem;line-height:1.5;white-space:pre-wrap;word-break:break-all;max-height:550px;overflow-y:auto;margin:0;margin-top:0.5rem;}
        .muted{color:#adadad;font-size:0.9rem;margin-block:0.25rem;}
    </style>
    <script>
        setTimeout(function(){
            window.location.reload();
        }, 30000);
    </script>
</head>
<body>
    <p class="muted">ESP-NOW Node Debug Server</p>
    <h2>Logs</h2>
    <p>Refreshes every 30 seconds. A maximum of 60 lines or 4096 bytes of logs are shown at a time.</p>
    <pre>)rawhtml";

const char PAGE_FOOTER[] PROGMEM = R"rawhtml(</pre>
</body>
</html>
)rawhtml";

String getLogHtml() {
    String html;
    html.reserve(sizeof(PAGE_HEADER) + logBuffer.length() * 2 + sizeof(PAGE_FOOTER));
    html += PAGE_HEADER;
    html += (logBuffer.length() == 0 ? "No logs recorded yet.\n" : htmlEscape(logBuffer));
    html += PAGE_FOOTER;
    return html;
}

void setupDebugServer() {
    if (!serverStarted) {
        debugServer.on("/", []() {
            debugServer.send(200, "text/html", getLogHtml());
        });
        debugServer.on("/raw", []() {
            debugServer.send(200, "text/plain", logBuffer.length() == 0 ? "No logs recorded yet.\n" : logBuffer);
        });
        debugServer.on("/favicon.ico", []() {
            // no need for favicon here since its base64 in the head
            debugServer.send(200, "image/x-icon", "data:,");
        });
        debugServer.onNotFound([]() {
            debugServer.send(404, "text/plain", "ESP-NOW Node Debug Server: 404 Page Not Found\n");
        });
        debugServer.begin();
        serverStarted = true;
        logMsg("[DEBUG_SERVER] Web server started on port 80 (http://" + WiFi.localIP().toString() + "/)");
    }
}
#endif

bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    const int maxAttempts = 10;
    const unsigned long attemptTimeoutMs = 15000;
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
            configTzTime(ntpTz, ntpServer);
            logMsg("[WIFI] Connected!");
            logMsg("- Local IP: " + WiFi.localIP().toString());
            logMsg("- Wi-Fi Station (STA) MAC Address: " + WiFi.macAddress());
            logMsg("- Wi-Fi RSSI: " + String(WiFi.RSSI()) + " dBm");
#if defined(DEBUG_SERVER)
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

String describeHttpError(int code, HTTPClient& http) {
    if (code >= 0) {
        return "HTTP status " + String(code);
    } else {
        String err = http.errorToString(code);
        return err.length() > 0 ? err : "unknown error";
    }
}

#if defined(ENABLE_DISCORD_WEBHOOK)
unsigned long lastUploadTime = 0;
bool alarmTriggered = false;

struct DiscordQueueItem {
    char message[256];
    bool ping;
};

QueueHandle_t discordQueue = nullptr;
TaskHandle_t discordWorkerTask = nullptr;

void sendDiscordWebhook(const String& messageText, bool pingUser);

void sendDiscordWebhookTask(void* param) {
    (void)param;
    DiscordQueueItem item;

    while (true) {
        if (xQueueReceive(discordQueue, &item, portMAX_DELAY) == pdTRUE) {
            sendDiscordWebhook(String(item.message), item.ping);
        }
    }
}

void queueDiscordWebhook(const String& messageText, bool pingUser) {
    if (!discordQueue) {
        discordQueue = xQueueCreate(8, sizeof(DiscordQueueItem));
    }

    if (!discordQueue) {
        logMsg("[DISCORD] Failed to initialize webhook queue; dropping notification");
        return;
    }

    DiscordQueueItem item;
    memset(&item, 0, sizeof(item));
    strncpy(item.message, messageText.c_str(), sizeof(item.message) - 1);
    item.ping = pingUser;

    if (xQueueSend(discordQueue, &item, 0) != pdTRUE) {
        logMsg("[DISCORD] Webhook queue full; dropping notification");
        return;
    }

    if (!discordWorkerTask) {
        xTaskCreatePinnedToCore(sendDiscordWebhookTask, "discord-webhook", 4096, nullptr, 1, &discordWorkerTask, 1);
    }
}

void sendDiscordWebhook(const String& messageText, bool pingUser = false) {
    if (WiFi.status() != WL_CONNECTED) {
        logMsg("[DISCORD] Wi-Fi unavailable; skipping webhook notification");
        return;
    }

    String fullContent = "";
    if (pingUser) {
        fullContent += "<@" + String(DISCORD_USER_ID) + "> ";
    }
    fullContent += messageText;

#if defined(DEBUG_SERVER)
    fullContent += "\nView debug logs at: http://" + WiFi.localIP().toString() + "/";
#endif

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (http.begin(client, DISCORD_WEBHOOK_URL)) {
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(5000);
        http.setConnectTimeout(5000);

        JsonDocument doc;
        doc["content"] = fullContent;
        String payload;
        serializeJson(doc, payload);

        logMsg("[DISCORD] Sending webhook notification...");
        int httpCode = http.POST(payload);
        if (httpCode > 0) {
            logPrintf("[DISCORD] Webhook sent successfully (Status %d)\n", httpCode);
        } else {
            logPrintf("[DISCORD] Webhook send failed (Code %d): %s\n", httpCode, describeHttpError(httpCode, http).c_str());
        }
        http.end();
    } else {
        logMsg("[DISCORD] Failed to initialize connection to webhook URL");
    }
}
#endif

// Structure to receive data
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
volatile bool hasDataToSend = false;

#ifdef RUNNING_NEW_ESP_IDF_VERSION
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
    logPrintf("\n");

    if (recv_info && recv_info->rx_ctrl) {
        lastEspNowRssi = recv_info->rx_ctrl->rssi;
    }
    char macStr[18] = "UNKNOWN";
    if (recv_info && recv_info->src_addr) {
        snprintf(
            macStr,
            sizeof(macStr),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            recv_info->src_addr[0],
            recv_info->src_addr[1],
            recv_info->src_addr[2],
            recv_info->src_addr[3],
            recv_info->src_addr[4],
            recv_info->src_addr[5]
        );
    }
    if (len == sizeof(myData)) {
        memcpy(&myData, incomingData, sizeof(myData));
        logPrintf("[ESP-NOW] Sender MAC: %s | Payload Size: %d bytes | Sender-to-Node RSSI: %d dBm\n", macStr, len, lastEspNowRssi);
        logPrintf(
            "[ESP-NOW] Data -> Temp: %.1f C, Humidity: %.1f%%, Pressure: %.1f hPa, ESP32 Temp: %.1f C, Battery Voltage: %.2f V\n",
            myData.temperature,
            myData.humidity,
            myData.pressure,
            myData.esp32_temperature,
            myData.esp32_voltage
        );
        hasDataToSend = true;
    } else {
        logPrintf("[ESP-NOW] Warning: Received packet from %s of unexpected size (%d bytes)\n", macStr, len);
    }
}
#else
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    char macStr[18] = "UNKNOWN";
    if (mac) {
        snprintf(
            macStr,
            sizeof(macStr),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2],
            mac[3], mac[4], mac[5]
        );
    }
    if (len == sizeof(myData)) {
        memcpy(&myData, incomingData, sizeof(myData));
        logPrintf("[ESP-NOW] Sender MAC: %s | Bytes: %d\n", macStr, len);
        logPrintf(
            "[ESP-NOW] Data -> Temp: %.1f C, Humidity: %.1f%%, Pressure: %.1f hPa, ESP32 Temp: %.1f C, Battery Voltage: %.2f V\n",
            myData.temperature,
            myData.humidity,
            myData.pressure,
            myData.esp32_temperature,
            myData.esp32_voltage
        );
        hasDataToSend = true;
    } else {
        logPrintf("[ESP-NOW] Warning: Received packet from %s of unexpected size (%d bytes)\n", macStr, len);
    }
}
#endif

void setup() {
    Serial.begin(115200);
    delay(2000); 
    logMsg("[SETUP] Starting ESP-NOW node setup...");
    logMsg(String("[ESP-IDF] Version: ") + esp_get_idf_version());

    WiFi.mode(WIFI_MODE_STA);

    if (!connectWiFi()) {
        logMsg("[SETUP] Initial Wi-Fi connection failed after 10 attempts.");
    }

#if defined(DEBUG_SERVER)
    setupDebugServer();
#endif

#if defined(ENABLE_DISCORD_WEBHOOK)
    if (!discordQueue) {
        discordQueue = xQueueCreate(8, sizeof(DiscordQueueItem));
    }
    if (discordQueue && !discordWorkerTask) {
        xTaskCreatePinnedToCore(sendDiscordWebhookTask, "discord-webhook", 4096, nullptr, 1, &discordWorkerTask, 1);
    }

    lastUploadTime = millis();
    alarmTriggered = false;
    logMsg("[DISCORD] Webhook alarm feature enabled (Threshold: " + String(ALARM_THRESHOLD) + "s)");
#else
    logMsg("[DISCORD] Webhook alarm feature disabled (missing build flags).");
#endif

    if (esp_now_init() != ESP_OK) {
        logMsg("[ESP-NOW] Error initializing ESP-NOW");
        return;
    }

    esp_now_register_recv_cb(OnDataRecv);
    logMsg("[SETUP] Setup complete. Waiting for incoming data...");
}

unsigned long lastPostTime = 0;
const unsigned long msgCooldownMs = 5000; // 5 second cooldown to prevent duplicates

void loop() {
#if defined(DEBUG_SERVER)
    if (WiFi.status() == WL_CONNECTED) {
        debugServer.handleClient();
    }
#endif

#if defined(ENABLE_DISCORD_WEBHOOK)
    if (!alarmTriggered && (millis() - lastUploadTime >= (unsigned long)ALARM_THRESHOLD * 1000UL)) {
        alarmTriggered = true;
        logMsg("[ALARM] Threshold elapsed without updates! Sending Discord webhook notification...");

        String alertText = "ESP-NOW Node: " + String(ALARM_THRESHOLD) + " seconds elapsed without an update!";
        queueDiscordWebhook(alertText, true); // true = include ping
    }
#endif

    if (hasDataToSend) {
        hasDataToSend = false;

        // Prevent duplicate DB entries from overlapping radio channels
        unsigned long elapsed = millis() - lastPostTime;
        if (elapsed < msgCooldownMs) {
            logPrintf("[LOOP] Cooldown active (%lu ms remaining), skipping duplicate payload\n", msgCooldownMs - elapsed);
            return; 
        }
        lastPostTime = millis();

        if (WiFi.status() != WL_CONNECTED) {
            logMsg("[LOOP] Wi-Fi disconnected. Attempting to reconnect...");
            connectWiFi();
        }

        if (WiFi.status() == WL_CONNECTED) {
            int routerRssi = WiFi.RSSI();
            logPrintf("[SIGNAL] Sensor-to-Node (ESP-NOW) RSSI: %d dBm | Node-to-Router (Wi-Fi) RSSI: %d dBm\n", lastEspNowRssi, routerRssi);

            JsonDocument doc;
            doc["temperature"] = myData.temperature;
            doc["humidity"] = myData.humidity;
            doc["pressure"] = myData.pressure;
            if (!isnan(myData.internal_temperature) && !isnan(myData.internal_humidity)) {
                doc["internal_temperature"] = myData.internal_temperature;
                doc["internal_humidity"] = myData.internal_humidity;
            }
            doc["esp32_temperature"] = myData.esp32_temperature;
            doc["esp32_voltage"] = myData.esp32_voltage;
#ifdef RUNNING_NEW_ESP_IDF_VERSION
            doc["wifi_strength"] = lastEspNowRssi;
#endif

            String payload;
            serializeJson(doc, payload);

            logMsg("[HTTP] Posting payload to " + String(SERVER_URL));
            logMsg("[HTTP] Payload: " + payload);

            HTTPClient http;
            http.begin(SERVER_URL);
            http.addHeader("Content-Type", "application/json");
            http.addHeader("X-API-Key", API_KEY);
            http.setTimeout(10000);
            http.setConnectTimeout(10000);
            int code = http.POST(payload);

            if (code >= 200 && code < 300) {
                String response = http.getString();
                if (response.length() > 0) {
                    logPrintf("[HTTP] Success (Status %d) | Response: %s\n", code, response.c_str());
                } else {
                    logPrintf("[HTTP] Success (Status %d)\n", code);
                }
#if defined(ENABLE_DISCORD_WEBHOOK)
                if (alarmTriggered) {
                    logMsg("[ALARM] Data updates resumed! Sending Discord recovery notification...");
                    queueDiscordWebhook("ESP-NOW Node: Data updates have resumed!", false);
                }
                lastUploadTime = millis();
                alarmTriggered = false;
#endif
            } else {
                logPrintf("[HTTP] Error (Code %d): %s\n", code, describeHttpError(code, http).c_str());
#if defined(ENABLE_DISCORD_WEBHOOK)
                String errorText = "ESP-NOW Node: HTTP upload failed (" + describeHttpError(code, http) + ")";
                queueDiscordWebhook(errorText, true);
#endif
            }
            http.end();
        } else {
            logMsg("[LOOP] Wi-Fi reconnection failed, skipping upload.");
        }
    }
}
