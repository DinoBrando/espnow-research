# receiver_v2

Firmware ESP32-C6: pengontrol eksperimen, pencatat metrik, dan pengirim STATUS. Log disimpan di flash (`/exp_log.csv`, terpisah dari log v1).

Board: ESP32C6 Dev Module, core esp32 3.3.12. Serial Monitor: 921600, line ending New Line.

## Perintah

| Perintah | Fungsi |
|---|---|
| `help` | daftar perintah |
| `status` | keadaan receiver, ukuran log, parameter, daftar node |
| `nodes` | node yang terdengar: umur, versi firmware, alasan reset terakhir, INA219, kanal, jumlah reset |
| `params` | parameter metode saat ini |
| `set key=value ...` | ubah parameter metode (berlaku sampai receiver restart) |
| `run N PAYLOAD INTERVAL_MS [DUR_S] [method=M] [probe=P] [mode=0\|1] [jitter=J] [key=value]` | satu run (default 60 dtk, metode 0) |
| `batch [kunci=nilai ...]` | matriks otomatis |
| `stop` | batalkan run atau batch |
| `dump` / `clearlog` / `header` | cetak log / hapus log / cetak header kolom |

### Kunci `batch`

| Kunci | Default | Keterangan |
|---|---|---|
| `nodes` | `1,5,10,20` | daftar |
| `payload` | `32,250` | daftar |
| `interval` | `50` | daftar (ms) |
| `method` | `0` | daftar, mis. `method=0,1` untuk berselang-seling |
| `mode` | `0` | 0 unicast, 1 broadcast |
| `dur` | `60` | detik |
| `reps` | `3` | |
| `probe` | `20` | probe RTT tiap paket ke-n |
| `jitter` | `0` | jeda acak tambahan per sampel (ms) |

Urutan: ulangan paling luar, lalu nodes, payload, interval, mode, method.

### Parameter metode (`set` atau ditulis di `run`/`batch`)

| Kunci | Default | Metode | Arti |
|---|---|---|---|
| `rc_down` | 90 | 1 | turun kecepatan jika PDR link < nilai ini (%) atau ≥ 2 gagal MAC dalam 1 dtk |
| `rc_up` | 3 | 1 | naik setelah N dtk berturut-turut tanpa gagal. Jika percobaan naik gagal, jeda ini berlipat (maks 4×) |
| `di_step` | 10 | 2 | pengurangan interval (ms) setelah 3 dtk lancar |
| `di_max` | 8 | 2 | interval maks = nominal × nilai ini |
| `bo_cw0` | 10 | 3 | CW awal (ms) |
| `bo_cwmax` | 320 | 3 | CW maks (ms) |
| `tp_lo` | −70 | 4 | naikkan daya jika RSSI < nilai ini |
| `tp_hi` | −60 | 4 | turunkan daya jika RSSI > nilai ini |
| `tp_step` | 2 | 4 | langkah (dBm) |
| `ack_retries` | 2 | 5 | maks kirim ulang per paket |
| `hop_fail` | 10 | 6 | pindah jika gagal MAC ≥ nilai ini (%) |
| `hop_rx` | 10 | 6 | atau jika paket diterima < nilai ini (% dari harapan) |
| `hop_win` | 3 | 6 | jumlah jendela 1 dtk berturut-turut |
| `keepalive` | 3000 | 6 | sender mencari receiver jika tak terdengar selama ini (ms) |

Setiap `run` dan `batch` mencatat baris `#PARAMS,...` di log.

## Format log

- `#HD,...` header baris D, `#HS,...` header baris S, `#PARAMS,...` parameter, `# RESET ...` node reset, `#...` lainnya komentar.
- `D,...` satu baris per node per run. `S,...` satu baris per run.

### Kolom baris D

| Kolom | Arti |
|---|---|
| `fw_rx`, `fw_tx` | versi firmware receiver dan sender (0.0 jika laporan tidak sampai) |
| `method` | 0–6 |
| `run_id` … `jitter_ms` | konfigurasi run (sama dengan v1) |
| `node` | ID node |
| `report_ok` | 1 = laporan sender sampai |
| `boot_changes` | berapa kali node terdeteksi reset selama run |
| `reset_reason` | alasan reset terakhir node (POWERON, BROWNOUT, …) |
| `generated` | sampel dibangkitkan |
| `skipped` | sampel ditimpa sampel lebih baru sebelum dikirim |
| `tx_new` | paket baru yang diterima stack |
| `tx_retx` | kiriman ulang (metode 5) |
| `tx_api_err` | paket baru ditolak `esp_now_send` (antrean penuh) |
| `tx_retx_api_err` | kiriman ulang yang ditolak |
| `mac_ok`, `mac_fail` | hasil callback kirim (paket baru + ulang) |
| `mac_pending` | kiriman tanpa callback saat laporan dibuat |
| `rx_unique` | paket unik diterima (dobel tidak dihitung) |
| `rx_dup` | paket dobel |
| `rx_late` | paket unik yang tiba setelah run selesai (dalam masa tenggang 5 dtk) |
| `rx_via_retx` | paket unik yang diterima dari kiriman ulang |
| `lost` | `tx_new − rx_unique` |
| `pdr_gen_pct` | `rx_unique / generated × 100` (PDR terhadap data yang dibangkitkan) |
| `pdr_sent_pct` | `rx_unique / tx_new × 100` (PDR terhadap paket yang dipancarkan) |
| `goodput_bps` | `rx_unique × payload × 8 / durasi` |
| `jitter_us`, `max_gap_ms` | jitter (RFC 3550, nilai akhir) dan celah terbesar, hanya paket berurutan |
| `rssi_avg`, `rssi_min` | RSSI paket diterima |
| `probes`, `echoes`, `rtt_*_us` | probe RTT dan hasilnya |
| `age_mean_us`, `age_max_us` | umur sampel saat pertama dikirim (antre di aplikasi) |
| `slips` | sender tertinggal dari jadwal pembangkitan |
| `rate_final_mbps`, `rate_mean_mbps`, `rate_changes` | kecepatan radio akhir, rata-rata per detik, jumlah perubahan |
| `txp_final_dbm`, `txp_mean_dbm`, `txp_min_dbm` | daya pancar |
| `intv_final_ms`, `intv_mean_ms` | interval kirim (metode 2) |
| `cw_final_ms`, `backoff_mean_ms` | CW akhir dan rata-rata jeda (metode 3) |
| `ack_confirmed`, `ack_giveup` | paket terkonfirmasi / tidak terkonfirmasi (metode 5) |
| `channel_final`, `hops`, `scans` | kanal akhir sender, jumlah pindah kanal, jumlah langkah pencarian receiver |
| `ina_ok`, `energy_mj`, `i_avg_ma`, `v_avg_v`, `ina_samples` | energi selama run (N01 dan N02) |

### Kolom baris S

`fw_rx, method, run_id, rep, mode, nodes, payload, interval_ms, duration_s, probe_every, jitter_ms, reports, generated, tx_new, rx_unique, rx_late, pdr_gen_pct, pdr_sent_pct, offered_bps (generated × payload × 8 / durasi), goodput_bps, jain_pdr (dari pdr_gen per node), worst_node, worst_pdr_pct, jitter_us_mean, rtt_mean_us, mac_fail, rssi_avg, rx_hops (pindah kanal oleh receiver), rx_channel_final, boot_changes`
