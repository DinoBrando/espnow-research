/*
  ESP-NOW EKSPERIMEN v2 - RECEIVER + PENGONTROL (ESP32-C6)

  Peran:
  1. Pengontrol eksperimen: START/STOP lewat broadcast, menjalankan matriks (batch).
  2. Pencatat metrik sisi penerima: paket unik, duplikat, terlambat, jitter, RSSI.
  3. Mengirim STATUS 1 kali per detik (per kelompok 10 node) di SEMUA metode:
     ACK bitmap, RSSI per node, perintah pindah kanal.
  4. Mengumpulkan laporan sender (status MAC, RTT, kecepatan radio, daya, energi, reset).

  Semua balasan ke sender (echo, ACK laporan, STATUS) dikirim BROADCAST berisi ID node tujuan,
  jadi receiver hanya butuh 1 peer (broadcast). Batas 20 peer ESP-NOW tidak tersentuh.

  PERINTAH SERIAL (akhiri dengan Enter):
    help | status | nodes | params | stop | dump | clearlog | header
    set key=value ...                 ubah parameter metode (lihat `params`)
    run N PAYLOAD INTERVAL_MS [DUR_S] [method=M] [probe=P] [mode=0|1] [jitter=J]
    batch [nodes=1,5,10,20] [payload=32,250] [interval=50] [method=0] [mode=0]
          [dur=60] [reps=3] [probe=20] [jitter=0]

  Metode: 0 baseline, 1 rate_control, 2 dynamic_interval, 3 dynamic_backoff,
          4 tx_power, 5 ack_retx, 6 channel_hopping

  Board: ESP32C6 Dev Module, core esp32 3.3.x.
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
const unsigned long BAUD = 921600;
const uint8_t  HOME_CHANNEL = 1;          // kanal awal setiap run, harus sama di sender
#define WIFI_POWER_SAVE 0
const char *LOG_PATH = "/exp_log.csv";

const uint32_t START_LEAD_MS = 1500;
const uint32_t GRACE_MS = 5000;           // paket yang tiba sampai 5 dtk setelah run tetap dihitung
const uint32_t REPORT_WAIT_BASE_MS = 3500;
const uint32_t REPORT_WAIT_PER_NODE_MS = 120;
const uint32_t GAP_BETWEEN_RUNS_MS = 1000;
const uint32_t HELLO_PERIOD_MS = 2000;
const uint32_t STATUS_PERIOD_MS = 1000;
const uint32_t HOP_HOLD_MS = 5000;        // jeda minimum antar pindah kanal
const uint32_t HOP_DELAY_MS = 500;        // pindah 0,5 dtk setelah diumumkan

// ===== tipe =====
enum State : uint8_t { ST_IDLE, ST_STARTING, ST_RUNNING, ST_GRACE, ST_REPORTING };

typedef struct {
  uint16_t runId, rep;
  uint8_t  nodes, payload, mode, method;
  uint16_t intervalMs, durationS, probeEvery, jitterMs;
} RunCfg;

typedef struct {
  bool     seen;
  uint16_t highest;
  uint64_t bm[2];               // bit i = seq (highest - i) diterima
  uint32_t rxUnique, rxDup, rxLate, rxRetx, bytes;
  bool     haveLast;
  uint32_t lastArrUs, lastTsUs, maxGapUs;
  int32_t  jitScaled;           // jitter RFC3550 x16
  int32_t  rssiSum;
  uint32_t rssiN;
  int8_t   rssiMin;
  int32_t  stRssiSum;           // jendela STATUS
  uint16_t stRssiN;
  uint16_t hopRx;               // jendela hop (1 dtk)
  uint8_t  hopFail;             // macFailWin terbaru di jendela hop
  uint8_t  bootChanges;
  bool     haveRep;
  ReportPkt rep;
} NodeRun;

typedef struct {
  uint32_t lastSeenMs;
  uint16_t fw;
  uint16_t bootId;
  uint8_t  resetReason;
  uint8_t  inaOk;
  uint8_t  channel;
  uint16_t resets;
} NodeInfo;

typedef struct {
  uint8_t  kind;                // 0 echo, 1 ACK laporan
  uint8_t  nodeId;
  uint16_t runId;
  uint16_t seq;
  uint32_t tsUs;
} EchoJob;

typedef struct {
  uint8_t  nodeId;
  uint16_t oldBoot, newBoot;
  uint8_t  reason;              // 0xFF = belum diketahui
} ResetEvent;

// ===== state global =====
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static volatile State state = ST_IDLE;
static volatile uint16_t curRunId = 0;
static volatile bool acceptData = false;
static volatile bool acceptReports = false;
static volatile bool runEnded = false;        // data setelah ini dihitung terlambat
static volatile uint32_t reportsGot = 0;
static volatile uint32_t echoDrop = 0, echoFail = 0;

static NodeRun run_[MAX_NODES + 1];
static NodeRun snap_[MAX_NODES + 1];
static NodeInfo info_[MAX_NODES + 1];
static RunCfg cfg;
static MethodParams gParams;
static QueueHandle_t echoQ;
static uint16_t runCounter;

static ResetEvent resetQ[16];
static volatile uint8_t resetHead = 0, resetTail = 0;

static uint32_t tStartMs, tEndMs, tGraceEndMs, repDeadlineMs;
static uint32_t nextStartTx, nextHelloMs, nextRunAt;
static uint8_t  startsSent;

static uint8_t  curChannel = HOME_CHANNEL;
static uint32_t nextStatusMs[4];
static uint8_t  hopId = 0, hopCount = 0, hopTarget = 0, hopAnnounceLeft = 0, hopBadWindows = 0;
static uint32_t hopAtMs = 0, hopHoldUntil = 0, nextHopEvalMs = 0, nextAnnounceMs = 0;
static bool     hopPending = false;

// batch
static bool batchActive = false;
static uint32_t batchIdx = 0, batchTotal = 0;
static uint16_t bNodes[8], bPayload[8], bInterval[8], bMode[2], bMethod[8];
static int nbNodes, nbPayload, nbInterval, nbMode, nbMethod;
static uint16_t bDur, bReps, bProbe, bJitter;

static bool logOk = false;
#if USE_LITTLEFS
static File logFile;
#endif

// ===== utilitas =====
static inline bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

static void bmShift(uint64_t bm[2], uint32_t k) {
  if (k == 0) return;
  if (k >= 128) { bm[0] = bm[1] = 0; return; }
  if (k >= 64) { bm[1] = bm[0] << (k - 64); bm[0] = 0; return; }
  bm[1] = (bm[1] << k) | (bm[0] >> (64 - k));
  bm[0] <<= k;
}
static inline bool bmGet(const uint64_t bm[2], uint32_t i) {
  return i < 64 ? (bm[0] >> i) & 1ULL : (bm[1] >> (i - 64)) & 1ULL;
}
static inline void bmSet(uint64_t bm[2], uint32_t i) {
  if (i < 64) bm[0] |= (1ULL << i); else bm[1] |= (1ULL << (i - 64));
}

static void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  curChannel = ch;
}

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
  emit("#HD,tag,fw_rx,fw_tx,method,run_id,rep,mode,nodes,payload,interval_ms,duration_s,probe_every,jitter_ms,"
       "node,report_ok,boot_changes,reset_reason,generated,skipped,tx_new,tx_retx,tx_api_err,tx_retx_api_err,"
       "mac_ok,mac_fail,mac_pending,rx_unique,rx_dup,rx_late,rx_via_retx,lost,pdr_gen_pct,pdr_sent_pct,"
       "goodput_bps,jitter_us,max_gap_ms,rssi_avg,rssi_min,probes,echoes,rtt_min_us,rtt_mean_us,rtt_std_us,"
       "rtt_max_us,age_mean_us,age_max_us,slips,rate_final_mbps,rate_mean_mbps,rate_changes,txp_final_dbm,"
       "txp_mean_dbm,txp_min_dbm,intv_final_ms,intv_mean_ms,cw_final_ms,backoff_mean_ms,ack_confirmed,"
       "ack_giveup,channel_final,hops,scans,ina_ok,energy_mj,i_avg_ma,v_avg_v,ina_samples");
  emit("#HS,tag,fw_rx,method,run_id,rep,mode,nodes,payload,interval_ms,duration_s,probe_every,jitter_ms,"
       "reports,generated,tx_new,rx_unique,rx_late,pdr_gen_pct,pdr_sent_pct,offered_bps,goodput_bps,"
       "jain_pdr,worst_node,worst_pdr_pct,jitter_us_mean,rtt_mean_us,mac_fail,rssi_avg,rx_hops,"
       "rx_channel_final,boot_changes");
  closeLog();
}

static void printParams(bool toLog) {
  char b[300];
  snprintf(b, sizeof(b),
           "#PARAMS,rc_down=%u,rc_up=%u,di_step=%u,di_max=%u,bo_cw0=%u,bo_cwmax=%u,tp_lo=%d,tp_hi=%d,"
           "tp_step=%u,ack_retries=%u,hop_fail=%u,hop_rx=%u,hop_win=%u,keepalive=%u,grace_ms=%lu,home_ch=%u,fw=%u.%u",
           gParams.rcDownPdr, gParams.rcUpS, gParams.diStepMs, gParams.diMaxMult, gParams.boCw0Ms,
           gParams.boCwMaxMs, gParams.tpLoDbm, gParams.tpHiDbm, gParams.tpStepDbm, gParams.ackRetries,
           gParams.hopFailPct, gParams.hopRxPct, gParams.hopWindows, gParams.keepaliveMs,
           (unsigned long)GRACE_MS, HOME_CHANNEL, FW_VERSION_MAJOR, FW_VERSION_MINOR);
  if (toLog) { openLog(); emit(b); closeLog(); }
  else Serial.println(b);
}

// ===== pengirim balasan (task terpisah) =====
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

// ===== deteksi reset node (dipanggil dari callback) =====
static void noteBoot(uint8_t id, uint16_t bootId, int reason) {
  NodeInfo &ni = info_[id];
  if (ni.bootId != 0 && ni.bootId != bootId) {
    ni.resets++;
    if (state != ST_IDLE) run_[id].bootChanges++;
    uint8_t nh = (resetHead + 1) % 16;
    if (nh != resetTail) {
      resetQ[resetHead].nodeId = id;
      resetQ[resetHead].oldBoot = ni.bootId;
      resetQ[resetHead].newBoot = bootId;
      resetQ[resetHead].reason = reason < 0 ? 0xFF : (uint8_t)reason;
      resetHead = nh;
    }
  }
  ni.bootId = bootId;
  if (reason >= 0) ni.resetReason = (uint8_t)reason;
}

// ===== penerimaan (task WiFi: singkat, tanpa Serial) =====
static void handleData(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(DataPkt)) return;
  DataPkt p;
  memcpy(&p, data, sizeof(p));
  uint8_t id = p.h.nodeId;
  if (id < 1 || id > MAX_NODES) return;
  uint32_t nowMs = millis();
  int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;

  portENTER_CRITICAL(&mux);
  info_[id].lastSeenMs = nowMs;
  noteBoot(id, p.bootId, -1);
  if (!acceptData || p.runId != curRunId) { portEXIT_CRITICAL(&mux); return; }

  uint32_t arr = (uint32_t)esp_timer_get_time();
  NodeRun &n = run_[id];
  bool isNew = false, inOrder = false;
  if (!n.seen) {
    n.seen = true;
    n.highest = p.seq;
    n.bm[0] = 1; n.bm[1] = 0;
    isNew = true; inOrder = true;
  } else if (p.seq > n.highest) {
    bmShift(n.bm, (uint32_t)(p.seq - n.highest));
    bmSet(n.bm, 0);
    n.highest = p.seq;
    isNew = true; inOrder = true;
  } else {
    uint32_t d = (uint32_t)(n.highest - p.seq);
    if (d < ACK_WINDOW) {
      if (bmGet(n.bm, d)) n.rxDup++;
      else { bmSet(n.bm, d); isNew = true; }
    } else {
      isNew = true;              // di luar jendela, tidak bisa dicek dobel (jarang)
    }
  }
  if (isNew) {
    n.rxUnique++;
    n.bytes += (uint32_t)len;
    if (runEnded) n.rxLate++;
    if (p.h.flags & FLAG_RETX) n.rxRetx++;
    if (inOrder && !(p.h.flags & FLAG_RETX)) {
      if (n.haveLast) {
        int32_t dd = (int32_t)((arr - n.lastArrUs) - (p.tsUs - n.lastTsUs));
        if (dd < 0) dd = -dd;
        n.jitScaled += dd - (n.jitScaled >> 4);
        uint32_t gap = arr - n.lastArrUs;
        if (gap > n.maxGapUs) n.maxGapUs = gap;
      }
      n.haveLast = true;
      n.lastArrUs = arr;
      n.lastTsUs = p.tsUs;
    }
    if (n.rssiN == 0 || rssi < n.rssiMin) n.rssiMin = rssi;
    n.rssiSum += rssi;
    n.rssiN++;
    n.stRssiSum += rssi;
    n.stRssiN++;
    n.hopRx++;
    n.hopFail = p.macFailWin;
  }
  portEXIT_CRITICAL(&mux);

  if ((p.h.flags & FLAG_PROBE) && !(p.h.flags & FLAG_RETX)) {
    EchoJob j = {0, id, p.runId, p.seq, p.tsUs};
    if (xQueueSend(echoQ, &j, 0) != pdTRUE) echoDrop++;
  }
}

static void handleHello(const uint8_t *data, int len) {
  if (len < (int)sizeof(HelloPkt)) return;
  HelloPkt h;
  memcpy(&h, data, sizeof(h));
  uint8_t id = h.h.nodeId;
  if (id < 1 || id > MAX_NODES) return;
  portENTER_CRITICAL(&mux);
  NodeInfo &ni = info_[id];
  ni.lastSeenMs = millis();
  ni.fw = h.fw;
  ni.inaOk = h.inaOk;
  ni.channel = h.channel;
  noteBoot(id, h.bootId, h.resetReason);
  portEXIT_CRITICAL(&mux);
}

static void handleReport(const uint8_t *data, int len) {
  if (len < (int)sizeof(ReportPkt)) return;
  ReportPkt r;
  memcpy(&r, data, sizeof(r));
  uint8_t id = r.h.nodeId;
  if (id < 1 || id > MAX_NODES) return;

  EchoJob j = {1, id, r.runId, 0, 0};
  if (xQueueSend(echoQ, &j, 0) != pdTRUE) echoDrop++;

  portENTER_CRITICAL(&mux);
  info_[id].lastSeenMs = millis();
  info_[id].fw = r.fw;
  info_[id].inaOk = r.inaOk;
  noteBoot(id, r.bootId, r.resetReason);
  if (acceptReports && r.runId == curRunId && !run_[id].haveRep) {
    run_[id].rep = r;
    run_[id].haveRep = true;
    reportsGot++;
  }
  portEXIT_CRITICAL(&mux);
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(PktHdr) || data[0] != PROTO_MAGIC) return;
  switch (data[1]) {
    case PKT_DATA:   handleData(info, data, len); break;
    case PKT_HELLO:  handleHello(data, len); break;
    case PKT_REPORT: handleReport(data, len); break;
    default: break;
  }
}

// ===== kontrol =====
static void sendHello() {
  HelloPkt h;
  memset(&h, 0, sizeof(h));
  h.h = makeHdr(PKT_HELLO, 0, 0);
  h.fw = FW_VERSION;
  h.channel = curChannel;
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
  s.method = cfg.method;
  s.intervalMs = cfg.intervalMs;
  s.durationS = cfg.durationS;
  s.probeEvery = cfg.probeEvery;
  s.jitterMs = cfg.jitterMs;
  s.startInMs = (uint16_t)startInMs;
  s.graceMs = (uint16_t)GRACE_MS;
  s.homeChannel = HOME_CHANNEL;
  s.p = gParams;
  esp_now_send(BCAST, (const uint8_t *)&s, sizeof(s));
}

static void sendStop() {
  StopPkt s;
  s.h = makeHdr(PKT_STOP, 0, 0);
  s.runId = cfg.runId;
  esp_now_send(BCAST, (const uint8_t *)&s, sizeof(s));
}

static void sendStatusGroup(uint8_t g) {
  StatusPkt sp;
  memset(&sp, 0, sizeof(sp));
  uint8_t first = 1 + g * STATUS_NODES_PER_FRAME;
  if (first > cfg.nodes) return;
  uint8_t count = cfg.nodes - first + 1;
  if (count > STATUS_NODES_PER_FRAME) count = STATUS_NODES_PER_FRAME;
  sp.s.h = makeHdr(PKT_STATUS, 0, 0);
  sp.s.runId = cfg.runId;
  sp.s.firstNode = first;
  sp.s.count = count;
  sp.s.curChannel = curChannel;
  if (hopPending) {
    int32_t left = (int32_t)(hopAtMs - millis());
    sp.s.hopChannel = hopTarget;
    sp.s.hopInMs = left > 0 ? (uint16_t)left : 0;
    sp.s.hopId = hopId;
  }
  portENTER_CRITICAL(&mux);
  for (uint8_t k = 0; k < count; k++) {
    NodeRun &n = run_[first + k];
    StatusEntry &e = sp.e[k];
    e.highest = n.seen ? n.highest : NO_SEQ;
    for (int b = 0; b < 16; b++) {
      uint64_t w = b < 8 ? n.bm[0] : n.bm[1];
      e.bitmap[b] = (uint8_t)((w >> (8 * (b % 8))) & 0xFF);
    }
    e.rssi = n.stRssiN ? (int8_t)(n.stRssiSum / (int32_t)n.stRssiN) : 0;
    n.stRssiSum = 0;
    n.stRssiN = 0;
  }
  portEXIT_CRITICAL(&mux);
  size_t len = sizeof(StatusHdr) + count * sizeof(StatusEntry);
  esp_now_send(BCAST, (const uint8_t *)&sp, len);
}

static uint8_t numGroups() {
  return (cfg.nodes + STATUS_NODES_PER_FRAME - 1) / STATUS_NODES_PER_FRAME;
}

static void scheduleStatus(uint32_t base) {
  uint8_t g = numGroups();
  for (uint8_t i = 0; i < g && i < 4; i++) nextStatusMs[i] = base + i * (STATUS_PERIOD_MS / g);
}

static void serviceStatus() {
  uint8_t g = numGroups();
  for (uint8_t i = 0; i < g && i < 4; i++) {
    if (reached(nextStatusMs[i])) {
      sendStatusGroup(i);
      nextStatusMs[i] += STATUS_PERIOD_MS;
    }
  }
  if (hopAnnounceLeft && reached(nextAnnounceMs)) {     // pengumuman pindah kanal ekstra
    for (uint8_t i = 0; i < g && i < 4; i++) sendStatusGroup(i);
    hopAnnounceLeft--;
    nextAnnounceMs = millis() + 80;
  }
}

// Channel Hopping: keputusan pindah kanal (hanya metode 6, saat RUNNING)
static void serviceHop() {
  if (cfg.method != M_CHANHOP) return;
  if (hopPending && reached(hopAtMs)) {
    setChannel(hopTarget);
    hopPending = false;
    hopCount++;
    hopHoldUntil = millis() + HOP_HOLD_MS;
    hopBadWindows = 0;
  }
  if (state != ST_RUNNING || !reached(nextHopEvalMs)) return;
  nextHopEvalMs += 1000;
  uint32_t sumRx = 0, sumFail = 0;
  portENTER_CRITICAL(&mux);
  for (int id = 1; id <= cfg.nodes; id++) {
    sumRx += run_[id].hopRx;
    sumFail += run_[id].hopRx ? run_[id].hopFail : 0;
    run_[id].hopRx = 0;
    run_[id].hopFail = 0;
  }
  portEXIT_CRITICAL(&mux);
  uint32_t expected = (uint32_t)cfg.nodes * 1000UL / cfg.intervalMs;
  bool bad = false;
  if (sumFail + sumRx > 0 && sumFail * 100 >= (uint32_t)gParams.hopFailPct * (sumFail + sumRx)) bad = true;
  if (expected > 0 && sumRx * 100 < (uint32_t)gParams.hopRxPct * expected) bad = true;
  hopBadWindows = bad ? hopBadWindows + 1 : 0;
  if (!hopPending && hopBadWindows >= gParams.hopWindows && reached(hopHoldUntil)) {
    uint8_t idx = 0;
    for (uint8_t i = 0; i < NUM_HOP_CH; i++) if (HOP_CHANNELS[i] == curChannel) idx = i;
    hopTarget = HOP_CHANNELS[(idx + 1) % NUM_HOP_CH];
    hopId++;
    hopAtMs = millis() + HOP_DELAY_MS;
    hopPending = true;
    hopAnnounceLeft = 5;
    nextAnnounceMs = millis();
    hopBadWindows = 0;
  }
}

static void startRun(const RunCfg &c) {
  cfg = c;
  runCounter++;
  cfg.runId = runCounter;
  curRunId = cfg.runId;

  portENTER_CRITICAL(&mux);
  memset(run_, 0, sizeof(run_));
  reportsGot = 0;
  runEnded = false;
  portEXIT_CRITICAL(&mux);
  echoDrop = 0; echoFail = 0;
  if (curChannel != HOME_CHANNEL) setChannel(HOME_CHANNEL);
  hopId = 0; hopCount = 0; hopPending = false; hopAnnounceLeft = 0; hopBadWindows = 0; hopHoldUntil = 0;

  acceptData = true;
  acceptReports = false;
  startsSent = 0;
  nextStartTx = millis();
  tStartMs = millis() + START_LEAD_MS;
  state = ST_STARTING;

  Serial.printf("# RUN id=%u rep=%u method=%u(%s) nodes=%u payload=%u interval=%ums dur=%us probe=%u mode=%u\n",
                (unsigned)cfg.runId, (unsigned)cfg.rep, (unsigned)cfg.method, methodName(cfg.method),
                (unsigned)cfg.nodes, (unsigned)cfg.payload, (unsigned)cfg.intervalMs,
                (unsigned)cfg.durationS, (unsigned)cfg.probeEvery, (unsigned)cfg.mode);
}

static void finishRun() {
  portENTER_CRITICAL(&mux);
  memcpy(snap_, run_, sizeof(run_));
  portEXIT_CRITICAL(&mux);
  acceptReports = false;
  acceptData = false;
  uint8_t chFinal = curChannel;

  openLog();
  static char b[900];
  const float dur = (float)cfg.durationS;
  uint32_t totGen = 0, totNew = 0, totRx = 0, totLate = 0, totMacFail = 0, nRep = 0, totBoot = 0;
  float sumX = 0, sumX2 = 0, worstPdr = 1000.0f;
  int nX = 0, worstNode = 0, jitN = 0;
  double jitSum = 0, rttW = 0, rssiSum = 0;
  uint32_t rttCnt = 0, rssiN = 0;

  for (int id = 1; id <= cfg.nodes; id++) {
    const NodeRun &n = snap_[id];
    const bool rep = n.haveRep;
    const ReportPkt &r = n.rep;
    uint32_t generated = rep ? r.generated : (n.seen ? (uint32_t)n.highest + 1 : 0);
    uint32_t txNew = rep ? r.sentNew : (n.seen ? (uint32_t)n.highest + 1 : 0);
    uint32_t lost = txNew > n.rxUnique ? txNew - n.rxUnique : 0;
    float pdrGen = generated ? 100.0f * n.rxUnique / generated : 0.0f;
    float pdrSent = txNew ? 100.0f * n.rxUnique / txNew : 0.0f;
    if (pdrGen > 100.0f) pdrGen = 100.0f;
    if (pdrSent > 100.0f) pdrSent = 100.0f;
    float goodput = (cfg.payload * 8.0f * n.rxUnique) / dur;
    float jit = n.jitScaled / 16.0f;
    float rssiAvg = n.rssiN ? (float)n.rssiSum / n.rssiN : 0.0f;
    int rssiMin = n.rssiN ? n.rssiMin : 0;
    float rateFinal = rep ? RATE_MBPS_X10[r.rateFinalIdx < NUM_RATES ? r.rateFinalIdx : 0] / 10.0f : 0.0f;

    snprintf(b, sizeof(b),
             "D,%u.%u,%u.%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%s,%lu,%u,%lu,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,"
             "%.2f,%.2f,%.0f,%.1f,%.2f,%.1f,%d,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%u,%.1f,%.1f,%u,%.2f,%.2f,%.2f,"
             "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%.2f,%.3f,%u",
             FW_VERSION_MAJOR, FW_VERSION_MINOR,
             rep ? (unsigned)(r.fw >> 8) : 0u, rep ? (unsigned)(r.fw & 0xFF) : 0u,
             (unsigned)cfg.method, (unsigned)cfg.runId, (unsigned)cfg.rep, (unsigned)cfg.mode,
             (unsigned)cfg.nodes, (unsigned)cfg.payload, (unsigned)cfg.intervalMs, (unsigned)cfg.durationS,
             (unsigned)cfg.probeEvery, (unsigned)cfg.jitterMs, id, rep ? 1 : 0,
             (unsigned)n.bootChanges, rep ? resetName(r.resetReason) : "NA",
             (unsigned long)generated, rep ? r.skipped : 0, (unsigned long)txNew, rep ? r.retx : 0,
             rep ? r.apiErr : 0, rep ? r.retxApiErr : 0, rep ? r.macOk : 0, rep ? r.macFail : 0,
             rep ? r.macPending : 0,
             (unsigned long)n.rxUnique, (unsigned long)n.rxDup, (unsigned long)n.rxLate,
             (unsigned long)n.rxRetx, (unsigned long)lost,
             pdrGen, pdrSent, goodput, jit, n.maxGapUs / 1000.0f, rssiAvg, rssiMin,
             rep ? r.probes : 0, rep ? r.echoes : 0,
             (unsigned long)(rep ? r.rttMinUs : 0), (unsigned long)(rep ? r.rttMeanUs : 0),
             (unsigned long)(rep ? r.rttStdUs : 0), (unsigned long)(rep ? r.rttMaxUs : 0),
             (unsigned long)(rep ? r.ageMeanUs : 0), (unsigned long)(rep ? r.ageMaxUs : 0),
             rep ? r.slips : 0,
             rateFinal, rep ? r.rateMeanX10 / 10.0f : 0.0f, rep ? r.rateChanges : 0,
             rep ? r.txpFinalQ / 4.0f : 0.0f, rep ? r.txpMeanQ / 4.0f : 0.0f, rep ? r.txpMinQ / 4.0f : 0.0f,
             rep ? r.intvFinalMs : 0, rep ? r.intvMeanMs : 0, rep ? r.cwFinalMs : 0, rep ? r.backoffMeanMs : 0,
             rep ? r.ackConfirmed : 0, rep ? r.ackGiveUp : 0,
             rep ? r.channelFinal : 0, rep ? r.hops : 0, rep ? r.scans : 0,
             rep ? r.inaOk : 0, (unsigned long)(rep ? r.energyMj : 0), rep ? r.iAvgUa / 1000.0f : 0.0f,
             rep ? r.vAvgMv / 1000.0f : 0.0f, rep ? r.inaSamples : 0);
    emit(b);

    totGen += generated;
    totNew += txNew;
    totRx += n.rxUnique;
    totLate += n.rxLate;
    totBoot += n.bootChanges;
    if (rep) { nRep++; totMacFail += r.macFail; }
    if (generated > 0) {
      sumX += pdrGen; sumX2 += pdrGen * pdrGen; nX++;
      if (pdrGen < worstPdr) { worstPdr = pdrGen; worstNode = id; }
    }
    if (n.rxUnique > 1) { jitSum += jit; jitN++; }
    if (rep && r.echoes > 0) { rttW += (double)r.rttMeanUs * r.echoes; rttCnt += r.echoes; }
    rssiSum += n.rssiSum;
    rssiN += n.rssiN;
  }

  float pdrGenAll = totGen ? 100.0f * totRx / totGen : 0.0f;
  float pdrSentAll = totNew ? 100.0f * totRx / totNew : 0.0f;
  if (pdrGenAll > 100.0f) pdrGenAll = 100.0f;
  if (pdrSentAll > 100.0f) pdrSentAll = 100.0f;
  float offered = (totGen * cfg.payload * 8.0f) / dur;
  float goodAgg = (totRx * cfg.payload * 8.0f) / dur;
  float jain = (nX > 0 && sumX2 > 0) ? (sumX * sumX) / ((float)nX * sumX2) : 0.0f;
  if (nX == 0) worstPdr = 0;

  snprintf(b, sizeof(b),
           "S,%u.%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f,%.0f,%.0f,%.3f,%d,%.2f,%.1f,%.0f,%lu,%.1f,%u,%u,%lu",
           FW_VERSION_MAJOR, FW_VERSION_MINOR, (unsigned)cfg.method, (unsigned)cfg.runId, (unsigned)cfg.rep,
           (unsigned)cfg.mode, (unsigned)cfg.nodes, (unsigned)cfg.payload, (unsigned)cfg.intervalMs,
           (unsigned)cfg.durationS, (unsigned)cfg.probeEvery, (unsigned)cfg.jitterMs,
           (unsigned long)nRep, (unsigned long)totGen, (unsigned long)totNew, (unsigned long)totRx,
           (unsigned long)totLate, pdrGenAll, pdrSentAll, offered, goodAgg, jain, worstNode, worstPdr,
           jitN ? (float)(jitSum / jitN) : 0.0f, rttCnt ? (float)(rttW / rttCnt) : 0.0f,
           (unsigned long)totMacFail, rssiN ? (float)(rssiSum / rssiN) : 0.0f,
           (unsigned)hopCount, (unsigned)chFinal, (unsigned long)totBoot);
  emit(b);
  closeLog();

  Serial.printf("# selesai run %u: laporan %lu/%u, echo_drop=%lu echo_fail=%lu, kanal akhir=%u, pindah=%u\n",
                (unsigned)cfg.runId, (unsigned long)nRep, (unsigned)cfg.nodes,
                (unsigned long)echoDrop, (unsigned long)echoFail, (unsigned)chFinal, (unsigned)hopCount);

  if (curChannel != HOME_CHANNEL) setChannel(HOME_CHANNEL);
  hopPending = false;
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
  uint32_t combos = (uint32_t)nbNodes * nbPayload * nbInterval * nbMode * nbMethod;
  uint32_t rep = batchIdx / combos;
  uint32_t c = batchIdx % combos;
  int iMe = c % nbMethod;   c /= nbMethod;
  int iMo = c % nbMode;     c /= nbMode;
  int iI = c % nbInterval;  c /= nbInterval;
  int iP = c % nbPayload;   c /= nbPayload;
  int iN = (int)c;

  RunCfg r;
  memset(&r, 0, sizeof(r));
  r.rep = (uint16_t)(rep + 1);
  r.nodes = (uint8_t)bNodes[iN];
  r.payload = (uint8_t)bPayload[iP];
  r.intervalMs = bInterval[iI];
  r.mode = (uint8_t)bMode[iMo];
  r.method = (uint8_t)bMethod[iMe];
  r.durationS = bDur;
  r.probeEvery = bProbe;
  r.jitterMs = bJitter;
  Serial.printf("# batch %lu/%lu\n", (unsigned long)(batchIdx + 1), (unsigned long)batchTotal);
  startRun(r);
}

// ===== perintah =====
static void batchDefaults() {
  bNodes[0] = 1; bNodes[1] = 5; bNodes[2] = 10; bNodes[3] = 20; nbNodes = 4;
  bPayload[0] = 32; bPayload[1] = 250; nbPayload = 2;
  bInterval[0] = 50; nbInterval = 1;
  bMode[0] = 0; nbMode = 1;
  bMethod[0] = 0; nbMethod = 1;
  bDur = 60; bReps = 3; bProbe = 20; bJitter = 0;
}

static int parseList(const char *s, uint16_t *out, int maxn) {
  int n = 0;
  char tmp[64];
  strncpy(tmp, s, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  char *save = nullptr;
  for (char *t = strtok_r(tmp, ",", &save); t && n < maxn; t = strtok_r(nullptr, ",", &save)) {
    int v = atoi(t);
    out[n++] = (uint16_t)(v < 0 ? 0 : v);
  }
  return n;
}

// parameter metode: kembalikan true jika key dikenali
static bool setParam(const char *k, const char *v) {
  int x = atoi(v);
  if (!strcmp(k, "rc_down")) gParams.rcDownPdr = (uint8_t)constrain(x, 1, 100);
  else if (!strcmp(k, "rc_up")) gParams.rcUpS = (uint8_t)constrain(x, 1, 60);
  else if (!strcmp(k, "di_step")) gParams.diStepMs = (uint8_t)constrain(x, 1, 250);
  else if (!strcmp(k, "di_max")) gParams.diMaxMult = (uint8_t)constrain(x, 1, 50);
  else if (!strcmp(k, "bo_cw0")) gParams.boCw0Ms = (uint16_t)constrain(x, 1, 5000);
  else if (!strcmp(k, "bo_cwmax")) gParams.boCwMaxMs = (uint16_t)constrain(x, 1, 10000);
  else if (!strcmp(k, "tp_lo")) gParams.tpLoDbm = (int8_t)constrain(x, -100, 0);
  else if (!strcmp(k, "tp_hi")) gParams.tpHiDbm = (int8_t)constrain(x, -100, 0);
  else if (!strcmp(k, "tp_step")) gParams.tpStepDbm = (uint8_t)constrain(x, 1, 10);
  else if (!strcmp(k, "ack_retries")) gParams.ackRetries = (uint8_t)constrain(x, 0, 5);
  else if (!strcmp(k, "hop_fail")) gParams.hopFailPct = (uint8_t)constrain(x, 1, 100);
  else if (!strcmp(k, "hop_rx")) gParams.hopRxPct = (uint8_t)constrain(x, 0, 100);
  else if (!strcmp(k, "hop_win")) gParams.hopWindows = (uint8_t)constrain(x, 1, 30);
  else if (!strcmp(k, "keepalive")) gParams.keepaliveMs = (uint16_t)constrain(x, 500, 30000);
  else return false;
  return true;
}

static void printHelp() {
  Serial.println("# perintah: help | status | nodes | params | stop | dump | clearlog | header");
  Serial.println("#   set key=value ...   (rc_down rc_up di_step di_max bo_cw0 bo_cwmax tp_lo tp_hi tp_step");
  Serial.println("#                        ack_retries hop_fail hop_rx hop_win keepalive)");
  Serial.println("#   run N PAYLOAD INTERVAL_MS [DUR_S] [method=M] [probe=P] [mode=0|1] [jitter=J]");
  Serial.println("#   batch [nodes=..] [payload=..] [interval=..] [method=..] [mode=..] [dur=..] [reps=..] [probe=..] [jitter=..]");
  Serial.println("#   metode: 0 baseline 1 rate_control 2 dynamic_interval 3 dynamic_backoff 4 tx_power 5 ack_retx 6 channel_hopping");
}

static void printNodes() {
  uint32_t now = millis();
  int cnt = 0;
  Serial.println("# node  umur(s)  fw    reset_terakhir  ina  kanal  reset_terhitung");
  portENTER_CRITICAL(&mux);
  NodeInfo copy[MAX_NODES + 1];
  memcpy(copy, info_, sizeof(copy));
  portEXIT_CRITICAL(&mux);
  for (int id = 1; id <= MAX_NODES; id++) {
    const NodeInfo &ni = copy[id];
    if (ni.lastSeenMs == 0) continue;
    Serial.printf("# N%02d   %6lu   %u.%u   %-14s  %u    %2u     %u%s\n", id,
                  (unsigned long)((now - ni.lastSeenMs) / 1000), ni.fw >> 8, ni.fw & 0xFF,
                  resetName(ni.resetReason), ni.inaOk, ni.channel, ni.resets,
                  ni.fw != FW_VERSION ? "   <-- VERSI BEDA" : "");
    cnt++;
  }
  Serial.printf("# total=%d\n", cnt);
}

static void printStatus() {
  static const char *names[] = {"IDLE", "STARTING", "RUNNING", "GRACE", "REPORTING"};
  Serial.printf("# fw=%u.%u state=%s runId=%u batch=%s %lu/%lu logOk=%d kanal=%u\n",
                FW_VERSION_MAJOR, FW_VERSION_MINOR, names[state], (unsigned)curRunId,
                batchActive ? "aktif" : "tidak", (unsigned long)batchIdx, (unsigned long)batchTotal,
                logOk ? 1 : 0, curChannel);
#if USE_LITTLEFS
  if (logOk) {
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) { Serial.printf("# ukuran log: %lu byte\n", (unsigned long)f.size()); f.close(); }
    Serial.printf("# flash: terpakai %lu / %lu byte\n",
                  (unsigned long)LittleFS.usedBytes(), (unsigned long)LittleFS.totalBytes());
  }
#endif
  printParams(false);
  printNodes();
}

static void cmdSet(char *args) {
  char *save = nullptr;
  int ok = 0;
  for (char *t = strtok_r(args, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
    char *eq = strchr(t, '=');
    if (!eq) continue;
    *eq = 0;
    if (setParam(t, eq + 1)) ok++;
    else Serial.printf("# parameter tidak dikenal: %s\n", t);
  }
  if (ok) printParams(false);
}

static void cmdRun(char *args) {
  if (state != ST_IDLE || batchActive) { Serial.println("# sedang berjalan, ketik stop dulu"); return; }
  int pos[4] = {0, 0, 0, 60};
  int np = 0, method = 0, probe = 20, mode = 0, jitter = 0;
  char *save = nullptr;
  for (char *t = strtok_r(args, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
    char *eq = strchr(t, '=');
    if (eq) {
      *eq = 0;
      int v = atoi(eq + 1);
      if (!strcmp(t, "method")) method = v;
      else if (!strcmp(t, "probe")) probe = v;
      else if (!strcmp(t, "mode")) mode = v;
      else if (!strcmp(t, "jitter")) jitter = v;
      else if (!setParam(t, eq + 1)) Serial.printf("# parameter tidak dikenal: %s\n", t);
    } else if (np < 4) {
      pos[np++] = atoi(t);
    }
  }
  if (np < 3 || pos[0] < 1 || pos[0] > MAX_NODES || pos[2] < 1 || method < 0 || method >= M_COUNT) {
    Serial.println("# format: run N PAYLOAD INTERVAL_MS [DUR_S] [method=M] [probe=P] [mode=0|1] [jitter=J]");
    return;
  }
  RunCfg r;
  memset(&r, 0, sizeof(r));
  r.rep = 1;
  r.nodes = (uint8_t)pos[0];
  r.payload = (uint8_t)constrain(pos[1], MIN_PAYLOAD, ESPNOW_MAX_PAYLOAD);
  r.intervalMs = (uint16_t)pos[2];
  r.durationS = (uint16_t)(pos[3] < 1 ? 1 : pos[3]);
  r.probeEvery = (uint16_t)(probe < 0 ? 0 : probe);
  r.mode = (uint8_t)(mode ? 1 : 0);
  r.method = (uint8_t)method;
  r.jitterMs = (uint16_t)(jitter < 0 ? 0 : jitter);
  printHeaders();
  printParams(true);
  startRun(r);
}

static void cmdBatch(char *args) {
  if (state != ST_IDLE || batchActive) { Serial.println("# sedang berjalan, ketik stop dulu"); return; }
  batchDefaults();
  char *save = nullptr;
  for (char *t = strtok_r(args, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
    char *eq = strchr(t, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = t, *val = eq + 1;
    if (!strcmp(k, "nodes")) nbNodes = parseList(val, bNodes, 8);
    else if (!strcmp(k, "payload")) nbPayload = parseList(val, bPayload, 8);
    else if (!strcmp(k, "interval")) nbInterval = parseList(val, bInterval, 8);
    else if (!strcmp(k, "mode")) nbMode = parseList(val, bMode, 2);
    else if (!strcmp(k, "method")) nbMethod = parseList(val, bMethod, 8);
    else if (!strcmp(k, "dur")) bDur = (uint16_t)atoi(val);
    else if (!strcmp(k, "reps")) bReps = (uint16_t)atoi(val);
    else if (!strcmp(k, "probe")) bProbe = (uint16_t)atoi(val);
    else if (!strcmp(k, "jitter")) bJitter = (uint16_t)atoi(val);
    else if (!setParam(k, val)) Serial.printf("# parameter tidak dikenal: %s\n", k);
  }
  if (nbNodes < 1 || nbPayload < 1 || nbInterval < 1 || nbMode < 1 || nbMethod < 1 || bReps < 1 || bDur < 1) {
    Serial.println("# argumen batch tidak valid");
    return;
  }
  uint16_t maxNode = 0;
  for (int i = 0; i < nbNodes; i++) {
    if (bNodes[i] < 1 || bNodes[i] > MAX_NODES) { Serial.println("# nodes harus 1..32"); return; }
    if (bNodes[i] > maxNode) maxNode = bNodes[i];
  }
  for (int i = 0; i < nbMethod; i++) {
    if (bMethod[i] >= M_COUNT) { Serial.println("# method harus 0..6"); return; }
  }
  for (int i = 0; i < nbPayload; i++) bPayload[i] = (uint16_t)constrain((int)bPayload[i], MIN_PAYLOAD, ESPNOW_MAX_PAYLOAD);
  for (int i = 0; i < nbInterval; i++) if (bInterval[i] < 1) bInterval[i] = 1;
  for (int i = 0; i < nbMode; i++) bMode[i] = bMode[i] ? 1 : 0;

  batchTotal = (uint32_t)bReps * nbNodes * nbPayload * nbInterval * nbMode * nbMethod;
  batchIdx = 0;
  uint32_t perRun = bDur + (START_LEAD_MS + GRACE_MS + REPORT_WAIT_BASE_MS / 2 + GAP_BETWEEN_RUNS_MS) / 1000;
  uint32_t estS = batchTotal * perRun;
  Serial.printf("# BATCH: %lu run, perkiraan %lu menit\n", (unsigned long)batchTotal, (unsigned long)(estS / 60));

  uint32_t now = millis();
  int missing = 0, wrongFw = 0;
  Serial.print("# node belum terdengar (<15 dtk):");
  for (int id = 1; id <= maxNode; id++) {
    if (info_[id].lastSeenMs == 0 || (now - info_[id].lastSeenMs) > 15000) { Serial.printf(" N%d", id); missing++; }
    else if (info_[id].fw != FW_VERSION) wrongFw++;
  }
  Serial.println(missing ? "  <-- cek daya dan ID node tersebut" : " (tidak ada)");
  if (wrongFw) Serial.printf("# PERINGATAN: %d node memakai versi firmware berbeda, cek dengan `nodes`\n", wrongFw);

  printHeaders();
  printParams(true);
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
  hopPending = false;
  if (curChannel != HOME_CHANNEL) setChannel(HOME_CHANNEL);
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
  else if (!strcmp(tok, "params")) printParams(false);
  else if (!strcmp(tok, "set")) cmdSet(rest);
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

static void printResetEvents() {
  while (resetTail != resetHead) {
    ResetEvent e = resetQ[resetTail];
    resetTail = (resetTail + 1) % 16;
    char b[120];
    snprintf(b, sizeof(b), "# RESET N%02u boot %04X -> %04X alasan=%s run=%u", e.nodeId, e.oldBoot, e.newBoot,
             e.reason == 0xFF ? "(menunggu HELLO)" : resetName(e.reason), (unsigned)curRunId);
    if (state != ST_IDLE) { openLog(); emit(b); closeLog(); }
    else Serial.println(b);
  }
}

// ===== setup / loop =====
void setup() {
  Serial.begin(BAUD);
  delay(1500);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  setChannel(HOME_CHANNEL);
#if !WIFI_POWER_SAVE
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif

  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW init gagal");
    while (true) delay(1000);
  }
  echoQ = xQueueCreate(64, sizeof(EchoJob));
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

  defaultParams(gParams);
  runCounter = (uint16_t)esp_random();
  batchDefaults();
  nextHelloMs = millis() + 500;

  Serial.println();
  Serial.printf("# === ESP-NOW Receiver/Controller v%u.%u ===\n", FW_VERSION_MAJOR, FW_VERSION_MINOR);
  Serial.print("# MAC receiver: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("# kanal awal=%u baud=%lu log=%s\n", HOME_CHANNEL, BAUD,
                logOk ? "LittleFS aktif" : "tidak aktif (CSV hanya ke Serial)");
  printHelp();
}

void loop() {
  pollSerial();
  printResetEvents();
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
        scheduleStatus(tStartMs + 1000);
        nextHopEvalMs = tStartMs + 1000;
        state = ST_RUNNING;
      }
      break;

    case ST_RUNNING:
      serviceStatus();
      serviceHop();
      if (reached(tEndMs)) {
        runEnded = true;
        tGraceEndMs = tEndMs + GRACE_MS;
        state = ST_GRACE;
      }
      break;

    case ST_GRACE:
      serviceStatus();          // ACK bitmap tetap dikirim selama masa tenggang
      serviceHop();             // menyelesaikan perpindahan yang sudah diumumkan
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
