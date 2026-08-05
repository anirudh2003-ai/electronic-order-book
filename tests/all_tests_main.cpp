#include "runners.hpp"
#include "winsock_runtime.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        WinsockRuntime winsock;

        runExecuteOrderTests();
        runFeedReplayTests();
        runStrategyRiskTests();
        runPnlTests();
        runUdpRecoveryTests();

        std::cout
            << "\nAll engine test suites passed.\n";

        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr
            << "Test runner failed: "
            << exception.what()
            << '\n';

        return EXIT_FAILURE;
    }
}