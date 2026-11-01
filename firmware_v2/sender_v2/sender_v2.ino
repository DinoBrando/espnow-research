/*
  ESP-NOW EKSPERIMEN v2 - SENDER (satu firmware untuk semua node)

  - Metode dipilih receiver lewat paket START, tidak perlu flash ulang:
      0 baseline           kirim setiap sampel segera
      1 rate_control       kecepatan bit radio (1..54 Mbps) berdasarkan PDR link tiap 1 dtk
      2 dynamic_interval   interval kirim membesar saat ada loss, mengecil saat lancar
      3 dynamic_backoff    jeda acak 0..CW sebelum kirim, CW x2 saat gagal, kembali saat berhasil
      4 tx_power           daya pancar diatur agar RSSI di receiver berada di target
      5 ack_retx           kirim ulang paket yang tidak tercantum di ACK bitmap STATUS
      6 channel_hopping    ikut pindah kanal sesuai perintah receiver, mencari receiver jika hilang
  - Data dibangkitkan tiap interval nominal. Hanya sampel terbaru yang disimpan (yang lama ditimpa).
  - Node dengan INA219 (alamat 0x40 di SDA 21 / SCL 22) mengukur energinya sendiri selama run.
  - Alasan reset (mis. BROWNOUT) dilaporkan ke receiver.

  SET ID (sekali per board, tersimpan di flash, tidak hilang saat upload ulang):
    Serial Monitor 115200, ketik:  id 7
  Perintah lain: status | ina | help

  Board: sesuai jenis ESP32 sender, core esp32 3.3.x.
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "espnow_proto.h"
#include "sender_compat.h"

#define FORCE_NODE_ID 0
#define WIFI_POWER_SAVE 0
const unsigned long BAUD = 115200;
const uint8_t HOME_CHANNEL = 1;           // harus sama dengan receiver

// INA219
#define INA_ENABLE 1
const uint8_t INA_ADDR = 0x40;
const int INA_SDA = 21;
const int INA_SCL = 22;
const float INA_SHUNT_OHM = 0.1f;         // resistor shunt modul (umumnya R100 = 0,1 ohm)
const uint32_t INA_PERIOD_MS = 100;

static const wifi_phy_rate_t RATE_PHY[NUM_RATES] = {
  WIFI_PHY_RATE_1M_L, WIFI_PHY_RATE_2M_L, WIFI_PHY_RATE_5M_L, WIFI_PHY_RATE_11M_L,
  WIFI_PHY_RATE_24M, WIFI_PHY_RATE_54M
};

enum SState : uint8_t { S_IDLE, S_WAIT_START, S_RUNNING, S_DRAIN, S_REPORTING };

// ===== identitas =====
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static Preferences prefs;
static uint8_t nodeId = 0;
static uint16_t bootId = 1;
static uint8_t resetReason = 0;
static bool inaOk = false;
static uint8_t rxMac[6];
static volatile bool haveRx = false;
static volatile bool needPeer = false;
static uint8_t curChannel = HOME_CHANNEL;

// ===== run =====
static volatile SState st = S_IDLE;
static volatile bool cValid = false;
static volatile uint16_t cRunId = 0;
static volatile bool abortFlag = false;
static volatile bool reportAcked = false;
static uint8_t cPayload = 32, cMode = 0, cMethod = 0;
static uint16_t cInterval = 50, cDur = 60, cProbe = 0, cJitter = 0, cGraceMs = 5000;
static MethodParams P;
static uint32_t startAtMs, endAtMs, quietUntilMs = 0, nextHelloMs = 0, reportAtMs = 0, drainUntilMs = 0;
static uint8_t reportTries = 0;
static uint32_t elapsedMs = 0;
static uint8_t txBuf[ESPNOW_MAX_PAYLOAD];

// pembangkitan data
static int64_t runT0Us, nextGenUs;
static uint16_t sampleId, seqNo;
static struct { bool valid; uint16_t id; int64_t genUs; } pending;

// penghitung (sebagian diubah dari callback WiFi)
static volatile bool counting = false;
static volatile uint32_t macOk, macFail, winOk, winFail;
static uint32_t generated, skipped, sentNew, retx, apiErr, retxApiErr, slips, probes;
static uint32_t macPendingFinal;
static volatile uint32_t rttN, rttMin, rttMax;
static volatile uint64_t rttSum, rttSumSq;
static uint64_t ageSum;
static uint32_t ageN, ageMax;

// jendela 1 detik
static uint32_t winStartMs, winApiErr, windows;
static uint8_t lastWinFail;
static uint32_t rateSumX10, intvSum, txpSumQ;

// Rate Control
static uint8_t rateIdx, rateChanges, goodStreak, upWait;
static bool justUp;
static uint32_t rateApiErr;

// Dynamic Interval
static uint16_t curIntvMs;
static uint8_t diGood;
static int64_t nextSendUs;

// Dynamic Backoff
static uint16_t cwMs;
static bool boScheduled;
static int64_t boSendAtUs;
static uint64_t boDelaySum;
static uint32_t boDelayN, boLastOk, boLastFail;

// Tx Power (satuan 0,25 dBm)
static int8_t txpMaxQ = 80, txpReqQ = 80, txpActualQ = 80, txpMinQ = 80;

// ACK dan Retransmisi
#define ACK_SLOTS 256
static struct { uint16_t seq; uint8_t retries; uint8_t state; uint32_t lastTxUs; } ack[ACK_SLOTS];  // state 0 kosong, 1 tunggu, 2 antre
static uint8_t retxFifo[64];
static uint8_t retxHead, retxTail;
static uint32_t ackConfirmed, ackGiveUp, ackUnresolved;
static bool ackFinalized;

// STATUS dari receiver
static volatile bool stNew = false;
static StatusEntry stEntry;

// Channel Hopping
static volatile uint32_t lastHeardRxMs = 0;
static volatile bool hopPending = false;
static volatile uint8_t hopCh = 0, lastHopId = 0;
static volatile uint32_t hopAtMs = 0;
static uint8_t hops, scans, scanIdx;
static bool scanning;
static uint32_t nextScanMs;

// Energi
static uint32_t nextInaMs, lastInaUs;
static double eJ, qC, vS, tS;
static uint32_t inaSamples;

// ===== utilitas =====
static inline bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }
static inline const uint8_t *dest() { return cMode ? BCAST : rxMac; }

static void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  curChannel = ch;
}

static void addPeers() {
  esp_now_peer_info_t p = {};
  p.channel = 0;              // 0 = kanal saat ini, jadi ikut saat pindah kanal
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

static void applyRate(uint8_t idx) {
  esp_err_t e;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  esp_now_rate_config_t rc = {};
  rc.phymode = idx < 4 ? WIFI_PHY_MODE_11B : WIFI_PHY_MODE_11G;
  rc.rate = RATE_PHY[idx];
  rc.ersu = false;
  rc.dcm = false;
  e = esp_now_set_peer_rate_config(dest(), &rc);
#else
  e = esp_wifi_config_espnow_rate(WIFI_IF_STA, RATE_PHY[idx]);
#endif
  if (e != ESP_OK) rateApiErr++;
}

static void applyTxPower(int8_t q) {
  if (q < 8) q = 8;
  if (q > txpMaxQ) q = txpMaxQ;
  esp_wifi_set_max_tx_power(q);
  int8_t actual = q;
  if (esp_wifi_get_max_tx_power(&actual) != ESP_OK) actual = q;
  txpActualQ = actual;
  if (actual < txpMinQ) txpMinQ = actual;
}

// ===== INA219 =====
static bool inaWrite16(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(INA_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(v >> 8));
  Wire.write((uint8_t)(v & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool inaRead16(uint8_t reg, uint16_t &v) {
  Wire.beginTransmission(INA_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)INA_ADDR, 2) != 2) return false;
  v = ((uint16_t)Wire.read() << 8) | (uint16_t)Wire.read();
  return true;
}

static bool inaInit() {
#if INA_ENABLE
  Wire.begin(INA_SDA, INA_SCL, 400000);
  Wire.beginTransmission(INA_ADDR);
  if (Wire.endTransmission() != 0) return false;
  // 32 V, PGA /8 (±320 mV), rata-rata 128 sampel untuk bus dan shunt, kontinu
  return inaWrite16(0x00, 0x3FFF);
#else
  return false;
#endif
}

static bool inaRead(float &volt, float &amp) {
  uint16_t sh, bus;
  if (!inaRead16(0x01, sh) || !inaRead16(0x02, bus)) return false;
  amp = (float)(int16_t)sh * 10e-6f / INA_SHUNT_OHM;
  volt = (float)(bus >> 3) * 0.004f;
  return true;
}

static void inaSample() {
  float v, i;
  uint32_t nowUs = (uint32_t)esp_timer_get_time();
  if (!inaRead(v, i)) return;
  double dt = (uint32_t)(nowUs - lastInaUs) / 1e6;
  lastInaUs = nowUs;
  if (dt <= 0 || dt > 1.0) return;
  eJ += (double)v * i * dt;
  qC += (double)i * dt;
  vS += (double)v * dt;
  tS += dt;
  inaSamples++;
}

// ===== callback =====
void sendStatusCounted(esp_now_send_status_t status) {
  if (!counting) return;
  portENTER_CRITICAL(&mux);
  if (status == ESP_NOW_SEND_SUCCESS) { macOk++; winOk++; }
  else { macFail++; winFail++; }
  portEXIT_CRITICAL(&mux);
}

static void onStart(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(StartPkt)) return;
  StartPkt s;
  memcpy(&s, data, sizeof(s));
  learnRx(info->src_addr);
  lastHeardRxMs = millis();
  if (cValid && s.runId == cRunId) return;
  cRunId = s.runId;
  cValid = true;
  uint32_t now = millis();
  if (nodeId == 0 || s.activeNodes < nodeId) {     // tidak ikut: diam sampai run selesai
    quietUntilMs = now + s.startInMs + (uint32_t)s.durationS * 1000UL + s.graceMs + 6000UL;
    st = S_IDLE;
    return;
  }
  cPayload = s.payload < MIN_PAYLOAD ? MIN_PAYLOAD : (s.payload > ESPNOW_MAX_PAYLOAD ? ESPNOW_MAX_PAYLOAD : s.payload);
  cMode = s.mode ? 1 : 0;
  cMethod = s.method < M_COUNT ? s.method : (uint8_t)M_BASELINE;
  cInterval = s.intervalMs < 1 ? 1 : s.intervalMs;
  cDur = s.durationS;
  cProbe = s.probeEvery;
  cJitter = s.jitterMs;
  cGraceMs = s.graceMs;
  P = s.p;
  startAtMs = now + s.startInMs;
  endAtMs = startAtMs + (uint32_t)cDur * 1000UL;
  abortFlag = false;
  reportAcked = false;
  st = S_WAIT_START;
}

static void onStatus(const uint8_t *data, int len) {
  if (len < (int)sizeof(StatusHdr)) return;
  StatusHdr h;
  memcpy(&h, data, sizeof(h));
  lastHeardRxMs = millis();
  if (!cValid || h.runId != cRunId) return;
  if (st != S_RUNNING && st != S_DRAIN && st != S_REPORTING) return;
  if (h.hopChannel && h.hopId != lastHopId) {
    lastHopId = h.hopId;
    hopCh = h.hopChannel;
    hopAtMs = millis() + h.hopInMs;
    hopPending = true;
  }
  if (nodeId >= h.firstNode && nodeId < h.firstNode + h.count) {
    size_t off = sizeof(StatusHdr) + (size_t)(nodeId - h.firstNode) * sizeof(StatusEntry);
    if ((int)(off + sizeof(StatusEntry)) > len) return;
    portENTER_CRITICAL(&mux);
    memcpy(&stEntry, data + off, sizeof(StatusEntry));
    stNew = true;
    portEXIT_CRITICAL(&mux);
  }
}

static void onEcho(const uint8_t *data, int len) {
  if (len < (int)sizeof(EchoPkt)) return;
  EchoPkt e;
  memcpy(&e, data, sizeof(e));
  lastHeardRxMs = millis();
  if (e.h.nodeId != nodeId || !cValid || e.runId != cRunId) return;
  if (st != S_RUNNING && st != S_DRAIN) return;
  uint32_t rtt = (uint32_t)esp_timer_get_time() - e.tsUs;
  portENTER_CRITICAL(&mux);
  rttN++;
  if (rtt < rttMin) rttMin = rtt;
  if (rtt > rttMax) rttMax = rtt;
  rttSum += rtt;
  rttSumSq += (uint64_t)rtt * rtt;
  portEXIT_CRITICAL(&mux);
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(PktHdr) || data[0] != PROTO_MAGIC) return;
  // paket yang hanya dikirim receiver: pelajari MAC receiver dari situ
  // (DATA broadcast dari sender lain di mode 1 sengaja tidak dipakai)
  uint8_t t = data[1];
  if (t == PKT_START || t == PKT_STOP || t == PKT_STATUS || t == PKT_ECHO || t == PKT_REPORT_ACK ||
      (t == PKT_HELLO && data[2] == 0)) {
    learnRx(info->src_addr);
  }
  switch (t) {
    case PKT_HELLO:
      if (data[2] == 0) lastHeardRxMs = millis();
      break;
    case PKT_START:  onStart(info, data, len); break;
    case PKT_STOP: {
      if (len < (int)sizeof(StopPkt)) break;
      StopPkt s;
      memcpy(&s, data, sizeof(s));
      if (cValid && s.runId == cRunId && (st == S_WAIT_START || st == S_RUNNING)) abortFlag = true;
      break;
    }
    case PKT_ECHO:   onEcho(data, len); break;
    case PKT_STATUS: onStatus(data, len); break;
    case PKT_REPORT_ACK: {
      if (len < (int)sizeof(StopPkt)) break;
      StopPkt a;
      memcpy(&a, data, sizeof(a));
      if (cValid && a.h.nodeId == nodeId && a.runId == cRunId) reportAcked = true;
      break;
    }
    default: break;
  }
}

// ===== ACK dan Retransmisi =====
static void ackAdd(uint16_t seq, uint32_t nowUs) {
  uint8_t i = seq & (ACK_SLOTS - 1);
  if (ack[i].state != 0) ackGiveUp++;          // slot lama (256 paket lalu) belum selesai
  ack[i].seq = seq;
  ack[i].retries = 0;
  ack[i].state = 1;
  ack[i].lastTxUs = nowUs;
}

// allowRetx=false dipakai setelah run selesai: hanya mengonfirmasi/menyerah, tidak mengirim ulang
static void ackEvaluate(const StatusEntry &e, bool allowRetx) {
  uint32_t nowUs = (uint32_t)esp_timer_get_time();
  for (int i = 0; i < ACK_SLOTS; i++) {
    if (ack[i].state != 1) continue;
    if ((uint32_t)(nowUs - ack[i].lastTxUs) < 200000UL) continue;   // terlalu baru untuk dinilai
    bool missing = false;
    uint16_t s = ack[i].seq;
    if (e.highest != NO_SEQ && s <= e.highest) {
      uint32_t d = (uint32_t)(e.highest - s);
      if (d >= ACK_WINDOW) { ackGiveUp++; ack[i].state = 0; continue; }
      bool rcv = (e.bitmap[d / 8] >> (d % 8)) & 1;
      if (rcv) { ackConfirmed++; ack[i].state = 0; continue; }
      missing = true;
    } else if ((uint32_t)(nowUs - ack[i].lastTxUs) > 300000UL) {
      missing = true;
    }
    if (!missing) continue;
    uint8_t nh = (uint8_t)((retxHead + 1) % sizeof(retxFifo));
    if (allowRetx && ack[i].retries < P.ackRetries && nh != retxTail) {
      ack[i].state = 2;
      retxFifo[retxHead] = (uint8_t)i;
      retxHead = nh;
    } else {
      ackGiveUp++;
      ack[i].state = 0;
    }
  }
}

static void serviceRetx() {
  if (retxTail == retxHead) return;
  uint8_t i = retxFifo[retxTail];
  retxTail = (uint8_t)((retxTail + 1) % sizeof(retxFifo));
  if (ack[i].state != 2) return;
  DataPkt *p = (DataPkt *)txBuf;
  p->h.flags = FLAG_RETX;
  p->seq = ack[i].seq;
  p->sample = 0;
  p->macFailWin = lastWinFail;
  p->rateIdx = rateIdx;
  uint32_t nowUs = (uint32_t)esp_timer_get_time();
  p->tsUs = nowUs;
  if (esp_now_send(dest(), txBuf, cPayload) == ESP_OK) retx++;
  else retxApiErr++;
  ack[i].retries++;
  ack[i].lastTxUs = nowUs;
  ack[i].state = 1;
}

// ===== kirim data =====
static void sendNew() {
  DataPkt *p = (DataPkt *)txBuf;
  bool probe = (cProbe > 0) && ((seqNo % cProbe) == 0);
  p->h.flags = probe ? FLAG_PROBE : 0;
  p->seq = seqNo;
  p->sample = pending.id;
  p->macFailWin = lastWinFail;
  p->rateIdx = rateIdx;
  int64_t now64 = esp_timer_get_time();
  p->tsUs = (uint32_t)now64;
  if (esp_now_send(dest(), txBuf, cPayload) == ESP_OK) {
    sentNew++;
    if (probe) probes++;
    uint32_t age = (uint32_t)(now64 - pending.genUs);
    ageSum += age;
    ageN++;
    if (age > ageMax) ageMax = age;
    if (cMethod == M_ACK) ackAdd(seqNo, (uint32_t)now64);
    seqNo++;
  } else {
    apiErr++;
    winApiErr++;
    if (cMethod == M_DBACKOFF) cwMs = (uint16_t)min((uint32_t)cwMs * 2, (uint32_t)P.boCwMaxMs);
  }
  pending.valid = false;
}

static void genSample(int64_t tUs) {
  generated++;
  if (pending.valid) skipped++;
  pending.valid = true;
  pending.id = sampleId++;
  pending.genUs = tUs;
}

// ===== jendela 1 detik: Rate Control dan Dynamic Interval =====
static void windowTick() {
  uint32_t wOk, wFail;
  portENTER_CRITICAL(&mux);
  wOk = winOk; wFail = winFail;
  winOk = 0; winFail = 0;
  portEXIT_CRITICAL(&mux);
  uint32_t wApi = winApiErr;
  winApiErr = 0;
  lastWinFail = wFail > 255 ? 255 : (uint8_t)wFail;
  windows++;
  rateSumX10 += RATE_MBPS_X10[rateIdx];
  intvSum += curIntvMs;
  txpSumQ += (uint32_t)txpActualQ;

  if (cMethod == M_RATE) {
    uint32_t n = wOk + wFail;
    if (n >= 5) {
      bool down = (wOk * 100 < (uint32_t)P.rcDownPdr * n) || wFail >= 2;
      if (down) {
        goodStreak = 0;
        if (justUp) upWait = (uint8_t)min((uint32_t)upWait * 2, (uint32_t)P.rcUpS * 4);
        justUp = false;
        if (rateIdx > 0) { rateIdx--; applyRate(rateIdx); rateChanges++; }
      } else if (wFail == 0) {
        if (justUp) { upWait = P.rcUpS; justUp = false; }
        goodStreak++;
        if (goodStreak >= upWait && rateIdx < NUM_RATES - 1) {
          rateIdx++; applyRate(rateIdx); rateChanges++;
          goodStreak = 0;
          justUp = true;
        }
      } else {
        goodStreak = 0;
        justUp = false;
      }
    }
  } else if (cMethod == M_DINTERVAL) {
    uint32_t maxIntv = (uint32_t)cInterval * P.diMaxMult;
    if (wApi + wFail > 0) {
      curIntvMs = (uint16_t)min((uint32_t)curIntvMs * 2, maxIntv);
      diGood = 0;
    } else if (++diGood >= 3) {
      curIntvMs = curIntvMs > cInterval + P.diStepMs ? curIntvMs - P.diStepMs : cInterval;
      diGood = 0;
    }
  }
}

static void handleStatus() {
  StatusEntry e;
  portENTER_CRITICAL(&mux);
  e = stEntry;
  stNew = false;
  portEXIT_CRITICAL(&mux);
  if (cMethod == M_TXPOWER && st == S_RUNNING) {
    int8_t q = txpReqQ;
    int8_t stepQ = (int8_t)(P.tpStepDbm * 4);
    if (e.rssi == 0 || e.rssi < P.tpLoDbm) q = (int8_t)min((int)txpReqQ + stepQ, (int)txpMaxQ);
    else if (e.rssi > P.tpHiDbm) q = (int8_t)max((int)txpReqQ - stepQ, 8);
    if (q != txpReqQ) { txpReqQ = q; applyTxPower(q); }
  }
  if (cMethod == M_ACK) {
    if (st == S_RUNNING) ackEvaluate(e, true);
    else if (!ackFinalized) ackEvaluate(e, false);   // STATUS masa tenggang: konfirmasi paket terakhir
  }
}

// dipanggil sekali sebelum laporan pertama: yang belum jelas statusnya dicatat terpisah
static void ackFinalize() {
  if (ackFinalized) return;
  ackFinalized = true;
  for (int i = 0; i < ACK_SLOTS; i++) {
    if (ack[i].state) { ackUnresolved++; ack[i].state = 0; }
  }
}

// dipanggil di RUNNING, DRAIN, REPORTING
static void serviceChannel() {
  if (hopPending && reached(hopAtMs)) {
    hopPending = false;
    setChannel(hopCh);
    hops++;
  }
  if (cMethod != M_CHANHOP || st != S_RUNNING) { scanning = false; return; }
  uint32_t now = millis();
  if ((uint32_t)(now - lastHeardRxMs) > P.keepaliveMs) {
    if (!scanning) {
      scanning = true;
      scanIdx = 0;
      for (uint8_t i = 0; i < NUM_HOP_CH; i++) if (HOP_CHANNELS[i] == curChannel) scanIdx = (i + 1) % NUM_HOP_CH;
      nextScanMs = now;
    }
    if (reached(nextScanMs)) {
      setChannel(HOP_CHANNELS[scanIdx]);
      scanIdx = (scanIdx + 1) % NUM_HOP_CH;
      scans++;
      nextScanMs = now + 1200;
    }
  } else {
    scanning = false;
  }
}

// ===== siklus run =====
static void beginRun() {
  portENTER_CRITICAL(&mux);
  macOk = macFail = winOk = winFail = 0;
  rttN = 0; rttMin = 0xFFFFFFFFu; rttMax = 0; rttSum = 0; rttSumSq = 0;
  stNew = false;
  portEXIT_CRITICAL(&mux);
  generated = skipped = sentNew = retx = apiErr = retxApiErr = slips = probes = 0;
  macPendingFinal = 0;
  ageSum = 0; ageN = 0; ageMax = 0;
  winApiErr = 0; windows = 0; lastWinFail = 0;
  rateSumX10 = intvSum = txpSumQ = 0;

  rateIdx = 0; rateChanges = 0; goodStreak = 0; upWait = P.rcUpS ? P.rcUpS : 1; justUp = false; rateApiErr = 0;
  applyRate(0);
  txpReqQ = txpMaxQ; txpMinQ = txpMaxQ;
  applyTxPower(txpMaxQ);
  curIntvMs = cInterval; diGood = 0;
  cwMs = P.boCw0Ms; boScheduled = false; boDelaySum = 0; boDelayN = 0; boLastOk = 0; boLastFail = 0;
  memset(ack, 0, sizeof(ack));
  retxHead = retxTail = 0;
  ackConfirmed = ackGiveUp = ackUnresolved = 0;
  ackFinalized = false;
  hopPending = false; lastHopId = 0; hops = 0; scans = 0; scanning = false;
  lastHeardRxMs = millis();
  eJ = qC = vS = tS = 0; inaSamples = 0;

  memset(txBuf, 0xA5, sizeof(txBuf));
  DataPkt *p = (DataPkt *)txBuf;
  p->h = makeHdr(PKT_DATA, nodeId, 0);
  p->runId = cRunId;
  p->bootId = bootId;
  seqNo = 0;
  sampleId = 0;
  pending.valid = false;
  runT0Us = esp_timer_get_time();
  nextGenUs = runT0Us + (int64_t)random(0, cInterval) * 1000;
  nextSendUs = nextGenUs;
  winStartMs = millis();
  lastInaUs = (uint32_t)runT0Us;
  nextInaMs = millis();
  counting = true;
  st = S_RUNNING;
  Serial.printf("# run %u mulai: metode=%u(%s) payload=%u interval=%ums dur=%us\n", (unsigned)cRunId,
                cMethod, methodName(cMethod), cPayload, cInterval, cDur);
}

static void endSending() {
  elapsedMs = (uint32_t)((esp_timer_get_time() - runT0Us) / 1000);
  if (pending.valid) { skipped++; pending.valid = false; }
  // paket yang masih menunggu ACK tetap dinilai dari STATUS masa tenggang (tanpa kirim ulang)
  for (int i = 0; i < ACK_SLOTS; i++) if (ack[i].state == 2) ack[i].state = 1;
  retxHead = retxTail = 0;
  boScheduled = false;
  drainUntilMs = millis() + 3000;
  uint32_t base = abortFlag ? millis() : endAtMs;
  reportAtMs = base + cGraceMs + 500UL + (uint32_t)nodeId * 70UL + (uint32_t)random(0, 40);
  reportTries = 0;
  st = S_DRAIN;
}

static void runStep() {
  if (abortFlag || reached(endAtMs)) { endSending(); return; }
  int64_t nowUs = esp_timer_get_time();
  const int64_t intvUs = (int64_t)cInterval * 1000;

  while (nowUs >= nextGenUs) {
    genSample(nextGenUs);
    int64_t step = intvUs + (cJitter ? (int64_t)random(0, cJitter + 1) * 1000 : 0);
    nextGenUs += step;
    if (nowUs > nextGenUs + intvUs) { slips++; nextGenUs = nowUs + step; }
    if (cMethod != M_DINTERVAL && cMethod != M_DBACKOFF && pending.valid) sendNew();
  }

  if (cMethod == M_DINTERVAL) {
    if (pending.valid && nowUs >= nextSendUs) {
      sendNew();
      // jadwal tetap (tidak ikut bergeser karena keterlambatan loop): saat interval = nominal,
      // kiriman tepat sejajar dengan pembangkitan sampel, sama seperti baseline
      nextSendUs += (int64_t)curIntvMs * 1000;
      if (nextSendUs < nowUs - (int64_t)curIntvMs * 1000) nextSendUs = nowUs;
    }
  } else if (cMethod == M_DBACKOFF) {
    uint32_t ok, fail;
    portENTER_CRITICAL(&mux);
    ok = macOk; fail = macFail;
    portEXIT_CRITICAL(&mux);
    if (fail != boLastFail) cwMs = (uint16_t)min((uint32_t)cwMs * 2, (uint32_t)P.boCwMaxMs);
    else if (ok != boLastOk) cwMs = P.boCw0Ms;
    boLastOk = ok; boLastFail = fail;
    if (pending.valid && !boScheduled) {
      uint32_t d = (uint32_t)random(0, (long)cwMs * 1000 + 1);
      boSendAtUs = pending.genUs + d;
      boScheduled = true;
      boDelaySum += d;
      boDelayN++;
    }
    if (boScheduled && nowUs >= boSendAtUs) {
      boScheduled = false;
      if (pending.valid) sendNew();
    }
  } else if (cMethod == M_ACK) {
    serviceRetx();
  }

  if (stNew) handleStatus();
  serviceChannel();
  if (reached(winStartMs + 1000)) { winStartMs += 1000; windowTick(); }
  if (inaOk && reached(nextInaMs)) { nextInaMs += INA_PERIOD_MS; inaSample(); }

  // tidur singkat jika tidak ada yang mendesak
  if (cMethod == M_ACK && retxTail != retxHead) return;
  int64_t next = nextGenUs;
  if (cMethod == M_DINTERVAL && pending.valid && nextSendUs < next) next = nextSendUs;
  if (cMethod == M_DBACKOFF && boScheduled && boSendAtUs < next) next = boSendAtUs;
  int64_t wait = next - esp_timer_get_time();
  if (wait > 1500) delay(1);
  else if (wait > 0) delayMicroseconds((uint32_t)wait);
}

static void drainStep() {
  if (stNew) handleStatus();
  serviceChannel();
  uint32_t done;
  portENTER_CRITICAL(&mux);
  done = macOk + macFail;
  portEXIT_CRITICAL(&mux);
  uint32_t issued = sentNew + retx;
  if (done >= issued || reached(drainUntilMs)) {
    counting = false;
    macPendingFinal = issued > done ? issued - done : 0;
    st = S_REPORTING;
    return;
  }
  delay(1);
}

static void sendReport() {
  ReportPkt r;
  memset(&r, 0, sizeof(r));
  r.h = makeHdr(PKT_REPORT, nodeId, 0);
  r.runId = cRunId;
  r.fw = FW_VERSION;
  r.bootId = bootId;
  r.resetReason = resetReason;
  r.method = cMethod;
  r.generated = (uint16_t)generated;
  r.skipped = (uint16_t)skipped;
  r.sentNew = (uint16_t)sentNew;
  r.retx = (uint16_t)retx;
  r.apiErr = (uint16_t)apiErr;
  r.retxApiErr = (uint16_t)retxApiErr;
  uint32_t n, mn, mx, ok, fail;
  uint64_t sum, sumsq;
  portENTER_CRITICAL(&mux);
  n = rttN; mn = rttMin; mx = rttMax; sum = rttSum; sumsq = rttSumSq;
  ok = macOk; fail = macFail;
  portEXIT_CRITICAL(&mux);
  r.macOk = (uint16_t)ok;
  r.macFail = (uint16_t)fail;
  r.macPending = (uint16_t)macPendingFinal;
  r.slips = (uint16_t)slips;
  r.probes = (uint16_t)probes;
  r.echoes = (uint16_t)n;
  if (n) {
    double mean = (double)sum / n;
    double var = (double)sumsq / n - mean * mean;
    r.rttMinUs = mn;
    r.rttMaxUs = mx;
    r.rttMeanUs = (uint32_t)(mean + 0.5);
    r.rttStdUs = (uint32_t)(sqrt(var > 0 ? var : 0) + 0.5);
  }
  r.ageMeanUs = ageN ? (uint32_t)(ageSum / ageN) : 0;
  r.ageMaxUs = ageMax;
  r.elapsedMs = elapsedMs;
  r.rateFinalIdx = rateIdx;
  r.rateChanges = rateChanges;
  r.rateMeanX10 = windows ? (uint16_t)(rateSumX10 / windows) : RATE_MBPS_X10[rateIdx];
  r.txpFinalQ = (uint8_t)txpActualQ;
  r.txpMinQ = (uint8_t)txpMinQ;
  r.txpMeanQ = windows ? (uint8_t)(txpSumQ / windows) : (uint8_t)txpActualQ;
  r.channelFinal = curChannel;
  r.hops = hops;
  r.scans = scans;
  r.ackConfirmed = (uint16_t)ackConfirmed;
  r.ackGiveUp = (uint16_t)ackGiveUp;
  r.ackUnresolved = (uint16_t)ackUnresolved;
  r.intvFinalMs = curIntvMs;
  r.intvMeanMs = windows ? (uint16_t)(intvSum / windows) : curIntvMs;
  r.cwFinalMs = cwMs;
  r.backoffMeanMs = boDelayN ? (uint16_t)(boDelaySum / boDelayN / 1000) : 0;
  r.inaOk = inaOk ? 1 : 0;
  r.inaSamples = (uint16_t)inaSamples;
  if (tS > 0) {
    r.energyMj = (uint32_t)(eJ * 1000.0 + 0.5);
    r.iAvgUa = (uint32_t)(qC / tS * 1e6 + 0.5);
    r.vAvgMv = (uint16_t)(vS / tS * 1000.0 + 0.5);
  }
  esp_now_send(rxMac, (const uint8_t *)&r, sizeof(r));
}

static void finishToIdle(bool acked) {
  st = S_IDLE;
  if (curChannel != HOME_CHANNEL) setChannel(HOME_CHANNEL);
  nextHelloMs = millis() + 500;
  Serial.printf("# run %u selesai%s: gen=%lu baru=%lu ulang=%lu api_err=%lu mac_ok=%lu mac_fail=%lu "
                "lewati=%lu rate=%.1f txp=%.2f kanal=%u energi=%.1fJ\n",
                (unsigned)cRunId, acked ? "" : " (laporan tidak di-ACK)", (unsigned long)generated,
                (unsigned long)sentNew, (unsigned long)retx, (unsigned long)apiErr, (unsigned long)macOk,
                (unsigned long)macFail, (unsigned long)skipped, RATE_MBPS_X10[rateIdx] / 10.0f,
                txpActualQ / 4.0f, curChannel, eJ);
}

static void reportStep() {
  if (stNew) handleStatus();
  serviceChannel();
  if (reportAcked) { finishToIdle(true); return; }
  if (!reached(reportAtMs)) { delay(1); return; }
  if (reportTries >= 8) { finishToIdle(false); return; }
  ackFinalize();
  sendReport();
  reportTries++;
  reportAtMs = millis() + 350UL + (uint32_t)random(0, 250);
}

static void waitStep() {
  if (abortFlag) { st = S_IDLE; return; }
  if (reached(startAtMs)) beginRun();
  else delay(1);
}

static void idleStep() {
  if (haveRx && nodeId != 0 && reached(nextHelloMs) && reached(quietUntilMs)) {
    HelloPkt h;
    memset(&h, 0, sizeof(h));
    h.h = makeHdr(PKT_HELLO, nodeId, 0);
    h.fw = FW_VERSION;
    h.bootId = bootId;
    h.resetReason = resetReason;
    h.inaOk = inaOk ? 1 : 0;
    h.channel = curChannel;
    esp_now_send(rxMac, (const uint8_t *)&h, sizeof(h));
    nextHelloMs = millis() + 1500UL + (uint32_t)random(0, 1000);
  }
  delay(2);
}

// ===== serial =====
static void printStatus() {
  static const char *names[] = {"IDLE", "WAIT_START", "RUNNING", "DRAIN", "REPORTING"};
  Serial.printf("# fw=%u.%u node ID=%u state=%s receiver=%s run=%u kanal=%u\n", FW_VERSION_MAJOR,
                FW_VERSION_MINOR, nodeId, names[st], haveRx ? "dikenal" : "belum terdengar", (unsigned)cRunId,
                curChannel);
  Serial.printf("# boot=%04X reset_terakhir=%s ina219=%s txp_maks=%.2f dBm\n", bootId, resetName(resetReason),
                inaOk ? "ada" : "tidak ada", txpMaxQ / 4.0f);
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
  } else if (!strcmp(tok, "ina")) {
    float v, i;
    if (inaOk && inaRead(v, i)) Serial.printf("# INA219: %.3f V  %.1f mA  %.1f mW\n", v, i * 1000, v * i * 1000);
    else Serial.println("# INA219 tidak terdeteksi");
  } else {
    Serial.println("# perintah: id N | status | ina | help");
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
  resetReason = (uint8_t)esp_reset_reason();
  bootId = (uint16_t)(esp_random() & 0xFFFF);
  if (bootId == 0) bootId = 1;

  prefs.begin("exp", false);
  nodeId = FORCE_NODE_ID ? FORCE_NODE_ID : prefs.getUChar("id", 0);
  defaultParams(P);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  setChannel(HOME_CHANNEL);
#if !WIFI_POWER_SAVE
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  int8_t q = 80;
  if (esp_wifi_get_max_tx_power(&q) == ESP_OK && q >= 8) txpMaxQ = q;
  txpReqQ = txpActualQ = txpMinQ = txpMaxQ;

  if (esp_now_init() != ESP_OK) {
    Serial.println("# ESP-NOW init gagal");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSentCb);

  inaOk = inaInit();
  randomSeed(esp_random() ^ nodeId);
  nextHelloMs = millis() + 500 + random(0, 1000);

  Serial.println();
  Serial.printf("# === ESP-NOW Sender v%u.%u ===\n", FW_VERSION_MAJOR, FW_VERSION_MINOR);
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
    case S_DRAIN:      drainStep(); break;
    case S_REPORTING:  reportStep(); break;
  }
}
