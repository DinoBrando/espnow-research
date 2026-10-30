/*
  BASELINE ESP-NOW - SENDER (satu firmware untuk semua node)

  - Tidak perlu MAC receiver: sender belajar MAC receiver dari paket HELLO/START.
  - Tidak perlu flash ulang tiap skenario: parameter run datang dari receiver.
  - Node ber-ID 1..N ikut run; node lain diam supaya tidak mengganggu pengukuran.

  CARA SET ID NODE (sekali per board, tersimpan di flash):
    buka Serial Monitor (115200), ketik:  id 7     lalu Enter
  Atau paksa ID lewat kode: ubah FORCE_NODE_ID (nilai bukan 0 mengalahkan ID di flash).

  PERINTAH SERIAL: id N | status | help
  Beri label fisik pada board (N01..N20) sesuai ID-nya.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "espnow_proto.h"
#include "sender_compat.h"

#define FORCE_NODE_ID 0
#define WIFI_POWER_SAVE 0                 // 0 = power save mati (baseline)
const uint8_t WIFI_CHANNEL = 1;           // harus sama dengan receiver
const unsigned long BAUD = 115200;

enum SState : uint8_t { S_IDLE, S_WAIT_START, S_RUNNING, S_REPORTING };

// ===== state =====
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static Preferences prefs;

static uint8_t nodeId = 0;
static uint8_t rxMac[6];
static volatile bool haveRx = false;
static volatile bool needPeer = false;

static volatile SState st = S_IDLE;
static volatile bool cValid = false;
static volatile uint16_t cRunId = 0;
static volatile bool abortFlag = false;
static uint8_t cPayload = 32, cMode = 0;
static uint16_t cInterval = 100, cDur = 30, cProbe = 0, cJitter = 0;
static uint32_t startAtMs, endAtMs, quietUntilMs = 0, nextHelloMs = 0, reportAtMs = 0;
static uint8_t reportTries = 0;
static uint32_t elapsedMs = 0;

static int64_t runT0Us, nextTxUs;
static uint32_t seqNo;
static uint8_t txBuf[ESPNOW_MAX_PAYLOAD];

static volatile uint32_t sentOk, apiErr, macOk, macFail, slips, probes;
static volatile uint32_t rttN, rttMin, rttMax;
static volatile uint64_t rttSum, rttSumSq;

// ===== utilitas =====
static inline bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

void sendStatusCounted(esp_now_send_status_t status) {
  // hanya hitung kiriman paket DATA: HELLO terjadi saat idle, REPORT setelah reportTries > 0
  if (st == S_RUNNING || (st == S_REPORTING && reportTries == 0)) {
    if (status == ESP_NOW_SEND_SUCCESS) macOk++;
    else macFail++;
  }
}

static void addPeers() {
  esp_now_peer_info_t p = {};
  p.channel = 0;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  memcpy(p.peer_addr, rxMac, 6);
  if (!esp_now_is_peer_exist(rxMac)) esp_now_add_peer(&p);
  memcpy(p.peer_addr, BCAST, 6);
  if (!esp_now_is_peer_exist(BCAST)) esp_now_add_peer(&p);
}

static void learnRx(const uint8_t *mac) {
  if (haveRx && memcmp(rxMac, mac, 6) == 0) return;
  memcpy(rxMac, mac, 6);
  haveRx = true;
  needPeer = true;
}

// ===== penerimaan paket (task WiFi: singkat) =====
static void onStart(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(StartPkt)) return;
  StartPkt s;
  memcpy(&s, data, sizeof(s));
  learnRx(info->src_addr);
  if (cValid && s.runId == cRunId) return;       // salinan START yang sama

  cRunId = s.runId;
  cValid = true;
  uint32_t now = millis();

  if (nodeId == 0 || s.activeNodes < nodeId) {   // tidak ikut run ini: diam
    quietUntilMs = now + s.startInMs + (uint32_t)s.durationS * 1000UL + 6000UL;
    st = S_IDLE;
    return;
  }

  cPayload = s.payload < MIN_PAYLOAD ? MIN_PAYLOAD : (s.payload > ESPNOW_MAX_PAYLOAD ? ESPNOW_MAX_PAYLOAD : s.payload);
  cMode = s.mode ? 1 : 0;
  cInterval = s.intervalMs < 1 ? 1 : s.intervalMs;
  cDur = s.durationS;
  cProbe = s.probeEvery;
  cJitter = s.jitterMs;

  portENTER_CRITICAL(&mux);
  sentOk = apiErr = macOk = macFail = slips = probes = 0;
  rttN = 0; rttMin = 0xFFFFFFFFu; rttMax = 0; rttSum = 0; rttSumSq = 0;
  portEXIT_CRITICAL(&mux);

  startAtMs = now + s.startInMs;
  endAtMs = startAtMs + (uint32_t)cDur * 1000UL;
  abortFlag = false;
  reportTries = 0;
  st = S_WAIT_START;
}

static void onStop(const uint8_t *data, int len) {
  if (len < (int)sizeof(StopPkt)) return;
  StopPkt s;
  memcpy(&s, data, sizeof(s));
  if (cValid && s.runId == cRunId && (st == S_WAIT_START || st == S_RUNNING)) abortFlag = true;
}

static void onEcho(const uint8_t *data, int len) {
  if (len < (int)sizeof(EchoPkt)) return;
  EchoPkt e;
  memcpy(&e, data, sizeof(e));
  if (e.h.nodeId != nodeId || !cValid || e.runId != cRunId) return;
  if (st != S_RUNNING && st != S_REPORTING) return;
  uint32_t rtt = (uint32_t)esp_timer_get_time() - e.tsUs;
  portENTER_CRITICAL(&mux);
  rttN++;
  if (rtt < rttMin) rttMin = rtt;
  if (rtt > rttMax) rttMax = rtt;
  rttSum += rtt;
  rttSumSq += (uint64_t)rtt * rtt;
  portEXIT_CRITICAL(&mux);
}

static void onAck(const uint8_t *data, int len) {
  if (len < (int)sizeof(StopPkt)) return;
  StopPkt a;
  memcpy(&a, data, sizeof(a));
  if (cValid && a.h.nodeId == nodeId && a.runId == cRunId && st == S_REPORTING) {
    st = S_IDLE;
    nextHelloMs = millis() + 500;
  }
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(PktHdr) || data[0] != PROTO_MAGIC) return;
  switch (data[1]) {
    case PKT_HELLO:      learnRx(info->src_addr); break;
    case PKT_START:      onStart(info, data, len); break;
    case PKT_STOP:       onStop(data, len); break;
    case PKT_ECHO:       onEcho(data, len); break;
    case PKT_REPORT_ACK: onAck(data, len); break;
    default: break;
  }
}

// ===== logika run =====
static void sendReport() {
  ReportPkt r;
  memset(&r, 0, sizeof(r));
  r.h = makeHdr(PKT_REPORT, nodeId, 0);
  r.runId = cRunId;

  uint32_t n, mn, mx;
  uint64_t sum, sumsq;
  portENTER_CRITICAL(&mux);
  n = rttN; mn = rttMin; mx = rttMax; sum = rttSum; sumsq = rttSumSq;
  portEXIT_CRITICAL(&mux);

  r.sent = sentOk;
  r.apiErr = apiErr;
  r.macOk = macOk;
  r.macFail = macFail;
  r.slips = slips;
  r.probes = probes;
  r.echoes = n;
  if (n) {
    double mean = (double)sum / (double)n;
    double var = (double)sumsq / (double)n - mean * mean;
    if (var < 0) var = 0;
    r.rttMinUs = mn;
    r.rttMaxUs = mx;
    r.rttMeanUs = (uint32_t)(mean + 0.5);
    r.rttStdUs = (uint32_t)(sqrt(var) + 0.5);
  }
  r.elapsedMs = elapsedMs;
  esp_now_send(rxMac, (const uint8_t *)&r, sizeof(r));
}

static void beginRun() {
  memset(txBuf, 0xA5, sizeof(txBuf));
  DataPkt *p = (DataPkt *)txBuf;
  p->h = makeHdr(PKT_DATA, nodeId, 0);
  p->runId = cRunId;
  seqNo = 0;
  runT0Us = esp_timer_get_time();
  nextTxUs = runT0Us + (int64_t)random(0, cInterval) * 1000;   // offset acak 0..interval
  st = S_RUNNING;
  Serial.printf("# run %u mulai: payload=%u interval=%ums dur=%us probe=%u mode=%u\n",
                (unsigned)cRunId, (unsigned)cPayload, (unsigned)cInterval, (unsigned)cDur,
                (unsigned)cProbe, (unsigned)cMode);
}

static void doSend() {
  DataPkt *p = (DataPkt *)txBuf;
  bool probe = (cProbe > 0) && ((seqNo % cProbe) == 0);
  p->h.flags = probe ? FLAG_PROBE : 0;
  p->seq = seqNo;
  if (probe) probes++;
  const uint8_t *dest = cMode ? BCAST : rxMac;
  p->tsUs = (uint32_t)esp_timer_get_time();
  esp_err_t e = esp_now_send(dest, txBuf, cPayload);
  if (e == ESP_OK) sentOk++;
  else apiErr++;
  seqNo++;
}

static void runStep() {
  if (abortFlag || reached(endAtMs)) {
    elapsedMs = (uint32_t)((esp_timer_get_time() - runT0Us) / 1000);
    reportTries = 0;
    reportAtMs = millis() + 700UL + (uint32_t)nodeId * 70UL + (uint32_t)random(0, 40);
    st = S_REPORTING;
    return;
  }
  int64_t nowUs = esp_timer_get_time();
  if (nowUs < nextTxUs) {
    int64_t w = nextTxUs - nowUs;
    if (w > 1500) delay(1);
    else delayMicroseconds((uint32_t)w);
    return;
  }
  doSend();
  int64_t step = (int64_t)cInterval * 1000;
  if (cJitter) step += (int64_t)random(0, cJitter + 1) * 1000;
  nextTxUs += step;
  int64_t after = esp_timer_get_time();
  if (after > nextTxUs + step) {          // tertinggal lebih dari satu interval
    slips++;
    nextTxUs = after + step;
  }
}

static void waitStep() {
  if (abortFlag) {
    elapsedMs = 0;
    reportTries = 0;
    reportAtMs = millis() + 700UL + (uint32_t)nodeId * 70UL;
    st = S_REPORTING;
    return;
  }
  if (reached(startAtMs)) beginRun();
  else delay(1);
}

static void reportStep() {
  if (!reached(reportAtMs)) { delay(1); return; }
  if (reportTries >= 8) {                 // menyerah
    st = S_IDLE;
    nextHelloMs = millis() + 500;
    Serial.printf("# run %u: laporan tidak di-ACK\n", (unsigned)cRunId);
    return;
  }
  sendReport();
  if (reportTries == 0) {
    Serial.printf("# run %u selesai: sent=%lu api_err=%lu mac_ok=%lu mac_fail=%lu slips=%lu\n",
                  (unsigned)cRunId, (unsigned long)sentOk, (unsigned long)apiErr,
                  (unsigned long)macOk, (unsigned long)macFail, (unsigned long)slips);
  }
  reportTries++;
  reportAtMs = millis() + 350UL + (uint32_t)random(0, 250);
}

static void idleStep() {
  if (haveRx && nodeId != 0 && reached(nextHelloMs) && reached(quietUntilMs)) {
    PktHdr h = makeHdr(PKT_HELLO, nodeId, 0);
    esp_now_send(rxMac, (const uint8_t *)&h, sizeof(h));
    nextHelloMs = millis() + 1500UL + (uint32_t)random(0, 1000);
  }
  delay(2);
}

// ===== serial =====
static void printStatus() {
  static const char *names[] = {"IDLE", "WAIT_START", "RUNNING", "REPORTING"};
  Serial.printf("# node ID=%u state=%s receiver=%s run=%u\n", nodeId, names[st],
                haveRx ? "dikenal" : "belum terdengar", (unsigned)cRunId);
  if (haveRx) {
    Serial.printf("# MAC receiver: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  rxMac[0], rxMac[1], rxMac[2], rxMac[3], rxMac[4], rxMac[5]);
  }
  Serial.print("# MAC node ini: ");
  Serial.println(WiFi.macAddress());
}

static void handleCmd(char *line) {
  char *save = nullptr;
  char *tok = strtok_r(line, " \t", &save);
  if (!tok) return;
  if (!strcmp(tok, "id")) {
    char *v = strtok_r(nullptr, " \t", &save);
    int id = v ? atoi(v) : 0;
    if (id < 1 || id > MAX_NODES) { Serial.println("# format: id N  (N = 1..32)"); return; }
    prefs.putUChar("id", (uint8_t)id);
    nodeId = FORCE_NODE_ID ? FORCE_NODE_ID : (uint8_t)id;
    Serial.printf("# ID disimpan: %u\n", nodeId);
  } else if (!strcmp(tok, "status")) {
    printStatus();
  } else {
    Serial.println("# perintah: id N | status | help");
  }
}

static void pollSerial() {
  static char buf[64];
  static size_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[n] = 0;
      if (n) handleCmd(buf);
      n = 0;
    } else if (n < sizeof(buf) - 1) {
      buf[n++] = c;
    }
  }
}

// ===== setup / loop =====
void setup() {
  Serial.begin(BAUD);
  delay(1000);

  prefs.begin("exp", false);
  nodeId = FORCE_NODE_ID ? FORCE_NODE_ID : prefs.getUChar("id", 0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
#if !WIFI_POWER_SAVE
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif

  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW init gagal");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSentCb);

  randomSeed(esp_random() ^ nodeId);
  nextHelloMs = millis() + 500 + random(0, 1000);

  Serial.println();
  Serial.println("# === ESP-NOW Baseline Sender ===");
  if (nodeId == 0) Serial.println("# ID belum diset! ketik:  id N   (N = 1..32)");
  printStatus();
}

void loop() {
  pollSerial();
  if (needPeer) {
    needPeer = false;
    addPeers();
  }
  switch (st) {
    case S_IDLE:       idleStep(); break;
    case S_WAIT_START: waitStep(); break;
    case S_RUNNING:    runStep(); break;
    case S_REPORTING:  reportStep(); break;
  }
}
