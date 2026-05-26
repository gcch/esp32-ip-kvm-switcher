/*
 * KCEVE KVM スイッチャー (KCEVE KC-SHM4 / AIMOS AM-SHM4 互換) for Speed Studio XIAO ESP32-C6
 *
 * Copyright (c) 2026 gcch.
 *
 */

#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_wifi.h"   // ★ 追加: esp_wifi_set_ps() のため

// =====================
// 定数
// =====================
static char HOSTNAME[64];
static char MDNS_DOMAIN[254];

static char WIFI_SSID[32];
static char WIFI_PASSWORD[255];

// BASIC認証 (空文字列にすると認証無効)
static char AUTH_USER[63];
static char AUTH_PASSWORD[256];

// =====================
// GPIO ピン定義
// =====================
const int RELAY_PINS[4] = {0, 1, 2, 21};  // D0, D1, D2, D3
const int NUM_CHANNELS  = 4;

// =====================
// タイミング定数 (ms)
// =====================
const int PULSE_MS          = 300;   // 通常ボタン押下
const int EDID_TOGGLE_MS    = 3300;  // PC1長押し: EDID ON/OFFトグル
const int FACTORY_RESET_MS  = 11000; // PC1長押し: デフォルト設定初期化
const int EDID_REFRESH_MS   = 3300;  // PC2長押し: EDIDリセット（再読込）

// =====================
// グローバル変数
// =====================
WebServer server(80);
int currentChannel = 1;

// =====================
// 設定ファイル読み込み
// AUTH_USER が空文字列の場合は認証スキップ
// =====================
void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }

  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    Serial.println("コンフィグファイルが見つかりません");
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println("JSON parse error");
    return;
  }

  strcpy(HOSTNAME,      doc["hostname"].as<const char*>());
  strcpy(MDNS_DOMAIN,   doc["mdns_domain"].as<const char*>());
  strcpy(WIFI_SSID,     doc["wifi_ssid"].as<const char*>());
  strcpy(WIFI_PASSWORD, doc["wifi_password"].as<const char*>());
  strcpy(AUTH_USER,     doc["auth_user"].as<const char*>());
  strcpy(AUTH_PASSWORD, doc["auth_password"].as<const char*>());

  Serial.println("コンフィグが読み込まれました");
  Serial.printf("HOSTNAME: %s\n", HOSTNAME);
  Serial.printf("MDNS_DOMAIN: %s\n", MDNS_DOMAIN);
  Serial.printf("WIFI_SSID: %s\n", WIFI_SSID);
  //Serial.printf("WIFI_PASSWORD: %s\n", WIFI_PASSWORD);
  //Serial.printf("AUTH_USER: %s\n", AUTH_USER);
  //Serial.printf("AUTH_PASSWORD: %s\n", AUTH_PASSWORD);

}

// =====================
// BASIC認証チェック
// AUTH_USER が空文字列の場合は認証スキップ
// =====================
bool checkAuth() {
  if (strlen(AUTH_USER) == 0) return true;
  if (!server.authenticate(AUTH_USER, AUTH_PASSWORD)) {
    server.requestAuthentication(BASIC_AUTH, "KVM Switcher");
    return false;
  }
  return true;
}

// =====================
// 低レベルリレー操作 (非ブロッキング版)
// ★ delay() の代わりに millis() ループで待機し、
//   その間も server.handleClient() を呼び続けることで
//   EDID操作中のネットワーク断を防ぐ
// =====================
void holdRelay(int pin, int ms) {
  digitalWrite(pin, HIGH);
  unsigned long start = millis();
  while (millis() - start < (unsigned long)ms) {
    server.handleClient();  // ★ ネットワーク処理を継続
    delay(10);
  }
  digitalWrite(pin, LOW);
}

// =====================
// 入力切替
// =====================
void pressButton(int channel) {
  if (channel < 1 || channel > NUM_CHANNELS) return;
  holdRelay(RELAY_PINS[channel - 1], PULSE_MS);
  currentChannel = channel;
  Serial.printf("入力 %d に切替\n", channel);
}

// =====================
// EDID操作
// =====================

// PC1を3秒長押し: EDID ON/OFFトグル
void edidToggle() {
  Serial.println("EDID トグル開始 (PC1 3秒)");
  holdRelay(RELAY_PINS[0], EDID_TOGGLE_MS);
  Serial.println("EDID トグル完了");
}

// PC1を10秒長押し: デフォルト設定初期化
void factoryReset() {
  Serial.println("デフォルト設定初期化開始 (PC1 10秒)");
  holdRelay(RELAY_PINS[0], FACTORY_RESET_MS);
  Serial.println("デフォルト設定初期化完了");
}

// PC2を3秒長押し: EDIDリセット（再読込）
void edidRefresh() {
  Serial.println("EDID リセット開始 (PC2 3秒)");
  holdRelay(RELAY_PINS[1], EDID_REFRESH_MS);
  Serial.println("EDID リセット完了");
}

// =====================
// Wi-Fi 接続 + 省電力モード無効化
// ★ setup() と自動再接続の両方から呼ぶ共通関数
// =====================
bool connectWiFi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Wi-Fi (%s) 接続中", WIFI_SSID);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    Serial.print('.');
    delay(1000);
    retry++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi 接続失敗");
    return false;
  }

  // ★ 省電力モード無効化 (Modem Sleep によるPing不達防止)
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.printf("アクセスページ: http://%s/\n", WiFi.localIP().toString().c_str());
  return true;
}

// =====================
// mDNS 起動 (再接続後の再起動にも対応)
// =====================
void startMDNS() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS起動: http://%s.%s/\n", HOSTNAME, MDNS_DOMAIN);
  } else {
    Serial.println("mDNS起動失敗");
  }
}

// =====================
// Web UI HTML
// =====================
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>KVM Switcher</title>
<style>
  :root {
    --bg: #f5f7fa;
    --surface: #ffffff;
    --border: #dde1ea;
    --accent: #2563eb;
    --accent2: #7c3aed;
    --text: #1e293b;
    --text-dim: #64748b;
    --inactive: #f1f5f9;
    --warn: #d97706;
    --danger: #dc2626;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'SF Mono', 'Fira Code', monospace;
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 20px;
  }
  .container { width: 100%; max-width: 480px; }
  .header { text-align: center; margin-bottom: 40px; }
  .header h1 {
    font-size: 13px;
    letter-spacing: 0.3em;
    color: var(--text-dim);
    text-transform: uppercase;
    margin-bottom: 8px;
  }
  .header .title {
    font-size: 28px;
    font-weight: 700;
    background: linear-gradient(135deg, var(--accent), var(--accent2));
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    background-clip: text;
    letter-spacing: -0.02em;
  }
  .status-bar {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 16px;
    margin-bottom: 24px;
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 12px;
    color: var(--text-dim);
  }
  .status-dot {
    width: 6px; height: 6px;
    border-radius: 50%;
    background: #16a34a;
    box-shadow: 0 0 8px #16a34a88;
    flex-shrink: 0;
  }
  .current-label { margin-left: auto; color: var(--accent); font-weight: 600; }
  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
    margin-bottom: 24px;
  }
  .btn {
    background: var(--inactive);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 28px 16px;
    cursor: pointer;
    text-align: center;
    transition: all 0.15s ease;
    position: relative;
    overflow: hidden;
  }
  .btn:hover { border-color: var(--accent); background: rgba(37,99,235,0.05); transform: translateY(-2px); }
  .btn:active { transform: translateY(0); }
  .btn.active {
    background: rgba(37,99,235,0.07);
    border-color: var(--accent);
    box-shadow: 0 0 0 3px rgba(37,99,235,0.12);
  }
  .btn.active::before {
    content: '';
    position: absolute;
    top: 0; left: 0; right: 0;
    height: 2px;
    background: linear-gradient(90deg, var(--accent), var(--accent2));
  }
  .btn-num { font-size: 36px; font-weight: 700; color: var(--text-dim); line-height: 1; margin-bottom: 6px; transition: color 0.15s; }
  .btn.active .btn-num { color: var(--accent); }
  .btn-label { font-size: 11px; letter-spacing: 0.1em; color: var(--text-dim); text-transform: uppercase; }
  .btn.active .btn-label { color: var(--text); }
  .btn-indicator {
    position: absolute; top: 10px; right: 10px;
    width: 6px; height: 6px;
    border-radius: 50%;
    background: var(--border);
    transition: all 0.15s;
  }
  .btn.active .btn-indicator { background: var(--accent); box-shadow: 0 0 6px rgba(37,99,235,0.5); }

  .section-label {
    font-size: 11px;
    letter-spacing: 0.2em;
    color: var(--text-dim);
    text-transform: uppercase;
    margin-bottom: 10px;
  }
  .edid-grid {
    display: grid;
    grid-template-columns: 1fr 1fr 1fr;
    gap: 8px;
    margin-bottom: 12px;
  }
  .edid-btn {
    background: transparent;
    border: 1px solid #f59e0b44;
    border-radius: 10px;
    padding: 12px 8px;
    cursor: pointer;
    color: var(--warn);
    font-family: inherit;
    font-size: 10px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    transition: all 0.15s ease;
    text-align: center;
    line-height: 1.6;
  }
  .edid-btn:hover { background: rgba(245,158,11,0.08); border-color: var(--warn); }
  .edid-btn.danger { color: var(--danger); border-color: #ef444444; }
  .edid-btn.danger:hover { background: rgba(239,68,68,0.08); border-color: var(--danger); }
  .edid-btn:disabled { opacity: 0.35; cursor: not-allowed; }
  .edid-btn.running { border-color: var(--warn); background: rgba(245,158,11,0.1); }
  .edid-btn.running.danger { border-color: var(--danger); background: rgba(239,68,68,0.1); }

  .progress-wrap { margin-bottom: 20px; }
  .progress-label { font-size: 11px; color: var(--text-dim); margin-bottom: 6px; min-height: 16px; }
  .progress-bar {
    height: 2px;
    background: var(--border);
    border-radius: 1px;
    overflow: hidden;
    opacity: 0;
    transition: opacity 0.2s;
  }
  .progress-bar.active { opacity: 1; }
  .progress-fill { height: 100%; background: var(--warn); width: 0%; }
  .progress-fill.danger { background: var(--danger); }

  .api-section {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 16px;
    font-size: 11px;
    color: var(--text-dim);
  }
  .api-section .api-title {
    color: var(--text);
    margin-bottom: 8px;
    font-size: 12px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
  }
  .api-endpoint { color: var(--accent); font-size: 11px; margin-top: 3px; }

  #toast {
    position: fixed;
    bottom: 24px; left: 50%;
    transform: translateX(-50%) translateY(80px);
    background: #1e293b;
    border: 1px solid #334155;
    color: #f1f5f9;
    padding: 10px 20px;
    border-radius: 8px;
    font-size: 12px;
    transition: transform 0.3s ease;
    pointer-events: none;
    white-space: nowrap;
  }
  #toast.show { transform: translateX(-50%) translateY(0); }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="title">KVM INPUT SELECTOR</div>
  </div>

  <div class="status-bar">
    <div class="status-dot"></div>
    <span>ONLINE</span>
    <span class="current-label" id="current-label">INPUT —</span>
  </div>

  <div class="grid">
    <button class="btn" onclick="switchInput(1)" id="btn1">
      <div class="btn-indicator"></div>
      <div class="btn-num">1</div>
      <div class="btn-label">Input</div>
    </button>
    <button class="btn" onclick="switchInput(2)" id="btn2">
      <div class="btn-indicator"></div>
      <div class="btn-num">2</div>
      <div class="btn-label">Input</div>
    </button>
    <button class="btn" onclick="switchInput(3)" id="btn3">
      <div class="btn-indicator"></div>
      <div class="btn-num">3</div>
      <div class="btn-label">Input</div>
    </button>
    <button class="btn" onclick="switchInput(4)" id="btn4">
      <div class="btn-indicator"></div>
      <div class="btn-num">4</div>
      <div class="btn-label">Input</div>
    </button>
  </div>

  <div class="section-label">EDID</div>
  <div class="edid-grid">
    <button class="edid-btn" id="ebtn-toggle" onclick="triggerEdid('toggle')">
      ⟳<br>EDID<br>ON/OFF
    </button>
    <button class="edid-btn" id="ebtn-refresh" onclick="triggerEdid('refresh')">
      ↺<br>EDID<br>REFRESH
    </button>
    <button class="edid-btn danger" id="ebtn-factory" onclick="triggerEdid('factory')">
      ✕<br>FACTORY<br>RESET
    </button>
  </div>

  <div class="progress-wrap">
    <div class="progress-label" id="progress-label"></div>
    <div class="progress-bar" id="progress-bar">
      <div class="progress-fill" id="progress-fill"></div>
    </div>
  </div>

  <div class="api-section">
    <div class="api-title">REST API</div>
    <div class="api-endpoint">POST /api/input         {"channel": 1}</div>
    <div class="api-endpoint">GET  /api/input</div>
    <div class="api-endpoint">POST /api/edid/toggle   EDID ON/OFF</div>
    <div class="api-endpoint">POST /api/edid/refresh  EDID リセット</div>
    <div class="api-endpoint">POST /api/edid/factory  設定初期化</div>
  </div>
</div>

<div id="toast"></div>

<script>
let current = 0;

const EDID_OPS = {
  toggle:  { label: 'EDID ON/OFF トグル中...', ms: 3000,  btnId: 'ebtn-toggle',  isDanger: false },
  refresh: { label: 'EDID リセット中...',       ms: 3000,  btnId: 'ebtn-refresh', isDanger: false },
  factory: { label: 'デフォルト設定初期化中...', ms: 10000, btnId: 'ebtn-factory', isDanger: true  },
};

function updateUI(ch) {
  current = ch;
  document.getElementById('current-label').textContent = 'INPUT ' + ch;
  for (let i = 1; i <= 4; i++) {
    document.getElementById('btn' + i).classList.toggle('active', i === ch);
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
    const data = await res.json();
    updateUI(data.channel);
    showToast('→ INPUT ' + ch);
  } catch(e) { showToast('ERROR'); }
}

async function fetchStatus() {
  try {
    const res = await fetch('/api/input');
    const data = await res.json();
    updateUI(data.channel);
  } catch(e) {}
}

async function triggerEdid(op) {
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
    const data = await res.json();
    showToast(data.message || '完了');
    if (op === 'refresh' || op === 'factory') {
      updateUI(1);
    }
  } catch(e) { showToast('ERROR'); }

  setTimeout(() => {
    allBtns.forEach(b => b.disabled = false);
    activeBtn.classList.remove('running');
    lbl.textContent = '';
    bar.classList.remove('active');
    fill.style.transition = 'none';
    fill.style.width = '0%';
  }, cfg.ms + 200);
}

fetchStatus();
</script>
</body>
</html>
)rawhtml";

// =====================
// HTTPハンドラ
// =====================
void handleRoot() {
  if (!checkAuth()) return;
  server.send(200, "text/html; charset=utf-8", HTML_PAGE);
}

void handleGetInput() {
  if (!checkAuth()) return;
  String json = "{\"channel\":" + String(currentChannel) + "}";
  server.send(200, "application/json", json);
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
    "{\"status\":\"ok\",\"action\":\"edid_toggle\",\"message\":\"EDID ON/OFF トグル完了\"}");
}

void handleEdidRefresh() {
  if (!checkAuth()) return;
  edidRefresh();
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"action\":\"edid_refresh\",\"message\":\"EDID リセット完了\"}");
}

void handleFactoryReset() {
  if (!checkAuth()) return;
  factoryReset();
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"action\":\"factory_reset\",\"message\":\"デフォルト設定初期化完了\"}");
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// =====================
// セットアップ
// =====================
void setup() {
  Serial.begin(115200);

  // 設定ファイル読み込み
  loadConfig();

  // リレーピン初期化
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }

  // Wi-Fi接続 (省電力モード無効化を含む)
  connectWiFi();

  // mDNS起動
  startMDNS();

  // ルーティング
  server.on("/",                  HTTP_GET,  handleRoot);
  server.on("/api/input",         HTTP_GET,  handleGetInput);
  server.on("/api/input",         HTTP_POST, handlePostInput);
  server.on("/api/edid/toggle",   HTTP_POST, handleEdidToggle);
  server.on("/api/edid/refresh",  HTTP_POST, handleEdidRefresh);
  server.on("/api/edid/factory",  HTTP_POST, handleFactoryReset);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Webサーバー起動");
}

// =====================
// メインループ
// ★ Wi-Fi 切断監視・自動再接続を追加
// =====================
void loop() {
  // Wi-Fi 切断検知 → 自動再接続
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi切断を検知 → 再接続中...");
    if (connectWiFi()) {
      startMDNS();  // mDNS も再起動
      Serial.println("再接続成功");
    } else {
      Serial.println("再接続失敗 → 5秒後にリトライ");
      delay(5000);
    }
  }

  server.handleClient();
}
