# B2 causal investigation and disposition — 2026-10-01

Status: **D0–D5 COMPLETED on 2026-10-02. Native training is the practical
default; B2 remains experimental.** Actual worker backing enforcement and
late native counterfactuals are implemented and exercised. Equal-row quality
loss persists; original Q longer recovery stays FAILED. The
[master plan](multi-gpu-training-plan.md) records this final disposition.

**[V0–V5 view-policy ablation completed](multi-gpu-training-b2-view-policy-20261002.md#v5--completed-disposition-and-next-decision).**
Full-view policy reduced the gap but remains **0.596867 / 1.050402 dB** below
native mean raw/cc PSNR. D0–D5 stays closed; failed Q/D evidence is unchanged.
Context refresh is a prospective research direction, not an execution demand.
Existing G1/O1 schedules remain closed.

**Future-evaluation authority (2026-10-07):** The
[selected raw dataset policy](multi-gpu-training-plan.md#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
governs all future evaluations: freshly admitted full-resolution `.insv`
originals, not `.lrv` previews or existing LRV-derived images/masks.
Kitchen and Source v5/readback 2,688/298 acceptance, measurements and protocols
are historical, not admission for the new selection. Fresh input/source/binary
bindings and matched native references are required; execution remains blocked
pending current approval within the master's unchanged execution/custody budget.

## Decision being made

[Q0–Q4](multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results)
found material quality loss in **uninterrupted** B2 and a separate longer-history
recovery failure. Native training is the practical default while these remain
unqualified. Keep B2 experimental; its earlier three-repeat throughput result
is historical valid evidence, not proof of comparable-quality acceleration.
O1 remains parked and tested G1 remains closed.

Answer two questions separately:

1. Does native save/load change a late marked-owner state or its continuation,
   beyond variability already present without saving?
2. How does retained B2 at step 20,000 compare with a fresh native all-owned
   control at the **same per-row update count**, as well as the retained control
   at step 30,000?

The second comparison is attribution evidence, not an equal-work speed test.
It does not isolate view weights from frozen context or prove one cause of
quality loss. Recovery cannot explain the uninterrupted B2 quality regression.
No new quality threshold, tolerance relaxation, raster/objective change,
finetune, context refresh, topology growth, GUI, remote work, capacity project
or O1/G1 revival is authorized by this plan.

## D0 — bind the failures, native inputs and safe resource ledger

Resolve `qualification.manifest.json`, `Q2-cause-handoff.txt`, the diagnostic
comparator and `quality-per-view.csv` in the local
`b2-qualification-20261001T142017Z` evidence directory. Preserve all originals,
including the first comparator refusal and subsequent decoded failures. Do not
rerun Q0–Q4 just to reconfirm those reported results. Rehash source/config/view
identities, failed and uninterrupted checkpoints, evaluator and native build.
Use a new local manifest/output root; private paths and device UUIDs stay local.

Retain the original step-10,000 source, 30,000-step learning-rate horizon,
marked ownership, sampler, native unordered raster and frozen shared appearance.
The existing ordered/all-owned G1 proof changes the contract and is not a valid
replacement. A dynamic exported-PLY ID hash is not the immutable optimizer mask:
validate each output's included/excluded coverage independently, then compare
all original owned-mask rows. Do not restore the rejected membership-equality
shortcut or exclude rows that cross opacity/dead-scale export thresholds.

Refresh disk/RAM and worker-device admission. Keep **24 GiB accounted per
worker device**, **12 GiB sampled aggregate host RSS**, **8 GiB new live outputs**
and **10 GiB minimum free scratch**. Include native capture buffers, raw and
packaged coexistence, checkpoint skeleton/reload allocations, old/new buffer
overlap, all attempts and verification workspace. Run GPU arms sequentially;
only the existing two-owner operation may use the two selected AMD devices
concurrently. Do not assume old free-space or query-process usage is current
worker headroom. No NVIDIA/CUDA validation.

Protect source checkpoints, B2 archive bases/`evidence-bases`, selected outputs
and the complete qualification evidence. Compact ZIPs use source-referenced
XOR/gzip and PLY deltas; they are not standalone backups. Existing NTFS compression
does not make logical bytes equal allocated bytes. Admit lossless packaging
only with room for raw plus archive and verified reconstruction before deleting
a new redundant expansion. Never prune older evidence to make the gate pass.

Direct `train` workers must enable **`SS_WORKER_CONTROL=1`** and keep writable
stdin for STOP. The Q control lacked that listener: its completed run does not
prove cooperative cap-stop behavior. Demonstrate that the new resource monitor
can actually stop an incomplete disposable run and preserve a valid checkpoint;
checking only an exit code or changing environment variables is insufficient.

**Exit:** bound evidence, concrete per-phase ledger, numeric enforcement scope,
exact command/environment manifest and a monitored launch plan. No long replay
or new quality arm launches before D1 passes. An unavailable external prerequisite
is a blocker, not permission to silently enlarge caps or shrink the work.

## D1 — enforce and observe allocations in the actual worker

Reuse existing native accounting and runtime allocation ownership; no second
allocator, telemetry service or renderer. Before editing exported interfaces,
inspect current references and read the Vulkan backend guidance.

| Existing location | Required use |
|---|---|
| `src/app/TrainerCore.cpp` | Actual owner/control lifecycle, completed-update reporting, native save/load and stdin stop handling. End-of-loop pool snapshots alone cannot establish a peak. |
| `src/backend/api/BackendRuntime.h` | Existing allocation and best-effort memory-usage contract; keep its limits explicit. |
| `src/backend/vulkan/VulkanRuntime.cpp`, `VulkanContext.cpp` | Worker-owned device allocations, actual allocation/free lifecycle, staging and pinned memory. |
| `src/engine/EngineQuery.cpp`, `src/core/Tensor.h`, `src/core/PoolSlots.h` | Pool/scratch attribution and arena ownership; count backing allocation once, never again for aliases/views. |
| `src/app/B1Group.cpp` | Bind the enforced limit to owner workers and propagate cap failures through existing failure/publication handling. |

Inventory **all actual backing allocations** on the worker's selected device:
pool and scratch, nonpool runtime allocations, staging (including the existing
pageable-copy ring), pinned-host and transient/reload storage. Distinguish
requested tensor bytes, retained allocator capacity, actual backing bytes,
pinned host storage and driver/system estimates. An existing `process_bytes`
counter covers only its documented allocation path, not every Vulkan allocation.
Do not sum pool and runtime views of the same backing memory.

Enforce the declared device cap before a new owned backing allocation can exceed
it; make reservation/failure rollback and concurrent requests correct. Credit a
free only at actual safe backing release, not at wrapper destruction or a phase
switch. Track worker-local current/peak bytes and allocation failures by device
and ownership category. Explicitly state how host-visible/device-local staging
is charged. Independently sample device budget/usage in the worker and require
headroom for driver/untracked use; owned-byte enforcement is not a true physical
VRAM-peak guarantee. Required unaccounted allocation paths block this gate.

Exercise a deliberately small cap through the real allocation path in a disposable
worker: refusal must occur before the over-cap allocation, without native OOM,
partial completion or corruption of source/prior outputs. Check deallocation,
reallocation overlap and a normal admitted run; reconcile counters against pool/
scratch and nonpool ownership without double counting. Retain logs showing the
actual worker PID/device/limit and safe refusal, not a separate probe with usage 0.
Use the full 24 GiB limit for subsequent experiments; the small limit is a test.

**Exit:** supported-device accounting/refusal smoke plus causal regression checks
for the new enforcement. Record instrumented allocations and unsampled driver
limits honestly. This proves future instrumented runs, not retroactive Q0 cap
compliance. A counter printed only after training is not completion of D1.

## D2 — construct the missing late-history counterfactual

The retained evidence lacks a live pre-save late state. Implement the smallest
opt-in native diagnostic at the existing trainer/save/load seam if needed. Keep
it off in ordinary runs and preserve all marked-owner validation. Do not create
a second trainer, checkpoint format, numerical codec or general replay framework.
Reuse native capture/comparison helpers only where their actual schema and owner
contract apply, not the G1 harness's ordered/all-owned setup.

For each owner, drive a fresh ordinary marked-owner prefix from the bound source
to the first Q STOP boundary (**15,040 for owner 0; 15,002 for owner 1**). Capture
at a completed update **before serialization**, with GPU work completed. Record
native values, packed codecs/bounds, moments, clocks, row IDs, frozen/shared and
relevant statistics, plus sampler/RNG/schedule/counter state affecting the next
update. Do not replace this with a capture made only after loading a checkpoint:
that would already incorporate any reconstruction loss being investigated.

Start with an immediate before-save/after-native-load comparison without an
optimizer update. Map serialized versus derived/reconstructed fields explicitly;
compare logical native state, not pointers, padding accidents or scratch content.
Verify metadata, ownership and next-input sequence as well as numerical fields.
A numerical mismatch here is a discriminating signal; attribute it to the
serialization/restore path only after input/layout/field mapping is validated.

Then branch from the **same verified late state S**:

| Arm | Intervention | Purpose |
|---|---|---|
| A | Continue the live state without save/reload | Native continuation reference |
| B | Save full native state, reload through the original marked-owner contract, continue | Save/load effect |
| C | Independently repeat no-save continuation from S | Ordinary unordered execution variability |

Use an independent in-memory snapshot/restore of S only if it covers all
continuation-relevant native and host state and its pre-branch equality is
verified. The no-save reference must not share the disk serializer under test.
If a clean independent reset is unavailable, establish another valid live-state
branch mechanism and prove equal starting state before comparing. Two separately
trained unequal prefixes do not become a counterfactual because they use the same
seed. Missing valid branching is a named mechanism to implement, not a reason to
accept a toy state or an after-load-only proof.

Replay identical next input IDs/faces/dimensions and native settings. Capture
immediate post-load, first-update, then **8- and 64-update** results from S; run
**three no-save continuations** in total to expose nondeterministic variation.
Use the same instrumentation/synchronization at comparison points in all arms.
Compare original-row state rather than compact PLY exports; retain streamed
summaries and complete data needed to explain the first mismatch, within D0 caps.
Probe artifacts are diagnostics, not a completed group. Never retarget or edit a
completed group's metadata to create the branches.

Keep the Q numerical screen exactly:
`min(abs(delta), abs(delta)/(1e-6+abs(reference))) <= 1e-4` for the native decoded
owned fields. Unowned packed values/bounds, frozen/shared state, clocks and
committed exposure sequence remain exact. Report raw codec/bound diagnostics
separately and do not waive decoded failure because final PSNR is similar.
Validate both owners and their native tail lanes.

Before altering code to fix a cause, record falsifiable alternatives and their
observed discriminators: immediate save/load loss; host/sampler state not restored;
no-save native variance followed by optimizer amplification; comparator/schema
error. Do not declare variance a checkpoint defect or use measured variance to
raise the acceptance threshold. Locate the earliest differing stage with bounded
native capture/replay; a gradient-input substitution may explain a diagnostic,
but cannot count as successful end-to-end recovery.

**Exit:** a valid immediate roundtrip and bounded A/B/C result for both late
states, causal classification and a runnable red-capable check for any demonstrated
defect. If these probes do not distinguish the cause, state exactly what remains
confounded; no unbounded longer repeats. D3 is independent after D1 and must
still complete. D4 handles attributable fixes before final disposition.

## D3 — run one equal-per-row-update quality counterfactual

Use the original step-10,000 native source and unchanged 30,000 horizon. Run a
**fresh all-owned control to step 20,000**: 10,000 updates, compared with retained
uninterrupted B2 at step 20,000 per owner (20,000 aggregate exposures) and retained
Q control at step 30,000 (20,000 all-owned updates). Do not substitute a step-20,000
checkpoint from an older growth-enabled source run.

Reuse the exact Q native control argv, full/latest-only saves and frozen settings.
Set `SS_B1_OWNER=all`, **`SS_B1_STOP_AT_STEP=20000`**, `SS_M0_EXPOSURES=1` and
**`SS_WORKER_CONTROL=1`**; clear conflicting legacy stop/evaluation/view switches.
20,000 is below the 30,000 horizon and is a valid native early-stop target.
Do not change `num_iterations` to 20,000. Unlike the full-horizon Q control,
this run needs the early-stop setting. Validate expected cooperative exit 42,
actual step-20,000 full checkpoint, 10,000 committed exposures, row clocks,
frozen state and complete all-owned coverage; exit 42 alone is not acceptance.

Evaluate the new control and compare all three model artifacts with the same
source-bound native evaluator and exact 132 held-out image-name ordering.
`SS_B1_EVAL_PLY` accepts the canonical all-owned checkpoint via native contract
validation and bound merged B2 PLY; it is not an arbitrary plain-source-PLY reader.
Reuse old metric arrays only if model, evaluator, source/appearance and view-order
identities match; otherwise reevaluate retained artifacts through the same binary.
Clear training-only switches for evaluation and keep evaluation wall separate.
If D4 changes numerical training behavior, label pre-fix artifacts separately;
they cannot silently serve as outputs of the repaired method.

Publish raw/color-corrected PSNR, SSIM and L1 aggregates, per-view differences,
regression counts and worst views using the existing metrics/order mapping.
Reuse Q's selected diagnostic images as post-results examples, not a predeclared
production boundary set. Preserve the complete per-view table and disclose actual
exposure/pixel counts and original schedule position for each method.

Interpretation must remain bounded: B2 near the shorter control is consistent
with an update-progress explanation, not proof that it is the sole cause. B2
worse than that control leaves differences beyond update count to explain.
View weighting, frozen context and independent optimization remain coupled in
this experiment; do not claim their individual contributions are isolated.
This is one attribution run, not a matched-wall speedup, confidence interval,
new quality threshold or application-acceptance vote.

**Exit:** completed shorter-control artifact/readback and the three-way matched
quality report. A negative or inconclusive result is valid evidence; no automatic
finetuning, doubled B2 training budget or context-refresh experiment follows.

## D4 — fix demonstrated defects and revalidate the affected contract

When D1/D2/D3 identifies an implementation or comparator defect, fix its cause
in the existing native path and retain a behavioral regression check at the real
seam. Do not stop after writing a root-cause note or a new implementation plan.
Maintain backward input-validation safety and migrate affected callers; no
reconstruction fallback, forged metadata, numerical relaxation or extra trainer.

Run the corrected small causal check first, then the valid native late-owner
branches. Build through the dev Vulkan script and run relevant headless, worker,
checkpoint/contract and supported-AMD checks once after integration, including
actual cap refusal and native CLI/recovery smoke. Permanent tests exercise the
consumer-visible defect, not copied wiring or source text. Remove throwaway
instrumentation when its evidence is captured; retain only the bounded diagnostic
facility actually needed for reproducible regression investigation.

A claim that longer recovery is repaired requires a **fresh uninterrupted and
faulted pair under the original Q schedule** (same source, 10,000 updates/owner,
STOP/identified-owner termination/resume), with D1 enforcement and all Q invariants
and unchanged decoded comparison. Re-admit disk/host resources before launching;
passing 64 updates cannot upgrade the old failed long-run result. Reuse unrelated
valid quality evidence; rerun affected evaluations when model/evaluator changes.
Do not repeat the all-owned 20,000-update control unless the fix invalidates it.

If the observed failure is native numerical variability with no demonstrated
restore defect, record that finding and keep the existing recovery qualification
failed; it is not permission to change the algorithm or contract. If the bounded
counterfactual remains nondiscriminating, preserve the precise uncertainty and
withhold the same claim. Do not invent a repair to force a green result.

**Exit:** every demonstrated in-scope causal defect is fixed and verified, or a
specific non-defect/unsupported-contract disposition with its evidence. A real
unreachable prerequisite is reported as blocked, not mislabeled completion.

## D5 — close the investigation and choose the practical path

Update this execution record, master status, qualification handoff, scorecard,
usage limits and relevant testing documentation. Keep the original Q failures
and accepted throughput medians intact. Bind commands, hashes, allocation/refusal
logs, state schemas, per-owner branches, native checkpoints and metric tables in
the local manifest. Do not retroactively turn Q's missing accounting into a pass.

| Evidence at closure | Practical disposition |
|---|---|
| Accounting enforced; demonstrated restore/contract defect fixed; original longer recovery now passes | Mark only that recovery contract repaired. Quality suitability remains separate; retained quality loss still prevents general release approval. |
| No-save variability explains divergence, or the bounded causal experiment cannot distinguish it | Recovery stays unqualified under the unchanged screen. Report evidence/uncertainty; use native training as practical default. |
| Equal-update comparison still leaves B2 materially worse, or only explains slower progress without demonstrating comparable-output acceleration | No demonstrated quality-qualified split-GPU winner on this workload. Retain native default; keep B2 only as the historical experimental throughput result. |
| A fix improves output quality | Report fresh matched quality and limits; do not assert comparable-quality speed without a separately declared acceptance contract and complete matched costs. |

**Autonomous completion contract:** finish D0–D5, including independent D3 and
all reachable causal repairs/revalidation. Passing one gate starts the next;
failure of a dependent claim does not cancel independent evidence collection.
The finite diagnostic budget is the specified two late states, immediate/1/8/64
windows and three no-save arms, plus targeted first-divergence probes and checks
required to verify an established fix. Extend only to test a named causal
prediction, not to search indefinitely for a favorable run.

The deliverable is worker-accounting evidence, valid late-state counterfactuals,
the shorter-control quality report, verified fixes where justified, and a named
practical disposition. **A defensible negative result completes the investigation;
an unimplemented necessary seam, untested fix or actionable defect does not.**
An unavailable source/device or unsafe resources block dependent execution;
finish reachable work and state the exact prerequisite rather than treating it
as a scientific rejection. Never delete retained evidence to avoid the blocker.

Do not ask for routine phase-by-phase approval or stop with another proposed
micro-plan. Application-specific quality tolerance and changed training methods
remain explicit user decisions, but their absence does not block this investigation
or a conservative native-default conclusion. No broad release, arbitrary-long
exactness, universal best architecture, new benchmark campaign or automatic
production expansion is implied. Leave changes local; no push/PR or unrelated
cleanup is authorized here.

## Execution evidence

The local `b2-causal-20261002T024609Z/causal.manifest.json` binds commands,
source/build hashes, monitored runs, native allocation logs and probe outputs.
Private paths and device UUIDs remain local. Original Q failures, source
checkpoints, archive bases and selected B2 outputs remain intact.

### D0 — bound evidence and admission

Bound 40 protected source/checkpoint/ownership/evidence files, the mutable
baseline evaluator executable, and six toolchain artifacts. Original step-10,000
state SHA-256 remains
`1986eadaeee4fe3eb86883ccb436797a2d63a01f7aca9bef2273ef64efa507f5`;
the 30,000 horizon and view manifest are unchanged. No Q training or failed
comparison was rerun to confirm its outcome.

Fresh ledger admission reserved 6,900,000,000 B of conservative new coexistence
against 18,030,194,688 B free scratch, leaving 11,130,194,688 B above the
10 GiB floor. The 24 GiB worker-backing, 12 GiB sampled host and 8 GiB new-live
limits remain unchanged. Runtime monitoring refreshes actual free/live/RSS;
no compression credit or older evidence deletion admits the work.

### D1 — actual backing enforcement and refusal passed

Every ordinary worker-runtime `VkDeviceMemory` allocation reserves its actual
memory requirement before allocation, including pool/scratch/nonpool, pinned,
pageable staging and params storage. Host-visible backing on nonlocal heaps
is conservatively included once in the total limit and reported separately.
Worker-local heap admission retains 1 GiB headroom above the larger of driver
usage and accounted backing plus pending reservations. Reservations remain
charged until safe backing release. Required separate NN/SfM/video allocations
are unreachable in the accepted ordinary static-image training worker.

Actual 1 MiB direct-worker and two-owner group caps refused a 12,000,000 B
request **before `vkAllocateMemory`**, with zero allocation calls. Direct exit
was 1; group exit was 2 with no completion publication. Zero, malformed and
overflowing caps also exited 1 cleanly. Runtime checks exercised allocation
overlap, concurrent requests, rollback, pinned/staging/params refusal and reuse.

The admitted direct worker's monitor sent STOP on its intentional completed-step
threshold. Exit 42 preserved a full step-10,103 checkpoint with 103 committed
exposures / 1,269,580,608 pixels. The unchanged original-source oracle passed
row clocks, frozen state and all 1M included/excluded IDs, including the 64-row
tail. This was an incomplete run, not completion of its step-20,000 target.
Its conservative owned reservation peak was **1,939,351,904 B** and sampled
host RSS maximum **944,439,296 B**.

The dev Vulkan build, 17/17 headless checks, actual AMD native CLI lifecycle,
FPBO numerical check and runtime cap/cleanup checks passed. Read-only review
found and repaired reservation-peak underreporting and throwing shutdown after
invalid cap input; verification exercised the corrected native paths.
These facts prove the new instrumented runs, not retroactive Q cap compliance
or a physical VRAM-peak guarantee.

The separately profiled four-update all-owned worker saved step 10,004 and
passed the original-source oracle. At pool reconciliation, actual backing was
**1,855,830,528 B**, exactly comprising 1,787,672,928 B device, 160 B pinned,
67,108,864 B pageable staging and 1,048,576 B params storage. The requested
pool-plus-scratch / runtime-device reports were 1,704.02 / 1,704.86 MiB
(rounded); their approximately 0.84 MiB difference is nonpool attribution,
not another backing allocation. The 32.76 MiB alias arena is included once
in pool capacity. Pool/scratch views are not added to the backing ledger.

### D2 — actual-source snapshot/reset smoke

At a fresh owner-0 step-10,001 live boundary, the opt-in native diagnostic
captured **379,983,112 B** of independent typed state. Real host-counter and
owned-device mutations were detected; independent restoration was exact.
All four native arms completed 64 updates with identical actual input payload
digests, exposures/settings and exact starting states.

Immediate native save/load and the 1-/8-update decoded screens passed.
At 64 updates, B/C1/C2 had **76,246 / 82,362 / 96,463** decoded screen errors
against A, with zero clock or frozen/unowned invariant errors. Thus no-save
variability already exists in this bounded smoke; these are not late-history
results or proof of a unique cause of Q's longer failure. The same `1e-4`
screen was retained. Exit 42 means the finite probe completed, not recovery
qualification or group completion.

Synchronous native decoding permits exact sampler reset; native A's timing
observations are replayed to hold adaptive settings equal. B uses the native
serializer/load and original owner contract, with sampler reconstruction checked
separately. In-process reload does not prove all process-restart/driver state.
The declared late-state probes completed as follows.

### D2 — late live-owner counterfactuals completed

Both boundaries came from genuine fresh native prefixes under the original
marked owner, 30,000 horizon and unordered raster path: owner 0 reached
**15,040** after 5,040 exposures / 62,207,037,504 pixels; owner 1 reached
**15,002** after 5,002 / 61,747,800,768. Both prefixes match the accepted
original Q lineage. Each had independently reset A/B/C1/C2 starts and 64
matched actual input-payload digests, dimensions, settings and exposures per
arm. Each owner-arm processed 792,018,432 pixels. Native source oracles
validated both full S checkpoints, immutable ownership, clocks and row coverage.

Immediate native save/load was byte-exact for serialized logical state in both
owners. Projection radii and unused packed SH padding remain explicit
reconstruction/storage diagnostics, not invented serializer failures.
All probes had zero metadata, row-clock, unowned/frozen-state or nonfinite-bound
errors. The unchanged decoded screen produced:

| Live state | Arm against A | Immediate | 1 update | 8 updates | 64 updates |
|---|---|---:|---:|---:|---:|
| Owner 0, 15,040 | B, save/load | 0 | 0 | 1 | 62,468 |
| Owner 0, 15,040 | C1, no save | 0 | 0 | 0 | 59,789 |
| Owner 0, 15,040 | C2, no save | 0 | 0 | 0 | 80,442 |
| Owner 1, 15,002 | B, save/load | 0 | 0 | 0 | 29,378 |
| Owner 1, 15,002 | C1, no save | 0 | 0 | 1 | 35,917 |
| Owner 1, 15,002 | C2, no save | 0 | 0 | 1 | 33,521 |

Counts are decoded elements outside the original `1e-4` screen, not raw-byte
differences. Raw/decoded divergence was already observable at one update;
owner-0 B first crossed the screen at eight updates in `world.quats`, whereas
owner-1's no-save repeats crossed first. All B/C arms failed at 64 updates.
Field metrics, bounded first mismatches, host/schema inventories and payload
hashes are retained in `D2-late.causal-analysis.json/.txt` and the native reports.

**Classification: native no-save variability with asymmetric probe failures;
no immediate serialization loss demonstrated.** Saving/loading is not necessary
for this bounded divergence. The asymmetry does not isolate or exclude an
additional continuation/restart effect, identify the internal reduction/optimizer
origin, or prove the unique cause of original long-Q failures. In-process reload,
synchronous decoding and native-A timing-input replay remain explicit confounds.
Original Q decoded recovery stays **FAILED**, not repaired or excused.

Both finite probes exited 42, restored S exactly and reported completed=true,
accepted=false, recovery_qualified=false. Sampled native-tree RSS maxima were
2,102,214,656 / 2,156,785,664 B; these earlier reports exclude the monitor itself
and are not retroactively relabelled whole-host peaks. Subsequent monitored
runs include the monitor's own RSS and separately admit verifier workspace.

### D3 — equal-per-row quality counterfactual completed

The fresh all-owned continuation kept the original 30,000 horizon and stopped
normally at **20,000**, with exit 42, no monitor STOP and a full native checkpoint.
It committed **10,000 exposures / 123,433,689,024 pixels**. The original-source
oracle passed, including all 1M original row clocks/IDs and the 64-row tail.
The state SHA-256 is
`d0da008f160e275b30e8df9b4863aa5b9a5484d885fa5ab4f524ab86172cf429`.

The integrated evaluator changed from its baseline build, so **all three**
models were freshly evaluated with the same current binary, source, settings
and exact 132 named held-out slots. The retained B2 and Q-control models each
reproduced all six original per-view arrays exactly; all 132 six-metric view
fingerprints are distinct. This additionally binds the reused named slot order.

| Mean held-out metric | Fresh all-owned 20k | Retained B2 20k | Retained Q control 30k |
|---|---:|---:|---:|
| L1 | 0.078694 | 0.091200 | 0.079798 |
| PSNR, dB | 20.765146 | 19.090783 | 20.693150 |
| SSIM | 0.807975 | 0.789371 | 0.809307 |
| Color-corrected L1 | 0.035317 | 0.048526 | 0.034570 |
| Color-corrected PSNR, dB | 25.290608 | 22.932693 | 25.467073 |
| Color-corrected SSIM | 0.842222 | 0.822982 | 0.843811 |

| B2 minus fresh all-owned 20k | Mean delta | Regressed / 132 | Worst regression slot / delta |
|---|---:|---:|---:|
| L1 | +0.012506 | 106 | 27 / +0.073829 |
| PSNR, dB | −1.674363 | 115 | 49 / −9.985100 |
| SSIM | −0.018605 | 118 | 38 / −0.113074 |
| Color-corrected L1 | +0.013208 | 129 | 23 / +0.044962 |
| Color-corrected PSNR, dB | −2.357914 | 128 | 49 / −7.922000 |
| Color-corrected SSIM | −0.019240 | 130 | 49 / −0.063520 |

Slots are zero-based and worst views are selected after results, not a
predeclared boundary subset. Raw/cc PSNR median deltas were −1.129000 /
−2.050100 dB. Fresh control 20k minus retained control 30k had mean raw PSNR
+0.071997 dB (70 regressions / 62 improvements) and cc PSNR −0.176465 dB
(91 / 41). Raw quality is not monotonically explained by the longer endpoint.
All pairwise six-metric means, medians, counts and per-view identities/deltas
are in `D3-quality-summary.json`, `D3-quality-per-view.csv` and the text report.

**Matching per-row update counts did not remove B2's quality regression.**
Each B2 owner still committed 10,000 exposures, versus 10,000 total in the fresh
all-owned control: B2 aggregate pixels were 246,870,006,336. View weights,
frozen context and differing native trajectories remain coupled. This is
attribution evidence, not an equal-work speed result or proof of one cause.
No application-quality threshold was invented.

### D4 — demonstrated defects fixed and affected contracts verified

The accounting implementation's pending/committed high-water and invalid-cap
shutdown defects were repaired. The diagnostic comparator now distinguishes
immediate serialized logical equality from post-update tolerance, rejects
nonfinite codec bounds before sanitized decoding, validates byte geometry
before stride division and identifies large frozen shared arrays without
inventing Gaussian row numbers. Consumer checks cover these failure boundaries.

The dev Vulkan build and 17/17 headless checks passed; actual supported AMD
runtime refusal/concurrency/cleanup, native CLI lifecycle and FPBO checks passed.
Independent real host/device mutation/reset, both late four-arm native matrices,
the fresh 10,000-update all-owned source oracle and all three evaluations passed
their structural/safety contracts. Numerical failures remain negative evidence,
not test exclusions. Final read-only source/completed-evidence review found no
concrete remaining defect; it did not prove cold-process restart or physical peaks.

No native serializer defect or unique mathematical cause was demonstrated.
Accordingly no training-math repair or fresh long-Q repair qualification is
claimed; the original long recovery failure remains unchanged.

### D5 — final practical disposition and preservation

**Keep native training as the practical default. Keep B2 experimental and
withhold longer-history recovery qualification and release approval.** Its
original cold/reuse 357.438379 / 250.754732 s medians against 431.199014 s native
control, and 17.105938% / 41.847100% wall reductions, remain valid bounded
historical evidence. They do not demonstrate comparable-quality acceleration.
O1 remains parked; tested G1 remains closed.

New monitored launches retained the original caps. Their largest conservative
worker backing reservation peak was **2,132,134,672 B** in the fresh control,
well below 24 GiB. D3's maximum sampled
native-tree-plus-monitor RSS was **6,778,662,912 B**, maximum new-live logical
output **3,938,138,081 B**, and minimum sampled free scratch **13,727,539,200 B**.
Earlier native-tree-only measurements retain that narrower scope. Separate
verification workspace was conservatively admitted; no whole-investigation RSS
or physical VRAM peak is claimed. Worker-owned backing counts and driver-budget
samples are not substituted for physical peaks or capacity evidence.

Final rehash preserved **40/40 protected evidence files**. The build-tree
executable intentionally changed from baseline SHA-256 `3c94e293…` to the
integrated `166d31fb…`; both identities and required fresh evaluations are bound.
Original Q failures, source/checkpoints, selected B2 outputs and archive bases
remain intact. Commands, binaries, schemas, ownership masks, inputs, native logs,
checkpoint oracles, numerical reports and metric arrays are retained in the
local causal manifest. Master, qualification, scorecard, usage and testing
documents point to this disposition. No tolerance/objective/raster change,
new architecture benchmark, production acceptance, CUDA/NVIDIA work or push occurred.
