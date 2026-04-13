#include "web.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <SD.h>

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
bool portalSignedIn = false;
bool spiffsReady = false;

const char* kGpsLogPath = "/gps_log.csv";
const char* kSessionLogPath = "/sessions.log";

bool isInternetReady() {
  const LteData lte = lteGetData();
  return lte.dataConnected && lte.ipAddress.length() > 0 && lte.ipAddress != "0.0.0.0";
}

bool shouldUseCaptivePortal() {
  return !isInternetReady();
}

String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    switch (ch) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
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

String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".json")) return "application/json";
  return "text/plain";
}

bool tryServeFromSpiffs(const String& requestPath) {
  if (!spiffsReady) {
    return false;
  }

  String path = requestPath;
  if (path.length() == 0 || path == "/") {
    path = "/index.html";
  }

  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) {
      return false;
    }
    server.streamFile(file, getContentType(path));
    file.close();
    return true;
  }

  if (SPIFFS.exists(path + ".html")) {
    File file = SPIFFS.open(path + ".html", FILE_READ);
    if (!file) {
      return false;
    }
    server.streamFile(file, "text/html");
    file.close();
    return true;
  }

  return false;
}

bool tryServeFromSd(const String& requestPath) {
  String path = requestPath;
  if (path.length() == 0 || path == "/") {
    path = "/index.html";
  }

  const String roots[2] = {"/www", "/sd/www"};
  for (size_t i = 0; i < 2; ++i) {
    String sdPath = roots[i] + path;
    if (SD.exists(sdPath)) {
      File file = SD.open(sdPath, FILE_READ);
      if (!file) {
        continue;
      }
      server.streamFile(file, getContentType(path));
      file.close();
      return true;
    }

    sdPath = roots[i] + path + ".html";
    if (SD.exists(sdPath)) {
      File file = SD.open(sdPath, FILE_READ);
      if (!file) {
        continue;
      }
      server.streamFile(file, "text/html");
      file.close();
      return true;
    }
  }

  return false;
}

String readSdFileSnippet(const char* path, size_t maxLen) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    return "";
  }

  String content;
  content.reserve(maxLen + 1);
  while (file.available() && content.length() < maxLen) {
    content += static_cast<char>(file.read());
  }
  file.close();
  return content;
}

void sendJson(const String& payload) {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", payload);
}

void sendPortalRedirect() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "Redirecting to captive portal...");
}

void sendCaptiveLandingPage() {
  const LteData status = lteGetData();
  String html;
  html.reserve(3800);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>ELQWifi Portal</title>";
  html += "<style>body{margin:0;font-family:Verdana,sans-serif;background:linear-gradient(135deg,#d8efe8 0%,#f9f7ee 100%);color:#173231;}";
  html += ".wrap{max-width:900px;margin:0 auto;padding:20px;}";
  html += ".hero{background:#124734;color:#e8fff6;padding:20px;border-radius:14px;box-shadow:0 10px 25px rgba(18,71,52,.2);}";
  html += ".grid{display:grid;grid-template-columns:1fr;gap:14px;margin-top:14px;}@media(min-width:840px){.grid{grid-template-columns:1fr 1fr;}}";
  html += ".card{background:#fff;border-radius:12px;padding:16px;box-shadow:0 8px 18px rgba(0,0,0,.08);}dt{font-weight:700;margin-top:8px;}dd{margin:4px 0 8px;}";
  html += "input,button{font-size:14px;padding:10px;border-radius:8px;border:1px solid #b8cec4;}button{background:#1f6e50;color:#fff;border:none;cursor:pointer;}";
  html += "small{opacity:.85;}</style></head><body><div class=\"wrap\">";
  html += "<div class=\"hero\"><h1 style=\"margin:0 0 8px\">ELQWifi Captive Portal</h1>";
  html += "<div>SSID: <strong>" + String(kApSsid) + "</strong> | Gateway: <strong>" + WiFi.softAPIP().toString() + "</strong></div>";
  html += "<small>Open this page to monitor LTE status and update APN.</small></div>";
  html += "<div class=\"grid\">";
  html += "<div class=\"card\"><h2 style=\"margin-top:0\">LTE Status</h2><dl>";
  html += "<dt>Modem responsive</dt><dd>" + String(status.responsive ? "yes" : "no") + "</dd>";
  html += "<dt>SIM ready</dt><dd>" + String(status.simReady ? "yes" : "no") + "</dd>";
  html += "<dt>Data connected</dt><dd>" + String(status.dataConnected ? "yes" : "no") + "</dd>";
  html += "<dt>APN</dt><dd>" + htmlEscape(status.apn) + "</dd>";
  html += "<dt>IP</dt><dd>" + htmlEscape(status.ipAddress) + "</dd>";
  html += "<dt>RSSI</dt><dd>" + String(status.rssi) + "</dd>";
  html += "<dt>CGATT</dt><dd>" + String(status.cgatt) + "</dd>";
  html += "</dl></div>";
  html += "<div class=\"card\"><h2 style=\"margin-top:0\">APN Setup</h2>";
  html += "<form method=\"POST\" action=\"/apn\">";
  html += "<label for=\"apn\">APN</label><br><input id=\"apn\" name=\"apn\" value=\"" + htmlEscape(status.apn) + "\" style=\"width:100%;box-sizing:border-box\"><br><br>";
  html += "<button type=\"submit\">Save APN</button></form>";
  html += "<p><small>Common PH APNs: tm, internet, internet.globe.com.ph, mnet</small></p>";
  html += "<p><a href=\"/status\">JSON status</a></p></div></div></div></body></html>";
  server.send(200, "text/html", html);
}

void sendStatusPage() {
  if (tryServeFromSd("/index.html")) {
    return;
  }

  if (tryServeFromSpiffs("/index.html")) {
    return;
  }
  sendCaptiveLandingPage();
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
  sendJson(payload);
}

void handlePortalStatus() {
  String payload;
  payload.reserve(64);
  payload += "{\"ok\":true,\"signedIn\":";
  payload += (portalSignedIn || isInternetReady()) ? "true" : "false";
  payload += "}";
  sendJson(payload);
}

void handlePortalSignin() {
  portalSignedIn = true;
  sendJson("{\"ok\":true}");
}

void handleSmsInbox() {
  sendJson("{\"ok\":true,\"messages\":[]}");
}

void handleSmsSend() {
  sendJson("{\"ok\":false,\"error\":\"SMS send endpoint not enabled in this firmware build\"}");
}

void handleModemHealth() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(280);
  payload += "{\"ok\":true";
  payload += ",\"signalQuality\":\"";
  payload += (status.rssi >= -85) ? "Excellent" : (status.rssi >= -95) ? "Good" : (status.rssi >= -105) ? "Fair" : "Weak";
  payload += "\"";
  payload += ",\"rssi\":" + String(status.rssi);
  payload += ",\"creg\":" + String(status.creg);
  payload += ",\"cereg\":" + String(status.cereg);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"simReady\":" + String(status.simReady ? "true" : "false");
  payload += ",\"dataConnected\":" + String(status.dataConnected ? "true" : "false");
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  sendJson(payload);
}

void handleGps() {
  sendJson("{\"hasFix\":false,\"fixType\":0,\"satellites\":0,\"latitude\":0,\"longitude\":0,\"altitudeMeters\":0,\"speedKph\":0,\"raw\":\"GPS not enabled in this firmware build\"}");
}

void handleNetInfo() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(260);
  payload += "{\"dataConnected\":" + String(status.dataConnected ? "true" : "false");
  payload += ",\"rssi\":" + String(status.rssi);
  payload += ",\"simReady\":" + String(status.simReady ? "true" : "false");
  payload += ",\"creg\":" + String(status.creg);
  payload += ",\"cereg\":" + String(status.cereg);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += ",\"downloadMbps\":0";
  payload += ",\"uploadMbps\":0";
  payload += ",\"apn\":\"" + escapeJson(status.apn) + "\"";
  payload += ",\"ipAddress\":\"" + escapeJson(status.ipAddress) + "\"";
  payload += "}";
  sendJson(payload);
}

void handleLogs() {
  const String gpsLog = readSdFileSnippet(kGpsLogPath, 3000);
  const String sessionLog = readSdFileSnippet(kSessionLogPath, 3000);

  String payload;
  payload.reserve(6400);
  payload += "{\"gpsLog\":\"" + escapeJson(gpsLog) + "\"";
  payload += ",\"sessionLog\":\"" + escapeJson(sessionLog) + "\"";
  payload += "}";
  sendJson(payload);
}

void handleGpsTest() {
  const LteData status = lteGetData();
  String payload;
  payload.reserve(220);
  payload += "{\"ok\":true,\"response\":\"GPS RF test endpoint is informational in this build\"";
  payload += ",\"modemState\":{";
  payload += "\"rssi\":" + String(status.rssi);
  payload += ",\"creg\":" + String(status.creg);
  payload += ",\"cereg\":" + String(status.cereg);
  payload += ",\"cgatt\":" + String(status.cgatt);
  payload += "}}";
  sendJson(payload);
}

void handleCaptiveProbe() {
  if (shouldUseCaptivePortal()) {
    sendCaptiveLandingPage();
    return;
  }

  const String uri = server.uri();
  if (uri == "/ncsi.txt") {
    server.send(200, "text/plain", "Microsoft NCSI");
    return;
  }
  if (uri == "/connecttest.txt") {
    server.send(200, "text/plain", "Microsoft Connect Test");
    return;
  }
  if (uri == "/hotspot-detect.html") {
    server.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return;
  }
  if (uri == "/success.txt") {
    server.send(200, "text/plain", "success");
    return;
  }

  server.send(204, "text/plain", "");
}

void handleApnPost() {
  if (!server.hasArg("apn")) {
    server.send(400, "text/plain", "Missing apn field");
    return;
  }
  const String apn = server.arg("apn");
  lteSetApn(apn);
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "APN updated");
}

void handleNotFound() {
  if (tryServeFromSd(server.uri())) {
    return;
  }

  if (tryServeFromSpiffs(server.uri())) {
    return;
  }

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

  spiffsReady = SPIFFS.begin(true);
  if (spiffsReady) {
    Serial.println("[WEB] SPIFFS mounted (/data assets available after Upload File System Image).");
  } else {
    Serial.println("[WEB] SPIFFS mount failed, using fallback inline portal page.");
  }

  server.on("/", HTTP_ANY, handleRoot);
  server.on("/status", HTTP_ANY, handleStatus);
  server.on("/portal/status", HTTP_GET, handlePortalStatus);
  server.on("/portal/signin", HTTP_POST, handlePortalSignin);
  server.on("/sms/inbox", HTTP_GET, handleSmsInbox);
  server.on("/sms/send", HTTP_POST, handleSmsSend);
  server.on("/modem/health", HTTP_GET, handleModemHealth);
  server.on("/modem/gps-test", HTTP_POST, handleGpsTest);
  server.on("/gps", HTTP_GET, handleGps);
  server.on("/netinfo", HTTP_GET, handleNetInfo);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/apn", HTTP_POST, handleApnPost);
  server.on("/generate_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/gen_204", HTTP_ANY, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/success.txt", HTTP_ANY, handleCaptiveProbe);
  server.on("/connecttest.txt", HTTP_ANY, handleCaptiveProbe);
  server.on("/redirect", HTTP_ANY, handleCaptiveProbe);
  server.on("/canonical.html", HTTP_ANY, handleCaptiveProbe);
  server.on("/fwlink", HTTP_ANY, handleCaptiveProbe);
  server.on("/ncsi.txt", HTTP_ANY, handleCaptiveProbe);
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
