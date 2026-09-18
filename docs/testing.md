# Testing

Use CTest's `headless` label for the ordinary post-build feedback loop. GPU
integration and native parity tests are separate gates; desktop rendering and
input still require the real application.

**Support policy:** NVIDIA support is deprecated, including CUDA and Vulkan on
NVIDIA GPUs. Do not run or extend those lanes unless explicitly re-enabled by
the user. Active GPU checks use Vulkan on non-NVIDIA hardware; CPU-only checks
remain supported. Historical cross-backend results below are reference material,
not current acceptance requirements. See [AGENTS.md](../AGENTS.md).

## Quick headless loop

```bat
build_develop.bat -DSS_BACKEND=vulkan
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error -j 2
```

On Linux use `bash build_develop.bash` with the same options; on macOS the
build directory is `build`. Use `-L fast` for deterministic CPU checks,
`-L worker` for subprocess/scheduler contracts, and `-R <name>` to focus.
Keep `--no-tests=error`: an empty selection is not a successful check.
Do not use bare `ctest` when intending to avoid GPU workloads.

The normal GUI-enabled Vulkan build registers 19 headless tests. None opens a
window or initializes a compute device. `-DSS_BUILD_GUI=OFF` runs 14 core tests;
it omits the batch, preset, argv and two stamp tests. CUDA also omits
`split_faces_test`, `frustum_size_test` and `bilagrid_selector_test` unless built
with `-DSS_BUILD_BACKEND_TESTS=ON`. Configure output identifies these differences.
Kernel parity and weight-dependent model tools are not automatically registered.

`cmake/SsRunTest.cmake` allocates a unique root for every CTest invocation of a
test, sets isolated temp/home/config/cache paths before launching it, and sets
`SS_NO_AUTO_FETCH=1`. This wrapper also isolates older tests that use fixed names
inside the system temp directory. Invoke those through CTest rather than assuming
direct executable invocations are concurrent-safe. Worker drivers receive the
target-resolved `spirula` path; no PATH lookup chooses another build.

On failure, read `Testing/Temporary/LastTest.log` and the printed directory under
`Testing/Artifacts/<test>-<unique-id>/`: it retains `process.log`, requests,
results, isolated application state and relevant outputs. Successful roots are
removed. CI runs the headless label on Windows, Linux and macOS and uploads its
job-owned `Testing/` directory on failure. Fast/worker/GPU outer timeouts are
60/300/600 seconds; subprocess drivers bound and reap their own children.

## Real training lifecycle

After building Vulkan, explicitly select a compatible non-NVIDIA device:

```bat
rem Prefer the non-NVIDIA device UUID printed by the device listing.
set SS_TEST_DEVICE=uuid:YOUR_DEVICE_UUID
cmake -E chdir build_vulkan ctest -L gpu --output-on-failure --no-tests=error
```

On POSIX prefix the CTest command with `SS_TEST_DEVICE=<selector>`. This
test-only selector is required and passed explicitly to the production CLI:
Vulkan accepts its normal selectors and canonicalizes to UUID.
Missing/unusable hardware fails, never skips. The driver does not change user
device preferences or download models.

`cli_training_smoke` generates six 64×64 images and 64 seed points, trains
12 steps with a 256-splat cap and full checkpoints every five steps, then resumes
through the scheduler/worker to step 24. It validates checkpoint contents using
the production readers and matches the request/result identity and output paths.
Longer finite attempts synchronize on a readable checkpoint before cooperative
stop or force-stop. The former must be resumable; the latter must be interrupted,
not successful, and retain a valid checkpoint. `checkpoint_resolution_test`
separately rejects incomplete newer siblings. Neither test claims a deterministic
kill during the atomic publication window.

The `training_gpu` CTest resource lock serializes this test within an invocation;
it is not a system-wide reservation against another trainer. Run hardware lanes
one at a time. No exact stochastic model bytes or loss trajectory are compared.

## 1. Native numerical tests and historical cross-backend results

`src/backend/tests/*.cpp` — currently 20 tools covering projection (fwd, bwd,
quant-grad), rasterization bwd, tile intersect, warp, FPBO, optimizer (general
+ geometry), densify, per-pixel train, PPISP, bilagrid, multi-scale loss
(`mask_loss_semantics`, `reg_loss_underflow` and `fpbo_split_parity` are
self-checking rather than dump-then-compare: the first pins what an image mask
means in the loss, in both mask modes and with none; the second sweeps log
scales past every exp(scales) underflow threshold, down to -inf, and fails if
the per-splat regularizers hand the optimizer a NaN or push a splat below
kMinLogScale; the third steps FPBO and the non-fused optimizer path from the
same state and fails if they disagree, which is how the two update laws for
linear splat colour are held together),
meshing (activation, LBVH, occupancy/bisection/color, moment raster, the
per-camera samplers and the visibility cull), plus
`backend/tests/engine/` which drives the *real* engine end to end
(render parity, train parity, and two self-checking tools rather than
dump-then-compare: `engine_reset_state` trains the same scene twice across an
`engine_reset()` and the two must land in the same place, and
`gt_bilagrid_sentinels` pins the GT bilateral grids' invariants -- one depth
scalar per camera, and the no-GT sentinels passing through untouched).
`backend/vulkan/tests/` adds 3 Vulkan-only smoke tests (runtime, pipeline,
sort/scan).

**Historical reference-generation workflow — not an active validation requirement.**
The same source can still build under both backends, but CUDA/NVIDIA execution
is deprecated. The following recipe documents existing reference provenance;
do not run its CUDA step or regenerate NVIDIA references under the current policy.

```bash
# on the CUDA machine
./build_cuda/projection_parity dump ref.bin
# on the target machine / device
./build_vulkan/projection_parity compare ref.bin
```

Inputs are deterministic, and comparison is tolerance-based — fast-math
exp/sqrt chains legitimately differ across compilers, and borderline-cull
flips change whole rows, so a small allowance for those is built in.

Two tests carry a **relative-RMS gate** alongside the per-element one, for the
same reason: they contain discrete per-pixel or per-splat decisions that flip
wherever an architecture's rounding differs from the reference's, so a handful
of large outliers is expected while the vector as a whole must still agree.
`msloss_parity`'s NMS / quantile / clip modes put an RX 7800 XT at 0.21% of
tight elements out of tolerance (max_abs 4.19) against 0% on NVIDIA Vulkan --
but 3.8e-4 relative RMS against 1.8e-7. A permuted or biased reference sits
orders of magnitude above that, which is what the RMS gate is there to catch.

`engine_train_parity` has two gates rather than one, because per-element
agreement is not something any implementation can hold across 12 optimizer
steps. The threshold-crossing kernels -- median depth, masked-tile skip, the
rasterize-bwd survivor batching -- flip a handful of pixels per step wherever
an architecture's rounding differs from the reference's, and Adam turns a
flipped gradient sign into a full-size parameter step, so the trajectories
separate. Measured against a CUDA reference (2026-08-25): NVIDIA Vulkan lands
0.003% of elements out of tolerance at 4.8e-7 relative RMS, while an RX 7800 XT
lands 4.4% at 1.4e-4 -- identically on amdvlk and RADV, and unchanged by
`RADV_PERFTEST=wave32`, so it is not a wave-size effect. The divergence starts
in `depth_loss` and `normal_loss` (discrete median-depth selection); `rgb_loss`,
`ssim` and `psnr` stay at 1e-7. An indexing or layout break, by contrast, puts
30%+ of elements out of tolerance at a relative RMS above 1. The RMS gate is
what keeps the test sharp; the element gate is loose enough to absorb the drift.

`msloss_parity` splits its reference into two channels. **Tight**: per-pixel
gradients (deterministic given the raw-loss sums, which enter them only through
smooth reduce math), the densification loss map in every mode, equal-shape
`v_ref_depth` / `v_ref_normal` scatters (one atomic per cell), and the quantile
outputs. **Loose**: `LossValues`, the SSIM display scalar, and scaled-GT
scatters, which accumulate atomically in a backend-specific order. Each config
runs twice and compares the second return, because the scalars come back
through a one-iteration-behind async readout on both backends. One expected
mismatch survives: the CUDA SSIM scalar sums over TILE-GRID positions, so an
image whose dims are not a multiple of the tile picks up zero-padded
out-of-image contributions that differ between the 24- and 16-wide tiles
(`ssim_cs` cfg). It is display-only; gradients and loss values are unaffected.

Several tools also take a `*_DUMP_GOT` environment variable
(`FPBO_DUMP_GOT`, `PPISP_DUMP_GOT`, `MSLOSS_DUMP_GOT`, `PWTRAIN_DUMP_GOT`,
`BILAGRID_DUMP_GOT`, `DENSIFY_DUMP_GOT`) to write the *actual* values
alongside the reference, which is how you diff a mismatch numerically instead
of guessing.

### Building them

```bash
# Active Vulkan tests are built unconditionally.
bash build_develop.bash -DSS_BACKEND=vulkan
```

Each `.cpp` becomes an executable of the same base name in `build_vulkan/`
(`build/` on macOS).

### Historical cross-machine / cross-vendor runs

The following transfer procedure describes how existing CUDA references were
produced. It is historical, not a request to run NVIDIA validation:

1. Transfer a matching `slangc` to the target and point `-DSS_SLANGC=` at
   it — SPIR-V is compiled at build time and never committed, so the target
   needs a compiler, and the version is pinned.
2. Dump references on the CUDA host.
3. Copy the `.bin` files over and run `compare` on the target.

Keep reference dumps out of git (`parity_refs/` is gitignored).

### macOS / MoltenVK

All 17 parity tools pass against a CUDA reference on Apple silicon, at
essentially the Linux numbers (`engine_render_parity`'s blit channel: 0.157%
of bytes on macOS against 0.152% on Linux, cap 0.2%).

`engine_render_parity` used to fail here on that channel, and the failure was
worth more than its number: the viewer's grid and frustum lines came out
*fragmented on macOS only*. It was not antialiasing, which is what this
section claimed for a while. `vis_blit`'s BVH descent read the popped node
inside a two-iteration child loop, and SPIRV-Cross re-materialized that
threadgroup read once per child instead of keeping it — so the second child
was read after the first child's push had overwritten the slot, and the
descent walked into the wrong subtree. The `[ForceUnroll]` on both child
loops is what keeps the pop a value; see the first MoltenVK rule in
`src/backend/vulkan/README.md`.

Three tools -- `msloss_parity`, `optimgeo_parity`, `meshing_parity` -- pass
only because `VulkanContext::init()` turns MoltenVK's default Metal fast-math
off. `SS_VK_FAST_MATH=1` puts it back, and they fail again; that is the knob
to reach for when measuring what the setting costs.

Speed is a separate question from parity, and macOS answers it differently. Two
kernel choices that cost nothing elsewhere cost an order of magnitude here, and
both are measured at run time rather than assumed: the GEMM tiling
(`OpGemm.cpp`, `SS_NN_GEMM_KERNEL` pins it) and the matcher's dot product
(`integerDotProduct4x8BitPackedUnsignedAccelerated`, `SS_SFM_NO_DOT4` pins it).
With those, an M2 runs SAM 3 image encoding at 12x an RTX 5070 (7.4 s vs 0.64 s,
was 140x) and brute-force matching at 22x (43 vs 1.9 ms per 8192x8192 pair, was
64x); the matcher's remainder is DP4A, which Apple has no instruction for. The
third is "slangc `[unroll]`" in `src/backend/vulkan/README.md`.

## 2. GUI / viewer checks

The web viewer can be driven headlessly over the Chrome DevTools Protocol.
Headless defaults to SwiftShader; to exercise a real GPU, run against a real
display (`DISPLAY=:0`).

A scripted run that serves the viewer needs **`--keep-viewer-alive 0`**, or
the process hangs at exit waiting on it.

### Desktop smoke checklist

Use a newly created scratch directory with isolated `APPDATA`/`LOCALAPPDATA`
(Windows) or `HOME`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME` (POSIX), and disable
unattended fetching. Do not point the smoke at the user's queue or private data.
Use the generated six-view scene from a retained integration fixture.

1. **D1 — graphics lifecycle:** launch the exact built `spirula` with the dataset
   directory as its argument. Observe a rendered frame with six camera frustums
   and the seed points. Close normally and verify exit code zero.
2. **D2 — actual input and scheduling:** through the real Batch form, choose that
   dataset, a short training configuration, explicit job GPU and scratch output.
   Submit; observe queue state and appended bottom-pane logs. Cancel an active
   attempt, verify its truthful stopped/interrupted state and no remaining child,
   then close normally. A first-frame screenshot does not satisfy this step.
3. Device-control changes additionally need a preview on A while a queued job
   targets B, with actual preview continuity—not merely the target label.

Capture the visible transition and process/state evidence. Canvas input that the
automation tool reports as delivered but that produces no visible change is not
successful interaction. Web-viewer CDP checks do not replace these desktop gates,
and the older scheduling/handoff acceptance backlog remains separate.

## 3. Deterministic application contracts

`dataset_parser_test` generates equivalent COLMAP text/binary, Nerfstudio and
Metashape scenes and checks canonical frame ordering, camera poses, intrinsics,
pinhole distortion, seed coordinates and train-frame scaling. Interval and
fraction train/eval splits must be disjoint and cover the complete source set;
both retain the same normalization. This is native coverage, not a replacement
for every historical lens/configuration golden.

`step_config_test` calls the shared training implementation around regularization,
supervision, median and distortion warmups, LR endpoints and an absolute resumed
midpoint. It also checks non-unit scene scaling and scale-agnostic mean updates.

`batch_process_test` checks preset/override resolution, frozen queued options,
independent foreground reservations, preflight errors and cancellation.
`scheduler_result_test` uses a test-only child executable to inject wrong
identities, malformed results and missing outputs; none may advance the workflow.
Real worker preparation remains covered by `scheduler_test`, while
`cli_training_smoke` covers real training. The protocol fixture is not a fake
successful compute workload. Rapid scheduler shutdown/pause cycles guard the
condition-variable lost-wake regression found by concurrent headless runs.

## What to run before calling a change done

| change | gate |
|---|---|
| any kernel | Vulkan build + relevant behavioral/numerical check on supported non-NVIDIA hardware |
| engine logic | Vulkan build + relevant real-engine workload on supported non-NVIDIA hardware |
| config field | add the row in `src/config/TrainConfig.h`; check `spirula train --help` and the GUI's All Options editor |
| training-loop logic | `step_config_test`, then `cli_training_smoke` on supported non-NVIDIA Vulkan hardware |
| build system | affected supported Vulkan modes in [build.md](build.md) |
| a comment you wrote | `python3 tools/check_comment_length.py` — the build runs it anyway ([lints](build.md#lints)) |
| `SS_FILE` or `SS_SOURCE_ROOT` | `source_path` on each supported host toolchain |
| a mesh format, or which colors it carries | `mesh_format_roundtrip` — writes every format and reads it back through the other implementation |
| a preset field, or a batch row's shape | `preset_roundtrip_test` + `batch_process_test`; actual form wiring also needs D2 |
| what a typed-in command line becomes, or what a message may carry into it | `command_argv_test` — the message stays one argument and stays JSON-safe |
| a per-cell optimizer launcher (Vulkan) | `SS_OPTIM_SLICE_CELLS=2048` on `optim_parity` / `optimgeo_parity`, which forces the multi-slice path only an SH buffer past ~24M splats would otherwise take ([SH layouts](notes/sh-quant-layout.md)) |
| scheduler / worker protocol | `ctest -L worker`; supported Vulkan train lifecycle when affected |
| desktop rendering or input | D1 + the affected D2 interaction, on a real display |
| ordinary host-only change | focused headless behavior test, then the headless label |

## Profiling

`SS_PROFILE=1` enables the env-gated per-stage timing breakdown
(H2D / D2H / D2D / memset / device / host). Header-only, works on both
backends — the right first tool when a backend is unexpectedly slow rather
than wrong.

Above that table both backends print **GPU time by kernel**, so the two are
directly comparable without a profiler. Vulkan brackets each dispatch with
timestamp queries; CUDA does the same with a CUDA event pair, injected by
`-Wl,--wrap=cudaLaunchKernel` (`backend/cuda/KernelProfilerCuda.cu`) so no
launch site is instrumented by hand and CUB's kernels are covered too. Rows
aggregate over template arguments / specialization constants, which is what
makes a CUDA row and a Vulkan row the same thing.

Two caveats on reading those numbers against each other. The intervals
include the gap before each kernel starts, so their sum runs a little over
the device-wait total. And a training run is **not** reproducible: atomic
order moves the trajectory, and the rasterization and sort kernels then see a
different scene — `rasterize_fwd` has been seen to move 70% between two runs
of the same binary. The image-sized kernels (losses, bilagrid, PPISP, FPBO)
hold to ~1%, so they can be A/B'd from a training run directly; for the rest
use the benchmark tools, which fix the workload:

```bash
./build_vulkan/raster_bench [num_splats] [iters] [macro_log2]   # raster fwd/bwd, binning
./build_vulkan/fpbo_bench   [num_splats] [iters]                # fused projection bwd + optimizer
```

A run that trains also prints a VRAM breakdown after the timing table: pool
capacity per `VramCategory` (`src/core/PoolSlots.h`), the scratch buffer, the
driver's process figure, and the twelve largest buffers. The pool never
shrinks, so those are training peaks, not the numbers at exit. Both front
ends emit it — the CLI at the end of the process, the GUI when its window
closes.
