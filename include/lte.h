#pragma once

#include <Arduino.h>

struct LteData {
  bool responsive;
  bool simReady;
  bool dataConnected;
  bool pdpActive;
  int rssi;
  int ber;
  int creg;
  int cereg;
  int cgatt;
  String apn;
  String ipAddress;
};

void lteInit();
void lteLoop();
bool lteIsResponsive();
bool lteDataModeActive();
void lteStartInternetGateway();
void lteSetApn(const String& apn);
String lteGetApn();
LteData lteGetData();
bool lteSendCommand(const char* cmd, String& response, uint32_t timeoutMs = 1500);
