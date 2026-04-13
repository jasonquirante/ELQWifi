#include "lte.h"

#include <Arduino.h>
#include <WiFi.h>

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_defaults.h>
#include <esp_netif_ppp.h>

#include <lwip/lwip_napt.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

constexpr int LTE_UART_RX_PIN = 17;
constexpr int LTE_UART_TX_PIN = 16;
constexpr int LTE_PWRKEY_PIN = 4;
constexpr uint32_t LTE_BAUD = 115200;
constexpr uint32_t LTE_BAUD_FALLBACKS[] = {115200, 9600, 57600, 38400};
constexpr uint32_t LTE_PPP_UART_BAUD = 115200;
constexpr uint32_t LTE_POWKEY_LOW_MS = 1800;
constexpr uint32_t LTE_BOOT_WAIT_MS = 20000;
constexpr uint32_t LTE_GATEWAY_RETRY_MS = 15000;
constexpr int LTE_MAX_PROBE_FAILURES_BEFORE_RESET = 6;
constexpr uint32_t LTE_ATTACH_TIMEOUT_MS = 120000;
constexpr uint32_t LTE_PPP_CONNECT_TIMEOUT_MS = 25000;
constexpr uint32_t LTE_PPP_TX_WRITE_TIMEOUT_MS = 250;
constexpr uint32_t LTE_PASSIVE_PROBE_WINDOW_MS = 25000;
constexpr uint32_t LTE_PASSIVE_PROBE_STEP_MS = 3000;

// TM Philippines APN configuration.
String configuredApn = "tm";
String configuredApnUser = "";
String configuredApnPass = "";
constexpr int LTE_PDP_AUTH_PAP = 1;

HardwareSerial lteSerial(2);
uint32_t activeLteBaud = LTE_BAUD;
int activeUartRxPin = LTE_UART_RX_PIN;
int activeUartTxPin = LTE_UART_TX_PIN;
bool activeUartInvert = false;
unsigned long lastProbeMs = 0;
bool statusDumpDone = false;
unsigned long lastGatewayAttemptMs = 0;
int consecutiveProbeFailures = 0;
int consecutiveAttachTimeouts = 0;
bool dataModeActive = false;
bool pppNetifCreated = false;
TaskHandle_t pppRxTaskHandle = nullptr;
esp_netif_t* pppNetif = nullptr;
bool pppHasIp = false;
volatile uint32_t pppRxBytes = 0;
volatile uint32_t pppTxBytes = 0;
bool pppAuthFallbackEnabled = false;
unsigned long dataModeEnteredMs = 0;
constexpr uint32_t LTE_PPP_BRINGUP_TIMEOUT_MS = 70000;
LteData currentLteData = {false, false, false, -1, -1, -1, -1, -1, "", ""};

int findTrailingInt(const String& line) {
  for (int i = line.length() - 1; i >= 0; --i) {
    if (isDigit(line[i])) {
      int end = i;
      while (i >= 0 && isDigit(line[i])) {
        --i;
      }
      return line.substring(i + 1, end + 1).toInt();
    }
  }
  return -1;
}

void updateStatusFromResponse(const char* cmd, const String& response) {
  if (strcmp(cmd, "AT+CPIN?") == 0) {
    currentLteData.simReady = response.indexOf("READY") != -1;
  } else if (strcmp(cmd, "AT+CSQ") == 0) {
    const int start = response.indexOf("+CSQ:");
    if (start != -1) {
      int comma = response.indexOf(',', start);
      int colon = response.indexOf(':', start);
      if (colon != -1 && comma != -1 && comma > colon) {
        currentLteData.rssi = response.substring(colon + 1, comma).toInt();
        currentLteData.ber = findTrailingInt(response.substring(comma + 1));
      }
    }
  } else if (strcmp(cmd, "AT+CREG?") == 0) {
    const int start = response.indexOf("+CREG:");
    if (start != -1) {
      currentLteData.creg = findTrailingInt(response.substring(start));
    }
  } else if (strcmp(cmd, "AT+CEREG?") == 0) {
    const int start = response.indexOf("+CEREG:");
    if (start != -1) {
      currentLteData.cereg = findTrailingInt(response.substring(start));
    }
  } else if (strcmp(cmd, "AT+CGATT?") == 0) {
    const int start = response.indexOf("+CGATT:");
    if (start != -1) {
      currentLteData.cgatt = findTrailingInt(response.substring(start));
    }
  }
}

esp_err_t pppTransmit(void* context, void* data, size_t len) {
  (void)context;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  size_t totalWritten = 0;
  const unsigned long startMs = millis();

  while (totalWritten < len && (millis() - startMs) < LTE_PPP_TX_WRITE_TIMEOUT_MS) {
    const size_t writtenNow = lteSerial.write(bytes + totalWritten, len - totalWritten);
    if (writtenNow > 0) {
      totalWritten += writtenNow;
      continue;
    }
    delay(1);
  }

  if (totalWritten > 0) {
    pppTxBytes += static_cast<uint32_t>(totalWritten);
  }
  return (totalWritten == len) ? ESP_OK : ESP_FAIL;
}

void pppRxTask(void* parameter) {
  (void)parameter;
  uint8_t buffer[256];

  while (dataModeActive) {
    const int availableBytes = lteSerial.available();
    if (availableBytes > 0) {
      const size_t chunk = static_cast<size_t>(min(availableBytes, static_cast<int>(sizeof(buffer))));
      const size_t readCount = lteSerial.readBytes(reinterpret_cast<char*>(buffer), chunk);
      if (readCount > 0 && pppNetif != nullptr) {
        pppRxBytes += static_cast<uint32_t>(readCount);
        esp_netif_receive(pppNetif, buffer, readCount, nullptr);
      }
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  pppRxTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void enableApNat() {
#if IP_NAPT
  // Get current softAP IP
  IPAddress apIp = WiFi.softAPIP();
  
  // Verify IP is valid (not 0.0.0.0)
  if (apIp[0] == 0 && apIp[1] == 0 && apIp[2] == 0 && apIp[3] == 0) {
    Serial.println("[LTE] softAP IP not ready (0.0.0.0); NAT enable deferred.");
    return;
  }
  
  // Enable NAT to allow AP clients to reach the PPP interface
  ip_napt_enable(static_cast<uint32_t>(apIp), 1);
  Serial.print("[LTE] NAT enabled on softAP IP ");
  Serial.println(apIp);
#else
  Serial.println("[LTE] NAPT support disabled in lwIP config.");
#endif
}

void resetPppSessionState(const char* reason) {
  if (reason != nullptr && reason[0] != '\0') {
    Serial.print("[LTE] Resetting PPP session state: ");
    Serial.println(reason);
  }

  const bool wasDataMode = dataModeActive;
  pppHasIp = false;
  currentLteData.dataConnected = false;
  currentLteData.ipAddress = "";
  dataModeActive = false;
  pppRxBytes = 0;
  pppTxBytes = 0;
  dataModeEnteredMs = 0;

  // Try to return modem to AT command mode after PPP data session teardown.
  if (wasDataMode) {
    delay(1200);
    lteSerial.print("+++");
    delay(1200);
    lteSerial.print("ATH\r\n");
    delay(300);
  }

  if (pppNetif != nullptr) {
    esp_netif_action_disconnected(pppNetif, nullptr, 0, nullptr);
    esp_netif_action_stop(pppNetif, nullptr, 0, nullptr);
  }
  lastGatewayAttemptMs = 0;
}

void onPppGotIp(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  (void)base;
  if (id != IP_EVENT_PPP_GOT_IP) {
    return;
  }

  auto* event = static_cast<ip_event_got_ip_t*>(data);
  if (event == nullptr || event->esp_netif != pppNetif) {
    return;
  }

  pppHasIp = true;
  currentLteData.dataConnected = true;
  currentLteData.ipAddress = IPAddress(event->ip_info.ip.addr).toString();
  Serial.print("[LTE] PPP got IP: ");
  Serial.println(currentLteData.ipAddress);
  enableApNat();
}

void onPppLostIp(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  (void)base;
  (void)data;
  if (id != IP_EVENT_PPP_LOST_IP) {
    return;
  }

  pppHasIp = false;
  currentLteData.dataConnected = false;
  currentLteData.ipAddress = "";
  dataModeActive = false;
  Serial.print("[LTE] PPP byte stats before loss RX=");
  Serial.print(pppRxBytes);
  Serial.print(" TX=");
  Serial.println(pppTxBytes);
  resetPppSessionState("IP_EVENT_PPP_LOST_IP");
  Serial.println("[LTE] PPP lost IP.");
}

const char* pppStatusName(int32_t id) {
  switch (id) {
    case NETIF_PPP_ERRORNONE: return "ERRORNONE";
    case NETIF_PPP_ERRORPARAM: return "ERRORPARAM";
    case NETIF_PPP_ERROROPEN: return "ERROROPEN";
    case NETIF_PPP_ERRORDEVICE: return "ERRORDEVICE";
    case NETIF_PPP_ERRORALLOC: return "ERRORALLOC";
    case NETIF_PPP_ERRORUSER: return "ERRORUSER";
    case NETIF_PPP_ERRORCONNECT: return "ERRORCONNECT";
    case NETIF_PPP_ERRORAUTHFAIL: return "ERRORAUTHFAIL";
    case NETIF_PPP_ERRORPROTOCOL: return "ERRORPROTOCOL";
    case NETIF_PPP_ERRORPEERDEAD: return "ERRORPEERDEAD";
    case NETIF_PPP_ERRORIDLETIMEOUT: return "ERRORIDLETIMEOUT";
    case NETIF_PPP_ERRORCONNECTTIME: return "ERRORCONNECTTIME";
    case NETIF_PPP_ERRORLOOPBACK: return "ERRORLOOPBACK";
    case NETIF_PPP_PHASE_DEAD: return "PHASE_DEAD";
    case NETIF_PPP_PHASE_MASTER: return "PHASE_MASTER";
    case NETIF_PPP_PHASE_HOLDOFF: return "PHASE_HOLDOFF";
    case NETIF_PPP_PHASE_INITIALIZE: return "PHASE_INITIALIZE";
    case NETIF_PPP_PHASE_SERIALCONN: return "PHASE_SERIALCONN";
    case NETIF_PPP_PHASE_DORMANT: return "PHASE_DORMANT";
    case NETIF_PPP_PHASE_ESTABLISH: return "PHASE_ESTABLISH";
    case NETIF_PPP_PHASE_AUTHENTICATE: return "PHASE_AUTHENTICATE";
    case NETIF_PPP_PHASE_CALLBACK: return "PHASE_CALLBACK";
    case NETIF_PPP_PHASE_NETWORK: return "PHASE_NETWORK";
    case NETIF_PPP_PHASE_RUNNING: return "PHASE_RUNNING";
    case NETIF_PPP_PHASE_TERMINATE: return "PHASE_TERMINATE";
    case NETIF_PPP_PHASE_DISCONNECT: return "PHASE_DISCONNECT";
    case NETIF_PPP_CONNECT_FAILED: return "CONNECT_FAILED";
    default: return "UNKNOWN";
  }
}

void onPppStatus(void* arg, esp_event_base_t base, int32_t id, void* data) {
  (void)arg;
  (void)base;
  (void)data;

  Serial.print("[LTE] PPP status event: ");
  Serial.print(pppStatusName(id));
  Serial.print(" (");
  Serial.print(id);
  Serial.println(")");

  if (id == NETIF_PPP_ERRORAUTHFAIL) {
    pppAuthFallbackEnabled = true;
    Serial.println("[LTE] PPP auth failed; enabling blank credentials fallback.");
    resetPppSessionState("PPP auth failure");
    return;
  }

  // Only hard-reset PPP on actual error events. Phase transitions like TERMINATE/DISCONNECT
  // may occur transiently during bring-up and should not be force-closed from here.
  if (id == NETIF_PPP_ERRORCONNECT || id == NETIF_PPP_ERRORPROTOCOL ||
      id == NETIF_PPP_ERRORPEERDEAD || id == NETIF_PPP_ERRORIDLETIMEOUT ||
      id == NETIF_PPP_ERRORCONNECTTIME || id == NETIF_PPP_CONNECT_FAILED) {
    resetPppSessionState("PPP status failure/terminate event");
  }
}

bool sendCommandExpectToken(const char* cmd, const char* token, String& response, uint32_t timeoutMs) {
  while (lteSerial.available()) {
    lteSerial.read();
  }

  lteSerial.print(cmd);
  lteSerial.print("\r\n");

  const unsigned long startMs = millis();
  response = "";

  while (millis() - startMs < timeoutMs) {
    while (lteSerial.available()) {
      response += static_cast<char>(lteSerial.read());
    }

    if (response.indexOf(token) != -1) {
      return true;
    }
    if (response.indexOf("ERROR") != -1 || response.indexOf("NO CARRIER") != -1 ||
        response.indexOf("CME ERROR") != -1 || response.indexOf("FAIL") != -1) {
      return false;
    }
    delay(10);
  }

  return false;
}

bool recoverModemAndProbe();

bool lteSendCommand(const char* cmd, String& response, uint32_t timeoutMs) {
  if (dataModeActive) {
    response = "";
    return false;
  }

  while (lteSerial.available()) {
    lteSerial.read();
  }

  Serial.print("[LTE] TX: ");
  Serial.println(cmd);
  lteSerial.print(cmd);
  lteSerial.print("\r\n");

  const unsigned long startMs = millis();
  response = "";

  while (millis() - startMs < timeoutMs) {
    while (lteSerial.available()) {
      const uint8_t byte = lteSerial.read();
      if (byte == 0x00) {
        continue;
      }
      response += static_cast<char>(byte);
    }

    if (response.indexOf("OK") != -1) {
      updateStatusFromResponse(cmd, response);
      return true;
    }
    if (response.indexOf("ERROR") != -1 || response.indexOf("+CME ERROR") != -1) {
      updateStatusFromResponse(cmd, response);
      return false;
    }
    delay(10);
  }

  updateStatusFromResponse(cmd, response);
  return false;
}

bool switchUartBaud(uint32_t baud) {
  if (baud == activeLteBaud) {
    return true;
  }

  lteSerial.flush();
  lteSerial.begin(baud, SERIAL_8N1, activeUartRxPin, activeUartTxPin, activeUartInvert);
  delay(200);
  activeLteBaud = baud;
  Serial.print("[LTE] UART switched to ");
  Serial.println(activeLteBaud);
  return true;
}

bool syncModemUartForPpp() {
  if (activeLteBaud == LTE_PPP_UART_BAUD) {
    return true;
  }

  const uint32_t previousBaud = activeLteBaud;
  String response;
  const String iprCmd = String("AT+IPR=") + LTE_PPP_UART_BAUD;
  if (!lteSendCommand(iprCmd.c_str(), response, 1500)) {
    Serial.println("[LTE] AT+IPR failed; keeping current UART baud.");
    return false;
  }

  switchUartBaud(LTE_PPP_UART_BAUD);
  if (!lteSendCommand("AT", response, 1200)) {
    Serial.println("[LTE] Modem did not answer after UART baud switch.");
    switchUartBaud(previousBaud);
    Serial.println("[LTE] Reverted UART baud to previous working speed.");
    return false;
  }

  Serial.println("[LTE] Modem UART synchronized for PPP traffic.");
  return true;
}

bool hasPdpIpv4Address(const String& response) {
  const int tag = response.indexOf("+CGPADDR:");
  if (tag == -1) {
    return false;
  }

  // SIM7600 may return either:
  // +CGPADDR: 1,"100.106.51.93"  (quoted)
  // +CGPADDR: 1,100.106.51.93      (unquoted)
  const int comma = response.indexOf(',', tag);
  if (comma == -1) {
    return false;
  }

  int start = comma + 1;
  while (start < response.length() && (response[start] == ' ' || response[start] == '"')) {
    ++start;
  }

  int end = start;
  while (end < response.length()) {
    const char c = response[end];
    if (c == '"' || c == '\r' || c == '\n' || c == ',') {
      break;
    }
    ++end;
  }

  const String ip = response.substring(start, end);
  return ip.length() > 0 && ip != "0.0.0.0";
}

bool activateAndVerifyPdpContext() {
  String response;
  // Deactivate any existing PDP context
  (void)lteSendCommand("AT+CGACT=0,1", response, 3000);
  delay(500);

  // Activate PDP context for PPP dial
  if (!lteSendCommand("AT+CGACT=1,1", response, 6000)) {
    Serial.println("[LTE] CGACT activation failed.");
    return false;
  }

  // Confirm modem actually has a bearer IP before PPP dial.
  for (int attempt = 0; attempt < 6; ++attempt) {
    delay(1000);
    if (lteSendCommand("AT+CGPADDR=1", response, 2500) && hasPdpIpv4Address(response)) {
      Serial.println("[LTE] PDP context has valid IPv4 address.");
      return true;
    }
  }

  Serial.println("[LTE] PDP context has no IPv4 address.");
  return false;
}

bool configureModemForUartPpp() {
  String response;
  if (!lteSendCommand("AT+IFC=0,0", response, 2000)) {
    Serial.println("[LTE] Warning: failed to disable UART flow control.");
  }
  if (!lteSendCommand("AT&D0", response, 2000)) {
    Serial.println("[LTE] Warning: failed to set DTR ignore mode.");
  }
  return true;
}

bool tryDialCommands() {
  String response;
  // Try the most common TM Philippines PPP dial sequence first
  const char* connectCommands[] = {"ATD*99***1#", "ATD*99#", "AT+CGDATA=\"PPP\",1"};
  
  for (const char* cmd : connectCommands) {
    Serial.print("[LTE] Attempting PPP dial: ");
    Serial.println(cmd);
    delay(500);
    
    if (sendCommandExpectToken(cmd, "CONNECT", response, LTE_PPP_CONNECT_TIMEOUT_MS)) {
      Serial.print("[LTE] PPP data mode entered with ");
      Serial.println(cmd);
      return true;
    }
    
    Serial.print("[LTE] PPP dial attempt failed: ");
    Serial.println(cmd);
    delay(1000);
  }
  return false;
}

bool isRegisteredToNetwork() {
  const bool csRegistered = (currentLteData.creg == 1 || currentLteData.creg == 5);
  const bool psRegistered = (currentLteData.cereg == 1 || currentLteData.cereg == 5);
  return csRegistered || psRegistered;
}

bool ensurePacketServiceReady() {
  String response;
  (void)lteSendCommand("AT+CFUN=1", response, 2000);
  (void)lteSendCommand("AT+CNMP=2", response, 2000);   // Automatic RAT selection.
  (void)lteSendCommand("AT+CMNB=3", response, 2000);   // Prefer LTE with fallback.
  (void)lteSendCommand("AT+COPS=0", response, 4000);   // Automatic operator selection.

  const unsigned long startMs = millis();
  while (millis() - startMs < LTE_ATTACH_TIMEOUT_MS) {
    (void)lteSendCommand("AT+CSQ", response, 1500);
    updateStatusFromResponse("AT+CSQ", response);
    (void)lteSendCommand("AT+CREG?", response, 1500);
    updateStatusFromResponse("AT+CREG?", response);
    (void)lteSendCommand("AT+CEREG?", response, 1500);
    updateStatusFromResponse("AT+CEREG?", response);
    (void)lteSendCommand("AT+CGATT?", response, 1500);
    updateStatusFromResponse("AT+CGATT?", response);

    if (isRegisteredToNetwork() && currentLteData.cgatt == 1) {
      consecutiveAttachTimeouts = 0;
      Serial.println("[LTE] Packet service ready.");
      return true;
    }

    if (isRegisteredToNetwork() && currentLteData.cgatt != 1) {
      Serial.println("[LTE] Registered but not attached; sending AT+CGATT=1...");
      (void)lteSendCommand("AT+CGATT=1", response, 5000);
    }

    Serial.print("[LTE] Waiting for attach CREG=");
    Serial.print(currentLteData.creg);
    Serial.print(" CEREG=");
    Serial.print(currentLteData.cereg);
    Serial.print(" CGATT=");
    Serial.print(currentLteData.cgatt);
    Serial.print(" CSQ=");
    Serial.println(currentLteData.rssi);
    delay(2500);
  }

  ++consecutiveAttachTimeouts;
  Serial.println("[LTE] Timed out waiting for packet service attach.");
  Serial.println("[LTE] Keeping modem online to continue network search; no hard reset this cycle.");

  if (consecutiveAttachTimeouts >= 4) {
    consecutiveAttachTimeouts = 0;
    (void)recoverModemAndProbe();
  }

  return false;
}

void tryReturnToCommandMode() {
  delay(1200);
  lteSerial.print("+++");
  delay(1200);

  String response;
  (void)lteSendCommand("ATH", response, 1200);
  (void)lteSendCommand("AT", response, 1200);
}

bool ensurePppNetif() {
  if (pppNetifCreated) {
    return pppNetif != nullptr;
  }

  (void)esp_netif_init();
  (void)esp_event_loop_create_default();

  esp_netif_inherent_config_t baseCfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
  baseCfg.if_desc = "ppp0";

  static esp_netif_driver_ifconfig_t driverCfg = {
      .handle = reinterpret_cast<void*>(1),
      .transmit = pppTransmit,
      .transmit_wrap = nullptr,
      .driver_free_rx_buffer = nullptr,
  };

  esp_netif_config_t netifCfg = {
      .base = &baseCfg,
      .driver = &driverCfg,
      .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP,
  };

  pppNetif = esp_netif_new(&netifCfg);
  if (pppNetif == nullptr) {
    Serial.println("[LTE] Failed to create PPP netif.");
    return false;
  }

  esp_netif_ppp_config_t pppCfg = {
      .ppp_phase_event_enabled = true,
      .ppp_error_event_enabled = true,
  };
  (void)esp_netif_ppp_set_params(pppNetif, &pppCfg);

  esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &onPppGotIp, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &onPppLostIp, nullptr);
  esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &onPppStatus, nullptr);

  pppNetifCreated = true;
  Serial.println("[LTE] PPP netif created.");
  return true;
}

bool enterPppDataMode() {
  String response;
  String apn = configuredApn;
  if (apn.length() == 0) {
    apn = "internet";
  }

  if (!ensurePacketServiceReady()) {
    Serial.println("[LTE] Packet service not ready; PPP dial deferred.");
    return false;
  }

  if (!syncModemUartForPpp()) {
    Serial.println("[LTE] Continuing PPP dial with previous UART speed.");
  }

  configureModemForUartPpp();

  String apnCandidates[6];
  size_t apnCount = 0;
  auto addApnCandidate = [&](const String& candidate) {
    if (candidate.length() == 0) {
      return;
    }
    for (size_t i = 0; i < apnCount; ++i) {
      if (apnCandidates[i] == candidate) {
        return;
      }
    }
    apnCandidates[apnCount++] = candidate;
  };

  // Add configured APN first, then common Philippines carriers
  addApnCandidate(apn);
  addApnCandidate("tm");
  addApnCandidate("internet");           // TM Philippines
  addApnCandidate("internet.globe.com.ph"); // Globe Telecom
  addApnCandidate("mnet");               // Smart Communications
  addApnCandidate("gprs.smart.com.ph");  // Smart GPRS

  for (size_t i = 0; i < apnCount; ++i) {
    const String candidate = apnCandidates[i];
    Serial.print("[LTE] Trying APN: ");
    Serial.println(candidate);
    if (!lteSendCommand((String("AT+CGDCONT=1,\"IP\",\"") + candidate + "\"").c_str(), response, 4000)) {
      Serial.println("[LTE] Failed to set APN context; trying next...");
      continue;
    }

    // Apply PAP/none auth profile on PDP context for SIM7600 compatibility.
    if (configuredApnUser.length() > 0 || configuredApnPass.length() > 0) {
      if (!lteSendCommand((String("AT+CGAUTH=1,") + LTE_PDP_AUTH_PAP + ",\"" + configuredApnUser + "\",\"" + configuredApnPass + "\"").c_str(), response, 3000)) {
        Serial.println("[LTE] AT+CGAUTH failed; continuing with modem defaults.");
      }
    }

    if (!activateAndVerifyPdpContext()) {
      Serial.println("[LTE] PDP context not confirmed; trying PPP dial anyway...");
    }

    if (tryDialCommands()) {
      configuredApn = candidate;
      currentLteData.apn = configuredApn;
      Serial.print("[LTE] Successfully connected using APN: ");
      Serial.println(configuredApn);
      return true;
    }
    
    Serial.print("[LTE] PPP dial failed for APN: ");
    Serial.println(candidate);
  }

  Serial.println("[LTE] Unable to enter PPP data mode with any APN.");
  return false;
}

bool startInternetGateway() {
  if (dataModeActive) {
    // If already in data mode, try to enable NAT again in case it wasn't enabled
    if (pppHasIp) {
      enableApNat();
    }
    return true;
  }

  const unsigned long now = millis();
  // Use longer retry interval (45 seconds) to avoid hammering the modem
  constexpr uint32_t GATEWAY_RETRY_INTERVAL_MS = 45000;
  if (lastGatewayAttemptMs != 0 && (now - lastGatewayAttemptMs) < GATEWAY_RETRY_INTERVAL_MS) {
    return false;
  }
  lastGatewayAttemptMs = now;

  if (!ensurePppNetif()) {
    return false;
  }

#if CONFIG_LWIP_PPP_PAP_SUPPORT || CONFIG_LWIP_PPP_CHAP_SUPPORT || CONFIG_LWIP_PPP_MSCHAP_SUPPORT || \
    CONFIG_LWIP_PPP_MPPE_SUPPORT
  if (pppAuthFallbackEnabled) {
    (void)esp_netif_ppp_set_auth(pppNetif, NETIF_PPP_AUTHTYPE_NONE, "", "");
    Serial.println("[LTE] PPP auth fallback mode: NONE.");
  } else if (configuredApnUser.length() > 0) {
    (void)esp_netif_ppp_set_auth(pppNetif,
                                 static_cast<esp_netif_auth_type_t>(NETIF_PPP_AUTHTYPE_PAP | NETIF_PPP_AUTHTYPE_CHAP),
                                 configuredApnUser.c_str(),
                                 configuredApnPass.c_str());
    Serial.println("[LTE] PPP auth mode: PAP/CHAP.");
  } else {
    (void)esp_netif_ppp_set_auth(pppNetif,
                                 static_cast<esp_netif_auth_type_t>(NETIF_PPP_AUTHTYPE_PAP | NETIF_PPP_AUTHTYPE_CHAP),
                                 "",
                                 "");
    Serial.println("[LTE] PPP auth mode: PAP/CHAP (blank credentials).");
  }
#else
  Serial.println("[LTE] PPP auth support disabled in sdkconfig.");
#endif

  if (!enterPppDataMode()) {
    return false;
  }

  dataModeActive = true;
  dataModeEnteredMs = millis();
  pppHasIp = false;
  pppRxBytes = 0;
  pppTxBytes = 0;
  currentLteData.apn = configuredApn;
  currentLteData.dataConnected = false;
  currentLteData.ipAddress = "";

  if (pppRxTaskHandle == nullptr) {
    xTaskCreate(pppRxTask, "ppp_rx", 4096, nullptr, 8, &pppRxTaskHandle);
  }

  delay(100);
  esp_netif_action_start(pppNetif, nullptr, 0, nullptr);
  esp_netif_action_connected(pppNetif, nullptr, 0, nullptr);

  Serial.println("[LTE] Internet gateway started. Waiting for PPP to establish...");
  
  // Give the PPP connection some time to establish
  const unsigned long startWait = millis();
  while (millis() - startWait < 5000) {
    delay(100);
    if (pppHasIp) {
      Serial.println("[LTE] PPP established successfully!");
      break;
    }
  }
  
  return true;
}

void sendCommandAndPrint(const char* cmd, uint32_t timeoutMs) {
  String response;
  const bool ok = lteSendCommand(cmd, response, timeoutMs);
  Serial.print("[LTE] >> ");
  Serial.println(cmd);
  Serial.println("[LTE] <<");
  Serial.println(response.length() ? response : String("(no response)"));
  if (!ok) {
    Serial.println("[LTE] Command ended without OK.");
  }
}

bool sendAtAndWaitOk(uint32_t timeoutMs) {
  String response;
  const bool ok = lteSendCommand("AT", response, timeoutMs);
  if (ok) {
    Serial.println("[LTE] AT response OK.");
  } else {
    Serial.println("[LTE] AT response failed or timed out.");
  }
  return ok;
}

bool probeModemAtBaud(uint32_t baud, int attempts, uint32_t timeoutMs) {
  Serial.print("[LTE] Trying baud ");
  Serial.println(baud);
  lteSerial.begin(baud, SERIAL_8N1, activeUartRxPin, activeUartTxPin, activeUartInvert);
  delay(300);

  for (int attempt = 1; attempt <= attempts; ++attempt) {
    Serial.print("[LTE] AT probe attempt ");
    Serial.print(attempt);
    Serial.print("/" );
    Serial.print(attempts);
    Serial.print(" @ ");
    Serial.print(baud);
    Serial.print("... ");
    // Send multiple wake ATs per attempt to handle SIM7600 sleep/settling latency.
    if (sendAtAndWaitOk(timeoutMs) || sendAtAndWaitOk(timeoutMs)) {
      Serial.println("SUCCESS!");
      activeLteBaud = baud;
      return true;
    }
    Serial.println("timeout");
    delay(500);
  }
  return false;
}

void dumpModemStatus() {
  if (statusDumpDone) {
    return;
  }
  Serial.println("[LTE] Running modem status checks...");
  sendCommandAndPrint("ATE0", 1200);
  sendCommandAndPrint("AT+CPIN?", 1500);
  sendCommandAndPrint("AT+CSQ", 1500);
  sendCommandAndPrint("AT+CREG?", 1500);
  sendCommandAndPrint("AT+CEREG?", 1500);
  sendCommandAndPrint("AT+CGATT?", 1500);
  statusDumpDone = true;
}

void pulsePwrKey(bool activeLow) {
  pinMode(LTE_PWRKEY_PIN, OUTPUT);
  digitalWrite(LTE_PWRKEY_PIN, activeLow ? HIGH : LOW);
  delay(200);
  digitalWrite(LTE_PWRKEY_PIN, activeLow ? LOW : HIGH);
  delay(LTE_POWKEY_LOW_MS);
  digitalWrite(LTE_PWRKEY_PIN, activeLow ? HIGH : LOW);
  delay(200);
  // Release the line so we don't accidentally hold PWRKEY asserted.
  pinMode(LTE_PWRKEY_PIN, INPUT_PULLUP);
}

void waitForModemBoot(const char* reason) {
  Serial.print("[LTE] Waiting ");
  Serial.print(LTE_BOOT_WAIT_MS / 1000);
  Serial.print(" seconds for modem boot (" );
  Serial.print(reason);
  Serial.println(")...");
  for (uint32_t elapsed = 0; elapsed < LTE_BOOT_WAIT_MS; elapsed += 1000) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(" Boot wait complete.");
}

bool probeModemOnce() {
  Serial.println("[LTE] Starting AT command probes...");
  activeUartRxPin = LTE_UART_RX_PIN;
  activeUartTxPin = LTE_UART_TX_PIN;
  activeUartInvert = false;

  Serial.print("[LTE] Probing UART mapping RX=GPIO");
  Serial.print(activeUartRxPin);
  Serial.print(", TX=GPIO");
  Serial.print(activeUartTxPin);
  Serial.println(", invert=OFF");

  for (size_t i = 0; i < sizeof(LTE_BAUD_FALLBACKS) / sizeof(LTE_BAUD_FALLBACKS[0]); ++i) {
    if (probeModemAtBaud(LTE_BAUD_FALLBACKS[i], 4, 2600)) {
      consecutiveProbeFailures = 0;
      Serial.print("[LTE] Working UART mapping locked: RX=GPIO");
      Serial.print(activeUartRxPin);
      Serial.print(", TX=GPIO");
      Serial.print(activeUartTxPin);
      Serial.println(", invert=OFF");
      return true;
    }
  }
  return false;
}

bool passiveProbeWindow(uint32_t totalWindowMs) {
  const unsigned long start = millis();
  while (millis() - start < totalWindowMs) {
    if (probeModemOnce()) {
      return true;
    }
    Serial.println("[LTE] Passive probe retry...");
    delay(LTE_PASSIVE_PROBE_STEP_MS);
  }
  return false;
}

bool recoverModemAndProbe() {
  Serial.println("[LTE] Repeated AT failures; attempting modem recovery...");
  statusDumpDone = false;

  pulsePwrKey(true);
  waitForModemBoot("recovery active-low");
  bool recovered = passiveProbeWindow(12000);
  if (!recovered) {
    // Keep recovery deterministic for SIM7600G boards wired with active-low PWRKEY.
    pulsePwrKey(true);
    waitForModemBoot("recovery active-low retry");
    recovered = passiveProbeWindow(12000);
  }
  if (recovered) {
    Serial.println("[LTE] Modem recovery successful.");
  } else {
    Serial.println("[LTE] Modem recovery failed.");
  }
  return recovered;
}

bool lteIsResponsive() {
  return currentLteData.responsive;
}

bool lteDataModeActive() {
  return dataModeActive;
}

LteData lteGetData() {
  currentLteData.responsive = currentLteData.responsive;
  const bool hasValidIp = currentLteData.ipAddress.length() > 0 && currentLteData.ipAddress != "0.0.0.0";
  currentLteData.dataConnected = pppHasIp && hasValidIp;
  return currentLteData;
}

void lteSetApn(const String& apn) {
  const String trimmed = apn;
  if (trimmed.length() == 0) {
    return;
  }
  configuredApn = trimmed;
  currentLteData.apn = configuredApn;
  Serial.print("[LTE] APN updated to: ");
  Serial.println(configuredApn);
}

String lteGetApn() {
  return configuredApn;
}

void lteInit() {
  Serial.println("\n[LTE] ==== SIM7600G INITIALIZATION ====");
  Serial.print("[LTE] UART RX pin: GPIO");
  Serial.print(LTE_UART_RX_PIN);
  Serial.print(", TX pin: GPIO");
  Serial.print(LTE_UART_TX_PIN);
  Serial.print(", PWRKEY pin: GPIO");
  Serial.print(LTE_PWRKEY_PIN);
  Serial.print(", Baud: ");
  Serial.println(LTE_BAUD);

  lteSerial.begin(LTE_BAUD, SERIAL_8N1, LTE_UART_RX_PIN, LTE_UART_TX_PIN, activeUartInvert);
  delay(500);

  pinMode(LTE_PWRKEY_PIN, INPUT_PULLUP);
  delay(50);

  Serial.println("[LTE] Checking if modem is already awake (passive probe window)...");
  bool responsive = passiveProbeWindow(LTE_PASSIVE_PROBE_WINDOW_MS);

  if (!responsive) {
    Serial.println("[LTE] Trying PWRKEY active-low pulse...");
    pulsePwrKey(true);
    waitForModemBoot("power-on active-low");
    responsive = passiveProbeWindow(15000);
  }

  if (!responsive) {
    Serial.println("[LTE] Entering full modem recovery cycle...");
    responsive = recoverModemAndProbe();
  }

  currentLteData.responsive = responsive;
  currentLteData.apn = configuredApn;

  if (responsive) {
    Serial.println("[LTE] SIM7600G is responsive.");
    dumpModemStatus();
  } else {
    Serial.println("[LTE] SIM7600G is not responding.");
    Serial.println("[LTE] Check power, UART wiring, and SIM card.");
  }
}

void lteLoop() {
  // If already in data mode, keep trying to enable NAT and maintain connection
  if (dataModeActive) {
    if (pppHasIp) {
      enableApNat();
      dataModeEnteredMs = millis();
    } else {
      const unsigned long now = millis();
      if (dataModeEnteredMs != 0 && (now - dataModeEnteredMs) > LTE_PPP_BRINGUP_TIMEOUT_MS) {
        Serial.println("[LTE] PPP bring-up timeout without IP; recovering to AT mode.");
        resetPppSessionState("PPP bring-up timeout");
        currentLteData.responsive = probeModemOnce();
        if (currentLteData.responsive) {
          Serial.println("[LTE] Modem recovered and responsive after PPP timeout.");
          dumpModemStatus();
        } else {
          Serial.println("[LTE] Modem still unresponsive after PPP timeout; forcing recovery pulse.");
          currentLteData.responsive = recoverModemAndProbe();
        }
      }
    }
    return;
  }

  const unsigned long now = millis();
  if (!currentLteData.responsive && (now - lastProbeMs >= 5000)) {
    lastProbeMs = now;
    Serial.println("[LTE] Periodic modem probe (responsive check)...");
    currentLteData.responsive = probeModemOnce();
    if (currentLteData.responsive) {
      consecutiveProbeFailures = 0;
      Serial.println("[LTE] SIM7600G responsive.");
      dumpModemStatus();
    } else {
      ++consecutiveProbeFailures;
      Serial.print("[LTE] Probe failed count: ");
      Serial.println(consecutiveProbeFailures);
      if (consecutiveProbeFailures >= LTE_MAX_PROBE_FAILURES_BEFORE_RESET) {
        consecutiveProbeFailures = 0;
        Serial.println("[LTE] Max probe failures reached; attempting recovery...");
        currentLteData.responsive = recoverModemAndProbe();
      }
    }
  }

  if (currentLteData.responsive) {
    if (startInternetGateway()) {
      Serial.println("[LTE] Gateway startup successful.");
    }
  }
}

void lteStartInternetGateway() {
  (void)startInternetGateway();
}
