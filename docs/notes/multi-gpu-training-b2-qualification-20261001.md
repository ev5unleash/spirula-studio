# B2 longer-run quality and recovery qualification — 2026-10-01

Status: **Q0–Q4 executed. Longer-history decoded recovery FAILED; matched
quality evidence is complete. Release qualification is withheld.** Fresh
resource admission and all declared training/fault/evaluation arms ran.
The [master plan](multi-gpu-training-plan.md) retains architecture policy and
[B2's selected cold/reuse result](multi-gpu-training-comparison-20260929.md#2026-10-01-integrated-b2--selected-cold-and-reuse).
That three-repeat throughput selection remains unchanged, not a quality pass.

**[D0–D5 causal continuation completed](multi-gpu-training-b2-causal-20261001.md#execution-evidence) on 2026-10-02.**
Worker backing enforcement is implemented and exercised. Late immediate native
roundtrips were exact, but no-save continuations also failed the unchanged screen.
Equal-row B2 raw/cc PSNR remained −1.674363 / −2.357914 dB below fresh control.
Native training remains default; B2 experimental. Q0–Q4 failures stay closed.

## Objective and fixed scope

Determine how the selected approximate method behaves over a useful longer
continuation and whether its existing recovery contract survives longer owner
histories. Keep native fixed-topology/appearance 3DGS FPBO, full-scene context,
original row identities, the supplied kitchen source and two supported AMD
Vulkan devices. O1 stays parked and tested G1 stays closed.

This is qualification of the existing `spirula train-blocks` path, not a new
architecture contest, three-repeat speed benchmark or globally exact optimizer.
No topology growth, GUI/remote work, enlarged-scene capacity, full meshing,
power-loss simulation or globally resumable merged checkpoint is authorized.
Quality acceptance has no user-approved numerical threshold yet: collect and
interpret the evidence, but do not invent a PSNR floor or declare a release pass.

## Q0 — bind inputs, exact commands and storage before launching

Read [current B2 usage and limits](multi-gpu-training-b1-execution.md#current-bounded-native-b2-path)
and the integrated scorecard. Resolve the local `b2.manifest.json` under
`b2-execution-20261001T024101Z`; reuse its exact source, config, view manifest,
worker devices, native evaluation and checkpoint-verification commands.
Rehash inputs and bind current source/build/toolchain/driver. Keep private paths
and device UUIDs in a new local qualification manifest, not committed docs.
The source is step 10,000 with original optimizer horizon 30,000; do not shorten
that horizon, change numerical settings or substitute a model PLY for state.

Predeclare this schedule before execution:

| Arm | Fresh source | Target | Logical training work |
|---|---|---|---|
| Native all-owned control | Step 10,000 | Step 30,000 | 20,000 updates/exposures |
| Uninterrupted B2 | Same source | Step 20,000 per owner (`--updates 10000`) | 10,000 per owner; 20,000 aggregate |
| Recovery B2 | Same source, separate new group | Same immutable step-20,000 target | Same committed owner schedule as uninterrupted B2; discarded retry work reported separately |

These are equal aggregate logical exposures, **not equal per-row optimizer
updates, view weights or trajectories**. Record all actual input/face/dimension/
pixel events using `SS_M0_EXPOSURES=1`. Validate exact expected counts from the
native step convention; do not silently discard terminal events to fit the table.
Do not insert the earlier benchmark's 101-update warmup or 1,200-exposure window.
All continuation work counts; this run does not replace those timing samples.

Refresh available disk, aggregate RAM and explicit device budget/usage. Retain
caps of **24 GiB accounted per device**, **12 GiB sampled aggregate host RSS**,
**8 GiB new live qualification outputs** and **10 GiB minimum free scratch**.
These are admission limits, not measured peaks. Include both replicas, periodic/
STOP/final checkpoints, old attempts, merge/evaluation, oracle restore workspace,
and simultaneous raw/archive data while packaging. Run arms sequentially.

The cleanup's 24.111 GiB free-space observation is historical, not admission.
NTFS compression changes physical allocation, not logical payload size; measure
both when relevant and do not count future compression as available headroom.
Existing source/B2 archives and `evidence-bases` are protected: compact evidence
uses source-referenced XOR/gzip and PLY deltas, so its ZIP files are not standalone.
Keep manifests, failure evidence and archive bases. Never blanket-clean Temp.

The native group already requests full, latest-only checkpoints; source save
cadence is 10,000 steps. There is no group CLI save-cadence override. Check real
retention across attempt directories, not only each worker's latest checkpoint.
If the projected peak does not fit, use verified existing lossless packaging
one completed arm at a time and account for its temporary coexistence. Verify
reconstruction before removing a newly created redundant expansion. Do not
remove older retained experiments or silently shrink the work/caps to admit Q1.

**Exit:** exact argv/environment, bound source and held-out ordering, declared
work, per-phase storage ledger and monitoring/stop procedure in the local
manifest. Resource breach means cooperative STOP, preserved checkpoint/evidence
and no dependent new launch; an incomplete arm is not a qualification result.

## Q1 — run the longer control and uninterrupted B2 arm

Reuse the native `train` all-owned control form from the retained `bench.py`:
`SS_B1_OWNER=all`, `SS_M0_EXPOSURES=1`, full/latest-only checkpoints, original save
cadence, viewer disabled and `--keep-viewer-alive 0`. **Clear both
`SS_B1_STOP_AT_STEP` and `SSPLAT_B1_STOP_AT_STEP`** and let the original
30,000-step horizon terminate this control normally. The native early-stop
validation rejects a target equal to the horizon; do not set it to 30,000.
Do not apply owner-specific view filtering to that control. Use the same frozen
numerical configuration as the selected B2 comparison.

Run a new uninterrupted group with the existing interface:

```text
spirula train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_UNINTERRUPTED_GROUP --updates 10000
```

Bind environment explicitly: clear stale owner/evaluation/stop switches in both
`SS_` and legacy `SSPLAT_` spellings before the coordinator invocation; it supplies
each worker's owner and target itself.
Set the existing exposure diagnostic in both arms. Preserve the validated group,
owner contracts and full final checkpoints. Require successful native completion,
complete unique owned/excluded row coverage, exact frozen context/shared state,
correct original-row clocks and native final readback using the retained oracle.
Apply expected clock increments per owner and original row, never one uniform
source clock. Preserve the 64-valid-row tail and inactive/nonfinite native rows.

Record full elapsed time and work, including required finalization. It is a
single qualification pair, not a fresh median or time-to-quality proof. Reuse
existing preparation with its identity/cost disclosed; do not label it cold.

**Exit:** accepted longer control and uninterrupted B2 artifacts, exposure/work
ledgers, resource observations and readback. Reuse frozen boundary/render checks
if their identities still match; no need to rerun closed short benchmarks.

## Q2 — exercise later STOP, killed-owner recovery and native comparison

Create a second fresh group with identical source/views/devices/10,000-update
target and a different output directory. Never extend a completed group or
modify `group.json`; resume accepts no target or configuration overrides.

1. After **both owners have completed at least 5,000 updates** (step 15,000),
   send `STOP` plus newline while the group is still incomplete. Use native
   progress/checkpoint evidence, not a fixed wall-clock sleep. Expect exit 42,
   validated saved owner steps and no `complete.json`; unequal progress is valid.
2. Run `spirula train-blocks --resume-group RECOVERY_GROUP`. After both owners
   reach at least step 19,000 but before completion, terminate exactly the owner
   child identified by this attempt's scheduler/process record. Do not kill by
   executable name. Expect failure exit 2, sibling-safe stop, preserved validated
   checkpoints and no completion publication. Record discarded work explicitly.
3. Resume the same group explicitly to step 20,000 per owner. Expect exit 0,
   native readback and no-replace `complete.json` binding the final outputs.
   A fault missed because the group already finished is not a passed fault test;
   retain that attempt and admit a fresh recovery arm if resources permit.

Compare each final resumed owner with its same-owner uninterrupted counterpart,
not with the all-owned control. Reconcile exposure replay through the **accepted
checkpoint lineage**: discard logs after an abandoned checkpoint for logical
schedule equality, but include all attempted work in recovery cost. A naive
concatenation of retries double-counts replay and cannot establish correctness.

Retain the existing numerical policy without relaxation:

- Owner/view/source identities, committed input/face/pixel sequence, per-row
  clocks, unowned packed values/bounds and frozen shared state are exact.
- Compare owned decoded parameters, moments and SH with the existing native
  `fpbo_split_parity` **1e-4** recovery screen and actual comparison formula.
  Reuse the retained comparator; do not replace it with a mean-error threshold.
- Report packed/raw bound differences separately. The four-update proof was
  **not byte-identical** in owned state, and its raw mixed-unit bound outliers
  were diagnostic. It does not grant a larger long-run tolerance.

A failure retains the earliest failing evidence and blocks a recovery-pass claim.
Investigate its cause with bounded native replay/comparison and fix attributable
checkpoint/sampler/contract defects in the existing path. Do not silently switch
to ordered raster, change optimizer settings, waive differences as approximation
or reopen G1 performance work. Finish analysis of valid Q1 outputs even if Q2
is blocked. No artificial power cut or power-loss durability claim is part of Q2.

**Exit:** later-history recovery evidence or a precise failed contract/root-cause
handoff, with no damaged source, false completion or weakened numerical gate.

## Q3 — measure matched output quality and explain regressions

Use the existing source-bound native evaluation path for final all-owned control,
uninterrupted merged B2 and accepted recovered B2. The recorded invocation is
ordinary `spirula train --resume SOURCE ...` with `SS_B1_EVAL_PLY=PLY`,
`SS_B1_EVAL_IMAGE=0`, explicit device, viewer disabled and `--keep-viewer-alive 0`.
Reuse the validated command/contract from the manifest; clear worker-only switches.
This reader requires the native `ss_b1_appearance` source/shared-state/body binding.
Verify that the `SS_B1_OWNER=all` control exports and passes that binding before
relying on its metrics. A plain source-snapshot PLY is not accepted by this route:
do not forge its comment or bypass validation to add a source-baseline metric.
No new metric implementation or Python ML dependency is needed.

Evaluate the **same 132 held-out views** and source-bound frozen appearance.
Native `metrics.json` provides per-view L1, PSNR, SSIM and color-corrected forms,
but no image-name list: retain the corresponding native dataset ordering and
map every metric entry to its held-out image ID. Length equality alone is not
view alignment. Reuse prior metrics only with matching model/source/evaluator/
view-order identities; the four-update evaluation is not the longer control.

Report aggregates and per-view deltas for B2 versus control and recovered versus
uninterrupted B2. Include median, worst regressions,
regression counts and metric-specific directions rather than hiding them in a
mean. Predeclare the retained boundary/distant-occluder diagnostic IDs if their
source mapping is valid. If not, mark that subset unavailable rather than
inventing a production boundary set. Review the worst-view outputs as a separate
post-results diagnostic, clearly distinguished from the predeclared subset.

Keep evaluation cost outside training wall and retain metrics/manifests first;
render only bounded diagnostic images that fit Q0's ledger. Expose any image
artifacts through the native evaluator's existing output path. Do not add an
image export subsystem just for this qualification. Discuss incomplete context,
view weighting, independent clocks or other causes only as established evidence
or labeled hypotheses. A quality regression is not automatically a wiring bug.

**Exit:** a source-bound longer-run quality report with readable per-view results,
resource/evaluation costs and explicit uncertainty. No invented threshold,
statistical confidence from one pair, quality-equivalence or release claim.

## Q4 — close qualification and name the next release decision

Update this execution record, the master status, B2 usage limits and scorecard
without replacing the accepted three-repeat performance table. Bind exact
commands, exit codes, hashes, checkpoint lineage, comparisons and resource data
in the new local manifest. The execution results below supersede planned status.

| Evidence | Decision |
|---|---|
| Longer recovery contract passes and matched quality report is complete | Mark the stated longer-history scenarios qualified; retain B2's bounded throughput selection. Present the observed approximation tradeoff for a release decision, not broad production approval. |
| Ownership, replay, safety or decoded recovery contract fails | Preserve and diagnose/fix the causal defect, repeat affected checks, and withhold the failed qualification claim. Publish valid independent quality findings rather than losing the whole attempt. |
| Trained quality regresses without a correctness defect | Report magnitude, affected views and conditions. Do not alter the objective or start finetuning/context refresh automatically; those change the selected method and its charged costs. |
| Required artifacts or resources are unavailable | Complete reachable analysis, record exact missing prerequisites and remain incomplete at that gate. Do not shrink, fabricate or count incomplete arms. |

Autonomously carry out the defined runs, checks and evidence-backed fixes; no
routine permission is needed between passing gates. Unchanged code/build/inputs
need no ceremonial rebuild. Any implementation fix requires the dev Vulkan
build, relevant headless/worker/native supported-device checks, a real changed-path
smoke and updated docs. Retain old evidence instead of reclassifying failures.

**Finish line:** Q0–Q4 execution record, valid paired quality results and recovery
disposition, retained runnable artifacts, plus one explicit release recommendation
and its limits. Application-specific quality acceptance remains a user decision;
absence of a threshold does not block collecting the report. Stop at that decision
instead of claiming production acceptance or expanding to topology, GUI, O1 or G1.

## Q0–Q4 execution results

The retained local `b2-qualification-20261001T142017Z/qualification.manifest.json`
binds exact argv/environment, hashes, driver/toolchain, monitoring samples,
checkpoint lineage, failure evidence and native metric/image outputs.
`quality-per-view.csv` maps all 132 metric slots to actual source image IDs/names;
`quality-summary.json` and `quality-report.txt` retain all six metrics and worst
views. Private paths and device UUIDs remain local. All source/B2 archive bases
were preserved; new outputs remain loose and runnable, without packaging.

### Q0 — admission and resource limitations

Rehashed original step-10,000 state, configuration, PLY, view manifest and
selected native binary; source state SHA-256 remains
`1986eadaeee4fe3eb86883ccb436797a2d63a01f7aca9bef2273ef64efa507f5`.
The 30,000-step horizon and numerical configuration were unchanged.
Existing preparation was reused, not cold discovery. The ledger admitted
7,340,032,000 B projected new live data against the unchanged 8 GiB limit.
Fresh scratch was 23,577,763,840 B; available host memory was 38,806,300 KiB.
Both supported AMD devices reported local heap budget 33,250,242,560 B and
query-process usage 0. The full `vulkaninfo` video enumeration timed out;
focused native Vulkan budget queries and `vulkaninfo --summary` succeeded.

Host RSS, scratch and new live outputs were monitored; no sampled breach was
observed. Maximum sampled aggregate native RSS was 6,912,749,568 B, below
12 GiB. Final new logical/allocated bytes were 4,161,428,071 / 4,161,768,600 B;
final free scratch was 18,655,502,336 B, above 10 GiB.
These samples are not true peaks.

**Device allocation enforcement is unverified.** The separate Vulkan probe's
budget remained unchanged and its usage stayed zero during worker execution:
it did not measure worker allocation. Its heap-size/budget arithmetic is not
worker in-use memory or a device peak. The control's native pool/scratch
high-water observation was 1,740.62 MiB; no equivalent long-owner telemetry
was captured. Fresh available-budget admission does not certify compliance
with the 24 GiB worker-accounting cap throughout the run.

Review also found the original direct-control environment lacked the native
stdin listener switch. That completed arm had no observed breach, but its
cooperative resource-stop path was not effective. Future direct invocations
enable `SS_WORKER_CONTROL=1`; fault acceptance now independently requires
actual injection, thresholds, incomplete publication and checkpoint validation,
not merely the expected exit code. Original commands/failures remain retained.

### Q1 — longer native artifacts and actual work

| Arm | Final boundary | Accepted exposures | Actual pixels | Exit | Outer wall, seconds |
|---|---|---:|---:|---:|---:|
| All-owned control | 30,000 | 20,000 | 246,870,006,336 | 0 | 6,545.854813 |
| Uninterrupted B2 | 20,000 per owner | 20,000 aggregate | 246,870,006,336 | 0 | 3,242.707497 |

All three final native checkpoint oracles passed original-row clocks, exact
unowned packed values/bounds/shared state and complete unique owned/excluded
coverage. The 1M original rows, 3,907 blocks, 64-valid-row tail and 7,150
original nonfinite means were preserved. Each row advanced 20,000 times in
control versus 10,000 times in its B2 owner. Actual training-view weights were
16–17 exposures per control image, 18–19 for owner 0's 532 images and 15–16
for owner 1's 663 images.

The control automatically evaluated at its original horizon; its outer wall
includes that evaluation, unlike B2's group wall. Its native training-loop
time was 6,351.47 s, excluding startup/final checkpoint/evaluation. Do not
derive a matched throughput ratio or time-to-quality claim from these walls.
This single qualification pair does not replace the accepted benchmark medians.

### Q2 — fault safety passed; decoded recovery failed

Actual STOP after both owners passed 15,000 exited 42 and saved 15,040/15,002,
without completion publication. The resumed attempt's exact scheduler/request/
parent-PID-bound owner worker was terminated after both reported at least 19,001.
The group exited 2, retained owner 0's prior 15,040 checkpoint and saved the
sibling at 19,005, without completion. Explicit resume exited 0 at 20,000
for both and published native validated completion without replacement.

Accepted checkpoint lineage exactly matches each uninterrupted owner's
input/face/dimension/pixel sequence: 20,000 aggregate committed exposures.
There were 24,032 attempted logged exposures, including 4,032 discarded events
and possibly the killed in-flight event; these are not all completed updates.
STOP/kill/final checkpoints passed exact source-relative clocks, unowned
packed state/bounds, frozen shared state and ownership coverage.

The retained comparator first exited 2 on `owned_ids_sha256`, before decoding.
Source inspection establishes that this hashes dynamically filtered exported
PLY rows, not immutable optimizer block ownership. Independently valid output
sidecars differed by 3,772/2,139 IDs for owners 0/1. Actual row classification
found opacity/dead-scale export-threshold crossings, not new nonfinite rows.

A separately retained diagnostic comparator removes only cross-output equality
of those dynamic exported-ID hashes, after independent native validation.
It still compares all original owned-mask rows and exact source/config/views/
mask/shared/unowned/clocks, using the unchanged
`min(abs(delta), abs(delta)/(1e-6+abs(reference))) <= 1e-4` screen.
**Both owners failed**, exiting 1: world/SH screen errors were
25,282,671 / 24,301,212; moment/bound screen errors were 30 / 22.
Clock and source-frozen byte errors were zero; no nonfinite mismatch appeared.
Maximum absolute mean differences were 0.042650 / 0.065560, with substantial
quaternion, scale, opacity and SH differences. Raw packed/bound diagnostics
are retained separately; no numerical tolerance was relaxed.

The causal handoff is `Q2-cause-handoff.txt`. Exact committed scheduling and
immutable ownership are established; checkpoint numerical loss versus fresh
unordered-raster variance and accumulated optimizer amplification remain
unresolved hypotheses. No late pre-save canonical counterfactual was captured.
The existing ordered/all-owned proof seam changes the marked-owner contract
and raster path, so it was not substituted as a long-history recovery proof.
There is no demonstrated checkpoint/sampler defect to fix from this evidence,
and no long-history recovery pass.

### Q3 — matched held-out quality

All three models used the same evaluator binary, original source/frozen
appearance and exact 132-name COLMAP/fraction-complement order.

| Mean metric | All-owned control | Uninterrupted B2 | Recovered B2 |
|---|---:|---:|---:|
| L1, lower better | 0.079798 | 0.091200 | 0.090951 |
| PSNR, dB, higher better | 20.693150 | 19.090783 | 19.108771 |
| SSIM, higher better | 0.809307 | 0.789371 | 0.789466 |
| Color-corrected L1 | 0.034570 | 0.048526 | 0.048425 |
| Color-corrected PSNR, dB | 25.467073 | 22.932693 | 22.946175 |
| Color-corrected SSIM | 0.843811 | 0.822982 | 0.823089 |

| B2 minus control | Mean delta | Median per-view delta | Regressions / 132 |
|---|---:|---:|---:|
| L1 | +0.011402 | +0.008449 | 101 |
| PSNR, dB | −1.602366 | −1.170050 | 111 |
| SSIM | −0.019937 | −0.019422 | 122 |
| Color-corrected L1 | +0.013956 | +0.012023 | 127 |
| Color-corrected PSNR, dB | −2.534380 | −2.215950 | 128 |
| Color-corrected SSIM | −0.020829 | −0.017898 | 128 |

Worst raw PSNR regression was slot 49: −9.4233 dB. Native GT/control/B2
images show markedly smeared/blotchy foreground texture and door/trim edges
in B2 relative to control. Recovery minus uninterrupted raw PSNR had mean
+0.017988 dB, median +0.000400 dB, 66/132 regressions and worst slot 31
−0.5568 dB. Other recovery deltas and worst views remain in the complete local
report. Similar aggregate metrics and visual layout do not waive decoded failure.
No authoritative boundary/distant-occluder subset was available; slots 49/31
are post-results diagnostics only.

Three final source-bound native evaluations exited 0 in
184.234335 / 183.099727 / 183.223630 s; four bounded diagnostic evaluations
also exited 0, costing 747.585907 s separately. Only selected-slot PNGs were
exported. Evaluation is outside training costs.
Independent clocks, changed view weights and frozen full-scene context are
established method differences; their individual causal contributions to the
quality regression are hypotheses, not isolated findings.

The initial all-owned PLY evaluation refused missing merged appearance binding.
`validate_b1_appearance` now accepts a canonical all-owned checkpoint PLY only
through existing native contract validation, exact original-source/shared
identity and full body/config/sidecar integrity. Plain source PLYs and partial/
foreign/tampered checkpoints remain refused; no PLY comment or model was forged.
All three final evaluations were rerun with the same corrected evaluator.
The dev Vulkan build, 16/16 headless (including extended `b1_merge_contract`)
and explicit supported-AMD 1/1 `cli_training_smoke` passed. The first CLI smoke
refused its missing explicit device before GPU work; the corrected invocation
and original evaluation refusal remain recorded.

### Q4 — release disposition

**Recommendation: withhold longer-history recovery qualification and release
approval; retain B2 only as the already demonstrated bounded throughput
selection with an explicit approximation warning.** This workload shows
material per-view quality regressions and fails the unchanged decoded recovery
screen. Worker cap enforcement was unverified in these historical Q runs;
the separate completed D1 now proves the new instrumented path, not retroactive Q compliance.
No quality floor was invented, and no single-pair confidence, universal
optimality, capacity gain or production acceptance is claimed.

The separate [D0–D5 continuation](multi-gpu-training-b2-causal-20261001.md#execution-evidence)
is complete. It found no immediate serialization loss and demonstrated bounded
native no-save variability with asymmetric failures, not a unique long-Q cause
or repaired recovery. The fresh equal-row control confirms persistent B2 quality
loss; release remains withheld. Application-specific quality acceptance is
distinct. No tolerance/raster/objective change, context refresh, topology growth
or G1/O1 reopening follows from this negative disposition.
