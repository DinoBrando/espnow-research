# sender_v2

Satu firmware untuk ke-20 node. Metode dan parameter datang dari receiver lewat paket START.

Board: ESP32 dengan USB CH340, core esp32 3.3.12. Serial Monitor: 115200, line ending New Line.

## Perintah Serial

| Perintah | Fungsi |
|---|---|
| `id N` | simpan ID node (1–32). Tersimpan di flash, tidak hilang saat upload ulang |
| `status` | versi, ID, keadaan, MAC, alasan reset terakhir, INA219, daya pancar maksimal |
| `ina` | baca INA219 sekali (tegangan, arus, daya) |

## INA219 (hanya N01 dan N02)

Susunan daya node: baterai → step-up → VIN dan GND ESP32. INA219 disisipkan **antara keluaran step-up dan pin VIN**:

```
  Baterai ──► Step-up IN+ / IN−
              Step-up OUT+ ──► VIN+ ┐
                                    INA219 ── VCC ◄── 3V3 ESP32
              ESP32 VIN    ◄── VIN- ┘        GND ◄── GND ESP32
                                             SDA ◄── GPIO21
                                             SCL ◄── GPIO22
              Step-up OUT− ─────────────────────► GND ESP32
```

- Hanya kabel (+) keluaran step-up yang lewat INA219. Kabel (−) langsung ke GND ESP32.
- Yang terukur adalah energi yang dipakai board ESP32 (sekitar 5 V), tanpa rugi-rugi step-up. Ini paling bersih untuk membandingkan metode.
- `v_avg_v` di CSV menunjukkan tegangan keluaran step-up. Kalau turun jauh dari 5 V selama sesi, baterai mulai habis.
- Alamat bawaan 0x40 (jumper tidak perlu diubah). Shunt 0,1 Ω (`INA_SHUNT_OHM`).
- Cabut USB saat mengukur.
- Node tanpa INA219 otomatis melewati pengukuran (`ina_ok`=0).
- Pembacaan tiap 100 ms; INA219 merata-rata 128 sampel sendiri, jadi beban CPU kecil.

## Perilaku

| Keadaan | Yang dilakukan |
|---|---|
| IDLE | HELLO ke receiver tiap 1,5–2,5 dtk berisi versi, ID boot, alasan reset, status INA219 |
| WAIT_START | menunggu waktu mulai bersama |
| RUNNING | membangkitkan sampel tiap interval, mengirim sesuai metode |
| DRAIN | berhenti mengirim, menunggu semua status kirim MAC masuk (maks 3 dtk) |
| REPORTING | mengirim laporan sampai di-ACK (maks 8 kali), lalu kembali ke kanal 1 |

Node dengan ID lebih besar dari jumlah node run itu diam total selama run.

## Konstanta di kode

| Konstanta | Default | Arti |
|---|---|---|
| `FORCE_NODE_ID` | 0 | selain 0: paksa ID ini |
| `HOME_CHANNEL` | 1 | harus sama dengan receiver |
| `INA_SDA`, `INA_SCL` | 21, 22 | pin I2C |
| `INA_SHUNT_OHM` | 0.1 | nilai shunt modul |
| `WIFI_POWER_SAVE` | 0 | power save WiFi mati |
