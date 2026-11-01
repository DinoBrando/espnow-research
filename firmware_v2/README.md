# Firmware v2.1: ESP-NOW baseline dan 6 metode adaptif

Satu firmware receiver dan satu firmware sender berisi baseline dan keenam metode. Metode dipilih dari receiver lewat perintah, jadi ke-20 sender tidak perlu di-flash ulang saat ganti metode.

| Folder | Board | Jumlah |
|---|---|---|
| [`receiver_v2/`](receiver_v2/) | ESP32-C6 (pengontrol + pencatat) | 1 |
| [`sender_v2/`](sender_v2/) | ESP32 (board CH340) | 20 |

**Firmware v2 tidak bisa bicara dengan firmware v1.** Semua board harus di-flash versi yang sama. Perintah `nodes` di receiver menandai node yang versinya berbeda.

## Riwayat versi

| Versi | Perubahan |
|---|---|
| 2.0 | Versi pertama (uji 2 node 2026-11-01) |
| 2.1 | Dynamic Interval: jadwal kirim tidak lagi bergeser karena keterlambatan loop. ACK: paket di detik terakhir dinilai dari STATUS masa tenggang; yang belum jelas dicatat di kolom baru `ack_unresolved`, bukan `ack_giveup`. `hop_rx` boleh di atas 100 untuk uji paksa pindah kanal |

## Metode

| ID | Metode | Yang diubah | Dasar keputusan |
|---|---|---|---|
| 0 | baseline | tidak ada | |
| 1 | rate_control | kecepatan bit radio: 1 → 2 → 5,5 → 11 → 24 → 54 Mbps | PDR link tiap 1 dtk (berhasil vs gagal di MAC; paket ditolak antrean tidak dihitung) |
| 2 | dynamic_interval | interval kirim (nominal sampai 8× nominal) | ada loss (antrean + MAC) dalam 1 dtk: × 2; 3 dtk lancar: −10 ms |
| 3 | dynamic_backoff | jeda acak 0..CW sebelum tiap kiriman | CW mulai 10 ms, × 2 tiap gagal (maks 320 ms), kembali ke 10 ms saat berhasil |
| 4 | tx_power | daya pancar, dari daya bawaan board (±15,5 dBm) turun sampai ±2 dBm, langkah 2 dBm | RSSI di receiver (dari STATUS). Target −70 sampai −60 dBm |
| 5 | ack_retx | kirim ulang paket yang belum diterima, maks 2 kali | ACK bitmap 128 paket di STATUS |
| 6 | channel_hopping | kanal 1 → 6 → 11 | receiver memutuskan: gagal MAC ≥ 10% atau paket diterima < 10% dari harapan, 3 dtk berturut-turut |

Semua angka di atas adalah nilai bawaan dan bisa diubah tanpa flash ulang (lihat `params` dan `set` di [README receiver](receiver_v2/README.md)).

Catatan daya pancar: daya maksimal board sender terbaca ±15,5 dBm (bawaan core). Tx Power Control hanya menurunkan daya dari titik itu dan menaikkannya kembali paling tinggi ke titik itu, sehingga daya awal semua metode sama.

## Aturan bersama semua metode

- Data dibangkitkan tiap **interval nominal** (`interval=`). Sender hanya menyimpan **sampel terbaru**. Kalau metode menunda pengiriman, sampel lama ditimpa dan dicatat sebagai `skipped`.
- Receiver mengirim **STATUS 1 kali per detik** (per kelompok 10 node) di **semua metode**, termasuk baseline, supaya beban tambahannya sama. Isinya: ACK bitmap, RSSI tiap node, perintah pindah kanal. STATUS tetap dikirim selama masa tenggang.
- Semua balasan receiver (STATUS, echo RTT, ACK laporan) dikirim **broadcast** berisi ID node tujuan. Receiver hanya memakai 1 peer, jadi batas 20 peer ESP-NOW tidak tersentuh. Data dari sender tetap unicast.
- **Masa tenggang 5 detik**: paket yang tiba sampai 5 dtk setelah run selesai tetap dihitung sampai, dan dicatat di `rx_late`.
- Setiap run dimulai dari kondisi yang sama: 1 Mbps, daya bawaan, kanal 1, interval nominal.
- Kiriman ulang (metode 5) hanya selama run. Paket yang hilang di detik terakhir tidak dikirim ulang.
- Versi firmware, alasan reset node, dan parameter metode tercatat di log.

## Uji coba sebelum flash 20 node

Gunakan N01 dan N02 (dengan INA219), plus N03 kalau ada. Contoh di bawah untuk 2 node; untuk 3 node ganti `nodes=2` menjadi `nodes=3`.

### Persiapan

1. Arduino IDE: Boards Manager → esp32 by Espressif → **3.3.12**.
2. Upload `receiver_v2/receiver_v2.ino` ke ESP32-C6. Pesan boot harus `=== ESP-NOW Receiver/Controller v2.1 ===`.
3. Upload `sender_v2/sender_v2.ino` ke sender. ID lama tetap tersimpan. Kalau board baru, ketik `id 1`, `id 2` di Serial Monitor (115200).
4. Di Serial Monitor tiap sender, ketik `status` (harus `fw=2.1`) lalu `ina`.
5. **Cabut USB dari N01 dan N02**, beri daya lewat baterai (melalui INA219).
6. Di receiver (921600, line ending New Line): `clearlog`, lalu `nodes`. Harus ada N01 dan N02 dengan fw 2.1, `ina` = 1.

### Uji semua metode sekaligus (±5 menit)

```
batch nodes=2 payload=32 interval=50 dur=30 reps=1 method=0,1,2,3,4,5,6 hop_rx=200 hop_win=1
```

Tunggu `# BATCH SELESAI`, lalu **wajib** kembalikan parameter hop:

```
set hop_rx=10 hop_win=3
```

`hop_rx=200` sengaja membuat receiver pindah kanal setiap ±5 detik di metode 6, khusus untuk menguji mekanisme pindah kanal. Metode lain mengabaikan parameter ini.

Yang dicek di baris D:

| Metode | Tanda berfungsi |
|---|---|
| semua | `report_ok`=1, `fw_tx`=2.1, `pdr_gen_pct` mendekati 100 |
| 0 | `tx_new` = `generated`, `skipped` = 0 |
| 1 | `rate_final_mbps` naik (dengan 2–3 node biasanya sampai 54), `rate_changes` > 0 |
| 2 | `intv_final_ms` = 50, `age_mean_us` kecil (puluhan µs, sama seperti baseline) |
| 3 | `backoff_mean_ms` sekitar 4–5 |
| 4 | `txp_final_dbm` turun jika `rssi_avg` di atas −60 |
| 5 | `ack_confirmed` = `tx_new`, `ack_giveup` = 0, `ack_unresolved` = 0 (tanpa loss) |
| 6 | `hops` > 0, `channel_final` bervariasi, kolom S `rx_hops` > 0, dan **semua node tetap `report_ok`=1** dengan `pdr_gen_pct` tinggi (ikut pindah) |
| N01, N02 | `ina_ok`=1, `energy_mj` > 0, `i_avg_ma` sekitar 150–160, `v_avg_v` sekitar 5,1–5,2 |

Setelah selesai: `dump`, salin ke `log.txt`, lalu kirim untuk dicek.

## Prosedur sesi resmi

1. Isi penuh semua baterai. Susun node di jarak 6 m, foto, dan catat.
2. `clearlog`, lalu `nodes` (harus 20 node, semua versi sama).
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
