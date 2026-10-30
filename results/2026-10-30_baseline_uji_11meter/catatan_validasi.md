# Sesi 2026-10-30: baseline uji coba, jarak 11 meter

Percobaan kedua. Sama dengan percobaan pertama (folder sesi 2 meter), kecuali jarak receiver ke node dinaikkan dari sekitar 2 m menjadi sekitar 11 m.

Perintah: `batch nodes=1,5,10,20 payload=32,250 interval=50 dur=30 reps=3`
Receiver: versi awal (echo dan ACK unicast). Channel 1, power save mati, unicast, probe RTT tiap paket ke-20.

## Validasi data

| Pemeriksaan | Hasil |
|---|---|
| Jumlah run | 24 (run_id 60476–60499, berurutan tanpa lompatan) |
| Tiap kombinasi node × payload | 3 run (rep 1, 2, 3) |
| Baris per node | 216 |
| Duplikat (run_id, node) / run_id | tidak ada |
| Jumlah kolom tiap baris | sesuai header |
| Total di baris S = jumlah baris D | cocok untuk semua run |
| Parameter tetap | sama di semua run |
| `tx_attempts = 600` untuk semua node yang laporannya sampai | ya |
| Header tambahan di akhir `log.txt` | ada (receiver sempat boot ulang setelah batch). Tidak memengaruhi data |

## Masalah perangkat (penting)

Ada **21 baris node** dengan `report_ok=0`, artinya node itu tidak mengirim penuh 30 detik atau laporannya tidak sampai:

| Run | Rep | Konfigurasi | Node | Kejadian |
|---|---|---|---|---|
| 60481 | 1 | 10 node berat | N07 | Berhenti setelah 4 paket (±0,2 dtk), lalu kembali normal di run berikutnya |
| 60482 | 1 | 20 node ringan | **N01** | Berhenti setelah 418 paket (±20,9 dtk) |
| 60483 | 1 | 20 node berat | **N01** | Tidak terdengar |
| 60483 | 1 | 20 node berat | N06 | Berhenti setelah 27 paket (±1,4 dtk), lalu kembali normal |
| 60483 | 1 | 20 node berat | N09 | Tidak terdengar, lalu kembali normal |
| 60484–60499 | 2, 3 | semua | **N01** | Tidak terdengar (16 run) |

Kesimpulan:

- **N01 mati di tengah run 60482 dan tidak kembali sampai sesi selesai.** Kemungkinan besar baterai habis.
- N06, N07, N09 sempat berhenti sesaat di run payload berat, lalu ikut lagi di run berikutnya. Kemungkinan reset karena tegangan baterai turun saat mengirim terus-menerus (brownout).

Akibatnya:

| Konfigurasi | Node aktif di rep 1 / 2 / 3 |
|---|---|
| 1 node (ringan dan berat) | 1 / **0** / **0**. Hanya 1 ulangan yang valid |
| 5 node | 5 / 4 / 4 |
| 10 node ringan | 10 / 9 / 9 |
| 10 node berat | 9 / 9 / 9 |
| 20 node ringan | 19 / 19 / 19 |
| 20 node berat | **17** / 19 / 19 |

Karena jumlah node aktif berbeda antar ulangan, beban yang ditawarkan juga berbeda. Ringkasan di folder ini hanya memakai baris node dengan `report_ok=1`, dan jumlah node aktif dicatat per run.

**Sesi ini tidak bisa dibandingkan satu lawan satu dengan sesi 2 meter** untuk konfigurasi 1 node (hanya 1 ulangan) dan 20 node berat (17–19 node, bukan 20). Disarankan mengulang sesi 11 meter setelah baterai N01 diisi dan baterai N06, N07, N09 diperiksa.

## Catatan pengukuran

- **14 paket dobel di tingkat radio** (kolom `reorder`). Ringkasan memakai paket unik (`rx − reorder`).
- **572 paket sudah di-ACK MAC tetapi tidak tercatat receiver**, semuanya di 20 node berat. Sama seperti sesi 2 meter: kemungkinan tiba setelah masa tenggang 0,5 detik.
- **RTT di run 20 node** sedikit terpengaruh penukaran peer di receiver versi ini.

## File

| File | Isi |
|---|---|
| `log.txt` | Salinan mentah hasil `dump` |
| `log_per_node.csv`, `log_ringkasan.csv` | Hasil `tools/split_log.py` (versi Excel: `_EXCEL.csv`) |
| `ringkasan_per_run.csv` | Satu baris per run, hanya node valid, dengan jumlah node aktif |
| `ringkasan_per_konfigurasi.csv` | Rata-rata ulangan yang valid per kombinasi node × payload |
| `20node_berat_per_node.csv` | PDR tiap node di 20 node berat, per ulangan, dengan RSSI |
| `perbandingan_2m_vs_11m.csv` | Ringkasan per konfigurasi sesi 2 m dan 11 m berdampingan |
| `cara_membaca.md` | Panduan membaca semua file dan kolom, rumus, keterbatasan, dan contoh kode |
