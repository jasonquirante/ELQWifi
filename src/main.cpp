#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>

#include "lte.h"
#include "web.h"

#define SD_CS 5

bool sdCardAvailable = false;
const char kBootLogPath[] = "/boot.log";

static void appendSdLog(const String& message) {
  if (!sdCardAvailable) {
    return;
  }

  File file = SD.open(kBootLogPath, FILE_APPEND);
  if (!file) {
    return;
  }

  file.println(message);
  file.close();
}

static void logStatus() {
  const LteData status = lteGetData();
  char buffer[200];
  snprintf(buffer, sizeof(buffer), "LTE status responsive=%s connected=%s ip=%s rssi=%d cgatt=%d",
           status.responsive ? "yes" : "no",
           status.dataConnected ? "yes" : "no",
           status.ipAddress.c_str(),
           status.rssi,
           status.cgatt);

  Serial.println(buffer);
  appendSdLog(buffer);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[ELQWifi] Booting...");

  if (SD.begin(SD_CS, SPI, 25000000U)) {
    sdCardAvailable = true;
    Serial.println("[ELQWifi] microSD card mounted.");
    File file = SD.open(kBootLogPath, FILE_WRITE);
    if (file) {
      file.println("ELQWifi boot " + String(millis()));
      file.close();
    }
  } else {
    Serial.println("[ELQWifi] microSD card mount failed.");
  }

  lteInit();
  webInit();

  appendSdLog("Boot complete.");
}

void loop() {
  lteLoop();
  webLoop();

  static unsigned long lastStatusMs = 0;
  if (millis() - lastStatusMs >= 10000) {
    lastStatusMs = millis();
    logStatus();
  }

  delay(10);
}
