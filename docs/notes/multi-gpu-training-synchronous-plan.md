# Synchronous same-view multi-GPU training

Status: **closed at Gate 0 (2026-10-16)**. The optimistic backward-only saving
did not exceed the observed native short-control wall spread. Gates 1–5 were
not entered; no partition/IPC executor or new training/evaluation flags were
implemented. See [execution result](#gate-0-execution-result-2026-10-16).

This investigation followed the closed B2 comparison without reopening its
results. The target was faster ordinary-native training without freezing
appearance or topology, changing the logical batch, or accepting weaker quality.
Scope was **two supported non-NVIDIA Vulkan devices on one Windows host**.
Remote hosts, model sharding, distributed forward/loss and other primitives were
outside this implementation. No production speedup or bitwise equivalence was
established. The unentered implementation gates below retain their proposed scope.

## Decision and evidence

Keep one authoritative model and the existing native optimizer. Run full native
forward, loss and appearance reverse hooks there; divide the same raster-backward
work among disjoint microtiles; reduce screen-space cotangents there; execute the
ordinary native projection/FPBO, shared appearance updates and densification once.
The second process is a raster-backward executor, not another trainer.

The closed campaign measured median complete walls of 2614.295323 s for ordinary
native, 1323.576282 s for B2 frozen and 1624.831338 s for B2 refresh1000. Both B2
arms failed the frozen quality rule. The one-GPU frozen causal control took
1337.948733 s, so B2's headline saving is not evidence of comparable multi-GPU
acceleration. Those results remain closed in
[multi-gpu-training-comparison-20260929.md](multi-gpu-training-comparison-20260929.md).

G1 already exposed a numerical trap: tolerable world-gradient error did not imply
identical quantized updates. Its ordered diagnostic repaired a bounded proof,
not unrestricted native training. Both file-staged and later resident-cost G1
runs were noncompetitive. Reuse its proof machinery, not its full-packet schedule
or its paired-image optimization semantics.

## Working rules

- Reuse `<NET>\ds-1104` and
  `<SOURCE> = <NET>\runs\native-src\step-000010000.ckpt`. That source has
  1M splats, 975 training inputs, 108 held-out inputs and horizon 30,000.
  Do not use the later step-10,001 stop-save. No new reconstruction/source run.
- Preserve the original videos and full 4,416-image corpus. Phase A cleanup is
  complete. Wired diagnostics remain useful; this plan does not delete them.
- No private machine/share paths in git. Use symbolic paths here; actual logs
  and outputs go under `<NET>\runs\sync-raster-*` and `<NET>\logs`.
- Before each run, keep at least 10 GiB free on every written volume **after**
  projected output, staging, checkpoint and system-commit/pagefile growth.
  Count retained failures. Check system commit and actual GPU backing too;
  shared memory is not free storage. No new ledgers, receipts or admissions.
- One process per selected GPU. Only the collaborating processes of one
  candidate may overlap; no independent GPU experiment or SfM overlaps it.
  No NVIDIA/CUDA implementation, runs, references or solver work.
- No wall-clock deadlines. A silent process alone is not evidence of a hang.
  A 30-minute no-progress observation triggers inspection of stage, CPU/GPU
  activity, completed work, child status and I/O; terminate only demonstrated
  non-progress or an explicit fatal failure, preserving its log and stage.
- `SS_WORKER_CONTROL=1` requires a launcher-owned, held-open stdin pipe.
  Use `reference/python/stop_at_checkpoint.py`; never shorten the 30,000-step
  horizon to stop. Clear all `SS_B1_*` settings, visibility/debug modes and
  candidate flags before ordinary controls/evaluation.
- One integration owner commits each working step locally after
  `bash tools/check_private_paths.sh`. No push or PR. Evidence is the source
  commit, standard build command, run log and output directory.

## Numerical and state contract

### Partition and reduction

The unit is a complete **8×8 microtile/workgroup**, with its original camera,
microtile coordinates, macro-bin lookup and full depth-ordered contributor range.
Backward currently uses 32 threads with two pixels per thread. Macro bins span
`8 << macro_log2` pixels; preserve all supported macro sizes and native cutoff
indices. Never divide a pixel's depth sweep or renumber intersection positions.

Raster backward returns **10 fp32 cotangents per projected screen row**: xy2,
conic3, opacity1, depth1 and RGB3. Sum these before native projection/SH VJP,
regularizers, quaternion tangent projection, Adam, block-bound reduction and
encoding. Do not sum packed codes, independently updated parameters or moments.
Do not divide by worker count: native cotangents already contain the original
batch/loss normalization. Native quantization runs once over original 256-row
blocks, retaining stable IDs, bounds, clocks and partial-block behavior.

Density accumulation follows the native operator: max for Max, addition for Sum,
and separate numerator/denominator sums for Avg, followed by one division with
native zero-denominator handling. Preserve per-field finite filtering at the
contribution, not at the final sum. Reject nonfinite reduced outputs; do not clip
or substitute zeros. Empty work returns explicit zero. A zero-data-gradient row
still takes the native moment/regularizer/codec update.

Screen-row sum payload alone is `40 × projected_row_count` bytes. Inputs,
statistics, staging and synchronization are additional. The existing six-array
world-gradient capture costs 236 MB per SH3/1M-capacity set; use it as a proof
oracle, not the default transport. Capture writes active rows only: initialize
or exclude unused capacity tails before comparisons.

### Full logical batch and single authority

Input batch one can still contain multiple equal-size face cameras. Preserve
every camera, pass, loss weight and native normalization. Place the seam at each
native raster-backward invocation, not around independently averaged views.
The first supported lane is the current source's resolved packed 3DGS/FPBO
configuration and its supported codecs. Gate 0 must verify it rather than force
FPBO, disable faces, change split-batch or convert the checkpoint. Reject
unsupported primitives/layouts/platforms/configurations before training starts.

Authority owns sampling, camera/row mappings, all forward caches and cotangents,
projection, parameters/moments/bounds, background/bilagrid/PPISP, color-shift EMA,
radii/density statistics, topology and clocks. Workers need no world-model or
optimizer replica. They receive raster inputs and return raster results only.

### Rounding is an unresolved gate

Two local fp32 sums followed by addition need not equal the ordinary native
atomic accumulation. Mathematical same-batch/one-update semantics is **not** a
claim of identical floating-point or quantized state.

Reuse the existing strict screen replay and canonical-state oracle: **zero
differing bytes** for a claim of exact-native-update equivalence. Existing world
data-gradient/render/loss tolerance
`abs(a-b) <= 0.0003 + 0.003 * max(abs(a),abs(b))`, with finite pairs, is diagnostic;
it does not qualify screen sums, decoded moments or packed-state differences.
No new epsilon, relaxed byte check or quality waiver may hide a failed gate.

## 0. Baseline, applicability and speed ceiling

Build through `build_develop.bat -DSS_BACKEND=vulkan`, then run:

```text
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error
```

Fix supported failures before proceeding. Inspect the source's resolved primitive,
codec, batch/face and FPBO settings. Record the two resolved device UUIDs/vendor
and reject the same physical device selected twice.

Profile a 100-update native continuation, source 10,000 to checkpoint 10,100:

```bat
set SS_PROFILE=1
python reference/python/stop_at_checkpoint.py <NET>\runs\sync-raster-profile 10100 ^
    build_vulkan\spirula.exe train --device uuid:A ^
    --data <NET>\ds-1104 --data-format colmap --resume <SOURCE> ^
    --output-dir-prefix <NET>\runs --output-dir-name sync-raster-profile ^
    --num-iterations 30000 --steps-per-save 100 --save-full-checkpoint 1 ^
    --disable-viewer 1 --keep-viewer-alive 0
set SS_PROFILE=
```

The launcher may save a later stop checkpoint; use the explicit target. Log the
actual completed steps and full wall, not an assumed exact stop count. Separately
run three unprofiled 100-update controls with distinct output names, otherwise
identical settings. Profiling is diagnostic, never a final timing arm.

Report native forward/binning, loss/appearance, raster backward, projection/FPBO,
shared optimization/densification, transfer and wait costs. Inventory actual
worker buffer bytes, worst observed rows/intersections and peak memory/backing.
GPU timestamp sums are not necessarily critical-path wall; report query exhaustion.

Compute the optimistic ceiling
`T_ideal = T_native - T_raster_bwd + T_raster_bwd/2`, using nonoverlapping stage
costs. It excludes communication and load imbalance. **Go** only if the resolved
source is supported and even this optimistic saving can exceed the observed
short-control wall spread. Otherwise close the backward-only decomposition
without implementing transport; retain the profile for the next design decision.

Deliver: build/headless result, native ms/update, raster-backward fraction,
optimistic ceiling, byte/memory inventory and local commit.

### Gate 0 execution result (2026-10-16)

**Outcome: closed by the specified ceiling rule.** Tested source was committed
locally as `007957a9`, based on `750cd7ad`; the only runtime addition was
profile-only raster tensor metadata. The executable was built before that
commit and reports `750cd7a`. Both native builds used
`build_develop.bat -DSS_BACKEND=vulkan` with MSVC 19.44.35229.0 Release and
Slang 2026.12.0.1; the existing tree had GUI and patented video disabled.
The post-change headless suite passed **17/17 in 13.12 s**. Native runtime
smokes passed separately with each selected UUID.

Fresh `vulkaninfo --summary` resolved two AMD Radeon AI PRO R9700 devices,
vendor `0x1002`, device `0x7551`, driver 32.0.31036.15 / 26.Q3:

- A: `uuid:00000000020000000000000000000000`
- B: `uuid:00000000010000000000000000000000`

The actual source `state.json` verified full step 10,000, 1M rows/capacity,
packed 3DGS, active FPBO, `joint_u_log_sqrt_g2_v1`, 256-row blocks,
SH optimizer/value bits 8/16, non-SH optimizer bits 16 and per-row bias clocks.
The unchanged source config retained horizon 30,000, fraction split, 975
training / 108 held-out inputs, bilagrid/PPISP and active densification.
Input batch resolved to one; startup reported 975 cameras after splitting.
All 101 observed raster calls had one camera, with 2880² or 3840² pixels,
actual macro-log2 1/2/3, no median and no distortion lanes. The profile
contained 101 ordinary backward and 101 FPBO calls, without a timestamp-pool
exhaustion warning. No source layout, face policy or native optimizer was changed.

All four continuations used the command above, the same explicit 10,100 target,
100-step full-save cadence and held-open stdin stop launcher. Each published
a readable full 10,100 checkpoint, then a full stop checkpoint at 10,101:
**101 actual updates**, with cooperative exit 42 in every case.

| Arm | Complete launcher wall, s | Training elapsed, s | Training ms/update |
|---|---:|---:|---:|
| Profile, diagnostic only | 68.210977 | 36.176 | 358.178218 |
| Native short 1 | 62.260272 | 35.421 | 350.702970 |
| Native short 2 | 68.066282 | 35.379 | 350.287129 |
| Native short 3 | 63.087702 | 35.853 | 354.980198 |

Native complete-wall median/range was **63.087702 [62.260272, 68.066282] s**;
spread **5.806010 s**. Median native training cost was **350.702970 ms/update**.
The launch boundary includes load, initial/target/stop saves and process exit.
The outer resource observer recorded 62.326907 / 68.131990 / 63.154748 s
for the controls; its own overhead is not substituted into launcher walls.
The profile is excluded from control timing.

Disjoint groups of printed GPU-entry totals, over the full profiled process:

| Work | GPU ms | ms/actual update |
|---|---:|---:|
| Forward projection, raster and binning/sort/scan | 871.112 | 8.624871 |
| Raster backward | 6996.249 | 69.269792 |
| Projection reverse / FPBO and camera-ID bounds | 484.918 | 4.801168 |
| PPISP image backward | 13196.172 | 130.655168 |
| Other loss and appearance | 5233.577 | 51.817594 |
| Shared optimization and densification | 218.543 | 2.163792 |
| Image byte conversion | 33.901 | 0.335653 |

The 52 entries cover 9,010 timestamped dispatches. Their rounded sum is
27,034.472 ms; the unrounded aggregate prints 27,034.474 ms. Runtime source
uses BOTTOM/BOTTOM timestamp brackets, not the obsolete TOP-start description.
These are GPU-work costs, not additive CPU critical-path intervals. Transfer
wall was 1,331.714 ms (upload 950.491, readback 380.931, D2D 0.293);
CPU device waits were 21,755.481 ms, memset 1.831 ms and full-process host
residual 45,079.179 ms. GPU execution can occur outside CPU wait scopes:
the printed −5,278.993 ms queue residual is not physical negative latency.

Raster backward was **25.878991% of timestamped GPU work**, or **19.339476%**
of profiled training elapsed. Granting ideal halving with no transport,
imbalance or added waits saves at most **3.4981245 s / 34.634896 ms per update**.
Substituting into profiled training gives 32.6778755 s; applying the same
optimistic saving to native median complete wall gives **59.5895775 s**.
The saving is **2.3078855 s smaller than the observed control spread**.
Gate 0 therefore fails even granting that optimistic subtraction. This is
the plan's screening decision, not proof that every longer run would be slower.
The dominant measured kernel was serial-authority PPISP image backward, not
raster backward; no forward/backward or appearance redesign was added.

Observed cardinality and logical-buffer inventory:

| Quantity | Maximum observed bytes/count |
|---|---:|
| Projected rows | 966,037 |
| Intersections | 10,459,280 |
| Screen input or returned screen cotangents, `40 * rows` | 38,641,480 B each |
| World-row mapping, `4 * rows` | 3,864,148 B |
| Flatten IDs, `4 * intersections` | 41,837,120 B |
| Tile offsets | 230,400 B |
| Active pixel inputs, `36 * pixels` | 530,841,600 B |
| Avg density numerator/denominator output, `8 * world_rows` | 8,000,000 B |

Active pixel inputs are native transmittance, last IDs, rendered depth,
post-hook RGB/depth/transmittance cotangents and the density weight map.
RGB render storage adds 176,947,200 B at 3840² but is not read by the
no-distortion specialization. Summing actual same-call shapes gives a maximum
**605,276,972 B active input** and **46,641,480 B result** (their maxima occur
on different calls). These are authority-side logical raster buffers and
prospective executor payloads, not an implemented IPC packet or measured
transfer volume. Optional lanes remain absent, not fabricated zero inputs.
The existing ordered diagnostic would need up to 6,167,666,688 B of partials
over these calls, plus sort arrays/scratch; it was not run.

Profile pool capacity was 2,197.88 MiB including the 165.21 MiB alias arena;
owned backing peaked at **2,461,713,824 B**, including device backing
2,393,556,128 B, pinned 256 B, staging 67,108,864 B and params 1,048,576 B.
Do not add backing to pool capacity. Sampled local budget usage was
2,436,345,856 B; the report's device-wide usage was 3,237.58 MiB.
Across all four runs the sampled process working-set peak was 1,768,701,952 B,
private bytes peaked at 3,798,319,104 B, and system commit peaked at
23,659,786,240 B against a 57,508,384,768 B limit.
Minimum free storage stayed above 25.37 GiB local / 170.55 GiB output,
including retained checkpoints. Samples are not guaranteed physical VRAM,
whole-process private-byte or system peaks. No worker mapping was allocated.

Evidence remains outside git under `<NET>`: `logs/sync-raster-profile.log`,
`logs/sync-raster-native-short-{1,2,3}.log`, each `-resources.jsonl` and
`-summary.json`, `logs/sync-raster-gate0-summary.json`, build/headless/device
logs, and corresponding `runs/sync-raster-*` output directories. Full source,
target and stop checkpoints remain preserved. No training processes remained
after the batch. Gates 1–5, numerical equivalence, candidate quality/timing
and finalist recovery are **not reached**, not passed. Native stays default.

## 1. Single-GPU partition proof before IPC

Add a narrow backward microtile-range interface to the existing Vulkan launcher
and shader. Preserve native default dispatch/math. Validate checked products,
range coverage and global coordinates; fold dimensions where needed to respect
65,535 dispatch limits. Do not hand-edit generated declarations.

Extend existing `raster_bwd_parity` fixtures for same-Vulkan full versus partitioned
execution: two cameras, image edges, empty work, complementary cuts, macro sizes,
optional distortion/median/statistics lanes and partial codec blocks. No fresh
CUDA references. Zero-work buffers must not retain previous results.

On one fixed **complete current-source logical batch**, run native forward/loss
and appearance reverse once, retaining identical raster inputs/post-hook
cotangents. Warm lazy allocations before diagnostic snapshots; do not use
layout-fixed snapshot restore across densification or allocation-schema changes.
Compare two ordinary unsplit replays, two complementary partition cuts with
declared local-sum/reduction order, and a zero-work-worker case. For each:

1. Verify exact source/batch/pass/face/row identities and exhaustive disjoint tiles.
2. Compare per-contribution bits using ordered diagnostics; compare screen rows
   and density numerator/denominator/max outputs.
3. Capture all six pre-regularizer world data gradients.
4. Apply the existing native projection/FPBO update once from identical state;
   compare parameters, moments, raw codes, bounds and bias clocks. Also decode
   parameters/moments with native codecs and report maxabs/relRMS/ULP differences.

Ordered G1 currently refuses `I != 1`. Do not remove the guard: multi-camera
screen rows can share a world row, and its plain statistics stores would race.
If this batch has multiple cameras, extend the **diagnostic** to fold screen
outputs by screen ID and statistics by world ID. Do not discard faces to pass.
Bound its scratch (`4 * intersections * 4^macro_log2 * (10 + density_lanes)`
bytes plus sorting); it is not an unrestricted production default.

**Go** only on the strict native screen/canonical-state checks and exact discrete
invariants. If ordinary native replays themselves are unstable, report that
baseline limitation: do not call a failed candidate comparison a pass. If local
sums fail, preserve the first screen/world/quaternion/codec divergence and stop
resident integration. A deterministic ordered production reduction is a separate
explicit design/cost decision, not a transport tweak or automatic tolerance change.

Deliver: runnable regression, full-batch one-update comparison, first divergence
if any, scratch/byte cost and local commit. No long quality runs at this gate.

## 2. Resident executor and complete-step failure safety

Implement one persistent child on GPU B; GPU A remains the authority and handles
its tile range. Use the existing `train`/`TrainerSession`, not a second driver.
Add an app-level `--raster-worker-device <selector>` alongside existing `--device`
selection; absent means unchanged ordinary native. Log resolved UUIDs and mode.
Expose persistent execution as a narrow hidden `spirula worker --raster-backward`
mode without changing the scheduler's existing one-request worker behavior.
These options are **planned, not available commands yet**. Localize their help
and human messages through existing i18n conventions.
Distributed mode must refuse inherited B1 ownership/freezing and incompatible
debug overrides, not silently depend on the caller having cleared them.

Reuse subprocess ownership and the bounded mapping/event mechanics from
`Subprocess` and G1. Start with Windows resident CPU-shared mappings, **not**
peer-GPU/zero-copy claims, tensor files, per-step process startup or a generic
transport framework. Retain explicit D2H, map copies, H2D, reduction and waits.
One outstanding batch/pass request suffices. Reuse owned buffers; grow only at a
quiescent boundary with checked capacities and resource-floor checks.

Each message carries protocol/version, attempt, logical step, pass/camera mapping,
range, dimensions/macro size/specialization, row/intersection counts and checked
buffer extents. No process-local Vulkan address crosses IPC. Reject stale,
duplicate, incomplete or incompatible work before update. Publish results only
after GPU writes/copies complete; hold mapping ownership until consume completion.

Worker inputs include packed screen rows, world-row mapping for statistics,
original tile offsets/flatten IDs, render transmittance/last IDs, required raster
outputs and post-hook RGB/depth/transmittance/optional cotangents. Enumerate the
exact enabled lanes from native flags; a missing optional lane is not zero by fiat.
Finish reduction/native screen consumption before pool alias reuse: `RasterBwd`
screen storage aliases the intersector arena, and tensor copies are shallow.

The logical step commits only after **all** native optimizers and densification,
device completion/error checks and completed-step publication. EMA, sampler,
radii and appearance preparation can mutate before Gaussian Adam; “worker failed
before Adam” is not rollback. On any fatal child/transport/device/update failure,
fail-stop the attempt and never publish/checkpoint partial live state. No
in-process retry or fallback. Resume only a separately qualified completed
checkpoint. Clean stop requests wait for a complete step; failure must not enter
an ordinary stop-and-save path.

Wait on completion or child exit, not a fixed timeout. Do not inherit G1's
300-second waits or subprocess forced-stop grace as a training deadline. Own and
reap the child; root exit/EOF must not leave an orphan. Preserve the held stdin
pipe if worker control is enabled; work metadata uses resident control, not the
trainer's STOP/CANCEL byte stream.

Also add a narrow app-level `train --eval-only 1` route to existing
`TrainerSession::eval()` after loading a native endpoint checkpoint. It must not
train, alter the horizon or select training images. The existing visibility
shortcut selects the training split; do not use it as held-out evaluation or
manufacture B1 merge metadata for native checkpoints.

Before Gate 3, replay Gate 1's identical captured full-batch inputs and starting
state through the actual resident GPU-A/GPU-B request, transfer, reduction and
native-update path, including the two-camera fixture and empty-worker case.
Require the same strict screen/statistic/canonical-state checks. Any failure
keeps timing and quality gates closed; a single-GPU partition proof is insufficient.

Deliver: repeated real two-GPU steps with native appearance/topology active,
partition and refusal regressions, child-loss/no-partial-save proof, clean default
native path and local commit. Build/headless plus actual runtime smoke required.

## 3. End-to-end cost gate

Run three matched short native and candidate continuations from `<SOURCE>`,
100 updates each, unprofiled, with identical save/stop policy. Include a one-GPU
partition-path control to separate refactoring cost from the second GPU's benefit.
Exercise real multi-camera passes and an actual native densification/appearance
transition; add a bounded transition smoke if the 100-step window misses one.
Record logical updates, exposure/pixel work and unchanged native sampling policy;
fixed-input numerical comparisons replay identical captured work. Do not reuse
G1 paired-image timing as this schedule's control.

Report launch-to-exit wall, training ms/update, local/remote raster time, every
transfer/map/reduction/wait cost, payload bytes and observed host/GPU/backing peaks.
Choose the deterministic tile cut from native intersection workload; freeze it
and the implementation before final repeats. No batch enlargement or stale step.

**Go** to long comparisons only with Gate 1 intact and candidate complete-wall
median below ordinary native with nonoverlapping observed short-run ranges.
Otherwise close this decomposition and record the critical-path bottleneck.
A kernel-only win or improvement over a deliberately slower partition control
is insufficient. Deliver timing/resource table and local commit.

## 4. Final ordinary-native quality and timing comparison

Freeze the implementation commit. Run **three fresh ordinary native controls and
three candidate runs**, each from `<SOURCE>` to explicit step-20,000 checkpoints,
horizon 30,000. Same named split, logical batch, native sampling/weights,
checkpoint cadence (2,000), full checkpoint setting and stop launcher. Keep
appearance, topology and ordinary updates active. Omit the new worker flag for
controls; the old one-GPU frozen B2 arm is not a baseline. Preserve all failures;
replace invalid runs from the common source, never splice a partial trajectory.

After Gate 2 implements the flags, use this command template with fresh names:

```bat
python reference/python/stop_at_checkpoint.py <NET>\runs\sync-raster-<ARM>-<N> 20000 ^
    build_vulkan\spirula.exe train --device uuid:A ^
    --data <NET>\ds-1104 --data-format colmap --resume <SOURCE> ^
    --output-dir-prefix <NET>\runs --output-dir-name sync-raster-<ARM>-<N> ^
    --num-iterations 30000 --steps-per-save 2000 --save-full-checkpoint 1 ^
    --disable-viewer 1 --keep-viewer-alive 0
```

Append `--raster-worker-device uuid:B` only for the candidate. Complete wall starts
before launch and ends after final native checkpoint publication, all worker
shutdown and process exit. Include loading, compilation/startup, transfers,
coordination, saves, reduction and readback. Record any extra stop step and charge
it equally; a mismatched completed-work endpoint is not a valid timing pair.

Evaluate the explicit step-20,000 native checkpoint from each run separately:

```bat
build_vulkan\spirula.exe train --device uuid:A --eval-only 1 ^
    --data <NET>\ds-1104 --data-format colmap ^
    --resume <NET>\runs\sync-raster-<ARM>-<N>\step-000020000.ckpt ^
    --output-dir-prefix <NET>\runs --output-dir-name sync-raster-<ARM>-<N>-eval ^
    --disable-viewer 1 --keep-viewer-alive 0
```

Use a clean environment, no raster worker and no B1/visibility settings. Verify
all 108 named held-out inputs/faces, identical metric keys and finite values.
Evaluation is outside every timed arm. Do not change the frozen rule:

- Six metrics: PSNR, SSIM and L1, each raw and color-corrected.
- For each metric, candidate **median of run means** and **median of run medians**
  must be no worse than the least favorable native repeat (higher is better for
  PSNR/SSIM; lower for L1).
- For every named `(image, face, metric)`, candidate median must be no worse than
  the worst native repeat. No cross-metric compensation, numerical epsilon or
  post-failure widening. Report every failed aggregate and named-view check.
- Candidate complete-wall median must be lower, with nonoverlapping observed
  ranges. If ranges overlap, run exactly one further batch of three per arm and
  count **all six**. Continued overlap is not an established timing win.

A 0.3 dB screen cannot replace these checks. Old B2 values contextualize the result;
only the fresh controls from this frozen implementation define its envelope.
Quality or timing failure closes this candidate; no automatic cadence sweep.
Deliver all metric failures, median/range walls, resource peaks and local commit.

## 5. Recovery qualification for a finalist only

Only a quality-and-timing finalist advances. Reuse the completed campaign's A5
intent, with native full state rather than merged inference PLY:

- Compare uninterrupted continuation with a late completed-checkpoint stop/resume,
  preserving source, ordered work and endpoint; verify native parameters,
  optimizer values/codes/bounds/clocks, topology, shared appearance and metrics.
- Inventory durable versus transient state explicitly: sampler/PRNG and consumed
  inputs, color-shift EMA **and host steps**, densification/oversize statistics,
  pending appearance flags and worker/pass version. Current full-checkpoint
  naming and diagnostic snapshots alone do not prove trajectory recovery.
- Kill the child before reply, during transfer, after reply/before update, and
  around completed-step/checkpoint publication. Exercise authority interruption
  during native update/save too. No completed checkpoint may contain partial or
  mixed-step state; retain the last independently loadable completed checkpoint.
- Restart with a fresh child and refuse stale mappings/results. Verify stop,
  process-tree cleanup, absence of orphans and resource release. Recover from
  interrupted publication without accepting a partially written checkpoint.

Use native readers/encoders and add only missing recovery-critical state to the
existing checkpoint path; no alternate checkpoint implementation or optimizer.
Compare required exact state with the existing strict oracle; do not waive a
recovery difference because final PSNR is close. Incomplete recovery evidence
precludes **selected**, even when steady-state quality/timing passed.

If recovery requires implementation changes, freeze the resulting commit,
repeat affected correctness gates and rerun the fresh matched Gate 4 quality
and timing comparisons before declaring **selected**. Earlier runs qualify only
their original commit and must not be pooled with the changed implementation.

Deliver reproducible recovery/failure results and a local commit. Do not run this
phase for a quality or timing failure.

## 6. Record outcome and supported surface

Update the comparison note with the tested commit/devices, same-host scope,
complete-wall medians/ranges, all six quality results and failed checks, payload
and memory costs, recovery results, logs/output locations and one outcome:

- **selected**: strict numerical/discrete gates, complete ordinary-native quality,
  full-wall timing and recovery all pass on the stated supported lane.
- **experimental win**: quality/timing gates pass, but recovery is not cleared;
  native remains default and this is not production-ready.
- **closed**: an applicability, numerical, resource, quality or timing gate fails.
  Name the failed gate and retain the evidence; this closes only this
  backward-only decomposition, not distributed Gaussian training generally.

Document the actual CLI, supported/refused layouts/platforms and stop/recovery
behavior in the existing build/testing/backend docs as implementation lands.
No default switch or upstream PR is authorized. Remove throwaway profiling
artifacts after proof, preserve retained failure evidence outside git, and leave
working source/docs committed locally.

If backward-only closes, the next decision uses its measured bottleneck:
full forward/backward tile distribution for a serial-front-end limit;
deterministic contribution reduction for a rounding limit; or spatial/model
sharding for capacity. None is silently added to this implementation, and none
inherits a speed or quality claim from papers or the closed B2 campaign.

## Implementation owners and reuse map

| Concern | Existing owners / reuse |
|---|---|
| Raster range and screen/statistic outputs | `src/backend/vulkan/kernels/RasterizeBwd.cpp`, `src/backend/vulkan/shaders/rasterize_bwd.slang`, `src/shaders/screen_layout.h` |
| Seam, original normalization and full batches | `src/engine/EngineLoss.cpp`, `EngineTrainStep.cpp`, `EngineDataManager.cpp` |
| Unchanged FPBO and codecs | `src/engine/EngineOptim.cpp`, `src/backend/vulkan/shaders/fpbo.slang`, `src/core/Tensor.h`, `GradQuant.cuh` |
| Alias lifetimes and backing | `src/core/PoolSlots.h`, `src/backend/vulkan/README.md` |
| Subprocess/hidden worker/native CLI | `src/app/Subprocess.h`, `Subprocess.cpp`, `src/app/cli/worker_main.cpp`, `main.cpp` |
| Proofs and replay | `src/backend/tests/raster_bwd_parity.cpp`, `src/backend/tests/engine/g1_native_proof.cpp`, `src/engine/EngineDiagnostic.cpp` |
| Canonical recovery | `src/engine/EngineCheckpoint.cpp`, `src/checkpoint/Resume.cpp`, native checkpoint tests |

Keep one edit/commit integration owner. After a gate resolves shared contracts,
independent proof-fixture, worker-control or finished-output metric analysis can
be delegated read-only or with explicit nonoverlapping ownership. Never delegate
concurrent independent GPU jobs. No generic scheduler/transport abstraction or
second training implementation is needed for this plan.
