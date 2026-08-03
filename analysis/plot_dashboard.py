from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


CURRENCY_LABEL = "GBP"


def read_csv(path: Path) -> pd.DataFrame:
    if not path.exists():
        print(f"warning: telemetry file not found: {path}")
        return pd.DataFrame()

    try:
        frame = pd.read_csv(path)

        if frame.empty:
            print(f"warning: telemetry file contains no rows: {path}")

        return frame

    except pd.errors.EmptyDataError:
        print(f"warning: telemetry file is empty: {path}")
        return pd.DataFrame()

    except pd.errors.ParserError as error:
        raise RuntimeError(
            f"Unable to parse telemetry CSV {path}: {error}"
        ) from error


def save_figure(fig: plt.Figure, output: Path) -> None:
    fig.tight_layout()
    fig.savefig(output, dpi=180, bbox_inches="tight")
    plt.close(fig)
    print(f"created {output}")


def event_x(frame: pd.DataFrame) -> pd.Series:
    if "event_index" in frame.columns:
        return frame["event_index"]
    return pd.Series(np.arange(len(frame)), index=frame.index)


def money(frame: pd.DataFrame, columns: Iterable[str], scale: float) -> pd.DataFrame:
    result = frame.copy()
    for column in columns:
        if column in result.columns:
            result[column] = result[column] / scale
    return result


def plot_pnl(state: pd.DataFrame, output: Path, scale: float) -> None:
    if state.empty:
        return

    frame = money(
        state,
        ["realised_pnl", "unrealised_pnl", "total_pnl"],
        scale,
    )

    pnl_columns = [
        "realised_pnl",
        "unrealised_pnl",
        "total_pnl",
    ]

    if (
            frame[pnl_columns]
                    .fillna(0)
                    .abs()
                    .to_numpy()
                    .max()
            == 0
    ):
        print(
            "skipped P&L chart: no fills or non-zero P&L "
            "were recorded"
        )
        return

    fig, ax = plt.subplots(figsize=(11, 6))
    x = event_x(frame)

    ax.plot(x, frame["realised_pnl"], label="Realised P&L")
    ax.plot(x, frame["unrealised_pnl"], label="Unrealised P&L")
    ax.plot(x, frame["total_pnl"], label="Total P&L")

    ax.axhline(0, linewidth=1)
    ax.set_title("Cumulative realised, unrealised and total P&L")
    ax.set_xlabel("Event")
    ax.set_ylabel(CURRENCY_LABEL)
    ax.legend()
    ax.grid(True, alpha=0.3)

    save_figure(fig, output)


def plot_equity_drawdown(state: pd.DataFrame, output: Path, scale: float) -> None:
    if state.empty:
        return

    frame = money(state, ["account_equity"], scale)
    equity = frame["account_equity"].fillna(0)

    if equity.nunique() <= 1:
        print(
            "skipped equity/drawdown chart: "
            "account equity did not change"
        )
        return
    frame["running_peak"] = frame["account_equity"].cummax()
    frame["drawdown"] = frame["account_equity"] - frame["running_peak"]

    fig, axes = plt.subplots(2, 1, figsize=(11, 8), sharex=True)
    x = event_x(frame)

    axes[0].plot(x, frame["account_equity"], label="Account equity")
    axes[0].plot(x, frame["running_peak"], label="Running peak")
    axes[0].set_ylabel(CURRENCY_LABEL)
    axes[0].set_title("Account equity")
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)

    axes[1].fill_between(x, frame["drawdown"], 0, alpha=0.35)
    axes[1].plot(x, frame["drawdown"], label="Drawdown")
    axes[1].axhline(0, linewidth=1)
    axes[1].set_xlabel("Event")
    axes[1].set_ylabel(CURRENCY_LABEL)
    axes[1].set_title("Drawdown from running equity peak")
    axes[1].grid(True, alpha=0.3)

    save_figure(fig, output)


def plot_inventory_risk(state: pd.DataFrame, output: Path) -> None:
    if state.empty:
        return

    inventory = state["inventory"].fillna(0)

    if inventory.nunique() == 1 and inventory.iloc[0] == 0:
        print(
            "skipped inventory chart: "
            "no strategy inventory was recorded"
        )
        return

    fig, ax = plt.subplots(figsize=(11, 6))
    x = event_x(state)

    ax.plot(x, state["inventory"], label="Inventory")

    if "maximum_absolute_position" in state.columns:
        limit = state["maximum_absolute_position"]
        ax.plot(x, limit, linestyle="--", label="Maximum position")
        ax.plot(x, -limit, linestyle="--", label="Minimum position")

    ax.axhline(0, linewidth=1)
    ax.set_title("Inventory position with risk limits")
    ax.set_xlabel("Event")
    ax.set_ylabel("Quantity")
    ax.legend()
    ax.grid(True, alpha=0.3)

    save_figure(fig, output)


def plot_market_quotes_executions(
        state: pd.DataFrame,
        fills: pd.DataFrame,
        output: Path,
        scale: float,
) -> None:
    if state.empty:
        return

    frame = money(
        state,
        [
            "best_bid",
            "best_ask",
            "mid_price",
            "strategy_bid",
            "strategy_ask",
        ],
        scale,
    )

    fig, ax = plt.subplots(figsize=(13, 7))
    x = event_x(frame)

    for column, label in [
        ("best_bid", "Market bid"),
        ("best_ask", "Market ask"),
        ("mid_price", "Mid-price"),
        ("strategy_bid", "Strategy bid"),
        ("strategy_ask", "Strategy ask"),
    ]:
        values = frame[column].replace(0, np.nan)
        ax.plot(x, values, label=label)

    if not fills.empty:
        execution = fills.copy()
        execution["price"] = execution["price"] / scale

        buys = execution[execution["side"] == "BUY"]
        sells = execution[execution["side"] == "SELL"]

        ax.scatter(
            buys["event_index"],
            buys["price"],
            marker="^",
            s=55,
            label="Buy execution",
        )
        ax.scatter(
            sells["event_index"],
            sells["price"],
            marker="v",
            s=55,
            label="Sell execution",
        )

    ax.set_title("Market bid/ask, strategy quotes and executions")
    ax.set_xlabel("Event")
    ax.set_ylabel(f"Price ({CURRENCY_LABEL})")
    ax.legend(ncol=2)
    ax.grid(True, alpha=0.3)

    save_figure(fig, output)


def plot_spreads(state: pd.DataFrame, output: Path, scale: float) -> None:
    if state.empty:
        return

    frame = money(
        state,
        ["market_spread", "strategy_spread"],
        scale,
    )

    fig, ax = plt.subplots(figsize=(11, 6))
    x = event_x(frame)

    ax.plot(x, frame["market_spread"], label="Market spread")
    ax.plot(
        x,
        frame["strategy_spread"].replace(0, np.nan),
        label="Strategy spread",
    )

    ax.set_title("Market spread and strategy spread")
    ax.set_xlabel("Event")
    ax.set_ylabel(f"Spread ({CURRENCY_LABEL})")
    ax.legend()
    ax.grid(True, alpha=0.3)

    save_figure(fig, output)


def plot_depth_heatmap(depth: pd.DataFrame, output: Path, scale: float) -> None:
    if depth.empty:
        return

    frame = depth.copy()
    frame["price"] = frame["price"] / scale
    frame["signed_quantity"] = np.where(
        frame["side"] == "BID",
        frame["quantity"],
        -frame["quantity"],
        )

    pivot = frame.pivot_table(
        index="price",
        columns="event_index",
        values="signed_quantity",
        aggfunc="sum",
        fill_value=0,
    ).sort_index()

    if pivot.empty:
        return

    fig, ax = plt.subplots(figsize=(14, 8))
    image = ax.imshow(
        pivot.to_numpy(),
        aspect="auto",
        origin="lower",
        interpolation="nearest",
        extent=[
            float(pivot.columns.min()),
            float(pivot.columns.max()),
            float(pivot.index.min()),
            float(pivot.index.max()),
        ],
    )

    ax.set_title("Order-book depth heatmap")
    ax.set_xlabel("Event")
    ax.set_ylabel(f"Price ({CURRENCY_LABEL})")
    colour_bar = fig.colorbar(image, ax=ax)
    colour_bar.set_label(
        "Signed depth: positive bid, negative ask"
    )

    save_figure(fig, output)


def depth_imbalance(depth: pd.DataFrame) -> pd.DataFrame:
    if depth.empty:
        return pd.DataFrame()

    grouped = (
        depth.groupby(["event_index", "side"], as_index=False)["quantity"]
        .sum()
        .pivot(index="event_index", columns="side", values="quantity")
        .fillna(0)
        .reset_index()
    )

    if "BID" not in grouped.columns:
        grouped["BID"] = 0
    if "ASK" not in grouped.columns:
        grouped["ASK"] = 0

    denominator = grouped["BID"] + grouped["ASK"]
    grouped["imbalance"] = np.where(
        denominator > 0,
        (grouped["BID"] - grouped["ASK"]) / denominator,
        0.0,
        )

    return grouped


def plot_imbalance(
        depth: pd.DataFrame,
        output: Path,
) -> None:
    frame = depth_imbalance(depth)

    if frame.empty:
        return

    # Ensure observations are in chronological event order
    # before calculating the rolling average.
    frame = (
        frame
        .sort_values("event_index")
        .reset_index(drop=True)
    )

    # Smooth the noisy raw imbalance series.
    #
    # Since your C++ program records depth every 100 events,
    # 100 observations represent approximately 10,000 events.
    frame["rolling_imbalance"] = (
        frame["imbalance"]
        .rolling(
            window=100,
            min_periods=1,
        )
        .mean()
    )

    fig, ax = plt.subplots(
        figsize=(11, 6)
    )

    # Raw event-level imbalance.
    ax.plot(
        frame["event_index"],
        frame["imbalance"],
        alpha=0.25,
        linewidth=0.6,
        label="Raw imbalance",
    )

    # Smoothed trend.
    ax.plot(
        frame["event_index"],
        frame["rolling_imbalance"],
        linewidth=1.5,
        label="Rolling mean",
    )

    # Zero means equal recorded bid and ask depth.
    ax.axhline(
        0,
        linewidth=1,
        linestyle="--",
        label="Balanced book",
    )

    ax.set_ylim(
        -1.05,
        1.05,
    )

    ax.set_title(
        "AAPL Order-Book Imbalance"
    )

    ax.set_xlabel(
        "Event"
    )

    ax.set_ylabel(
        "(Bid depth - ask depth) / total depth"
    )

    ax.grid(
        True,
        alpha=0.3,
    )

    ax.legend()

    save_figure(
        fig,
        output,
    )


def plot_fill_rate_maker_taker(
        quotes: pd.DataFrame,
        fills: pd.DataFrame,
        output: Path,
) -> None:
    if quotes.empty and fills.empty:
        return

    fig, axes = plt.subplots(2, 1, figsize=(11, 8))

    if not quotes.empty:
        accepted = quotes[
            (quotes["accepted"] == 1) &
            (quotes["order_id"] != 0)
            ][
            ["event_index", "order_id"]
        ].drop_duplicates(
            subset="order_id",
            keep="first",
        )

        accepted["order_id"] = pd.to_numeric(
            accepted["order_id"],
            errors="coerce",
        )
        accepted = accepted.dropna(subset=["order_id"])
        accepted["order_id"] = accepted["order_id"].astype(np.uint64)

        accepted_by_event = (
            accepted
            .groupby("event_index")
            .size()
            .rename("new_accepted_orders")
        )

        filled_by_event = pd.Series(
            dtype=float,
            name="new_filled_orders",
        )

        if not fills.empty:
            filled = fills[["event_index", "order_id"]].copy()
            filled["order_id"] = pd.to_numeric(
                filled["order_id"],
                errors="coerce",
            )
            filled = filled.dropna(subset=["order_id"])
            filled["order_id"] = filled["order_id"].astype(np.uint64)

            filled = filled[
                filled["order_id"].isin(accepted["order_id"])
            ].drop_duplicates(
                subset="order_id",
                keep="first",
            )

            filled_by_event = (
                filled
                .groupby("event_index")
                .size()
                .rename("new_filled_orders")
            )

        timeline = pd.concat(
            [
                accepted_by_event,
                filled_by_event,
            ],
            axis=1,
        ).fillna(0).sort_index()

        timeline["accepted_orders"] = (
            timeline["new_accepted_orders"].cumsum()
        )

        timeline["filled_orders"] = (
            timeline["new_filled_orders"].cumsum()
        )

        timeline["fill_rate"] = (
                timeline["filled_orders"] /
                timeline["accepted_orders"].replace(0, np.nan) *
                100.0
        )

        axes[0].plot(
            timeline.index,
            timeline["fill_rate"],
        )
        axes[0].set_ylim(0, 105)
        axes[0].set_ylabel("Fill rate (%)")
        axes[0].set_title(
            "Cumulative filled quote orders / accepted quote orders"
        )
        axes[0].grid(True, alpha=0.3)

    if not fills.empty:
        counts = (
            fills.assign(
                liquidity=np.where(
                    fills["was_maker"] == 1,
                    "Maker",
                    "Taker",
                    )
            )
            .groupby("liquidity")["quantity"]
            .sum()
            .reindex(["Maker", "Taker"], fill_value=0)
        )

        axes[1].bar(counts.index, counts.values)
        axes[1].set_ylabel("Filled quantity")
        axes[1].set_title("Maker versus taker filled quantity")
        axes[1].grid(True, axis="y", alpha=0.3)

    axes[1].set_xlabel("Liquidity role")
    save_figure(fig, output)


def calculate_markouts(
        state: pd.DataFrame,
        fills: pd.DataFrame,
        horizons_ns: list[int],
        scale: float,
) -> pd.DataFrame:
    if state.empty or fills.empty:
        return pd.DataFrame()

    mids = (
        state[["timestamp_ns", "mid_price"]]
        .drop_duplicates("timestamp_ns")
        .sort_values("timestamp_ns")
        .copy()
    )

    fills_frame = fills.sort_values("timestamp_ns").copy()
    fills_frame["mid_at_fill"] = fills_frame["mid_at_fill"].replace(
        0,
        np.nan,
    )

    rows: list[dict[str, float]] = []

    for horizon in horizons_ns:
        target = fills_frame[
            ["timestamp_ns", "side", "price", "mid_at_fill"]
        ].copy()

        target["target_timestamp_ns"] = (
                target["timestamp_ns"] + horizon
        )

        future = pd.merge_asof(
            target.sort_values("target_timestamp_ns"),
            mids.rename(
                columns={
                    "timestamp_ns": "future_timestamp_ns",
                    "mid_price": "future_mid",
                }
            ).sort_values("future_timestamp_ns"),
            left_on="target_timestamp_ns",
            right_on="future_timestamp_ns",
            direction="forward",
        )

        future = future.dropna(subset=["future_mid"])
        if future.empty:
            continue

        signed_markout = np.where(
            future["side"] == "BUY",
            future["future_mid"] - future["price"],
            future["price"] - future["future_mid"],
            )

        rows.append(
            {
                "horizon_ns": horizon,
                "mean_markout": float(np.mean(signed_markout) / scale),
                "median_markout": float(np.median(signed_markout) / scale),
                "observations": int(len(signed_markout)),
            }
        )

    return pd.DataFrame(rows)


def horizon_label(value_ns: int) -> str:
    if value_ns >= 1_000_000_000:
        return f"{value_ns / 1_000_000_000:g}s"
    if value_ns >= 1_000_000:
        return f"{value_ns / 1_000_000:g}ms"
    if value_ns >= 1_000:
        return f"{value_ns / 1_000:g}µs"
    return f"{value_ns}ns"


def plot_markouts(
        state: pd.DataFrame,
        fills: pd.DataFrame,
        output: Path,
        scale: float,
) -> None:
    horizons = [
        1_000_000,
        10_000_000,
        100_000_000,
        1_000_000_000,
        5_000_000_000,
    ]

    frame = calculate_markouts(
        state,
        fills,
        horizons,
        scale,
    )

    if frame.empty:
        print(
            "skipped markout curve: the simulation does not contain "
            "future midpoint observations at the configured horizons"
        )
        return

    fig, ax = plt.subplots(figsize=(10, 6))
    labels = [
        horizon_label(int(value))
        for value in frame["horizon_ns"]
    ]

    ax.plot(labels, frame["mean_markout"], marker="o", label="Mean")
    ax.plot(labels, frame["median_markout"], marker="o", label="Median")
    ax.axhline(0, linewidth=1)
    ax.set_title("Post-trade markout curve")
    ax.set_xlabel("Horizon after fill")
    ax.set_ylabel(f"Signed markout ({CURRENCY_LABEL})")
    ax.legend()
    ax.grid(True, alpha=0.3)

    save_figure(fig, output)


def percentile_value(values: pd.Series, percentile: float) -> float:
    return float(np.percentile(values.to_numpy(), percentile))


def plot_latency_percentiles(latency: pd.DataFrame, output: Path) -> None:
    if latency.empty:
        return

    rows: list[dict[str, float | str]] = []

    for operation, group in latency.groupby("operation"):
        values = group["latency_ns"].dropna()
        if values.empty:
            continue

        rows.append(
            {
                "operation": operation,
                "p50": percentile_value(values, 50),
                "p95": percentile_value(values, 95),
                "p99": percentile_value(values, 99),
                "maximum": float(values.max()),
            }
        )

    frame = pd.DataFrame(rows)
    if frame.empty:
        return

    x = np.arange(len(frame))
    width = 0.2

    fig, ax = plt.subplots(figsize=(12, 7))
    ax.bar(x - 1.5 * width, frame["p50"], width, label="p50")
    ax.bar(x - 0.5 * width, frame["p95"], width, label="p95")
    ax.bar(x + 0.5 * width, frame["p99"], width, label="p99")
    ax.bar(x + 1.5 * width, frame["maximum"], width, label="Maximum")

    ax.set_xticks(x)
    ax.set_yscale("log")
    ax.set_xticklabels(frame["operation"], rotation=20, ha="right")
    ax.set_ylabel("Latency (ns)")
    ax.set_title("Operation latency percentiles")
    ax.legend()
    ax.grid(True, axis="y", alpha=0.3)

    save_figure(fig, output)


def plot_throughput_queue(
        state: pd.DataFrame,
        output: Path,
) -> None:
    if state.empty:
        return

    required_columns = {
        "wall_time_ns",
        "event_index",
    }

    if not required_columns.issubset(state.columns):
        print(
            "skipped throughput chart: "
            "required state columns are missing"
        )
        return

    frame = (
        state
        .dropna(
            subset=[
                "wall_time_ns",
                "event_index",
            ]
        )
        .sort_values("wall_time_ns")
        .copy()
    )

    if len(frame) < 2:
        print(
            "skipped throughput chart: "
            "not enough observations"
        )
        return

    start_time_ns = frame["wall_time_ns"].iloc[0]

    frame["elapsed_seconds"] = (
                                       frame["wall_time_ns"] -
                                       start_time_ns
                               ) / 1_000_000_000.0

    frame["second_bucket"] = (
        np.floor(
            frame["elapsed_seconds"]
        )
        .astype(int)
    )

    throughput = (
        frame
        .groupby("second_bucket")
        .size()
        .rename("rows_per_second")
    )

    total_elapsed_seconds = float(
        frame["elapsed_seconds"].iloc[-1]
    )

    last_bucket = int(
        throughput.index.max()
    )

    last_bucket_duration = (
            total_elapsed_seconds -
            last_bucket
    )

    # The final bucket is normally shorter than one complete
    # second because processing stops partway through it.
    #
    # Counting that partial bucket as a full second produces
    # the misleading final throughput collapse.
    if (
            len(throughput) > 1
            and last_bucket_duration < 0.95
    ):
        throughput = throughput.drop(
            index=last_bucket
        )

    if throughput.empty:
        print(
            "skipped throughput chart: "
            "no complete one-second intervals"
        )
        return

    average_throughput = float(
        throughput.mean()
    )

    queue_was_measured = (
            "queue_depth" in frame.columns
            and frame["queue_depth"]
            .fillna(0)
            .abs()
            .max() > 0
    )

    if not queue_was_measured:
        # LOBSTER reference processing reads directly from
        # CSV files and does not pass rows through the SPSC
        # network queue, so display only real throughput data.
        fig, ax = plt.subplots(
            figsize=(11, 6)
        )

        ax.plot(
            throughput.index,
            throughput.values,
            marker="o",
            linewidth=1.5,
            label="Rows processed",
        )

        ax.axhline(
            average_throughput,
            linestyle="--",
            linewidth=1.2,
            label=(
                f"Mean: "
                f"{average_throughput:,.0f} rows/s"
            ),
        )

        ax.set_title(
            "LOBSTER Reference-Row Processing Throughput"
        )

        ax.set_xlabel(
            "Completed elapsed second"
        )

        ax.set_ylabel(
            "Rows processed per second"
        )

        ax.grid(
            True,
            alpha=0.3,
        )

        ax.legend()

        save_figure(
            fig,
            output,
        )

        return

    # This branch remains available for simulations where
    # an SPSC queue depth was genuinely recorded.
    queue = (
        frame
        .groupby("second_bucket")[
            "queue_depth"
        ]
        .max()
        .rename("maximum_queue_depth")
    )

    queue = queue.reindex(
        throughput.index
    ).fillna(0)

    fig, axes = plt.subplots(
        2,
        1,
        figsize=(11, 8),
        sharex=True,
    )

    axes[0].plot(
        throughput.index,
        throughput.values,
        marker="o",
        linewidth=1.5,
        label="Events processed",
    )

    axes[0].axhline(
        average_throughput,
        linestyle="--",
        linewidth=1.2,
        label=(
            f"Mean: "
            f"{average_throughput:,.0f} events/s"
        ),
    )

    axes[0].set_ylabel(
        "Events/s"
    )

    axes[0].set_title(
        "Event Processing Throughput"
    )

    axes[0].grid(
        True,
        alpha=0.3,
    )

    axes[0].legend()

    axes[1].plot(
        queue.index,
        queue.values,
        marker="o",
        linewidth=1.5,
    )

    axes[1].set_xlabel(
        "Completed elapsed second"
    )

    axes[1].set_ylabel(
        "Queue entries"
    )

    axes[1].set_title(
        "Maximum SPSC Queue Depth per Second"
    )

    axes[1].grid(
        True,
        alpha=0.3,
    )

    save_figure(
        fig,
        output,
    )


def plot_udp_recovery(
        state: pd.DataFrame,
        recovery: pd.DataFrame,
        output: Path,
) -> None:
    if state.empty and recovery.empty:
        return

    counter_columns = [
        "gaps_detected",
        "events_recovered",
        "recovery_misses",
    ]

    has_counter_activity = (
            not state.empty
            and all(
        column in state.columns
        for column in counter_columns
    )
            and state[counter_columns]
            .fillna(0)
            .to_numpy()
            .any()
    )

    has_recovery_events = not recovery.empty

    if not has_counter_activity and not has_recovery_events:
        print(
            "skipped UDP recovery chart: "
            "no UDP recovery activity was recorded"
        )
        return

    fig, axes = plt.subplots(3, 1, figsize=(12, 10), sharex=False)

    if not state.empty:
        x = event_x(state)

        axes[0].plot(
            x,
            state["gaps_detected"],
            label="Gaps detected",
        )
        axes[0].plot(
            x,
            state["events_recovered"],
            label="Events recovered",
        )
        axes[0].plot(
            x,
            state["recovery_misses"],
            label="Recovery misses",
        )
        axes[0].set_title("UDP sequence and recovery counters")
        axes[0].set_xlabel("Event")
        axes[0].set_ylabel("Cumulative count")
        axes[0].legend()
        axes[0].grid(True, alpha=0.3)

    if not recovery.empty:
        completed = recovery[
            recovery["event"].isin(
                ["gap_end", "gap_failed"]
            )
        ].copy()

        if not completed.empty:
            completed["duration_us"] = (
                    completed["duration_ns"] / 1_000.0
            )

            axes[1].bar(
                np.arange(len(completed)),
                completed["duration_us"],
            )
            axes[1].set_ylabel("Duration (µs)")
            axes[1].set_title("Recovery duration by incident")
            axes[1].grid(True, axis="y", alpha=0.3)

        state_map = {
            "Healthy": 0,
            "Recovering": 1,
            "Failed": 2,
        }

        recovery["state_value"] = (
            recovery["feed_state"]
            .map(state_map)
            .fillna(-1)
        )

        axes[2].step(
            recovery["wall_time_ns"],
            recovery["state_value"],
            where="post",
        )
        axes[2].set_yticks([0, 1, 2])
        axes[2].set_yticklabels(
            ["Healthy", "Recovering", "Failed"]
        )
        axes[2].set_xlabel("Steady-clock timestamp (ns)")
        axes[2].set_title("Feed-state timeline")
        axes[2].grid(True, alpha=0.3)

    save_figure(fig, output)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Generate trading, risk, market-quality and "
            "low-latency charts from the simulator telemetry CSV files."
        )
    )

    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path("results/telemetry"),
    )

    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("results/charts"),
    )

    parser.add_argument(
        "--monetary-scale",
        type=float,
        default=100.0,
        help=(
            "Divisor used for prices and monetary values. "
            "Use 100 for pence and 10000 for LOBSTER prices."
        ),
    )

    parser.add_argument(
        "--currency",
        type=str,
        default="GBP",
        help="Currency label used on graph axes, such as GBP or USD.",
    )

    args = parser.parse_args()

    if args.monetary_scale <= 0:
        parser.error("--monetary-scale must be greater than zero")

    global CURRENCY_LABEL
    CURRENCY_LABEL = args.currency.upper()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    state = read_csv(args.data_dir / "state.csv")
    depth = read_csv(args.data_dir / "depth.csv")
    quotes = read_csv(args.data_dir / "quotes.csv")
    fills = read_csv(args.data_dir / "fills.csv")
    latency = read_csv(args.data_dir / "latency.csv")
    recovery = read_csv(args.data_dir / "recovery.csv")

    plot_pnl(
        state,
        args.output_dir / "01_pnl.png",
        args.monetary_scale,
        )

    plot_equity_drawdown(
        state,
        args.output_dir / "02_equity_drawdown.png",
        args.monetary_scale,
        )

    plot_inventory_risk(
        state,
        args.output_dir / "03_inventory_risk.png",
        )

    plot_market_quotes_executions(
        state,
        fills,
        args.output_dir / "04_market_quotes_executions.png",
        args.monetary_scale,
        )

    plot_spreads(
        state,
        args.output_dir / "05_spreads.png",
        args.monetary_scale,
        )

    plot_depth_heatmap(
        depth,
        args.output_dir / "06_depth_heatmap.png",
        args.monetary_scale,
        )

    plot_imbalance(
        depth,
        args.output_dir / "07_order_book_imbalance.png",
        )

    plot_fill_rate_maker_taker(
        quotes,
        fills,
        args.output_dir / "08_fill_rate_maker_taker.png",
        )

    plot_markouts(
        state,
        fills,
        args.output_dir / "09_markout_curve.png",
        args.monetary_scale,
        )

    plot_latency_percentiles(
        latency,
        args.output_dir / "10_latency_percentiles.png",
        )

    plot_throughput_queue(
        state,
        args.output_dir / "11_throughput_queue_depth.png",
        )

    plot_udp_recovery(
        state,
        recovery,
        args.output_dir / "12_udp_recovery_feed_state.png",
        )


if __name__ == "__main__":
    main()