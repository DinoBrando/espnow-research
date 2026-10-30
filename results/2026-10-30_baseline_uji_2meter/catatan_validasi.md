# Sesi 2026-10-30: baseline uji coba

Perintah: `batch nodes=1,5,10,20 payload=32,250 interval=50 dur=30 reps=3`
Receiver: versi awal (echo dan ACK unicast, muncul pesan `ESPNOW: Peer is full` di run 20 node).
Channel 1, power save mati, unicast, probe RTT tiap paket ke-20.

## Validasi data

| Pemeriksaan | Hasil |
|---|---|
| Jumlah run | 24 (run_id 1720–1743, berurutan tanpa lompatan) |
| Tiap kombinasi node × payload | 3 ulangan (rep 1, 2, 3) |
| Baris per node | 216 = 3 × 2 × (1 + 5 + 10 + 20) |
| Duplikat (run_id, node) di baris D | tidak ada |
| Duplikat run_id di baris S | tidak ada |
| Jumlah kolom tiap baris | sesuai header |
| Laporan sender | 216 dari 216 sampai (`report_ok=1` semua) |
| Total di baris S = jumlah baris D | cocok untuk semua run |
| `tx_attempts = tx_sent + tx_api_err` | cocok untuk semua baris |
| Parameter tetap (mode, interval, durasi, probe, jitter) | sama di semua run |
| RSSI tiap node sepanjang sesi | berubah maks 3,8 dB, jadi node tidak berpindah |

## Catatan pengukuran

- **4 paket dobel di tingkat radio** (kolom `reorder`): run 1727 N06, 1732 N08, 1743 N09, 1743 N19. Paket diterima dua kali karena ACK MAC hilang lalu sender mengirim ulang. Di `ringkasan_per_konfigurasi.csv`, PDR dan goodput dihitung dari paket unik (`rx - reorder`).
- **589 paket sudah di-ACK di tingkat MAC tetapi tidak tercatat receiver**, semuanya di run 20 node payload berat (1727, 1735, 1743). Contoh: run 1735 N01 punya `lost_gap=0` tetapi kurang 15 paket, artinya semua paket yang hilang ada di ekor run. Penyebab paling mungkin: paket masih mengantre di sender saat run berakhir dan baru tiba setelah masa tenggang 0,5 detik. Ini kelemahan pengukuran (sekitar 6% dari loss di konfigurasi itu), bukan kondisi jaringan.
- **RTT di run 20 node** sedikit terpengaruh penukaran peer di receiver versi ini.

## File

| File | Isi |
|---|---|
| `log.txt` | Salinan mentah hasil `dump` |
| `log_per_node.csv`, `log_ringkasan.csv` | Hasil `tools/split_log.py` (versi Excel: `_EXCEL.csv`) |
| `ringkasan_per_konfigurasi.csv` | Rata-rata dan simpangan baku dari 3 ulangan per kombinasi, plus rincian sumber loss |
| `20node_berat_per_node.csv` | PDR tiap node di 20 node payload berat, per ulangan, dengan RSSI |
| `cara_membaca.md` | Panduan membaca semua file dan kolom, rumus, keterbatasan, dan contoh kode analisis |
