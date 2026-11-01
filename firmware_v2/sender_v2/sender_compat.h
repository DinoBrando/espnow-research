/*
  Kompatibilitas signature callback status kirim ESP-NOW.
  - core lama   : void cb(const uint8_t *mac, esp_now_send_status_t status)
  - IDF baru    : void cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
  Dua overload di bawah membuat esp_now_register_send_cb(onSentCb) valid di keduanya:
  kompilator memilih overload yang cocok dengan tipe fungsi yang diminta.
*/
#pragma once
#include <esp_now.h>

void sendStatusCounted(esp_now_send_status_t status);   // diimplementasi di sender_v2.ino

inline void onSentCb(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  sendStatusCounted(status);
}

template <typename T>
inline void onSentCb(const T *info, esp_now_send_status_t status) {
  (void)info;
  sendStatusCounted(status);
}
