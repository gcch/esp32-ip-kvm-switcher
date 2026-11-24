/*
 * KCEVE KVM スイッチャー (KCEVE KC-SHM4 / AIMOS AM-SHM4 互換) for Speed Studio XIAO ESP32-C6
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
#include "esp_wifi.h"   // ★ 追加: esp_wifi_set_ps() のため

#ifndef BUILD_NUMBER
#define BUILD_NUMBER "unknown"
#endif

// =====================
// 定数
// =====================
static char HOSTNAME[64];
static char MDNS_DOMAIN[254];

static char WIFI_SSID[33];
static char WIFI_PASSWORD[64];

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
bool otaUploadAuthorized = false;
bool otaUploadFailed = false;
bool otaRestartPending = false;
bool configLoaded = false;

// ★ Wi-Fi 監視用
volatile bool wifiDisconnectedEvent = false;   // onWiFiEvent から即時セットされるフラグ
unsigned long lastWifiCheckMs = 0;             // 前回の生存確認時刻
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000; // 10秒毎に強制的にも生存確認
unsigned long lastReconnectAttemptMs = 0;      // 前回の再接続試行時刻
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;  // 再接続失敗時のリトライ間隔（非ブロッキング）

// =====================
// 設定ファイル読み込み
// AUTH_USER が空文字列の場合は認証スキップ
// =====================
bool copyConfigString(char* destination, size_t capacity, JsonVariantConst value, const char* key) {
  const char* text = value.as<const char*>();
  if (text == nullptr) {
    Serial.printf("設定項目がありません: %s\n", key);
    return false;
  }

  const size_t length = strlen(text);
  if (length >= capacity) {
    Serial.printf("設定値が長すぎます: %s (最大 %u バイト)\n", key, (unsigned)(capacity - 1));
    return false;
  }

  memcpy(destination, text, length + 1);
  return true;
}

bool loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return false;
  }

  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    Serial.println("コンフィグファイルが見つかりません");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println("JSON parse error");
    return false;
  }

  bool valid = true;
  valid &= copyConfigString(HOSTNAME, sizeof(HOSTNAME), doc["hostname"], "hostname");
  valid &= copyConfigString(MDNS_DOMAIN, sizeof(MDNS_DOMAIN), doc["mdns_domain"], "mdns_domain");
  valid &= copyConfigString(WIFI_SSID, sizeof(WIFI_SSID), doc["wifi_ssid"], "wifi_ssid");
  valid &= copyConfigString(WIFI_PASSWORD, sizeof(WIFI_PASSWORD), doc["wifi_password"], "wifi_password");
  valid &= copyConfigString(AUTH_USER, sizeof(AUTH_USER), doc["auth_user"], "auth_user");
  valid &= copyConfigString(AUTH_PASSWORD, sizeof(AUTH_PASSWORD), doc["auth_password"], "auth_password");
  if (!valid) return false;
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("Wi-Fi SSID が空です");
    return false;
  }
  if (strlen(WIFI_SSID) > 32) {
    Serial.println("Wi-Fi SSID は32バイト以内にしてください");
    return false;
  }
  if (strlen(WIFI_PASSWORD) > 63) {
    Serial.println("Wi-Fi パスワードは63バイト以内にしてください");
    return false;
  }

  Serial.println("コンフィグが読み込まれました");
  Serial.printf("HOSTNAME: %s\n", HOSTNAME);
  Serial.printf("MDNS_DOMAIN: %s\n", MDNS_DOMAIN);
  return true;
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
// Wi-Fi イベントハンドラ
// ★ ルーターの瞬断/再起動時、WiFi.status() の更新が遅れる、
//   もしくはされないケースがあるため、切断イベントを直接検知して
//   即座に再接続フラグを立てる
// =====================
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFiEvent] 切断イベントを検知");
      wifiDisconnectedEvent = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("[WiFiEvent] IP取得完了");
      wifiDisconnectedEvent = false;
      break;
    default:
      break;
  }
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
  WiFi.setAutoReconnect(true);  // ★ 追加: ライブラリ内蔵の自動再接続も有効化
  WiFi.persistent(false);       // ★ 追加: 毎回のbegin()でフラッシュ書き込みしない
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Wi-Fi (%s) 接続中", WIFI_SSID);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    Serial.print('.');
    server.handleClient();  // ★ 追加: 再接続試行中もWebサーバー処理を止めない
    delay(500);
    retry++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi 接続失敗");
    return false;
  }

  // ★ 省電力モード無効化 (Modem Sleep によるPing不達防止)
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifiDisconnectedEvent = false;
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
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>KVMスイッチ</title>
<style>
:root{color-scheme:light;--ink:#191919;--line:#c5c5c5;--blue:#005ea8;--focus:#2670e8;--warning:#b76900;--danger:#b50909}
*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,"Noto Sans JP",sans-serif;line-height:1.6;margin:0;background:#f3f5f7;color:var(--ink)}.page-header{border-bottom:4px solid var(--blue);background:#fff;padding:16px 20px}.header-inner{max-width:1100px;margin:0 auto;display:flex;align-items:baseline;justify-content:space-between;gap:12px;flex-wrap:wrap}.title{font-size:1.55rem;line-height:1.35;margin:0}.firmware-version{font-size:.9rem;color:#505a64}.container{max-width:1100px;margin:0 auto;padding:20px 20px 48px}.tabs{display:flex;gap:4px;overflow-x:auto;border-bottom:2px solid #767676;margin:0;padding:0}.tab{white-space:nowrap;min-height:44px;padding:8px 14px;margin:0 0 -2px;border:0;border-bottom:2px solid transparent;border-radius:4px 4px 0 0;background:#fff;color:var(--blue);font:inherit;font-weight:700;cursor:pointer}.tab[aria-selected=true]{color:var(--ink);border-bottom-color:var(--blue)}:focus-visible{outline:3px solid var(--focus);outline-offset:2px}[hidden]{display:none!important}h2{font-size:1.3rem;margin:0 0 16px;padding-bottom:8px;border-bottom:2px solid var(--ink)}h3{font-size:1.1rem;margin:24px 0 12px}section[role=tabpanel]{background:#fff;border:1px solid var(--line);border-radius:4px;padding:24px;margin:20px 0}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:12px 0 24px}.btn,.edid-btn,#ota-submit{font:inherit;font-weight:700;min-height:44px;padding:8px 16px;border:2px solid var(--blue);border-radius:4px;background:var(--blue);color:#fff;cursor:pointer;margin:3px}.grid .btn{width:100%;margin:0;padding:8px 4px}.btn:hover,.edid-btn:hover,#ota-submit:hover{background:#003e73;border-color:#003e73}.btn.active{background:#003e73;border-color:#003e73;box-shadow:inset 0 -3px 0 #ffbf47}.btn-indicator,.btn-num{display:none}.btn-label{display:block;font-size:.95rem}.edid-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin:12px 0}.edid-btn{width:100%;margin:0}.edid-btn.danger{background:var(--danger);border-color:var(--danger)}.edid-btn.danger:hover,.edid-btn.running.danger{background:#8b0000;border-color:#8b0000}.edid-btn:disabled,#ota-submit:disabled{background:#b8b8b8;border-color:#b8b8b8;cursor:not-allowed}.edid-btn.running{background:#003e73}.notice{padding:12px 16px;border-left:4px solid var(--warning);background:#fff4e5}.progress-wrap{margin:12px 0}.progress-label{min-height:24px;margin-bottom:6px}.progress-bar{height:6px;background:var(--line);overflow:hidden}.progress-fill{height:100%;width:0;background:var(--blue)}.progress-fill.danger{background:var(--danger)}.status-table{width:100%;border-collapse:collapse;margin:12px 0}th{text-align:left}td{padding:10px 12px;border-bottom:1px solid var(--line);vertical-align:top}td:first-child{font-weight:700;width:34%;background:#f5f5f5}.status-value{font-variant-numeric:tabular-nums}#toast{position:fixed;z-index:1000;bottom:12px;left:50%;transform:translate(-50%,80px);max-width:calc(100% - 24px);padding:12px 18px;background:#e8f5e9;border:2px solid #2e7d32;border-radius:4px;box-shadow:0 2px 10px #0004;transition:transform .2s;pointer-events:none}#toast.show{transform:translate(-50%,0)}
@media(max-width:640px){.container{padding:16px 12px 36px}.page-header{padding:16px 12px}section[role=tabpanel]{padding:16px;margin:14px 0}.title{font-size:1.4rem}.grid{gap:6px}.grid .btn{font-size:.9rem}.edid-grid{grid-template-columns:1fr}.status-table,tbody,tr,th,td{display:block;width:100%!important}.status-table th{padding:8px 12px 2px;background:#f5f5f5}td:first-child{border-bottom:0;padding-bottom:2px}td+td{padding-top:4px}}
@media(prefers-reduced-motion:reduce){*,*::before,*::after{scroll-behavior:auto!important;transition-duration:.01ms!important;animation-duration:.01ms!important}}
</style>
</head>
<body>
<header class="page-header"><div class="header-inner"><h1 class="title">KVMスイッチ</h1><div class="firmware-version">FW <span id="firmware-version">取得中</span></div></div></header>
<main class="container">
  <div class="tabs" role="tablist" aria-label="管理画面の機能">
    <button class="tab" id="tab-control" type="button" role="tab" aria-selected="true" aria-controls="panel-control" tabindex="0">操作</button>
    <button class="tab" id="tab-device" type="button" role="tab" aria-selected="false" aria-controls="panel-device" tabindex="-1">デバイス</button>
    <button class="tab" id="tab-ota" type="button" role="tab" aria-selected="false" aria-controls="panel-ota" tabindex="-1">ファームウェア更新</button>
  </div>
  <section id="panel-control" role="tabpanel" aria-labelledby="tab-control" tabindex="0">
  <h2>入力切替</h2>
  <div class="grid">
    <button class="btn" type="button" onclick="switchInput(1)" id="btn1" aria-pressed="false" aria-label="入力 1 に切り替え"><span class="btn-label">入力 1</span></button>
    <button class="btn" type="button" onclick="switchInput(2)" id="btn2" aria-pressed="false" aria-label="入力 2 に切り替え"><span class="btn-label">入力 2</span></button>
    <button class="btn" type="button" onclick="switchInput(3)" id="btn3" aria-pressed="false" aria-label="入力 3 に切り替え"><span class="btn-label">入力 3</span></button>
    <button class="btn" type="button" onclick="switchInput(4)" id="btn4" aria-pressed="false" aria-label="入力 4 に切り替え"><span class="btn-label">入力 4</span></button>
  </div>
  <h3>EDID 操作</h3>
  <p>長押し操作を実行します。初期設定へのリセットは KVM の設定を消去します。</p>
  <div class="edid-grid">
    <button class="edid-btn" type="button" id="ebtn-toggle" onclick="triggerEdid('toggle')">EDID ON/OFF 切替（約 3 秒）</button>
    <button class="edid-btn" type="button" id="ebtn-refresh" onclick="triggerEdid('refresh')">EDID 再読込（約 3 秒）</button>
    <button class="edid-btn danger" type="button" id="ebtn-factory" onclick="triggerEdid('factory')">初期設定にリセット（約 11 秒）</button>
  </div>
  <div class="progress-wrap"><div class="progress-label" id="progress-label" role="status" aria-live="polite"></div><div class="progress-bar" id="progress-bar"><div class="progress-fill" id="progress-fill"></div></div></div>
  </section>
  <section id="panel-device" role="tabpanel" aria-labelledby="tab-device" tabindex="0" hidden>
  <h2>デバイスの状態</h2>
  <table class="status-table"><tbody>
    <tr><th scope="row">SoC</th><td id="device-chip">確認中</td></tr>
    <tr><th scope="row">CPU</th><td id="device-cpu">確認中</td></tr>
    <tr><th scope="row">RAM（合計）</th><td id="device-ram-total">確認中</td></tr>
    <tr><th scope="row">RAM（空き）</th><td id="device-ram-free">確認中</td></tr>
    <tr><th scope="row">PSRAM</th><td id="device-psram">未搭載</td></tr>
    <tr><th scope="row">内蔵フラッシュ</th><td id="device-flash">確認中</td></tr>
    <tr><th scope="row">ファームウェア領域</th><td id="device-app">確認中</td></tr>
    <tr><th scope="row">LittleFS</th><td id="device-filesystem">確認中</td></tr>
    <tr><th scope="row">選択中の入力</th><td id="current-label">未取得</td></tr>
    <tr><th scope="row">ファームウェアビルド</th><td id="device-firmware-version">取得中</td></tr>
  </tbody></table>
  </section>
  <section id="panel-ota" role="tabpanel" aria-labelledby="tab-ota" tabindex="0" hidden>
  <h2>ファームウェア情報</h2>
  <table class="status-table"><tbody>
    <tr><th scope="row">ビルド番号</th><td id="ota-firmware-version">取得中</td></tr>
    <tr><th scope="row">実行中パーティション</th><td id="ota-running-partition">取得中</td></tr>
  </tbody></table>
  <h2>ファームウェア更新</h2>
  <p class="notice">ESP32 用のファームウェア（.bin）を選択してください。OTA 対応パーティションが必要です。更新中は電源を切らないでください。完了後、端末は自動で再起動します。</p>
  <form id="ota-form">
    <table class="status-table"><tbody>
      <tr><th scope="row"><label for="firmware-file">更新ファイル</label></th><td><input id="firmware-file" name="firmware" type="file" accept=".bin,application/octet-stream" required></td></tr>
      <tr><th scope="row">送信状況</th><td><div class="progress-label" id="ota-status" role="status" aria-live="polite">更新ファイルを選択してください。</div><div class="progress-bar"><div class="progress-fill" id="ota-progress"></div></div></td></tr>
    </tbody></table>
    <button class="edid-btn danger" id="ota-submit" type="submit">ファームウェアを更新</button>
  </form>
  </section>
</main>

<div id="toast" role="status" aria-live="polite"></div>

<script>
let current = 0;

const EDID_OPS = {
  toggle:  { label: 'EDID ON/OFF を切替中...', ms: 3300,  btnId: 'ebtn-toggle',  isDanger: false },
  refresh: { label: 'EDID を再読込中...',       ms: 3300,  btnId: 'ebtn-refresh', isDanger: false },
  factory: { label: '初期設定にリセット中...', ms: 11000, btnId: 'ebtn-factory', isDanger: true  },
};

function updateUI(ch) {
  current = ch;
  document.getElementById('current-label').textContent = '入力 ' + ch;
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
    showToast('入力 ' + ch + ' に切り替えました');
  } catch(e) { showToast('入力の切替に失敗しました'); }
}

async function fetchStatus() {
  try {
    const [inputResponse, deviceResponse] = await Promise.all([fetch('/api/input'), fetch('/api/device')]);
    if (!inputResponse.ok || !deviceResponse.ok) throw new Error();
    const input = await inputResponse.json();
    const device = await deviceResponse.json();
    updateUI(input.channel);
    const version = device.version || '不明';
    document.getElementById('firmware-version').textContent = version;
    document.getElementById('device-firmware-version').textContent = version;
    document.getElementById('ota-firmware-version').textContent = version;
    document.getElementById('device-chip').textContent = `${device.chip} (Rev ${device.revision}, ${device.cores}コア)`;
    document.getElementById('device-cpu').textContent = `${device.cpuMHz} MHz`;
    document.getElementById('device-ram-total').textContent = formatBytes(device.ramTotal);
    document.getElementById('device-ram-free').textContent = `${formatBytes(device.ramFree)}（最小空き ${formatBytes(device.ramMinFree)}）`;
    document.getElementById('device-psram').textContent = device.psramTotal ? `${formatBytes(device.psramFree)} / ${formatBytes(device.psramTotal)} 空き` : '未搭載';
    document.getElementById('device-flash').textContent = formatBytes(device.flashTotal);
    document.getElementById('device-app').textContent = `${device.appLabel}（${formatBytes(device.appSize)}、空き ${formatBytes(device.appFree)}）`;
    document.getElementById('device-filesystem').textContent = `${formatBytes(device.fsUsed)} / ${formatBytes(device.fsTotal)} 使用`;
    document.getElementById('ota-running-partition').textContent = device.appLabel;
  } catch(e) {
    document.getElementById('firmware-version').textContent = '取得失敗';
    document.getElementById('device-firmware-version').textContent = '取得失敗';
    document.getElementById('ota-firmware-version').textContent = '取得失敗';
  }
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(2) + ' MiB';
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KiB';
  return bytes + ' B';
}

async function triggerEdid(op) {
  if (op === 'factory' && !window.confirm('KVM を初期設定にリセットします。実行しますか？')) return;
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
    lbl.textContent = data.message || '操作が完了しました';
    showToast(data.message || '完了');
    if (op === 'refresh' || op === 'factory') {
      updateUI(1);
    }
  } catch(e) { lbl.textContent = '操作に失敗しました'; showToast('操作に失敗しました'); }

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

const otaForm = document.getElementById('ota-form');
otaForm.addEventListener('submit', event => {
  event.preventDefault();
  const fileInput = document.getElementById('firmware-file');
  const file = fileInput.files[0];
  const status = document.getElementById('ota-status');
  const progress = document.getElementById('ota-progress');
  const submit = document.getElementById('ota-submit');
  if (!file) { status.textContent = 'ファームウェアファイルを選択してください。'; return; }
  if (!file.name.toLowerCase().endsWith('.bin')) { status.textContent = '.bin ファイルを選択してください。'; return; }
  if (!window.confirm('選択したファームウェアで端末を更新します。実行しますか？')) return;
  const request = new XMLHttpRequest();
  const body = new FormData(otaForm);
  submit.disabled = true;
  fileInput.disabled = true;
  progress.classList.remove('danger');
  progress.style.width = '0%';
  status.textContent = 'ファームウェアを送信しています...';
  request.upload.addEventListener('progress', event => {
    if (!event.lengthComputable) return;
    const percent = Math.round(event.loaded * 100 / event.total);
    progress.style.width = percent + '%';
    status.textContent = '送信中 ' + percent + '%';
  });
  request.addEventListener('load', () => {
    let result = {};
    try { result = JSON.parse(request.responseText); } catch (e) {}
    if (request.status >= 200 && request.status < 300) {
      progress.style.width = '100%';
      status.textContent = result.message || '更新を適用しました。端末の再起動を待っています。';
    } else {
      progress.classList.add('danger');
      status.textContent = result.error || '更新に失敗しました。ファイルと端末の状態を確認してください。';
      submit.disabled = false;
      fileInput.disabled = false;
    }
  });
  request.addEventListener('error', () => {
    progress.classList.add('danger');
    status.textContent = '通信に失敗しました。端末の状態を確認してください。';
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

void handleGetDevice() {
  if (!checkAuth()) return;
  const esp_partition_t* app = esp_ota_get_running_partition();
  char json[640];
  snprintf(json, sizeof(json),
    "{\"version\":\"%s\",\"chip\":\"%s\",\"revision\":%u,\"cores\":%u,\"cpuMHz\":%u,\"ramTotal\":%u,\"ramFree\":%u,\"ramMinFree\":%u,\"psramTotal\":%u,\"psramFree\":%u,\"flashTotal\":%u,\"appLabel\":\"%s\",\"appSize\":%u,\"appFree\":%u,\"fsTotal\":%u,\"fsUsed\":%u}",
    BUILD_NUMBER, ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz(),
    ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getPsramSize(), ESP.getFreePsram(),
    ESP.getFlashChipSize(), app ? app->label : "unknown", ESP.getSketchSize(), ESP.getFreeSketchSpace(),
    LittleFS.totalBytes(), LittleFS.usedBytes());
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
      Serial.println("OTA rejected: firmware filename must end in .bin");
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      otaUploadFailed = true;
      Update.printError(Serial);
      return;
    }
    Serial.printf("OTA upload started: %s\n", upload.filename.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE && otaUploadAuthorized && !otaUploadFailed) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaUploadFailed = true;
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END && otaUploadAuthorized && !otaUploadFailed) {
    if (!Update.end(true)) {
      otaUploadFailed = true;
      Update.printError(Serial);
    } else {
      Serial.printf("OTA upload complete: %u bytes\n", upload.totalSize);
      otaRestartPending = true;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaUploadFailed = true;
    Serial.println("OTA upload aborted");
  }
}

void handleOtaComplete() {
  if (!checkAuth()) return;
  if (otaUploadFailed || !otaUploadAuthorized || !otaRestartPending) {
    Update.abort();
    otaUploadAuthorized = false;
    otaRestartPending = false;
    server.send(400, "application/json", "{\"error\":\"更新に失敗しました。ファイル形式、ファームウェア、または OTA 用パーティションを確認してください。\"}");
    return;
  }

  otaUploadAuthorized = false;
  otaRestartPending = false;
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"更新しました。端末を再起動しています。\"}");
  delay(500);
  ESP.restart();
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
  configLoaded = loadConfig();
  if (!configLoaded) {
    Serial.println("設定を確認して LittleFS に config.json を書き込んでください");
  }

  // リレーピン初期化
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }

  // ★ Wi-Fi イベントハンドラ登録（接続前に登録しておく）
  WiFi.onEvent(onWiFiEvent);

  // Wi-Fi接続 (省電力モード無効化を含む)
  if (configLoaded) {
    connectWiFi();
    startMDNS();
  }

  // ルーティング
  server.on("/",                  HTTP_GET,  handleRoot);
  server.on("/api/input",         HTTP_GET,  handleGetInput);
  server.on("/api/device",        HTTP_GET,  handleGetDevice);
  server.on("/api/input",         HTTP_POST, handlePostInput);
  server.on("/api/edid/toggle",   HTTP_POST, handleEdidToggle);
  server.on("/api/edid/refresh",  HTTP_POST, handleEdidRefresh);
  server.on("/api/edid/factory",  HTTP_POST, handleFactoryReset);
  server.on("/api/ota", HTTP_POST, handleOtaComplete, handleOtaUpload);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Webサーバー起動");
}

// =====================
// メインループ
// ★ Wi-Fi 切断監視・自動再接続
//   1) onWiFiEvent からの即時フラグ
//   2) WiFi.status() の定期ポーリング
//   3) ルーター再起動などで status() 自体が更新されないケースに備え、
//      10秒毎に IP が失われていないかも強制チェック
//   のいずれかで再接続をトリガーする。
//   また、再接続の待機・リトライは delay() でブロッキングせず、
//   Webサーバーへの応答を止めないようにしている。
// =====================
void loop() {
  if (!configLoaded) {
    server.handleClient();
    return;
  }

  unsigned long now = millis();

  bool statusDown  = (WiFi.status() != WL_CONNECTED);
  bool ipLost      = (WiFi.localIP() == IPAddress(0, 0, 0, 0));
  bool periodicHit = (now - lastWifiCheckMs > WIFI_CHECK_INTERVAL_MS);

  if (periodicHit) {
    lastWifiCheckMs = now;
  }

  bool needReconnect = statusDown || wifiDisconnectedEvent || (periodicHit && ipLost);

  if (needReconnect) {
    // ★ 非ブロッキング化: 前回試行から一定間隔を空けてからのみ再試行する
    if (now - lastReconnectAttemptMs > WIFI_RETRY_INTERVAL_MS || lastReconnectAttemptMs == 0) {
      lastReconnectAttemptMs = now;
      wifiDisconnectedEvent = false;

      Serial.println("Wi-Fi切断を検知 → 再接続中...");
      if (connectWiFi()) {
        startMDNS();  // mDNS も再起動
        Serial.println("再接続成功");
      } else {
        Serial.println("再接続失敗 → 次回リトライまで通常処理を継続");
      }
    }
  }

  server.handleClient();
}
