# Run 2026-10-22-run4: not published

This run is kept as measured, for audit. The published results come from the next run, which repeated every suite.

Reason: other work ran on the measured machine during this run (a compiler and the verification script, started from
an interactive session). In several suites the throughput varied more than in run 2026-10-21-run3: the median
coefficient of variation per configuration was 4.0% against 0.6% (pipeline), 3.5% against 0.8% (connections), 1.5%
against 0.4% (capacity) and 3.6% against 0.7% (validation). One repetition, `validation/valkey-kvpace-16c-r4.json`,
measured 41,901 operations per second; the other four repetitions of that configuration measured 207,396 to 215,508.

The source tree was at commit 4b0317b; `env.json` and `manifest.json` describe the run.
