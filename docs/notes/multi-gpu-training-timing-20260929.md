# Kitchen multi-GPU timing record — 2026-09-29

This is the retained timing ledger for [M0](multi-gpu-training-m0-execution.md)
and the [B1 fixed-topology proof](multi-gpu-training-b1-execution.md). Numbers
below are observed unless explicitly marked *derived*. The absolute local
dataset, output, checkpoint and raw-tool-output paths are recorded only in
the private local M0 manifest, not in this repository. Nothing here is a
production multi-GPU speedup claim. Historical quality and 20% stop policies
are superseded for performance-only benchmarking; see the disposition below.

## Run identity and normalization

- Source base: `8f8c0f5a58c4740824ef5e9d4a335614424e4860` plus uncommitted
  local changes. Tracked binary-diff digest at this ledger's creation:
  `178b1d01b060c524508d4b07d1e28e3b92c47942`. New native files are
  untracked and separately identified by their contents: B1 merger
  `a01cc5074935ea8d25b55c74439ca668c570954e`, Vulkan ownership header
  `3e0dfad6769c27c84c5cbaaaa4569854e8094301`, M0 staging benchmark
  `e952de6cacc009fd9004665a74497569340bf833`, EngineB1
  `17f4fa3bef5564b1b1280cfefbd08952108152f6`. This is an artifact
  digest, not a committed revision or a claim that future worktree edits
  reproduce these exact values.
- Windows 11, Release headless Vulkan via `build_develop.bat
  -DSS_BACKEND=vulkan -DSS_BUILD_BACKEND_TESTS=ON -DSS_BUILD_GUI=OFF`;
  MSVC 14.44.35207, pinned Slang 2026.12.0.1. Two AMD Radeon AI PRO R9700
  discrete devices, driver 26.Q3, Vulkan 1.4.349, UUID selectors ending
  `...01000000000000000000000000` and `...02000000000000000000000000`;
  each reports 32,624 MiB. Usable budgets and PCIe/NUMA topology are unknown.
  No third supported GPU exists on this host.
- Kitchen COLMAP `sparse/0`: 1,327 registered images, 1,195 training and
  132 held-out views on a 0.9 fraction split, 488,119 seed points, one
  post-split face per input. Resolutions include 3024×4032 and 4080×3060.
  Native 3DGS, SH3, 1M cap, packed projection, quantization level 1,
  effective FPBO, disk image cache, original 30,000-step horizon. Input
  images, checkpoint and final model were never moved or rewritten for a
  timing comparison.
- Every short window restarts a named full checkpoint in a separate process
  on an explicit UUID. The reported interval is between two CLI progress
  timestamps after at least 67 early or 100 middle/late warmup steps;
  checkpoint publication and setup are excluded. Progress prints every 100
  steps: **per-step median, p95 and camera IDs were not captured**. Random
  sampling/background trajectories were not bit-replayed, so these are
  independent repeats, not identical per-view traces.

## Native single-device full controls

| Run | Active/cap | Resumed training segment | Held-out raw / corrected PSNR | Reported `engine_vram` |
|---|---:|---:|---:|---:|
| Initial full control | 1M / 1M | 29,866 steps, 02:40:31.995 | 20.2990 / 25.4256 dB on 132 views | 1,949.81 MiB |
| Retained-checkpoint repeat | 1M / 1M | 29,866 steps, 02:40:40.982 | 20.2877 / 25.3972 dB on 132 views | 1,901.95 MiB |

Both segments resume step 134; their time excludes the first 134 steps,
startup, checkpoint I/O and evaluation. The retained run has full 5k/10k/
15k/20k/25k/30k checkpoints. `engine_vram` is retained pool high-water,
not simultaneous phase residency or a driver budget guarantee. The final
full checkpoint restored separately and reproduced held-out PSNR within
0.000002 dB.

## Independent-job one/two-device capacity screen

Each simultaneous worker updates its **own complete model**, not a shard of
one model. For a pair, use the slower worker's 200-step interval. Aggregate
throughput factor is *derived* as `2 × median(single) / median(pair max)`.

| Frozen state | One device, independent 200-step samples (s) | Concurrent device-0 / device-1 samples (s) | Median one / pair-max (s) | Derived aggregate factor |
|---|---|---|---:|---:|
| Early, step 134; 488,119 active | 66.951 | 66.244/66.644 | 66.951 / 66.644 | 2.01×, one sample |
| Middle, step 10k; 1M active | 65.320, 65.864, 65.492 | 65.597/66.382, 65.687/65.658, 65.525/66.058 | 65.492 / 66.058 | 1.98× |
| Late, step 25k; 1M active | 62.924, 62.269, 61.406 | 63.568/62.722, 61.907/62.825, 62.788/63.209 | 62.269 / 63.209 | 1.97× |

The near-2× result is **aggregate independent-job throughput only**. It
does not reduce time-to-quality for one model. These windows use the same
full-resolution dataset and original schedule; setup and cooperative-stop
checkpoint costs are separate.

## B1: one model approximated by two owned/context blocks

The B1 experiment starts each worker from the **same full step-10k snapshot**.
For its first fixed-topology proof, all 1M source rows stay render-visible
on both devices; owner 0 updates 1,953 whole 256-row FPBO blocks and owner 1
updates the complementary 1,954. A common local view manifest assigns
514/681 disjoint training images. Shared appearance and densification are
frozen for the two workers **and the matched one-device all-owned control**.
Each worker's unowned parameter, quantized value/moment and bias-step hash
was unchanged after training. This is approximate block optimization, not
globally coupled gradient synchronization or a capacity-saving worker yet.

| Repeat | All-owned, 400 measured images (s) | Two blocks, 200 measured images each (s, slower worker) | Derived steady aggregate factor |
|---|---:|---:|---:|
| 1 | 130.916 | 64.553 | 2.03× |
| 2 | 130.010 | 64.930 | 2.00× |
| 3 | 128.819 | 65.248 | 1.97× |
| **Median** | **130.010** | **64.930** | **2.00×** |

Each block actually executes 302 optimizer updates including warmup, while
the one-device control executes 502. The measured image exposure total is
400 in both arms, but parameter update counts/objectives differ. One extra
two-block diagnostic with complete included/excluded row sidecars measured
64.221/65.829 s per worker and merged 484,117+484,302=968,419 valid rows;
it is not part of the three-repeat median.

One end-to-end **short-window** sample, excluding partition discovery and
input staging but including process setup, final checkpoint write, merge and
native held-out evaluation, was:

| Component | Two blocks | One all-owned device |
|---|---:|---:|
| Process wall | 101.540 s | 164.216 s |
| Owned PLY merge and native readback | 0.552 s | — |
| 132-view held-out evaluation | 182.776 s | 183.248 s |
| **Total, derived** | **284.868 s** | **347.464 s** |

That is 18.0% less wall, below the **historical** provisional 20% short
end-to-end screen, now superseded as a benchmark stop rule. It excludes
partition construction, whole-job input staging and a production merged
checkpoint, so it is **not** an end-to-end time-to-quality result.
The normal step medians, full control training time, profiled timings and
these short end-to-end totals must not be mixed into one speedup ratio.

### Merged quality and recovery evidence

| Paired repeat | Raw PSNR, merged / all-owned | Corrected PSNR, merged / all-owned | Worst raw delta on 20-view overlap/large-projection diagnostic set |
|---|---:|---:|---:|
| 1 | 20.3082 / 20.2123 dB | 24.8113 / 24.7440 dB | -0.974 dB |
| 2 | 20.3227 / 20.2726 dB | 24.8342 / 24.7459 dB | -0.756 dB |
| 3 | 20.3817 / 20.3628 dB | 24.8529 / 24.7589 dB | -0.405 dB |

The 20-view set combines the ten highest projected owner-overlap ratios and
ten largest projected candidate counts from the *frozen* held-out trace.
It was not approved as the application boundary set. One view outside it
regressed 2.668 dB in repeat 1, but not in repeats 2/3; an additional
fully covered-ID merge had a -2.185 dB out-of-set view. Thus a per-view
quality guarantee is not established. The native binary-PLY merger verified
same schema, complementary owner masks, complete unique included+excluded
source-row coverage (1M IDs) and readback of the merged PLY. Valid merged
vertex counts across three early runs were 968,401, 968,435 and 968,430;
a later fully covered-ID run produced 968,419. Invalid, low-opacity and
dead-scale owned rows are explicitly excluded rather than duplicated.

An earlier full owner-0 B1 checkpoint resumed for one step with its context
hash unchanged; a wrong-owner resume failed. After adding a source-content
identity and published step to the sidecar header, a two-owner **zero-step**
real checkpoint merge covered all 1M IDs and yielded 985,614 valid vertices.
The current source-bound sidecar format has **not** been exercised by a
full trained-block resume. The merged PLY is renderable, not a globally
resumable merged training checkpoint.

## Visibility, residency and transport context

- Profiled 302-step windows were kept separate from normal speed rows.
  Middle retained pool/process were 1,782.16/1,783.89 MiB; late
  1,870.14/1,873.70 MiB. Late profiled wall was 97.09 s; PPISP image
  backward 48.24 s and raster backward 25.72 s of stamped GPU intervals
  over 302 calls. H2D moved 11.80 GB/1.226 s; D2H 0.193 GB/0.056 s.
  The driver peak *within* named phases, host RSS and growth old+new
  allocations were not measured.
- Frozen 1M snapshot, **all 1,195 training views**: projected candidates
  median/p95/max 97,760/563,094/803,047; intersections
  472,898/1,173,299/1,508,647. Equal-*row* y partition from this trace
  has 500k owned rows per side, 35.8% foreign projected candidates,
  420.5M/206.1M intersection load and 368,635/319,676 unique foreign
  context rows. B1's actual mask is **256-row block-aligned**, so do not
  substitute these row-wise capacities for measured worker peaks. Projected
  visibility is not proof of meaningful contribution.
- Completed pinned-host local H2D/D2H (ms per payload): device 0 at
  64 KiB 0.068/0.073, 1 MiB 0.110/0.175, 16 MiB 0.721/1.051,
  64 MiB 2.561/3.051; device 1 respectively 0.080/0.094,
  0.253/0.317, 2.721/2.858, 10.616/10.774. The large-payload device
  asymmetry repeated in reverse measurement order; topology is unknown.
- Two-process ACK-serialized, content-verified host bridge steady packet
  medians A→B/B→A: 64 KiB 0.564/0.511 ms, 1 MiB 3.840/3.538 ms,
  16 MiB 47.065/46.460 ms, 64 MiB 187.999/180.495 ms. Parent bridge
  sampled each completed packet; first-process setup packet was excluded.
  This includes pipe copying, validation, a device poison and an extra
  receiving-GPU D2H read, and is not a production transport. Opposing
  64 MiB streams measured ~192/186 ms medians. One 16 MiB attempt failed
  content validation; six subsequent eight-packet repeats passed. The
  simple local-copy-only 64 MiB A→B floor is 13.667 ms before the process
  hop or packing. No compute/transport overlap was demonstrated.

## Gates and raw evidence

- **Observed checks:** pinned Vulkan dev build; 13/13 applicable headless
  tests with unrelated `delaunay_degenerate` excluded after its t16 surface
  coverage failure; AMD `cli_training_smoke` 1/1; focused
  `fpbo_split_parity` passed; native two-owner contexts, PLY merge/readback
  and malformed sidecar rejection passed. The full headless selection was
  13/14 when that unrelated Delaunay test failed once. No CUDA/NVIDIA lane
  was run. This ledger is not a substitute for runtime validation of all
  unfinished recovery/production cases.
- **Raw local artifacts at this ledger's creation:** each then-named output
  held its `config.json`, PLY or checkpoint and, where evaluated, `metrics.json`.
  Original complete step-5k through step-30k snapshots were in the retained
  control run at that time; later external cleanup is recorded below.
  Full-run and instrumented terminal logs are referenced by exact opaque
  tool-output IDs in the private M0 manifest. The timing-window driver and
  private M0/B1 manifests preserve the exact UUIDs, commands, source/output
  roots and all individual repeat intervals. Raw per-step samples were never
  captured; no per-step median/p95 should be inferred from a 200-step sum.
  After this ledger was saved, four agent-owned diagnostic GT/render PNGs
  for one selected view and a byte-identical duplicate timed merged PLY were
  removed. Their `metrics.json`, configs, primary merged model and full
  source checkpoints were independently verified present afterward. The
  removed images cannot serve as live visual evidence; the measured scores
  and raw timing rows remain in this ledger and retained metrics.
- **Later benchmark disposition (dated history):** this ledger's fixed-topology
  B1 short experiment delivered ~2× steady aggregate image-exposure throughput
  and 18.0% less short wall before partition cost. The older-source three-repeat
  longer reusable-preparation benchmark and its then-unverified cold/pixel costs
  remain recorded separately in the [comparison scorecard](multi-gpu-training-comparison-20260929.md).
  A **different regenerated step-10k native checkpoint** subsequently produced
  three new complete control/B1 cold and reuse repeats with measured pixels and
  sampled per-worker host RSS in that scorecard: control median 427.351 s,
  B1 median 337.416 s cold / 232.915 s reuse. The native G1 synthetic
  non-FPBO parity, source-FPBO nonfused gradient-only diagnostic and completed
  synthetic-pattern staging are not actual G1 owner training or a capacity
  claim. O1/G1 training wall, current device VRAM peaks, B2 and recovery remain
  unverified/unaccepted; sidecar config/view binding and full trained resume
  remain open. The then-recorded C: 1,953,808,384 B free scratch was a
  **historical resource snapshot**, not the current free-space state. The old
  checkpoint files were subsequently removed externally; no old merged PLY
  stands in for their optimizer state.
