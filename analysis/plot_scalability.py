from pathlib import Path
PROJECT_ROOT = Path(__file__).resolve().parents[1]

import matplotlib.pyplot as plt
import pandas as pd


INPUT_PATH = (
        PROJECT_ROOT
        / "results"
        / "scalability"
        / "lobster_scalability.csv"
)

UDP_INPUT_PATH = (
        PROJECT_ROOT
        / "results"
        / "scalability"
        / "udp_scalability.csv"
)

OUTPUT_DIRECTORY = (
        PROJECT_ROOT
        / "results"
        / "lobster_charts"
)


def save_figure(
    figure: plt.Figure,
    filename: str,
) -> None:
    OUTPUT_DIRECTORY.mkdir(
        parents=True,
        exist_ok=True,
    )

    output_path = OUTPUT_DIRECTORY / filename

    figure.tight_layout()
    figure.savefig(
        output_path,
        dpi=180,
        bbox_inches="tight",
    )
    plt.close(figure)

    print(f"created {output_path}")


def load_results() -> pd.DataFrame:
    if not INPUT_PATH.exists():
        raise FileNotFoundError(
            "Scalability CSV was not found at: "
            f"{INPUT_PATH.resolve()}\n"
            "Run the C++ program using --scalability first."
        )

    data = pd.read_csv(INPUT_PATH)

    required_columns = {
        "events",
        "repetition",
        "runtime_seconds",
        "throughput_eps",
        "p50_ns",
        "p95_ns",
        "p99_ns",
        "max_sampled_ns",
        "peak_working_set_mb",
        "final_resting_orders",
        "rejected_events",
        "invariants_ok",
    }

    missing_columns = required_columns.difference(
        data.columns
    )

    if missing_columns:
        raise ValueError(
            "The scalability CSV is missing columns: "
            f"{sorted(missing_columns)}"
        )

    if data.empty:
        raise ValueError(
            "The scalability CSV contains no benchmark rows."
        )

    if data["rejected_events"].sum() != 0:
        raise ValueError(
            "The scalability benchmark contains rejected events."
        )

    invariants_ok = (
        pd.to_numeric(
            data["invariants_ok"],
            errors="coerce",
        )
        .fillna(0)
        .astype(bool)
    )

    if not invariants_ok.all():
        raise ValueError(
            "At least one benchmark run failed its "
            "order-book invariant check."
        )

    return data


def create_summary(
    data: pd.DataFrame,
) -> pd.DataFrame:
    summary = (
        data.groupby(
            "events",
            as_index=False,
        )
        .agg(
            runtime_seconds=(
                "runtime_seconds",
                "median",
            ),
            runtime_min=(
                "runtime_seconds",
                "min",
            ),
            runtime_max=(
                "runtime_seconds",
                "max",
            ),
            throughput_eps=(
                "throughput_eps",
                "median",
            ),
            throughput_min=(
                "throughput_eps",
                "min",
            ),
            throughput_max=(
                "throughput_eps",
                "max",
            ),
            p50_ns=(
                "p50_ns",
                "median",
            ),
            p95_ns=(
                "p95_ns",
                "median",
            ),
            p99_ns=(
                "p99_ns",
                "median",
            ),
            max_sampled_ns=(
                "max_sampled_ns",
                "median",
            ),
            peak_working_set_mb=(
                "peak_working_set_mb",
                "median",
            ),
            final_resting_orders=(
                "final_resting_orders",
                "median",
            ),
        )
        .sort_values("events")
        .reset_index(drop=True)
    )

    return summary


def plot_runtime(
    summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["runtime_seconds"],
        marker="o",
        label="Measured median runtime",
    )

    first_event_count = summary["events"].iloc[0]
    first_runtime = summary[
        "runtime_seconds"
    ].iloc[0]

    linear_reference = (
        first_runtime
        * summary["events"]
        / first_event_count
    )

    axis.plot(
        summary["events"],
        linear_reference,
        marker="o",
        linestyle="--",
        label="Linear scaling reference",
    )

    axis.set_title(
        "LOBSTER-derived replay runtime scaling"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Runtime (seconds)")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "14_scalability_runtime.png",
    )


def plot_throughput(
    summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["throughput_eps"],
        marker="o",
        label="Median throughput",
    )

    axis.fill_between(
        summary["events"],
        summary["throughput_min"],
        summary["throughput_max"],
        alpha=0.2,
        label="Minimum–maximum range",
    )

    axis.set_title(
        "Replay throughput across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Events per second")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "15_scalability_throughput.png",
    )


def plot_latency(
    summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["p50_ns"],
        marker="o",
        label="p50",
    )

    axis.plot(
        summary["events"],
        summary["p95_ns"],
        marker="o",
        label="p95",
    )

    axis.plot(
        summary["events"],
        summary["p99_ns"],
        marker="o",
        label="p99",
    )

    axis.plot(
        summary["events"],
        summary["max_sampled_ns"],
        marker="o",
        linestyle="--",
        label="Median sampled maximum",
    )

    axis.set_yscale("log")

    axis.set_title(
        "Sampled event latency across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Latency (nanoseconds, log scale)")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "16_scalability_latency.png",
    )


def plot_memory(
    summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["peak_working_set_mb"],
        marker="o",
        label="Median peak working set",
    )

    axis.set_title(
        "Observed process memory across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Peak working set (MB)")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "17_scalability_memory.png",
    )

def plot_resting_orders(
        summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["final_resting_orders"],
        marker="o",
        label="Median final resting orders",
    )

    axis.set_title(
        "Final resting-order count across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Resting orders")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "18_scalability_resting_orders.png",
    )


def plot_correctness(
        data: pd.DataFrame,
) -> None:
    frame = data.copy()

    frame["successful_run"] = (
            (
                    frame["rejected_events"] == 0
            )
            &
            (
                    pd.to_numeric(
                        frame["invariants_ok"],
                        errors="coerce",
                    ).fillna(0) == 1
            )
    ).astype(int)

    grouped = (
        frame.groupby(
            "events",
            as_index=False,
        )
        .agg(
            total_runs=(
                "repetition",
                "count",
            ),
            successful_runs=(
                "successful_run",
                "sum",
            ),
            rejected_events=(
                "rejected_events",
                "sum",
            ),
        )
        .sort_values("events")
    )

    grouped["failed_runs"] = (
            grouped["total_runs"]
            - grouped["successful_runs"]
    )

    positions = list(
        range(len(grouped))
    )

    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.bar(
        [
            position - 0.2
            for position in positions
        ],
        grouped["successful_runs"],
        width=0.4,
        label="Successful runs",
    )

    axis.bar(
        [
            position + 0.2
            for position in positions
        ],
        grouped["failed_runs"],
        width=0.4,
        label="Failed runs",
    )

    axis.set_xticks(positions)

    axis.set_xticklabels(
        [
            f"{int(events):,}"
            for events in grouped["events"]
        ]
    )

    axis.set_title(
        "Correctness validation across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Benchmark repetitions")
    axis.grid(
        True,
        axis="y",
        alpha=0.3,
    )
    axis.legend()

    save_figure(
        figure,
        "19_scalability_correctness.png",
    )




def main() -> None:
    data = load_results()
    summary = create_summary(data)

    print("\nScalability benchmark summary")
    print("-----------------------------")
    print(summary.to_string(index=False))
    print()

    plot_runtime(summary)
    plot_throughput(summary)
    plot_latency(summary)
    plot_memory(summary)
    plot_resting_orders(summary)
    plot_correctness(data)

def load_udp_results() -> pd.DataFrame:
    if not UDP_INPUT_PATH.exists():
        raise FileNotFoundError(
            "UDP scalability CSV was not found at: "
            f"{UDP_INPUT_PATH.resolve()}\n"
            "Run the C++ program using "
            "--udp-scalability first."
        )

    data = pd.read_csv(
        UDP_INPUT_PATH
    )

    required_columns = {
        "events",
        "repetition",
        "runtime_seconds",
        "throughput_eps",
        "gaps_detected",
        "events_recovered",
        "recovery_misses",
        "recovery_p50_ns",
        "recovery_p95_ns",
        "recovery_p99_ns",
        "recovery_max_ns",
        "application_rejections",
        "decode_failures",
        "sequencer_failures",
        "final_feed_healthy",
        "invariants_ok",
    }

    missing = required_columns.difference(
        data.columns
    )

    if missing:
        raise ValueError(
            "UDP scalability CSV is missing: "
            f"{sorted(missing)}"
        )

    return data


def create_udp_summary(
        data: pd.DataFrame,
) -> pd.DataFrame:
    return (
        data.groupby(
            "events",
            as_index=False,
        )
        .agg(
            throughput_eps=(
                "throughput_eps",
                "median",
            ),
            gaps_detected=(
                "gaps_detected",
                "median",
            ),
            events_recovered=(
                "events_recovered",
                "median",
            ),
            recovery_misses=(
                "recovery_misses",
                "median",
            ),
            recovery_p50_ns=(
                "recovery_p50_ns",
                "median",
            ),
            recovery_p95_ns=(
                "recovery_p95_ns",
                "median",
            ),
            recovery_p99_ns=(
                "recovery_p99_ns",
                "median",
            ),
            recovery_max_ns=(
                "recovery_max_ns",
                "median",
            ),
        )
        .sort_values("events")
    )


def plot_udp_throughput(
        summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["throughput_eps"],
        marker="o",
        label="Median UDP throughput",
    )

    axis.set_title(
        "UDP processing throughput across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Events per second")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "20_udp_scalability_throughput.png",
    )


def plot_udp_recovery_counts(
        summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["gaps_detected"],
        marker="o",
        label="Gaps detected",
    )

    axis.plot(
        summary["events"],
        summary["events_recovered"],
        marker="o",
        linestyle="--",
        label="Events recovered",
    )

    axis.plot(
        summary["events"],
        summary["recovery_misses"],
        marker="o",
        label="Recovery misses",
    )

    axis.set_title(
        "UDP recovery outcomes across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel("Cumulative count")
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "21_udp_scalability_recovery_counts.png",
    )


def plot_udp_recovery_latency(
        summary: pd.DataFrame,
) -> None:
    figure, axis = plt.subplots(
        figsize=(10, 6)
    )

    axis.plot(
        summary["events"],
        summary["recovery_p50_ns"],
        marker="o",
        label="p50",
    )

    axis.plot(
        summary["events"],
        summary["recovery_p95_ns"],
        marker="o",
        label="p95",
    )

    axis.plot(
        summary["events"],
        summary["recovery_p99_ns"],
        marker="o",
        label="p99",
    )

    axis.plot(
        summary["events"],
        summary["recovery_max_ns"],
        marker="o",
        linestyle="--",
        label="Median maximum",
    )

    axis.set_yscale("log")

    axis.set_title(
        "UDP recovery latency across workload sizes"
    )
    axis.set_xlabel("Event applications")
    axis.set_ylabel(
        "Recovery duration (nanoseconds, log scale)"
    )
    axis.ticklabel_format(
        style="plain",
        axis="x",
    )
    axis.grid(True, alpha=0.3)
    axis.legend()

    save_figure(
        figure,
        "22_udp_scalability_recovery_latency.png",
    )


def main() -> None:
    data = load_results()
    summary = create_summary(data)

    print("\nDirect replay scalability summary")
    print("---------------------------------")
    print(summary.to_string(index=False))
    print()

    plot_runtime(summary)
    plot_throughput(summary)
    plot_latency(summary)
    plot_memory(summary)
    plot_resting_orders(summary)
    plot_correctness(data)

    udp_data = load_udp_results()
    udp_summary = create_udp_summary(
        udp_data
    )

    print("\nUDP scalability summary")
    print("-----------------------")
    print(udp_summary.to_string(index=False))
    print()

    plot_udp_throughput(
        udp_summary
    )

    plot_udp_recovery_counts(
        udp_summary
    )

    plot_udp_recovery_latency(
        udp_summary
    )

if __name__ == "__main__":
    main()