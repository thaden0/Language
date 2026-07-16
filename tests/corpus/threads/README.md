# Threads corpus

Worker tests keep output deterministic: workers compute and return or send
values; only the spawning thread writes output, at join/receive points.

`native_floor.lev` covers the additive `sysTcpListen(port, reusePort)` overload
and `std::cpuCount()` used by worker-pool sizing.
