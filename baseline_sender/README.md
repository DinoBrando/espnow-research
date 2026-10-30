# baseline_sender

Firmware pengirim. **Satu firmware yang sama untuk semua node.** Yang membedakan tiap board hanya ID node yang tersimpan di flash.

- Tidak perlu mengisi MAC receiver. Sender mengenali receiver dari paket HELLO dan START yang disiarkan receiver.
- Tidak perlu flash ulang untuk tiap skenario. Semua parameter run (payload, interval, durasi, dll) datang dari receiver.
- Setelah ID diset, board bisa dicabut dari laptop dan dijalankan dengan baterai.

## File

| File | Isi |
|---|---|
| `baseline_sender.ino` | Sketch utama |
| `espnow_proto.h` | Format paket bersama. **Harus identik** dengan salinan di `baseline_receiver/` |
| `sender_compat.h` | Penyesuaian signature callback status kirim ESP-NOW agar bisa dikompilasi di versi core lama maupun baru |

## Upload dan set ID

1. Board sesuai jenis ESP32 yang dipakai (mis. ESP32C6 Dev Module, ESP32 Dev Module).
2. Upload `baseline_sender.ino`.
3. Selagi masih tercolok, buka Serial Monitor: **115200**, line ending **New Line**.
4. Ketik `id N` (N = 1..32), lalu Enter. Contoh: `id 7`. Harus muncul `# ID disimpan: 7`.
5. Ketik `status` untuk memastikan.
6. Tempel label fisik di board (mis. `N07`).

ID tersimpan di flash (Preferences) dan tetap ada setelah daya dicabut atau firmware di-upload ulang.

**Aturan ID:** untuk eksperimen 20 node, ID harus **1 sampai 20 tanpa lompatan dan tanpa dobel**. Run dengan `N` node hanya mengaktifkan ID 1..N.

Alternatif: isi `FORCE_NODE_ID` di kode dengan nomor ID sebelum upload. Nilai selain 0 mengalahkan ID di flash, tetapi harus diedit untuk tiap board.

## Perintah Serial

| Perintah | Fungsi |
|---|---|
| `id N` | Menyimpan ID node (1..32) |
| `status` | Menampilkan ID, keadaan, apakah receiver sudah dikenal, MAC receiver, MAC node ini |

Serial hanya untuk persiapan. Saat eksperimen, Serial Monitor sender tidak diperlukan.

## Perilaku

| Keadaan | Yang dilakukan |
|---|---|
| `IDLE` | Mengirim HELLO ke receiver tiap 1,5–2,5 detik (kalau ID sudah diset dan receiver sudah dikenal) |
| `WAIT_START` | Menerima START, menunggu waktu mulai yang sama dengan node lain |
| `RUNNING` | Mengirim paket data secara periodik |
| `REPORTING` | Mengirim laporan statistik ke receiver sampai di-ACK (maks 8 kali) |

Detail saat `RUNNING`:

- Paket pertama dikirim dengan offset acak 0..interval, supaya node tidak mulai serentak. Setelah itu periodik sesuai interval (ditambah jitter acak jika `jitter > 0`).
- Tiap paket berisi ID node, `run_id`, nomor urut, dan timestamp kirim (mikrodetik). Sisanya diisi padding sampai ukuran payload.
- Tiap paket ke-`probe` meminta echo dari receiver untuk mengukur RTT.
- Kalau sender tertinggal lebih dari satu interval dari jadwal (CPU atau antrean TX sibuk), jadwal direset dan `slips` bertambah.

Node dengan ID lebih besar dari `N` tidak mengirim apa pun selama run, termasuk HELLO, supaya tidak mengganggu pengukuran.

Setelah run, laporan dikirim dengan jeda `700 ms + ID × 70 ms` supaya laporan 20 node tidak bertabrakan.

## Isi laporan ke receiver

| Data | Arti |
|---|---|
| `sent` | `esp_now_send()` diterima stack |
| `apiErr` | `esp_now_send()` ditolak (antrean TX penuh, dll) |
| `macOk` / `macFail` | Hasil callback status kirim (ACK MAC) |
| `slips` | Berapa kali tertinggal dari jadwal |
| `probes` / `echoes` | Probe RTT yang dikirim dan echo yang kembali |
| RTT min, rata-rata, std, maks | Dalam mikrodetik |
| `elapsedMs` | Lama run sebenarnya di sisi sender |

## Konfigurasi di kode

| Konstanta | Default | Arti |
|---|---|---|
| `FORCE_NODE_ID` | 0 | 0 = pakai ID dari flash. Selain 0 = paksa ID ini |
| `WIFI_CHANNEL` | 1 | Harus sama dengan receiver |
| `WIFI_POWER_SAVE` | 0 | 0 = power save WiFi mati (baseline). Disiapkan untuk tes energi nanti |
| `BAUD` | 115200 | Baud Serial |

## Troubleshooting

| Gejala | Penyebab dan solusi |
|---|---|
| `status` menampilkan `ID=0` | ID belum diset. Ketik `id N` |
| `receiver=belum terdengar` | Receiver belum menyala, sedang menjalankan batch (HELLO berhenti selama batch), atau channel berbeda |
| Node tidak muncul di `nodes` receiver | Cek baterai, ID, dan channel. Sender hanya mengirim HELLO setelah mengenal receiver |
| Compile error di `esp_now_register_send_cb` | Versi core esp32 terlalu lama atau alpha. Pakai versi stabil 3.x atau lebih baru |
| Perintah Serial tidak bereaksi | Line ending bukan New Line |

## Catatan baterai

- Isi penuh semua baterai sebelum sesi tes, dan pakai jenis baterai yang sama di semua node.
- Tegangan yang turun di akhir sesi panjang bisa memengaruhi daya pancar. Pecah batch per jumlah node kalau sesi lebih dari sekitar 1 jam.
- Jangan letakkan baterai menutupi ujung antena PCB.
