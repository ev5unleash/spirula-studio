# Multi-GPU training: short-run improvement plan

Updated 2026-10-17. Status: **executed; placement and result-tail hypotheses
rejected, no speedup**. Production implementation remains `d1a829af`; execution
started from checkout `41e4e614` with this local plan. This follows the
[brainstorm handoff](multi-gpu-training-brainstorm-handoff.md); completed B2,
G1, O1, raster-only and image-strip results remain preserved, not execution queues.

## Goal and scope

**Decrease the training time of one model by parallelizing its work across the
two supported AMD GPUs.** Benchmarking supports an implementation decision; it
is not a separate deliverable. Independent jobs, larger batches, frozen
appearance/topology and capacity-only improvements do not satisfy this goal.

Keep the selected contract: same logical batch/objective, active appearance and
topology, checked numerics and one synchronous authoritative native update.
Use Windows Vulkan on non-NVIDIA hardware. Native remains the default.
`SS_PPISP_REDUCTION=staged` is the fair control in both native and dual arms;
qualifying a standalone single-GPU PPISP optimization is **not this plan**.
Execution was authorized by the subsequent request to execute this plan.

## Reuse the evidence; do not repeat the campaign

The [existing fixed-input comparison](multi-gpu-image-strip-investigation.md#matched-fixed-input-continuations-no-speedup)
measured about 100.68 ms/native update versus 176.55 ms/dual update: the existing
strip path needs roughly 75.87 ms/update removed just to tie that native control.
This is one workload, not a universal performance bound. Its implementation is
complete; another unchanged benchmark cannot make it faster.

Use retained logs and the current source to trace snapshot, fork, raw-statistic
collectives and join/merge dependencies. Overlapping timing medians cannot be
summed into an overhead budget. Prepared projection/binning data changes with
training; caching stale state is not an optimization under this contract.

## Short-run rules

- State the question and the result that would change the next action before
  launching. If existing evidence answers it, do not run it again.
- Default timing screen: **8 actual updates**, first 2 treated as warm-up and
  the next 6 compared. Keep all 8 in the record; topology remains active.
- Start with one matched native/candidate pair. Reuse a suitable existing
  control for rejection; require a fresh matching control before claiming a win.
  Match source, input, settings, authority device and timing boundary.
- **Stop after a decisive short result.** Reject a clearly slower candidate;
  keep a clear improvement for focused correctness/ordinary-sampling checks.
  Do not finish a larger repeat matrix merely because it was planned.
- Extend to **20 updates only if the 8-update result is inconclusive and the
  extension could change the decision**. A third control is for observed drift
  or contradictory results, not routine reassurance. If still ambiguous, park
  the hypothesis rather than escalating to 100/1000/full-horizon runs.
- Compare the same completed updates, per-step timings and their variation, not
  just a favorable median. A mixed/noisy result is inconclusive, not a win.
- Preserve full resolution, source state and original 30000-step horizon.
  Shorten the observed continuation, not the learning-rate schedule.
- No full training runs, quality campaigns, broad parameter sweeps or full test
  suites by default. A larger run needs an explicit unresolved question that
  short checks cannot answer and a new user decision.

## 1. Try the cheapest change to actual parallel execution

The current strip cut is approximately half the image, aligned to 64 rows;
measured worker and authority pixel times differ, as do the device/host transfer
routes. Neither observation establishes the cause of the loss.

First discriminator: **reverse the physical authority/worker assignments**
using the existing selectors. Use one 8-update screen and a native control on
that authority device; do not compare against a native run on the other card.
Do not rerun the known losing orientation just to confirm it still loses.

If the timeline identifies pixel-work imbalance as important, try one informed,
64-row-aligned cut rather than introducing a dynamic scheduler or sweeping cuts.
A small improvement that leaves dual substantially slower is not success and
does not justify longer benchmarking. Continue only if the remaining costs have
a credible route to beating native.

## 2. Make one critical-path improvement, not a transport rewrite

If placement/cut alone does not suffice, use the retained timeline and the short
screen to choose **one** source-backed hypothesis. If essential ordering data is
missing, capture one short diagnostic; do not start another profiling campaign.

Prioritize costs that prevent useful overlap: the authority's pre-fork snapshot,
collective synchronization, or result return/merge. The current transport sends
full projected-state/binning structures and returns full projected gradients;
GT/mask already cover only the worker region plus halo. Worker-relevant routing
is an option only if avoided traffic exceeds packing/remapping costs. Preserve
contributor ordering, global loss normalization and gradient ownership.

Before editing, identify which dependency becomes shorter or overlaps useful
work and why its potential saving matters relative to the observed gap. Nominal
PCIe bandwidth, extension availability and adding overlapping timer values are
not that argument. Do not build a same-process runtime, peer-memory transport,
general scheduler or new benchmark framework speculatively.

Implement the smallest justified change in the existing executor/operators.
Build incrementally after the edit, run affected checks once, then use the
8-update screen. Keep or reject the change from that result; extend only under
the short-run rules. If the measured critical path rules out this decomposition,
stop tuning it and report the specific blocker before proposing another split.
The multi-GPU speedup goal remains unmet; do not substitute a single-GPU result.

## 3. Check a promising candidate, then stop measuring

- Reuse the existing same-input source proof after a change to partitioning,
  math or transport. Require the established pointwise/gradient checks without
  relaxing tolerances. A correctness failure stops performance qualification.
- For a candidate that beats the fresh native short control, run one short
  ordinary-sampler native/dual **behavioral check** through the existing CLI
  stop launcher. Target 10 updates beyond the source and compare the shared
  target checkpoints, not unequal later stop-saves. With `SS_M0_EXPOSURES=1`,
  verify matching input/face/dimension sequences through that target. Record
  actual stop counts; keep appearance/topology active. This CLI path does not
  expose common-prefix per-update timings: do not treat its subprocess wall as
  another matched timing result or run longer merely to obtain progress output.
- Check actual state/update diagnostics and nonfinites. Matching clocks or
  finite values alone is not equivalence; row slots are not stable identities
  across topology changes. Do not invent a full-update tolerance after observing
  the candidate. Resolve a suspect discrepancy with the smallest common-state
  diagnostic, not an automatic long quality run.
- Exercise only failure/lifecycle paths changed by the implementation. Unchanged
  targeted evidence does not need repeating; transport changes still need their
  affected fail-stop/publication check.

Report training-interval savings separately from startup and final-save wall.
Account for additional worker startup: if extrapolating a break-even update
count, label it an estimate, not an observed full-run speedup. Do not rank short
full-path times dominated by variable loading/cache/save effects.
The initial timing decision is fixed-input only; the ordinary-sampler smoke
checks behavior, not representative speed. State that limit rather than
presenting a repeated-view win as a general training-speedup qualification.

## Existing tools and completion record

Use `g1_native_proof image-strip-source-run --updates 8 --input-index INDEX`
with the existing source/device/output arguments; add `--worker-device` only for
the dual arm. Its per-update output supports the 2+6 split without a new harness.
It stops at the selected update count, not an arbitrary mid-run threshold; eight
updates are the initial commitment. It is fixed-input, **not ordinary sampling**.
See [existing commands](../testing.md#experimental-same-view-image-strips).

For ordinary sampling use `reference/python/stop_at_checkpoint.py` with the
normal trainer, a fresh output directory and a save cadence that publishes the
selected target. Preserve the horizon; the stop-save can land after the target.

Before execution, check storage afresh, retain the 10 GiB post-growth floor and
one process per GPU, and preserve originals, corpus, source and failed evidence.
Record only what the decision needs: source/build, devices/settings/input IDs,
actual update count, matching timing boundaries, checks, result and artifact
references. Keep private paths out of this repository. No new ledger or report
framework. No push or PR.

Completion is a real two-GPU improvement with a matched short timing result and
targeted behavioral/numerical evidence, or an explicit rejected hypothesis with
its blocker. A rejected hypothesis is not achievement of the overall goal.
Short checks do not certify long-run quality, general workload coverage or full
recovery. Do not change defaults on timing evidence alone.

## Execution result — 2026-10-17

**Native remains recommended; the single-model multi-GPU speedup goal is unmet.**
The permitted completion branch is rejection with an explicit measured blocker,
not a qualifying implementation or a request to continue an older campaign.

### Reversed physical placement: reject after eight updates

Question: can physical-role reversal beat native on that authority device?
A clear win would advance to numerical/ordinary-sampler checks; a clear loss
would stop placement testing and select one critical-path hypothesis.

Both arms used the preserved full native step-10000 source, input 847 at
2880×2880, 1M capacity, original 30000-step horizon, active appearance/topology,
staged PPISP and the existing fixed-input source-run mode. Authority was
`uuid:00000000010000000000000000000000`; the dual worker was
`uuid:00000000020000000000000000000000`, reversing the earlier losing assignment.
Only periodic saving was disabled; both final full checkpoints contain step
10008 and full-resume metadata. The retained final-build-10 executable hashes
matched before execution; no rebuild or code change preceded this timing pair.

Per-update `TrainerProgress` latency, milliseconds; first two updates are warm-up:

| Completed step | Native | Reversed dual |
|---:|---:|---:|
| 10001 | 157.686 | 315.675 |
| 10002 | 45.164 | 176.573 |
| 10003 | 113.886 | 177.227 |
| 10004 | 90.092 | 152.117 |
| 10005 | 92.503 | 153.668 |
| 10006 | 92.237 | 157.281 |
| 10007 | 92.468 | 154.694 |
| 10008 | 100.068 | 156.358 |

Updates 3–8 averaged **96.8757 ms native / 158.5575 ms dual**: dual was
**63.67% slower**, requiring **61.6818 ms/update** removed merely to tie.
Medians were 92.4855 / 155.5260 ms; ranges were 90.092–113.886 /
152.117–177.227 ms. Every measured dual update exceeded every measured native
update. This is decisive rejection: no 20-update extension, repeat control,
known-losing-orientation rerun or cut sweep.

The full eight-update loops were 0.794946 / 1.451712 s. Mode-entry-through-
destruction walls were 15.837576 / 14.808222 s, while train-call-including-save
walls were 4.812333 / 5.189135 s. Loading/startup/final-save variation makes the
apparently lower dual full-path wall **not a speedup**. These are fixed-input
measurements, not ordinary-sampling or representative-training qualification.

### One common-clock diagnostic: result-tail optimization cannot close the gap

The selected hypothesis was avoidable serialization in worker return and
authority validation/upload/merge, including timer waits. Original logs showed
29.107–32.104 ms waiting for the worker after local pixel completion, followed
by 20.477–21.142 ms of authority join work. Worker-minus-authority pixel GPU
time averaged about 7.99 ms: imbalance alone did not justify a cut experiment.

One additional eight-update dual run used temporary, buffered
`QueryPerformanceCounter` stamps in the existing executor. It changed no math,
partition, submission, fence or wait. Worker stamps travelled through the
existing shared mapping; the temporary layout used protocol 6. Host stamps
share a clock; device-local GPU durations are not calibrated cross-device
timestamps. This diagnostic is not another uninstrumented timing candidate.

For updates 3–8, worker post-D2H finite validation occupied
**18.1873–19.9828 ms**, before completion publication. The authority's entire
post-worker join occupied **21.6628–24.6361 ms**, including output copy,
validation, upload, merge and final finite-result readback. These two terminal
intervals are ordered and nonoverlapping; their removal cannot advance any
earlier collective or worker computation.

An intentionally overgenerous counterfactual deletes **both whole intervals**,
not merely duplicated finite scans. Subtracting them from each corresponding
diagnostic step's JSON latency gives 137.6224, 109.8152, 117.4424, 116.7441, 112.7718 and
120.3629 ms: **119.1265 ms mean**, still above the fresh native 96.8757 ms.
Every corresponding residual remains 19.7232–24.9394 ms slower than native.
These are hypothetical residuals, **not observed optimized performance**, and
do not predict arbitrary rescheduling or another transport. They reject a
return-validation/merge-only fix for this execution path.

The remaining dependency chain includes the authority's prepared-state
snapshot/publication, worker preparation/upload, globally normalized raw-stat
rendezvous, pixel work, return and the authority update. In the diagnostic,
snapshot-through-publication was 7.7082–8.0037 ms and worker upload after ID
validation was 14.1556–15.3838 ms. These are not an additive removable budget.
Existing copies already batch behind a fence; raw-statistic reductions precede
their dependent VJPs. Deferring timer synchronization may remove CPU wake/query
gaps, not the queued GPU computation or dependent transfer. No measured narrow
change established a route past the residual deficit.

The timer hypothesis was also screened directly. Enclose each GPU duration
inside its causal host interval: first start at request publication (authority)
or input-H2D fence return (worker), then start later segments at the preceding
collective H2D fence return; end at each stop/query return or final pixel drain.
Summing host-minus-GPU enclosure slack over the four disjoint segments,
**separately per role**, yielded only 0.9307–1.1764 ms authority /
0.4090–0.6522 ms worker per update. This generously includes other host work;
it is not permission to subtract GPU elapsed directly from a stop-wait duration.
After both roles' GPU stop/query calls returned, the three disjoint collective
service windows through both H2D fence returns totalled 2.2083–3.0351 ms/update,
including actual copies, summation, validation and acknowledgements. Neither
observation supplies a timer-only route across the remaining deficit.

**Stop tuning this executor decomposition.** No performance patch or transport
rewrite was retained, and no standalone single-GPU optimization was substituted.
This does not rule out every image-strip algorithm or another decomposition.

### Checks, preservation and evidence limits

- Both timing arms and the diagnostic exited 0 after exactly eight updates;
  recorded losses were finite. Source-state SHA256 and canonical config hashes
  matched across all arms. Source, protected dataset and failed evidence remain.
- The native Windows Vulkan dev build passed with the diagnostic, then passed
  again after exact restoration of the original executor. MSVC Release and
  pinned Slang 2026.12.0.1 were used; GUI and patented video remained disabled.
- Restored `vk_runtime_smoke --strip-finite` passed on both AMD R9700 UUIDs.
  No full suite, CUDA/NVIDIA validation or longer training was run.
- No promising candidate remained, so new source-gradient proof,
  ordinary-sampler comparison, decoded-update qualification and lifecycle
  reruns were **not reached**, not passed. Existing numerical/lifecycle evidence
  is unchanged; finite losses and full checkpoints do not prove equivalence.
- Temporary instrumentation was removed; its source and build/run evidence
  remain outside the checkout. Defaults and production executor are unchanged.
  Final process inspection found no trainer/proof/runtime-smoke processes.
  Observed free space was 22.417 GiB local / 163.859 GiB output, above the
  10 GiB post-growth floor. Recheck before any future work. No push or PR.

Private evidence is indexed by basenames, beside the existing preserved runs:
`short-run-reversed-{native,dual}-8.{log,json}`,
`short-run-reversed-timeline-8.{log,json}`,
`short-run-screen-summary.json`, `short-run-timeline-build.log`,
`short-run-timeline-build-identity.json`, `short-run-restored-build.log`,
`short-run-restored-runtime-smokes.log`, and the preserved original/temporary
executor source copies. Each run root contains `image-strip-source-run.jsonl`
and its full step-10008 checkpoint. Private paths and exact commands are retained
in the JSON logs and `PRIVATE-HANDOFF.txt`, not copied into this repository.
