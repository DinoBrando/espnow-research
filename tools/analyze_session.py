"""
Ringkasan satu sesi baseline dari log_per_node.csv.
Hanya baris dengan report_ok=1 (node mengirim penuh 30 detik dan laporannya sampai) yang dipakai.

python analyze_session.py <folder_sesi> [--compare <folder_sesi_lain> <label_ini> <label_lain>]
"""
import csv
import statistics as st
import collections
import sys
from pathlib import Path


def load(folder):
    d = list(csv.DictReader(open(Path(folder) / "log_per_node.csv")))
    for r in d:
        for k in r:
            if k not in ("pdr_pct", "pdr_sent_pct", "jitter_us", "max_gap_ms", "rssi_avg"):
                r[k] = int(r[k])
            else:
                r[k] = float(r[k])
        r["rx_unique"] = r["rx"] - r["reorder"]
        r["pdr_unique"] = 100 * r["rx_unique"] / r["tx_attempts"] if r["tx_attempts"] else 0.0
        r["late"] = max(0, r["mac_ok"] - r["rx_unique"])
    return d


def per_run(d):
    runs = collections.OrderedDict()
    for r in d:
        runs.setdefault(r["run_id"], []).append(r)
    out = []
    for rid, rows in runs.items():
        v = [r for r in rows if r["report_ok"] == 1]
        n, p = rows[0]["nodes"], rows[0]["payload"]
        rec = dict(run_id=rid, rep=rows[0]["rep"], nodes=n, payload=p,
                   active_nodes=len(v), excluded_nodes=";".join(f"N{r['node']:02d}" for r in rows if r["report_ok"] != 1))
        if v:
            att = sum(r["tx_attempts"] for r in v)
            rxu = sum(r["rx_unique"] for r in v)
            x = [r["pdr_unique"] for r in v]
            echoes = sum(r["echoes"] for r in v)
            rec.update(
                offered_kbps=round(len(v) * 20 * p * 8 / 1000, 1),
                pdr=round(100 * rxu / att, 2),
                goodput_kbps=round(rxu * p * 8 / 30 / 1000, 1),
                jain=round(sum(x) ** 2 / (len(x) * sum(i * i for i in x)), 3) if sum(x) else 0.0,
                worst_node=min(v, key=lambda r: r["pdr_unique"])["node"],
                worst_pdr=round(min(x), 2),
                jitter_ms=round(st.mean(r["jitter_us"] for r in v) / 1000, 2),
                rtt_ms=round(sum(r["rtt_mean_us"] * r["echoes"] for r in v) / echoes / 1000, 1) if echoes else "",
                echo_return_pct=round(100 * echoes / sum(r["probes"] for r in v), 1),
                rssi_avg=round(sum(r["rssi_avg"] * r["rx"] for r in v) / max(1, sum(r["rx"] for r in v)), 1),
                tx_attempts=att, lost=att - rxu,
                lost_tx_api_err=sum(r["tx_api_err"] for r in v),
                lost_mac_fail=sum(r["mac_fail"] for r in v),
                lost_late_unrecorded=sum(r["late"] for r in v),
                radio_duplicates=sum(r["reorder"] for r in v))
        out.append(rec)
    return out


def per_config(runs):
    cfg = collections.OrderedDict()
    for r in sorted(runs, key=lambda r: (r["nodes"], r["payload"], r["rep"])):
        cfg.setdefault((r["nodes"], r["payload"]), []).append(r)
    out = []
    for (n, p), rs in cfg.items():
        v = [r for r in rs if r["active_nodes"] > 0]
        ms = lambda k: (round(st.mean(r[k] for r in v), 2), round(st.stdev(r[k] for r in v), 2) if len(v) > 1 else "")
        rec = dict(nodes=n, payload=p, beban="ringan" if p == 32 else "berat",
                   reps_valid=len(v), active_nodes_per_rep=";".join(str(r["active_nodes"]) for r in rs))
        if v:
            rec.update(offered_kbps_mean=ms("offered_kbps")[0])
            for k, name in [("pdr", "pdr"), ("goodput_kbps", "goodput_kbps"), ("jitter_ms", "jitter_ms")]:
                rec[name + "_mean"], rec[name + "_std"] = ms(k)
            rec["jain_mean"] = ms("jain")[0]
            rec["worst_node_pdr_mean"] = ms("worst_pdr")[0]
            rtt = [r["rtt_ms"] for r in v if r["rtt_ms"] != ""]
            rec["rtt_ms_mean"] = round(st.mean(rtt), 1) if rtt else ""
            rec["rtt_ms_std"] = round(st.stdev(rtt), 1) if len(rtt) > 1 else ""
            rec["echo_return_pct"] = ms("echo_return_pct")[0]
            rec["rssi_avg"] = ms("rssi_avg")[0]
            for k in ["tx_attempts", "lost", "lost_tx_api_err", "lost_mac_fail", "lost_late_unrecorded", "radio_duplicates"]:
                rec[k + "_total"] = sum(r[k] for r in v)
        out.append(rec)
    return out


def write(path, rows):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)


def heavy20_per_node(d):
    H = [r for r in d if r["nodes"] == 20 and r["payload"] == 250]
    reps = sorted({r["rep"] for r in H})
    out = []
    for nd in range(1, 21):
        rows = {r["rep"]: r for r in H if r["node"] == nd}
        v = [rows[k] for k in reps if k in rows and rows[k]["report_ok"] == 1]
        rec = dict(node=nd)
        rec["rssi_avg"] = round(st.mean(r["rssi_avg"] for r in v), 1) if v else ""
        for k in reps:
            r = rows.get(k)
            rec[f"pdr_rep{k}"] = round(r["pdr_unique"], 2) if r and r["report_ok"] == 1 else ""
        rec["reps_valid"] = len(v)
        rec["pdr_mean"] = round(st.mean(r["pdr_unique"] for r in v), 2) if v else ""
        rec["tx_api_err_mean"] = round(st.mean(r["tx_api_err"] for r in v), 1) if v else ""
        out.append(rec)
    return out


def main():
    folder = Path(sys.argv[1])
    d = load(folder)
    runs = per_run(d)
    cfg = per_config(runs)
    write(folder / "ringkasan_per_run.csv", runs)
    write(folder / "ringkasan_per_konfigurasi.csv", cfg)
    pn = heavy20_per_node(d)
    write(folder / "20node_berat_per_node.csv", pn)
    for r in cfg:
        print({k: r.get(k) for k in ["nodes", "beban", "reps_valid", "active_nodes_per_rep", "offered_kbps_mean", "pdr_mean", "pdr_std",
                                     "goodput_kbps_mean", "goodput_kbps_std", "jain_mean", "worst_node_pdr_mean", "jitter_ms_mean",
                                     "rtt_ms_mean", "lost_tx_api_err_total", "lost_mac_fail_total", "lost_late_unrecorded_total",
                                     "radio_duplicates_total", "rssi_avg"]})
    v = [r for r in pn if r["pdr_mean"] != ""]
    if len(v) > 2:
        print("korelasi RSSI vs PDR 20 node berat:", round(st.correlation([r["rssi_avg"] for r in v], [r["pdr_mean"] for r in v]), 3))
    if "--compare" in sys.argv:
        i = sys.argv.index("--compare")
        other, la, lb = Path(sys.argv[i + 1]), sys.argv[i + 2], sys.argv[i + 3]
        cfg_b = {(r["nodes"], r["payload"]): r for r in per_config(per_run(load(other)))}
        comp = []
        for r in cfg:
            b = cfg_b.get((r["nodes"], r["payload"]), {})
            rec = dict(nodes=r["nodes"], payload=r["payload"], beban=r["beban"])
            for k in ["active_nodes_per_rep", "offered_kbps_mean", "pdr_mean", "goodput_kbps_mean", "jain_mean",
                      "worst_node_pdr_mean", "jitter_ms_mean", "rtt_ms_mean", "rssi_avg"]:
                rec[f"{k}_{lb}"] = b.get(k, "")
                rec[f"{k}_{la}"] = r.get(k, "")
            comp.append(rec)
        write(folder / f"perbandingan_{lb}_vs_{la}.csv", comp)
        print("perbandingan ditulis")


if __name__ == "__main__":
    main()
