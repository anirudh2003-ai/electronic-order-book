from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent

DIRECTORIES = [
    "include",
    "src",
    "tests",
    "demos",
    "benchmarks",
    "support",
]

FILES = [
    # Public headers
    "include/types.hpp",
    "include/winsock_runtime.hpp",
    "include/spsc_ring_buffer.hpp",
    "include/order_book.hpp",
    "include/market_event.hpp",
    "include/lobster_loader.hpp",
    "include/binary_codec.hpp",
    "include/replay_engine.hpp",
    "include/risk_management.hpp",
    "include/market_maker_strategy.hpp",
    "include/pnl_engine.hpp",
    "include/recovery.hpp",
    "include/udp_feed.hpp",
    "include/validation.hpp",
    "include/runners.hpp",

    # Production source files
    "src/main.cpp",
    "src/order_book.cpp",
    "src/market_event.cpp",
    "src/lobster_loader.cpp",
    "src/binary_codec.cpp",
    "src/replay_engine.cpp",
    "src/risk_management.cpp",
    "src/validation.cpp",

    # Shared scenario configuration
    "support/scenario_defaults.hpp",
    "support/scenario_defaults.cpp",

    # Test files
    "tests/test_support.hpp",
    "tests/test_support.cpp",
    "tests/order_book_tests.cpp",
    "tests/replay_tests.cpp",
    "tests/strategy_tests.cpp",
    "tests/pnl_tests.cpp",
    "tests/recovery_tests.cpp",

    # Demonstrations
    "demos/strategy_pnl_demo.cpp",
    "demos/udp_recovery_demo.cpp",
    "demos/lobster_demo.cpp",

    # Benchmarks
    "benchmarks/benchmark_utils.hpp",
    "benchmarks/benchmark_utils.cpp",
    "benchmarks/replay_scalability.cpp",
    "benchmarks/matching_engine_benchmark.cpp",
    "benchmarks/udp_scalability.cpp",
]


def create_directory(relative_path: str) -> None:
    directory = PROJECT_ROOT / relative_path

    if directory.exists():
        print(f"folder already exists: {relative_path}")
        return

    directory.mkdir(parents=True, exist_ok=True)
    print(f"created folder:       {relative_path}")


def create_file(relative_path: str) -> None:
    file_path = PROJECT_ROOT / relative_path

    file_path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    if file_path.exists():
        print(f"file already exists:  {relative_path}")
        return

    file_path.touch()
    print(f"created file:         {relative_path}")


def main() -> None:
    print(f"Project root: {PROJECT_ROOT}")
    print()

    for directory in DIRECTORIES:
        create_directory(directory)

    print()

    for file in FILES:
        create_file(file)

    print()
    print("Project structure created successfully.")
    print("Existing files were not modified or overwritten.")


if __name__ == "__main__":
    main()