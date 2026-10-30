/*
  Protokol bersama eksperimen baseline ESP-NOW (receiver/pengontrol <-> sender).
  File ini HARUS identik di folder baseline_receiver dan baseline_sender.
*/
#pragma once
#include <stdint.h>

#define PROTO_MAGIC         0xE5
#define MAX_NODES           32     // ID node valid: 1..MAX_NODES
#define ESPNOW_MAX_PAYLOAD  250
#define MIN_PAYLOAD         16     // header paket data = 14 byte

enum PktType : uint8_t {
  PKT_HELLO      = 1,   // idle: receiver->broadcast (cari receiver) dan sender->receiver (lapor hadir)
  PKT_START      = 2,   // receiver->broadcast, dikirim berulang untuk satu run
  PKT_STOP       = 3,   // receiver->broadcast (batalkan run)
  PKT_DATA       = 4,   // sender->receiver (unicast) atau broadcast
  PKT_ECHO       = 5,   // receiver->sender, balasan untuk paket probe RTT
  PKT_REPORT     = 6,   // sender->receiver, statistik sisi pengirim setelah run
  PKT_REPORT_ACK = 7    // receiver->sender
};

#define FLAG_PROBE 0x01   // paket data ini minta di-echo (untuk RTT)

typedef struct __attribute__((packed)) {
  uint8_t magic;
  uint8_t type;
  uint8_t nodeId;
  uint8_t flags;
} PktHdr;                       // 4 byte

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint32_t seq;
  uint32_t tsUs;                // esp_timer pengirim saat kirim (dipakai untuk jitter dan RTT)
} DataPkt;                      // 14 byte, sisanya padding sampai ukuran payload

typedef DataPkt EchoPkt;        // layout sama: runId, seq, tsUs disalin dari paket probe

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint8_t  activeNodes;         // node ber-ID 1..activeNodes ikut
  uint8_t  payload;             // ukuran paket data (16..250)
  uint8_t  mode;                // 0 = unicast ke receiver, 1 = broadcast
  uint8_t  pad;
  uint16_t intervalMs;
  uint16_t durationS;
  uint16_t probeEvery;          // 0 = tanpa probe RTT
  uint16_t jitterMs;            // 0 = periodik murni
  uint16_t startInMs;           // sisa waktu sampai mulai, relatif saat paket ini dikirim
} StartPkt;

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
} StopPkt;                      // dipakai juga sebagai REPORT_ACK

typedef struct __attribute__((packed)) {
  PktHdr   h;
  uint16_t runId;
  uint32_t sent;                // esp_now_send() sukses diterima stack
  uint32_t apiErr;              // esp_now_send() gagal (mis. antrean TX penuh)
  uint32_t macOk;               // callback status kirim: sukses (ACK MAC diterima)
  uint32_t macFail;             // callback status kirim: gagal
  uint32_t slips;               // jadwal kirim tertinggal lebih dari satu interval
  uint32_t probes;
  uint32_t echoes;
  uint32_t rttMinUs;
  uint32_t rttMeanUs;
  uint32_t rttStdUs;
  uint32_t rttMaxUs;
  uint32_t elapsedMs;
} ReportPkt;

static_assert(sizeof(PktHdr) == 4, "PktHdr");
static_assert(sizeof(DataPkt) == 14, "DataPkt");
static_assert(sizeof(StartPkt) <= ESPNOW_MAX_PAYLOAD, "StartPkt");
static_assert(sizeof(ReportPkt) <= ESPNOW_MAX_PAYLOAD, "ReportPkt");

static inline PktHdr makeHdr(uint8_t type, uint8_t nodeId, uint8_t flags) {
  PktHdr h;
  h.magic = PROTO_MAGIC;
  h.type = type;
  h.nodeId = nodeId;
  h.flags = flags;
  return h;
}
