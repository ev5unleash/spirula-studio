# Split-GPU training comparison

**Outcome: closed (2026-10-16).** On the full-resolution original-derived
subset, both tested B2 schedules were faster than ordinary native but failed
the frozen six-metric, named-view quality rule over three repeats. Median
complete wall was 2,614.295 s native, 1,323.576 s B2 frozen, and 1,624.831 s
B2 refresh 1,000, with non-overlapping observed ranges. B2 frozen failed
3 aggregate and 197 per-view metric checks; refresh failed 8 and 351.
Native remains the default. No finalist recovery run was warranted;
durability, large-scene capacity and production release remain unqualified.

The subsequent [synchronous raster investigation](multi-gpu-training-synchronous-plan.md#gate-0-execution-result-2026-10-16)
also closed, at its native speed-ceiling gate, before partition/IPC implementation.
It did not reopen B2 or produce a new multi-GPU quality/speedup result.

The subsequent [same-view image-strip implementation](multi-gpu-image-strip-investigation.md)
is **complete and experimental**: checked source gradients and lite lifecycle
checks passed, but fixed-input dual training was about **75.4% slower** than the
mean native control. Native remains recommended; staged PPISP is a strong
standalone operator optimization, not a two-GPU speedup. Full-update equivalence,
long-run quality and recovery are not certified; no full suite was run.

## 2026-10-16 synchronous raster — closed at Gate 0

The proposed decomposition kept native forward/loss, appearance, projection,
optimizer and topology on one authority, distributing only same-view raster
backward to a second supported Windows Vulkan device. Tested source was
committed as `007957a9`, based on `750cd7ad`; its only runtime addition was
profile-only raster metadata. The pre-commit executable reports `750cd7a`.
The native Vulkan dev build passed, followed by **17/17 headless tests in
13.12 s** and separate runtime smokes on both AMD Radeon AI PRO R9700 UUIDs
(`00000000020000000000000000000000` and `00000000010000000000000000`).
Driver was 32.0.31036.15 / 26.Q3; Slang was 2026.12.0.1.

The preserved full source at step 10,000 was verified as packed 3DGS with
native FPBO, 1M splats, original 256-row codec blocks and horizon 30,000.
The same 975-training / 108-held-out split, appearance and densification
remained active. One profiled and three unprofiled continuations targeted
the full step-10,100 checkpoint with the existing held-pipe stop launcher.
Every run also saved step 10,101: **101 actual updates**, cooperative exit 42.

| Arm | Complete launcher wall, s | Training ms/update |
|---|---:|---:|
| Profile, diagnostic only | 68.210977 | 358.178218 |
| Native short 1 | 62.260272 | 350.702970 |
| Native short 2 | 68.066282 | 350.287129 |
| Native short 3 | 63.087702 | 354.980198 |

Native median/range was **63.087702 [62.260272, 68.066282] s**, a
**5.806010 s spread**. Raster backward cost **6.996249 s** over 101 updates:
25.878991% of GPU timestamp totals / 19.339476% of profiled training elapsed.
Even free, balanced two-way execution could save only **3.4981245 s**,
below the observed spread by **2.3078855 s**. The screening rule therefore
closes this backward-only decomposition before transport work. Timestamp sums
are not critical-path wall. PPISP image backward alone took 13.196172 GPU s;
the proposed serial front end contains more work than the distributed stage.

Maximum observed rows/intersections were **966,037 / 10,459,280**. Returned
screen cotangents alone reached **38,641,480 B**; full enabled raster inputs
reached **605,276,972 B** and screen-plus-Avg-statistic results **46,641,480 B**.
These are logical source-buffer shapes, not measured worker IPC traffic.
Owned backing peaked at 2,461,713,824 B, sampled process working-set peak
at 1,768,701,952 B, and sampled system commit at 23,659,786,240 B.
Minimum free storage remained above 25.37 GiB local / 170.55 GiB output.
Physical VRAM/private-byte/system peaks were not established.

The plan retains detailed stage, byte and memory accounting. Evidence is
`<NET>/logs/sync-raster-{profile,native-short-1,native-short-2,native-short-3}.log`,
their resource/summary logs, `sync-raster-gate0-summary.json`, build/headless/device
logs and corresponding `<NET>/runs/sync-raster-*` checkpoints. No GPU jobs
remain. **Gates 1–5 were not reached**: no partition/IPC executor, new worker
or eval-only flags, candidate metrics, timing win or recovery qualification.
No new numerical equivalence claim is made. Native remains default.

## 2026-10-16 original-derived subset — closed

### Dataset, endpoint and timing

The approved selection is every fourth **sorted position**, not identical
source-frame numbers, from each of four full-resolution camera folders:
1,104 hardlinks from the protected 4,416-image corpus. CPU bundle adjustment
in both mapping phases produced one component with 1,083 registered images
(98.10%), 286,599 points and 1.258 px mean reprojection error. No rig or
manifest was added. The shared full step-10,000 source has 1,000,000 splats,
horizon 30,000, and a fraction split of 975 training / 108 held-out images.
Every scored endpoint is step 20,000; all 108 named views are unsplit.

Hardware: two AMD Radeon AI PRO R9700 Vulkan devices, driver 32.0.31036.15;
Slang 2026.12.0.1, native Windows Release build. Training and evaluation
used build commit `ce9ab00e`; source generation used `c1fcd4c0`.
The native stop launcher holds an open stdin pipe and preserves the 30,000
horizon. Its later stop-save is not the scored endpoint.

Complete wall includes process launch, source load, training, every save and
refresh, merge and readback, through output publication/process completion.
B2's merge decodes its staged PLY before publication. Evaluation is outside
the boundary for every arm, with B1/worker-control variables cleared first.
Native evaluation resumes its exact endpoint with a separate zero-update
invocation; this does not shorten the timed training horizon.

### Scorecard

PSNR columns are medians of run means. The frozen single-GPU arm is a
one-run causal control, **not** the quality or timing baseline.

| Arm | Repeats | Complete wall median [min, max], s | Raw PSNR, dB | CC PSNR, dB | Quality | Disposition |
|---|---:|---:|---:|---:|---|---|
| Ordinary native | 3 | 2614.295 [2605.292, 2616.196] | 18.953360 | 20.641679 | Defines envelope | Default |
| All-owned frozen control | 1 | 1337.949 [1337.949, 1337.949] | 19.118900 | 20.775652 | Causal control only | Not a candidate |
| B2 frozen, full views | 3 | 1323.576 [1316.037, 1335.371] | 19.027596 | 20.634828 | **FAIL** | **closed** |
| B2 refresh 1000, full views | 3 | 1624.831 [1619.963, 1785.884] | 18.920056 | 20.387788 | **FAIL** | **closed** |

Both B2 arms passed the timing rule: median wall reductions were
49.371585% frozen and 37.848210% refresh, and each candidate's slowest run
was faster than native's fastest. No additional overlap batch was needed.
These are timing improvements, **not comparable-quality speedups**. The
single-GPU frozen control also took 1,337.949 s, so the reduction against
ordinary native must not be attributed solely to the second GPU.

### Frozen quality envelope

The envelope uses only these three fresh native repeats. L1 is lower-better;
PSNR and SSIM are higher-better, raw and colour-corrected. Each candidate's
median of three run means and median of three run medians must be no worse
than the corresponding least favourable native run. Each named view's
candidate median must also be no worse than that view's worst native repeat.
No tolerance or cross-metric compensation was applied. Displayed numbers are
rounded; decisions use the unrounded summaries of the published arrays.

**Median of run means:**

| Metric | Least favourable native run mean | B2 frozen | B2 refresh 1000 |
|---|---:|---:|---:|
| psnr | 18.951683333 | 19.027596296 pass | 18.920056481 **FAIL** |
| ssim | 0.776084593 | 0.778322944 pass | 0.775538991 **FAIL** |
| l1 | 0.075006032 | 0.074205247 pass | 0.075778852 **FAIL** |
| cc_psnr | 20.639723148 | 20.634827778 **FAIL** | 20.387787963 **FAIL** |
| cc_ssim | 0.691217824 | 0.695938324 pass | 0.689002963 **FAIL** |
| cc_l1 | 0.057869132 | 0.058530465 **FAIL** | 0.061867908 **FAIL** |

**Median of run medians:**

| Metric | Least favourable native run median | B2 frozen | B2 refresh 1000 |
|---|---:|---:|---:|
| psnr | 18.653250000 | 18.930300000 pass | 18.764800000 pass |
| ssim | 0.775769500 | 0.784847000 pass | 0.782202000 pass |
| l1 | 0.070881600 | 0.069016500 pass | 0.069077400 pass |
| cc_psnr | 20.648800000 | 20.690500000 pass | 20.456400000 **FAIL** |
| cc_ssim | 0.693908000 | 0.699016000 pass | 0.697166000 pass |
| cc_l1 | 0.053947850 | 0.055324800 **FAIL** | 0.059361300 **FAIL** |

**Named-view failures, out of 108 per metric:**

| Metric | B2 frozen | B2 refresh 1000 |
|---|---:|---:|
| psnr | 24 | 45 |
| ssim | 18 | 43 |
| l1 | 31 | 57 |
| cc_psnr | 40 | 72 |
| cc_ssim | 27 | 47 |
| cc_l1 | 57 | 87 |
| Total failed view-metric checks / 648 | 197 | 351 |
| Distinct views with any failure / 108 | 69 | 96 |

Together with the 3 / 8 aggregate failures, these are 200 / 359 failed
checks per candidate. All **1,320 checks**, including every one of the
**559 failures**, are retained in `<NET>\runs\quality-checks.csv`, with
candidate value, native envelope, delta, metric, scope and named view.
Independent recomputation from the nine metric files reproduced the counts
and verified 108 identical unique identities and 5,832 finite metric values.

### Decision, retained evidence and cleanup

Pilot decision-table row 2 matched for both B2 arms: their raw PSNR was
within 0.3 dB of native and wall was lower. Both therefore received two more
unchanged repeats alongside native. Neither passed final quality, so the
tested schedules are **closed**, not selected or experimental wins. No new
cadence, reconciliation stage, solver or candidate was introduced.
Step 6 is not applicable: there is no finalist that passed both rules.
This does not clear the known longer recovery failure or establish durability.

`<NET>` is the private durable directory recorded outside git. Evidence is
the ordinary logs, outputs and commits, not a new custody/approval regime:

- Source: `runs\native-src\step-000010000.ckpt`; named membership:
  `logs\step2-split.log`.
- Native: `runs\native-{1,2,3}` and each `-eval` output.
- Causal control: `runs\frozen-1` and `frozen-1-eval`.
- B2 frozen: `runs\b2-ctl-{1,2,3}`; evaluations use each `-eval`, except
  repeat 3's confirmed evaluation is `b2-ctl-3-eval-confirm`.
- B2 refresh: `runs\b2-ref-1`, `b2-ref-2-retry`, `b2-ref-3` and their
  `-eval` outputs. Logs are `logs\step3-*.log` / `step4-*.log`.
- The original `runs\b2-ref-2` failed step-18,000 staging PLY writes;
  both workers exited 1 and the group exited 2 after 1,284.611717 s.
  Share free space fell from 30.383 GiB at launch to 0.517 GiB afterward;
  the space loss is unattributed. Last committed generation 17,000 and all
  failure logs/results remain. It was not timed as a successful repeat.
  After capacity restoration, the replacement ran fresh from step 10,000.
- Frozen repeat 3's timed group completed before the controller was
  interrupted. Its untimed evaluation was repeated to observe exit 0;
  the timed training result and configuration were unchanged.

Validation included `build_develop.bat -DSS_BACKEND=vulkan`, **17/17
headless tests**, and real AMD named-view smokes in CPU/disk image-cache
modes (8 faces / 48 finite values each). The Windows nested-helper test
now cancels after `READY`; the checkpoint stop launcher and indexed,
named evaluator are committed and exercised by this campaign.
No NVIDIA/CUDA validation was performed.

Phase A cleanup is complete: source preservation and corpus relocation were
verified, retired artifacts and the unused staging benchmark were removed,
and the last five delete-pending old executables (135,333,888 logical bytes)
vanished after scoped server handle release. No further unused source
scaffold was established. `EngineDiagnostic*`, `ObservationSchurDispatch.h`
and `g1_native_proof.cpp` retain live build/runtime or manual-test consumers
and remain. The native default, protected originals and corpus are unchanged.

### Prospective alternatives — research, not measured results

The closed result rejects the tested B2 schedules, not distributed 3DGS.
No implementation or new experiment was launched for this assessment.
The options and recommendation below are **engineering inferences**, not
local speed/quality guarantees; the completed comparison and its rules remain
unchanged.

Primary architectural references:

- [Grendel, sections 3–4](https://arxiv.org/html/2406.18533v2):
  Gaussian/optimizer-state sharding, pixel partitions, sparse forward/reverse
  communication and dynamic load balancing. Its larger-batch optimizer
  scaling is an approximation, not sequential Adam trajectory equivalence.
- [gsplat distributed rasterization](https://docs.gsplat.studio/main/apis/rasterization.html):
  ranks jointly render a global scene and return gradients to remote
  Gaussian owners. Its [version-pinned implementation](https://docs.gsplat.studio/versions/1.5.3/_modules/gsplat/rendering.html)
  gathers cameras and redistributes projected attributes to image-owning ranks.
- [VastGaussian, section 4](https://arxiv.org/html/2402.17427v1):
  overlapping spatial cells, visibility-based observation selection,
  independent optimization and merge. This is a different optimization
  algorithm, not a synchronous substitute for monolithic native training.

| Option | Purpose | Missing work or limitation |
|---|---|---|
| Synchronous same-view tile work | Preserve the original view/global batch and one logical native update while distributing raster work | Tile-range dispatch, resident exchange, gradient reduction, shared-state/statistic synchronization; no local speed proof |
| Same-global-model multi-view data parallelism | Distribute a fixed global batch's work rather than independently evolve scene blocks | Enlarging a batch-one step changes Adam/sampling semantics and needs a separately evaluated schedule; FPBO cannot simply enable existing subbatch accumulation |
| B2 prefix plus ordinary-native global finish | Test whether a coupled final stage repairs approximation error while retaining some prefix time saving | Production B2 merge is PLY-only, not a full native resume point; optimizer/clocks/codec state must be reunified first; quality recovery is unproved |
| Gaussian plus pixel sharding | Scale model/optimizer capacity as well as compute, following the mixed-parallel design | Sparse routing, reverse reduction, ownership/topology migration and balancing; more work than replicated-model tile distribution |

Native reuse is substantial but incomplete. `EngineOptim.cpp` already
provides `engine_fpbo_capture_data_gradients` and
`engine_fpbo_step_from_data_gradients`, sharing the native FPBO update.
The seam requires initialized packed 3DGS, matching codec/layout/forward
state and `split_batch=false`; it is not a complete shared-appearance or
densification interface. Its six dense fp32 arrays contain 236 MB per
gradient set at SH3 and 1M capacity rows, before caches, replicas and staging.
`B1Merge.cpp` publishes only a bound inference PLY, so an "8k B2 + 2k native"
schedule is not currently a drop-in continuation with retained optimizer state.

The recommended first investigation was native stage/transfer profiling, then
**same-view raster-backward partitioning with one optimizer authority**. Its
native speed-ceiling screen subsequently closed at Gate 0 before transport
implementation; see the completed result above. The broader same-view
image-space strip path was then implemented experimentally, as linked below,
without changing the logical batch or freezing appearance/topology.

Correctness constraints include complete depth-ordered contributors per
pixel, SSIM/multiscale neighborhood semantics, correct normalization,
regularizers applied once, shared appearance updates, densification
sum/max semantics, stable row IDs, codec blocks/bounds and optimizer clocks.
Mathematical equivalence alone does not establish floating-point or
quantized-state agreement. Reuse G1's native seam, state comparisons and
failure replay, but retain its unordered numerical failures and adverse
file-staged/resident-cost timings as evidence. Faster transport alone is
not a numerical repair or a demonstrated speedup.

These papers are algorithm references, not a proposal to add PyTorch/CUDA,
replace the native Vulkan engine, or borrow published hardware speed claims.
Local AMD/Vulkan transport performance and any multi-machine extension remain
unmeasured. Independent spatial blocks are a capacity-oriented alternative
with their own quality objective, not a reason to relax the completed gate.

The [completed image-strip investigation](multi-gpu-image-strip-investigation.md)
records two-GPU Windows Vulkan implementation, exact opt-in/refusals, targeted
source-gradient/lifecycle checks and slower fixed-input training. It recommends
native, not a speedup or full-update/quality/recovery qualification. The earlier
synchronous raster and B2 outcomes above remain **closed**.

## Historical record — not current execution authority

Everything below is historical. Its admission, custody, budget and approval
wording does not govern the completed campaign or override the live plan.

Status: **B2 was performance-selected for both cold and valid reused-preparation
fixed-topology/appearance kitchen work on two AMD R9700 Vulkan devices.
B2-0 through B2-5 are complete; this is not broad production acceptance.**
The [2026-10-01 integrated comparison](#2026-10-01-integrated-b2--selected-cold-and-reuse)
is the historical bounded-throughput selection evidence. The
[master plan's B2 gates](multi-gpu-training-plan.md#5-b2--active-bounded-implementation)
remain its historical reproduction contract.

**Future-evaluation authority (2026-10-07):** The
[selected raw dataset policy](multi-gpu-training-plan.md#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
governs all future evaluations: freshly admitted full-resolution `.insv`
originals, not `.lrv` previews or existing LRV-derived images/masks.
Kitchen and Source v5/readback 2,688/298 acceptance, measurements and protocols
are historical, not admission for the new selection. Fresh input/source/binary
bindings and matched native references are required; execution remains blocked
pending current approval within the master's unchanged execution/custody budget.

Historical PPISP microprices are not full-wall or release qualification;
C2v3 remains unapplied/unadmitted at full3,517,362,683B.
See the [continuation handoff](multi-gpu-training-handoff-20261002.md).
B1 remains a historical lead, not the historical B2 bounded-throughput selection;
no quality/recovery-qualified split-GPU path is settled.

**Evidence availability (2026-10-07):** The user approved retirement of
**41 owners /63 paths /10.829GiB** of selected closed positive G1 timing/
transport raw outputs. Historical measurements, original manifests and command/
exposure/resource ledgers remain unchanged; full raw packet/native-state/FNV
reinspection of those selected results is no longer available. First numerical
failures, mixed failed-child/native-readback source and smoke evidence, B2 base
chains and current custody remain. See the
[exact retention boundary](multi-gpu-training-plan.md#temp-allocation-and-historical-retention--2026-10-07).

**Longer qualification executed:** [Q0–Q4 results](multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results).
Both owners failed the unchanged decoded recovery screen despite exact committed
exposure replay/clocks/frozen-state checks and successful later fault safety.
Matched 132-view raw/color-corrected mean PSNR regressed 1.602366 / 2.534380 dB
against control. Release qualification is withheld; those Q runs did not prove
worker device-allocation enforcement. The selected three-repeat performance
result below remains unchanged, not a quality or longer-recovery pass.

**[D0–D5 causal investigation completed](multi-gpu-training-b2-causal-20261001.md#execution-evidence) on 2026-10-02.**
Actual worker backing is now enforced and verified. Late immediate roundtrips
were exact; both save/load and no-save continuations failed by 64 updates.
At equal per-row updates, B2 mean raw/cc PSNR remained −1.674363 / −2.357914 dB
below fresh native control. Native training is the practical default; B2 stays
experimental. No unique original-Q numerical cause, repaired longer recovery,
comparable-quality speed win or release approval is demonstrated.

**[V0–V5 view-policy ablation completed](multi-gpu-training-b2-view-policy-20261002.md#v5--completed-disposition-and-next-decision) on 2026-10-02.**
On 132 matched held-out views, full-view F improves raw/cc mean PSNR over fresh
partitioned P by **1.048877 / 1.287893 dB**; residual F−N is
**−0.596867 / −1.050402 dB**, regressing **103 / 125** views. Descriptive gap
closure is **63.732684% / 55.078309%** in this one pair, not isolated causal or
population evidence. Native renders show improvement with remaining errors.
Native remains default/B2 experimental; no new speed, quality/recovery/release
qualification replaces historical throughput medians. Context refresh and
justified cheaper-coverage or remaining-trajectory research are prospective
directions only, subject to fresh selected-data admission and current approval.

**Closed G1 evidence:** original proof/full repeats and C0–C4 are executed;
C3 correctness passed, C4 performance failed (5.498× measured / 7.147× complete
wall versus fresh native-resident control). Those rejected G1 schedules and O1
histories remain closed, not active tasks. Materially different source-grounded
candidates require fresh selected-data admission and current approval;
historical rejections are not architecture-wide impossibility results.
The [benchmark contract](multi-gpu-training-plan.md#10-benchmark-selection-and-separate-release-gates)
separates measured throughput from release approval. Exact commands, hashes,
logs and private identities remain in local manifests.

**Closed O1 mechanism continuation:**
[O1 M0–M4](multi-gpu-training-o1-screen-20260930.md#completed-m0m4-continuation--m0-resource-admission-rejected)
executed through M0 and rejected the declared 7 GiB retained-allocation
schedule before implementation; M1–M4 are **NOT RUN**. No next implementation
is admitted for that schedule. A materially different concrete cap/retention
schedule requires fresh selected-data/resource admission and current approval,
not a silent cap increase. O0–O3 remains closed and all
scorecard measurements remain unchanged; O1 speed/capacity and integrated
eviction implementation remain unaccepted.

**R0–R3 completed, not admitted:**
[same-cap retention execution](multi-gpu-training-o1-screen-20260930.md#completed-r0r3-execution--ordered-scratch-only-candidate-rejected)
bound unchanged source/config/native and fresh supported-device/scratch
snapshots; traced owned-slot, transform-view and caller-visible result
lifetimes; specified one independent ordered-scratch release/reacquire cycle.
At 850,000 rows the native padded world is **125,842,760 B**. At 2M
intersections the mandatory backward partials/keys are **6,176,000,000 B**.
The phase-scoped retained-capacity lower bound is **7,801,278,064 B**:
**285,085,296 B over the unchanged 7 GiB cap**, before unresolved additions,
for both 128/256-block chunks. Complete maximum/host/pinned/driver peaks remain
unknown; native default 64 MiB staging also needs reconciliation with 16 MiB
pinned policy. Only scratch after reduction is released; no global/image/arena
savings are claimed. R3 is **CLOSED, NOT ADMITTED**; fresh M0/M1–M4, build,
native replay and timing were not run. Separate local
`o1-retention-20260930T183046Z/retention.manifest.json`, lifetime/schedule/
phase ledgers and arithmetic verifier retain evidence. Original M0 and all
scorecard measurements remain unchanged; no O1 implementation was admitted.

## Longer-run qualification — separate release evidence

The [executed qualification](multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results)
retains one full-horizon control continuation and uninterrupted/recovered B2
outputs from the same step-10,000 source. Each arm committed 20,000 aggregate
exposures and 246,870,006,336 pixels, but control advanced each row 20,000
times versus 10,000 in its B2 owner, with different view weights.
Outer walls were 6,545.854813 s control / 3,242.707497 s uninterrupted B2;
control included automatic final evaluation, so no matched speed ratio is
claimed. Preparation was reused and this is not a fresh benchmark median.

| Mean held-out metric | Control | Uninterrupted B2 | Recovered B2 |
|---|---:|---:|---:|
| PSNR, dB | 20.693150 | 19.090783 | 19.108771 |
| Color-corrected PSNR, dB | 25.467073 | 22.932693 | 22.946175 |
| SSIM | 0.809307 | 0.789371 | 0.789466 |

B2 minus control raw PSNR had mean −1.602366 dB, median −1.170050 dB,
111/132 regressions and worst slot 49 −9.4233 dB. Color-corrected PSNR
had mean −2.534380 dB and 128/132 regressions. Recovery minus uninterrupted
raw PSNR mean +0.017988 dB does not waive the decoded-state failure.
Exact named slot mapping, all six metrics/per-view deltas and native diagnostic
images are retained locally; no valid predeclared boundary subset was available.

STOP saved 15,040/15,002 (exit 42); killing the attempt-bound owner worker
after both passed 19,000 produced exit 2 and sibling-safe saved state.
Explicit resume reached 20,000 each (exit 0). Accepted lineage was exact;
4,032 logged discarded exposure events were charged separately.
Dynamic PLY membership differed, and the retained comparator initially rejected
its output-ID hash before decoding. A separate original-owned-row diagnostic
kept independent native contracts and the same `1e-4` formula; both owners
failed. Exact clocks/unowned/shared state and original nonfinite rows survived.
The numerical cause remains unproved, not excused as approximation.

New outputs remain runnable; final logical/allocated storage was
4,161,428,071 / 4,161,768,600 B and scratch free 18,655,502,336 B.
Maximum sampled native RSS was 6,912,749,568 B. External Vulkan budget probes
did not measure worker allocation, so the 24 GiB worker cap and true device
peaks remain unverified. **Withhold longer-history recovery qualification
and release approval; retain only the already demonstrated bounded throughput
selection with the observed approximation limitations.**

## 2026-10-02 causal disposition — equal-row quality still regresses

The [completed native investigation](multi-gpu-training-b2-causal-20261001.md#execution-evidence)
kept source step 10,000 and horizon 30,000. Fresh all-owned control stopped at
20,000 with a full native checkpoint/source oracle and exactly 10,000 updates,
123,433,689,024 pixels, original row clocks and frozen shared state.
Retained B2 owners also advanced each owned row 10,000 times, but committed
20,000 aggregate exposures / 246,870,006,336 pixels with different context/weights.
This comparison is attribution, not matched-wall speed evidence.

All three models were freshly evaluated with the same current native binary
and 132 named held-out slots. Retained-model six-metric per-view arrays reproduce
the original Q arrays exactly. The raw/cc PSNR means are:

| Endpoint | Raw PSNR, dB | Color-corrected PSNR, dB |
|---|---:|---:|
| Fresh native all-owned 20k | 20.765146 | 25.290608 |
| Retained uninterrupted B2 20k | 19.090783 | 22.932693 |
| Retained native Q control 30k | 20.693150 | 25.467073 |

B2 minus fresh control mean raw/cc deltas are **−1.674363 / −2.357914 dB**,
with 115/132 and 128/132 regressions. Worst raw/cc regressions both occur at
zero-based slot 49: −9.985100 / −7.922000 dB. All six metrics, medians,
pairwise counts and named per-view deltas remain in the causal report.
Update-count difference alone does not remove the regression; context,
view weights and trajectory effects are not separately isolated.

Actual original-owner live states 15,040/15,002 had exact immediate native
save/load and matched replay/reset invariants. At 64 updates, B/C1/C2 decoded
errors were 62,468/59,789/80,442 and 29,378/35,917/33,521. Native no-save
variability is demonstrated, but asymmetric earlier crossings and in-process/
synchronization/timing confounds prevent a unique-cause or repaired-Q claim.
The unchanged original decoded screen remains failed.

Worker-owned actual backing admission now includes all ordinary runtime
categories once before allocation; tiny-cap direct/group native refusal and
normal runs passed. This does not retroactively prove Q resource compliance
or physical peaks. Keep native training default, B2 experimental and release
withheld. Original bounded throughput medians and G1/O1 closures are unchanged.


## 2026-10-01 integrated B2 — selected cold and reuse

**Selection:** three accepted independent repetitions select integrated native
B2 for **both cold and valid reused preparation** on this bounded workload.
The complete-wall ranges do not overlap the native control. No additional
20% threshold applies. Historical B1/G1/O1 rows below are not pooled with these
runs, even where the immutable regenerated source is shared.
Use the [supported native invocation and recovery contract](multi-gpu-training-b1-execution.md#current-bounded-native-b2-path).

**Bound identity and work.** Source `state.tar` remains 380,013,056 B,
SHA-256 `1986EADAEEE4FE3EB86883CCB436797A2D63A01F7ACA9BEF2273EF64EFA507F5`;
config SHA-256 `0AC7B6A45A69F9591082DCF3B601C731758EC334D2C5AAD29DA7DAE5A7CB6FA6`.
Final native binary SHA-256 is
`120FA242BC6ED875A28ED5E3B6E8F754C2DE91AC09810B20A9D094D9F0C4619A`.
Two AMD Radeon AI PRO R9700 Vulkan devices, driver 26.Q3, Slang 2026.12.0.1
and MSVC 14.44 were used; GUI/patent options were off, with no CUDA/NVIDIA.
The unchanged 3DGS SH3 native FPBO quantization-1 source has 1M rows/cap,
3,907 whole codec blocks with a 64-valid-row tail and 7,150 inactive original
means; SH values/moments are 16/8-bit and non-SH moments 16-bit.
The original horizon is 30,000, with 1,195 training and 132 held-out views.

Each fresh native visibility sweep covered exactly unique IDs 0–1194; all
three greedy builders produced the same 532/663 assignment and view SHA-256
`413AB451A5493334B71B8D7BC31C8F3F00A45A4B1D96735AA51F7FE5DF987119`.
Native source/view contracts, clocks, complete owned/excluded coverage and
merge/readback validation were mandatory in the timed B2 path, not optional
private conventions. Three visibility runs exited 0, three controls stopped
cooperatively with exit 42, and three coordinated native groups exited 0.

**Observed seconds.** Reuse is the complete coordinated process through validated
publication/readback; cold adds its freshly executed native sweep and greedy
builder. Range means maximum minus minimum. Independently rounded columns may
not sum exactly.

| Repeat | Native visibility | Greedy builder | Complete preparation | Control complete | B2 reuse complete | B2 cold complete | Control measured 1,200 exposures | B2 measured 600-exposure bottleneck |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 106.285693 | 0.384585 | 106.670278 | 431.141347 | 250.768101 | 357.438379 | 388.987 | 194.970 |
| 2 | 105.767393 | 0.315113 | 106.082506 | 431.199014 | 250.659100 | 356.741606 | 388.988 | 194.939 |
| 3 | 106.959097 | 0.309126 | 107.268223 | 431.416781 | 250.754732 | 358.022954 | 389.219 | 194.958 |
| Median | 106.285693 | 0.315113 | 106.670278 | **431.199014** | **250.754732** | **357.438379** | **388.988** | **194.958** |
| Range | 1.191704 | 0.075459 | 1.185717 | **0.275434** | **0.109001** | **1.281349** | 0.232 | 0.031 |

Derived median cold savings are **17.105938%**, ratio **1.206359×**;
reuse savings are **41.847100%**, ratio **1.719605×**, versus control.
Each control ran 1,302 updates and ended at 11,302; each owner ran 702 and
ended at 10,702. Control measured steps were 10,101–11,300 (1,200), versus
10,101–10,700 per owner (600 each). Each process also performed 101 warmup
and one final update: 1,302 total control exposures versus 1,404 pair exposures.
The **1,200 aggregate measured exposures** are equal, not optimizer clocks or
objectives. The deterministic native B1 sampler repeats within each method;
control and pair do **not** replay equal image-ID schedules between methods.
Native unordered raster remains unchanged in both timed arms.

| Repeat | Control measured pixels | B2 pair measured pixels | Control MPix/s | B2 aggregate MPix/s |
|---|---:|---:|---:|---:|
| 1 | 14,812,089,408 | 14,813,257,536 | 38.078623 | 75.977112 |
| 2 | 14,812,089,408 | 14,813,257,536 | 38.078525 | 75.989194 |
| 3 | 14,812,089,408 | 14,813,257,536 | 38.055926 | 75.981789 |
| Median | 14,812,089,408 | 14,813,257,536 | **38.078525** | **75.981789** |

Maximum paired pixel mismatch is **0.007886315%**. Exposure rates are
derived as 1,200 divided by the corresponding measured interval, not complete
wall; pixels/interval yields the MPix/s above. These are throughput observations,
not equal-trajectory or quality comparisons.

**Resources and cost boundaries.** Windows native process APIs sampled RSS
about every 0.5 s. Aggregate samples include the B2 coordinator and both workers;
per-process maxima are not added to invent a simultaneous aggregate peak.

| Repeat | Control sampled RSS maximum (B) | B2 aggregate sampled RSS maximum (B) | B2 coordinator / two workers' individual sampled maxima (B; not summed) |
|---|---:|---:|---|
| 1 | 974,938,112 | 1,784,266,752 | 64,393,216 / 978,927,616 / 977,408,000 |
| 2 | 975,212,544 | 1,843,576,832 | 64,208,896 / 1,036,988,416 / 977,977,344 |
| 3 | 975,126,528 | 1,784,119,296 | 64,565,248 / 978,010,112 / 978,288,640 |

Fresh initial per-device budget/usage was 33,250,242,560 / 0 B.
Admission caps were 24 GiB accounted per device, 12 GiB aggregate sampled host,
8 GiB new live outputs and 10 GiB minimum free scratch; observed native budget
and resource gates passed. A separate first-four-update profiler observed owner
0 pool+scratch 1,691.40 MiB, driver process snapshot 1,692.13 MiB and physical
device-in-use 2,744.04 MiB; owner 1 details remain in retained logs. This is not a
1,200-exposure-window, simultaneous-device or true-peak proof. Full-scene context
is replicated; **no capacity gain or true host/device/pinned peak is established**.

Complete timing includes contract validation, input/startup, both full native
checkpoints, collection, merge, consumer readback and completion publication.
Cold charges native 1,195-view discovery plus greedy construction; valid reuse
does not repeat that work. OS cache was not flushed. The historical 9,860.617 s
source-generation cost is common retained preparation, not a fresh B2-only cost.
Evaluation was omitted in both timed arms. Separate four-update merged-consumer
evaluation of 132 views took 178.894 s, with PSNR 20.3301 / color-corrected
24.6987: an observation, not a quality floor or timing-arm cost.

Evidence packaging occurred **after** timed validated output. Source-referenced
byte-XOR/gzip private archives retain full native data for proofs and the first
two repeats; every member's reconstructed SHA-256 was verified. Restore archives
before use; third-repeat outputs remain loose and runnable, and immutable
original source/raw PLY bases remain. Packaging is benchmark evidence-retention
cost, not required training finalization or credited speedup. Exact commands,
packaging durations, identities and logs remain in local manifests, with no
private paths or device identities required here.

**Executed correctness and release limits.** Real normal four-update completion
and unequal STOP at 10,002/10,001 followed by resume to 10,004 preserved exact
exposure replay; native decoded parameters/moments/SH met the `1e-4` contract.
Unowned packed state/bounds, shared state and all row clocks matched exactly.
Owned raw bytes are not identical: four diagnostic outliers had maximum
4.05e-4 under a raw-bound comparison, cause unestablished; this does not imply
arbitrary long-run exact recovery. Existing ordered source diagnostics separately
reported zero canonical differing bytes for all-owned/complementary updates over
three low/median/high source views 1188/1165/760; they are correctness reuse,
not the unordered performance comparator.

Post-benchmark verification passed **9/9 full native checkpoints**: control
and both owners in each of the three repeats. First/second-repeat archived
checkpoint bytes were restored before checking; third-repeat live checkpoints
passed the same oracle. Checks verified original-row clocks, exact unowned
packed state/bounds/shared state and complete unique 1M owned/excluded ID
coverage. No timing row was invalidated. This proves those final checkpoint
invariants, not arbitrary long-run exact resumed trajectories or true GPU peaks.

Killed-owner/sibling-safe stop and restart, stale/duplicate attempts, unequal
pairs, current/prior writers and duplicate group refusals passed. Foreign
completion-hardlink refusal preserved bytes and restarted without retraining.
Missing/wrong owner, config, horizon, views and sidecars were rejected, as was
interpreting a merged PLY as a native optimizer checkpoint. Final dev Vulkan
build, 16/16 headless, AMD `fpbo_split_parity` and 1/1 `cli_training_smoke` passed.
Actual merged render/eval and mesh-input consumers passed; full meshing was not
claimed. The prior bounded sidecar/config/view/latest-resume gaps are closed by
this native contract and exercised scenarios, not retroactively by old B1 runs.

Power-loss durability, arbitrary long exact owner resume, application quality/
release, topology growth, oversized or heterogeneous capacity, GUI/remote work,
and a globally resumable merged optimizer remain unproved. B2 is the **named
bounded cold/reuse selection**, not a universal best architecture, globally
exact trajectory or production approval. Native fallback remains available for
other requirements; tested G1 stays closed and O1 parked.

## Historical source: matched longer fixed work (retained, not pooled)

The source was the retained full kitchen step-10k checkpoint: 1M active/cap, 3DGS SH3, packed projection, quantization level 1 and effective FPBO. The original 30k learning-rate horizon, 1,195 training views, full-resolution images and source checkpoint were unchanged. One AMD Radeon AI PRO R9700 Vulkan device ran the all-owned frozen-topology/appearance control; two explicit AMD R9700 UUIDs ran independent, complementary 256-row owners concurrently, each with full-scene frozen render context. The existing frozen view manifest assigned 514/681 views and was reused in all repeats. Every process restarted from the same source. No profiling, held-out evaluation or data transformation entered the timed work.

The predeclared measured work was **1,200 image exposures per arm** after 101 warmup updates per process: 1,200 control updates versus 600 measured updates per owner. Counting warmup and the final update, the control ran 1,302 updates and the pair ran 702 each. These are equal measured image exposures, **not equal optimizer clocks or objectives**. Sampled image IDs and actual pixel totals were not recorded; unequal image sizes and non-replayed stochastic sampling prevent a claim of exactly equal pixel work. The native process interval includes input loading, camera staging, partition setup, training, PLY and full checkpoint publication. The B1 total also includes native merge and readback. Cold construction of the reused visibility/view manifest is **not** included. No method-specific whole-job transfer to a remote machine was needed or measured. All times are seconds:

| Repeat | Control process | B1 pair process (both exited) | B1 merge/readback | B1 reusable-preparation total | Control 1,200-exposure interval | B1 600-exposure bottleneck interval |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 424.409 | 231.791 | 0.595 | 232.386 | 387.709 | 194.917 |
| 2 | 425.283 | 232.054 | 0.606 | 232.660 | 389.839 | 195.281 |
| 3 | 425.982 | 231.736 | 0.588 | 232.324 | 389.870 | 195.078 |
| Median | 425.283 | 231.791 | 0.595 | 232.386 | 389.839 | 195.078 |
| Range | 1.573 | 0.318 | 0.018 | 0.336 | 2.161 | 0.364 |

Derived median reusable-preparation wall ratio: **1.830×**, or 192.897 s (45.4%) less for B1. Derived measured-interval aggregate exposures/s: control **3.078**, B1 **6.151** (1.998×); these are not per-owner update-throughput or quality comparisons. Startup to first completed step was 2.054–2.315 s on the control and 2.493–2.846 s per block worker. The last progress-to-checkpoint event was ~1.1–1.3 s but includes an additional update and context verification; checkpoint I/O is not separately isolated. Evaluation was omitted in both arms, so there is no evaluation-inclusive long total. The earlier **single** short sample including evaluation, but excluding partition construction, was 347.464 s control versus 284.868 s B1; it is a different timing boundary, not a fourth long repeat.

All nine native processes exited through the intended cooperative stop and published full checkpoints, model PLYs and configs. Every owner retained the same unowned parameter/moment/bias-state context hash across each run. Each of the three native merges validated matching source identity and step, complementary masks, complete unique coverage of all 1M included/excluded original row IDs, and read back its published PLY (944,169 / 944,258 / 944,270 valid rows). The all-owned and block paths both replicate the full 1M-row scene on GPU, so **no capacity gain was measured**. Previous profiled middle/late pool and process memory was ~1.8–1.9 GiB per worker, high-water rather than simultaneous phase peak; host RSS, driver peak, usable VRAM and current-run worker peaks were unverified. **At that historical run's end**, free scratch was 1,953,808,384 bytes and further checkpoint-heavy runs were unsafe without space planning; that is not the present disk state. Its then-generated runs and controls were retained at the time, but historical source checkpoints were subsequently removed externally.

For **this historical source**, cold partition discovery, actual pixel totals and current-run host RSS were not recorded. Its 1.830× result applies to reusable preparation on that source alone. The later regenerated source below is a different checkpoint: do not combine repeats or medians across the two.

## Regenerated source: three cold/reuse repeats

**Observed identity and preparation.** Historical step-10k checkpoint files were externally removed; no old merged PLY substitutes for the optimizer state. A fresh native kitchen run regenerated step-10k, step-20k and step-30k checkpoints under the original 30k horizon. An intended step-10k external stop missed its expected state file and the run finished all 30k steps: 9,860.617 s common source-generation preparation (training interval 02:41:19.893), **not a B1-only cost**. The new step-10k `state.tar` is 380,013,056 B, SHA-256 `1986EADAEEE4FE3EB86883CCB436797A2D63A01F7ACA9BEF2273EF64EFA507F5`; config SHA-256 `0AC7B6A45A69F9591082DCF3B601C731758EC334D2C5AAD29DA7DAE5A7CB6FA6`. Native binary SHA-256 `7291AF8422D4CF0CD3B7459B14FA77F7CD57EC0CEDD045D8DBE9F647553FA61A` identifies this experiment binary, not every later rebuild. Revision `8f8c0f5a58c4740824ef5e9d4a335614424e4860` plus retained local B1/diagnostic changes. Both arms restart this **new** source: 1M reserved/current rows, 3DGS SH3, packed quantization 1, effective FPBO, full-resolution disk cache, 1,195 train / 132 evaluation views, original 30k schedule. No evaluation entered timed runs.

Three process-cold visibility traces each enumerated **1,195 unique training IDs, exactly 0–1194** (not just 1,195 records); greedy view assignment was 532/663, with identical manifest SHA-256 `413AB451A5493334B71B8D7BC31C8F3F00A45A4B1D96735AA51F7FE5DF987119` in all builds. The native checkpoint had 992,850 finite means and 7,150 inactive/nonfinite rows; only diagnostic spatial-median ranking excludes the latter. Source rows/storage/IDs remain intact. Finite-row median is a **scheduling proxy**, not the centroid/ownership of actual 256-row B1 codec blocks. A nonfinite-guard failure and a sampler-produced 1,195-record/1,194-unique trace were rejected before these three valid repeats. OS cache was not flushed; “cold” means a newly executed discovery/build, not cold filesystem cache. The control does **not** require B1's visibility discovery.

**Observed seconds**, three independent native process runs per arm. Control complete includes input, startup, training and checkpoint; B1 pair includes both workers and full checkpoints, then merge/PLY readback. B1 cold additionally includes native visibility trace + greedy build. Times are rounded independently, so column sums may differ by 0.001 s.

| Repeat | Native visibility | Greedy build | B1 complete preparation | Control complete = cold/reuse | B1 pair | B1 merge/readback | B1 reuse total | B1 cold total | Control measured 1,200 exposures | B1 measured 600-exposure bottleneck |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 104.167 | 0.333 | 104.500 | 427.162 | 232.290 | 0.626 | 232.915 | 337.416 | 390.579 | 194.766 |
| 2 | 104.316 | 0.292 | 104.609 | 427.382 | 232.650 | 0.544 | 233.194 | 337.802 | 390.410 | 195.232 |
| 3 | 104.469 | 0.298 | 104.767 | 427.351 | 232.004 | 0.559 | 232.563 | 337.330 | 390.916 | 194.675 |
| Median | 104.316 | 0.298 | 104.609 | **427.351** | 232.290 | 0.559 | **232.915** | **337.416** | 390.579 | 194.766 |
| Range | 0.302 | 0.041 | 0.267 | 0.220 | 0.646 | 0.082 | 0.631 | 0.472 | 0.506 | 0.557 |

**Derived:** median B1 cold 337.416 s vs control 427.351 s = 1.267× / **21.045%** less; B1 reuse 232.915 s = 1.835× / **45.498%** less. Do not charge optional B1 view discovery to control or compare an artificial ~531 s control. Each control ran 101 warmup + 1,200 measured + one final update (1,302); each B1 owner ran 101 + 600 + one final update (702). Source exposures are steps 10101–11300 in control and 10101–10700 per owner. B1 workers have independent clocks/objectives; pixels and exposure counts are comparable, not bit-replayed view schedules.

`SS_M0_EXPOSURES` recorded actual input ID, face, dimensions and pixels for every measured exposure. **Observed measured intervals** (no evaluation):

| Repeat | Control pixels | B1 pair pixels | Control MPix/s | B1 aggregate MPix/s | Control exposures/s | B1 aggregate exposures/s |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 14,813,257,536 | 14,811,213,312 | 37.926406 | 76.046195 | 3.07236 | 6.16124 |
| 2 | 14,812,673,472 | 14,811,797,376 | 37.941327 | 75.867672 | 3.07369 | 6.14653 |
| 3 | 14,811,797,376 | 14,814,133,632 | 37.889975 | 76.096744 | 3.06971 | 6.16412 |
| Median | 14,812,673,472 | 14,811,797,376 | **37.926406** | **76.046195** | **3.07236** | **6.16124** |

The largest paired pixel mismatch is **0.015773%** (derived); no replay/equal optimizer-update claim. Exposure rates are derived from 1,200 divided by each measured control or B1 bottleneck interval, and displayed rounded values should not be substituted for the raw interval.

**Observed sampled host RSS bytes**, approximately every 0.5 s; these are per-worker process samples, not simultaneous host peaks or GPU VRAM:

| Repeat | Control | B1 owner 0 | B1 owner 1 |
|---|---:|---:|---:|
| 1 | 1,304,408,064 | 1,337,622,528 | 1,338,294,272 |
| 2 | 1,313,501,184 | 1,356,566,528 | 1,339,072,512 |
| 3 | 1,316,618,240 | 1,311,326,208 | 1,333,059,584 |

All nine workers stopped cooperatively (exit 42), each publishing a full 380,013,056 B checkpoint plus PLY/config; B1 workers also published ownership sidecars. All three merges passed same-source/step and complete unique included+excluded 1M-ID coverage, read back 946,709 / 946,715 / 946,739 vertices. The unchanged unowned context hashes were owner 0 `b0cd4492ca10724`, owner 1 `1c647329b9b784d4` across repeats. Both owners still replicate the full scene: **no measured capacity gain**. Current driver VRAM peaks, simultaneous phase peaks, usable device budgets and pinned-buffer/host aggregate peaks remain **unverified**; historical ~1.8–1.9 GiB pool high-water belongs to the older source. Free C: scratch after fresh runs was **61,744,529,408 B observed**, superseding the historical 1,953,808,384 B snapshot; recheck before any new checkpoint-heavy run.

## Candidate screen on the same scene

Resolved `quantization_level=1` means 16-bit packed SH values, 8-bit joint SH moments and 16-bit joint non-SH moments (`src/app/TrainerCore.cpp`, `src/engine/EngineOptim.cpp`). From the actual 256-row FPBO codec layout (`src/core/Tensor.h`), for 1M reserved rows and SH3 (`K=15`), there are 3,907 blocks and 46,008,832 padded SH cells. **Derived allocation subtotal**, not a process/device peak: world values 148,048,920 B (56 B/row non-SH plus padded 16-bit SH and bounds); non-SH moments 56,312,560 B; SH moments 92,080,176 B; radii, accumulation and per-splat bias counters 16,000,000 B; sum **312,441,656 B (297.97 MiB)**. Other training state, shared appearance, image caches, scratch, growth copies, allocator capacity and runtime overhead are excluded. Do not use this subtotal as a capacity estimate or assume every row is dirty every step.

| Approach | Measured wall / throughput (historical rows remain separate) | Worker and host memory | Setup / communication / finalization | Update semantics / missing proof |
|---|---|---|---|---|
| **B2 integrated native group — historical selection** | **357.438379 s cold / 250.754732 s reuse**, control **431.199014 s**; [fresh 2026-10-01 three-repeat evidence](#2026-10-01-integrated-b2--selected-cold-and-reuse), not pooled with rows below | Sampled aggregate RSS 1.784–1.844 GB; full-world replicas, admitted numeric caps; true peaks/capacity gain unproved | Fresh discovery/build charged to cold; native contracts/full checkpoints/coordinator/merge/readback/publication included | Selected both bounded cold/reuse modes; approximate independent clocks, exercised four-update recovery/consumers; not broad production approval |
| Historical resident all-owned control (2026-09-29 B1 comparison) | **427.351 s cold = reuse**; 3.07236 exposures/s and 37.926406 MPix/s (medians) | Sampled RSS in historical table above; historical ~1.8–1.9 GiB GPU pool high-water is not this run's peak | Startup/input/checkpoint included; B1 discovery not charged | Native one-device FPBO, frozen topology/appearance, three repeats; normal growth-enabled resident timing is historical and not a matched long run. |
| Historical B1 independent owned/context blocks | **337.416 s cold / 232.915 s reuse**; 6.16124 aggregate exposures/s and 76.046195 MPix/s | Sampled RSS in historical table above; each worker holds full world context, no capacity saving, then-current GPU VRAM peaks unverified | Three then-newly timed view sweeps/builds in cold only; startup/input/checkpoints/merge/readback included; no per-step IPC | Approximate independent objective, each owner 702 updates vs 1,302 control; three repeats and validated unique owned output. |
| O1 CPU-authoritative state | **Unverified training wall, throughput and capacity**; accepted 21.3637501 s resident diagnostic is not performance | Observed retained pool + scratch max 8,019,788,176 B, sampled RSS 3,019,374,592 B; semantic-live/true peaks unknown. Derived 312,441,656 B subtotal is not VRAM saving | No coherent bounded discovery/native chunk-update/partial-gradient servicing schedule; O2 not admitted, no timing repeats or fresh speed reference | Six native updates and discarded same-input canonical-byte oracle replays passed; exact poisoned checkpoint reload. All 3,907 codec-pair block unions dirty and 1M clocks changed every update. O1 offload parity remains unverified; first O3 branch stops implementation admission. |
| G1 synchronous whole-image ownership | **5,640.418 s complete** vs its fresh ordered-path **801.617 s resident control** (7.036× slower); 0.245 exposures/s and 3.026 MPix/s. Not directly matched to historical B1 rows. | Full-world replicas; sampled full-run RSS sum max 5,669,163,008 B; separate resource-smoke live-pool samples ~4.84 GB/context; true peaks and capacity gain unverified | Actual native binary-file packet traffic and worker lifecycle included; 2,274,354,841,120 logical staged write bytes/run; native stop/readback passed | Strict source/edge gradient and canonical parity passed. Three complete repeats per method, identical primary pixels, method-local exact repeat payloads; G1 pairs versus sequential control have different trajectories. This staged schedule is not a performance winner. |

### Native G1 diagnostics on the regenerated source

**Observed synthetic parity:** `g1_native_proof --synthetic uuid:DEVICE_A uuid:DEVICE_B` used two explicit AMD devices, 512 3DGS SH3 rows with native non-FPBO values/moment codecs split into two 256-row owners. Three steps passed native renders/lagged loss and float-gradient tolerances; parameters, packed values/moments/bounds and bias counters matched byte-for-byte. Warmup seeded DC momentum on an invisible row; an empty-image worker, overlapping work, unequal image dimensions, always-invisible clock/carried momentum, exact depth ties, and stale/version/source/truncated-packet rejection were exercised. This **does not** test FPBO optimizer state from the actual kitchen checkpoint.

**Observed actual-source diagnostic:** `g1_native_proof --kitchen --resume CHECKPOINT --device uuid:DEVICE_A --images 1188,1165,760` loads the unchanged source-FPBO checkpoint and performs native **nonfused projection VJP** using its actual FPBO SH values, with shared appearance forward frozen, source loss weights, random background and tile-skip disabled; it never updates the optimizer. Full-float finite gradients were scanned in bounded 2 MiB host chunks. The illustrative contiguous owner split is 499,968 / 500,032 rows (whole 256-row codec blocks), **not the B1 spatial mask**. Source FPBO layout cannot be resumed into the standalone optimizer's different SH quantized cell/block layout; a diagnostic resume-layout override was rejected and removed. Three selected views from the **new** all-view trace:

| Input ID | Dimensions; pixels | Projected candidates | Illustrative owner candidates | Tile intersections | Rows with nonzero Gaussian gradient | Zero-gradient rows |
|---|---:|---:|---:|---:|---:|---:|
| 1188 (low) | 3024×4032; 12,192,768 | 14,208 | 7,257 / 6,951 | 220,461 | 14,179 | 985,821 |
| 1165 (median) | 3024×4032; 12,192,768 | 139,742 | 71,035 / 68,707 | 674,501 | 121,697 | 878,303 |
| 760 (high) | 4080×3060; 12,484,800 | 822,825 | 411,480 / 411,345 | 1,930,018 | 744,979 | 255,021 |
| Three-view total | **36,870,336 pixels** | **923,678 unique-row union** (not sum) | — | **2,824,980** | — | — |

The source diagnostic prints `source_layout=checkpoint_native` and
`gradient_path=nonfused`; this is a path label, not an FPBO owner update.
The 1M-row source has 3,907 FPBO codec blocks, with a final **64-valid-row
partial block**. The 512-row synthetic proof contains only full blocks and
does not establish parity for source-tail optimizer updates. The historical
owner-update proof therefore required valid lanes in that last 256-row block.

Actual dense float Gaussian-gradient capture is **236,000,000 B per view**, 708,000,000 B for these three; that is captured native gradient traffic, **not routed packet bytes**. Native world values are 148,048,920 B and total illustrative owner state 152,392,736 B, together 300,441,656 B; adding 12 MB radii/accumulation auxiliaries yields the derived 312,441,656 B subtotal above. Two full world replicas would be 296,097,840 B before scratch/shared state. Neither projection candidate counts nor zero-gradient rows establish an O1 safe writeback skip.

### Source-native FPBO input: Gate 1 passed

The retained step-10k source and config were freshly hashed and match the
identities above. The native Vulkan build uses MSVC and pinned Slang
2026.12.0.1; two freshly enumerated AMD R9700 devices remain supported.
`engine_fpbo_capture_data_gradients` captures the native internal FPBO VJP
before regularizers into six original-row-indexed fp32 arrays.
`engine_fpbo_step_from_data_gradients` consumes already weighted/reduced
gradients through the same regularizer, Adam and native codec update.
No layout conversion, packed-gradient summation or second optimizer is used.

**Observed:** `g1_native_proof --fpbo --resume CHECKPOINT --device
uuid:DEVICE_A --images 1188,1165,760` completed three updates at steps
10000–10002 in **18.246 s diagnostic wall**, not a performance repeat.
Internal-VJP versus external-input updates and resident all-owned versus
complementary 256-row owner updates were byte-identical across Gaussian
values, native packed moments/SH values, bounds, bias counters, statistics
and frozen shared appearance/history. The 3,907th block retained its 64 valid
lanes; source IDs and inactive rows were not compacted. Native render/loss
and float-gradient comparisons use absolute tolerance 0.0003 plus 0.003
times the maximum absolute compared value; canonical state accepts zero
differing bytes. No mismatch was reported.

The final integrated `--fpbo` run also passed in **18.024 s diagnostic wall**.
Its repeated resident captures reported gradient maximum absolute errors
**2.50991434e-7 / 2.07601261e-7 / 3.82750295e-6** across the three updates,
with zero out-of-tolerance values; render errors were zero and loss errors
at most **5.96046448e-8**. Same-input native/external and block-owner canonical
comparisons still reported zero differing bytes.

The loss weights, original 30k horizon and effective packed FPBO quantization
level 1 remain native. Random background and tile skipping are diagnostic
overrides; topology and shared appearance are frozen. Radii/accumulation are
restored before alternative updates. Shared snapshots cover PPISP,
bilateral-grid float/quantized values and Adam/AdaGrad history, background,
and color-shift state; this source has zero color-shift regularizer weight.

The initial **ownership-only** `--fpbo-two-owner` diagnostic also completed
three source updates with byte-identical resident results and unchanged
unowned context, at **50.287 s diagnostic wall** and **2,788,560,824
file-staged bytes per update**. Its gradients were produced by the resident
oracle and sent to both owners; workers independently checked the final
view. This is not distributed whole-image gradient reduction or training
wall acceptance. The whole-image contribution implementation and its failed
numerical gate are recorded below; the earlier ownership-only result must
not substitute for them.

The CPU packet mode passed source/config/attempt/topology/layout/range/
version/step/owner/length/truncation/duplicate/incomplete-publication checks.
Existing `fpbo_split_parity` passed on an explicit supported device.
Initial Vulkan device-local budgets were 33,250,242,560 B each; later driver
peaks, pinned peaks and aggregate host peaks are not inferred from that
admission snapshot. Exact commands, logs and private identities are local.

### Whole-image worker gradients: Gate 2 failed, timing blocked

**Historical failure, superseded by the ordered-raster continuation below.**

The extended source proof obtains native float gradients from independent
whole-image workers, validates source/config/attempt/topology/version/layout/
row-range identities, restores frozen shared/statistic state, and reduces
equal-image-weight contributions in declared order before authoritative
owner updates. It retains full-world replicas explicitly. The independently
rendered resident batch and worker batch must satisfy the unchanged float
tolerances and native canonical-state acceptance; in-tolerance gradients
do not waive differing packed state.

**Observed initial actual-source attempt:** `--fpbo-two-owner` failed on
view 1188 at step 10000, before owner update/publication, after **15.936 s
diagnostic wall**. Across 59,000,000 gradient values, maximum absolute error
was **3.91155481e-8**, relative RMS **5.86964083e-7**, with zero values outside
the declared per-element `0.0003 + 0.003 * max(abs(a), abs(b))` tolerance.
Render maximum absolute error was zero; native loss maximum absolute error
was **5.96046448e-8**. Native internal versus same-input external FPBO
remained byte-identical, as did the worker's frozen canonical context.
Applying independently captured resident versus worker gradients nevertheless
produced **10,100 differing canonical bytes in 17 fields**:

| Native field group | Differing bytes in member order |
|---|---|
| Means / quaternions / scales / opacity / DC values | 22 / 588 / 61 / 38 / 160 |
| Corresponding joint non-SH moment codes | 1,187 / 2,689 / 2,301 / 196 / 533 |
| Corresponding non-SH moment bounds | 576 / 634 / 301 / 324 / 296 |
| Joint SH moment codes / bounds | 11 / 183 |

Maximum finite parameter deviations were respectively **1.86264515e-9 /
1.1920929e-7 / 8.58306885e-6 / 2.38418579e-7 / 1.1920929e-7**.
SH value storage, per-row clocks and frozen shared/statistic bytes did not
report differences. Packed/bounds numeric errors were not computed by this
initial reporting snapshot; its printed zeroes for those fields are not
measurements. Detailed mismatch artifacts belong in the local manifest.

**Observed native FPBO edge attempt:** `--fpbo-edges` uses 576 rows, three
codec blocks and 64 valid tail lanes. Its first completed update passed
process-fresh empty-worker/empty-owner-visibility, carried-history, bias,
valid-tail and unowned-context checks, with **2,177,084 file-staged bytes**
and **624,112 B separate owner native artifacts**. Its second update,
containing overlapping unequal-sized views, failed before owner publication:
reduced-gradient maximum absolute error **2.91038305e-11**, relative RMS
**7.03254129e-9**, zero out-of-tolerance values, but **864 differing canonical
bytes**, including quaternion maximum deviation **0.000997605501**.
This is a failed gate, not a successful three-update edge proof.

The subsequent targeted evidence run retained every differing element and
both gradient/preupdate/native-postupdate snapshots instead of merely
reconfirming the failure. For that separate attempt, view 1188 gradient
maximum absolute error was **1.06869265e-7**, relative RMS **1.13982396e-6**,
with zero out-of-tolerance values; **8,686 canonical bytes / 8,631 typed
elements** differed. The complete local ledger includes old/new codes and
float bounds, plus raw gradients, preupdate quaternion moment codes/bounds,
bias and output values for **453 changed quaternion rows**. Bounds errors
are now measured (scale-moment bounds maximum **0.00035238266**); packed
bytes are labeled non-applicable for float error rather than reported as
unmeasured zeroes. This is retained diagnostic evidence, not a passing gate.

**Inference, not established root cause:** native backward atomic/reduction
ordering can change tiny residual gradients; quaternion tangent projection
and native Adam's `1e-15` epsilon can amplify near-zero differences. Changes
in moment codes/bounds follow the changed inputs. Same-device worker
differences rule out attributing the evidence solely to cross-device
arithmetic. No threshold, optimizer epsilon or source gradient was changed
to make acceptance pass.

**Decision:** retain B1's measured lead. Gate 3 resource/profiling and three
1,200-exposure timing repeats are blocked by the failed Gate 2 numerical
contract. No G1 throughput, capacity, time-to-quality or production approval
is inferred from these diagnostics. The historical next numerical direction was
row-level resident/worker gradient and native-moment replay at the failed
boundary; the completed continuation below records its resolution.

### Completed-boundary safety and final integration

`g1_native_proof --fpbo-stop-check uuid:DEVICE_A uuid:DEVICE_B` passed the
first 576-row empty-worker/partial-tail update, then completed-boundary 2
publication/readback. One next-step child succeeded and another rejected a
truncated packet; no partial next version was published. Stable row IDs and
source/config/attempt/version/layout were validated. Both per-owner native
`state.tar` artifacts were reloaded through `engine_load_checkpoint` after
deliberately poisoning a parameter, with byte-identical canonical readback.
The retained `state.tar`/PLY outputs are per-owner proof artifacts, not a
globally merged resumable checkpoint. This independent safety result does
not waive the failed later numerical case or qualify actual-source stop
after a successful whole-image update.

The first integration's Vulkan build, CPU packet checks and headless suite
passed 14/14. At that point, multiple-image publication and later edge
updates remained blocked. The following continuation completes those
numerical gates; its resource/timing results are recorded separately.

### Ordered native raster: Gate 2 now passed

Two executed probes resolved the initiating divergence without changing
the optimizer: `--fpbo-replay` replayed both retained gradients twice and
reproduced both saved native outputs exactly (**9.050 s diagnostic wall**).
`--fpbo-producer` repeated source view 1188 from the same frozen state:
render and RGB/depth/transmittance pixel cotangents were byte-identical,
but raster screen gradients differed in **78,714 bytes**, maximum absolute
error **0.000427246094**, relative RMS **3.1876936e-7**.

The proof-native raster path now records the existing fragment partials in
unique intersection/microtile slots, stably groups by packed screen ID,
and folds original intersection order followed by microtile y/x order.
It shares the raster VJP and native FPBO regularizer/Adam/codec bodies.
The backend-internal thread-local control is off by default; ordinary
training retains its historical path. Packed/unpacked single-image 3DGS
and empty visibility are supported; 3DGUT is not part of this bounded proof.
Scratch is intersection/subtile-sized, not per-pixel, and appears in the
native pool accounting. The serial segment fold is a proof implementation,
not a performance claim. No epsilon, tolerance or layout was changed, and
no reference-gradient substitution was used.

**Observed after correction:** producer screen gradients repeat exactly;
source-native Gate 1 passed (**16.954 s diagnostic wall**). The actual
whole-image source Gate 2 passed **six updates, steps 10000–10005**, with
single-image low/median/high and three-image reductions
(**191.494 s diagnostic wall**). Captured/reduced gradients, renders,
canonical parameters/moments/bounds/bias and frozen context reported zero
differences; scalar loss maximum observed error was **3.81469727e-6**,
within the unchanged tolerance. The final 64-row block passed. Completed
boundary 10006 native owner checkpoint readback and failed-worker/no-partial-
publication checks passed.

All **three** 576-row FPBO edge updates passed (**2.891 s diagnostic wall**),
including empty worker/owner visibility, overlap, unequal-sized views,
ties, invisible carried history, partial tail and unchanged unowned bytes.
Completed boundary 4 native checkpoint readback and failure rejection passed.
The Vulkan dev build, existing FPBO split coverage and packet rejection
checks passed; the headless suite passed **14/14**, without exclusions.

Strict Gate 2 opened the timing gate. The completed measurements below charge
ordered scratch/reduction and actual worker lifecycle against fresh controls.
Historical control/B1 medians remain context, not silently matched to this
different implementation.

### Fixed-work runner and separate resource evidence

The persistent-worker and resident-control smoke lanes passed exact native
update/merged-state comparisons, final checkpoint reload and G1 truncated
next-version rejection. Full timing uses 101 single-image warmup exposures,
1,200 measured exposures and one final update, with ordinal-modulo-1,195 IDs:
G1 performs 600 measured pairs / 702 total updates; control performs 1,200
measured sequential / 1,302 total updates. The two optimizer trajectories
are explicitly different. Repeated resident gradient oracles, RGB readbacks
and per-step native archives are absent from the full timing loop.

**Separate profiled resource smoke, not benchmark wall:** G1's sampled
simultaneous parent-plus-worker RSS maximum was **6,291,042,304 B**, below
the 8 GiB admission limit. Its complete diagnostic wall was **53.150 s**;
control's was **9.654 s**. These runs contain smoke-only resident/failure
checks and cannot rank training performance. G1's parent packet write /
read-validation totals were **12.972 / 7.421 s**; worker totals were
**7.603 / 13.148 s**. These are completed file-staging phases, not GPU copy
or physical-disk-write timings. Worker state restore/capture/priming was
**7.818 s**, and gradient upload/native update/readback **1.205 s**;
stage sums overlap and must not be added to complete wall.

The local phase/device ledgers and process-exit backend profiler retain
actual payloads, D2H/H2D/synchronization aggregates, working-set samples and
available live-allocation/global-driver estimates. True driver/device and
simultaneous host peaks, pinned-host peak and physical disk write bytes
remain **unknown**. Native owner forwards/loss refreshes are zero: updates
reuse the image stash. Startup priming and smoke-only extra image work are
recorded separately from declared primary exposure counts.

The resource smoke's two worker backend profiles report **14,476,885,144 B
H2D** and **8,303,859,180 B D2H** in total, with **3.785 s** aggregate
transfer-call wall and **4.373 s** aggregate GPU-wait wall. These include
startup/source restore, diagnostic work and readbacks; overlapping process
totals are not extra elapsed time. Derived queue-latency estimates were
negative and are not usable physical latency measurements. Sampled live
pool maxima were **4,836,967,312 B parent**, **4,844,469,088 B worker 0** and
**4,843,104,688 B worker 1**. Reported driver/global usage samples ranged up
to **5,950,709,760 B**; these are observations, not certified device peaks.

### Completed three-repeat native fixed-work comparison

All six fresh subprocesses exited **0**, without profiler switches. Each
performed exactly **1,302 primary exposures**, including **1,200 measured
exposures / 14,811,505,344 measured pixels**. The actual ordinal, phase,
image/face and dimension/pixel manifests match across all six runs.
G1 uses 600 measured paired updates versus 1,200 sequential control updates;
equal primary exposure/pixel work is not equal optimizer trajectories.

| Repeat | Control complete command wall (s) | G1 complete command wall (s) | Control measured interval (s) | G1 measured interval (s) |
|---|---:|---:|---:|---:|
| 1 | 801.617 | 5,654.014 | 733.024 | 4,907.973 |
| 2 | 801.503 | 5,619.337 | 732.899 | 4,875.564 |
| 3 | 801.733 | 5,640.418 | 733.164 | 4,895.042 |
| Median | 801.617 | 5,640.418 | 733.024 | 4,895.042 |
| Range | 0.230 | 34.677 | 0.264 | 32.410 |

Complete outer wall includes process creation/startup through native
checkpoint/readback, worker shutdown, pool reset and process exit. Native
inner complete medians were **801.544 / 5,640.362 s**. Measured aggregate
throughput medians were **1.637 / 0.245 exposures/s** and **20.206 / 3.026
megapixels/s**, control / G1. G1 complete wall is **7.036×** control.
Control startup/priming, warmup and checkpoint medians were
**2.281 / 62.189 / 2.528 s**; G1 common startup, worker startup, warmup and
checkpoint medians were **2.272 / 2.824 / 724.980 / 6.537 s**.

G1 parent packet write/read-validation medians were **1,890.877 /
1,184.375 s**, with **2,382.034 s** child-barrier wall, **76.748 s** CPU
reduction and **36.392 s** owned merge/publication. Two-worker summed
read-validation/write medians were **2,026.802 / 1,171.867 s**; native
restore/image capture/priming and gradient-upload/update/readback medians
were **1,171.594 / 209.074 s**. These overlapping ledgers cannot be summed
into additional complete wall. File staging/validation dominates this
implementation; no hypothetical faster transport is credited.

Each G1 run actually wrote **2,274,354,841,120 logical packet-file bytes**
across parent and workers, and read the same volume. Payloads comprise
**844,241,053,360 B** source native state, **245,991,356,480 B** frozen
stat/appearance context, **422,420,968,336 B** owner native-state return,
**123,083,219,648 B** frozen-state return and **638,616,000,000 B** float
gradients across both directions, plus identity/framing bytes. These are
logical application bytes, not physical disk traffic. Owner refresh
forwards remain **0**. Actual forwards were **1,305 G1 / 1,303 control**:
1,302 primary forwards plus three / one startup priming forwards.

Sampled simultaneous RSS maxima across the full repeats were
**5,669,163,008 B G1 / 1,227,894,784 B control**. Each G1 final output was
about **1.391 GB**, beneath its 10 GiB live scratch/output cap; sampled RSS
stayed beneath 8 GiB. Final observed scratch free space was **43,981,946,880 B**.
True driver/host/pinned peaks and physical disk bytes remain unknown.

All native checkpoint poison/reload checks passed, ending at clocks
**10,702 G1 / 11,302 control**. Canonical/frozen-state/stable-ID payloads
were byte-identical across repeats within each method. Complete packet
hashes differ because attempt IDs are deliberately distinct. Cross-method
final states are not asserted equal. Full timing omits failure injection;
the earlier strict/smoke failure-publication checks remain separate.
Final packet rejection passed and headless coverage passed **14/14**.

The first PowerShell-supervised control completed native work but exposed
a null supervisor exit-code capture. It is retained as diagnostic only,
excluded from the three accepted controls. All accepted commands use direct
subprocess exit codes. Historical **427.351 / 337.416 / 232.915 s**
control/B1 cold/reuse medians remain different-schedule context.


### Completed staging/context cost continuation — C4 failed

**C0–C4 CLOSED, C3 PASS / C4 FAIL.** The
[implementation execution record](multi-gpu-training-g1-implementation-20260929.md#completed-stagingcontext-cost-continuation--c0c4-closed)
contains the reconciled inventory, isolated candidate, all correctness and
failure checks, lifecycle medians and retained reproduction protocol.
Exact commands/hashes/artifacts are local in `continuation.manifest.json`,
`packet-inventory.json` and `bounded-results.json`. Accepted observations remain
historical; selected raw outputs were retired under the availability notice
above. This is a fresh **ordered G1 / native-resident control**
comparison, not a new matched comparison to historical B1 or to the
original ordered full timing above.

C0 matched source/config/binary and all nine original implementation hashes.
Initial scratch was 45.98 GB; both supported AMD R9700 devices reported
driver 26.Q3 / API 1.4.349 and 33,250,242,560 B budget / 0 B usage each,
an admission snapshot only. C1 audited 223 field/direction/operation rows
and 15 actual packets (FNV + SHA-256), reconciling all three original
2,274,354,841,120 B ledgers: remainder 56,160 B identity and 2,187,136 B
framing (5,620 headers and 194,712 length words).

C2 omitted only kind-12 frozen returns, retaining the first 20 mutable
native fields plus an 8 B digest of actual GPU frozen bytes. Parent immutable
authority and source/config/attempt/topology/layout/version/step/owner
bindings require both owner validations before publication; full final
kind-16 state/checkpoint is intact, source refresh unchanged, hash cost
charged. Of 123,083,219,648 B original frozen returns, 175,082,816 B final
return remains. At 1,200 exposures, omitted payload 122,908,136,832 B less
11,232 B new attestations plus 235,872 B framing reduction (29,484 length
words) gives **122,908,361,472 B net logical reduction**, not measured physical
disk savings. The independently reconciled bounded ledger implies
**28,363,468,032 B reduction at the unchanged 120-exposure schedule**.
Final source/config rehashes remained unchanged.

C3 passed the corrected Vulkan build, packet/native FPBO checks, six source
updates on views 1188,1165,760, edge/stop checks, both timing smokes and
native checkpoint readback. The actual altered worker rejected stale
identity/version/digest and missing-owner contributions without publication.
Headless passed **14/14, no exclusions**; 14 invalid timing-option cases
rejected before output. Positive even `--measured-exposures` retains the
full default 1,200, 101 warmup and one final exposure; smoke is unchanged.
Only full control timing admits `--raster resident`; the strict oracle/G1
remains ordered.

All six alternating fresh unprofiled subprocesses exited **0**, matching
**222 primary / 120 measured exposures**, actual image/face/dimension
manifests and **2,706,794,496 primary / 1,463,132,160 measured pixels**.
G1 ran 162 updates,
60 measured pairs, ending at **10,162**; control ran 222 updates,
120 measured sequential, ending at **10,222**. Optimizer trajectories differ;
effective raster choices deliberately differ to price the resident speed
comparator rather than slowing it for numerical convenience.

| Repeat | G1 complete outer (s) | Resident control complete outer (s) | G1 measured (s) | Resident control measured (s) |
|---|---:|---:|---:|---:|
| 1 | 1,193.8296401 | 165.4116338 | 483.8473523 | 85.7383396 |
| 2 | 1,181.4037324 | 165.2343851 | 471.4147791 | 85.7106115 |
| 3 | 1,181.1578243 | 165.3048309 | 471.1778352 | 85.8431813 |
| Median | **1,181.4037324** | **165.3048309** | **471.4147791** | **85.7383396** |
| Max − min | 12.6718158 | 0.1772487 | 12.6695171 | 0.1325698 |

Median measured seconds/exposure were **3.92845649 G1 / 0.71448616 control**:
**5.498× slower measured / 7.147× slower complete G1**, with nonoverlapping
ranges in the wrong direction. Complete outer wall includes process lifecycle
through checkpoint/readback and exit. Lifecycle medians (G1/control, s):
source startup **2.3564437/2.4305189**, worker startup **2.7584634/0**,
warmup **689.7928880/72.6766556**, final update **6.8502611/0.7874096**,
checkpoint/readback **6.5246553/2.5680690**, shutdown **0.1633725/0**.
Parent write/read-validation medians **435.2941085/209.7876349 s** overlap
worker/barrier work and are not additive elapsed costs or credited savings.
**Extrapolation only:** 10× each measured window plus its observed other
lifecycle gives median **5,424.1367443 s G1 / 937.0566902 s control** for
1,200 exposures. Assumptions are linear per-exposure cost at the new image
mix despite different optimizer trajectories; no full run was observed.

Each bounded G1 run wrote and equally read **479,164,517,728 logical bytes**:
source native 195,287,076,400; frozen sent 56,901,915,200; owner native
97,943,979,856; frozen final 175,082,816; attestations 2,592; gradients
128,856,000,000; identity 12,960; framing 447,904 B. Full-repeat sampled
simultaneous RSS max was **5,578,842,112 B G1 / 1,257,558,016 B control**
(below 8 GiB); active outputs about **1.399/1.014 GB** (below 10 GiB).
Separate `SS_PROFILE=1` G1 smoke exited 0; live-pool maxima sampled
**4,836,967,312 / 4,844,469,088 / 4,843,104,688 B**, parent/worker 0/worker 1.
True host/device/pinned peaks and physical disk bytes remain unknown;
Windows transfer counters include file/device/console traffic.

All six independent native checkpoint readbacks passed. G1 final
canonical/frozen/stable-ID payloads repeat exactly excluding the 112 B
attempt header. **Native-resident control payloads do not repeat byte-exactly**;
no equality is asserted. Unordered raster nondeterminism is an inference,
not an established cause. Cross-method equality is not claimed. Failure
injection was separate C3 evidence, not part of accepted timing.
The initial capture watcher denied Windows deletion (exit 1); corrected
shared read/write/delete capture passed fresh smoke (exit 0). Initial
wrong-directory build launch exited 1 without a build; corrected explicit-
directory build exited 0. Neither failed attempt is accepted timing/parity.

**Admission failed; no 1,200-exposure long repeats were reopened.**
Another cost change needs its own C1 justification and C2–C4 evidence.
No capacity, quality, recovery or production approval is granted.

### Completed two-process staging proxy (synthetic patterns, not G1 traffic)

**Observed** 24 passing GPU trials across three fresh repeats (both directions, two sizes and local copies), 96 ACK-completed packets / 18,434,348,160 forwarded bytes. Each pairwise trial used eight packets and discarded the first for its steady-packet median. The native D2H/pattern check, coordinator pipe bridge/SHA-256, H2D, poison, extra GPU readback and receiver ACK are included serially; no overlap or actual G1 routed data. A bounded-buffer supervisor also exercised short-stream, failed-child, timeout and failed-spawn behavior. The source/payload sizes below are **derived sizing**, and table times are **observed completed-packet medians** in milliseconds:

| Pattern payload | A→B repeats; median; range | B→A repeats; median; range |
|---|---:|---:|
| 148,048,920 B world | 767.707 / 744.604 / 739.944; **744.604**; 27.764 | 742.288 / 726.762 / 735.639; **735.639**; 15.526 |
| 236,000,000 B dense gradient | 1500.684 / 1483.164 / 1488.574; **1488.574**; 17.520 | 1459.596 / 1459.625 / 1467.657; **1459.625**; 8.061 |

| Local completed-copy average, three repeats (ms) | Device A H2D / D2H median | Device B H2D / D2H median |
|---|---:|---:|
| World-size pattern | 5.950 / 6.516 | 23.808 / 23.498 |
| Dense-gradient-size pattern | 9.292 / 9.714 | 38.203 / 36.838 |

**Derived conditional isolated local-copy-only floors**, A→B / B→A: world 30.324 / 29.448 ms; gradient 47.917 / 46.130 ms. These omit IPC, packing, scheduling, synchronization and any shared-state/stats return; physical topology is unknown. The ~4× device-B local-copy asymmetry is measured, not explained. Validation-heavy bridge medians cannot rule out G1, while local-copy floors below ~0.325 s/image do not prove G1 wins. This historical proxy did not measure owner-routed native packets, stats returns, synchronization, resource peaks or training wall; the actual G1 evidence above is separate. The older source's 64 MiB bridge and all-view candidate distribution in the [timing ledger](multi-gpu-training-timing-20260929.md) are historical separate measurements.

## Decision and next execution gate — closed

The [source-native FPBO implementation record](multi-gpu-training-g1-implementation-20260929.md)
retains the original closed proof/full-repeat history and C0–C4 reproduction
protocol. **No current candidate is open. C3 passed; C4 lost, so no new
1,200-exposure repeats are authorized.** Another staging/context cost change
needs its own actual-byte C1 justification and C2–C4 evidence before long
timing can reopen; there is no pending claimed implemented option.

Retain B1's historical measured lead as history, not current production selection.
Neither this bounded G1 resident-speed comparison nor its extrapolation
establishes improvement against historical B1 or the original ordered full
timing. The result rejects this candidate here, not all G1 transports.
O1, topology growth, global merged resumable checkpoints, oversized-scene
capacity, quality, G2 and B2 remain outside **this historical G1 micro-proof**;
its result claims no time-to-quality or capacity gain. The later
[integrated B2 section](#2026-10-01-integrated-b2--selected-cold-and-reuse)
supersedes the then-open bounded resume/sidecar/config/view and consumer gaps
with current native evidence, without upgrading historical B1/G1 runs.

<a id="next-planned-experiment--o1-residencywriteback-screen"></a>

### Completed O1 screen — bounded schedule not admitted

The [O1 screen](multi-gpu-training-o1-screen-20260930.md#completed-o0o3-execution-record)
is **complete with a feasibility blocker**, not an admitted offload trainer.
O0 rehashed the original retained source/config and admitted one AMD R9700
Vulkan device; no source regeneration. O1 executed three independent source
updates and an ordered three-update sequence, with six discarded same-input
native internal/external canonical-byte oracle replays and exact poisoned
native checkpoint reload. All 1M rows, 7,150 nonfinite-mean rows and 3,907
blocks (3,906 full plus 64 valid tail rows) remain. Every final row clock
is its source clock +3 (99–10000 → 102–10003); global boundary is 10003.

**Observed:** every update dirtied all 3,907 blocks in each packed/bounds
codec-pair union and changed all million bias clocks. Means changed on
934,189 / 830,816 / 229,854 zero-data-gradient rows independently,
934,189 / 828,274 / 228,779 with carried state. Statistics were captured
before restoration: radii/accumulation weights change even though optimizer
statistics and shared appearance are frozen for the diagnostic. SH paired-cell
transposition and padding are accounted, not compacted.
Retained pool + scratch max was 8,019,788,176 B at high-view backward prime;
7,946,903,176 B epoch requests are **not** semantic-live peak. Alias arena
max 37,463,312 B is counted once, scratch 0. Canonical snapshot storage
387,983,064 B and world/optimizer/clock subtotal 300,441,656 B are accounting,
not memory savings; the original derived 312,441,656 B subtotal remains so.
Sampled RSS max was 3,019,374,592 B and full-attempt live output max
627,089,379 B. Admission budget/usage was 33,250,242,560 / 0 B and scratch
19,553,894,400 B; the 6 GB device estimate understated the 8.02 GB retained
pool, within budget. True GPU/host/pinned/image-cache semantic-live peaks
remain **unknown**.

**O2 NOT ADMITTED, first O3 branch selected:** a conservative prospective
host render spatial index would need native projection-support bounds,
invalid-row fallback and source/topology/parameter-generation refresh; false
positives, workspace and costs are unknown. Independently, optimizer blocks
must stream densely. Existing GPU candidate IDs depend on full-world
projection. No native block-range contract preserving global-N normalization,
clocks, whole-block reductions and row IDs, or bounded partial-gradient/state
servicing, is established. Thus there is no coherent schedule to price:
no repeats or fresh native speed reference, and **21.3637501 s is diagnostic
only**. This is not architectural impossibility.

Final build and CPU packet checks exited 0; headless passed 14/14, exit 0.
Initial comment-budget build failure and the rejected diagnostic's invalid
homogeneous row-clock assumption remain preserved; source-relative +3 checking
corrects the harness without relaxing native acceptance. The single handoff
is the precise missing-mechanism record, not an eviction/reload micro-plan.
Retain native resident baseline and historical B1 lead; no speed/capacity
or production winner. G1 stays closed.

### Completed O1 M0–M4 continuation — 7 GiB schedule rejected at M0

The [continuation execution record](multi-gpu-training-o1-screen-20260930.md#completed-m0m4-continuation--m0-resource-admission-rejected)
is **CLOSED through M0, NOT ADMITTED**; the original M1–M4 plan remains
a reproduction/conditional contract, not executed work.

| Fresh evidence / declared contract | Result |
|---|---|
| Identity and device | Original source/config SHA matches; native binary and 14 source bindings unchanged; AMD R9700 `vulkaninfo` summary/full exited 0 |
| Available budget / scratch snapshot | 33,250,242,560 B budget, 0 B usage; 18,821,275,648 B free scratch |
| Retained device cap | 7 GiB = **7,516,192,768 B**, below 8,019,788,176 B resident accounting; not device exhaustion |
| Other caps | Accounted host and sampled aggregate RSS each 8 GiB; pinned 16 MiB; live outputs 10 GiB; minimum free scratch 10 GiB |
| Declared servicing | Sequential 128/256 whole 256-row optimizer blocks; render cap 850,000 rows / 2,000,000 tile intersections; retain full-image loss/shared/native ordered-raster buffers, no full Gaussian copy |
| Optimistic native non-splat retention | 8,019,788,176 − 560,441,660 full Gaussian allocations − 15,628 unrelated B1 ownership = **7,459,330,888 B** |
| Required render rows | 822,825 candidates; 46,078,200 B non-SH + 75,719,680 B native padded SH + 25,720 B bounds = **121,823,600 B** |
| Admission floor | **7,581,154,488 B**, exceeding the cap by **64,961,720 B** before optimizer chunks, mapping, additional staging or unknown overhead |
| Dependent gates | M1, M2, M3, M4 **NOT RUN**; no transfer pricing, repeats or eviction implementation |

SH accounting uses 3,215 blocks / 823,040 padded rows × 46 cells × 2 B.
Some full-row projection mask/bookkeeping is intentionally retained by this
schedule. This specific capacity-ledger rejection is not a measured simultaneous
peak or an architecture-impossibility result. Available hardware budget does
not waive the declared residency target.

Local `mechanism.manifest.json`, `schedule.json` and `admission-ledger.json`
under `o1-mechanism-20260930T175846Z` retain commands, hashes and arithmetic
assertions; private paths/UUIDs remain local. Only device queries, identity
checks and ledger assertions are fresh: no new dev build, headless run or
native six-update repeat ran because relevant inputs/code did not change.
O0–O3 and G1 remain closed; O1 training remains unverified. Retain the native
resident baseline and historical B1 lead, with no admitted next implementation
for this rejected schedule and no speed/capacity/production approval.
