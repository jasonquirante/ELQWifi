#include "web.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "lte.h"

namespace {
constexpr const char* kApSsid = "ELQWifi";
constexpr uint8_t kDnsPort = 53;
const IPAddress kApIp(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApNetmask(255, 255, 255, 0);

WebServer server(80);
DNSServer dnsServer;
bool dnsCaptiveActive = false;

bool isInternetReady() {
  const LteData lte = lteGetData();
  return lte.dataConnected && lte.ipAddress.length() > 0 && lte.ipAddress != "0.0.0.0";
}

bool shouldUseCaptivePortal() {
  return !isInternetReady();
}

String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    switch (ch) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

void sendPortalRedirect() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "Redirecting to captive portal...");
}

void sendStatusPage() {
  const LteData status = lteGetData();
  String html;
  html.reserve(2048);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>ELQWifi Status</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f4f7fb;color:#20232a;padding:20px;}h1{margin-top:0;} .card{background:#fff;border-radius:12px;box-shadow:0 10px 30px rgba(0,0,0,0.08);padding:20px;margin-bottom:20px;} dt{font-weight:700;} dd{margin:0 0 12px 0;color:#333;}</style></head><body>";
  html += "<div class=\"card\"><h1>ELQWifi Device Status</h1><p>Access point: <strong>" + String(kApSsid) + "</strong></p><p>WiFi IP: <strong>" + WiFi.softAPIP().toString() + "</strong></p></div>";
  html += "<div class=\"card\"><h2>SIM7600G LTE</h2><dl>";
  html += "<dt>Modem responsive</dt><dd>" + String(status.responsive ? "yes" : "no") + "</dd>";
  html += "<dt>SIM ready</dt><dd>" + String(status.simReady ? "yes" : "no") + "</dd>";
  html += "<dt>Data connected</dt><dd>" + String(status.dataConnected ? "yes" : "no") + "</dd>";
  html += "<dt>APN</dt><dd>" + escapeJson(status.apn) + "</dd>";
  html += "<dt>IP address</dt><dd>" + escapeJson(status.ipAddress) + "</dd>";
  html += "<dt>RSSI</dt><dd>" + String(status.rssi) + "</dd>";
  html += "<dt>CGATT</dt><dd>" + String(status.cgatt) + "</dd>";
  html += "</dl></div>";
  html += "<div class=\"card\"><p>Connect your client to this AP and browse to <strong>http://192.168.4.1/</strong>.</p></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleRoot() {
  sendStatusPage();
}

void handleStatus() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(256);
  payload += "{";
  payload += "\"responsive\":" + String(status.responsive ? "true" : "false");
  payload += ",\"simReady\":" + String(status.simReady ? "true" : "false");
  payload += ",\"dataConnected\":" + String(status.dataConnected ? "true" : "false");
  payload += ",\"rssi\":" + String(status.rssi);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"apn\":\"" + escapeJson(status.apn) + "\"";
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleCaptiveProbe() {
  if (shouldUseCaptivePortal()) {
    sendPortalRedirect();
    return;
  }

  const String uri = server.uri();
  if (uri == "/ncsi.txt") {
    server.send(200, "text/plain", "Microsoft NCSI");
    return;
  }

  server.send(204, "text/plain", "");
}

void handleNotFound() {
  if (shouldUseCaptivePortal()) {
    sendPortalRedirect();
    return;
  }

  if (server.uri() == "/status") {
    handleStatus();
    return;
  }

  sendStatusPage();
}
}  // namespace

void webInit() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kApIp, kApGateway, kApNetmask);
  WiFi.softAP(kApSsid);
  delay(200);

  Serial.print("[WEB] AP SSID: ");
  Serial.println(kApSsid);
  Serial.print("[WEB] AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/generate_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/gen_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/fwlink", HTTP_ANY, handleCaptiveProbe);
  server.onNotFound(handleNotFound);
  server.begin();

  dnsServer.start(kDnsPort, "*", kApIp);
  dnsCaptiveActive = true;
  Serial.println("[WEB] HTTP server started.");
  Serial.println("[WEB] Captive DNS started.");
}

void webLoop() {
  const bool captive = shouldUseCaptivePortal();
  if (captive && !dnsCaptiveActive) {
    dnsServer.start(kDnsPort, "*", kApIp);
    dnsCaptiveActive = true;
    Serial.println("[WEB] Captive DNS enabled.");
  } else if (!captive && dnsCaptiveActive) {
    dnsServer.stop();
    dnsCaptiveActive = false;
    Serial.println("[WEB] Captive DNS disabled.");
  }

  if (dnsCaptiveActive) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
}
