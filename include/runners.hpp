#pragma once

// Test runners
void runExecuteOrderTests();
void runFeedReplayTests();
void runStrategyRiskTests();
void runPnlTests();
void runUdpRecoveryTests();

// Demonstrations
void runStrategyPnlDemo();
void runUdpRecoveryTelemetryDemo();
void runFullLobsterUdpRecoveryDemo();
void runLobsterReplayDemo();

// Benchmarks
void runLobsterScalabilityBenchmark();
void runMatchingEngineLatencyBenchmark();
void runLobsterUdpScalabilityBenchmark();