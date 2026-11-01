"""
Serial monitor lewat Command Prompt yang sekaligus menyimpan semua keluaran ke file.

Pasang sekali:
  pip install pyserial

Pakai:
  python tools/serial_log.py                  # tampilkan daftar port COM
  python tools/serial_log.py COM5             # 921600 baud, simpan ke log_<tanggal>_<jam>.txt
  python tools/serial_log.py COM5 115200      # baud lain (mis. untuk sender)
  python tools/serial_log.py COM5 --out results/sesi1/log.txt

Selama berjalan:
  - Semua yang dikirim board tampil di layar dan langsung ditulis ke file log.
  - Ketik perintah (mis. nodes, batch ..., dump) lalu Enter: dikirim ke board dengan akhiran newline.
  - Ketik :q lalu Enter (atau tekan Ctrl+C) untuk keluar.

Catatan:
  - Tutup Serial Monitor Arduino IDE dulu. Satu port COM hanya bisa dibuka satu program.
  - Skrip ini membuka port tanpa menyentuh DTR/RTS, supaya board tidak ter-reset saat dibuka.
  - File log bisa langsung diproses: python tools/split_log.py <file log>
"""
import argparse
import datetime
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial belum terpasang. Jalankan:  pip install pyserial")
    sys.exit(1)


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("Tidak ada port serial yang terdeteksi.")
        return
    print("Port yang terdeteksi:")
    for p in ports:
        print(f"  {p.device:<8} {p.description}")


def open_port(port, baud):
    ser = serial.serial_for_url(port, do_not_open=True)
    ser.baudrate = baud
    ser.timeout = 0.2
    # Jangan reset board: DTR/RTS dibiarkan tidak aktif sebelum port dibuka
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def reader(ser, logf, stop):
    buf = b""
    while not stop.is_set():
        try:
            chunk = ser.read(4096)
        except serial.SerialException as e:
            print(f"\n[port terputus: {e}]")
            stop.set()
            break
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.rstrip(b"\r").decode("utf-8", errors="replace")
            print(text, flush=True)
            logf.write(text + "\n")
            logf.flush()
    if buf:
        text = buf.rstrip(b"\r").decode("utf-8", errors="replace")
        print(text, flush=True)
        logf.write(text + "\n")
        logf.flush()


def main():
    ap = argparse.ArgumentParser(description="Serial monitor + pencatat log")
    ap.add_argument("port", nargs="?", help="mis. COM5 (Windows) atau /dev/ttyUSB0")
    ap.add_argument("baud", nargs="?", type=int, default=921600)
    ap.add_argument("--out", help="nama file log (default log_<tanggal>_<jam>.txt)")
    args = ap.parse_args()

    if not args.port:
        list_ports()
        print("\nContoh:  python tools/serial_log.py COM5")
        return

    out = args.out or datetime.datetime.now().strftime("log_%Y%m%d_%H%M%S.txt")
    try:
        ser = open_port(args.port, args.baud)
    except serial.SerialException as e:
        print(f"Gagal membuka {args.port}: {e}")
        print("Pastikan Serial Monitor Arduino IDE sudah ditutup dan nomor COM benar.")
        list_ports()
        sys.exit(1)

    stop = threading.Event()
    with open(out, "a", encoding="utf-8") as logf:
        print(f"[terhubung ke {args.port} @ {args.baud} | log: {out} | ketik :q untuk keluar]")
        t = threading.Thread(target=reader, args=(ser, logf, stop), daemon=True)
        t.start()
        try:
            while not stop.is_set():
                line = sys.stdin.readline()
                if line == "":          # stdin ditutup
                    time.sleep(0.5)
                    break
                cmd = line.rstrip("\r\n")
                if cmd == ":q":
                    break
                ser.write((cmd + "\n").encode("utf-8"))
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            t.join(timeout=1)
            ser.close()
    print(f"[selesai, log tersimpan di {out}]")


if __name__ == "__main__":
    main()
