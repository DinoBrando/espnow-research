# results

Simpan hasil tiap sesi tes di subfolder sendiri:

```
results/
  2026-10-30_baseline_ch1/
    log.txt              # salinan mentah hasil `dump`
    log_per_node.csv     # dari tools/split_log.py
    log_ringkasan.csv
    catatan.md           # kondisi tes (lihat README utama)
    susunan.jpg          # foto penempatan node
```

Selalu sertakan `log.txt` mentah, supaya CSV bisa dibuat ulang kalau skrip berubah.
