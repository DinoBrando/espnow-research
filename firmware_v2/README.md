# Firmware v2: ESP-NOW baseline dan 6 metode adaptif

Satu firmware receiver dan satu firmware sender berisi baseline dan keenam metode. Metode dipilih dari receiver lewat perintah, jadi ke-20 sender tidak perlu di-flash ulang saat ganti metode.

| Folder | Board | Jumlah |
|---|---|---|
| [`receiver_v2/`](receiver_v2/) | ESP32-C6 (pengontrol + pencatat) | 1 |
| [`sender_v2/`](sender_v2/) | ESP32 (board CH340) | 20 |

**Firmware v2 tidak bisa bicara dengan firmware v1.** Semua board harus di-flash v2. Perintah `nodes` di receiver menandai node yang versinya berbeda.

## Metode

| ID | Metode | Yang diubah | Dasar keputusan |
|---|---|---|---|
| 0 | baseline | tidak ada | |
| 1 | rate_control | kecepatan bit radio: 1 → 2 → 5,5 → 11 → 24 → 54 Mbps | PDR link tiap 1 dtk (berhasil vs gagal di MAC; paket ditolak antrean tidak dihitung) |
| 2 | dynamic_interval | interval kirim (nominal sampai 8× nominal) | ada loss (antrean + MAC) dalam 1 dtk: × 2; 3 dtk lancar: −10 ms |
| 3 | dynamic_backoff | jeda acak 0..CW sebelum tiap kiriman | CW mulai 10 ms, × 2 tiap gagal (maks 320 ms), kembali ke 10 ms saat berhasil |
| 4 | tx_power | daya pancar (2–20 dBm, langkah 2 dBm) | RSSI di receiver (dari STATUS). Target −70 sampai −60 dBm |
| 5 | ack_retx | kirim ulang paket yang belum diterima, maks 2 kali | ACK bitmap 128 paket di STATUS |
| 6 | channel_hopping | kanal 1 → 6 → 11 | receiver memutuskan: gagal MAC ≥ 10% atau paket diterima < 10% dari harapan, 3 dtk berturut-turut |

Semua angka di atas adalah nilai bawaan dan bisa diubah tanpa flash ulang (lihat `params` dan `set` di [README receiver](receiver_v2/README.md)).

## Aturan bersama semua metode

- Data dibangkitkan tiap **interval nominal** (`interval=`). Sender hanya menyimpan **sampel terbaru**. Kalau metode menunda pengiriman, sampel lama ditimpa dan dicatat sebagai `skipped`.
- Receiver mengirim **STATUS 1 kali per detik** (per kelompok 10 node) di **semua metode**, termasuk baseline, supaya beban tambahannya sama. Isinya: ACK bitmap, RSSI tiap node, perintah pindah kanal.
- Semua balasan receiver (STATUS, echo RTT, ACK laporan) dikirim **broadcast** berisi ID node tujuan. Receiver hanya memakai 1 peer, jadi batas 20 peer ESP-NOW tidak tersentuh. Data dari sender tetap unicast.
- **Masa tenggang 5 detik**: paket yang tiba sampai 5 dtk setelah run selesai tetap dihitung sampai, dan dicatat di `rx_late`.
- Setiap run dimulai dari kondisi yang sama: 1 Mbps, daya maksimal, kanal 1, interval nominal.
- Versi firmware, alasan reset node, dan parameter metode tercatat di log.

## Uji coba dengan 3 ESP (sebelum flash 20 node)

Gunakan N01, N02 (dengan INA219), dan N03.

### Persiapan

1. Arduino IDE: Boards Manager → esp32 by Espressif → **3.3.12**.
2. Upload `receiver_v2` ke ESP32-C6.
3. Upload `sender_v2` ke 3 board. ID lama tetap tersimpan. Kalau board baru, ketik `id 1`, `id 2`, `id 3` di Serial Monitor (115200).
4. Di Serial Monitor tiap sender, ketik `status` lalu `ina`. N01 dan N02 harus menampilkan tegangan dan arus.
5. **Cabut USB dari N01 dan N02**, beri daya lewat baterai (melalui INA219).
6. Di receiver (921600, line ending New Line): `clearlog`, lalu `nodes`. Harus ada N01–N03 dengan fw 2.0, `ina` = 1 untuk N01 dan N02.

### Uji per metode (masing-masing 20 detik)

```
run 3 32 50 20 method=0
run 3 32 50 20 method=1
run 3 32 50 20 method=2
run 3 32 50 20 method=3
run 3 32 50 20 method=4
run 3 32 50 20 method=5
run 3 32 50 30 method=6 hop_rx=100 hop_win=1
set hop_rx=10 hop_win=3
run 3 250 20 20 method=0
```

Yang dicek di baris D:

| Metode | Tanda berfungsi |
|---|---|
| semua | `report_ok`=1, `fw_tx`=2.0, `pdr_gen_pct` mendekati 100 |
| 0 | `tx_new` = `generated`, `skipped` = 0 |
| 1 | `rate_final_mbps` naik (dengan 3 node, kemungkinan sampai 54), `rate_changes` > 0 |
| 2 | `intv_final_ms` = 50 (kanal lancar, tidak perlu memperbesar interval) |
| 3 | `backoff_mean_ms` sekitar 5 |
| 4 | `txp_final_dbm` turun jika `rssi_avg` di atas −60 |
| 5 | `ack_confirmed` mendekati `tx_new` |
| 6 | `hops` > 0, `channel_final` bukan 1, dan **semua node tetap `report_ok`=1** (ikut pindah). Parameter `hop_rx=100 hop_win=1` sengaja membuat receiver gampang pindah, khusus untuk uji ini |
| N01, N02 | `ina_ok`=1, `energy_mj` > 0, `i_avg_ma` sekitar 100–150 |

Setelah selesai: `dump`, salin ke `log.txt`, lalu kirim untuk dicek.

## Prosedur sesi resmi

1. Isi penuh semua baterai. Susun node di jarak 6 m, foto, dan catat.
2. `clearlog`, lalu `nodes` (harus 20 node, semua fw 2.0).
3. Batch pertama (interval 50 ms, ±28 menit):
   ```
   batch method=M interval=50
   ```
4. `nodes`, ganti baterai yang lemah kalau perlu.
5. Batch kedua (interval 20 ms):
   ```
   batch method=M interval=20
   ```
6. `dump`, simpan ke `log.txt`, lalu `python ../tools/split_log.py log.txt`.

Nilai bawaan `batch`: `nodes=1,5,10,20 payload=32,250 dur=60 reps=3 probe=20`.

Channel Hopping diuji dua kali dengan sumber gangguan di kanal 1: `method=0` (gangguan tanpa metode) dan `method=6` (gangguan dengan metode).
