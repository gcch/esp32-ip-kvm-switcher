/*
 * Speed Studio XIAO ESP32-C6 KVM Switcher for KCEVE KC-SHM4 / AIMOS AM-SHM4
 *
 * Copyright (c) 2026 gcch.
 *
 */

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "esp_wifi.h"   // esp_wifi_set_ps()
#include "esp_log.h"    // esp_log_set_vprintf() for capturing ESP-IDF logs
#include "esp_sntp.h"   // SNTP sync notification / enable state
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#ifndef BUILD_NUMBER
#define BUILD_NUMBER "unknown"
#endif

// =====================
// Build-time options
// =====================
// Setup SoftAP: used for the initial setup and as a fallback when the Wi-Fi connection fails.
// The password must be 8-63 characters (WPA2). Override it at build time, e.g.
//   -DSETUP_AP_PASSWORD=\"my-secret-pass\"
#ifndef SETUP_AP_PASSWORD
#define SETUP_AP_PASSWORD "kvm-setup"
#endif
#ifndef SETUP_AP_SSID_PREFIX
#define SETUP_AP_SSID_PREFIX "KVM-Setup-"
#endif

// Defaults used when a key is missing from /config.json
#define DEFAULT_HOSTNAME     "esp32-kvm"
#define DEFAULT_MDNS_DOMAIN  "local"
#define DEFAULT_NTP_SERVER1  "ntp.nict.jp"
#define DEFAULT_NTP_SERVER2  "pool.ntp.org"
#define DEFAULT_TIMEZONE     "JST-9"

// =====================
// Runtime settings (loaded from /config.json on LittleFS)
// =====================
static char HOSTNAME[64];
static char MDNS_DOMAIN[254];

static char WIFI_SSID[33];
static char WIFI_PASSWORD[64];

// BASIC authentication (an empty user name disables authentication)
static char AUTH_USER[63];
static char AUTH_PASSWORD[256];

// IP address / DNS
static bool IP_STATIC  = false;  // false: DHCP, true: manual IP address
static bool DNS_MANUAL = false;  // false: DNS servers from DHCP, true: manual DNS servers
static char IP_ADDR[16];
static char IP_SUBNET[16];
static char IP_GATEWAY[16];
static char IP_DNS1[16];
static char IP_DNS2[16];

// NTP / time zone
static bool NTP_ENABLED = true;
static char NTP_SERVER1[64];
static char NTP_SERVER2[64];
static char TZ_STRING[48];       // POSIX TZ string, e.g. "JST-9"

// =====================
// GPIO pin definitions
// =====================
const int RELAY_PINS[4] = {0, 1, 2, 21};  // D0, D1, D2, D3
const int NUM_CHANNELS  = 4;

// =====================
// Timing constants (ms)
// =====================
const int PULSE_MS          = 300;   // Normal button press
const int EDID_TOGGLE_MS    = 3300;  // PC1 long press: toggle EDID ON/OFF
const int FACTORY_RESET_MS  = 11000; // PC1 long press: restore factory defaults
const int EDID_REFRESH_MS   = 3300;  // PC2 long press: reset (reload) EDID

// =====================
// Global variables
// =====================
WebServer server(80);
int currentChannel = 1;
bool otaUploadAuthorized = false;
bool otaUploadFailed = false;
bool otaRestartPending = false;
bool configLoaded = false;

// Setup SoftAP state
bool setupApActive = false;
char setupApSsid[32];

// Deferred restart (lets the HTTP response be flushed before rebooting)
bool restartPending = false;
unsigned long restartAtMs = 0;

// Wi-Fi monitoring
volatile bool wifiDisconnectedEvent = false;   // Set immediately by onWiFiEvent
volatile bool dnsReapplyNeeded = false;        // Set when a new IP lease was obtained
unsigned long lastWifiCheckMs = 0;             // Last forced liveness check
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;     // Force a liveness check every 10 s
unsigned long lastReconnectAttemptMs = 0;      // Last reconnect attempt
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;      // Non-blocking retry interval
const unsigned long WIFI_RETRY_INTERVAL_AP_MS = 30000;  // Slower retries while the setup AP is active
const unsigned long AP_FALLBACK_AFTER_MS = 300000;      // Start the setup AP after 5 min without Wi-Fi
bool staDownTracking = false;
unsigned long staDownSinceMs = 0;

// NTP state (updated from the SNTP task, so keep it volatile)
volatile bool ntpEverSynced = false;           // Synchronized at least once since boot / settings change
volatile time_t ntpLastSyncEpoch = 0;
volatile bool ntpSyncLogPending = false;

// =====================
// Time helpers
// =====================
static const time_t MIN_VALID_EPOCH = 1735689600;  // 2025-01-01 00:00:00 UTC

// Formats 't' as local time ("YYYY-MM-DD HH:MM:SS", optionally followed by the zone name)
static bool formatLocalTime(time_t t, char* out, size_t cap, bool withZone) {
  if (cap == 0) return false;
  out[0] = '\0';
  struct tm tmv;
  if (localtime_r(&t, &tmv) == nullptr) return false;
  return strftime(out, cap, withZone ? "%Y-%m-%d %H:%M:%S %Z" : "%Y-%m-%d %H:%M:%S", &tmv) > 0;
}

static bool isTimeValid() {
  return time(nullptr) >= MIN_VALID_EPOCH;
}

// =====================
// Console log capture
// Mirrors everything written to Log onto the UART (Serial) and keeps the most
// recent output in a ring buffer so that the Web UI can display it.
// Each line is prefixed with the local time (once known) and the uptime in seconds.
// =====================
struct LogReadResult {
  size_t   length;  // number of bytes copied into the output buffer
  uint32_t next;    // cursor the client should use for the next request
  bool     reset;   // client cursor is ahead of the device (device restarted)
  bool     lost;    // client cursor is older than the buffer (output dropped)
  bool     more;    // more data is available right now
};

class LogBuffer : public Print {
public:
  static const size_t CAPACITY = 8192;  // must be a power of two

  // Call once after Serial.begin()
  void begin() {
    if (mutex_ == nullptr) {
      mutex_ = xSemaphoreCreateMutexStatic(&mutexStorage_);
    }
    esp_log_set_vprintf(&LogBuffer::vprintfHook);
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* data, size_t len) override {
    Serial.write(data, len);
    store(data, len);
    return len;
  }

  // Copy up to 'cap' bytes of log output starting at absolute offset 'since'
  LogReadResult read(uint32_t since, char* out, size_t cap) {
    LogReadResult r = {0, since, false, false, false};
    if (mutex_ == nullptr) return r;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    const uint32_t oldest = (head_ > CAPACITY) ? (head_ - CAPACITY) : 0;
    if (since > head_) {
      since = oldest;       // device restarted since the client's last request
      r.reset = true;
    } else if (since < oldest) {
      since = oldest;       // part of the output was overwritten
      r.lost = true;
    }
    size_t n = head_ - since;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) {
      out[i] = buf_[(since + i) & (CAPACITY - 1)];
    }
    r.length = n;
    r.next = since + (uint32_t)n;
    r.more = (r.next < head_);
    xSemaphoreGive(mutex_);
    return r;
  }

  static int vprintfHook(const char* fmt, va_list args);

private:
  void putChar(char c) {
    buf_[head_ & (CAPACITY - 1)] = c;
    head_++;
  }

  // Local time string for the current second (call with the mutex held).
  // The formatted string is cached so that localtime_r() runs at most once per second.
  const char* timestamp() {
    static const char* const UNSET = "---------- --:--:--";
    const time_t now = time(nullptr);
    if (now < MIN_VALID_EPOCH) return UNSET;
    if (now != cachedSec_) {
      if (!formatLocalTime(now, cachedStamp_, sizeof(cachedStamp_), false)) return UNSET;
      cachedSec_ = now;
    }
    return cachedStamp_;
  }

  void store(const uint8_t* data, size_t len) {
    if (mutex_ == nullptr) return;  // not initialised yet
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
      const char c = (char)data[i];
      if (c == '\r') continue;  // normalise CRLF to LF
      if (atLineStart_ && c != '\n') {
        char prefix[56];
        const unsigned long ms = millis();
        int n = snprintf(prefix, sizeof(prefix), "[%s] [%lu.%03lu] ", timestamp(), ms / 1000UL, ms % 1000UL);
        if (n < 0) n = 0;
        if (n >= (int)sizeof(prefix)) n = (int)sizeof(prefix) - 1;
        for (int k = 0; k < n; k++) putChar(prefix[k]);
        atLineStart_ = false;
      }
      putChar(c);
      if (c == '\n') atLineStart_ = true;
    }
    xSemaphoreGive(mutex_);
  }

  char buf_[CAPACITY];
  uint32_t head_ = 0;          // total number of bytes ever written
  bool atLineStart_ = true;
  time_t cachedSec_ = 0;
  char cachedStamp_[24] = {0};
  SemaphoreHandle_t mutex_ = nullptr;
  StaticSemaphore_t mutexStorage_;
};

LogBuffer Log;

// Redirect ESP-IDF / Arduino core logs (ESP_LOGx, log_e, ...) into Log as well
int LogBuffer::vprintfHook(const char* fmt, va_list args) {
  char line[256];
  const int n = vsnprintf(line, sizeof(line), fmt, args);
  if (n <= 0) return n;
  const size_t len = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
  Log.write((const uint8_t*)line, len);
  return n;
}

// =====================
// Input validation helpers
// =====================

// Strict dotted-quad IPv4 parser (no IPv6, no shorthand forms)
static bool parseIPv4(const char* text, IPAddress& out) {
  if (text == nullptr || text[0] == '\0') return false;
  unsigned parts[4] = {0, 0, 0, 0};
  const char* p = text;
  for (int count = 0; count < 4; count++) {
    if (*p < '0' || *p > '9') return false;
    unsigned value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      value = value * 10 + (unsigned)(*p - '0');
      if (++digits > 3 || value > 255) return false;
      p++;
    }
    parts[count] = value;
    if (count < 3) {
      if (*p != '.') return false;
      p++;
    }
  }
  if (*p != '\0') return false;
  out = IPAddress(parts[0], parts[1], parts[2], parts[3]);
  return true;
}

static uint32_t ipToU32(const IPAddress& ip) {
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

// Contiguous mask between /1 and /30
static bool isValidSubnetMask(const IPAddress& mask) {
  const uint32_t inverted = ~ipToU32(mask);
  return inverted >= 3 && (inverted & (inverted + 1)) == 0;
}

// Returns nullptr when valid, otherwise a short error message
static const char* validateStaticIp(const char* ip, const char* subnet, const char* gateway) {
  IPAddress a, m, g;
  if (!parseIPv4(ip, a)) return "Invalid IP address";
  if (!parseIPv4(subnet, m) || !isValidSubnetMask(m)) return "Invalid subnet mask";
  if (!parseIPv4(gateway, g)) return "Invalid default gateway";
  if (a[0] == 0 || a[0] == 127 || a[0] >= 224) return "IP address must be a unicast address";
  const uint32_t ipv = ipToU32(a);
  const uint32_t mask = ipToU32(m);
  const uint32_t gw = ipToU32(g);
  if ((ipv & ~mask) == 0 || (ipv | mask) == 0xFFFFFFFFu) return "IP address must not be the network or broadcast address";
  if ((gw & mask) != (ipv & mask)) return "Default gateway must be in the same subnet as the IP address";
  if (gw == ipv) return "Default gateway must differ from the IP address";
  if ((gw & ~mask) == 0 || (gw | mask) == 0xFFFFFFFFu) return "Invalid default gateway";
  return nullptr;
}

static const char* validateDnsServer(const char* text, bool required) {
  if (text == nullptr || text[0] == '\0') return required ? "Primary DNS server is required" : nullptr;
  IPAddress ip;
  if (!parseIPv4(text, ip) || ipToU32(ip) == 0) return "Invalid DNS server address";
  return nullptr;
}

// Host name or IPv4 literal (letters, digits, '-' and '.')
static bool isValidHostName(const char* text) {
  if (text == nullptr) return false;
  const size_t length = strlen(text);
  if (length == 0 || length > 63) return false;
  for (size_t i = 0; i < length; i++) {
    const char c = text[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok) return false;
    if (c == '.' && i + 1 < length && text[i + 1] == '.') return false;
  }
  return text[0] != '.' && text[0] != '-' && text[length - 1] != '.' && text[length - 1] != '-';
}

// POSIX TZ string: printable ASCII without spaces, quotes or backslashes
static bool isValidTimezone(const char* text) {
  if (text == nullptr) return false;
  const size_t length = strlen(text);
  if (length == 0 || length >= sizeof(TZ_STRING)) return false;
  for (size_t i = 0; i < length; i++) {
    const unsigned char c = (unsigned char)text[i];
    if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\\') return false;
  }
  return true;
}

// =====================
// Config file load / save
// An empty AUTH_USER disables authentication.
// Every key is optional; missing keys fall back to the defaults.
// =====================
static void setConfigDefaults() {
  snprintf(HOSTNAME, sizeof(HOSTNAME), "%s", DEFAULT_HOSTNAME);
  snprintf(MDNS_DOMAIN, sizeof(MDNS_DOMAIN), "%s", DEFAULT_MDNS_DOMAIN);
  WIFI_SSID[0] = '\0';
  WIFI_PASSWORD[0] = '\0';
  AUTH_USER[0] = '\0';
  AUTH_PASSWORD[0] = '\0';
  IP_STATIC = false;
  DNS_MANUAL = false;
  IP_ADDR[0] = IP_SUBNET[0] = IP_GATEWAY[0] = IP_DNS1[0] = IP_DNS2[0] = '\0';
  NTP_ENABLED = true;
  snprintf(NTP_SERVER1, sizeof(NTP_SERVER1), "%s", DEFAULT_NTP_SERVER1);
  snprintf(NTP_SERVER2, sizeof(NTP_SERVER2), "%s", DEFAULT_NTP_SERVER2);
  snprintf(TZ_STRING, sizeof(TZ_STRING), "%s", DEFAULT_TIMEZONE);
}

// Copies a JSON string into 'destination'. A missing key selects 'fallback'.
// Returns false (leaving 'destination' untouched) if the value is not a string or too long.
static bool readConfigString(char* destination, size_t capacity, JsonVariantConst value, const char* key, const char* fallback) {
  if (value.isNull()) {
    snprintf(destination, capacity, "%s", fallback);
    return true;
  }

  const char* text = value.as<const char*>();
  if (text == nullptr) {
    Log.printf("Config value must be a string: %s\n", key);
    return false;
  }

  const size_t length = strlen(text);
  if (length >= capacity) {
    Log.printf("Config value too long: %s (max %u bytes)\n", key, (unsigned)(capacity - 1));
    return false;
  }

  memcpy(destination, text, length + 1);
  return true;
}

bool loadConfig() {
  setConfigDefaults();

  // Format on failure so that settings can still be saved on a blank flash
  if (!LittleFS.begin(true)) {
    Log.println("LittleFS mount failed");
    return false;
  }

  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    Log.println("Config file not found");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Log.println("JSON parse error");
    return false;
  }

  bool valid = true;
  valid &= readConfigString(HOSTNAME, sizeof(HOSTNAME), doc["hostname"], "hostname", DEFAULT_HOSTNAME);
  valid &= readConfigString(MDNS_DOMAIN, sizeof(MDNS_DOMAIN), doc["mdns_domain"], "mdns_domain", DEFAULT_MDNS_DOMAIN);
  valid &= readConfigString(WIFI_SSID, sizeof(WIFI_SSID), doc["wifi_ssid"], "wifi_ssid", "");
  valid &= readConfigString(WIFI_PASSWORD, sizeof(WIFI_PASSWORD), doc["wifi_password"], "wifi_password", "");
  valid &= readConfigString(AUTH_USER, sizeof(AUTH_USER), doc["auth_user"], "auth_user", "");
  valid &= readConfigString(AUTH_PASSWORD, sizeof(AUTH_PASSWORD), doc["auth_password"], "auth_password", "");

  char ipMode[16];
  char dnsMode[16];
  valid &= readConfigString(ipMode, sizeof(ipMode), doc["ip_mode"], "ip_mode", "dhcp");
  valid &= readConfigString(dnsMode, sizeof(dnsMode), doc["dns_mode"], "dns_mode", "auto");
  valid &= readConfigString(IP_ADDR, sizeof(IP_ADDR), doc["ip_address"], "ip_address", "");
  valid &= readConfigString(IP_SUBNET, sizeof(IP_SUBNET), doc["subnet_mask"], "subnet_mask", "");
  valid &= readConfigString(IP_GATEWAY, sizeof(IP_GATEWAY), doc["gateway"], "gateway", "");
  valid &= readConfigString(IP_DNS1, sizeof(IP_DNS1), doc["dns1"], "dns1", "");
  valid &= readConfigString(IP_DNS2, sizeof(IP_DNS2), doc["dns2"], "dns2", "");
  valid &= readConfigString(NTP_SERVER1, sizeof(NTP_SERVER1), doc["ntp_server1"], "ntp_server1", DEFAULT_NTP_SERVER1);
  valid &= readConfigString(NTP_SERVER2, sizeof(NTP_SERVER2), doc["ntp_server2"], "ntp_server2", DEFAULT_NTP_SERVER2);
  valid &= readConfigString(TZ_STRING, sizeof(TZ_STRING), doc["timezone"], "timezone", DEFAULT_TIMEZONE);

  JsonVariantConst ntpEnabled = doc["ntp_enabled"];
  if (!ntpEnabled.isNull()) {
    if (ntpEnabled.is<bool>()) {
      NTP_ENABLED = ntpEnabled.as<bool>();
    } else {
      Log.println("Config value must be a boolean: ntp_enabled");
      valid = false;
    }
  }
  if (!valid) return false;

  // Wi-Fi: an empty SSID simply means "not configured yet" (the setup AP is started)
  if (strlen(WIFI_SSID) > 32) {
    Log.println("Wi-Fi SSID must be 32 bytes or less");
    return false;
  }
  const size_t passwordLength = strlen(WIFI_PASSWORD);
  if (passwordLength != 0 && passwordLength < 8) {
    Log.println("Wi-Fi password must be 8 to 63 bytes (or empty for an open network)");
    return false;
  }

  // IP / DNS
  if (strcmp(ipMode, "static") == 0) {
    IP_STATIC = true;
  } else if (strcmp(ipMode, "dhcp") == 0) {
    IP_STATIC = false;
  } else {
    Log.println("ip_mode must be \"dhcp\" or \"static\"");
    return false;
  }
  if (strcmp(dnsMode, "manual") == 0) {
    DNS_MANUAL = true;
  } else if (strcmp(dnsMode, "auto") == 0) {
    DNS_MANUAL = false;
  } else {
    Log.println("dns_mode must be \"auto\" or \"manual\"");
    return false;
  }
  if (IP_STATIC) {
    const char* message = validateStaticIp(IP_ADDR, IP_SUBNET, IP_GATEWAY);
    if (message == nullptr && !DNS_MANUAL) message = "A static IP address requires manual DNS servers";
    if (message != nullptr) {
      Log.printf("Invalid network config: %s\n", message);
      return false;
    }
  }
  if (DNS_MANUAL) {
    const char* message = validateDnsServer(IP_DNS1, true);
    if (message == nullptr) message = validateDnsServer(IP_DNS2, false);
    if (message != nullptr) {
      Log.printf("Invalid DNS config: %s\n", message);
      return false;
    }
  }

  // NTP
  if (NTP_ENABLED && !isValidHostName(NTP_SERVER1)) {
    Log.println("ntp_server1 is invalid");
    return false;
  }
  if (NTP_SERVER2[0] != '\0' && !isValidHostName(NTP_SERVER2)) {
    Log.println("ntp_server2 is invalid");
    return false;
  }
  if (!isValidTimezone(TZ_STRING)) {
    Log.println("timezone is invalid");
    return false;
  }

  Log.println("Config loaded");
  Log.printf("HOSTNAME: %s\n", HOSTNAME);
  Log.printf("MDNS_DOMAIN: %s\n", MDNS_DOMAIN);
  return true;
}

// Writes all settings to /config.json (via a temporary file so that a failed write keeps the old file)
bool saveConfig() {
  JsonDocument doc;
  doc["hostname"]      = HOSTNAME;
  doc["mdns_domain"]   = MDNS_DOMAIN;
  doc["wifi_ssid"]     = WIFI_SSID;
  doc["wifi_password"] = WIFI_PASSWORD;
  doc["auth_user"]     = AUTH_USER;
  doc["auth_password"] = AUTH_PASSWORD;
  doc["ip_mode"]       = IP_STATIC ? "static" : "dhcp";
  doc["ip_address"]    = IP_ADDR;
  doc["subnet_mask"]   = IP_SUBNET;
  doc["gateway"]       = IP_GATEWAY;
  doc["dns_mode"]      = DNS_MANUAL ? "manual" : "auto";
  doc["dns1"]          = IP_DNS1;
  doc["dns2"]          = IP_DNS2;
  doc["ntp_enabled"]   = NTP_ENABLED;
  doc["ntp_server1"]   = NTP_SERVER1;
  doc["ntp_server2"]   = NTP_SERVER2;
  doc["timezone"]      = TZ_STRING;

  File file = LittleFS.open("/config.tmp", "w");
  if (!file) {
    Log.println("Failed to open /config.tmp for writing");
    return false;
  }
  const size_t written = serializeJsonPretty(doc, file);
  file.close();
  if (written == 0) {
    Log.println("Failed to write /config.tmp");
    LittleFS.remove("/config.tmp");
    return false;
  }

  if (!LittleFS.rename("/config.tmp", "/config.json")) {
    // Some filesystems refuse to overwrite an existing file
    LittleFS.remove("/config.json");
    if (!LittleFS.rename("/config.tmp", "/config.json")) {
      Log.println("Failed to replace /config.json");
      return false;
    }
  }
  Log.println("Config saved");
  return true;
}

static bool staEnabled() {
  return configLoaded && WIFI_SSID[0] != '\0';
}

static bool isAuthEnabled() {
  return AUTH_USER[0] != '\0';
}

// =====================
// BASIC authentication check
// Authentication is skipped when AUTH_USER is empty
// =====================
bool checkAuth() {
  if (!isAuthEnabled()) return true;
  if (!server.authenticate(AUTH_USER, AUTH_PASSWORD)) {
    server.requestAuthentication(BASIC_AUTH, "KVM Switcher");
    return false;
  }
  return true;
}

// =====================
// Low-level relay control (non-blocking)
// Waits with a millis() loop instead of delay() and keeps calling
// server.handleClient() meanwhile, so the network stays responsive
// during EDID operations.
// =====================
void holdRelay(int pin, int ms) {
  digitalWrite(pin, HIGH);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ms) {
    server.handleClient();  // keep serving network requests
    delay(10);
  }
  digitalWrite(pin, LOW);
}

// =====================
// Input switching
// =====================
void pressButton(int channel) {
  if (channel < 1 || channel > NUM_CHANNELS) return;
  holdRelay(RELAY_PINS[channel - 1], PULSE_MS);
  currentChannel = channel;
  Log.printf("Switched to input %d\n", channel);
}

// =====================
// EDID operations
// =====================

// PC1 long press (3 s): toggle EDID ON/OFF
void edidToggle() {
  Log.println("EDID toggle started (PC1, 3 s)");
  holdRelay(RELAY_PINS[0], EDID_TOGGLE_MS);
  Log.println("EDID toggle done");
}

// PC1 long press (10 s): restore factory defaults
void factoryReset() {
  Log.println("Factory reset started (PC1, 10 s)");
  holdRelay(RELAY_PINS[0], FACTORY_RESET_MS);
  Log.println("Factory reset done");
}

// PC2 long press (3 s): reset (reload) EDID
void edidRefresh() {
  Log.println("EDID refresh started (PC2, 3 s)");
  holdRelay(RELAY_PINS[1], EDID_REFRESH_MS);
  Log.println("EDID refresh done");
}

// =====================
// Wi-Fi event handler
// When the router blinks or reboots, WiFi.status() may update late or never,
// so the disconnect event is detected directly and a reconnect flag is raised.
// =====================
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Log.println("[WiFiEvent] Disconnect event detected");
      wifiDisconnectedEvent = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Log.println("[WiFiEvent] Got IP address");
      wifiDisconnectedEvent = false;
      dnsReapplyNeeded = true;  // handled in loop() to keep this handler short
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Log.println("[WiFiEvent] Setup AP: client connected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Log.println("[WiFiEvent] Setup AP: client disconnected");
      break;
    default:
      break;
  }
}

// =====================
// Manual DNS servers while DHCP is in use
// A DHCP lease may overwrite the DNS servers, so the override is re-applied
// after every new lease and checked periodically.
// With a static IP address the DNS servers are set by WiFi.config() instead.
// =====================
static void ensureManualDns() {
  if (IP_STATIC || !DNS_MANUAL) return;
  IPAddress dns1, dns2;
  if (!parseIPv4(IP_DNS1, dns1)) return;
  if (!parseIPv4(IP_DNS2, dns2)) dns2 = IPAddress();
  if (WiFi.dnsIP(0) == dns1 && (ipToU32(dns2) == 0 || WiFi.dnsIP(1) == dns2)) return;
  WiFi.setDNS(dns1, dns2);
  Log.printf("Manual DNS applied: %s%s%s\n", IP_DNS1, IP_DNS2[0] ? ", " : "", IP_DNS2);
}

// =====================
// Wi-Fi connection + power save disabled
// Shared by setup() and the automatic reconnect logic
// =====================
bool connectWiFi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(setupApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);  // also enable the library's built-in auto reconnect
  WiFi.persistent(false);       // do not write flash on every begin()

  if (IP_STATIC) {
    IPAddress ip, subnet, gateway, dns1, dns2;
    parseIPv4(IP_ADDR, ip);
    parseIPv4(IP_SUBNET, subnet);
    parseIPv4(IP_GATEWAY, gateway);
    parseIPv4(IP_DNS1, dns1);
    if (!parseIPv4(IP_DNS2, dns2)) dns2 = IPAddress();
    if (!WiFi.config(ip, gateway, subnet, dns1, dns2)) {
      Log.println("Static IP configuration failed");
    }
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Log.printf("Connecting to Wi-Fi (%s)", WIFI_SSID);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    Log.print('.');
    server.handleClient();  // keep serving the Web UI while connecting
    delay(500);
    retry++;
  }
  Log.println();

  if (WiFi.status() != WL_CONNECTED) {
    Log.println("Wi-Fi connection failed");
    return false;
  }

  // Disable power saving (Modem Sleep can make the device drop pings)
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifiDisconnectedEvent = false;
  ensureManualDns();
  Log.printf("Access page: http://%s/\n", WiFi.localIP().toString().c_str());
  return true;
}

// =====================
// Setup SoftAP
// Started when Wi-Fi is not configured yet, when the first connection fails,
// or when the connection has been down for AP_FALLBACK_AFTER_MS.
// It is stopped again once the station is connected and nobody uses the AP.
// =====================
void startSetupAP() {
  if (setupApActive) return;

  // SSID suffix: last three bytes of the factory MAC address
  const uint64_t efuse = ESP.getEfuseMac();
  snprintf(setupApSsid, sizeof(setupApSsid), "%s%02X%02X%02X", SETUP_AP_SSID_PREFIX,
           (unsigned)((efuse >> 24) & 0xFF), (unsigned)((efuse >> 32) & 0xFF), (unsigned)((efuse >> 40) & 0xFF));

  WiFi.mode(WIFI_AP_STA);  // AP + STA so that the network scan keeps working
  if (!WiFi.softAP(setupApSsid, SETUP_AP_PASSWORD)) {
    Log.println("Failed to start the setup AP");
    return;
  }
  setupApActive = true;
  Log.printf("Setup AP started: SSID=%s, URL=http://%s/\n", setupApSsid, WiFi.softAPIP().toString().c_str());
}

void stopSetupAP() {
  if (!setupApActive) return;
  WiFi.softAPdisconnect(true);
  setupApActive = false;
  Log.println("Setup AP stopped");
}

// =====================
// mDNS start (also used to restart after a reconnect)
// =====================
void startMDNS() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    Log.printf("mDNS started: http://%s.%s/\n", HOSTNAME, MDNS_DOMAIN);
  } else {
    Log.println("mDNS start failed");
  }
}

// =====================
// NTP / time zone
// =====================

// Called from the SNTP task after the system clock was set: keep it minimal
static void onNtpSynced(struct timeval* tv) {
  ntpLastSyncEpoch = tv->tv_sec;
  ntpEverSynced = true;
  ntpSyncLogPending = true;
}

static void applyTimezone() {
  setenv("TZ", TZ_STRING, 1);
  tzset();
}

// (Re)starts SNTP. Calling it again also triggers an immediate resynchronization.
// The server name buffers are global because lwIP keeps pointers to them.
static void applyNtp() {
  applyTimezone();
  if (NTP_ENABLED) {
    configTzTime(TZ_STRING, NTP_SERVER1, NTP_SERVER2[0] ? NTP_SERVER2 : nullptr);
    Log.printf("NTP started: %s%s%s (TZ=%s)\n", NTP_SERVER1, NTP_SERVER2[0] ? ", " : "", NTP_SERVER2, TZ_STRING);
  } else {
    if (esp_sntp_enabled()) esp_sntp_stop();
    Log.println("NTP disabled");
  }
}

static void scheduleRestart(unsigned long delayMs) {
  restartAtMs = millis() + delayMs;
  restartPending = true;
}

// =====================
// Web UI HTML
// =====================
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 KVM Switch</title>
<style>
:root{color-scheme:light;--ink:#191919;--line:#c5c5c5;--blue:#005ea8;--focus:#2670e8;--warning:#b76900;--danger:#b50909}
*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,"Noto Sans JP",sans-serif;line-height:1.6;margin:0;background:#f3f5f7;color:var(--ink)}.page-header{border-bottom:4px solid var(--blue);background:#fff;padding:16px 20px}.header-inner{max-width:1100px;margin:0 auto;display:flex;align-items:baseline;justify-content:space-between;gap:12px;flex-wrap:wrap}.title{font-size:1.55rem;line-height:1.35;margin:0}.firmware-version{font-size:.9rem;color:#505a64}.container{max-width:1100px;margin:0 auto;padding:20px 20px 48px}.tabs{display:flex;gap:4px;overflow-x:auto;border-bottom:2px solid #767676;margin:0;padding:0}.tab{white-space:nowrap;min-height:44px;padding:8px 14px;margin:0 0 -2px;border:0;border-bottom:2px solid transparent;border-radius:4px 4px 0 0;background:#fff;color:var(--blue);font:inherit;font-weight:700;cursor:pointer}.tab[aria-selected=true]{color:var(--ink);border-bottom-color:var(--blue)}:focus-visible{outline:3px solid var(--focus);outline-offset:2px}[hidden]{display:none!important}h2{font-size:1.3rem;margin:0 0 16px;padding-bottom:8px;border-bottom:2px solid var(--ink)}h3{font-size:1.1rem;margin:24px 0 12px}section[role=tabpanel]{background:#fff;border:1px solid var(--line);border-radius:4px;padding:24px;margin:20px 0}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:12px 0 24px}.btn,.edid-btn,#ota-submit{font:inherit;font-weight:700;min-height:44px;padding:8px 16px;border:2px solid var(--blue);border-radius:4px;background:var(--blue);color:#fff;cursor:pointer;margin:3px}.grid .btn{width:100%;margin:0;padding:8px 4px}.btn:hover,.edid-btn:hover,#ota-submit:hover{background:#003e73;border-color:#003e73}.btn.active{background:#003e73;border-color:#003e73;box-shadow:inset 0 -3px 0 #ffbf47}.btn-indicator,.btn-num{display:none}.btn-label{display:block;font-size:.95rem}.edid-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin:12px 0}.edid-btn{width:100%;margin:0}.edid-btn.danger{background:var(--danger);border-color:var(--danger)}.edid-btn.danger:hover,.edid-btn.running.danger{background:#8b0000;border-color:#8b0000}.edid-btn:disabled,#ota-submit:disabled{background:#b8b8b8;border-color:#b8b8b8;cursor:not-allowed}.edid-btn.running{background:#003e73}.notice{padding:12px 16px;border-left:4px solid var(--warning);background:#fff4e5}.progress-wrap{margin:12px 0}.progress-label{min-height:24px;margin-bottom:6px}.progress-bar{height:6px;background:var(--line);overflow:hidden}.progress-fill{height:100%;width:0;background:var(--blue)}.progress-fill.danger{background:var(--danger)}.status-table{width:100%;border-collapse:collapse;margin:12px 0}th{text-align:left}td{padding:10px 12px;border-bottom:1px solid var(--line);vertical-align:top}td:first-child{font-weight:700;width:34%;background:#f5f5f5}.status-value{font-variant-numeric:tabular-nums}#toast{position:fixed;z-index:1000;bottom:12px;left:50%;transform:translate(-50%,80px);max-width:calc(100% - 24px);padding:12px 18px;background:#e8f5e9;border:2px solid #2e7d32;border-radius:4px;box-shadow:0 2px 10px #0004;transition:transform .2s;pointer-events:none}#toast.show{transform:translate(-50%,0)}
.log-toolbar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:0 0 12px}.log-toolbar .btn{margin:0}.log-toolbar label{display:inline-flex;align-items:center;gap:6px;min-height:44px;padding:0 4px}.log-status{color:#505a64;font-size:.9rem}.log-view{height:420px;overflow:auto;margin:0;padding:12px;background:#111;color:#e6e6e6;border-radius:4px;font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all}
@media(max-width:640px){.log-view{height:320px}}
@media(max-width:640px){.container{padding:16px 12px 36px}.page-header{padding:16px 12px}section[role=tabpanel]{padding:16px;margin:14px 0}.title{font-size:1.4rem}.grid{gap:6px}.grid .btn{font-size:.9rem}.edid-grid{grid-template-columns:1fr}.status-table,tbody,tr,th,td{display:block;width:100%!important}.status-table th{padding:8px 12px 2px;background:#f5f5f5}td:first-child{border-bottom:0;padding-bottom:2px}td+td{padding-top:4px}}
@media(prefers-reduced-motion:reduce){*,*::before,*::after{scroll-behavior:auto!important;transition-duration:.01ms!important;animation-duration:.01ms!important}}
@media(min-width:641px){.form-table th{width:34%;vertical-align:top;padding-top:12px}}
.form-table input[type=text],.form-table input[type=password],.form-table select{width:100%;max-width:420px;min-height:44px;padding:6px 10px;margin:0 0 6px;font:inherit;color:inherit;background:#fff;border:2px solid #767676;border-radius:4px}
.form-table input:disabled,.form-table select:disabled{background:#ececec;color:#6b6b6b}
.form-table .btn{margin:2px 0 6px}
.choice{display:inline-flex;align-items:center;gap:6px;min-height:44px;margin-right:16px}
.choice input{width:20px;height:20px;margin:0}
.field-hint{display:block;margin-top:2px;color:#505a64;font-size:.9rem}
.save-btn{width:auto;min-width:200px;margin:8px 0}
.form-status{min-height:24px;margin-top:6px}
.form-status.error{color:var(--danger);font-weight:700}
.notice.danger{border-left-color:var(--danger);background:#fdecea}
.notice{margin:0 0 16px}
</style>
</head>
<body>
<header class="page-header"><div class="header-inner"><h1 class="title">ESP32 KVM Switch</h1><div class="firmware-version">FW <span id="firmware-version">Loading</span></div></div></header>
<main class="container">
  <div class="tabs" role="tablist" aria-label="Admin panel sections">
    <button class="tab" id="tab-control" type="button" role="tab" aria-selected="true" aria-controls="panel-control" tabindex="0">Control</button>
    <button class="tab" id="tab-device" type="button" role="tab" aria-selected="false" aria-controls="panel-device" tabindex="-1">Device</button>
    <button class="tab" id="tab-network" type="button" role="tab" aria-selected="false" aria-controls="panel-network" tabindex="-1">Network</button>
    <button class="tab" id="tab-time" type="button" role="tab" aria-selected="false" aria-controls="panel-time" tabindex="-1">Time</button>
    <button class="tab" id="tab-account" type="button" role="tab" aria-selected="false" aria-controls="panel-account" tabindex="-1">Account</button>
    <button class="tab" id="tab-log" type="button" role="tab" aria-selected="false" aria-controls="panel-log" tabindex="-1">Console Log</button>
    <button class="tab" id="tab-ota" type="button" role="tab" aria-selected="false" aria-controls="panel-ota" tabindex="-1">Firmware Update</button>
  </div>
  <section id="panel-control" role="tabpanel" aria-labelledby="tab-control" tabindex="0">
  <h2>Input Selection</h2>
  <div class="grid">
    <button class="btn" type="button" onclick="switchInput(1)" id="btn1" aria-pressed="false" aria-label="Switch to input 1"><span class="btn-label">Input 1</span></button>
    <button class="btn" type="button" onclick="switchInput(2)" id="btn2" aria-pressed="false" aria-label="Switch to input 2"><span class="btn-label">Input 2</span></button>
    <button class="btn" type="button" onclick="switchInput(3)" id="btn3" aria-pressed="false" aria-label="Switch to input 3"><span class="btn-label">Input 3</span></button>
    <button class="btn" type="button" onclick="switchInput(4)" id="btn4" aria-pressed="false" aria-label="Switch to input 4"><span class="btn-label">Input 4</span></button>
  </div>
  <h3>EDID Control</h3>
  <p>Runs long-press button actions. Factory reset erases the KVM settings.</p>
  <div class="edid-grid">
    <button class="edid-btn" type="button" id="ebtn-toggle" onclick="triggerEdid('toggle')">Toggle EDID ON/OFF (~3 s)</button>
    <button class="edid-btn" type="button" id="ebtn-refresh" onclick="triggerEdid('refresh')">Refresh EDID (~3 s)</button>
    <button class="edid-btn danger" type="button" id="ebtn-factory" onclick="triggerEdid('factory')">Factory Reset (~11 s)</button>
  </div>
  <div class="progress-wrap"><div class="progress-label" id="progress-label" role="status" aria-live="polite"></div><div class="progress-bar" id="progress-bar"><div class="progress-fill" id="progress-fill"></div></div></div>
  </section>
  <section id="panel-device" role="tabpanel" aria-labelledby="tab-device" tabindex="0" hidden>
  <h2>Device Status</h2>
  <table class="status-table"><tbody>
    <tr><th scope="row">SoC</th><td id="device-chip">Checking</td></tr>
    <tr><th scope="row">CPU</th><td id="device-cpu">Checking</td></tr>
    <tr><th scope="row">RAM (Total)</th><td id="device-ram-total">Checking</td></tr>
    <tr><th scope="row">RAM (Free)</th><td id="device-ram-free">Checking</td></tr>
    <tr><th scope="row">PSRAM</th><td id="device-psram">Not installed</td></tr>
    <tr><th scope="row">Internal Flash</th><td id="device-flash">Checking</td></tr>
    <tr><th scope="row">Firmware Partition</th><td id="device-app">Checking</td></tr>
    <tr><th scope="row">LittleFS</th><td id="device-filesystem">Checking</td></tr>
    <tr><th scope="row">Current Input</th><td id="current-label">Unknown</td></tr>
    <tr><th scope="row">Current Time</th><td id="device-time">Checking</td></tr>
    <tr><th scope="row">Firmware Build</th><td id="device-firmware-version">Loading</td></tr>
  </tbody></table>
  </section>
  <section id="panel-network" role="tabpanel" aria-labelledby="tab-network" tabindex="0" hidden>
  <h2>Network Status</h2>
  <div class="notice danger" id="net-ap-notice" hidden>The setup access point is active. Open this page through it (for example http://192.168.4.1/) and enter your Wi-Fi settings below.</div>
  <table class="status-table"><tbody>
    <tr><th scope="row">Wi-Fi Mode</th><td id="net-mode">Checking</td></tr>
    <tr><th scope="row">Connected SSID</th><td id="net-ssid">Checking</td></tr>
    <tr><th scope="row">Signal Strength</th><td id="net-rssi">Checking</td></tr>
    <tr><th scope="row">IP Address</th><td id="net-ip">Checking</td></tr>
    <tr><th scope="row">Subnet Mask</th><td id="net-subnet">Checking</td></tr>
    <tr><th scope="row">Default Gateway</th><td id="net-gateway">Checking</td></tr>
    <tr><th scope="row">DNS Servers</th><td id="net-dns">Checking</td></tr>
    <tr><th scope="row">MAC Address</th><td id="net-mac">Checking</td></tr>
    <tr><th scope="row">Hostname</th><td id="net-hostname">Checking</td></tr>
    <tr><th scope="row">Setup AP</th><td id="net-ap">Checking</td></tr>
  </tbody></table>

  <h2>Wi-Fi Settings</h2>
  <form id="wifi-form" autocomplete="off">
    <table class="status-table form-table"><tbody>
      <tr><th scope="row"><label for="wifi-ssid">SSID</label></th><td>
        <input id="wifi-ssid" type="text" maxlength="32" autocomplete="off" spellcheck="false" autocapitalize="off">
        <div><button class="btn" type="button" id="wifi-scan">Scan Networks</button></div>
        <select id="wifi-scan-list" aria-label="Detected networks" hidden></select>
        <span class="field-hint" id="wifi-scan-status" role="status" aria-live="polite"></span>
      </td></tr>
      <tr><th scope="row"><label for="wifi-password">Password</label></th><td>
        <input id="wifi-password" type="password" maxlength="63" autocomplete="new-password">
        <label class="choice"><input type="checkbox" id="wifi-open"> Open network (no password)</label>
        <span class="field-hint">Leave blank to keep the saved password. WPA2 passwords are 8 to 63 characters.</span>
      </td></tr>
    </tbody></table>
    <button class="edid-btn save-btn" id="wifi-submit" type="submit">Save and Restart</button>
    <div class="form-status" id="wifi-msg" role="status" aria-live="polite"></div>
  </form>

  <h2>IP Address and DNS</h2>
  <form id="ip-form" autocomplete="off">
    <table class="status-table form-table"><tbody>
      <tr><th scope="row" id="ip-mode-label">IP Address</th><td>
        <div role="radiogroup" aria-labelledby="ip-mode-label">
          <label class="choice"><input type="radio" name="ip-mode" id="ip-dhcp" value="dhcp" checked> Automatic (DHCP)</label>
          <label class="choice"><input type="radio" name="ip-mode" id="ip-static" value="static"> Manual</label>
        </div>
      </td></tr>
      <tr><th scope="row"><label for="ip-addr">IP Address</label></th><td><input id="ip-addr" type="text" inputmode="decimal" placeholder="192.168.1.50" autocomplete="off" spellcheck="false"></td></tr>
      <tr><th scope="row"><label for="ip-subnet">Subnet Mask</label></th><td><input id="ip-subnet" type="text" inputmode="decimal" placeholder="255.255.255.0" autocomplete="off" spellcheck="false"></td></tr>
      <tr><th scope="row"><label for="ip-gateway">Default Gateway</label></th><td><input id="ip-gateway" type="text" inputmode="decimal" placeholder="192.168.1.1" autocomplete="off" spellcheck="false"></td></tr>
      <tr><th scope="row" id="dns-mode-label">DNS Servers</th><td>
        <div role="radiogroup" aria-labelledby="dns-mode-label">
          <label class="choice"><input type="radio" name="dns-mode" id="dns-auto" value="auto" checked> Automatic (DHCP)</label>
          <label class="choice"><input type="radio" name="dns-mode" id="dns-manual" value="manual"> Manual</label>
        </div>
        <span class="field-hint">A manual IP address requires manual DNS servers.</span>
      </td></tr>
      <tr><th scope="row"><label for="dns1">Primary DNS</label></th><td><input id="dns1" type="text" inputmode="decimal" placeholder="8.8.8.8" autocomplete="off" spellcheck="false"></td></tr>
      <tr><th scope="row"><label for="dns2">Secondary DNS</label></th><td><input id="dns2" type="text" inputmode="decimal" placeholder="Optional" autocomplete="off" spellcheck="false"></td></tr>
    </tbody></table>
    <button class="edid-btn save-btn" id="ip-submit" type="submit">Save and Restart</button>
    <div class="form-status" id="ip-msg" role="status" aria-live="polite"></div>
  </form>
  </section>

  <section id="panel-time" role="tabpanel" aria-labelledby="tab-time" tabindex="0" hidden>
  <h2>Time Status</h2>
  <table class="status-table"><tbody>
    <tr><th scope="row">Current Time</th><td id="time-now">Checking</td></tr>
    <tr><th scope="row">Time Zone</th><td id="time-zone">Checking</td></tr>
    <tr><th scope="row">NTP Status</th><td id="time-status">Checking</td></tr>
    <tr><th scope="row">Last Synchronization</th><td id="time-last">Checking</td></tr>
  </tbody></table>
  <button class="edid-btn save-btn" id="ntp-sync" type="button">Synchronize Now</button>
  <div class="form-status" id="ntp-sync-msg" role="status" aria-live="polite"></div>

  <h2>NTP Settings</h2>
  <form id="ntp-form" autocomplete="off">
    <table class="status-table form-table"><tbody>
      <tr><th scope="row">NTP Client</th><td><label class="choice"><input type="checkbox" id="ntp-enabled" checked> Synchronize the clock with NTP servers</label></td></tr>
      <tr><th scope="row"><label for="ntp-server1">Primary Server</label></th><td><input id="ntp-server1" type="text" maxlength="63" placeholder="ntp.nict.jp" autocomplete="off" spellcheck="false" autocapitalize="off"></td></tr>
      <tr><th scope="row"><label for="ntp-server2">Secondary Server</label></th><td><input id="ntp-server2" type="text" maxlength="63" placeholder="Optional" autocomplete="off" spellcheck="false" autocapitalize="off"></td></tr>
      <tr><th scope="row"><label for="ntp-tz">Time Zone</label></th><td>
        <select id="ntp-tz"></select>
        <input id="ntp-tz-custom" type="text" maxlength="47" placeholder="e.g. JST-9" aria-label="Custom POSIX time zone string" autocomplete="off" spellcheck="false" hidden>
        <span class="field-hint">Pick a region or a fixed UTC offset. Any other zone can be entered as a POSIX TZ string (for example JST-9).</span>
      </td></tr>
    </tbody></table>
    <button class="edid-btn save-btn" id="ntp-submit" type="submit">Save</button>
    <div class="form-status" id="ntp-msg" role="status" aria-live="polite"></div>
  </form>
  </section>

  <section id="panel-account" role="tabpanel" aria-labelledby="tab-account" tabindex="0" hidden>
  <h2>Web UI Account</h2>
  <div class="notice danger" id="account-warning" hidden>Authentication is disabled. Anyone on the network can operate this device. Set a user name and password below.</div>
  <form id="account-form" autocomplete="off">
    <table class="status-table form-table"><tbody>
      <tr><th scope="row"><label for="account-user">User Name</label></th><td>
        <input id="account-user" type="text" maxlength="62" autocomplete="username" spellcheck="false" autocapitalize="off">
        <span class="field-hint">Printable ASCII without spaces or ":". Leave empty to disable authentication.</span>
      </td></tr>
      <tr><th scope="row"><label for="account-password">New Password</label></th><td>
        <input id="account-password" type="password" maxlength="255" autocomplete="new-password">
        <span class="field-hint">8 to 255 printable ASCII characters. Leave blank to keep the current password.</span>
      </td></tr>
      <tr><th scope="row"><label for="account-password2">Confirm Password</label></th><td><input id="account-password2" type="password" maxlength="255" autocomplete="new-password"></td></tr>
    </tbody></table>
    <button class="edid-btn save-btn" id="account-submit" type="submit">Save</button>
    <div class="form-status" id="account-msg" role="status" aria-live="polite"></div>
  </form>
  </section>
  <section id="panel-log" role="tabpanel" aria-labelledby="tab-log" tabindex="0" hidden>
  <h2>Console Log</h2>
  <p>Shows the most recent console output of the device (about 8 KiB). Older output is discarded automatically. Each line starts with the local time (shown as dashes until the clock is set by NTP) followed by the uptime in seconds.</p>
  <div class="log-toolbar">
    <button class="btn" type="button" id="log-pause" aria-pressed="false">Pause</button>
    <button class="btn" type="button" id="log-clear">Clear View</button>
    <button class="btn" type="button" id="log-download">Download</button>
    <label><input type="checkbox" id="log-autoscroll" checked> Auto-scroll</label>
    <span class="log-status" id="log-status" role="status" aria-live="polite">Waiting...</span>
  </div>
  <pre class="log-view" id="log-view" tabindex="0" aria-label="Console log output"></pre>
  </section>
  <section id="panel-ota" role="tabpanel" aria-labelledby="tab-ota" tabindex="0" hidden>
  <h2>Firmware Info</h2>
  <table class="status-table"><tbody>
    <tr><th scope="row">Build Number</th><td id="ota-firmware-version">Loading</td></tr>
    <tr><th scope="row">Running Partition</th><td id="ota-running-partition">Loading</td></tr>
  </tbody></table>
  <h2>Firmware Update</h2>
  <p class="notice">Select a firmware file (.bin) for the ESP32. An OTA-capable partition table is required. Do not power off during the update. The device restarts automatically when it finishes.</p>
  <form id="ota-form">
    <table class="status-table"><tbody>
      <tr><th scope="row"><label for="firmware-file">Update File</label></th><td><input id="firmware-file" name="firmware" type="file" accept=".bin,application/octet-stream" required></td></tr>
      <tr><th scope="row">Upload Status</th><td><div class="progress-label" id="ota-status" role="status" aria-live="polite">Select a firmware file.</div><div class="progress-bar"><div class="progress-fill" id="ota-progress"></div></div></td></tr>
    </tbody></table>
    <button class="edid-btn danger" id="ota-submit" type="submit">Update Firmware</button>
  </form>
  </section>
</main>

<div id="toast" role="status" aria-live="polite"></div>

<script>
let current = 0;

const EDID_OPS = {
  toggle:  { label: 'Toggling EDID ON/OFF...', ms: 3300,  btnId: 'ebtn-toggle',  isDanger: false },
  refresh: { label: 'Refreshing EDID...',       ms: 3300,  btnId: 'ebtn-refresh', isDanger: false },
  factory: { label: 'Resetting to factory defaults...', ms: 11000, btnId: 'ebtn-factory', isDanger: true  },
};

function updateUI(ch) {
  current = ch;
  document.getElementById('current-label').textContent = 'Input ' + ch;
  for (let i = 1; i <= 4; i++) {
    const selected = i === ch;
    document.getElementById('btn' + i).classList.toggle('active', selected);
    document.getElementById('btn' + i).setAttribute('aria-pressed', String(selected));
  }
}

function showToast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(() => t.classList.remove('show'), 2000);
}

async function switchInput(ch) {
  try {
    const res = await fetch('/api/input', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({channel: ch})
    });
    if (!res.ok) throw new Error();
    const data = await res.json();
    updateUI(data.channel);
    showToast('Switched to input ' + ch);
  } catch(e) { showToast('Failed to switch input'); }
}

async function fetchStatus() {
  try {
    const [inputResponse, deviceResponse] = await Promise.all([fetch('/api/input'), fetch('/api/device')]);
    if (!inputResponse.ok || !deviceResponse.ok) throw new Error();
    const input = await inputResponse.json();
    const device = await deviceResponse.json();
    updateUI(input.channel);
    const version = device.version || 'Unknown';
    document.getElementById('firmware-version').textContent = version;
    document.getElementById('device-firmware-version').textContent = version;
    document.getElementById('ota-firmware-version').textContent = version;
    document.getElementById('device-chip').textContent = `${device.chip} (Rev ${device.revision}, ${device.cores} cores)`;
    document.getElementById('device-cpu').textContent = `${device.cpuMHz} MHz`;
    document.getElementById('device-ram-total').textContent = formatBytes(device.ramTotal);
    document.getElementById('device-ram-free').textContent = `${formatBytes(device.ramFree)} (min free ${formatBytes(device.ramMinFree)})`;
    document.getElementById('device-psram').textContent = device.psramTotal ? `${formatBytes(device.psramFree)} / ${formatBytes(device.psramTotal)} free` : 'Not installed';
    document.getElementById('device-flash').textContent = formatBytes(device.flashTotal);
    document.getElementById('device-app').textContent = `${device.appLabel} (${formatBytes(device.appSize)}, ${formatBytes(device.appFree)} free)`;
    document.getElementById('device-filesystem').textContent = `${formatBytes(device.fsUsed)} / ${formatBytes(device.fsTotal)} used`;
    document.getElementById('ota-running-partition').textContent = device.appLabel;
    document.getElementById('device-time').textContent = device.time || 'Not synchronized';
  } catch(e) {
    document.getElementById('firmware-version').textContent = 'Failed to load';
    document.getElementById('device-firmware-version').textContent = 'Failed to load';
    document.getElementById('ota-firmware-version').textContent = 'Failed to load';
  }
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(2) + ' MiB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KiB';
  return bytes + ' B';
}

async function triggerEdid(op) {
  if (op === 'factory' && !window.confirm('Reset the KVM to factory defaults. Continue?')) return;
  const cfg     = EDID_OPS[op];
  const allBtns = Object.values(EDID_OPS).map(c => document.getElementById(c.btnId));
  const bar     = document.getElementById('progress-bar');
  const fill    = document.getElementById('progress-fill');
  const lbl     = document.getElementById('progress-label');

  allBtns.forEach(b => b.disabled = true);
  const activeBtn = document.getElementById(cfg.btnId);
  activeBtn.classList.add('running');
  lbl.textContent = cfg.label;
  bar.classList.add('active');
  fill.classList.toggle('danger', cfg.isDanger);
  fill.style.transition = 'none';
  fill.style.width = '0%';
  setTimeout(() => {
    fill.style.transition = `width ${cfg.ms}ms linear`;
    fill.style.width = '100%';
  }, 50);

  try {
    const res = await fetch('/api/edid/' + op, { method: 'POST' });
    if (!res.ok) throw new Error();
    const data = await res.json();
    lbl.textContent = data.message || 'Operation completed';
    showToast(data.message || 'Done');
    if (op === 'refresh' || op === 'factory') {
      updateUI(1);
    }
  } catch(e) { lbl.textContent = 'Operation failed'; showToast('Operation failed'); }

  setTimeout(() => {
    allBtns.forEach(b => b.disabled = false);
    activeBtn.classList.remove('running');
    lbl.textContent = '';
    bar.classList.remove('active');
    fill.style.transition = 'none';
    fill.style.width = '0%';
  }, cfg.ms + 200);
}

const tabs = Array.from(document.querySelectorAll('[role="tab"]'));
function selectTab(tab) {
  tabs.forEach(item => {
    const selected = item === tab;
    item.setAttribute('aria-selected', String(selected));
    item.tabIndex = selected ? 0 : -1;
    document.getElementById(item.getAttribute('aria-controls')).hidden = !selected;
  });
  tab.focus();
  onTabShown(tab.id);
}
tabs.forEach((tab, index) => {
  tab.addEventListener('click', () => selectTab(tab));
  tab.addEventListener('keydown', event => {
    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
    event.preventDefault();
    selectTab(tabs[(index + (event.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length]);
  });
});

fetchStatus();

// ---- Console log viewer ----
const tabLog = document.getElementById('tab-log');
const logView = document.getElementById('log-view');
const logStatus = document.getElementById('log-status');
const logPauseBtn = document.getElementById('log-pause');
const logAutoScroll = document.getElementById('log-autoscroll');
const LOG_MAX_CHARS = 60000;
let logCursor = 0;
let logText = '';
let logPaused = false;
let logBusy = false;
let logConnected = null;

function setLogStatus(connected) {
  if (connected === logConnected) return;
  logConnected = connected;
  logStatus.textContent = connected ? 'Live' : 'Disconnected. Retrying...';
}

function appendLog(text) {
  if (!text) return;
  logText += text;
  if (logText.length > LOG_MAX_CHARS) logText = logText.slice(logText.length - LOG_MAX_CHARS);
  renderLog();
}

function renderLog() {
  logView.textContent = logText;
  if (logAutoScroll.checked) logView.scrollTop = logView.scrollHeight;
}

async function pollLog() {
  if (logBusy || logPaused || document.hidden) return;
  if (tabLog.getAttribute('aria-selected') !== 'true') return;
  logBusy = true;
  try {
    // Keep fetching while the device reports more data (bounded to stay responsive)
    for (let i = 0; i < 10; i++) {
      const res = await fetch('/api/log?since=' + logCursor, { cache: 'no-store' });
      if (!res.ok) throw new Error();
      const text = await res.text();
      if (res.headers.get('X-Log-Reset') === '1') {
        logText = '';
        appendLog('--- device restarted ---\n');
      } else if (res.headers.get('X-Log-Lost') === '1') {
        appendLog('--- older output was dropped ---\n');
      }
      // Strip ANSI color escape sequences emitted by some log sources
      appendLog(text.replace(/\x1b\[[0-9;]*m/g, ''));
      logCursor = Number(res.headers.get('X-Log-Next')) || 0;
      setLogStatus(true);
      if (res.headers.get('X-Log-More') !== '1') break;
    }
  } catch (e) {
    setLogStatus(false);
  } finally {
    logBusy = false;
  }
}

logPauseBtn.addEventListener('click', () => {
  logPaused = !logPaused;
  logPauseBtn.textContent = logPaused ? 'Resume' : 'Pause';
  logPauseBtn.setAttribute('aria-pressed', String(logPaused));
  if (!logPaused) pollLog();
});
document.getElementById('log-clear').addEventListener('click', () => { logText = ''; renderLog(); });
document.getElementById('log-download').addEventListener('click', () => {
  const url = URL.createObjectURL(new Blob([logText], { type: 'text/plain' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = 'kvm-console-log.txt';
  a.click();
  URL.revokeObjectURL(url);
});
logAutoScroll.addEventListener('change', renderLog);
tabLog.addEventListener('click', pollLog);
setInterval(pollLog, 1000);

// ---- Settings: Network / Time / Account ----
function byId(id) { return document.getElementById(id); }
function sleep(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }
function isTabSelected(id) { return byId(id).getAttribute('aria-selected') === 'true'; }
function ipOrEmpty(value) { return value && value !== '0.0.0.0' ? value : ''; }

async function apiGet(path) {
  const res = await fetch(path, { cache: 'no-store' });
  if (!res.ok) throw new Error('Request failed (' + res.status + ')');
  return res.json();
}

async function apiPost(path, body) {
  const res = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {})
  });
  let data = {};
  try { data = await res.json(); } catch (e) {}
  if (!res.ok) throw new Error(data.error || 'Request failed (' + res.status + ')');
  return data;
}

function setStatus(id, message, isError) {
  const el = byId(id);
  el.textContent = message || '';
  el.classList.toggle('error', !!isError);
}

function showValue(id, value) {
  byId(id).textContent = (value === undefined || value === null || value === '' || value === '0.0.0.0') ? '-' : value;
}

// After saving settings that restart the device: reload once it answers again
async function waitForRestart(msgId, message) {
  await sleep(8000);
  for (let i = 0; i < 26; i++) {
    try {
      const res = await fetch('/api/input', { cache: 'no-store' });
      if (res.ok) { location.reload(); return; }
    } catch (e) {}
    await sleep(2000);
  }
  setStatus(msgId, message + ' The device did not answer at this address yet.', true);
}

function onTabShown(id) {
  if (id === 'tab-device') fetchStatus();
  else if (id === 'tab-network') loadNetwork();
  else if (id === 'tab-time') loadNtp();
  else if (id === 'tab-account') loadAccount();
}

// ---- Network ----
let lastNet = null;
let wifiSaved = { ssid: '', hasPassword: false };
let networkFormsFilled = false;

function updateIpFormState() {
  const isStatic = byId('ip-static').checked;
  ['ip-addr', 'ip-subnet', 'ip-gateway'].forEach(id => { byId(id).disabled = !isStatic; });
  if (isStatic) byId('dns-manual').checked = true;  // a static address needs manual DNS
  byId('dns-auto').disabled = isStatic;
  const manualDns = byId('dns-manual').checked;
  ['dns1', 'dns2'].forEach(id => { byId(id).disabled = !manualDns; });
}

// Prefill empty manual fields with the values currently in use
function prefillIpFields() {
  if (!lastNet) return;
  if (byId('ip-static').checked && !byId('ip-addr').value) {
    byId('ip-addr').value = ipOrEmpty(lastNet.ip);
    byId('ip-subnet').value = ipOrEmpty(lastNet.subnet);
    byId('ip-gateway').value = ipOrEmpty(lastNet.gateway);
  }
  if (byId('dns-manual').checked && !byId('dns1').value) {
    byId('dns1').value = ipOrEmpty(lastNet.dns1);
    byId('dns2').value = ipOrEmpty(lastNet.dns2);
  }
}

function fillNetworkForms(cfg) {
  wifiSaved = { ssid: cfg.wifi_ssid || '', hasPassword: !!cfg.wifi_has_password };
  byId('wifi-ssid').value = wifiSaved.ssid;
  const open = !!wifiSaved.ssid && !wifiSaved.hasPassword;
  byId('wifi-open').checked = open;
  byId('wifi-password').disabled = open;
  byId('wifi-password').placeholder = wifiSaved.hasPassword ? '(unchanged)' : '';
  (cfg.ip_mode === 'static' ? byId('ip-static') : byId('ip-dhcp')).checked = true;
  byId('ip-addr').value = cfg.ip || '';
  byId('ip-subnet').value = cfg.subnet || '';
  byId('ip-gateway').value = cfg.gateway || '';
  (cfg.dns_mode === 'manual' ? byId('dns-manual') : byId('dns-auto')).checked = true;
  byId('dns1').value = cfg.dns1 || '';
  byId('dns2').value = cfg.dns2 || '';
  updateIpFormState();
}

async function loadNetwork() {
  try {
    const n = await apiGet('/api/network');
    lastNet = n;
    showValue('net-mode', n.mode);
    showValue('net-ssid', n.connected ? n.ssid : 'Not connected');
    showValue('net-rssi', n.connected ? n.rssi + ' dBm' : '');
    showValue('net-ip', n.ip);
    showValue('net-subnet', n.subnet);
    showValue('net-gateway', n.gateway);
    showValue('net-dns', [n.dns1, n.dns2].filter(v => v && v !== '0.0.0.0').join(', '));
    showValue('net-mac', n.mac);
    showValue('net-hostname', n.hostname + ' (' + n.mdns + ')');
    byId('net-ap').textContent = n.ap.active
      ? n.ap.ssid + ' (' + n.ap.ip + ', ' + n.ap.clients + ' client' + (n.ap.clients === 1 ? '' : 's') + ')'
      : 'Inactive';
    byId('net-ap-notice').hidden = !n.ap.active;
    if (!networkFormsFilled) {
      fillNetworkForms(n.config);
      networkFormsFilled = true;
    }
  } catch (e) { /* keep the last values */ }
}

async function scanWifi() {
  const button = byId('wifi-scan');
  const list = byId('wifi-scan-list');
  button.disabled = true;
  list.hidden = true;
  setStatus('wifi-scan-status', 'Scanning...');
  try {
    let data = await apiGet('/api/wifi/scan?start=1');
    for (let i = 0; i < 20 && data.scanning; i++) {
      await sleep(1000);
      data = await apiGet('/api/wifi/scan');
    }
    if (data.scanning) throw new Error('timed out');
    const nets = (data.networks || []).slice().sort((a, b) => b.rssi - a.rssi);
    list.textContent = '';
    const head = document.createElement('option');
    head.value = '';
    head.textContent = nets.length ? 'Select a network (' + nets.length + ' found)' : 'No networks found';
    list.appendChild(head);
    nets.forEach(net => {
      const option = document.createElement('option');
      option.value = net.ssid;
      option.dataset.secure = net.secure ? '1' : '0';
      option.textContent = net.ssid + ' (' + net.rssi + ' dBm' + (net.secure ? '' : ', open') + ')';
      list.appendChild(option);
    });
    list.hidden = false;
    setStatus('wifi-scan-status', '');
  } catch (e) {
    setStatus('wifi-scan-status', 'Scan failed: ' + e.message, true);
  } finally {
    button.disabled = false;
  }
}

byId('wifi-scan').addEventListener('click', scanWifi);
byId('wifi-scan-list').addEventListener('change', event => {
  const option = event.target.selectedOptions[0];
  if (!option || !option.value) return;
  byId('wifi-ssid').value = option.value;
  const open = option.dataset.secure === '0';
  byId('wifi-open').checked = open;
  byId('wifi-password').disabled = open;
  byId('wifi-password').value = '';
  if (!open) byId('wifi-password').focus();
});
byId('wifi-open').addEventListener('change', event => {
  byId('wifi-password').disabled = event.target.checked;
  if (event.target.checked) byId('wifi-password').value = '';
});
document.querySelectorAll('input[name="ip-mode"], input[name="dns-mode"]').forEach(radio => {
  radio.addEventListener('change', () => { updateIpFormState(); prefillIpFields(); });
});

byId('wifi-form').addEventListener('submit', async event => {
  event.preventDefault();
  const ssid = byId('wifi-ssid').value;
  const open = byId('wifi-open').checked;
  const password = byId('wifi-password').value;
  if (!ssid) { setStatus('wifi-msg', 'Enter the SSID.', true); return; }
  const body = { ssid: ssid };
  if (open) body.password = '';
  else if (password) body.password = password;
  else if (ssid !== wifiSaved.ssid || !wifiSaved.hasPassword) { setStatus('wifi-msg', 'Enter the Wi-Fi password.', true); return; }
  if (!window.confirm('Save the Wi-Fi settings and restart the device. Continue?')) return;
  byId('wifi-submit').disabled = true;
  setStatus('wifi-msg', 'Saving...');
  try {
    const data = await apiPost('/api/wifi', body);
    const message = data.message || 'Saved. The device is restarting.';
    setStatus('wifi-msg', message);
    waitForRestart('wifi-msg', message);
  } catch (e) {
    setStatus('wifi-msg', e.message, true);
    byId('wifi-submit').disabled = false;
  }
});

byId('ip-form').addEventListener('submit', async event => {
  event.preventDefault();
  const isStatic = byId('ip-static').checked;
  const manualDns = byId('dns-manual').checked;
  const body = { mode: isStatic ? 'static' : 'dhcp', dns_mode: manualDns ? 'manual' : 'auto' };
  if (isStatic) {
    body.ip = byId('ip-addr').value.trim();
    body.subnet = byId('ip-subnet').value.trim();
    body.gateway = byId('ip-gateway').value.trim();
  }
  if (manualDns) {
    body.dns1 = byId('dns1').value.trim();
    body.dns2 = byId('dns2').value.trim();
  }
  if (!window.confirm('Save the IP settings and restart the device. The address used to reach this page may change. Continue?')) return;
  byId('ip-submit').disabled = true;
  setStatus('ip-msg', 'Saving...');
  try {
    const data = await apiPost('/api/ip', body);
    const message = data.message || 'Saved. The device is restarting.';
    setStatus('ip-msg', message);
    waitForRestart('ip-msg', message);
  } catch (e) {
    setStatus('ip-msg', e.message, true);
    byId('ip-submit').disabled = false;
  }
});

// ---- Time / NTP ----
// Time zone presets grouped by region. Values are POSIX TZ strings (the sign is
// inverted relative to UTC: "JST-9" means UTC+9). DST rules are included where used.
const TZ_GROUPS = [
  ['UTC', [
    ['UTC0', 'UTC']
  ]],
  ['Asia', [
    ['JST-9', 'Japan (JST, UTC+9)'],
    ['KST-9', 'Korea (KST, UTC+9)'],
    ['CST-8', 'China / Taiwan (UTC+8)'],
    ['HKT-8', 'Hong Kong (UTC+8)'],
    ['<+08>-8', 'Singapore / Malaysia / Philippines (UTC+8)'],
    ['<+07>-7', 'Thailand / Vietnam / Jakarta (UTC+7)'],
    ['<+0630>-6:30', 'Myanmar (UTC+6:30)'],
    ['<+06>-6', 'Bangladesh / Bhutan (UTC+6)'],
    ['<+0545>-5:45', 'Nepal (UTC+5:45)'],
    ['IST-5:30', 'India / Sri Lanka (UTC+5:30)'],
    ['PKT-5', 'Pakistan (UTC+5)'],
    ['<+05>-5', 'Uzbekistan / Kazakhstan (UTC+5)'],
    ['<+0430>-4:30', 'Afghanistan (UTC+4:30)'],
    ['<+04>-4', 'UAE / Oman / Azerbaijan (UTC+4)'],
    ['<+0330>-3:30', 'Iran (UTC+3:30)'],
    ['<+03>-3', 'Saudi Arabia / Iraq / Turkey (UTC+3)'],
    ['IST-2IDT,M3.4.4/26,M10.5.0', 'Israel (IST/IDT)']
  ]],
  ['Europe', [
    ['GMT0', 'Iceland / West Africa (GMT)'],
    ['WET0WEST,M3.5.0/1,M10.5.0', 'Portugal (WET/WEST)'],
    ['GMT0BST,M3.5.0/1,M10.5.0', 'United Kingdom / Ireland (GMT/BST)'],
    ['CET-1CEST,M3.5.0,M10.5.0/3', 'Central Europe (CET/CEST)'],
    ['EET-2EEST,M3.5.0/3,M10.5.0/4', 'Eastern Europe (EET/EEST)'],
    ['MSK-3', 'Moscow (MSK, UTC+3)']
  ]],
  ['Africa', [
    ['WAT-1', 'West Africa / Nigeria (WAT, UTC+1)'],
    ['EET-2EEST,M4.5.5/0,M10.5.4/24', 'Egypt (EET/EEST)'],
    ['SAST-2', 'South Africa (SAST, UTC+2)'],
    ['CAT-2', 'Central Africa (CAT, UTC+2)'],
    ['EAT-3', 'East Africa (EAT, UTC+3)']
  ]],
  ['North America', [
    ['HST10', 'Hawaii (HST)'],
    ['AKST9AKDT,M3.2.0,M11.1.0', 'Alaska (AKST/AKDT)'],
    ['PST8PDT,M3.2.0,M11.1.0', 'US / Canada Pacific'],
    ['MST7MDT,M3.2.0,M11.1.0', 'US / Canada Mountain'],
    ['MST7', 'Arizona (MST, no DST)'],
    ['CST6CDT,M3.2.0,M11.1.0', 'US / Canada Central'],
    ['CST6', 'Mexico City (CST, no DST)'],
    ['EST5EDT,M3.2.0,M11.1.0', 'US / Canada Eastern'],
    ['AST4ADT,M3.2.0,M11.1.0', 'Atlantic Canada (AST/ADT)'],
    ['NST3:30NDT,M3.2.0,M11.1.0', 'Newfoundland (NST/NDT)']
  ]],
  ['South America', [
    ['<-05>5', 'Colombia / Peru / Ecuador (UTC-5)'],
    ['<-04>4', 'Venezuela / Bolivia (UTC-4)'],
    ['<-04>4<-03>,M9.1.6/24,M4.1.6/24', 'Chile (UTC-4/-3)'],
    ['<-03>3', 'Brazil (Sao Paulo) / Argentina (UTC-3)']
  ]],
  ['Oceania', [
    ['AWST-8', 'Australia Western (Perth)'],
    ['ACST-9:30', 'Australia Central (Darwin, no DST)'],
    ['ACST-9:30ACDT,M10.1.0,M4.1.0/3', 'Australia Central (Adelaide)'],
    ['AEST-10', 'Australia Eastern (Brisbane, no DST)'],
    ['AEST-10AEDT,M10.1.0,M4.1.0/3', 'Australia Eastern (Sydney / Melbourne)'],
    ['ChST-10', 'Guam (ChST, UTC+10)'],
    ['NZST-12NZDT,M9.5.0,M4.1.0/3', 'New Zealand (NZST/NZDT)']
  ]]
];

// Fixed whole-hour UTC offsets (UTC-12 .. UTC+14) so that every region is covered
const TZ_FIXED_OFFSETS = [];
for (let h = -12; h <= 14; h++) {
  if (h === 0) continue;
  const abs = String(Math.abs(h)).padStart(2, '0');
  const name = (h > 0 ? '+' : '-') + abs;
  TZ_FIXED_OFFSETS.push(['<' + name + '>' + (-h), 'UTC' + (h > 0 ? '+' : '-') + Math.abs(h)]);
}
// Skip offsets that are already offered by a named region above (keeps option values unique)
const TZ_NAMED_VALUES = new Set([].concat(...TZ_GROUPS.map(group => group[1].map(item => item[0]))));
TZ_GROUPS.push(['Fixed UTC offsets', TZ_FIXED_OFFSETS.filter(item => !TZ_NAMED_VALUES.has(item[0]))]);

// Flat list used for matching a stored value against the presets
const TZ_PRESETS = [].concat(...TZ_GROUPS.map(group => group[1]));
let ntpFormFilled = false;
let lastNtp = { enabled: true, lastSyncEpoch: 0 };

(function initTimezoneSelect() {
  const select = byId('ntp-tz');
  TZ_GROUPS.forEach(group => {
    const optgroup = document.createElement('optgroup');
    optgroup.label = group[0];
    group[1].forEach(item => {
      const option = document.createElement('option');
      option.value = item[0];
      option.textContent = item[1];
      optgroup.appendChild(option);
    });
    select.appendChild(optgroup);
  });
  const custom = document.createElement('option');
  custom.value = 'custom';
  custom.textContent = 'Custom (POSIX TZ string)';
  select.appendChild(custom);
  select.addEventListener('change', () => {
    byId('ntp-tz-custom').hidden = select.value !== 'custom';
  });
})();

function setTimezoneField(tz) {
  const select = byId('ntp-tz');
  const preset = TZ_PRESETS.some(item => item[0] === tz);
  select.value = preset ? tz : 'custom';
  byId('ntp-tz-custom').hidden = preset;
  byId('ntp-tz-custom').value = preset ? '' : tz;
}

function getTimezoneField() {
  const value = byId('ntp-tz').value;
  return value === 'custom' ? byId('ntp-tz-custom').value.trim() : value;
}

function updateNtpFormState() {
  const enabled = byId('ntp-enabled').checked;
  ['ntp-server1', 'ntp-server2'].forEach(id => { byId(id).disabled = !enabled; });
}
byId('ntp-enabled').addEventListener('change', updateNtpFormState);

function renderNtpStatus(n) {
  lastNtp = n;
  showValue('time-now', n.timeValid ? n.now : 'Not set');
  showValue('time-zone', n.timezone);
  let text;
  if (!n.enabled) text = 'Disabled';
  else if (n.synced) text = 'Synchronized';
  else if (!n.wifi) text = 'Waiting for Wi-Fi';
  else text = 'Waiting for a response from the NTP server';
  showValue('time-status', text);
  showValue('time-last', n.lastSync || (n.enabled ? 'Not synchronized since boot' : ''));
}

async function loadNtp() {
  try {
    const n = await apiGet('/api/ntp');
    renderNtpStatus(n);
    byId('ntp-sync').disabled = !n.enabled;
    if (!ntpFormFilled) {
      byId('ntp-enabled').checked = n.enabled;
      byId('ntp-server1').value = n.server1 || '';
      byId('ntp-server2').value = n.server2 || '';
      setTimezoneField(n.timezone || 'UTC0');
      updateNtpFormState();
      ntpFormFilled = true;
    }
  } catch (e) { /* keep the last values */ }
}

byId('ntp-form').addEventListener('submit', async event => {
  event.preventDefault();
  const body = {
    enabled: byId('ntp-enabled').checked,
    server1: byId('ntp-server1').value.trim(),
    server2: byId('ntp-server2').value.trim(),
    timezone: getTimezoneField()
  };
  byId('ntp-submit').disabled = true;
  setStatus('ntp-msg', 'Saving...');
  try {
    const data = await apiPost('/api/ntp', body);
    setStatus('ntp-msg', data.message || 'Saved.');
    loadNtp();
  } catch (e) {
    setStatus('ntp-msg', e.message, true);
  } finally {
    byId('ntp-submit').disabled = false;
  }
});

byId('ntp-sync').addEventListener('click', async () => {
  const button = byId('ntp-sync');
  const before = lastNtp.lastSyncEpoch || 0;
  button.disabled = true;
  setStatus('ntp-sync-msg', 'Requesting synchronization...');
  try {
    await apiPost('/api/ntp/sync');
    let done = false;
    for (let i = 0; i < 20 && !done; i++) {
      await sleep(1000);
      const n = await apiGet('/api/ntp');
      renderNtpStatus(n);
      done = n.synced && n.lastSyncEpoch !== before;
    }
    if (done) setStatus('ntp-sync-msg', 'Synchronized.');
    else setStatus('ntp-sync-msg', 'No response from the NTP server yet. Check the server name, the DNS settings and the Wi-Fi connection.', true);
  } catch (e) {
    setStatus('ntp-sync-msg', e.message, true);
  } finally {
    button.disabled = !lastNtp.enabled;
  }
});

// ---- Account ----
let accountFilled = false;

async function loadAccount() {
  try {
    const a = await apiGet('/api/account');
    byId('account-warning').hidden = a.enabled;
    byId('account-password').placeholder = a.hasPassword ? '(unchanged)' : '';
    if (!accountFilled) {
      byId('account-user').value = a.user || '';
      accountFilled = true;
    }
  } catch (e) { /* keep the last values */ }
}

byId('account-form').addEventListener('submit', async event => {
  event.preventDefault();
  const user = byId('account-user').value;
  const password = byId('account-password').value;
  if (password !== byId('account-password2').value) {
    setStatus('account-msg', 'The passwords do not match.', true);
    return;
  }
  if (!user && !window.confirm('Authentication will be disabled and anyone on the network can operate this device. Continue?')) return;
  const body = { user: user };
  if (password) body.password = password;
  byId('account-submit').disabled = true;
  setStatus('account-msg', 'Saving...');
  try {
    const data = await apiPost('/api/account', body);
    setStatus('account-msg', data.message || 'Saved.');
    byId('account-password').value = '';
    byId('account-password2').value = '';
    setTimeout(() => location.reload(), 1500);  // the browser asks for the new credentials
  } catch (e) {
    setStatus('account-msg', e.message, true);
  } finally {
    byId('account-submit').disabled = false;
  }
});

// Refresh the status tables while their tabs are visible
setInterval(() => {
  if (document.hidden) return;
  if (isTabSelected('tab-network')) loadNetwork();
  if (isTabSelected('tab-time')) loadNtp();
}, 5000);

const otaForm = document.getElementById('ota-form');
otaForm.addEventListener('submit', event => {
  event.preventDefault();
  const fileInput = document.getElementById('firmware-file');
  const file = fileInput.files[0];
  const status = document.getElementById('ota-status');
  const progress = document.getElementById('ota-progress');
  const submit = document.getElementById('ota-submit');
  if (!file) { status.textContent = 'Select a firmware file.'; return; }
  if (!file.name.toLowerCase().endsWith('.bin')) { status.textContent = 'Select a .bin file.'; return; }
  if (!window.confirm('Update the device with the selected firmware. Continue?')) return;
  const request = new XMLHttpRequest();
  const body = new FormData(otaForm);
  submit.disabled = true;
  fileInput.disabled = true;
  progress.classList.remove('danger');
  progress.style.width = '0%';
  status.textContent = 'Uploading firmware...';
  request.upload.addEventListener('progress', event => {
    if (!event.lengthComputable) return;
    const percent = Math.round(event.loaded * 100 / event.total);
    progress.style.width = percent + '%';
    status.textContent = 'Uploading ' + percent + '%';
  });
  request.addEventListener('load', () => {
    let result = {};
    try { result = JSON.parse(request.responseText); } catch (e) {}
    if (request.status >= 200 && request.status < 300) {
      progress.style.width = '100%';
      status.textContent = result.message || 'Update applied. Waiting for the device to restart.';
    } else {
      progress.classList.add('danger');
      status.textContent = result.error || 'Update failed. Check the file and the device state.';
      submit.disabled = false;
      fileInput.disabled = false;
    }
  });
  request.addEventListener('error', () => {
    progress.classList.add('danger');
    status.textContent = 'Communication failed. Check the device state.';
    submit.disabled = false;
    fileInput.disabled = false;
  });
  request.open('POST', '/api/ota');
  request.send(body);
});
</script>
</body>
</html>
)rawhtml";

// =====================
// JSON response helpers
// =====================
static void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

static void sendError(int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(code, doc);
}

// Parses the request body as JSON. Sends a 400 response and returns false on failure.
static bool parseJsonBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) {
    sendError(400, "No request body");
    return false;
  }
  if (deserializeJson(doc, server.arg("plain"))) {
    sendError(400, "Invalid JSON");
    return false;
  }
  return true;
}

static const char* wifiModeName(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_STA:   return "Station";
    case WIFI_MODE_AP:    return "Access Point";
    case WIFI_MODE_APSTA: return "Station + Access Point";
    default:              return "Off";
  }
}

// =====================
// HTTP handlers
// =====================
void handleRoot() {
  if (!checkAuth()) return;
  // send_P() streams the page in chunks instead of copying it into a String
  server.send_P(200, "text/html; charset=utf-8", HTML_PAGE);
}

void handleGetInput() {
  if (!checkAuth()) return;
  String json = "{\"channel\":" + String(currentChannel) + "}";
  server.send(200, "application/json", json);
}

void handleGetDevice() {
  if (!checkAuth()) return;
  const esp_partition_t* app = esp_ota_get_running_partition();

  char timeText[40] = "";
  const time_t nowEpoch = time(nullptr);
  if (nowEpoch >= MIN_VALID_EPOCH) formatLocalTime(nowEpoch, timeText, sizeof(timeText), true);

  char json[768];
  snprintf(json, sizeof(json),
    "{\"version\":\"%s\",\"chip\":\"%s\",\"revision\":%u,\"cores\":%u,\"cpuMHz\":%u,\"ramTotal\":%u,\"ramFree\":%u,\"ramMinFree\":%u,\"psramTotal\":%u,\"psramFree\":%u,\"flashTotal\":%u,\"appLabel\":\"%s\",\"appSize\":%u,\"appFree\":%u,\"fsTotal\":%u,\"fsUsed\":%u,\"time\":\"%s\"}",
    BUILD_NUMBER, ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz(),
    ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getPsramSize(), ESP.getFreePsram(),
    ESP.getFlashChipSize(), app ? app->label : "unknown", ESP.getSketchSize(), ESP.getFreeSketchSpace(),
    LittleFS.totalBytes(), LittleFS.usedBytes(), timeText);
  server.send(200, "application/json", json);
}

void handleGetLog() {
  if (!checkAuth()) return;
  uint32_t since = 0;
  if (server.hasArg("since")) {
    since = (uint32_t)strtoul(server.arg("since").c_str(), nullptr, 10);
  }

  // Single-threaded WebServer, so a static buffer is safe here
  static char chunk[2049];
  const LogReadResult r = Log.read(since, chunk, sizeof(chunk) - 1);
  chunk[r.length] = '\0';

  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Log-Next",  String(r.next));
  server.sendHeader("X-Log-Reset", r.reset ? "1" : "0");
  server.sendHeader("X-Log-Lost",  r.lost  ? "1" : "0");
  server.sendHeader("X-Log-More",  r.more  ? "1" : "0");
  server.send(200, "text/plain; charset=utf-8", chunk);
}

void handlePostInput() {
  if (!checkAuth()) return;
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }
  String body = server.arg("plain");
  int idx = body.indexOf("\"channel\"");
  if (idx < 0) {
    server.send(400, "application/json", "{\"error\":\"missing channel\"}");
    return;
  }
  int colon = body.indexOf(':', idx);
  int ch = body.substring(colon + 1).toInt();
  if (ch < 1 || ch > NUM_CHANNELS) {
    server.send(400, "application/json", "{\"error\":\"invalid channel\"}");
    return;
  }
  pressButton(ch);
  String json = "{\"status\":\"ok\",\"channel\":" + String(ch) + "}";
  server.send(200, "application/json", json);
}

void handleEdidToggle() {
  if (!checkAuth()) return;
  edidToggle();
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"action\":\"edid_toggle\",\"message\":\"EDID ON/OFF toggle done\"}");
}

void handleEdidRefresh() {
  if (!checkAuth()) return;
  edidRefresh();
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"action\":\"edid_refresh\",\"message\":\"EDID refresh done\"}");
}

void handleFactoryReset() {
  if (!checkAuth()) return;
  factoryReset();
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"action\":\"factory_reset\",\"message\":\"Factory reset done\"}");
}

// ---------------------
// Network: status
// ---------------------
void handleGetNetwork() {
  if (!checkAuth()) return;
  JsonDocument doc;
  const bool connected = (WiFi.status() == WL_CONNECTED);

  doc["mode"]      = wifiModeName(WiFi.getMode());
  doc["connected"] = connected;
  doc["ssid"]      = connected ? WiFi.SSID() : String();
  doc["rssi"]      = connected ? (int)WiFi.RSSI() : 0;
  doc["mac"]       = WiFi.macAddress();
  doc["hostname"]  = HOSTNAME;
  doc["mdns"]      = String(HOSTNAME) + "." + MDNS_DOMAIN;
  doc["ip"]        = connected ? WiFi.localIP().toString() : String();
  doc["subnet"]    = connected ? WiFi.subnetMask().toString() : String();
  doc["gateway"]   = connected ? WiFi.gatewayIP().toString() : String();
  doc["dns1"]      = connected ? WiFi.dnsIP(0).toString() : String();
  doc["dns2"]      = connected ? WiFi.dnsIP(1).toString() : String();

  JsonObject ap = doc["ap"].to<JsonObject>();
  ap["active"] = setupApActive;
  if (setupApActive) {
    ap["ssid"]    = setupApSsid;
    ap["ip"]      = WiFi.softAPIP().toString();
    ap["clients"] = (int)WiFi.softAPgetStationNum();
  }

  // Saved settings (passwords are never returned)
  JsonObject cfg = doc["config"].to<JsonObject>();
  cfg["wifi_ssid"]         = WIFI_SSID;
  cfg["wifi_has_password"] = (WIFI_PASSWORD[0] != '\0');
  cfg["ip_mode"]           = IP_STATIC ? "static" : "dhcp";
  cfg["ip"]                = IP_ADDR;
  cfg["subnet"]            = IP_SUBNET;
  cfg["gateway"]           = IP_GATEWAY;
  cfg["dns_mode"]          = DNS_MANUAL ? "manual" : "auto";
  cfg["dns1"]              = IP_DNS1;
  cfg["dns2"]              = IP_DNS2;

  sendJson(200, doc);
}

// ---------------------
// Network: Wi-Fi scan (asynchronous)
//   GET /api/wifi/scan?start=1 : start a new scan
//   GET /api/wifi/scan         : poll; "scanning" is false once results are ready
// ---------------------
void handleGetWifiScan() {
  if (!checkAuth()) return;
  JsonDocument doc;

  int count = WiFi.scanComplete();
  if (server.hasArg("start") && count != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);  // async
    count = WIFI_SCAN_RUNNING;
  }

  if (count == WIFI_SCAN_RUNNING) {
    doc["scanning"] = true;
    doc["networks"].to<JsonArray>();
    sendJson(200, doc);
    return;
  }

  doc["scanning"] = false;
  JsonArray networks = doc["networks"].to<JsonArray>();
  if (count > 0) {
    // Keep one entry per SSID (the strongest), skip hidden networks
    static const int MAX_NETWORKS = 24;
    int keep[MAX_NETWORKS];
    int keepCount = 0;
    for (int i = 0; i < count; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      int duplicate = -1;
      for (int k = 0; k < keepCount; k++) {
        if (WiFi.SSID(keep[k]) == ssid) { duplicate = k; break; }
      }
      if (duplicate >= 0) {
        if (WiFi.RSSI(i) > WiFi.RSSI(keep[duplicate])) keep[duplicate] = i;
      } else if (keepCount < MAX_NETWORKS) {
        keep[keepCount++] = i;
      }
    }
    for (int k = 0; k < keepCount; k++) {
      JsonObject entry = networks.add<JsonObject>();
      entry["ssid"]   = WiFi.SSID(keep[k]);
      entry["rssi"]   = (int)WiFi.RSSI(keep[k]);
      entry["secure"] = (WiFi.encryptionType(keep[k]) != WIFI_AUTH_OPEN);
    }
  }
  WiFi.scanDelete();
  sendJson(200, doc);
}

// ---------------------
// Network: save Wi-Fi settings (SSID / password) and restart
// "password" omitted: keep the saved password. "password" empty: open network.
// ---------------------
void handlePostWifi() {
  if (!checkAuth()) return;
  JsonDocument doc;
  if (!parseJsonBody(doc)) return;

  const char* ssid = doc["ssid"] | "";
  const bool hasPasswordField = !doc["password"].isNull();
  const char* password = doc["password"] | "";

  const size_t ssidLength = strlen(ssid);
  if (ssidLength == 0 || ssidLength > 32) {
    sendError(400, "SSID must be 1 to 32 bytes");
    return;
  }
  if (hasPasswordField) {
    const size_t passwordLength = strlen(password);
    if (passwordLength != 0 && (passwordLength < 8 || passwordLength > 63)) {
      sendError(400, "Wi-Fi password must be 8 to 63 characters (empty for an open network)");
      return;
    }
  } else if (strcmp(ssid, WIFI_SSID) != 0) {
    sendError(400, "Enter the password for the new network");
    return;
  }

  char oldSsid[sizeof(WIFI_SSID)];
  char oldPassword[sizeof(WIFI_PASSWORD)];
  memcpy(oldSsid, WIFI_SSID, sizeof(oldSsid));
  memcpy(oldPassword, WIFI_PASSWORD, sizeof(oldPassword));

  snprintf(WIFI_SSID, sizeof(WIFI_SSID), "%s", ssid);
  if (hasPasswordField) snprintf(WIFI_PASSWORD, sizeof(WIFI_PASSWORD), "%s", password);

  if (!saveConfig()) {
    memcpy(WIFI_SSID, oldSsid, sizeof(oldSsid));
    memcpy(WIFI_PASSWORD, oldPassword, sizeof(oldPassword));
    sendError(500, "Failed to save the settings");
    return;
  }
  Log.printf("Wi-Fi settings saved (SSID: %s). Restarting...\n", WIFI_SSID);

  char message[200];
  snprintf(message, sizeof(message),
           "Saved. The device is restarting. Reconnect to your network, then open http://%s.%s/ (or check your router for the IP address).",
           HOSTNAME, MDNS_DOMAIN);
  JsonDocument res;
  res["status"]  = "ok";
  res["message"] = message;
  sendJson(200, res);
  scheduleRestart(1000);
}

// ---------------------
// Network: save IP address / DNS settings and restart
// ---------------------
void handlePostIp() {
  if (!checkAuth()) return;
  JsonDocument doc;
  if (!parseJsonBody(doc)) return;

  const char* mode = doc["mode"] | "dhcp";
  const char* dnsMode = doc["dns_mode"] | "auto";
  const char* ip = doc["ip"] | "";
  const char* subnet = doc["subnet"] | "";
  const char* gateway = doc["gateway"] | "";
  const char* dns1 = doc["dns1"] | "";
  const char* dns2 = doc["dns2"] | "";

  if (strcmp(mode, "static") != 0 && strcmp(mode, "dhcp") != 0) {
    sendError(400, "Invalid IP address mode");
    return;
  }
  if (strcmp(dnsMode, "manual") != 0 && strcmp(dnsMode, "auto") != 0) {
    sendError(400, "Invalid DNS mode");
    return;
  }
  const bool useStatic = (strcmp(mode, "static") == 0);
  const bool useManualDns = (strcmp(dnsMode, "manual") == 0);

  const char* problem = nullptr;
  if (useStatic) {
    problem = validateStaticIp(ip, subnet, gateway);
    if (problem == nullptr && !useManualDns) problem = "A static IP address requires manual DNS servers";
  }
  if (problem == nullptr && useManualDns) {
    problem = validateDnsServer(dns1, true);
    if (problem == nullptr) problem = validateDnsServer(dns2, false);
  }
  if (problem != nullptr) {
    sendError(400, problem);
    return;
  }

  // Keep a copy so that the old settings can be restored if saving fails
  const bool oldStatic = IP_STATIC;
  const bool oldManualDns = DNS_MANUAL;
  char oldIp[sizeof(IP_ADDR)], oldSubnet[sizeof(IP_SUBNET)], oldGateway[sizeof(IP_GATEWAY)];
  char oldDns1[sizeof(IP_DNS1)], oldDns2[sizeof(IP_DNS2)];
  memcpy(oldIp, IP_ADDR, sizeof(oldIp));
  memcpy(oldSubnet, IP_SUBNET, sizeof(oldSubnet));
  memcpy(oldGateway, IP_GATEWAY, sizeof(oldGateway));
  memcpy(oldDns1, IP_DNS1, sizeof(oldDns1));
  memcpy(oldDns2, IP_DNS2, sizeof(oldDns2));

  IP_STATIC = useStatic;
  DNS_MANUAL = useManualDns;
  snprintf(IP_ADDR, sizeof(IP_ADDR), "%s", useStatic ? ip : "");
  snprintf(IP_SUBNET, sizeof(IP_SUBNET), "%s", useStatic ? subnet : "");
  snprintf(IP_GATEWAY, sizeof(IP_GATEWAY), "%s", useStatic ? gateway : "");
  snprintf(IP_DNS1, sizeof(IP_DNS1), "%s", useManualDns ? dns1 : "");
  snprintf(IP_DNS2, sizeof(IP_DNS2), "%s", useManualDns ? dns2 : "");

  if (!saveConfig()) {
    IP_STATIC = oldStatic;
    DNS_MANUAL = oldManualDns;
    memcpy(IP_ADDR, oldIp, sizeof(oldIp));
    memcpy(IP_SUBNET, oldSubnet, sizeof(oldSubnet));
    memcpy(IP_GATEWAY, oldGateway, sizeof(oldGateway));
    memcpy(IP_DNS1, oldDns1, sizeof(oldDns1));
    memcpy(IP_DNS2, oldDns2, sizeof(oldDns2));
    sendError(500, "Failed to save the settings");
    return;
  }
  Log.printf("IP settings saved (%s, DNS %s). Restarting...\n", useStatic ? "static" : "DHCP", useManualDns ? "manual" : "auto");

  char message[200];
  if (useStatic) {
    snprintf(message, sizeof(message), "Saved. The device is restarting. Open http://%s/ once it is back.", IP_ADDR);
  } else {
    snprintf(message, sizeof(message), "Saved. The device is restarting.");
  }
  JsonDocument res;
  res["status"]  = "ok";
  res["message"] = message;
  sendJson(200, res);
  scheduleRestart(1000);
}

// ---------------------
// NTP / time: status
// ---------------------
void handleGetNtp() {
  if (!checkAuth()) return;
  JsonDocument doc;
  char nowText[40];
  char lastText[40];

  doc["enabled"]  = NTP_ENABLED;
  doc["server1"]  = NTP_SERVER1;
  doc["server2"]  = NTP_SERVER2;
  doc["timezone"] = TZ_STRING;
  doc["wifi"]     = (WiFi.status() == WL_CONNECTED);
  doc["running"]  = (bool)esp_sntp_enabled();

  const time_t now = time(nullptr);
  const bool valid = (now >= MIN_VALID_EPOCH);
  doc["timeValid"] = valid;
  doc["now"] = (valid && formatLocalTime(now, nowText, sizeof(nowText), true)) ? nowText : "";

  const time_t last = ntpLastSyncEpoch;
  doc["synced"] = (bool)ntpEverSynced;
  doc["lastSyncEpoch"] = (uint32_t)last;
  doc["lastSync"] = (ntpEverSynced && formatLocalTime(last, lastText, sizeof(lastText), true)) ? lastText : "";

  sendJson(200, doc);
}

// ---------------------
// NTP / time: save settings and apply them immediately (no restart)
// Keys that are omitted keep their current value.
// ---------------------
void handlePostNtp() {
  if (!checkAuth()) return;
  JsonDocument doc;
  if (!parseJsonBody(doc)) return;

  const bool enabled = doc["enabled"] | NTP_ENABLED;
  const char* server1 = doc["server1"] | NTP_SERVER1;
  const char* server2 = doc["server2"] | NTP_SERVER2;
  const char* timezone = doc["timezone"] | TZ_STRING;

  if (enabled && !isValidHostName(server1)) {
    sendError(400, "Enter a valid primary NTP server (letters, digits, '-' and '.')");
    return;
  }
  if (!enabled && server1[0] != '\0' && !isValidHostName(server1)) {
    sendError(400, "Invalid primary NTP server");
    return;
  }
  if (server2[0] != '\0' && !isValidHostName(server2)) {
    sendError(400, "Invalid secondary NTP server");
    return;
  }
  if (!isValidTimezone(timezone)) {
    sendError(400, "Invalid time zone (use a POSIX TZ string such as JST-9)");
    return;
  }

  const bool oldEnabled = NTP_ENABLED;
  char oldServer1[sizeof(NTP_SERVER1)], oldServer2[sizeof(NTP_SERVER2)], oldTimezone[sizeof(TZ_STRING)];
  memcpy(oldServer1, NTP_SERVER1, sizeof(oldServer1));
  memcpy(oldServer2, NTP_SERVER2, sizeof(oldServer2));
  memcpy(oldTimezone, TZ_STRING, sizeof(oldTimezone));

  NTP_ENABLED = enabled;
  snprintf(NTP_SERVER1, sizeof(NTP_SERVER1), "%s", server1);
  snprintf(NTP_SERVER2, sizeof(NTP_SERVER2), "%s", server2);
  snprintf(TZ_STRING, sizeof(TZ_STRING), "%s", timezone);

  if (!saveConfig()) {
    NTP_ENABLED = oldEnabled;
    memcpy(NTP_SERVER1, oldServer1, sizeof(oldServer1));
    memcpy(NTP_SERVER2, oldServer2, sizeof(oldServer2));
    memcpy(TZ_STRING, oldTimezone, sizeof(oldTimezone));
    sendError(500, "Failed to save the settings");
    return;
  }

  ntpEverSynced = false;  // report the status of the new configuration
  ntpLastSyncEpoch = 0;
  applyNtp();

  JsonDocument res;
  res["status"]  = "ok";
  res["message"] = "NTP settings saved and applied.";
  sendJson(200, res);
}

// ---------------------
// NTP / time: synchronize now
// ---------------------
void handleNtpSync() {
  if (!checkAuth()) return;
  if (!NTP_ENABLED) {
    sendError(409, "NTP is disabled");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendError(409, "Wi-Fi is not connected");
    return;
  }
  Log.println("NTP synchronization requested from the Web UI");
  applyNtp();  // restarting SNTP triggers a new request
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"NTP synchronization requested\"}");
}

// ---------------------
// Account (Web UI BASIC authentication)
// ---------------------
void handleGetAccount() {
  if (!checkAuth()) return;
  JsonDocument doc;
  doc["enabled"]     = isAuthEnabled();
  doc["user"]        = AUTH_USER;
  doc["hasPassword"] = (AUTH_PASSWORD[0] != '\0');
  sendJson(200, doc);
}

// "user" empty: disable authentication.
// "password" omitted or empty: keep the saved password.
void handlePostAccount() {
  if (!checkAuth()) return;
  JsonDocument doc;
  if (!parseJsonBody(doc)) return;

  const char* user = doc["user"] | "";
  const char* password = doc["password"] | "";
  const size_t userLength = strlen(user);
  const size_t passwordLength = strlen(password);

  if (userLength > 0) {
    if (userLength >= sizeof(AUTH_USER)) {
      sendError(400, "User name must be 62 characters or less");
      return;
    }
    // BASIC authentication cannot carry ':' in the user name; keep both fields plain ASCII
    for (size_t i = 0; i < userLength; i++) {
      const unsigned char c = (unsigned char)user[i];
      if (c <= 0x20 || c >= 0x7F || c == ':') {
        sendError(400, "User name must be printable ASCII without spaces or ':'");
        return;
      }
    }
    if (passwordLength > 0) {
      if (passwordLength < 8 || passwordLength >= sizeof(AUTH_PASSWORD)) {
        sendError(400, "Password must be 8 to 255 characters");
        return;
      }
      for (size_t i = 0; i < passwordLength; i++) {
        const unsigned char c = (unsigned char)password[i];
        if (c < 0x20 || c >= 0x7F) {
          sendError(400, "Password must be printable ASCII");
          return;
        }
      }
    } else if (AUTH_PASSWORD[0] == '\0') {
      sendError(400, "Enter a password");
      return;
    }
  }

  char oldUser[sizeof(AUTH_USER)];
  char oldPassword[sizeof(AUTH_PASSWORD)];
  memcpy(oldUser, AUTH_USER, sizeof(oldUser));
  memcpy(oldPassword, AUTH_PASSWORD, sizeof(oldPassword));

  if (userLength == 0) {
    AUTH_USER[0] = '\0';
    AUTH_PASSWORD[0] = '\0';
  } else {
    snprintf(AUTH_USER, sizeof(AUTH_USER), "%s", user);
    if (passwordLength > 0) snprintf(AUTH_PASSWORD, sizeof(AUTH_PASSWORD), "%s", password);
  }

  if (!saveConfig()) {
    memcpy(AUTH_USER, oldUser, sizeof(oldUser));
    memcpy(AUTH_PASSWORD, oldPassword, sizeof(oldPassword));
    sendError(500, "Failed to save the settings");
    return;
  }
  Log.println(isAuthEnabled() ? "Web UI account updated" : "Web UI authentication disabled");

  JsonDocument res;
  res["status"]  = "ok";
  res["enabled"] = isAuthEnabled();
  res["message"] = isAuthEnabled() ? "Account saved. Sign in again with the new credentials."
                                   : "Authentication disabled.";
  sendJson(200, res);
}

void handleOtaUpload() {
  if (!checkAuth()) {
    otaUploadAuthorized = false;
    otaUploadFailed = true;
    return;
  }

  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadAuthorized = true;
    otaUploadFailed = false;
    otaRestartPending = false;
    if (!upload.filename.endsWith(".bin")) {
      otaUploadFailed = true;
      Log.println("OTA rejected: firmware filename must end in .bin");
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaUploadFailed = true;
      Update.printError(Log);
      return;
    }
    Log.printf("OTA upload started: %s\n", upload.filename.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE && otaUploadAuthorized && !otaUploadFailed) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaUploadFailed = true;
      Update.printError(Log);
    }
  } else if (upload.status == UPLOAD_FILE_END && otaUploadAuthorized && !otaUploadFailed) {
    if (!Update.end(true)) {
      otaUploadFailed = true;
      Update.printError(Log);
    } else {
      Log.printf("OTA upload complete: %u bytes\n", upload.totalSize);
      otaRestartPending = true;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaUploadFailed = true;
    Log.println("OTA upload aborted");
  }
}

void handleOtaComplete() {
  if (!checkAuth()) return;
  if (otaUploadFailed || !otaUploadAuthorized || !otaRestartPending) {
    Update.abort();
    otaUploadAuthorized = false;
    otaRestartPending = false;
    server.send(400, "application/json", "{\"error\":\"Update failed. Check the file format, firmware, or OTA partition.\"}");
    return;
  }

  otaUploadAuthorized = false;
  otaRestartPending = false;
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Update complete. Restarting the device.\"}");
  delay(500);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// =====================
// Setup
// =====================
void setup() {
  Serial.begin(115200);
  Log.begin();  // start capturing console output for the Web UI

  // Load the settings file (missing keys use defaults)
  configLoaded = loadConfig();
  if (!configLoaded) {
    Log.println("Settings are missing or invalid. Defaults are used and the setup AP will be started.");
  }
  applyTimezone();  // set the time zone early so that log timestamps use local time

  // Initialize relay pins
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }

  // Register the Wi-Fi event handler (before connecting)
  WiFi.onEvent(onWiFiEvent);
  sntp_set_time_sync_notification_cb(onNtpSynced);

  // Routing
  server.on("/",                  HTTP_GET,  handleRoot);
  server.on("/api/input",         HTTP_GET,  handleGetInput);
  server.on("/api/device",        HTTP_GET,  handleGetDevice);
  server.on("/api/log",           HTTP_GET,  handleGetLog);
  server.on("/api/input",         HTTP_POST, handlePostInput);
  server.on("/api/edid/toggle",   HTTP_POST, handleEdidToggle);
  server.on("/api/edid/refresh",  HTTP_POST, handleEdidRefresh);
  server.on("/api/edid/factory",  HTTP_POST, handleFactoryReset);
  server.on("/api/network",       HTTP_GET,  handleGetNetwork);
  server.on("/api/wifi/scan",     HTTP_GET,  handleGetWifiScan);
  server.on("/api/wifi",          HTTP_POST, handlePostWifi);
  server.on("/api/ip",            HTTP_POST, handlePostIp);
  server.on("/api/ntp",           HTTP_GET,  handleGetNtp);
  server.on("/api/ntp",           HTTP_POST, handlePostNtp);
  server.on("/api/ntp/sync",      HTTP_POST, handleNtpSync);
  server.on("/api/account",       HTTP_GET,  handleGetAccount);
  server.on("/api/account",       HTTP_POST, handlePostAccount);
  server.on("/api/ota", HTTP_POST, handleOtaComplete, handleOtaUpload);
  server.onNotFound(handleNotFound);

  // Wi-Fi: connect as a station, or fall back to the setup AP
  if (staEnabled()) {
    if (connectWiFi()) {
      startMDNS();
    } else {
      Log.println("Starting the setup AP so that the Wi-Fi settings can be corrected");
      startSetupAP();
    }
  } else {
    Log.println("Wi-Fi is not configured. Starting the setup AP");
    startSetupAP();
  }

  applyNtp();  // start SNTP (after the network stack has been initialized by WiFi.mode())

  server.begin();
  Log.println("Web server started");
}

// =====================
// Wi-Fi supervision (called from loop())
//   1) immediate flag from onWiFiEvent
//   2) periodic polling of WiFi.status()
//   3) a forced IP check every 10 s, for cases like a router reboot where
//      status() itself is not updated
// Any of them triggers a reconnect. Waiting and retrying never blocks on
// delay(), so the Web UI keeps responding.
// While the setup AP has clients, reconnect attempts are suspended because
// scanning for the router would disturb those clients.
// =====================
static void superviseWiFi(unsigned long now) {
  const bool connected = (WiFi.status() == WL_CONNECTED);
  const bool ipLost = (WiFi.localIP() == IPAddress(0, 0, 0, 0));
  const bool periodicHit = (now - lastWifiCheckMs > WIFI_CHECK_INTERVAL_MS);
  if (periodicHit) lastWifiCheckMs = now;

  // Measure how long the station has been down
  if (connected) {
    staDownTracking = false;
  } else if (!staDownTracking) {
    staDownTracking = true;
    staDownSinceMs = now;
  }

  // Keep the manual DNS override alive while DHCP is in use
  if (connected && (dnsReapplyNeeded || periodicHit)) {
    dnsReapplyNeeded = false;
    ensureManualDns();
  }

  const bool apHasClients = setupApActive && WiFi.softAPgetStationNum() > 0;
  const bool needReconnect = !connected || wifiDisconnectedEvent || (periodicHit && ipLost);
  const unsigned long retryInterval = setupApActive ? WIFI_RETRY_INTERVAL_AP_MS : WIFI_RETRY_INTERVAL_MS;

  if (needReconnect && !apHasClients) {
    // Non-blocking: only retry after the retry interval has elapsed
    if (now - lastReconnectAttemptMs > retryInterval || lastReconnectAttemptMs == 0) {
      lastReconnectAttemptMs = now;
      wifiDisconnectedEvent = false;

      Log.println("Wi-Fi disconnect detected, reconnecting...");
      if (connectWiFi()) {
        startMDNS();  // restart mDNS as well
        Log.println("Reconnected");
      } else {
        Log.println("Reconnect failed, continuing until next retry");
      }
    }
  }

  // Offer the setup AP when the connection stays down for a long time
  if (!setupApActive && staDownTracking && now - staDownSinceMs > AP_FALLBACK_AFTER_MS) {
    Log.println("Wi-Fi has been down for a long time. Starting the setup AP");
    startSetupAP();
  }

  // Stop the setup AP once the station is back and nobody is using it
  if (setupApActive && connected && !apHasClients) {
    stopSetupAP();
  }
}

// =====================
// Main loop
// =====================
void loop() {
  const unsigned long now = millis();

  if (restartPending && (long)(now - restartAtMs) >= 0) {
    Log.println("Restarting the device...");
    delay(50);
    ESP.restart();
  }

  if (ntpSyncLogPending) {
    ntpSyncLogPending = false;
    Log.println("NTP: time synchronized");
  }

  if (staEnabled()) {
    superviseWiFi(now);
  }

  server.handleClient();
}