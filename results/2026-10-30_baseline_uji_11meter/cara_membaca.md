# Cara membaca hasil baseline 2026-10-30, jarak 11 meter

Dokumen ini menjelaskan isi folder ini supaya orang lain (atau model AI) bisa menganalisis datanya tanpa konteks tambahan. **Baca `catatan_validasi.md` dulu**: di sesi ini ada node yang mati dan reset.

## 1. Konteks penelitian

Penelitian ini membandingkan enam metode adaptif untuk mengurangi kongesti pada jaringan ESP-NOW multi-node: Rate Control, Dynamic Interval, Dynamic Backoff, Tx Power Control, ACK dan Retransmisi, serta Channel Hopping. Folder ini berisi **baseline** (tanpa mekanisme adaptif) pada jarak receiver ke node sekitar **11 meter**. Sesi pembanding ada di folder sesi 2 meter (tanggal sama).

Status sesi: **uji coba, data tidak lengkap** karena masalah baterai (bagian 7).

## 2. Desain eksperimen

| Aspek | Nilai |
|---|---|
| Topologi | Bintang: 1 receiver, N sender, semua sender mengirim ke receiver |
| Jarak receiver ke node | ±11 m (sesi pembanding: ±2 m) |
| Receiver | ESP32-C6, terhubung ke laptop lewat USB |
| Sender | Board ESP32, daya baterai, ID 1–20 |
| Jumlah node yang diminta (N) | 1, 5, 10, 20. Run dengan N node berarti sender ber-ID 1..N diminta mengirim |
| Payload | 32 byte (ringan) dan 250 byte (berat), termasuk header aplikasi 14 byte |
| Interval kirim | 50 ms per node (20 paket per detik per node), periodik tanpa jitter |
| Durasi per run | 30 detik, jadi 600 percobaan kirim per node per run |
| Pengulangan | 3. Urutan: ulangan paling luar |
| Total | 24 run, 216 baris per node |
| Mode kirim | Unicast (`mode=0`), channel 1, power save mati, PHY rate default |
| Probe RTT | Tiap paket ke-20 (30 probe per node per run) |

Belum dicatat dan perlu dilengkapi tim: jenis board sender, jenis baterai, susunan node, ruangan, dan kondisi WiFi sekitar.

## 3. Daftar file

| File | Isi | Pakai untuk analisis? |
|---|---|---|
| `log.txt` | Salinan mentah keluaran `dump` receiver | Sumber asli |
| `log_per_node.csv` | 216 baris, satu per node per run, **termasuk node yang mati** | Ya, saring `report_ok == 1` |
| `log_ringkasan.csv` | 24 baris, ringkasan per run yang dihitung receiver. **Memasukkan node yang mati sebagian** | Hati-hati, lebih baik pakai `ringkasan_per_run.csv` |
| `ringkasan_per_run.csv` | 24 baris, dihitung ulang dari node valid saja | **Ya** |
| `ringkasan_per_konfigurasi.csv` | 8 baris, rata-rata ulangan valid per kombinasi | **Ya**, untuk tabel hasil |
| `20node_berat_per_node.csv` | 20 baris, PDR tiap node di 20 node berat | Ya, untuk analisis keadilan |
| `perbandingan_2m_vs_11m.csv` | 8 baris, sesi 2 m dan 11 m berdampingan, dihitung dengan metode yang sama | Ya, dengan memperhatikan jumlah node aktif |
| `log_*_EXCEL.csv` | Isi sama, format Excel Indonesia (`;` dan desimal `,`) | **Jangan**, duplikat |
| `catatan_validasi.md` | Pemeriksaan kelengkapan dan daftar node bermasalah | Baca dulu |

Semua CSV memakai pemisah koma, desimal titik, UTF-8, satu baris header.

## 4. Kolom `log_per_node.csv`

Kunci unik: (`run_id`, `node`). Format kolom sama persis dengan sesi 2 meter.

| Kolom | Arti |
|---|---|
| `run_id`, `rep`, `mode`, `nodes`, `payload`, `interval_ms`, `duration_s`, `probe_every`, `jitter_ms` | Konfigurasi run |
| `node` | ID node |
| `report_ok` | 1 = node mengirim penuh dan laporannya sampai. **0 = node mati, reset, atau laporannya hilang**; kolom sisi sender bernilai 0 dan `tx_attempts` ditaksir dari nomor urut terakhir + 1 |
| `tx_attempts` | `tx_sent + tx_api_err` (600 untuk node valid) |
| `tx_sent` | Diterima stack WiFi |
| `tx_api_err` | Ditolak `esp_now_send()` (antrean TX penuh), tidak pernah dipancarkan |
| `mac_ok`, `mac_fail` | Status kirim tingkat MAC: berhasil di-ACK / gagal setelah retry |
| `rx` | Paket diterima receiver, termasuk duplikat |
| `lost_gap` | Hilang menurut lompatan nomor urut, tidak termasuk ekor run |
| `reorder` | Paket dengan nomor urut ≤ yang terakhir; praktis = duplikat |
| `pdr_pct`, `pdr_sent_pct` | `rx / tx_attempts`, `rx / tx_sent` (× 100, maks 100, termasuk duplikat) |
| `goodput_bps` | Byte payload diterima × 8 / 30 |
| `jitter_us` | Estimator jitter RFC 3550, nilai di akhir run |
| `max_gap_ms` | Celah terbesar antar paket diterima |
| `rssi_avg`, `rssi_min` | RSSI paket diterima (dBm). 0 jika tidak ada paket |
| `probes`, `echoes` | Probe RTT dikirim / echo kembali |
| `rtt_min_us`, `rtt_mean_us`, `rtt_std_us`, `rtt_max_us` | RTT dalam mikrodetik |
| `slips` | Sender tertinggal dari jadwal (semua 0) |

## 5. Kolom file turunan

Semua file turunan **hanya memakai baris `report_ok == 1`**. Paket unik: `rx_unique = rx − reorder`.

### `ringkasan_per_run.csv`

| Kolom | Rumus / arti |
|---|---|
| `run_id`, `rep`, `nodes`, `payload` | Konfigurasi. `nodes` = jumlah node yang diminta |
| `active_nodes` | Jumlah node valid di run itu |
| `excluded_nodes` | Node yang dikeluarkan, mis. `N01;N06;N09` |
| `offered_kbps` | `active_nodes × 20 × payload × 8 / 1000` |
| `pdr` | `Σ rx_unique / Σ tx_attempts × 100` |
| `goodput_kbps` | `Σ rx_unique × payload × 8 / 30 / 1000` |
| `jain` | Indeks Jain dari PDR unik tiap node |
| `worst_node`, `worst_pdr` | Node dengan PDR unik terendah |
| `jitter_ms` | Rata-rata `jitter_us` antar node / 1000 |
| `rtt_ms` | Rata-rata `rtt_mean_us` dibobot `echoes` / 1000 |
| `echo_return_pct` | `Σ echoes / Σ probes × 100` |
| `rssi_avg` | Rata-rata `rssi_avg` dibobot `rx` |
| `tx_attempts`, `lost` | Percobaan kirim dan paket hilang (`tx_attempts − Σ rx_unique`) |
| `lost_tx_api_err` | Hilang di antrean sender |
| `lost_mac_fail` | Hilang di udara |
| `lost_late_unrecorded` | `Σ max(0, mac_ok − rx_unique)`: sudah di-ACK MAC tetapi tidak tercatat |
| `radio_duplicates` | `Σ reorder` |

Run tanpa node valid (1 node di rep 2 dan 3) hanya berisi kolom konfigurasi dan `active_nodes = 0`.

### `ringkasan_per_konfigurasi.csv`

Kunci: (`nodes`, `payload`). Rata-rata dan simpangan baku dihitung dari run di `ringkasan_per_run.csv` yang punya `active_nodes > 0`.

| Kolom | Arti |
|---|---|
| `reps_valid` | Jumlah ulangan dengan node valid |
| `active_nodes_per_rep` | Node aktif di rep 1;2;3, mis. `17;19;19` |
| `offered_kbps_mean` | Rata-rata beban yang ditawarkan |
| `pdr_mean`, `pdr_std`, `goodput_kbps_mean`, `goodput_kbps_std`, `jitter_ms_mean`, `jitter_ms_std`, `rtt_ms_mean`, `rtt_ms_std` | Rata-rata dan simpangan baku antar ulangan. Kosong jika hanya 1 ulangan |
| `jain_mean`, `worst_node_pdr_mean`, `echo_return_pct`, `rssi_avg` | Rata-rata antar ulangan |
| `*_total` | Jumlah dari semua ulangan valid |

### `20node_berat_per_node.csv`

| Kolom | Arti |
|---|---|
| `node` | ID node |
| `rssi_avg` | Rata-rata RSSI di ulangan yang valid |
| `pdr_rep1`, `pdr_rep2`, `pdr_rep3` | PDR unik per ulangan. **Kosong jika node tidak valid di ulangan itu** |
| `reps_valid` | Jumlah ulangan valid |
| `pdr_mean`, `tx_api_err_mean` | Rata-rata ulangan yang valid |

### `perbandingan_2m_vs_11m.csv`

Satu baris per kombinasi. Setiap metrik muncul dua kali dengan akhiran `_2m` dan `_11m`: `active_nodes_per_rep`, `offered_kbps_mean`, `pdr_mean`, `goodput_kbps_mean`, `jain_mean`, `worst_node_pdr_mean`, `jitter_ms_mean`, `rtt_ms_mean`, `rssi_avg`. Kedua sesi dihitung dengan metode yang sama (hanya node valid). Di sesi 2 m semua node valid.

## 6. Hasil utama

| Node | Beban | Node aktif (rep 1;2;3) | Ditawarkan (kbps) | PDR (%) | Goodput (kbps) | Jain | Jitter (ms) | RTT (ms) |
|---|---|---|---|---|---|---|---|---|
| 1 | ringan | 1;0;0 | 5,1 | 100 | 5,1 | 1,000 | 0,59 | 3,5 |
| 1 | berat | 1;0;0 | 40,0 | 100 | 40,0 | 1,000 | 0,39 | 4,3 |
| 5 | ringan | 5;4;4 | 22,2 | 100 | 22,2 | 1,000 | 0,21 | 2,9 |
| 5 | berat | 5;4;4 | 173,3 | 100 | 173,3 | 1,000 | 0,65 | 8,1 |
| 10 | ringan | 10;9;9 | 47,8 | 100 | 47,8 | 1,000 | 0,25 | 3,7 |
| 10 | berat | 9;9;9 | 360,0 | 100 | 360,0 | 1,000 | 1,23 | 11,9 |
| 20 | ringan | 19;19;19 | 97,3 | 100 | 97,3 | 1,000 | 0,85 | 8,6 |
| 20 | berat | 17;19;19 | 733,3 | 74,25 ± 6,53 | 542,5 | 0,899 | 70,64 | 1172,0 |

Per ulangan untuk 20 node berat (beban berbeda karena jumlah node aktif berbeda):

| Rep | Node aktif | Ditawarkan (kbps) | PDR (%) | Goodput (kbps) | Jain |
|---|---|---|---|---|---|
| 1 | 17 | 680 | 81,69 | 555,5 | 0,927 |
| 2 | 19 | 760 | 71,58 | 544,0 | 0,889 |
| 3 | 19 | 760 | 69,48 | 528,1 | 0,880 |

Hal yang sudah diketahui:

- Seperti di 2 m, loss hanya terjadi di 20 node berat. Semua kombinasi lain PDR 100%.
- Di 20 node berat, 90% loss terjadi di antrean sender (`tx_api_err` 7.726 dari 8.587 paket hilang), loss di udara hanya 120, dan 572 terlambat.
- Unfairness tetap kuat: korelasi RSSI rata-rata vs PDR rata-rata per node = 0,92.
- **Goodput 20 node berat lebih rendah daripada di 2 m**: 528–544 kbps dengan 19 node (760 kbps ditawarkan), sedangkan sesi 2 m mencapai 593–596 kbps dengan 20 node (800 kbps ditawarkan). Kapasitas efektif turun sekitar 9–11%.
- **RSSI hampir tidak turun**: rata-rata per node −60,5 dBm di 2 m dan −62,2 dBm di 11 m. Yang berubah adalah sebarannya: di 2 m −73 sampai −45 dBm, di 11 m −68 sampai −54 dBm. Node yang sangat kuat di 2 m (N06 −47 dBm dan N11 −45 dBm) turun ke −65 dan −62 dBm, sehingga sinyal antar node jauh lebih setara.
- Dugaan (belum dibuktikan): di 2 m, sejumlah node bersinyal kuat tetap lolos saat tabrakan (capture effect), sehingga sebagian tabrakan masih menghasilkan paket yang diterima. Di 11 m sinyal antar node lebih setara, jadi lebih banyak tabrakan yang menghilangkan kedua paket, dan goodput total turun.

## 7. Keterbatasan

1. **Data tidak lengkap karena node mati dan reset** (lihat `catatan_validasi.md`). 1 node hanya punya 1 ulangan; konfigurasi lain punya node aktif yang berbeda antar ulangan dan lebih sedikit daripada sesi 2 m. Perbandingan antar sesi harus memperhatikan `active_nodes_per_rep` dan `offered_kbps_mean`.
2. **`log_ringkasan.csv` (baris S dari receiver) memasukkan node yang mati sebagian.** Misalnya run 60482 memasukkan 418 paket dari N01. Untuk analisis, pakai `ringkasan_per_run.csv`.
3. **Paket terlambat dihitung hilang.** 572 paket di 20 node berat sudah di-ACK MAC tetapi tidak tercatat, kemungkinan tiba setelah masa tenggang 0,5 detik.
4. **Status kirim sebagian paket tidak tercatat** karena sender berhenti menghitung saat mengirim laporan pertama.
5. **RTT di run 20 node** sedikit terpengaruh penukaran peer di receiver.
6. Kolom mentah `pdr_pct` dan `goodput_bps` memasukkan duplikat. Pakai file turunan atau `rx − reorder`.
7. `lost_gap` tidak menangkap loss di ekor run.
8. `jitter_us` adalah nilai akhir estimator, bukan rata-rata.
9. Jarak dicatat kira-kira. Posisi dan orientasi node juga berubah antar sesi, jadi perbedaan 2 m vs 11 m tidak hanya disebabkan jarak.

## 8. Contoh memuat data (Python)

```python
import pandas as pd

d = pd.read_csv("log_per_node.csv")
v = d[d["report_ok"] == 1].copy()          # buang node mati / reset
v["rx_unique"] = v["rx"] - v["reorder"]

per_run = v.groupby(["run_id", "rep", "nodes", "payload"]).agg(
    active_nodes=("node", "count"),
    attempts=("tx_attempts", "sum"),
    rx_unique=("rx_unique", "sum"),
).reset_index()
per_run["pdr"] = 100 * per_run["rx_unique"] / per_run["attempts"]
per_run["goodput_kbps"] = per_run["rx_unique"] * per_run["payload"] * 8 / 30 / 1000
print(per_run[per_run.nodes == 20])
```

## 9. Analisis yang disarankan

- Bandingkan 2 m vs 11 m pada 20 node berat dengan memperhitungkan beban yang ditawarkan, misalnya goodput / offered.
- Distribusi RSSI per node di kedua sesi, dan hubungannya dengan PDR per node.
- Apakah node yang paling dirugikan sama di kedua sesi (posisi) atau berubah (sinyal relatif).
- Kapan node mulai bermasalah dalam sesi, untuk memperkirakan daya tahan baterai saat beban berat.
