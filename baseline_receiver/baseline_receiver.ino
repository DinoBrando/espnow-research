/*
  BASELINE ESP-NOW - RECEIVER + PENGONTROL EKSPERIMEN (ESP32-C6)

  Receiver ini mengatur seluruh eksperimen: memberi perintah ke sender lewat broadcast,
  mencatat statistik sisi penerima (loss, throughput, jitter, RSSI), mengumpulkan laporan
  sisi pengirim (MAC ok/gagal, RTT, dll), lalu mencetak hasil sebagai CSV.

  PERINTAH SERIAL (akhiri dengan Enter):
    help
    status                  keadaan, progres batch, ukuran log
    nodes                   node yang terdengar dan umur terakhir terdengar
    run N PAYLOAD INTERVAL_MS [DURASI_S] [PROBE_TIAP] [MODE]
                            satu run. contoh: run 10 128 50 30
    batch [nodes=1,5,10,20] [payload=32,128,250] [interval=100,50,20,10]
          [mode=0] [dur=30] [reps=3] [probe=20] [jitter=0]
                            jalankan seluruh matriks. tanpa argumen = nilai default
    stop                    batalkan run / batch
    dump                    cetak isi log CSV di flash (LittleFS)
    clearlog                hapus log
    header                  cetak ulang header kolom CSV

  FORMAT KELUARAN (satu baris = satu record, kolom dipisah koma):
    #...   komentar / progres (abaikan saat parsing)
    #HD,.. header baris D      D,... satu baris per node per run
    #HS,.. header baris S      S,... satu baris ringkasan per run

  Pengaturan Arduino IDE: Board ESP32C6 Dev Module. Pilih partisi yang punya SPIFFS/LittleFS
  (default sudah ada) kalau ingin log tersimpan di flash.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "espnow_proto.h"

#define USE_LITTLEFS 1
#if USE_LITTLEFS
#include <LittleFS.h>
#endif

// ===== konfigurasi =====
const unsigned long BAUD = 921600;     // turunkan ke 115200 jika teks rusak
const uint8_t  WIFI_CHANNEL = 1;       // harus sama di semua sender
#define WIFI_POWER_SAVE 0              // 0 = power save mati (baseline); ubah nanti untuk tes energi
const char *LOG_PATH = "/baseline_log.csv";

const uint32_t START_LEAD_MS = 1500;           // jeda dari perintah sampai run mulai
const uint32_t GRACE_MS = 500;                 // paket telat yang masih dihitung
const uint32_t REPORT_WAIT_BASE_MS = 3500;     // batas tunggu laporan sender
const uint32_t REPORT_WAIT_PER_NODE_MS = 120;
const uint32_t GAP_BETWEEN_RUNS_MS = 1000;
const uint32_t HELLO_PERIOD_MS = 2000;

// ===== tipe =====
enum State : uint8_t { ST_IDLE, ST_STARTING, ST_RUNNING, ST_GRACE, ST_REPORTING };

typedef struct {
  uint16_t runId;
  uint16_t rep;
  uint8_t  nodes, payload, mode;
  uint16_t intervalMs, durationS, probeEvery, jitterMs;
} RunCfg;

typedef struct {
  uint32_t rx, bytes, reorder, lostGap;
  uint32_t lastSeq, lastArrUs, lastTsUs, maxGapUs;
  int32_t  jitScaled;          // jitter RFC3550, skala x16
  int32_t  rssiSum;
  int8_t   rssiMin;
  bool     seen;
  bool     haveRep;
  ReportPkt rep;
} NodeRun;

typedef struct {
  uint8_t  mac[6];
  uint8_t  kind;               // 0 = echo probe, 1 = ACK laporan
  uint8_t  nodeId;
  uint16_t runId;
  uint32_t seq;
  uint32_t tsUs;
} EchoJob;

// ===== state global =====
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static volatile State state = ST_IDLE;
static volatile uint16_t curRunId = 0;
static volatile bool acceptData = false;
static volatile bool acceptReports = false;
static volatile uint32_t reportsGot = 0;
static volatile uint32_t echoDrop = 0, echoFail = 0;
static volatile uint32_t lastSeenMs[MAX_NODES + 1];

static NodeRun run_[MAX_NODES + 1];
static NodeRun snap_[MAX_NODES + 1];
static RunCfg cfg;
static QueueHandle_t echoQ;
static uint16_t runCounter;

static uint32_t tStartMs, tEndMs, tGraceEndMs, repDeadlineMs;
static uint32_t nextStartTx, nextHelloMs, nextRunAt;
static uint8_t startsSent;

// batch
static bool batchActive = false;
static uint32_t batchIdx = 0, batchTotal = 0;
static uint16_t bNodes[8], bPayload[8], bInterval[8], bMode[2];
static int nbNodes, nbPayload, nbInterval, nbMode;
static uint16_t bDur, bReps, bProbe, bJitter;

static bool logOk = false;
#if USE_LITTLEFS
static File logFile;
#endif

// ===== utilitas =====
static inline bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

static void openLog() {
#if USE_LITTLEFS
  if (logOk) logFile = LittleFS.open(LOG_PATH, "a");
#endif
}

static void closeLog() {
#if USE_LITTLEFS
  if (logOk && logFile) logFile.close();
#endif
}

static void emit(const char *s) {
  Serial.println(s);
#if USE_LITTLEFS
  if (logOk && logFile) logFile.println(s);
#endif
}

static void printHeaders() {
  openLog();
  emit("#HD,tag,run_id,rep,mode,nodes,payload,interval_ms,duration_s,probe_every,jitter_ms,node,report_ok,"
       "tx_attempts,tx_sent,tx_api_err,mac_ok,mac_fail,rx,lost_gap,reorder,pdr_pct,pdr_sent_pct,goodput_bps,"
       "jitter_us,max_gap_ms,rssi_avg,rssi_min,probes,echoes,rtt_min_us,rtt_mean_us,rtt_std_us,rtt_max_us,slips");
  emit("#HS,tag,run_id,rep,mode,nodes,payload,interval_ms,duration_s,probe_every,jitter_ms,reports,"
       "tx_attempts,rx,pdr_pct,offered_bps,goodput_bps,jain_pdr,worst_node,worst_pdr_pct,jitter_us_mean,"
       "rtt_mean_us,mac_fail,rssi_avg");
  closeLog();
}

// Echo dan ACK laporan dikirim lewat BROADCAST, bukan unicast per node.
// Alasannya: ESP-NOW hanya mengizinkan 20 peer, sedangkan 20 sender + broadcast = 21.
// Tiap paket membawa nodeId, dan sender mengabaikan echo/ACK yang bukan miliknya,
// jadi receiver cukup punya satu peer (broadcast) untuk membalas semua node.
// Konsekuensi: broadcast tidak di-ACK/retry di tingkat MAC, jadi echo bisa hilang
// saat kanal padat (terlihat dari echoes < probes). Laporan yang ACK-nya hilang
// dikirim ulang oleh sender (maks 8 kali), jadi tetap sampai.
static void echoTask(void *arg) {
  (void)arg;
  EchoJob j;
  for (;;) {
    if (xQueueReceive(echoQ, &j, portMAX_DELAY) != pdTRUE) continue;
    if (j.kind == 0) {
      EchoPkt e;
      e.h = makeHdr(PKT_ECHO, j.nodeId, 0);
      e.runId = j.runId;
      e.seq = j.seq;
      e.tsUs = j.tsUs;
      if (esp_now_send(BCAST, (const uint8_t *)&e, sizeof(e)) != ESP_OK) echoFail++;
    } else {
      StopPkt a;
      a.h = makeHdr(PKT_REPORT_ACK, j.nodeId, 0);
      a.runId = j.runId;
      if (esp_now_send(BCAST, (const uint8_t *)&a, sizeof(a)) != ESP_OK) echoFail++;
    }
  }
}

// ===== penerimaan paket (jalan di task WiFi: singkat, tanpa Serial) =====
static void handleData(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!acceptData || len < (int)sizeof(DataPkt)) return;
  DataPkt p;
  memcpy(&p, data, sizeof(p));
  uint8_t id = p.h.nodeId;
  if (id < 1 || id > MAX_NODES || p.runId != curRunId) return;

  uint32_t arr = (uint32_t)esp_timer_get_time();
  int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  lastSeenMs[id] = millis();

  portENTER_CRITICAL(&mux);
  NodeRun &n = run_[id];
  if (!n.seen) {
    n.seen = true;
    n.lostGap += p.seq;                      // paket sebelum yang pertama terdengar
    n.lastSeq = p.seq;
    n.rssiMin = rssi;
  } else {
    if (p.seq > n.lastSeq) {
      n.lostGap += p.seq - n.lastSeq - 1;
      n.lastSeq = p.seq;
    } else {
      n.reorder++;                           // telat atau duplikat
    }
    int32_t d = (int32_t)((arr - n.lastArrUs) - (p.tsUs - n.lastTsUs));
    if (d < 0) d = -d;
    n.jitScaled += d - (n.jitScaled >> 4);
    uint32_t gap = arr - n.lastArrUs;
    if (gap > n.maxGapUs) n.maxGapUs = gap;
    if (rssi < n.rssiMin) n.rssiMin = rssi;
  }
  n.lastArrUs = arr;
  n.lastTsUs = p.tsUs;
  n.rx++;
  n.bytes += (uint32_t)len;
  n.rssiSum += rssi;
  portEXIT_CRITICAL(&mux);

  if (p.h.flags & FLAG_PROBE) {
    EchoJob j;
    memset(&j, 0, sizeof(j));
    memcpy(j.mac, info->src_addr, 6);
    j.kind = 0;
    j.nodeId = id;
    j.runId = p.runId;
    j.seq = p.seq;
    j.tsUs = p.tsUs;
    if (xQueueSend(echoQ, &j, 0) != pdTRUE) echoDrop++;
  }
}

static void handleHello(const uint8_t *data, int len) {
  if (len < (int)sizeof(PktHdr)) return;
  uint8_t id = data[2];
  if (id >= 1 && id <= MAX_NODES) lastSeenMs[id] = millis();
}

static void handleReport(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(ReportPkt)) return;
  ReportPkt r;
  memcpy(&r, data, sizeof(r));
  uint8_t id = r.h.nodeId;
  if (id < 1 || id > MAX_NODES) return;
  lastSeenMs[id] = millis();

  EchoJob j;                                  // ACK selalu, supaya sender berhenti mengulang
  memset(&j, 0, sizeof(j));
  memcpy(j.mac, info->src_addr, 6);
  j.kind = 1;
  j.nodeId = id;
  j.runId = r.runId;
  if (xQueueSend(echoQ, &j, 0) != pdTRUE) echoDrop++;

  if (acceptReports && r.runId == curRunId) {
    portENTER_CRITICAL(&mux);
    if (!run_[id].haveRep) {
      run_[id].rep = r;
      run_[id].haveRep = true;
      reportsGot++;
    }
    portEXIT_CRITICAL(&mux);
  }
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(PktHdr) || data[0] != PROTO_MAGIC) return;
  switch (data[1]) {
    case PKT_DATA:   handleData(info, data, len); break;
    case PKT_HELLO:  handleHello(data, len); break;
    case PKT_REPORT: handleReport(info, data, len); break;
    default: break;
  }
}

// ===== kontrol run =====
static void sendHello() {
  PktHdr h = makeHdr(PKT_HELLO, 0, 0);
  esp_now_send(BCAST, (const uint8_t *)&h, sizeof(h));
}

static void sendStart(uint32_t startInMs) {
  StartPkt s;
  memset(&s, 0, sizeof(s));
  s.h = makeHdr(PKT_START, 0, 0);
  s.runId = cfg.runId;
  s.activeNodes = cfg.nodes;
  s.payload = cfg.payload;
  s.mode = cfg.mode;
  s.intervalMs = cfg.intervalMs;
  s.durationS = cfg.durationS;
  s.probeEvery = cfg.probeEvery;
  s.jitterMs = cfg.jitterMs;
  s.startInMs = (uint16_t)startInMs;
  esp_now_send(BCAST, (const uint8_t *)&s, sizeof(s));
}

static void sendStop() {
  StopPkt s;
  s.h = makeHdr(PKT_STOP, 0, 0);
  s.runId = cfg.runId;
  esp_now_send(BCAST, (const uint8_t *)&s, sizeof(s));
}

static void startRun(const RunCfg &c) {
  cfg = c;
  runCounter++;
  cfg.runId = runCounter;
  curRunId = cfg.runId;

  portENTER_CRITICAL(&mux);
  memset(run_, 0, sizeof(run_));
  reportsGot = 0;
  portEXIT_CRITICAL(&mux);
  echoDrop = 0; echoFail = 0;

  acceptData = true;
  acceptReports = false;
  startsSent = 0;
  nextStartTx = millis();
  tStartMs = millis() + START_LEAD_MS;
  state = ST_STARTING;

  Serial.printf("# RUN id=%u rep=%u nodes=%u payload=%u interval=%ums dur=%us probe=%u mode=%u jitter=%u\n",
                (unsigned)cfg.runId, (unsigned)cfg.rep, (unsigned)cfg.nodes, (unsigned)cfg.payload,
                (unsigned)cfg.intervalMs, (unsigned)cfg.durationS, (unsigned)cfg.probeEvery,
                (unsigned)cfg.mode, (unsigned)cfg.jitterMs);
}

static void finishRun() {
  portENTER_CRITICAL(&mux);
  memcpy(snap_, run_, sizeof(run_));
  portEXIT_CRITICAL(&mux);
  acceptReports = false;

  openLog();
  char b[512];
  uint32_t totAtt = 0, totRx = 0, totBytes = 0, totMacFail = 0, nRep = 0;
  float sumX = 0, sumX2 = 0;
  int nX = 0;
  float worstPdr = 1000.0f;
  int worstNode = 0;
  double jitSum = 0;
  int jitN = 0;
  double rttW = 0;
  uint32_t rttCnt = 0;
  double rssiSum = 0;
  uint32_t rssiN = 0;

  for (int id = 1; id <= cfg.nodes; id++) {
    const NodeRun &n = snap_[id];
    const bool rep = n.haveRep;
    uint32_t sent = 0, apiErr = 0, macOk = 0, macFail = 0, slips = 0, probes = 0, echoes = 0;
    uint32_t rMin = 0, rMean = 0, rStd = 0, rMax = 0;
    if (rep) {
      sent = n.rep.sent; apiErr = n.rep.apiErr; macOk = n.rep.macOk; macFail = n.rep.macFail;
      slips = n.rep.slips; probes = n.rep.probes; echoes = n.rep.echoes;
      rMin = n.rep.rttMinUs; rMean = n.rep.rttMeanUs; rStd = n.rep.rttStdUs; rMax = n.rep.rttMaxUs;
      nRep++;
    }
    uint32_t attempts = rep ? (sent + apiErr) : (n.seen ? n.lastSeq + 1 : 0);
    float pdr = attempts ? 100.0f * n.rx / attempts : 0.0f;
    if (pdr > 100.0f) pdr = 100.0f;
    float pdrSent = sent ? 100.0f * n.rx / sent : 0.0f;
    if (pdrSent > 100.0f) pdrSent = 100.0f;
    float goodput = (n.bytes * 8.0f) / (float)cfg.durationS;
    float jit = n.jitScaled / 16.0f;
    float maxGapMs = n.maxGapUs / 1000.0f;
    float rssiAvg = n.rx ? (float)n.rssiSum / n.rx : 0.0f;
    int rssiMin = n.rx ? n.rssiMin : 0;

    snprintf(b, sizeof(b),
             "D,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f,%.0f,%.1f,%.2f,%.1f,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
             (unsigned)cfg.runId, (unsigned)cfg.rep, (unsigned)cfg.mode, (unsigned)cfg.nodes,
             (unsigned)cfg.payload, (unsigned)cfg.intervalMs, (unsigned)cfg.durationS,
             (unsigned)cfg.probeEvery, (unsigned)cfg.jitterMs,
             id, rep ? 1 : 0,
             (unsigned long)attempts, (unsigned long)sent, (unsigned long)apiErr,
             (unsigned long)macOk, (unsigned long)macFail,
             (unsigned long)n.rx, (unsigned long)n.lostGap, (unsigned long)n.reorder,
             pdr, pdrSent, goodput, jit, maxGapMs, rssiAvg, rssiMin,
             (unsigned long)probes, (unsigned long)echoes,
             (unsigned long)rMin, (unsigned long)rMean, (unsigned long)rStd, (unsigned long)rMax,
             (unsigned long)slips);
    emit(b);

    totAtt += attempts;
    totRx += n.rx;
    totBytes += n.bytes;
    totMacFail += macFail;
    if (attempts > 0) {
      sumX += pdr;
      sumX2 += pdr * pdr;
      nX++;
      if (pdr < worstPdr) { worstPdr = pdr; worstNode = id; }
    }
    if (n.rx > 1) { jitSum += jit; jitN++; }
    if (echoes > 0) { rttW += (double)rMean * echoes; rttCnt += echoes; }
    rssiSum += n.rssiSum;
    rssiN += n.rx;
  }

  float pdrAll = totAtt ? 100.0f * totRx / totAtt : 0.0f;
  if (pdrAll > 100.0f) pdrAll = 100.0f;
  float offered = ((float)totAtt * cfg.payload * 8.0f) / (float)cfg.durationS;
  float goodAgg = ((float)totBytes * 8.0f) / (float)cfg.durationS;
  float jain = (nX > 0 && sumX2 > 0) ? (sumX * sumX) / ((float)nX * sumX2) : 0.0f;
  if (nX == 0) worstPdr = 0;
  float jitMean = jitN ? (float)(jitSum / jitN) : 0.0f;
  float rttMean = rttCnt ? (float)(rttW / rttCnt) : 0.0f;
  float rssiMean = rssiN ? (float)(rssiSum / rssiN) : 0.0f;

  snprintf(b, sizeof(b),
           "S,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%lu,%.2f,%.0f,%.0f,%.3f,%d,%.2f,%.1f,%.0f,%lu,%.1f",
           (unsigned)cfg.runId, (unsigned)cfg.rep, (unsigned)cfg.mode, (unsigned)cfg.nodes,
           (unsigned)cfg.payload, (unsigned)cfg.intervalMs, (unsigned)cfg.durationS,
           (unsigned)cfg.probeEvery, (unsigned)cfg.jitterMs,
           (unsigned)nRep,
           (unsigned long)totAtt, (unsigned long)totRx,
           pdrAll, offered, goodAgg, jain, worstNode, worstPdr, jitMean, rttMean,
           (unsigned long)totMacFail, rssiMean);
  emit(b);
  closeLog();

  Serial.printf("# selesai run %u: laporan %lu/%u, echo_drop=%lu echo_fail=%lu\n",
                (unsigned)cfg.runId, (unsigned long)nRep, (unsigned)cfg.nodes,
                (unsigned long)echoDrop, (unsigned long)echoFail);

  state = ST_IDLE;
  nextRunAt = millis() + GAP_BETWEEN_RUNS_MS;
  if (batchActive) {
    batchIdx++;
    if (batchIdx >= batchTotal) {
      batchActive = false;
      Serial.println("# BATCH SELESAI");
    }
  }
}

static void startNextRun() {
  uint32_t combos = (uint32_t)nbNodes * nbPayload * nbInterval * nbMode;
  uint32_t rep = batchIdx / combos;
  uint32_t c = batchIdx % combos;
  int iM = c % nbMode;     c /= nbMode;
  int iI = c % nbInterval; c /= nbInterval;
  int iP = c % nbPayload;  c /= nbPayload;
  int iN = (int)c;

  RunCfg r;
  memset(&r, 0, sizeof(r));
  r.rep = (uint16_t)(rep + 1);
  r.nodes = (uint8_t)bNodes[iN];
  r.payload = (uint8_t)bPayload[iP];
  r.intervalMs = bInterval[iI];
  r.mode = (uint8_t)bMode[iM];
  r.durationS = bDur;
  r.probeEvery = bProbe;
  r.jitterMs = bJitter;
  Serial.printf("# batch %lu/%lu\n", (unsigned long)(batchIdx + 1), (unsigned long)batchTotal);
  startRun(r);
}

// ===== perintah =====
static void batchDefaults() {
  bNodes[0] = 1; bNodes[1] = 5; bNodes[2] = 10; bNodes[3] = 20; nbNodes = 4;
  bPayload[0] = 32; bPayload[1] = 128; bPayload[2] = 250; nbPayload = 3;
  bInterval[0] = 100; bInterval[1] = 50; bInterval[2] = 20; bInterval[3] = 10; nbInterval = 4;
  bMode[0] = 0; nbMode = 1;
  bDur = 30; bReps = 3; bProbe = 20; bJitter = 0;
}

static int parseList(const char *s, uint16_t *out, int maxn) {
  int n = 0;
  char tmp[64];
  strncpy(tmp, s, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  char *save = nullptr;
  for (char *t = strtok_r(tmp, ",", &save); t && n < maxn; t = strtok_r(nullptr, ",", &save)) {
    int v = atoi(t);
    if (v < 0) v = 0;
    out[n++] = (uint16_t)v;
  }
  return n;
}

static void printHelp() {
  Serial.println("# perintah: help | status | nodes | run N PAYLOAD INTERVAL_MS [DUR_S] [PROBE] [MODE]");
  Serial.println("#           batch [nodes=..] [payload=..] [interval=..] [mode=..] [dur=..] [reps=..] [probe=..] [jitter=..]");
  Serial.println("#           stop | dump | clearlog | header");
}

static void printNodes() {
  uint32_t now = millis();
  Serial.print("# node terdengar (umur detik):");
  int cnt = 0;
  for (int id = 1; id <= MAX_NODES; id++) {
    if (lastSeenMs[id] == 0) continue;
    Serial.printf(" N%d(%lu)", id, (unsigned long)((now - lastSeenMs[id]) / 1000));
    cnt++;
  }
  Serial.printf("  total=%d\n", cnt);
}

static void printStatus() {
  static const char *names[] = {"IDLE", "STARTING", "RUNNING", "GRACE", "REPORTING"};
  Serial.printf("# state=%s runId=%u batch=%s %lu/%lu logOk=%d\n",
                names[state], (unsigned)curRunId, batchActive ? "aktif" : "tidak",
                (unsigned long)batchIdx, (unsigned long)batchTotal, logOk ? 1 : 0);
#if USE_LITTLEFS
  if (logOk) {
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) { Serial.printf("# ukuran log: %lu byte\n", (unsigned long)f.size()); f.close(); }
    Serial.printf("# flash: terpakai %lu / %lu byte\n",
                  (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
  }
#endif
  printNodes();
}

static void cmdRun(char *args) {
  if (state != ST_IDLE) { Serial.println("# sedang berjalan, ketik stop dulu"); return; }
  int v[6] = {0, 0, 0, 30, 20, 0};
  int n = 0;
  char *save = nullptr;
  for (char *t = strtok_r(args, " \t", &save); t && n < 6; t = strtok_r(nullptr, " \t", &save)) v[n++] = atoi(t);
  if (n < 3 || v[0] < 1 || v[0] > MAX_NODES || v[2] < 1) {
    Serial.println("# format: run N PAYLOAD INTERVAL_MS [DUR_S] [PROBE] [MODE]");
    return;
  }
  RunCfg r;
  memset(&r, 0, sizeof(r));
  r.rep = 1;
  r.nodes = (uint8_t)v[0];
  r.payload = (uint8_t)constrain(v[1], MIN_PAYLOAD, ESPNOW_MAX_PAYLOAD);
  r.intervalMs = (uint16_t)v[2];
  r.durationS = (uint16_t)(v[3] < 1 ? 1 : v[3]);
  r.probeEvery = (uint16_t)v[4];
  r.mode = (uint8_t)(v[5] ? 1 : 0);
  r.jitterMs = 0;
  batchActive = false;
  printHeaders();
  startRun(r);
}

static void cmdBatch(char *args) {
  if (state != ST_IDLE) { Serial.println("# sedang berjalan, ketik stop dulu"); return; }
  batchDefaults();
  char *save = nullptr;
  for (char *t = strtok_r(args, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
    char *eq = strchr(t, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = t;
    const char *val = eq + 1;
    if (!strcmp(k, "nodes")) nbNodes = parseList(val, bNodes, 8);
    else if (!strcmp(k, "payload")) nbPayload = parseList(val, bPayload, 8);
    else if (!strcmp(k, "interval")) nbInterval = parseList(val, bInterval, 8);
    else if (!strcmp(k, "mode")) nbMode = parseList(val, bMode, 2);
    else if (!strcmp(k, "dur")) bDur = (uint16_t)atoi(val);
    else if (!strcmp(k, "reps")) bReps = (uint16_t)atoi(val);
    else if (!strcmp(k, "probe")) bProbe = (uint16_t)atoi(val);
    else if (!strcmp(k, "jitter")) bJitter = (uint16_t)atoi(val);
  }
  if (nbNodes < 1 || nbPayload < 1 || nbInterval < 1 || nbMode < 1 || bReps < 1 || bDur < 1) {
    Serial.println("# argumen batch tidak valid");
    return;
  }
  uint16_t maxNode = 0;
  for (int i = 0; i < nbNodes; i++) {
    if (bNodes[i] < 1 || bNodes[i] > MAX_NODES) { Serial.println("# nodes harus 1..32"); return; }
    if (bNodes[i] > maxNode) maxNode = bNodes[i];
  }
  for (int i = 0; i < nbPayload; i++) bPayload[i] = (uint16_t)constrain((int)bPayload[i], MIN_PAYLOAD, ESPNOW_MAX_PAYLOAD);
  for (int i = 0; i < nbInterval; i++) if (bInterval[i] < 1) bInterval[i] = 1;
  for (int i = 0; i < nbMode; i++) bMode[i] = bMode[i] ? 1 : 0;

  batchTotal = (uint32_t)bReps * nbNodes * nbPayload * nbInterval * nbMode;
  batchIdx = 0;
  uint32_t estS = batchTotal * (uint32_t)(bDur + 5);
  Serial.printf("# BATCH: %lu run, perkiraan %lu menit (%lu jam)\n",
                (unsigned long)batchTotal, (unsigned long)(estS / 60), (unsigned long)(estS / 3600));

  uint32_t now = millis();
  int missing = 0;
  Serial.print("# node belum terdengar (<15 dtk):");
  for (int id = 1; id <= maxNode; id++) {
    if (lastSeenMs[id] == 0 || (now - lastSeenMs[id]) > 15000) { Serial.printf(" N%d", id); missing++; }
  }
  Serial.println(missing ? "  <-- cek daya dan ID node tersebut" : " (tidak ada)");

  printHeaders();
  batchActive = true;
  nextRunAt = millis() + 2000;
}

static void stopAll() {
  bool wasActive = (state != ST_IDLE) || batchActive;
  if (state != ST_IDLE) {
    for (int i = 0; i < 3; i++) { sendStop(); delay(30); }
  }
  acceptData = false;
  acceptReports = false;
  state = ST_IDLE;
  batchActive = false;
  Serial.println(wasActive ? "# DIBATALKAN" : "# tidak ada yang berjalan");
}

static void cmdDump() {
#if USE_LITTLEFS
  if (state != ST_IDLE) { Serial.println("# tunggu idle"); return; }
  if (!logOk) { Serial.println("# LittleFS tidak aktif"); return; }
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) { Serial.println("# belum ada log"); return; }
  Serial.println("# --- DUMP MULAI ---");
  uint8_t buf[256];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    Serial.write(buf, (size_t)n);
  }
  f.close();
  Serial.println("# --- DUMP SELESAI ---");
#else
  Serial.println("# LittleFS dimatikan di kode");
#endif
}

static void cmdClear() {
#if USE_LITTLEFS
  if (logOk && LittleFS.remove(LOG_PATH)) Serial.println("# log dihapus");
  else Serial.println("# tidak ada log / gagal hapus");
#endif
}

static void handleCmd(char *line) {
  char *save = nullptr;
  char *tok = strtok_r(line, " \t", &save);
  if (!tok) return;
  char *rest = save ? save : (char *)"";
  if (!strcmp(tok, "help")) printHelp();
  else if (!strcmp(tok, "status")) printStatus();
  else if (!strcmp(tok, "nodes")) printNodes();
  else if (!strcmp(tok, "stop")) stopAll();
  else if (!strcmp(tok, "run")) cmdRun(rest);
  else if (!strcmp(tok, "batch")) cmdBatch(rest);
  else if (!strcmp(tok, "dump")) cmdDump();
  else if (!strcmp(tok, "clearlog")) cmdClear();
  else if (!strcmp(tok, "header")) printHeaders();
  else Serial.println("# perintah tidak dikenal, ketik help");
}

static void pollSerial() {
  static char buf[200];
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
  delay(1500);

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

  echoQ = xQueueCreate(32, sizeof(EchoJob));
  xTaskCreate(echoTask, "echo", 4096, nullptr, 5, nullptr);

  esp_now_peer_info_t bp = {};
  memcpy(bp.peer_addr, BCAST, 6);
  bp.channel = 0;
  bp.ifidx = WIFI_IF_STA;
  bp.encrypt = false;
  esp_now_add_peer(&bp);
  esp_now_register_recv_cb(onRecv);

#if USE_LITTLEFS
  logOk = LittleFS.begin(true);
#endif

  runCounter = (uint16_t)esp_random();     // acak supaya tidak bentrok dengan run lama di sender
  batchDefaults();
  nextHelloMs = millis() + 500;

  Serial.println();
  Serial.println("# === ESP-NOW Baseline Receiver/Controller ===");
  Serial.print("# MAC receiver: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("# channel=%u baud=%lu log=%s\n", WIFI_CHANNEL, BAUD, logOk ? "LittleFS aktif" : "tidak aktif (CSV hanya ke Serial)");
  printHeaders();
  printHelp();
}

void loop() {
  pollSerial();
  uint32_t now = millis();

  switch (state) {
    case ST_IDLE:
      if (!batchActive && reached(nextHelloMs)) {
        sendHello();
        nextHelloMs = now + HELLO_PERIOD_MS;
      }
      if (batchActive && reached(nextRunAt)) startNextRun();
      break;

    case ST_STARTING:
      if (startsSent < 5 && reached(nextStartTx)) {
        int32_t remain = (int32_t)(tStartMs - now);
        sendStart(remain > 0 ? (uint32_t)remain : 0);
        startsSent++;
        nextStartTx = now + 150;
      }
      if (reached(tStartMs)) {
        tEndMs = tStartMs + (uint32_t)cfg.durationS * 1000UL;
        state = ST_RUNNING;
      }
      break;

    case ST_RUNNING:
      if (reached(tEndMs)) {
        tGraceEndMs = tEndMs + GRACE_MS;
        state = ST_GRACE;
      }
      break;

    case ST_GRACE:
      if (reached(tGraceEndMs)) {
        acceptData = false;
        acceptReports = true;
        repDeadlineMs = now + REPORT_WAIT_BASE_MS + REPORT_WAIT_PER_NODE_MS * cfg.nodes;
        state = ST_REPORTING;
      }
      break;

    case ST_REPORTING:
      if (reportsGot >= cfg.nodes || reached(repDeadlineMs)) finishRun();
      break;
  }
  delay(1);
}
