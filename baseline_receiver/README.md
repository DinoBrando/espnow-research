# baseline_receiver

Firmware receiver ESP32-C6. Perannya tiga sekaligus:

1. **Pengontrol eksperimen.** Menyiarkan START/STOP ke semua sender dan menjalankan matriks tes (batch).
2. **Pencatat metrik sisi penerima.** Loss, goodput, jitter, celah antar-paket, RSSI.
3. **Pengumpul laporan sender.** Status kirim MAC dan RTT dari tiap node, dikirim lewat ESP-NOW setelah run.

Semua hasil dicetak ke Serial sebagai baris CSV dan ditambahkan ke file `/baseline_log.csv` di flash (LittleFS).

## File

| File | Isi |
|---|---|
| `baseline_receiver.ino` | Sketch utama |
| `espnow_proto.h` | Format paket bersama. **Harus identik** dengan salinan di `baseline_sender/` |

## Upload

- Board: **ESP32C6 Dev Module**
- Partition scheme: bawaan (sudah ada ruang untuk LittleFS)
- Port: UART (lewat chip USB-serial) atau USB native. Kalau pakai USB native, set **USB CDC On Boot = Enabled**.
- Serial Monitor: **921600**, line ending **New Line**. Kalau teks rusak, ubah `BAUD` ke 115200 dan upload ulang.

Saat boot, receiver mencetak MAC-nya, channel, status log, header CSV, dan daftar perintah.

## Perintah Serial

| Perintah | Fungsi |
|---|---|
| `help` | Daftar perintah |
| `status` | Keadaan receiver, progres batch, ukuran log, node yang terdengar |
| `nodes` | Node yang terdengar dan berapa detik lalu terakhir terdengar |
| `run N PAYLOAD INTERVAL_MS [DUR_S] [PROBE] [MODE]` | Satu run. Default `DUR_S=30`, `PROBE=20`, `MODE=0` |
| `batch [kunci=nilai ...]` | Menjalankan seluruh matriks secara otomatis (lihat di bawah) |
| `stop` | Membatalkan run atau batch yang sedang berjalan |
| `dump` | Mencetak seluruh isi log di flash |
| `clearlog` | Menghapus log di flash |
| `header` | Mencetak ulang header kolom CSV |

### Parameter `run`

| Parameter | Arti | Rentang |
|---|---|---|
| `N` | Jumlah node aktif (ID 1..N yang mengirim) | 1–32 |
| `PAYLOAD` | Ukuran paket ESP-NOW dalam byte, termasuk header 14 byte | 16–250 |
| `INTERVAL_MS` | Jeda antar kiriman per node | ≥ 1 |
| `DUR_S` | Durasi pengiriman | ≥ 1 |
| `PROBE` | Tiap paket ke-`PROBE` meminta echo untuk RTT. `0` = tanpa RTT | ≥ 0 |
| `MODE` | `0` = unicast ke receiver, `1` = broadcast | 0/1 |

Contoh: `run 10 128 50 30` (10 node, 128 byte, tiap 50 ms, 30 detik).

### Parameter `batch`

Semua argumen opsional. Daftar dipisah koma tanpa spasi.

| Kunci | Default | Arti |
|---|---|---|
| `nodes` | `1,5,10,20` | Jumlah node yang diuji (maks 8 nilai) |
| `payload` | `32,128,250` | Ukuran payload (maks 8 nilai) |
| `interval` | `100,50,20,10` | Interval kirim (maks 8 nilai) |
| `mode` | `0` | `0` unicast, `1` broadcast, `0,1` keduanya |
| `dur` | `30` | Durasi tiap run (detik) |
| `reps` | `3` | Jumlah pengulangan |
| `probe` | `20` | Probe RTT tiap paket ke-n |
| `jitter` | `0` | Jeda acak tambahan 0..n ms per kiriman. `0` = periodik murni |

Batch menjalankan **semua kombinasi** (`nodes × payload × interval × mode × reps`). Urutan: pengulangan paling luar, jadi tiap kombinasi tersebar di waktu yang berbeda. Sebelum mulai, receiver mencetak jumlah run, perkiraan durasi, dan node yang belum terdengar.

Contoh baseline tim:
```
batch nodes=1,5,10,20 payload=32,250 interval=50 dur=30 reps=3
```
24 run, sekitar 15 menit.

Batch tanpa argumen = 144 run, sekitar 1,5 jam.

## Alur satu run

| Tahap | Lama |
|---|---|
| START disiarkan 5 kali, node bersiap | 1,5 detik |
| Node mengirim data | `DUR_S` |
| Masa tenggang untuk paket telat | 0,5 detik |
| Menunggu laporan sender | sampai semua masuk, maks 3,5 detik + 120 ms × N |
| Mencetak baris D dan S | langsung |
| Jeda sebelum run berikutnya | 1 detik |

Selama run berjalan, Serial Monitor diam. Hasil hanya muncul setelah run selesai.

## Format keluaran

- `#...` : komentar atau progres. Abaikan saat parsing.
- `#HD,...` : header baris D. `#HS,...` : header baris S.
- `D,...` : satu baris per node per run.
- `S,...` : satu baris ringkasan per run.

Gunakan [`tools/split_log.py`](../tools/split_log.py) untuk memisahkan baris D dan S menjadi dua file CSV.

## Kamus kolom CSV

### Kolom konfigurasi (ada di baris D dan S)

| Kolom | Arti |
|---|---|
| `run_id` | ID unik run (acak saat receiver boot, lalu naik 1 tiap run) |
| `rep` | Pengulangan ke berapa (1..reps). Selalu 1 untuk perintah `run` |
| `mode` | 0 = unicast, 1 = broadcast |
| `nodes` | Jumlah node aktif |
| `payload` | Ukuran paket (byte) |
| `interval_ms` | Interval kirim per node |
| `duration_s` | Durasi pengiriman |
| `probe_every` | Probe RTT tiap paket ke-n (0 = mati) |
| `jitter_ms` | Jeda acak tambahan per kiriman (0 = periodik murni) |

### Baris D (per node)

| Kolom | Arti | Diukur di |
|---|---|---|
| `node` | ID node | |
| `report_ok` | 1 = laporan sender sampai. 0 = tidak sampai, kolom sisi sender bernilai 0 | |
| `tx_attempts` | Jumlah percobaan kirim (`tx_sent + tx_api_err`). Jika laporan tidak sampai: nomor urut terakhir yang diterima + 1 | sender |
| `tx_sent` | `esp_now_send()` diterima stack WiFi | sender |
| `tx_api_err` | `esp_now_send()` ditolak (mis. antrean TX penuh). Paket tidak pernah dipancarkan | sender |
| `mac_ok` | Callback kirim sukses (ACK MAC diterima) | sender |
| `mac_fail` | Callback kirim gagal setelah retry MAC | sender |
| `rx` | Paket data yang diterima receiver | receiver |
| `lost_gap` | Paket hilang menurut lompatan nomor urut | receiver |
| `reorder` | Paket yang datang telat atau dobel | receiver |
| `pdr_pct` | `rx / tx_attempts × 100`. PDR ujung ke ujung, termasuk paket yang ditolak stack | gabungan |
| `pdr_sent_pct` | `rx / tx_sent × 100`. PDR hanya untuk paket yang benar-benar dipancarkan | gabungan |
| `goodput_bps` | `byte diterima × 8 / duration_s` (byte = seluruh payload ESP-NOW) | receiver |
| `jitter_us` | Jitter antar-kedatangan (estimator RFC 3550, nilai di akhir run) | receiver |
| `max_gap_ms` | Celah terbesar antara dua paket berurutan yang diterima | receiver |
| `rssi_avg` | Rata-rata RSSI paket yang diterima (dBm) | receiver |
| `rssi_min` | RSSI terendah (dBm) | receiver |
| `probes` | Jumlah paket probe RTT yang dikirim | sender |
| `echoes` | Jumlah echo yang kembali | sender |
| `rtt_min_us`, `rtt_mean_us`, `rtt_std_us`, `rtt_max_us` | Statistik RTT (mikrodetik). Latency satu arah ≈ RTT/2 | sender |
| `slips` | Berapa kali sender tertinggal lebih dari satu interval dari jadwal | sender |

Hubungan yang berguna untuk analisis: biasanya `lost_gap ≈ tx_api_err + mac_fail`. Jadi loss bisa dipisah menjadi loss di antrean lokal sender (`tx_api_err`) dan loss di udara (`mac_fail`).

### Baris S (ringkasan per run)

| Kolom | Arti |
|---|---|
| `reports` | Jumlah node yang laporannya sampai |
| `tx_attempts` | Total percobaan kirim semua node |
| `rx` | Total paket diterima |
| `pdr_pct` | PDR gabungan (`rx / tx_attempts × 100`) |
| `offered_bps` | Beban yang ditawarkan: `tx_attempts × payload × 8 / duration_s` |
| `goodput_bps` | Goodput gabungan semua node |
| `jain_pdr` | Indeks keadilan Jain dari PDR per node (1 = semua node setara) |
| `worst_node`, `worst_pdr_pct` | Node dengan PDR terendah dan nilainya |
| `jitter_us_mean` | Rata-rata `jitter_us` antar node |
| `rtt_mean_us` | RTT rata-rata, dibobot jumlah echo per node |
| `mac_fail` | Total `mac_fail` semua node |
| `rssi_avg` | RSSI rata-rata semua paket yang diterima |

## Konfigurasi di kode

| Konstanta | Default | Arti |
|---|---|---|
| `BAUD` | 921600 | Baud Serial |
| `WIFI_CHANNEL` | 1 | Channel WiFi. Harus sama dengan semua sender |
| `WIFI_POWER_SAVE` | 0 | 0 = power save WiFi mati (baseline). Disiapkan untuk tes energi nanti |
| `USE_LITTLEFS` | 1 | Simpan log ke flash |
| `LOG_PATH` | `/baseline_log.csv` | Nama file log di flash |
| `START_LEAD_MS` | 1500 | Jeda dari perintah sampai run mulai |
| `GRACE_MS` | 500 | Masa tenggang paket telat |
| `REPORT_WAIT_BASE_MS`, `REPORT_WAIT_PER_NODE_MS` | 3500, 120 | Batas tunggu laporan sender |
| `GAP_BETWEEN_RUNS_MS` | 1000 | Jeda antar run dalam batch |
| `HELLO_PERIOD_MS` | 2000 | Periode HELLO broadcast saat idle |

## Catatan teknis

- **Echo dan ACK laporan dikirim lewat broadcast.** ESP-NOW hanya mengizinkan 20 peer, sedangkan 20 sender ditambah alamat broadcast berjumlah 21. Karena itu receiver hanya memakai satu peer (broadcast) untuk semua balasan. Tiap balasan membawa ID node, dan sender mengabaikan balasan yang bukan miliknya. Menerima data dari sender tidak membutuhkan peer sama sekali.
- **Akibat broadcast:** echo tidak di-ACK atau di-retry di tingkat MAC, jadi saat kanal padat sebagian echo bisa hilang (`echoes < probes`). RTT dihitung dari echo yang kembali. ACK laporan yang hilang tidak masalah karena sender mengirim ulang laporannya (maks 8 kali).
- **RTT** diukur di sender: waktu kirim probe sampai echo diterima, termasuk waktu proses di receiver. Echo dikirim dari task terpisah supaya callback penerimaan tetap singkat.
- **Callback penerimaan tidak memakai Serial.** Semua penghitungan selesai di callback, pencetakan dilakukan setelah run. Jadi baud Serial tidak memengaruhi metrik.
- `echo_drop` dan `echo_fail` di baris komentar akhir run menunjukkan echo atau ACK yang gagal dikirim receiver. Nilai besar berarti RTT dan laporan bisa kurang lengkap.
- **Jangan tutup lalu buka lagi Serial Monitor saat batch berjalan** jika memakai port UART. Di banyak board ini me-reset ESP32. Run yang sudah selesai tetap aman di flash.

## Troubleshooting

| Gejala | Penyebab dan solusi |
|---|---|
| Teks berantakan | Baud Serial Monitor tidak sama dengan `BAUD` |
| `perintah tidak dikenal` padahal benar | Line ending Serial Monitor bukan New Line |
| `nodes` kosong | Sender belum di-set ID, baterai mati, atau `WIFI_CHANNEL` berbeda |
| Semua kolom bernilai 0 dan `laporan 0/N` | ID sender di luar 1..N. Contoh: sender ID 16 dengan `run 1 ...` |
| `report_ok=0` di beberapa node | Laporan hilang. Sesekali wajar. Kalau sering, cek jarak dan RSSI node itu |
| `log=tidak aktif` saat boot | LittleFS gagal. Pilih partition scheme yang punya SPIFFS/LittleFS |
