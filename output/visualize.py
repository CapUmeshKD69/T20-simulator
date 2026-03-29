"""
T20 Simulator — Visualization Suite
Generates all charts from the simulator CSV outputs.

Usage:
    python visualize.py

Outputs (saved as PNG in output/ directory):
    1. gantt_timeline.png        — Thread activity Gantt chart
    2. wait_time_comparison.png  — SJF vs FCFS wait time per batsman
    3. avg_wait_bar.png          — Average wait time SJF vs FCFS
    4. role_activity_pie.png     — Pie chart of actions by role
    5. ball_timeline.png         — Ball-by-ball event scatter plot
"""

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# Resolve paths relative to this script's directory
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR   = os.path.normpath(os.path.join(SCRIPT_DIR, ".."))
OUT_DIR    = SCRIPT_DIR  # charts saved next to this script


def out_path(filename):
    """Return full path for an output file in the output/ folder."""
    return os.path.join(OUT_DIR, filename)


def csv_path(filename):
    """Return full path for a CSV in the project root."""
    return os.path.join(ROOT_DIR, filename)


# ─────────────────────────────────────────────────
#  Utility
# ─────────────────────────────────────────────────
def load_csv(path):
    if not os.path.isfile(path):
        print(f"  [SKIP] {path} not found.")
        return None
    df = pd.read_csv(path)
    if df.empty:
        print(f"  [SKIP] {path} is empty.")
        return None
    return df


# ─────────────────────────────────────────────────
#  1. Gantt Timeline
# ─────────────────────────────────────────────────
def chart_gantt_timeline(gantt_df):
    print("  [1/5] Generating gantt_timeline.png ...")

    color_map = {
        "Bowler":  "#e74c3c",
        "Fielder": "#3498db",
    }
    action_colors = {
        "Bowled":  "#e74c3c",
        "Fielded": "#3498db",
        "Catch":   "#2ecc71",
    }

    threads = sorted(gantt_df["ThreadID"].unique())
    y_map = {tid: idx for idx, tid in enumerate(threads)}

    fig, ax = plt.subplots(figsize=(16, max(4, len(threads) * 0.5)))

    bar_width = 0.6
    for _, row in gantt_df.iterrows():
        tid = row["ThreadID"]
        action = str(row["Action"]).strip()
        ts = float(row["Timestamp"])
        color = action_colors.get(action, "#95a5a6")

        ax.barh(y_map[tid], 8, left=ts, height=bar_width,
                color=color, edgecolor="black", linewidth=0.3)

    ax.set_yticks(list(y_map.values()))
    ax.set_yticklabels([f"Thread {t}" for t in threads], fontsize=8)
    ax.set_xlabel("Elapsed Time (ms)")
    ax.set_title("Thread Activity Gantt Chart", fontweight="bold")
    ax.grid(axis="x", linestyle="--", alpha=0.3)

    patches = [mpatches.Patch(color=c, label=l) for l, c in action_colors.items()]
    ax.legend(handles=patches, loc="upper right", fontsize=8)

    plt.tight_layout()
    plt.savefig(out_path("gantt_timeline.png"), dpi=200)
    plt.close()
    print("    -> Saved gantt_timeline.png")


# ─────────────────────────────────────────────────
#  2. Wait Time Comparison  (SJF vs FCFS)
# ─────────────────────────────────────────────────
def chart_wait_comparison(sjf_df, fcfs_df):
    print("  [2/5] Generating wait_time_comparison.png ...")

    sjf_mid  = sjf_df[(sjf_df["RosterIndex"] >= 3) & (sjf_df["RosterIndex"] <= 7)].copy()
    fcfs_mid = fcfs_df[(fcfs_df["RosterIndex"] >= 3) & (fcfs_df["RosterIndex"] <= 7)].copy()

    merged = pd.merge(
        sjf_mid[["RosterIndex", "WaitTimeMs"]].rename(columns={"WaitTimeMs": "SJF"}),
        fcfs_mid[["RosterIndex", "WaitTimeMs"]].rename(columns={"WaitTimeMs": "FCFS"}),
        on="RosterIndex", how="outer",
    ).fillna(0.0).sort_values("RosterIndex")

    x_labels = [f"Batsman #{int(r)}" for r in merged["RosterIndex"]]
    x = np.arange(len(merged))
    width = 0.35

    fig, ax = plt.subplots(figsize=(10, 6))
    bars1 = ax.bar(x - width/2, merged["SJF"],  width, label="SJF",  color="#2980b9")
    bars2 = ax.bar(x + width/2, merged["FCFS"], width, label="FCFS", color="#e67e22")

    ax.bar_label(bars1, fmt="%.1f", fontsize=7, padding=2)
    ax.bar_label(bars2, fmt="%.1f", fontsize=7, padding=2)

    ax.set_xlabel("Middle-Order Batsman (Roster Index)")
    ax.set_ylabel("Wait Time (ms)")
    ax.set_title("Middle-Order Wait Time: SJF vs FCFS", fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels)
    ax.legend()
    ax.grid(axis="y", linestyle="--", alpha=0.3)

    plt.tight_layout()
    plt.savefig(out_path("wait_time_comparison.png"), dpi=200)
    plt.close()
    print("    -> Saved wait_time_comparison.png")


# ─────────────────────────────────────────────────
#  3. Average Wait Time Bar
# ─────────────────────────────────────────────────
def chart_avg_wait(sjf_df, fcfs_df):
    print("  [3/5] Generating avg_wait_bar.png ...")

    sjf_mid  = sjf_df[(sjf_df["RosterIndex"] >= 3) & (sjf_df["RosterIndex"] <= 7)]
    fcfs_mid = fcfs_df[(fcfs_df["RosterIndex"] >= 3) & (fcfs_df["RosterIndex"] <= 7)]

    sjf_avg  = sjf_mid["WaitTimeMs"].mean()  if not sjf_mid.empty  else 0
    fcfs_avg = fcfs_mid["WaitTimeMs"].mean() if not fcfs_mid.empty else 0

    sjf_all  = sjf_df["WaitTimeMs"].mean()  if not sjf_df.empty  else 0
    fcfs_all = fcfs_df["WaitTimeMs"].mean() if not fcfs_df.empty else 0

    labels = ["Middle Order\n(SJF)", "Middle Order\n(FCFS)", "All Batsmen\n(SJF)", "All Batsmen\n(FCFS)"]
    values = [sjf_avg, fcfs_avg, sjf_all, fcfs_all]
    colors = ["#2980b9", "#e67e22", "#3498db", "#f39c12"]

    fig, ax = plt.subplots(figsize=(8, 5))
    bars = ax.bar(labels, values, color=colors, edgecolor="black", linewidth=0.5)
    ax.bar_label(bars, fmt="%.1f ms", fontsize=9, padding=3)

    ax.set_ylabel("Average Wait Time (ms)")
    ax.set_title("Average Wait Time: SJF vs FCFS", fontweight="bold")
    ax.grid(axis="y", linestyle="--", alpha=0.3)

    plt.tight_layout()
    plt.savefig(out_path("avg_wait_bar.png"), dpi=200)
    plt.close()
    print(f"    -> SJF middle-order avg: {sjf_avg:.2f} ms")
    print(f"    -> FCFS middle-order avg: {fcfs_avg:.2f} ms")
    print("    -> Saved avg_wait_bar.png")


# ─────────────────────────────────────────────────
#  4. Role Activity Pie Chart
# ─────────────────────────────────────────────────
def chart_role_pie(gantt_df):
    print("  [4/5] Generating role_activity_pie.png ...")

    role_counts = gantt_df["Role"].value_counts()

    colors = {"Bowler": "#e74c3c", "Fielder": "#3498db"}
    pie_colors = [colors.get(r, "#95a5a6") for r in role_counts.index]

    fig, axes = plt.subplots(1, 2, figsize=(12, 5))

    # Left: by role
    axes[0].pie(role_counts, labels=role_counts.index, colors=pie_colors,
                autopct="%1.1f%%", startangle=140, textprops={"fontsize": 10})
    axes[0].set_title("Events by Role", fontweight="bold")

    # Right: by action type
    action_counts = gantt_df["Action"].value_counts()
    act_colors = {"Bowled": "#e74c3c", "Fielded": "#3498db", "Catch": "#2ecc71"}
    act_pie_colors = [act_colors.get(a, "#95a5a6") for a in action_counts.index]

    axes[1].pie(action_counts, labels=action_counts.index, colors=act_pie_colors,
                autopct="%1.1f%%", startangle=140, textprops={"fontsize": 10})
    axes[1].set_title("Events by Action Type", fontweight="bold")

    plt.suptitle("Thread Activity Distribution", fontweight="bold", fontsize=13)
    plt.tight_layout()
    plt.savefig(out_path("role_activity_pie.png"), dpi=200)
    plt.close()
    print("    -> Saved role_activity_pie.png")


# ─────────────────────────────────────────────────
#  5. Ball-by-Ball Event Timeline
# ─────────────────────────────────────────────────
def chart_ball_timeline(gantt_df):
    print("  [5/5] Generating ball_timeline.png ...")

    color_map = {"Bowled": "#e74c3c", "Fielded": "#3498db", "Catch": "#2ecc71"}
    marker_map = {"Bowled": "o", "Fielded": "s", "Catch": "X"}

    fig, ax = plt.subplots(figsize=(14, 5))

    for action, group in gantt_df.groupby("Action"):
        color = color_map.get(action, "#95a5a6")
        marker = marker_map.get(action, "o")
        ax.scatter(group["Timestamp"], group["ThreadID"],
                   c=color, marker=marker, s=40, label=action,
                   edgecolors="black", linewidths=0.3, alpha=0.8)

    ax.set_xlabel("Elapsed Time (ms)")
    ax.set_ylabel("Thread ID")
    ax.set_title("Ball-by-Ball Event Timeline", fontweight="bold")
    ax.legend(fontsize=9)
    ax.grid(True, linestyle="--", alpha=0.3)

    plt.tight_layout()
    plt.savefig(out_path("ball_timeline.png"), dpi=200)
    plt.close()
    print("    -> Saved ball_timeline.png")



# ─────────────────────────────────────────────────
#  Statistics Summary (printed to console)
# ─────────────────────────────────────────────────
def print_statistics(sjf_df, fcfs_df):
    print()
    print("=" * 60)
    print("  SCHEDULING STATISTICS — SJF vs FCFS")
    print("=" * 60)

    sjf_mid  = sjf_df[(sjf_df["RosterIndex"] >= 3) & (sjf_df["RosterIndex"] <= 7)]
    fcfs_mid = fcfs_df[(fcfs_df["RosterIndex"] >= 3) & (fcfs_df["RosterIndex"] <= 7)]

    def stats(df, label):
        if df.empty:
            print(f"  {label}: No data")
            return
        wt = df["WaitTimeMs"]
        print(f"  {label}:")
        print(f"    Average Wait Time : {wt.mean():.2f} ms")
        print(f"    Min Wait Time     : {wt.min():.2f} ms")
        print(f"    Max Wait Time     : {wt.max():.2f} ms")
        print(f"    Std Deviation     : {wt.std():.2f} ms")
        print(f"    Total Wait Time   : {wt.sum():.2f} ms")
        print(f"    Batsmen Scheduled : {len(df)}")

    print()
    print("  --- Middle Order (Roster #3 to #7) ---")
    stats(sjf_mid, "SJF")
    print()
    stats(fcfs_mid, "FCFS")

    # Improvement percentage
    sjf_avg  = sjf_mid["WaitTimeMs"].mean() if not sjf_mid.empty else 0
    fcfs_avg = fcfs_mid["WaitTimeMs"].mean() if not fcfs_mid.empty else 0
    if fcfs_avg > 0:
        improvement = ((fcfs_avg - sjf_avg) / fcfs_avg) * 100
        print(f"\n  >> SJF reduces middle-order avg wait by {improvement:.1f}% vs FCFS")

    print()
    print("  --- All Batsmen ---")
    stats(sjf_df, "SJF")
    print()
    stats(fcfs_df, "FCFS")

    sjf_all  = sjf_df["WaitTimeMs"].mean() if not sjf_df.empty else 0
    fcfs_all = fcfs_df["WaitTimeMs"].mean() if not fcfs_df.empty else 0
    if fcfs_all > 0:
        improvement = ((fcfs_all - sjf_all) / fcfs_all) * 100
        print(f"\n  >> SJF reduces overall avg wait by {improvement:.1f}% vs FCFS")

    # Per-batsman table
    print()
    print("  --- Per-Batsman Wait Time Table ---")
    print(f"  {'Roster#':<10} {'SJF (ms)':<15} {'FCFS (ms)':<15} {'Difference':<15}")
    print(f"  {'-'*55}")

    merged = pd.merge(
        sjf_df[["RosterIndex", "WaitTimeMs"]].rename(columns={"WaitTimeMs": "SJF"}),
        fcfs_df[["RosterIndex", "WaitTimeMs"]].rename(columns={"WaitTimeMs": "FCFS"}),
        on="RosterIndex", how="outer",
    ).fillna(0.0).sort_values("RosterIndex")

    for _, row in merged.iterrows():
        ri = int(row["RosterIndex"])
        s = row["SJF"]
        f = row["FCFS"]
        diff = f - s
        marker = " *" if 3 <= ri <= 7 else ""
        print(f"  #{ri:<9} {s:<15.2f} {f:<15.2f} {diff:<+15.2f}{marker}")

    print(f"\n  (* = middle order)")
    print("=" * 60)


# ─────────────────────────────────────────────────
#  Main
# ─────────────────────────────────────────────────
def main():
    print("=" * 50)
    print("  T20 Simulator — Visualization Suite")
    print("=" * 50)

    gantt_df = load_csv(csv_path("gantt_chart.csv"))
    sjf_df   = load_csv(csv_path("wait_times_sjf.csv"))
    fcfs_df  = load_csv(csv_path("wait_times_fcfs.csv"))

    generated = 0

    # 1. Gantt timeline
    if gantt_df is not None:
        chart_gantt_timeline(gantt_df)
        generated += 1

    # 2 & 3. Wait time charts (need both SJF and FCFS)
    if sjf_df is not None and fcfs_df is not None:
        chart_wait_comparison(sjf_df, fcfs_df)
        generated += 1
        chart_avg_wait(sjf_df, fcfs_df)
        generated += 1
        print_statistics(sjf_df, fcfs_df)
    else:
        print("  [SKIP] Need both wait_times_sjf.csv and wait_times_fcfs.csv for charts 2 & 3.")
        print("         Run simulator once with use_sjf_scheduling=true and once with false.")

    # 4. Role pie chart
    if gantt_df is not None:
        chart_role_pie(gantt_df)
        generated += 1

    # 5. Ball timeline
    if gantt_df is not None:
        chart_ball_timeline(gantt_df)
        generated += 1

    print()
    print(f"  Done! {generated} chart(s) generated.")
    print("=" * 50)


if __name__ == "__main__":
    main()
