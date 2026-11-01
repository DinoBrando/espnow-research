# tools

## serial_log.py

Serial monitor lewat Command Prompt yang sekaligus menyimpan semua keluaran ke file. Pengganti Serial Monitor Arduino IDE saat menjalankan batch: tidak perlu Ctrl+A/Ctrl+C, dan keluaran panjang tidak terpotong.

Pasang sekali: `pip install pyserial`

```
python tools/serial_log.py                    # daftar port COM
python tools/serial_log.py COM5               # receiver, 921600 baud
python tools/serial_log.py COM7 115200        # sender (untuk set ID)
python tools/serial_log.py COM5 --out results/2026-10-31_baseline/log.txt
```

- Keluaran board tampil di layar dan langsung ditulis ke file (default `log_<tanggal>_<jam>.txt`). File dibuka dalam mode tambah, jadi tidak menimpa isi lama.
- Ketik perintah (`nodes`, `batch ...`, `dump`, dll) lalu Enter untuk mengirimnya ke board.
- Ketik `:q` atau tekan Ctrl+C untuk keluar.
- Tutup Serial Monitor Arduino IDE dulu. Satu port hanya bisa dibuka satu program.
- Port dibuka tanpa mengaktifkan DTR/RTS, supaya board tidak ter-reset.

File log hasilnya bisa langsung diproses dengan `split_log.py`. Tetap disarankan menjalankan `dump` di akhir sesi dan memproses bagian itu, karena isi flash tidak tercampur pesan error ESP-IDF.

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
