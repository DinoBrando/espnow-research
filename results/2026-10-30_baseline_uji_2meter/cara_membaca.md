# Cara membaca hasil baseline 2026-10-30

Dokumen ini menjelaskan isi folder ini supaya orang lain (atau model AI) bisa menganalisis datanya tanpa konteks tambahan. Baca juga `catatan_validasi.md`.

## 1. Konteks penelitian

Penelitian ini membandingkan enam metode adaptif untuk mengurangi kongesti pada jaringan ESP-NOW multi-node: Rate Control, Dynamic Interval, Dynamic Backoff, Tx Power Control, ACK dan Retransmisi, serta Channel Hopping. Folder ini berisi **baseline**: kinerja ESP-NOW tanpa mekanisme adaptif apa pun. Semua metode nantinya dibandingkan terhadap angka ini.

Status sesi: **uji coba**. Datanya valid dan konsisten, tetapi ada dua kelemahan pengukuran (bagian 7) yang rencananya diperbaiki sebelum baseline resmi.

## 2. Desain eksperimen

| Aspek | Nilai |
|---|---|
| Topologi | Bintang: 1 receiver, N sender, semua sender mengirim ke receiver |
| Receiver | ESP32-C6, terhubung ke laptop lewat USB |
| Sender | Board ESP32, daya baterai, ID 1–20 |
| Jumlah node aktif (N) | 1, 5, 10, 20. Run dengan N node berarti hanya sender ber-ID 1..N yang mengirim |
| Payload | 32 byte (ringan) dan 250 byte (berat). Ukuran ini adalah seluruh payload ESP-NOW, termasuk header aplikasi 14 byte |
| Interval kirim | 50 ms per node (20 paket per detik per node), periodik tanpa jitter |
| Durasi per run | 30 detik, jadi 600 percobaan kirim per node per run |
| Pengulangan | 3 kali per kombinasi. Urutan: ulangan paling luar (rep 1 semua kombinasi, lalu rep 2, lalu rep 3) |
| Total | 24 run, 216 baris per node |
| Mode kirim | Unicast ke MAC receiver (`mode=0`) |
| Channel WiFi | 1 |
| Power save WiFi | Mati |
| PHY rate | Default ESP-NOW (tidak diubah) |
| Probe RTT | Tiap paket ke-20 meminta echo (30 probe per node per run) |
| Offset awal | Tiap node mulai dengan offset acak 0..50 ms, lalu periodik |

Belum dicatat dan perlu dilengkapi tim: jenis board sender, jenis baterai, jarak dan susunan node, kondisi WiFi sekitar. RSSI tiap node (bagian 6) bisa dipakai sebagai pengganti informasi jarak.

## 3. Daftar file

| File | Isi | Pakai untuk analisis? |
|---|---|---|
| `log.txt` | Salinan mentah keluaran `dump` receiver | Sumber asli. Dipakai kalau CSV perlu dibuat ulang |
| `log_per_node.csv` | 216 baris, satu per node per run | **Ya**, data utama |
| `log_ringkasan.csv` | 24 baris, satu per run, dihitung receiver | Ya |
| `ringkasan_per_konfigurasi.csv` | 8 baris, rata-rata 3 ulangan per kombinasi node × payload, dihitung ulang dari `log_per_node.csv` | **Ya**, untuk tabel hasil |
| `20node_berat_per_node.csv` | 20 baris, PDR tiap node di 20 node payload berat | Ya, untuk analisis keadilan |
| `log_per_node_EXCEL.csv`, `log_ringkasan_EXCEL.csv` | Isi sama, format Excel Indonesia (pemisah `;`, desimal `,`) | **Jangan**, duplikat untuk dibuka di Excel |
| `catatan_validasi.md` | Hasil pemeriksaan kelengkapan dan anomali | Baca dulu |

Semua CSV memakai pemisah koma, desimal titik, encoding UTF-8, dengan satu baris header.

## 4. Kolom `log_per_node.csv`

Kunci unik: (`run_id`, `node`).

### Konfigurasi

| Kolom | Arti |
|---|---|
| `run_id` | ID run (1720–1743) |
| `rep` | Ulangan ke (1–3) |
| `mode` | 0 = unicast |
| `nodes` | Jumlah node aktif di run ini |
| `payload` | Ukuran paket (byte) |
| `interval_ms` | Interval kirim per node |
| `duration_s` | Durasi run |
| `probe_every` | Probe RTT tiap paket ke-n |
| `jitter_ms` | Jitter kirim acak (0 = periodik) |
| `node` | ID node |
| `report_ok` | 1 = laporan sender sampai ke receiver (semua baris bernilai 1) |

### Sisi sender (dikirim sender ke receiver setelah run)

| Kolom | Arti |
|---|---|
| `tx_attempts` | Percobaan kirim = `tx_sent + tx_api_err`. Selalu 600 di sesi ini |
| `tx_sent` | Paket yang diterima stack WiFi (`esp_now_send()` sukses) |
| `tx_api_err` | Paket yang ditolak `esp_now_send()`, umumnya karena antrean TX penuh. **Tidak pernah dipancarkan** |
| `mac_ok` | Paket yang mendapat ACK di tingkat MAC 802.11 (callback status kirim sukses) |
| `mac_fail` | Paket yang gagal setelah semua retry MAC |
| `slips` | Berapa kali sender tertinggal lebih dari satu interval dari jadwal. Semua 0, jadi CPU sender tidak menjadi hambatan |
| `probes` | Probe RTT yang dikirim (30) |
| `echoes` | Echo yang kembali |
| `rtt_min_us`, `rtt_mean_us`, `rtt_std_us`, `rtt_max_us` | Statistik RTT dalam **mikrodetik**, dari echo yang kembali saja |

### Sisi receiver

| Kolom | Arti |
|---|---|
| `rx` | Paket data diterima, **termasuk duplikat** |
| `lost_gap` | Paket hilang menurut lompatan nomor urut. **Tidak menghitung paket hilang di ekor run** (setelah paket terakhir yang diterima) |
| `reorder` | Paket dengan nomor urut ≤ yang terakhir diterima. Di sesi ini praktis = duplikat |
| `pdr_pct` | `rx / tx_attempts × 100`, dibatasi maks 100. Karena `rx` termasuk duplikat, sedikit bias ke atas |
| `pdr_sent_pct` | `rx / tx_sent × 100`, dibatasi maks 100 |
| `goodput_bps` | `jumlah byte payload diterima × 8 / 30` (termasuk duplikat dan header 14 byte) |
| `jitter_us` | Jitter antar-kedatangan, estimator RFC 3550 (`J += (|D| − J)/16`), **nilai di akhir run**, bukan rata-rata |
| `max_gap_ms` | Celah terbesar antara dua paket berurutan yang diterima |
| `rssi_avg` | Rata-rata RSSI paket diterima (dBm) |
| `rssi_min` | RSSI terendah (dBm) |

## 5. Kolom file turunan

### `ringkasan_per_konfigurasi.csv`

Dihitung dari `log_per_node.csv`. Kunci: (`nodes`, `payload`). "Rata-rata" = rata-rata dari 3 ulangan; "std" = simpangan baku sampel antar ulangan.

| Kolom | Rumus |
|---|---|
| `beban` | `ringan` (32 B) atau `berat` (250 B) |
| `reps` | Jumlah ulangan (3) |
| `offered_kbps` | `nodes × 20 paket/detik × payload × 8 / 1000` |
| `pdr_mean`, `pdr_std` | PDR per run = `Σ(rx − reorder) / Σ tx_attempts × 100` (paket unik) |
| `goodput_kbps_mean`, `goodput_kbps_std` | Per run: `Σ(rx − reorder) × payload × 8 / 30 / 1000` |
| `jain_mean` | Per run: indeks Jain dari PDR unik tiap node, `(Σx)² / (n × Σx²)` |
| `worst_node_pdr_mean` | Per run: PDR unik terendah di antara node |
| `jitter_ms_mean`, `jitter_ms_std` | Dari `jitter_us_mean` di `log_ringkasan.csv`, dibagi 1000 |
| `rtt_ms_mean`, `rtt_ms_std` | Dari `rtt_mean_us` di `log_ringkasan.csv` (rata-rata dibobot jumlah echo), dibagi 1000 |
| `echo_return_pct` | `Σ echoes / Σ probes × 100` |
| `tx_attempts_total` | Jumlah percobaan kirim dari 3 ulangan |
| `lost_total` | `tx_attempts_total − Σ(rx − reorder)` |
| `lost_tx_api_err` | `Σ tx_api_err` (hilang di antrean sender) |
| `lost_mac_fail` | `Σ mac_fail` (hilang di udara) |
| `lost_late_unrecorded` | `Σ max(0, mac_ok − (rx − reorder))` (sudah di-ACK MAC tetapi tidak tercatat receiver) |
| `radio_duplicates` | `Σ reorder` |

Sisa loss yang tidak masuk tiga kategori di atas = `lost_total − lost_tx_api_err − lost_mac_fail − lost_late_unrecorded`. Di 20 node berat nilainya 87, yaitu paket yang status kirimnya tidak tercatat (lihat bagian 7).

### `20node_berat_per_node.csv`

| Kolom | Arti |
|---|---|
| `node` | ID node |
| `rssi_avg` | Rata-rata `rssi_avg` node itu dari 3 ulangan (dBm) |
| `pdr_rep1`, `pdr_rep2`, `pdr_rep3` | PDR unik di tiap ulangan |
| `pdr_mean` | Rata-rata ketiganya |
| `tx_api_err_mean` | Rata-rata paket yang ditolak antrean per ulangan |

## 6. Hasil utama (untuk cek kewajaran)

| Node | Beban | Ditawarkan (kbps) | PDR (%) | Goodput (kbps) | Jain | Jitter (ms) | RTT (ms) |
|---|---|---|---|---|---|---|---|
| 1 | ringan | 5,1 | 100 | 5,1 | 1,000 | 0,19 | 2,6 |
| 1 | berat | 40,0 | 100 | 40,0 | 1,000 | 0,23 | 4,4 |
| 5 | ringan | 25,6 | 100 | 25,6 | 1,000 | 0,30 | 3,4 |
| 5 | berat | 200,0 | 100 | 200,0 | 1,000 | 0,95 | 7,0 |
| 10 | ringan | 51,2 | 100 | 51,2 | 1,000 | 0,29 | 3,5 |
| 10 | berat | 400,0 | 100 | 400,0 | 1,000 | 1,94 | 12,6 |
| 20 | ringan | 102,4 | 100 | 102,4 | 1,000 | 0,56 | 7,7 |
| 20 | berat | 800,0 | 74,41 ± 0,20 | 595,2 | 0,894 | 64,97 | 1166,6 |

Hal yang sudah diketahui:

- Loss hanya terjadi di **20 node payload berat**. Semua kombinasi lain PDR 100%.
- Di 20 node berat, **91% loss terjadi di antrean sender** (`tx_api_err` 8.396 dari 9.214). Loss di udara (`mac_fail`) hanya 142.
- Goodput mentok di sekitar 595 kbps, jadi kapasitas efektif kanal di konfigurasi ini sekitar 600 kbps.
- **Unfairness kuat dan konsisten antar ulangan.** Korelasi Pearson RSSI rata-rata vs PDR rata-rata per node = 0,87. Node dengan RSSI ≥ −57 dBm selalu 100%, node di bawah −63 dBm sekitar 33–55%. Dugaan penyebab: capture effect (frame lebih kuat menang saat tabrakan), node lemah lebih sering retry, antreannya penuh.
- RTT di 20 node berat sebagian besar adalah waktu tunggu di antrean sender (probe mengantre di belakang paket data).
- RSSI tiap node stabil sepanjang sesi (berubah maks 3,8 dB), jadi node tidak berpindah.
- RSSI antar node tersebar dari −44 sampai −75 dBm (selisih sekitar 30 dB).

## 7. Keterbatasan pengukuran

Perhatikan ini sebelum menarik kesimpulan:

1. **Paket terlambat dihitung hilang.** Receiver berhenti menghitung 0,5 detik setelah run berakhir. Di 20 node berat, 589 paket sudah di-ACK MAC tetapi tidak tercatat (bukti: misalnya run 1735 node 1 punya `lost_gap=0` tetapi kurang 15 paket, berarti semuanya hilang di ekor run). PDR 74,41% menghitungnya sebagai hilang. Jika dihitung sampai, PDR 76,04%.
2. **Status kirim sebagian paket tidak tercatat.** Sender berhenti menghitung `mac_ok`/`mac_fail` saat mengirim laporan pertama, padahal paket di antrean masih bisa terkirim setelahnya. Akibatnya `mac_ok + mac_fail` bisa lebih kecil dari `tx_sent` (selisih 87 di 20 node berat), dan 589 di atas adalah batas bawah.
3. **RTT di run 20 node sedikit terpengaruh** penukaran peer di receiver (batas 20 peer ESP-NOW; receiver versi ini menghapus dan menambah peer saat mengirim echo).
4. **Kolom `pdr_pct` dan `goodput_bps` di file mentah memasukkan duplikat.** Untuk analisis, pakai `rx − reorder` atau file `ringkasan_per_konfigurasi.csv`. Ada 4 duplikat di seluruh sesi.
5. **`lost_gap` tidak menangkap loss di ekor run.** Untuk jumlah hilang, pakai `tx_attempts − (rx − reorder)`.
6. **`jitter_us` adalah nilai akhir estimator**, bukan rata-rata sepanjang run.
7. Latency satu arah tidak diukur langsung. Pendekatan yang dipakai: RTT/2, dengan asumsi jalur pergi dan pulang simetris.
8. Hanya satu interval (50 ms) dan dua ukuran payload yang diuji. Titik jenuh di antara 400 dan 800 kbps belum dipetakan.

## 8. Contoh memuat data (Python)

```python
import pandas as pd

d = pd.read_csv("log_per_node.csv")
d["rx_unique"] = d["rx"] - d["reorder"]
d["pdr_unique"] = 100 * d["rx_unique"] / d["tx_attempts"]
d["late"] = (d["mac_ok"] - d["rx_unique"]).clip(lower=0)

per_run = d.groupby(["run_id", "rep", "nodes", "payload"]).agg(
    attempts=("tx_attempts", "sum"),
    rx_unique=("rx_unique", "sum"),
    api_err=("tx_api_err", "sum"),
    mac_fail=("mac_fail", "sum"),
).reset_index()
per_run["pdr"] = 100 * per_run["rx_unique"] / per_run["attempts"]

print(per_run.groupby(["nodes", "payload"])["pdr"].agg(["mean", "std"]))
```

## 9. Analisis yang disarankan

- PDR, goodput, jitter, dan RTT terhadap jumlah node, dipisah ringan dan berat.
- Goodput terhadap beban yang ditawarkan, untuk memperkirakan titik jenuh kanal.
- Komposisi loss di 20 node berat: antrean sender vs udara vs terlambat.
- Keadilan antar node: Jain, PDR per node vs RSSI, dan apakah urutan node terburuk sama di tiap ulangan.
- Distribusi RTT per node di 20 node berat, dikaitkan dengan `tx_api_err`.
- Variasi antar ulangan, untuk menentukan berapa ulangan yang cukup di tes metode adaptif.
