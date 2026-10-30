"""
Pisahkan log Serial receiver baseline ESP-NOW menjadi dua CSV:
  <nama>_per_node.csv   (baris D: satu baris per node per run)
  <nama>_ringkasan.csv  (baris S: satu baris per run)

Cara pakai (Windows, Python 3, tanpa library tambahan):
  1. Di receiver ketik `dump`, salin semua teks Serial Monitor (Ctrl+A, Ctrl+C),
     tempel ke Notepad, simpan sebagai mis. log.txt
  2. python split_log.py log.txt
     python split_log.py log.txt --excel   (untuk Excel berbahasa Indonesia:
                                            pemisah ';' dan desimal ',')

Baris komentar (#...), baris kosong, dan pagar ``` diabaikan.
Baris D/S yang jumlah kolomnya tidak cocok dengan header dilewati dan dilaporkan.
Run yang sama (run_id + node) yang muncul dua kali hanya disimpan sekali.
"""
import csv
import sys
from pathlib import Path


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    excel = "--excel" in sys.argv
    if not args:
        print(__doc__)
        sys.exit(1)

    src = Path(args[0])
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()

    hdr = {"D": None, "S": None}
    rows = {"D": [], "S": []}
    seen = set()
    bad = 0

    for raw in lines:
        line = raw.strip().strip("`").strip()
        if line.startswith("#HD,") or line.startswith("#HS,"):
            kind = line[2]
            cols = line.split(",")[2:]          # buang '#HD' dan 'tag'
            hdr[kind] = cols
            continue
        if not line or line.startswith("#"):
            continue
        if line[:2] not in ("D,", "S,"):
            continue
        kind = line[0]
        vals = line.split(",")[1:]              # buang tag D/S
        if hdr[kind] is None or len(vals) != len(hdr[kind]):
            bad += 1
            continue
        cols = hdr[kind]
        key = (kind, vals[cols.index("run_id")],
               vals[cols.index("node")] if kind == "D" else "")   # run_id (+ node)
        if key in seen:
            continue
        seen.add(key)
        rows[kind].append(vals)

    names = {"D": "per_node", "S": "ringkasan"}
    for kind in ("D", "S"):
        if not rows[kind]:
            print(f"tidak ada baris {kind}")
            continue
        out = src.with_name(f"{src.stem}_{names[kind]}.csv")
        delim = ";" if excel else ","
        with out.open("w", newline="", encoding="utf-8-sig" if excel else "utf-8") as f:
            w = csv.writer(f, delimiter=delim)
            w.writerow(hdr[kind])
            for r in rows[kind]:
                w.writerow([v.replace(".", ",") for v in r] if excel else r)
        print(f"{out.name}: {len(rows[kind])} baris")

    if bad:
        print(f"peringatan: {bad} baris dilewati (header belum ada / jumlah kolom tidak cocok)")


if __name__ == "__main__":
    main()
