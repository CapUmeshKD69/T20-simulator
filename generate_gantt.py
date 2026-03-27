import pandas as pd
import matplotlib.pyplot as plt


def build_intervals(events_df: pd.DataFrame):
    active = {}
    intervals = []

    max_ts = float(events_df["Timestamp"].max()) if not events_df.empty else 0.0

    for row in events_df.itertuples(index=False):
        key = (str(row.ThreadID), str(row.Resource))
        action = str(row.Action).strip().lower()
        ts = float(row.Timestamp)

        if action == "acquired":
            active[key] = ts
        elif action == "released":
            start = active.pop(key, None)
            if start is not None and ts >= start:
                intervals.append((str(row.ThreadID), str(row.Resource), start, ts - start))

    # Handle missing Released events (for example canceled/deadlocked threads)
    # by extending the bar to the last known timestamp in the trace.
    for (thread_id, resource), start in active.items():
        if max_ts >= start:
            intervals.append((thread_id, resource, start, max_ts - start))

    return intervals


def main():
    df = pd.read_csv("gantt_chart.csv")

    required = {"Timestamp", "ThreadID", "Resource", "Action"}
    missing = required.difference(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {sorted(missing)}")

    df = df.sort_values("Timestamp").reset_index(drop=True)
    intervals = build_intervals(df)

    if not intervals:
        print("No intervals to plot. Check gantt_chart.csv contents.")
        return

    plot_df = pd.DataFrame(intervals, columns=["ThreadID", "Resource", "Start", "Duration"])

    thread_order = sorted(plot_df["ThreadID"].unique(), key=lambda x: int(x) if x.isdigit() else x)
    y_positions = {tid: idx for idx, tid in enumerate(thread_order)}

    color_map = {
        "Pitch": "#d62728",  # red
        "End1": "#1f77b4",   # blue
        "End2": "#2ca02c",   # green
        "end1_mutex": "#1f77b4",
        "end2_mutex": "#2ca02c",
    }

    fig, ax = plt.subplots(figsize=(14, 7))

    for row in plot_df.itertuples(index=False):
        color = color_map.get(row.Resource, "#7f7f7f")
        ax.barh(
            y_positions[row.ThreadID],
            row.Duration,
            left=row.Start,
            height=0.6,
            color=color,
            edgecolor="black",
            linewidth=0.4,
        )

    ax.set_yticks(list(y_positions.values()))
    ax.set_yticklabels(list(y_positions.keys()))
    ax.set_xlabel("Elapsed Time (ms)")
    ax.set_ylabel("ThreadID")
    ax.set_title("T20 Simulator Pitch Resource Gantt Chart")

    legend_items = [
        plt.Line2D([0], [0], color="#d62728", lw=6, label="Pitch"),
        plt.Line2D([0], [0], color="#1f77b4", lw=6, label="End1"),
        plt.Line2D([0], [0], color="#2ca02c", lw=6, label="End2"),
        plt.Line2D([0], [0], color="#7f7f7f", lw=6, label="Other"),
    ]
    ax.legend(handles=legend_items, loc="upper right")

    ax.grid(axis="x", linestyle="--", alpha=0.35)
    plt.tight_layout()
    plt.savefig("gantt_visualization.png", dpi=200)
    print("Saved gantt_visualization.png")


if __name__ == "__main__":
    main()
