#pragma once
// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/). Lets the
// browser web-installer (ESP Web Tools) ask for WiFi creds right after flashing,
// over the same USB cable — no need to join the C3-AdBlock-XXXX portal AP.
// The setup portal stays as the fallback for boards powered without a computer.
//
// Packet: "IMPROV" | version=1 | type | len | data[len] | checksum (sum of all
// preceding bytes). Log output on the same Serial is fine — the browser side
// scans for the "IMPROV" header and ignores everything else.
//
// main.cpp provides improvJoin(): try the creds, persist them on success.

#ifndef FW_VERSION
#define FW_VERSION "1.1.0"
#endif

static bool improvJoin(const String& ssid, const String& pass);

enum : uint8_t { IMP_STATE = 0x01, IMP_ERROR = 0x02, IMP_RPC = 0x03, IMP_RESULT = 0x04 };
enum : uint8_t { IMP_READY = 0x02, IMP_PROVISIONING = 0x03, IMP_PROVISIONED = 0x04 };
enum : uint8_t { IMP_OK = 0x00, IMP_BAD_PACKET = 0x01, IMP_UNKNOWN_CMD = 0x02, IMP_CANT_CONNECT = 0x03 };
enum : uint8_t { CMD_WIFI = 0x01, CMD_STATE = 0x02, CMD_INFO = 0x03, CMD_SCAN = 0x04 };

static uint8_t impBuf[9 + 255 + 1];
static size_t  impLen = 0;
static bool    impBusy = false;   // a WiFi attempt services the loop too — don't re-enter

static void impSend(uint8_t type, const uint8_t* data, size_t n) {
  uint8_t hdr[9] = { 'I','M','P','R','O','V', 1, type, (uint8_t)n };
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  for (size_t i = 0; i < n; i++) sum += data[i];
  Serial.write(hdr, 9); Serial.write(data, n); Serial.write(sum); Serial.write('\n');
  Serial.flush();
}
static void impState(uint8_t s) { impSend(IMP_STATE, &s, 1); }
static void impError(uint8_t e) { impSend(IMP_ERROR, &e, 1); }

// RPC result: cmd | total len | (len | string)...
static void impResult(uint8_t cmd, std::initializer_list<String> strs) {
  uint8_t d[255]; size_t n = 2;
  for (const String& s : strs) {
    size_t l = s.length(); if (n + 1 + l > sizeof(d)) break;
    d[n++] = l; memcpy(d + n, s.c_str(), l); n += l;
  }
  d[0] = cmd; d[1] = n - 2;
  impSend(IMP_RESULT, d, n);
}

static bool impOnline() { return (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED; }
static String impUrl() { return "http://" + WiFi.localIP().toString(); }

static void impHandle(const uint8_t* d, size_t n) {
  if (n < 2 || (size_t)d[1] + 2 > n) { impError(IMP_BAD_PACKET); return; }
  switch (d[0]) {
    case CMD_STATE:
      impState(impOnline() ? IMP_PROVISIONED : IMP_READY);
      if (impOnline()) impResult(CMD_STATE, { impUrl() });
      break;
    case CMD_INFO:
#if CONFIG_IDF_TARGET_ESP32C3
      impResult(CMD_INFO, { "C3 AdBlocker", FW_VERSION, "ESP32-C3", "c3adblock" });
#else
      impResult(CMD_INFO, { "C3 AdBlocker", FW_VERSION, "ESP32", "c3adblock" });
#endif
      break;
    case CMD_SCAN: {
      int found = WiFi.scanNetworks();
      for (int i = 0; i < found && i < 20; i++)
        impResult(CMD_SCAN, { WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES" });
      WiFi.scanDelete();
      impResult(CMD_SCAN, {});                      // empty result = end of list
      break;
    }
    case CMD_WIFI: {
      size_t sl = d[2]; if (3 + sl >= n) { impError(IMP_BAD_PACKET); return; }
      size_t pl = d[3 + sl]; if (4 + sl + pl > n) { impError(IMP_BAD_PACKET); return; }
      String ssid, pass;
      ssid.concat((const char*)d + 3, sl); pass.concat((const char*)d + 4 + sl, pl);
      impError(IMP_OK); impState(IMP_PROVISIONING);
      impBusy = true;
      bool ok = improvJoin(ssid, pass);
      impBusy = false;
      if (!ok) { impError(IMP_CANT_CONNECT); impState(IMP_READY); return; }
      impState(IMP_PROVISIONED);
      impResult(CMD_WIFI, { impUrl() });            // installer shows "Visit device" -> dashboard
      Serial.println("[improv] WiFi saved, restarting");
      delay(500); ESP.restart();                    // boot the normal (non-portal) path
      break;
    }
    default:
      impError(IMP_UNKNOWN_CMD);
  }
}

// Call often from every blocking loop (WiFi connect, setup portal, main loop).
static void improvLoop() {
  if (impBusy) return;
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (impLen < 6) {                               // match the "IMPROV" header byte by byte
      if (b == "IMPROV"[impLen]) impBuf[impLen++] = b;
      else impLen = (b == 'I') ? (impBuf[0] = b, 1) : 0;
      continue;
    }
    impBuf[impLen++] = b;
    if (impLen == 7 && b != 1) { impLen = 0; continue; }        // version 1 only
    if (impLen < 9 || impLen < 9 + (size_t)impBuf[8] + 1) continue;
    size_t n = impBuf[8]; uint8_t sum = 0;
    for (size_t i = 0; i < 9 + n; i++) sum += impBuf[i];
    uint8_t type = impBuf[7];
    impLen = 0;
    if (sum != impBuf[9 + n]) { impError(IMP_BAD_PACKET); continue; }
    if (type == IMP_RPC) impHandle(impBuf + 9, n);
  }
}
