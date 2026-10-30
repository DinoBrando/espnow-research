# tools

## split_log.py

Mengubah log Serial receiver (hasil `dump`) menjadi dua file CSV.

```
python tools/split_log.py log.txt           # CSV standar (koma)
python tools/split_log.py log.txt --excel   # Excel Indonesia (titik koma, desimal koma)
```

Keluaran ditulis di samping file input:

- `<nama>_per_node.csv`: baris D, satu baris per node per run
- `<nama>_ringkasan.csv`: baris S, satu baris per run

Skrip mengabaikan baris komentar `#`, baris kosong, dan pagar ```` ``` ````. Baris yang tersalin dua kali hanya disimpan sekali. Baris yang jumlah kolomnya tidak cocok dengan header dilewati dan dilaporkan.

Hanya butuh Python 3, tanpa library tambahan.
