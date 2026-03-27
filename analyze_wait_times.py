import pandas as pd
import matplotlib.pyplot as plt
import numpy as np


def load_csv(path: str) -> pd.DataFrame:
    try:
        return pd.read_csv(path)
    except FileNotFoundError:
        print(
            f"Missing file: {path}. Run the simulator twice by toggling use_sjf_scheduling "
            "(true then false) and generate both wait_times_sjf.csv and wait_times_fcfs.csv."
        )
        raise
    except Exception as exc:
        print(f"Failed to read {path}: {exc}")
        raise


def filter_middle_order(df: pd.DataFrame) -> pd.DataFrame:
    required = {"RosterIndex", "WaitTimeMs", "ThreadID"}
    missing = required.difference(df.columns)
    if missing:
        raise ValueError(f"Missing columns {sorted(missing)} in dataset")

    middle = df[(df["RosterIndex"] >= 3) & (df["RosterIndex"] <= 7)].copy()
    middle = middle.sort_values("RosterIndex")
    return middle


def main() -> None:
    try:
        sjf_df = load_csv("wait_times_sjf.csv")
        fcfs_df = load_csv("wait_times_fcfs.csv")
    except Exception:
        return

    try:
        sjf_middle = filter_middle_order(sjf_df)
        fcfs_middle = filter_middle_order(fcfs_df)
    except Exception as exc:
        print(f"Data validation error: {exc}")
        return

    sjf_avg = float(sjf_middle["WaitTimeMs"].mean()) if not sjf_middle.empty else 0.0
    fcfs_avg = float(fcfs_middle["WaitTimeMs"].mean()) if not fcfs_middle.empty else 0.0

    print(f"Middle Order Average Wait (SJF): {sjf_avg:.3f} ms")
    print(f"Middle Order Average Wait (FCFS): {fcfs_avg:.3f} ms")

    merged = pd.merge(
        sjf_middle[["RosterIndex", "WaitTimeMs"]].rename(columns={"WaitTimeMs": "SJF"}),
        fcfs_middle[["RosterIndex", "WaitTimeMs"]].rename(columns={"WaitTimeMs": "FCFS"}),
        on="RosterIndex",
        how="outer",
    ).fillna(0.0)

    merged = merged.sort_values("RosterIndex")
    x_labels = [f"B{int(idx)}" for idx in merged["RosterIndex"]]

    x = np.arange(len(merged))
    width = 0.36

    fig, ax = plt.subplots(figsize=(10, 6))
    ax.bar(x - width / 2, merged["SJF"], width, label="SJF", color="#1f77b4")
    ax.bar(x + width / 2, merged["FCFS"], width, label="FCFS", color="#ff7f0e")

    ax.set_xlabel("Middle Order Batsmen (Roster Index)")
    ax.set_ylabel("Wait Time (ms)")
    ax.set_title("Middle Order Wait Time Comparison: SJF vs FCFS")
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels)
    ax.legend()
    ax.grid(axis="y", linestyle="--", alpha=0.35)

    plt.tight_layout()
    plt.savefig("wait_time_comparison.png", dpi=200)
    print("Saved wait_time_comparison.png")


if __name__ == "__main__":
    main()
