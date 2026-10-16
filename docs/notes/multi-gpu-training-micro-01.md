# Multi-GPU training M0: capacity, visibility, and transport feasibility

Status: **M0/B1 measurements and the G1 native proof are recorded; G1
C0–C4 is closed with C3 correctness PASS / C4 performance FAIL.** No G1
long repeats reopen. The next active plan is the
[bounded O1 residency/writeback screen](multi-gpu-training-o1-screen-20260930.md),
not yet executed. O1 training and architecture selection remain unverified.
See the [comparison scorecard](multi-gpu-training-comparison-20260929.md),
[M0 execution record](multi-gpu-training-m0-execution.md),
[B1 execution record](multi-gpu-training-b1-execution.md) and
[timing ledger](multi-gpu-training-timing-20260929.md). This document retains
the benchmark protocol; completed experiments are not pending checklists.
The [roadmap's selection and release gates](multi-gpu-training-plan.md#10-benchmark-selection-and-separate-release-gates)
govern the decision.

## 1. Decision this experiment must produce

Rank a bounded B1/O1/G1 comparison for **performance on the supplied kitchen
dataset**, then choose the best split-GPU option to develop next. The kitchen
dataset is sufficient: no second workload, new dataset, PSNR/boundary envelope,
full convergence or demonstrated capacity failure is an entry condition.
Whether a single device already fits is a useful baseline fact, not a reason
to stop this comparison. Independent jobs on two GPUs are a throughput
reference, not cooperative training of one model.

Use the existing native trainer and Vulkan backend on supported non-NVIDIA
devices, one GPU per process. Preserve valid outputs and source/owner provenance,
unchanged unowned B1 context, coherent updates on the actually owned state,
and explicit budget/storage safety. Disclose that B1 optimizes approximate
independent blocks, unlike an exact globally coupled candidate. Unsupported
devices or unsafe operations are marked **unverified** for that row, rather
than replaced by a different hardware result or allowed to block safe,
disposable timing runs. Production quality, recovery and oversized-scene
capacity acceptance are separate release questions.

Use only probes that resolve a candidate's bottleneck or a close ranking.
Whole-job staging matters for B1; single-device candidate discovery and
transfer for O1; pairwise streaming for G1. Do not build every probe as a
checklist or implement every branch before screening it.

## 2. Existing facilities and the small instrumentation gap

| Facility | Reuse | Gap to close only where needed |
|---|---|---|
| `src/app/TrainerCore.cpp` | `TrainerSession`, `build_step_config()`, native evaluation/checkpoints | Record the resolved experiment configuration and original schedule horizon. |
| `src/engine/EngineDataManager.cpp` | `engine_train_step_managed()`, real image/face sampling | Record input image IDs, post-split face IDs, weights, and the effective split-batch/FPBO choice. |
| `src/engine/EngineTrainStep.cpp` | Forward/backward, accumulated gradients, optimizer/densification | Add bounded diagnostic observations at existing phase boundaries. |
| `src/engine/EngineQuery.cpp` | `engine_get_pool_breakdown_categorized()`, `engine_get_scratch_bytes()`, `engine_vram_report()` | The current report is pool high-water accounting, not simultaneous phase residency. |
| `src/core/PoolSlots.h`, `src/core/Tensor.h` | Slot categories, formats, backing allocations, alias arena | Distinguish requested bytes, retained capacity, and arena-backed views. |
| `src/engine/EngineState.h`, `src/engine/EngineForward.cpp` | Packed IDs and visibility/intersection intermediates | Export bounded per-view summaries before their buffers are reused. |
| `src/backend/common/Profiler.h` | `SS_PROFILE=1` stage/copy/synchronization timing | Separate instrumented diagnosis from normal end-to-end timings. |
| `src/backend/api/BackendRuntime.h` | Device identity, memory queries, pinned host allocations and copies | Measure completed transfers, including the interprocess hop. |
| Existing engine parity and raster/FPBO benchmark harnesses | Production kernels, fixtures, synchronization patterns | Extend a relevant harness; add a small transfer benchmark only if existing coverage cannot measure this path. |

Do not add a second trainer, public distributed-training options, a general
telemetry framework, or a transport abstraction in M0. Experiment-only capture
belongs at these existing seams. Any environment switch is `SS_`-prefixed and
read through `spirula::env()`.

## 3. M0.1 — freeze the question and baseline

### Minimum reproducible performance manifest

Before comparing candidates, record source revision and local patch digest,
build/backend/driver, OS, explicit supported GPU UUIDs and usable VRAM/host RAM
budgets; dataset and source-checkpoint identities (private paths stay local);
resolved trainer configuration and original schedule horizon; image/face IDs,
pixel dimensions, active/reserved rows, primitive/formats, effective
split-batch/FPBO choice, frozen/shared state and work exposure; exact commands,
warmup, measured interval, repetitions, outputs, and timing boundaries.
Record peak memory where available and free disk space before writing
checkpoint-heavy outputs. Mark absent telemetry **unverified**, not zero.

This is a performance manifest, not a production intake form. A target
quality envelope, held-out/boundary IDs, topology/NUMA detail, per-phase
residency and a second stress workload can be added when a release or a
specific performance/capacity decision needs them. No historical X4 19 dB
floor, matched 1/2 dB regression limits, 15% VRAM reserve or kitchen quality
threshold gates this benchmark. Quality renders are optional diagnostic
evidence, never a substitute for timing or production acceptance.

### Available snapshot windows and coverage

The retained X4 3M checkpoints at steps 500/25,000/45,000 and 6M at
47,602 provide separate frozen-state windows under their original 50,000-step
horizon; the 6M state alone is **not** an early/middle/late trajectory. They
are optional diagnostic controls, not kitchen substitutes. The original
kitchen step-5k through step-30k control snapshots, including the historical
step-10k B1 source, were available for the earlier measurements; their
checkpoint files were subsequently removed externally. A newly regenerated
step-10k/20k/30k native source is **different** from that history; the new
source's three-repeat comparison is in the
[scorecard](multi-gpu-training-comparison-20260929.md). Select matched
available snapshots only where actual files/configuration and candidate
support permit; growth and phase peaks remain **unverified**. Do not shorten
`num_iterations` to manufacture a window: preserve its original schedule
and stop cooperatively after fixed work.

### Next benchmark sequence

1. Freeze a kitchen reference from the same source state, image/face/pixel
   workload and active-state semantics; record effective optimizer updates,
   exposure and frozen appearance/topology in each arm. Compare a one-device
   control, independent two-device jobs as a distinct reference, and B1's
   two owned/context workers without calling equal images equal optimization.
2. Repeat three independent matched unprofiled warmup-plus-fixed-work windows
   per compared state/device configuration from the same checkpoint (20 warmup
   + 200 measured steps is the existing starting pattern, not a pass threshold).
   Capture both workers, slower-worker wall and aggregate images/pixels per
   second, memory and interval spread; diagnose with instrumentation separately.
   Do not infer a per-step p95 from window sums.
3. Screen O1 CPU-authoritative bounded working-set viability against actual
   candidate discovery, H2D/D2H, dirty-writeback and device memory costs;
   screen G1 whole-image synchronous ownership against actual supported
   pairwise transfer, gradient return, synchronization and worker-residency
   costs. Start with measured native transport and representative packets,
   derived bounds explicitly labelled as such; implement the smallest viable
   O1/G1 slice for direct fixed-work timing when a bound cannot rank it.
   A traffic-only estimate is not a measured speedup.
4. For contenders, run a **longer predeclared fixed-work, overhead-inclusive**
   comparison with three repeats of each arm from the same source. Total wall
   runs from preparation, partition/candidate discovery, input staging and
   process startup through training, checkpoint/output, merge/readback where
   applicable and validated output. Report cold preparation and amortized
   reuse totals separately where input preparation is reusable. Evaluation
   is optional in a separate column; if performed, report its inclusive total
   for both arms. Keep steady-window throughput distinct. Compare total wall
   first and throughput/memory with consistent exposure/pixel/active-state
   semantics; no arbitrary 20% gain threshold. Report measured, derived and
   unverified cells distinctly.

The historical B1 ledger measured about **2.00× aggregate image-exposure
throughput** over three 400-image windows and **18.0% less short total wall**
in one overhead-partial sample. The older-source three-repeat 1,200-exposure
reuse-only comparison is also retained. A **different regenerated step-10k
source** now has its own three-repeat cold/reuse control/B1 totals, actual
measured pixel exposure and sampled worker host RSS in the
[scorecard](multi-gpu-training-comparison-20260929.md): median control
427.351 s vs B1 337.416 s cold / 232.915 s reuse. G1 synthetic non-FPBO
three-step parity, native source-FPBO nonfused gradient-only replay and
completed pattern staging are diagnostics, not source-FPBO owner training.
O1/G1 training wall and simultaneous driver VRAM peaks remain unverified.

### Short runtime decision before another full train

Historical X4 feasibility screening used the retained 3M/6M windows, a
15-minute candidate budget and a provisional 20% median-step signal. That
was **not** a measured kitchen performance gate and is superseded here by
the repeated fixed-work comparison above. A short replay establishes neither
full convergence nor time-to-quality.

### Workload-specific diagnosis, not intake

The supplied kitchen workload alone starts this comparison. Add another
workload only if a ranking cannot answer the deployment question. Diagnose
input wait with cache/decode timing, scratch with a large view or changed
resolution, persistent-state capacity with active/reserved row peaks, and
block/offload viability with measured visibility and active-set bounds.
None of these probes is a universal prerequisite; a fit on one GPU does not
end a split-GPU performance benchmark.

### Reproducible windows

Keep the original schedule horizon, checkpoint identity and effective
configuration, including existing compact storage, quantized moments where
configured, packed projection, split-batch accumulation, aliasing and bounded
prefetch. `_resolve_split_vs_fpbo()` may select FPBO for a single-view
no-op split or disable it for unequal faces/accumulation; record the executed
path rather than requested flags. Match camera/face IDs, pixels and exposure
as closely as the trainer permits. Identify non-replayable sampler,
background, noise and densification randomness; add narrow capture/replay
only if it could change the decision. Instrumented/synchronized diagnosis
must not become the unprofiled speed result.

If a target fails to fit, preserve its failure phase/allocation request and
label fitting-prefix estimates **extrapolated**. Reduced resolution, cap or
SH cannot prove the original target fits, but no such failure is required
to begin the kitchen throughput comparison.

## 4. M0.2 — measure simultaneous residency and time

When a memory bound could change the B1/O1/G1 ranking or a capacity claim,
capture observations around input staging, projection, intersection/sort,
raster forward, loss, backward, optimizer, densification and output.
Include growth allocations before old storage is released and retained
high-water capacity; allocator high-water events may be needed because a
phase-boundary snapshot misses transient peaks. Do not hold up a kitchen
throughput result for a missing full phase-by-phase trace.

For device `i`, use the accounting model:

```text
M_peak,i = max_phase [ P_i(N_reserved) + S_i(phase) + C_i(phase) + E_i(phase) ]
          + separately accounted runtime/driver overhead
```

- `P`: persistent parameters, codec bounds, optimizer state, densification
  state, cameras and trainable appearance/background state, by actual format.
- `S`: scratch allocations retained at that instant, plus current alias arena
  capacity. Record logical live demand separately; inactive unaliased retained
  buffers still occupy memory.
- `C`: input/render-working-set caches and staging/communication buffers,
  including double buffering if a candidate requires it.
- `E`: temporary topology growth, compaction, checkpoint and migration copies.

Classify each physical allocation once. `VramCategory::Splat` includes both
persistent state and transient gradients; the category alone is not a residency
classification. Quantized rows may have several physical backing allocations.
An alias view owns no extra capacity: count the arena once, plus private fallback
allocations during arena growth. See [scratch accounting](vram-splat-x-img.md).

Report `N_active`, reserved capacity, `C * N`, `nnz`, `n_isects`, dimensions,
pool capacity, alias/scratch capacity, and driver/process memory where available.
Honor `MemoryUsage::has_*`: unavailable is not zero. Reconcile pool/backend
allocations with driver figures without adding the same bytes twice. Report
host RSS, pinned/staging peaks and input cache size as separate budgets; shared
memory on an integrated GPU does not create an independent host capacity pool.

If diagnosis needs it, separate input wait/decode, preparation, transfers,
GPU stages, synchronization, topology and checkpoint I/O. Keep critical-path
wall distinct from sums of overlapping work. Only report p95/per-step
extremes when raw per-step samples exist; existing 200-step interval sums
support interval timing, not per-step distributions.

## 5. M0.3 — test visibility-based working-set feasibility

If B1 context or O1 active-set bounds could change the decision, aggregate
native projection/tile results per input view and post-warp face on available
frozen states. Compare a simple spatial split and a visibility-aware
rebalance when partition quality is uncertain. Keep the common scene
coordinates; do not independently normalize blocks.

Use `(checkpoint, topology epoch, row ID)` as the diagnostic identity. Packed
row indices are not stable IDs across densification or compaction. Reduce on
device or sample bounded captures where necessary; do not export an unbounded
`C * N` matrix or retain `ForwardCache` views beyond their pool phase.

For candidates actually being screened, estimate and record relevant rows:

- Unique owned rows per block, foreign visible/contributing rows per view,
  context union size and duplication, and number of owners touched by a view.
- Worst and percentile working-set bytes with actual primitive/SH formats,
  render scratch, owned moments/gradients and context that has no optimizer state.
- Per-block image/pixel coverage, estimated raster/intersection load and maximum
  load imbalance. Equal Gaussian counts do not imply equal training time.
- Boundary/cross-block views, thin structures and large-footprint splats to retain
  for later quality evaluation. Include distant occluders and translucent context;
  a geometric halo or camera-center assignment alone is not sufficient.
- For CPU residency, the active/candidate fraction, consecutive-view reuse,
  unique dirty rows, predicted cache misses, and the cost of finding candidates.
  A scan of an already GPU-resident full scene is only diagnostic evidence, not
  an implementation of an out-of-core candidate query.

Projection visibility is a conservative candidate set, not proof of meaningful
contribution or a safe pruning rule. Distinguish available `accum_weight`
statistics from projected visibility and respect their production semantics.
Do not replace the trainer's culling/occlusion rules to make a trace smaller.

For `3dgut`, account for its world-space raster inputs and matching backward
payload. Do not estimate all primitives from the `3dgs` screen-row byte count.
Collect a separate row for each primitive actually required by the workload.

This probe can reject obviously oversized context or a nearly global active
set. The retained kitchen visibility traces constrain B1 work imbalance,
but are not a capacity proof. Independent-block quality, if needed for
production, requires separate render and train/merge evaluation.

## 6. M0.4 — measure host staging before planning overlap

For B1, measure whole-job partition/input staging where relevant. For O1,
measure same-device candidate discovery, H2D/D2H and writeback; for G1,
measure the actual completed GPU A -> host -> process boundary -> GPU B path
on supported devices in both directions. One GPU per process uses explicit
UUID selection. Host-visible Vulkan staging pointers are not automatically
shared between processes: count every copy, IPC handoff, flush and wait.
Choose representative measured packet sizes within host/device budgets;
reuse buffers, warm up, validate received contents, and separate latency,
bandwidth, packing and synchronization from first-use cost. Test concurrent
traffic only when an otherwise credible schedule depends on it. A file
transfer is not a streaming result, and unsupported pairs stay unverified.

The current Vulkan stream abstraction uses a single compute queue. Establish
serialized transfer+compute timing first; credit overlap only after an actual
concurrent replay proves it. Device-local D2D bandwidth is not inter-GPU bandwidth.
Use native backend APIs and the existing process-launch patterns; the worker's
JSON request/result channel remains control data, not a proposed per-step tensor
transport. A minimal benchmark IPC hop is not a production protocol commitment.

Build candidate-specific byte/latency estimates, where needed, for:

1. CPU-resident state: cache misses, dirty parameter/moment writeback, any dense
   optimizer/regularizer passes and topology state. Count pinned-host capacity.
2. Global ownership: parameters or primitive-specific render packets routed to
   whole-image workers, gradients/statistics returned to owners, shared-state
   synchronization, and topology/migration traffic.
3. Blocks: initial input/context staging, any proposed context refresh, and final
   output collection, amortized over the whole run rather than hidden per step.

Present traffic-only lower bounds and conservative no-overlap estimates
separately from measured completed transfers and training wall. A clearly
noncompetitive traffic/residency bound can screen a candidate without
building it; if close or uncertain, build the smallest correctness-preserving
proof and measure it. No fixed gain threshold or quality floor decides ranking.

## 7. M0.5 — decision table and handoff

| Evidence for this kitchen performance comparison | Next action |
|---|---|
| B1 full-work total wall leads after partition/staging/output, with owned context unchanged and coherent owned updates | Select B1 for further split-GPU work, explicitly as approximate independent optimization; production quality/recovery/capacity remain separate. |
| O1 traffic, candidate discovery and residency are credible and its exact bounded-working-set fixed-work proof leads | Select O1; do not treat a synthetic copy bound as measured training. |
| G1 pairwise transfer, routing and residency are credible and its synchronous whole-image fixed-work proof leads | Select G1; do not credit unmeasured copy/compute overlap. |
| A bound rules out O1 or G1 against measured end-to-end results | Mark screened out with measured inputs and derived bound; avoid implementation for its own sake. |
| Close ranking, unstable repeated runs or missing supported transport lane | Resolve only the specific comparison with a targeted probe; report unsupported rows unverified. |
| One device fits or production quality envelope is absent | Continue the performance comparison; neither fact is a performance stop. |
| No split candidate improves the fixed-work total | Report that outcome and the best measured option; do not invent a capacity or quality claim. |

Retain a local manifest, source/config/checkpoint identity, commands, window
intervals and longer-run repeats, validated outputs, wall/throughput/memory
results and relevant transport/visibility artifacts. Distinguish **measured**,
**derived/extrapolated** and **unverified** cells, including B1's changed
optimizer-update/objective semantics; do not infer per-step distributions
from interval sums. Select the best supported split-GPU option by comparable
fixed-work total wall, throughput, memory/budgets and implementation cost,
explaining any unresolved close call. Missing full phase residency, a
growth-crossing snapshot, held-out quality or an oversized-scene failure
limits those respective claims but does not universally block timing.
The [roadmap's section 10](multi-gpu-training-plan.md#10-benchmark-selection-and-separate-release-gates)
separately governs production quality, recovery and capacity release gates.

### Validation when implementing the probes

Use the dev build entrypoint. On Windows:

```bat
build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_BACKEND_TESTS=ON
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error -j 2
set SS_TEST_DEVICE=uuid:YOUR_DEVICE_UUID
cmake -E chdir build_vulkan ctest -R "^cli_training_smoke$" --output-on-failure --no-tests=error
```

On Linux use `bash build_develop.bash` with the same options and the documented
environment-variable syntax. See [testing](../testing.md) for test registration
and isolation. Set `--disable-viewer 1 --keep-viewer-alive 0` for unattended
training. Use the existing train CLI/configuration path; M0 introduces no assumed
new command-line flags.

The currently implemented native diagnostics use `SS_M0_EXPOSURES=1` to
write `M0_EXPOSURE` step/input/face/dimensions/pixels for actual training
steps. For source-checkpoint **training-view** visibility, set **both**
`SS_M0_VISIBILITY=1` and `SS_M0_VISIBILITY_TRAIN=1` on the native train CLI
with `--resume CHECKPOINT`; the CLI directly evaluates the train split
using ordered `engine_eval_forward(..., input_id)` calls and exits before
training. A mere count of 1,195 trace records does **not** prove 1,195-view
coverage: verify unique IDs exactly equal expected `0..1194` before
building a manifest. `M0_VIS_PARTITION model=finite_row_median` excludes
nonfinite/inactive means only from diagnostic spatial ranking, preserving
source row IDs/storage; it is not actual B1 256-row codec-block ownership.
The separate GPU diagnostic accepts
`g1_native_proof --synthetic uuid:DEVICE_A uuid:DEVICE_B` or
`g1_native_proof --kitchen --resume CHECKPOINT --device uuid:DEVICE_A --images 1188,1165,760`.
The latter is actual-checkpoint **nonfused gradient-only**, not FPBO
owner-update parity. See the [scorecard](multi-gpu-training-comparison-20260929.md)
for its one next source-FPBO parity/performance micro-plan.

CPU checks cover capture schemas/accounting arithmetic where new logic warrants
them. Native GPU checks compare probe-off and probe-on numerical behavior, verify
transfer contents, and exercise the actual primitive/format used in the experiment.
If alias lifetimes change, use `SS_POOL_ALIAS_POISON=1` with the relevant native
checks. Register hardware probes as GPU-only, never `headless`; do not run
NVIDIA/CUDA checks or create new CUDA reference dumps. This document itself
requires documentation/link checks, not a trainer build or benchmark run.
