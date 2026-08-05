#include "runners.hpp"
#include "winsock_runtime.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

int main(
    int argc,
    char* argv[]
) {
    try {
        WinsockRuntime winsock;

        /*
            Isolated matching-engine latency benchmark.
        */
        if (
            argc >= 2 &&
            std::string{argv[1]} ==
                "--benchmark"

        ) {
            runMatchingEngineLatencyBenchmark();

            return EXIT_SUCCESS;
        }

        /*
            UDP gap-detection and recovery telemetry demo.
        */
        if (
            argc >= 2 &&
            std::string{argv[1]} ==
                "--udp-demo"
        ) {
            runUdpRecoveryTelemetryDemo();

            return EXIT_SUCCESS;
        }

        if (
    argc >= 2 &&
    std::string{argv[1]} ==
        "--udp-full"
) {
            runFullLobsterUdpRecoveryDemo();

            return EXIT_SUCCESS;
}
        if (
    argc >= 2 &&
    std::string{argv[1]} ==
        "--scalability"
) {
            runLobsterScalabilityBenchmark();

            return EXIT_SUCCESS;
}
        if (
    argc >= 2 &&
    std::string{argv[1]} ==
        "--udp-scalability"
) {
            runLobsterUdpScalabilityBenchmark();

            return EXIT_SUCCESS;
}

        /*
            Normal test and demonstration mode.
        */
        runExecuteOrderTests();
        runFeedReplayTests();
        runStrategyRiskTests();
        runPnlTests();
        runUdpRecoveryTests();

        runStrategyPnlDemo();
        runLobsterReplayDemo();

        return EXIT_SUCCESS;
    } catch (
        const std::exception& exception
    ) {
        std::cerr
            << "Fatal error: "
            << exception.what()
            << '\n';

        return EXIT_FAILURE;
    }
}