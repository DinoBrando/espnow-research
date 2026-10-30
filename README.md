# ESP-NOW Baseline: Pengukuran Kinerja Multi-Node

Firmware untuk mengukur kinerja mentah ESP-NOW (tanpa mekanisme adaptif) dengan 1 receiver dan sampai 20 node pengirim. Hasil ini menjadi pembanding untuk enam metode adaptif yang akan diuji berikutnya (Rate Control, Dynamic Interval, Dynamic Backoff, Tx Power Control, ACK dan Retransmisi, Channel Hopping).

## Isi repo

| Folder | Isi |
|---|---|
| [`baseline_receiver/`](baseline_receiver/) | Firmware receiver ESP32-C6: pengontrol eksperimen sekaligus pencatat metrik |
| [`baseline_sender/`](baseline_sender/) | Firmware pengirim, satu firmware yang sama untuk semua node |
| [`tools/`](tools/) | `split_log.py`: mengubah log Serial receiver menjadi file CSV |
| [`results/`](results/) | Tempat menyimpan log mentah dan CSV hasil tes |

## Cara kerja singkat

- Receiver terhubung ke laptop lewat USB. Semua sender memakai baterai.
- Receiver menyiarkan perintah START berisi parameter run (jumlah node, ukuran payload, interval, durasi). Sender tidak perlu di-flash ulang untuk tiap skenario.
- Run dengan `N` node berarti hanya sender ber-ID **1 sampai N** yang mengirim. Sender lain diam.
- Data dikirim **unicast** ke receiver. Broadcast hanya dipakai untuk pesan kontrol (HELLO, START, STOP) di luar periode pengukuran.
- Receiver menghitung loss, goodput, jitter, dan RSSI. Sender menghitung status kirim MAC dan RTT, lalu mengirim laporannya ke receiver setelah run selesai.
- Semua hasil keluar di Serial Monitor receiver sebagai baris CSV dan juga disimpan di flash receiver (LittleFS).

## Kebutuhan

- Arduino IDE 2.x
- Paket board **esp32 by Espressif Systems** versi **3.0.0 atau lebih baru** (disarankan versi stabil, bukan alpha)
- 1 × ESP32-C6 untuk receiver, sampai 20 × ESP32 untuk sender
- Python 3 (hanya untuk `tools/split_log.py`, tanpa library tambahan)

## Prosedur tes baseline

Desain tes: 1, 5, 10, dan 20 node; payload ringan (32 byte) dan berat (250 byte); interval kirim 50 ms; durasi 30 detik; 3 kali pengulangan. Total 24 run, sekitar 15 menit.

### A. Persiapan (sekali)

1. Upload `baseline_sender` ke semua board pengirim.
2. Saat board masih tercolok USB, buka Serial Monitor (115200, line ending **New Line**), ketik `id 1`, `id 2`, dan seterusnya. ID harus **1 sampai 20, tanpa lompatan dan tanpa dobel**. ID tersimpan di flash.
3. Tempel label N01 sampai N20 di board.
4. Isi penuh semua baterai.
5. Susun node sesuai [panduan penempatan](#panduan-penempatan-node).

### B. Menjalankan tes

6. Upload `baseline_receiver` ke ESP32-C6, colok ke laptop, buka Serial Monitor (**921600**, **New Line**).
7. Nyalakan semua sender.
8. Ketik `clearlog` untuk menghapus log uji coba lama.
9. Ketik `nodes`. Harus tertulis `total=20`.
10. Cek posisi dan RSSI: `run 20 32 100 10`, lalu lihat kolom `rssi_avg` di baris D (target -45 sampai -70 dBm).
11. Ketik `clearlog` lagi supaya run cek posisi tidak ikut tercatat.
12. Jalankan:
    ```
    batch nodes=1,5,10,20 payload=32,250 interval=50 dur=30 reps=3
    ```
13. Tunggu sampai muncul `# BATCH SELESAI`. Jangan tutup Serial Monitor, jangan biarkan laptop tidur, dan jangan pindahkan node.

### C. Export ke CSV

14. Ketik `dump` di receiver.
15. Klik area keluaran Serial Monitor, tekan Ctrl+A lalu Ctrl+C, tempel ke Notepad, simpan sebagai `log.txt`.
16. Jalankan:
    ```
    python tools/split_log.py log.txt
    ```
    Untuk Excel berbahasa Indonesia (pemisah `;`, desimal `,`), tambahkan `--excel`.
17. Hasil: `log_per_node.csv` (satu baris per node per run) dan `log_ringkasan.csv` (satu baris per run). Simpan di `results/` dengan nama yang jelas, misalnya `results/2026-10-30_baseline_ch1/`.

Penjelasan tiap kolom CSV ada di [README receiver](baseline_receiver/README.md#kamus-kolom-csv).

## Panduan penempatan node

```
            N01  N02  N03  N04  N05
         N20                        N06
       N19                            N07
       N18          [RECEIVER]        N08
       N17                            N09
         N16                        N10
            N15  N14  N13  N12  N11

   receiver ke tiap node: ±1,5–2 m     antar node: minimal ±20 cm
```

- Jarak receiver ke semua node kurang lebih sama, supaya loss yang terukur karena kongesti, bukan karena jarak.
- Semua board menghadap ke arah yang sama. Antena PCB tidak tertutup baterai atau logam.
- Angkat node dari lantai dan dari meja logam.
- Tidak ada orang yang lalu-lalang di antara node dan receiver selama tes.
- Laptop tidak tepat di samping receiver. Matikan WiFi laptop kalau bisa.
- **Pakai susunan yang sama persis** untuk baseline dan keenam metode adaptif.

## Yang harus dicatat di tiap sesi tes

Simpan di `results/<tanggal>_<nama>/catatan.md`:

- Tanggal, jam, dan lokasi
- Foto susunan node
- Jarak receiver ke node, jarak antar node, tinggi node dari lantai
- Channel WiFi (`WIFI_CHANNEL`) dan perkiraan jumlah WiFi lain di sekitar
- Jenis baterai dan kondisinya
- Versi paket board esp32 di Arduino IDE
- Perintah `batch` yang dipakai
- Kejadian tidak normal (node mati, receiver reset, dan sebagainya)

## Aturan kolaborasi

- `espnow_proto.h` ada di dua folder karena Arduino IDE hanya membaca file di dalam folder sketch. **Kedua salinan harus selalu identik.** Kalau mengubah salah satu, salin ke folder lain di commit yang sama.
- `WIFI_CHANNEL` harus sama di receiver dan semua sender.
- Jangan mengubah parameter baseline (interval, payload, power save, channel) di tengah pengambilan data. Kalau perlu diubah, mulai sesi baru dan catat alasannya.
- Simpan log mentah (`log.txt`) bersama CSV-nya, supaya hasil bisa diproses ulang kalau skrip berubah.
