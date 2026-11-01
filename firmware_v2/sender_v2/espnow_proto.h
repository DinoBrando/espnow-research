/*
  Protokol bersama eksperimen ESP-NOW, versi 2.
  File ini HARUS identik di folder receiver_v2 dan sender_v2.

  Perubahan dari v1:
  - PROTO_MAGIC baru (0xE6) supaya firmware v1 dan v2 tidak saling bicara.
  - START membawa ID metode dan parameternya.
  - STATUS dari receiver (broadcast, 1 kali per detik per kelompok 10 node):
    ACK bitmap 128 paket, RSSI per node, perintah pindah kanal.
  - REPORT berisi statistik semua metode, energi INA219, versi firmware, dan alasan reset.
*/
#pragma once
#include <stdint.h>

#define PROTO_MAGIC         0xE6
#define FW_VERSION_MAJOR    2
#define FW_VERSION_MINOR    1
#define FW_VERSION          ((FW_VERSION_MAJOR << 8) | FW_VERSION_MINOR)

#define MAX_NODES           32      // ID node valid: 1..MAX_NODES
#define ESPNOW_MAX_PAYLOAD  250
#define MIN_PAYLOAD         24      // header paket data = 18 byte
#define STATUS_NODES_PER_FRAME 10
#define ACK_WINDOW          128     // bitmap ACK mencakup 128 nomor urut terakhir
#define NO_SEQ              0xFFFF
#define NUM_RATES           6
#define NUM_HOP_CH          3

enum Method : uint8_t {
  M_BASELINE   = 0,
  M_RATE       = 1,   // Rate Control: kecepatan bit radio berdasarkan PDR link
  M_DINTERVAL  = 2,   // Dynamic Interval
  M_DBACKOFF   = 3,   // Dynamic Backoff
  M_TXPOWER    = 4,   // Tx Power Control
  M_ACK        = 5,   // ACK dan Retransmisi lapisan aplikasi
  M_CHANHOP    = 6,   // Channel Hopping
  M_COUNT      = 7
};

enum PktType : uint8_t {
  PKT_HELLO      = 1,   // receiver->broadcast (idle) dan sender->receiver (lapor hadir)
  PKT_START      = 2,   // receiver->broadcast
  PKT_STOP       = 3,   // receiver->broadcast
  PKT_DATA       = 4,   // sender->receiver
  PKT_ECHO       = 5,   // receiver->broadcast, balasan probe RTT
  PKT_REPORT     = 6,   // sender->receiver, statistik setelah run
  PKT_REPORT_ACK = 7,   // receiver->broadcast
  PKT_STATUS     = 8    // receiver->broadcast, 1 Hz per kelompok node selama run
};

#define FLAG_PROBE 0x01   // paket data minta di-echo (RTT)
#define FLAG_RETX  0x02   // paket data adalah kiriman ulang (metode ACK)

typedef struct __attribute__((packed)) {
  uint8_t magic;
  uint8_t type;
  uint8_t nodeId;     // 0 = receiver
  uint8_t flags;
} PktHdr;                                   // 4 byte

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint16_t seq;         // nomor urut paket baru (naik hanya jika esp_now_send menerima paket)
  uint16_t sample;      // nomor sampel data yang dibawa
  uint32_t tsUs;        // esp_timer sender saat kirim
  uint16_t bootId;      // acak per boot, untuk mendeteksi reset
  uint8_t  macFailWin;  // jumlah gagal MAC sender di jendela 1 detik sebelumnya (maks 255)
  uint8_t  rateIdx;     // indeks kecepatan radio saat ini
} DataPkt;                                  // 18 byte, sisanya padding

typedef struct __attribute__((packed)) {
  PktHdr   h;           // h.nodeId = node tujuan
  uint16_t runId;
  uint16_t seq;
  uint32_t tsUs;
} EchoPkt;                                  // 12 byte

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t fw;
  uint16_t bootId;
  uint8_t  resetReason; // esp_reset_reason() saat boot
  uint8_t  inaOk;
  uint8_t  channel;
  uint8_t  pad;
} HelloPkt;                                 // 12 byte

typedef struct __attribute__((packed)) {
  uint8_t  rcDownPdr;   // Rate Control: turun jika PDR link < nilai ini (%)
  uint8_t  rcUpS;       // Rate Control: naik setelah N detik berturut-turut 100%
  uint8_t  diStepMs;    // Dynamic Interval: pengurangan interval saat kondisi baik
  uint8_t  diMaxMult;   // Dynamic Interval: interval maks = nominal x nilai ini
  uint16_t boCw0Ms;     // Dynamic Backoff: CW awal
  uint16_t boCwMaxMs;   // Dynamic Backoff: CW maks
  int8_t   tpLoDbm;     // Tx Power: naikkan daya jika RSSI < nilai ini
  int8_t   tpHiDbm;     // Tx Power: turunkan daya jika RSSI > nilai ini
  uint8_t  tpStepDbm;   // Tx Power: langkah (dBm)
  uint8_t  ackRetries;  // ACK: maks kirim ulang per paket
  uint8_t  hopFailPct;  // Channel Hopping: pindah jika gagal MAC >= nilai ini (%)
  uint8_t  hopRxPct;    // Channel Hopping: atau jika paket diterima < nilai ini (% dari harapan)
  uint8_t  hopWindows;  // Channel Hopping: berapa jendela 1 detik berturut-turut
  uint8_t  pad;
  uint16_t keepaliveMs; // Channel Hopping: sender mencari receiver jika tak terdengar selama ini
} MethodParams;                             // 18 byte

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint8_t  activeNodes; // node ber-ID 1..activeNodes ikut
  uint8_t  payload;
  uint8_t  mode;        // 0 = unicast, 1 = broadcast
  uint8_t  method;
  uint16_t intervalMs;  // interval nominal pembangkitan data
  uint16_t durationS;
  uint16_t probeEvery;
  uint16_t jitterMs;
  uint16_t startInMs;
  uint16_t graceMs;
  uint8_t  homeChannel;
  uint8_t  pad;
  MethodParams p;
} StartPkt;                                 // 42 byte

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
} StopPkt;                                  // juga dipakai untuk REPORT_ACK

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint8_t  firstNode;
  uint8_t  count;
  uint8_t  curChannel;
  uint8_t  hopChannel;  // 0 = tidak ada perintah pindah
  uint16_t hopInMs;     // pindah dalam sekian ms sejak paket ini diterima
  uint8_t  hopId;       // naik setiap ada perintah pindah baru
  uint8_t  pad;
} StatusHdr;                                // 14 byte

typedef struct __attribute__((packed)) {
  uint16_t highest;     // nomor urut unik tertinggi yang diterima, NO_SEQ jika belum ada
  uint8_t  bitmap[16];  // bit i (byte i/8, bit i%8) = paket (highest - i) sudah diterima
  int8_t   rssi;        // rata-rata RSSI node ini sejak STATUS sebelumnya, 0 = tidak ada paket
} StatusEntry;                              // 19 byte

typedef struct __attribute__((packed)) {
  StatusHdr   s;
  StatusEntry e[STATUS_NODES_PER_FRAME];
} StatusPkt;                                // 204 byte

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint16_t fw;
  uint16_t bootId;
  uint8_t  resetReason;
  uint8_t  method;
  // data
  uint16_t generated;   // sampel dibangkitkan
  uint16_t skipped;     // sampel ditimpa sampel lebih baru sebelum sempat dikirim
  uint16_t sentNew;     // paket baru yang diterima stack
  uint16_t retx;        // kiriman ulang yang diterima stack (metode ACK)
  uint16_t apiErr;      // paket baru ditolak esp_now_send (sampel hilang)
  uint16_t retxApiErr;  // kiriman ulang ditolak esp_now_send
  uint16_t macOk;       // callback sukses (paket baru + ulang)
  uint16_t macFail;
  uint16_t macPending;  // kiriman yang belum ada callback saat laporan dibuat
  uint16_t slips;
  uint16_t probes;
  uint16_t echoes;
  uint32_t rttMinUs, rttMeanUs, rttStdUs, rttMaxUs;
  uint32_t ageMeanUs;   // umur sampel saat pertama dikirim (pembangkitan -> kirim)
  uint32_t ageMaxUs;
  uint32_t elapsedMs;
  // Rate Control
  uint8_t  rateFinalIdx;
  uint8_t  rateChanges;
  uint16_t rateMeanX10; // rata-rata kecepatan (Mbps x10) per jendela 1 detik
  // Tx Power (satuan 0,25 dBm)
  uint8_t  txpFinalQ;
  uint8_t  txpMinQ;
  uint8_t  txpMeanQ;
  uint8_t  pad1;
  // Channel Hopping
  uint8_t  channelFinal;
  uint8_t  hops;
  uint8_t  scans;
  uint8_t  pad2;
  // ACK
  uint16_t ackConfirmed;
  uint16_t ackGiveUp;
  // Dynamic Interval / Backoff
  uint16_t intvFinalMs;
  uint16_t intvMeanMs;
  uint16_t cwFinalMs;
  uint16_t backoffMeanMs;
  // Energi (INA219)
  uint8_t  inaOk;
  uint8_t  pad3;
  uint16_t inaSamples;
  uint32_t energyMj;
  uint32_t iAvgUa;
  uint16_t vAvgMv;
  uint16_t ackUnresolved; // ACK: paket yang statusnya belum diketahui saat laporan dibuat
} ReportPkt;

static_assert(sizeof(PktHdr) == 4, "PktHdr");
static_assert(sizeof(DataPkt) == 18, "DataPkt");
static_assert(sizeof(DataPkt) <= MIN_PAYLOAD, "MIN_PAYLOAD");
static_assert(sizeof(StartPkt) <= ESPNOW_MAX_PAYLOAD, "StartPkt");
static_assert(sizeof(StatusPkt) <= ESPNOW_MAX_PAYLOAD, "StatusPkt");
static_assert(sizeof(ReportPkt) <= ESPNOW_MAX_PAYLOAD, "ReportPkt");
static_assert(ACK_WINDOW == 8 * sizeof(((StatusEntry *)0)->bitmap), "ACK_WINDOW");

// Tabel kecepatan radio untuk Rate Control (Mbps x10). Indeks 0 = bawaan ESP-NOW (1 Mbps).
static const uint16_t RATE_MBPS_X10[NUM_RATES] = {10, 20, 55, 110, 240, 540};
static const uint8_t  HOP_CHANNELS[NUM_HOP_CH] = {1, 6, 11};

static inline PktHdr makeHdr(uint8_t type, uint8_t nodeId, uint8_t flags) {
  PktHdr h;
  h.magic = PROTO_MAGIC;
  h.type = type;
  h.nodeId = nodeId;
  h.flags = flags;
  return h;
}

static inline void defaultParams(MethodParams &p) {
  p.rcDownPdr = 90;
  p.rcUpS = 3;
  p.diStepMs = 10;
  p.diMaxMult = 8;
  p.boCw0Ms = 10;
  p.boCwMaxMs = 320;
  p.tpLoDbm = -70;
  p.tpHiDbm = -60;
  p.tpStepDbm = 2;
  p.ackRetries = 2;
  p.hopFailPct = 10;
  p.hopRxPct = 10;
  p.hopWindows = 3;
  p.pad = 0;
  p.keepaliveMs = 3000;
}

static inline const char *methodName(uint8_t m) {
  static const char *n[] = {"baseline", "rate_control", "dynamic_interval", "dynamic_backoff",
                            "tx_power", "ack_retx", "channel_hopping"};
  return m < M_COUNT ? n[m] : "?";
}

static inline const char *resetName(uint8_t r) {
  // urutan mengikuti esp_reset_reason_t
  static const char *n[] = {"UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT", "WDT",
                            "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE", "PWR_GLITCH", "CPU_LOCKUP"};
  return r < sizeof(n) / sizeof(n[0]) ? n[r] : "OTHER";
}
