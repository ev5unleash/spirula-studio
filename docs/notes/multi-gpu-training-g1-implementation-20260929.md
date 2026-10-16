# G1 execution record and staging/context cost plan — source-native FPBO

Status: **CLOSED: C0–C4 executed on 2026-09-30; C3 correctness passed, C4 performance admission failed. No 1,200-exposure long repeats are reopened and no current cost candidate is open.** The original bounded proof and three ordered-path full-repeat observations remain closed. The [dated scorecard](multi-gpu-training-comparison-20260929.md#completed-stagingcontext-cost-continuation--c4-failed) records the fresh resident-speed comparison; the [roadmap](multi-gpu-training-plan.md#10-benchmark-selection-and-separate-release-gates) and [M0 protocol](multi-gpu-training-micro-01.md) separate benchmark from release acceptance.

**Future-evaluation authority (2026-10-07):** The
[selected raw dataset policy](multi-gpu-training-plan.md#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
governs all future evaluations: freshly admitted full-resolution `.insv`
originals, not `.lrv` previews or existing LRV-derived images/masks.
Kitchen and Source v5/readback 2,688/298 acceptance, measurements and protocols
are historical, not admission for the new selection. Fresh input/source/binary
bindings and matched native references are required; execution remains blocked
pending current approval within the master's unchanged execution/custody budget.

Subsequent [O1 native residency/writeback screen](multi-gpu-training-o1-screen-20260930.md)
completed diagnostics but stopped before O2: O1 remains unverified with a
precise missing-mechanism handoff, not an admitted eviction/reload plan.
This does not reopen any G1 gate or long repeat.

**Evidence availability (2026-10-07):** User-approved retirement removed
**41 owners /63 paths /10.829GiB** of selected positive timing/transport raw
outputs. Exact historical raw packet/native-state/FNV replay is unavailable
for those results; authentic command/measurement/receipt records remain.
First failures and mixed failed-child/native-readback source/smoke witnesses
remain exact. The protocol below is historical, not proof that retired payloads
can be reconstructed. See the
[retention boundary](multi-gpu-training-plan.md#temp-allocation-and-historical-retention--2026-10-07).

## Decision and bounded deliverable

Implemented the narrow external reduced-float-Gaussian-gradient input to the
**existing native FPBO optimizer**, proved source-layout/owner update parity,
and measured the synchronous two-device whole-image proof. B1 retains its
historical measured lead, not production selection. This file-staged G1
schedule is not advanced as a speedup; its result does not exclude other
transports or establish oversized-scene capacity.

The completed gated sequence was:

1. Native FPBO external-gradient input and resident single-device parity.
2. One and multiple versioned, two-owner updates on the actual source, including partial codec blocks and safe stop/readback.
3. Actual packet/resource observations and three complete fixed-work timing repeats, only after parity passes.

The numerical contract was not weakened, and no checkpoint conversion,
synthetic transport substitute or B2 implementation entered acceptance.
The original protocol below is retained for reproduction, not an open
implementation checklist.

| Gate | Completed result |
|---|---|
| 1 — native FPBO input | Source-native and same-input update parity passed. |
| 2 — versioned owners | Source/multi-image and edge cases, partial codec tail, unchanged context, rejection and native stop/readback passed. |
| 3 — resources and timing | Separate resource smoke and three unprofiled repeats per method completed; G1 lost to the fresh matched resident control. |

Complete command-wall medians were **801.617 s resident / 5,640.418 s G1**
(**7.036× slower G1**). Both methods measured 1,200 primary exposures and
14,811,505,344 pixels; paired versus sequential optimizer trajectories differ.
Historical B1/control medians are not matched peers for this ordered-path
schedule. True host/device/pinned peaks and physical disk bytes remain unknown.

## Completed staging/context cost continuation — C0–C4 CLOSED

**Decision: C3 PASS / C4 FAIL (noncompetitive).** All gates were executed;
the candidate lost the bounded performance gate, so no new long repeats
were run or authorized. Accepted observations and numerical failure history
remain unchanged; selected positive raw outputs were retired as noted above.
Another cost change would require its own C1 justification and C2–C4 evidence,
fresh selected-data admission and current approval; no current candidate is open.

Exact commands, source/config/patch/binary hashes, device identities and
artifact bindings are local in `continuation.manifest.json`, with
`packet-inventory.json` and `bounded-results.json`. Public implementation
references are [the native runner](../../src/backend/tests/engine/g1_native_proof.cpp),
[the FPBO seam](../../src/engine/EngineOptim.cpp) and the
[original protocol below](#retained-c0c4-protocol-for-reproduction).

| Gate | Executed result |
|---|---|
| C0 — bind/admit | Source/config/binary and all nine original implementation-file hashes matched. Initial scratch 45.98 GB; two supported AMD R9700 devices, driver 26.Q3 / Vulkan API 1.4.349, each budget 33,250,242,560 B and usage 0 B at admission only. |
| C1 — actual inventory | 223 field/direction/operation rows; 15 real packets verified with FNV and SHA-256. All three original accepted G1 ledgers reconciled to 2,274,354,841,120 B written and equally read per run. |
| C2 — one isolated reduction | Kind-12 owner returns omit frozen fields after the first 20 mutable native fields, replacing them with an 8 B actual-GPU-frozen digest. Source/context refresh is unchanged; final kind-16 full state and native checkpoint remain complete. |
| C3 — correctness/failures | Corrected Vulkan build, packet checks, native FPBO and six source updates on views 1188,1165,760, edges, stop/readback and both timing smokes passed. Altered-worker stale identity/version/digest and missing-owner refusals passed; 14/14 headless, no exclusions. |
| C4 — bounded cost | Six alternating fresh 120-exposure runs exited 0; G1 lost outside the observed ranges: 5.498× measured and 7.147× complete wall versus fresh native-resident control. Long-repeat admission failed. |

### Reconciled inventory, delta and integrity

The original five payload categories in C1 below leave exactly **56,160 B
identity + 2,187,136 B framing**: 5,620 headers / 629,440 B and 194,712
length words / 1,557,696 B. Frozen-state return totals 123,083,219,648 B,
including the retained 175,082,816 B checkpoint return. Only kind 12 omits
122,908,136,832 B; at the original 1,200-exposure horizon the new attestations
add 11,232 B and 29,484 fewer length words save another 235,872 B:
**net logical-byte reduction 122,908,361,472 B**, not physical disk savings.
At the unchanged 120-exposure schedule, the derived reduction is
**28,363,468,032 B**; the bounded write/read ledger was independently
reconciled. Final source/config rehashes remained unchanged.

The parent retains immutable authority. The worker digest covers actual GPU
frozen bytes and is bound to source/config, attempt, topology, layout,
version, step and owner; both owners must validate before publication.
Worker hashing is charged to the native phase. No tolerance, optimizer,
codec, row ownership, source refresh or final readback contract was relaxed.
Full timing now supports positive even `--measured-exposures` (default
1,200), retaining 101 warmup exposures and one final update; smoke is
unchanged. Fourteen invalid-option cases rejected before output creation.
`--raster resident` is accepted only for full control timing; ordered raster
remains the numerical oracle and G1 path.

### Bounded timing and accounting

All six unprofiled runs matched **222 actual primary exposures /
2,706,794,496 primary pixels**, image/face/dimension manifests and
**120 measured exposures / 1,463,132,160 measured pixels**.
G1 completed 162 updates (60 measured pairs), clock **10,162**; control
completed 222 updates (120 measured sequential), clock **10,222**.
Trajectories differ. Effective raster was **ordered G1 / native-resident
control**, deliberately not equal raster settings; this is not a comparison
against historical B1 or the original ordered full timing.

| Repeat | G1 complete outer wall (s) | Control complete outer wall (s) | G1 measured (s) | Control measured (s) |
|---|---:|---:|---:|---:|
| 1 | 1,193.8296401 | 165.4116338 | 483.8473523 | 85.7383396 |
| 2 | 1,181.4037324 | 165.2343851 | 471.4147791 | 85.7106115 |
| 3 | 1,181.1578243 | 165.3048309 | 471.1778352 | 85.8431813 |
| Median | **1,181.4037324** | **165.3048309** | **471.4147791** | **85.7383396** |
| Max − min | 12.6718158 | 0.1772487 | 12.6695171 | 0.1325698 |

Median measured rates are **3.92845649 / 0.71448616 s per exposure**
(G1/control). Ranges are separated in the wrong direction; no window
advantage exists. Complete outer wall includes subprocess lifecycle through
native checkpoint/readback and exit.

| Lifecycle median (s) | G1 | Control |
|---|---:|---:|
| Source startup/priming | 2.3564437 | 2.4305189 |
| Worker startup | 2.7584634 | 0 |
| Warmup | 689.7928880 | 72.6766556 |
| Final update | 6.8502611 | 0.7874096 |
| Checkpoint/readback | 6.5246553 | 2.5680690 |
| Worker shutdown | 0.1633725 | 0 |

Parent write/read-validation medians are **435.2941085 / 209.7876349 s**.
They overlap worker and barrier intervals, not additive elapsed time or
credited savings. **Extrapolation only:** scaling each observed measured
window by 10 while retaining its observed other lifecycle yields median
1,200-exposure totals **5,424.1367443 s G1 / 937.0566902 s control**.
This assumes linear per-exposure cost at a new image mix despite differing
optimizer trajectories; these are not observed full runs.

### Resources, repeatability and exclusions

Each bounded G1 run wrote and read **479,164,517,728 logical packet bytes**:

| Category | Bytes per run |
|---|---:|
| Source native context | 195,287,076,400 |
| Frozen context sent | 56,901,915,200 |
| Owner native return | 97,943,979,856 |
| Frozen final/checkpoint return | 175,082,816 |
| Frozen attestations | 2,592 |
| Float gradients, both directions | 128,856,000,000 |
| Identity | 12,960 |
| Framing | 447,904 |

Full-repeat sampled simultaneous RSS maxima were **5,578,842,112 B G1 /
1,257,558,016 B control**, below 8 GiB; active-root outputs were about
**1.399 / 1.014 GB**, below 10 GiB. Separate `SS_PROFILE=1` G1 smoke exited
0; live-pool sampled maxima were **4,836,967,312 B parent / 4,844,469,088 B
worker 0 / 4,843,104,688 B worker 1**. These are observations, not true
host/device/pinned peaks. Physical disk bytes remain unknown; Windows
process transfer counters include files/devices/console, not physical disk
traffic. No capacity, quality, recovery or production approval follows.

G1 final canonical/frozen/stable-ID payloads repeat byte-exact across all
three runs after excluding the 112 B attempt header. **Native-resident
control payloads do not repeat byte-identically**; no equality is asserted.
Unordered raster nondeterminism is an inference, not an established cause
of this discrepancy. Independent native checkpoint readback passed for all
six; cross-method final equality is neither required nor claimed.
Failure injection was exercised separately in C3, not in accepted timing.

The initial packet-capture watcher denied Windows deletion (exit 1);
corrected `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` capture
passed a fresh smoke (exit 0). Initial build launch used the wrong working
directory (exit 1, no build); explicit-directory build exited 0. Neither
failed attempt is an accepted numerical or timing result.

## Retained C0–C4 protocol for reproduction

The following is the original admission/reproduction protocol, **not a
pending checklist**. C0–C4 were executed above; the C4 failure branch closed
this candidate without reopening 1,200-exposure repeats.

### C0 — bind inputs and admit resources

- Read the local `g1-fpbo-implementation-20260929.manifest.json` under the
  platform temporary `opencode` directory. Resolve `checkpoint`, `devices`,
  `gate_1.command`, `gate_2.source_run.command`, `gate_2.edge_run.command`,
  `gate_3.accepted_runs[].command` and `final_verification.packet_check.command`.
  Keep private paths and device UUIDs local.
- Rehash the source and config against the manifest and the identity contract
  below. Record current source revision/patch, executable hash, toolchain,
  driver and both supported non-NVIDIA devices. Rebuild through the dev
  entrypoint after code changes; never run an old executable against new code.
- Create a new attempt/output directory and a continuation manifest linked
  to the original evidence. Preserve accepted outputs and failure packets.
  Require at least 10 GiB free scratch for a sequential attempt, enforce the
  existing 10 GiB live-output and 8 GiB sampled aggregate-RSS caps, and record
  current per-device headroom before allocating. If transient allocation
  estimates do not fit, stop rather than weakening checks or using NVIDIA.
- Confirm the selected run's `result.tsv`, `schedule.tsv`, `exposures.tsv`,
  `worker0.phases.tsv`, `worker1.phases.tsv`, `rss.tsv` and final native
  outputs exist. Full timing deletes consumed `.in`/`.out` packets: their
  absence is expected, not proof that packet bytes were retained. Check
  Gate-2 artifact roots separately before using their packets.

**Exit artifact:** identity/resource admission and an availability inventory.
Missing source/config or required evidence blocks dependent work; a regenerated
source requires new identities and fresh matched controls, not old medians.

### C1 — inventory actual bytes and required refreshes

Audit [g1_native_proof.cpp](../../src/backend/tests/engine/g1_native_proof.cpp):
`source_state`, `statistics`, `shared_state`, `Header`, `packet`, `receive`,
`receive_once`, `timing_packet`, `timing_receive`, `timing_worker` and
`fixed_work_timing`. Trace worker capture, owner update, checkpoint and the
parent merge/publication boundary. The current first-20-field native/frozen
classification is a source-schema convention, not a portable field contract.

Produce one local inventory row per field/direction/operation with:
field name/layout, actual bytes and packet count, producer/consumer,
authoritative owner, mutation boundary, consumer read, required refresh
frequency, retention lifetime, measured phase cost and evidence path.
Separate render inputs, owner values/moments/bounds/bias, frozen
statistics/appearance, float gradients, identity/framing and checkpoint-only
state. Zero visible gradient does not imply zero optimizer-state mutation.

Reconcile the unchanged runner's ledger against the retained per-run logical
write total **2,274,354,841,120 B** (and equal logical reads):

| Payload category | Recorded bytes |
|---|---:|
| Source native state | 844,241,053,360 |
| Frozen context sent | 245,991,356,480 |
| Owner native-state return | 422,420,968,336 |
| Frozen-state return | 123,083,219,648 |
| Float gradients, both directions | 638,616,000,000 |

Account for the remaining identity/framing bytes explicitly. Use retained
real packets where available; if a required timing payload is absent, capture
one bounded native operation in the existing runner, outside timed runs,
rather than substituting synthetic packets or retaining an entire long run.
Compare serialized field sizes and hashes with ledger counts.

**Exit artifact:** reconciled inventory plus one proposed reduction, its
semantic proof obligation and the measured cost it targets. Parent
write/read-validation medians were **1,890.877 / 1,184.375 s**; worker totals
and barrier wall overlap these and cannot be added or subtracted as savings.
Unchanged context residency or smaller returns are candidates, not conclusions.
If no justified reduction exists, close the candidate here.

### C2 — implement one reduction and preserve the protocol

Change only the selected cost in the existing persistent-worker runner and
native FPBO path. Do not combine a refresh change with a transport replacement,
gradient compression or a different optimizer. Record before/after actual
payload counts and residency; all retained state and transient copies remain
charged. No general transport framework, speculative overlap or retries.

For any cached field, bind its validity to source/config, attempt, topology,
layout and the applicable model version; prove which operations leave it
unchanged. An omitted return needs an explicit authority and validation
argument, not the assumption that the receiver already has the right bytes.
Preserve stable row IDs, 256-row ownership, the 64-valid-row tail, weighting,
one global update clock, unchanged unowned context and two-owner completion
before publication. Do not remove payload integrity or rejection checks to
make validation cheaper.

**Exit artifact:** one isolated candidate patch, executable identity and the
inventory delta, with no performance claim.

### C3 — strict correctness and failure gate

Build once through `build_develop.bat -DSS_BACKEND=vulkan
-DSS_BUILD_BACKEND_TESTS=ON`, then run the existing command forms in
[Build, evidence and completion](#build-evidence-and-completion), resolving
arguments from the local manifest:

1. `--packet-check`, then `--fpbo` and `--fpbo-two-owner` on views
   **1188,1165,760**, including the existing multi-update/multi-image checks.
2. `--fpbo-edges` and `--fpbo-stop-check`: empty/overlap/tie/invisible-row
   behavior, partial tail, unowned/frozen equality, completed-boundary native
   checkpoint readback and failed-child/no-partial-publication behavior.
3. `--fpbo-timing-smoke` for both `--method g1` and `--method control`, each
   with a new output directory. Extend the existing smoke/rejection cases
   where the chosen change introduces a cache lifetime or omitted payload:
   exercise changed identity/version, stale reuse and a failed/missing owner.
4. Run `cmake -E chdir build_vulkan ctest -L headless
   --output-on-failure --no-tests=error` after the candidate is complete.

Retain the current strict native and same-input canonical parity contract;
record numerical errors and packed-state/context equality separately.
The candidate's actual changed worker path must be exercised, not just an
unchanged oracle. Compare payload state independently of attempt headers.
Any mismatch, rejected valid update, accepted stale contribution or partial
publication stops timing. Do not relax tolerances, substitute reference
gradients, alter Adam or hide a failure with checkpoint conversion.

**Exit artifact:** commands, actual subprocess exit codes and per-case results,
plus native readback evidence. A null supervisor exit code is not success.

### C4 — bounded native cost comparison and decision

The existing `--fpbo-timing-smoke` is a protocol/failure smoke, **not a short
benchmark**. `--fpbo-timing --measured-exposures N` now selects a positive
even measured count, defaulting to 1,200; malformed, duplicate, overflow and
original-horizon-exceeding counts are rejected before output creation.
The schedule retains 101 warmup exposures and one final single-image update.
Smoke semantics are unchanged. Actual schedule/exposure/clock accounting is
checked before final checkpoint publication.

Predeclare **120 measured exposures, three fresh process repeats per method**,
restarting from the same source, alternating candidate/control execution.
Use ordinal-modulo-1,195 image selection and record actual faces, dimensions,
pixels, weights and clocks. This gives 60 measured G1 updates versus 120
sequential control updates; it does not assert equal optimizer trajectories.
Resolve the existing `--fpbo-timing` command arrays from the manifest, choose
new outputs and append `--measured-exposures 120`.

Keep the ordered native numerical oracle separate from the best applicable
resident speed comparator. `--raster ordered` remains the strict-proof default;
only full control timing accepts `--raster resident`, selecting native unordered
raster before priming and recording the effective choice.
Do not slow a comparator for numerical convenience. Match source, primary
exposures/pixels, configuration and lifecycle boundaries, and disclose the
paired/sequential semantics. Historical B1 medians are context only.

Record complete subprocess wall and separate startup/priming, warmup,
measured critical path, final update, checkpoint/readback and shutdown.
For preparation, validation, refresh, capture, update, synchronization and
publication, retain actual phase intervals, payload bytes and sampled
resources. Do not sum overlapping worker ledgers, credit hypothetical overlap,
or describe logical bytes as physical disk traffic. Keep `SS_PROFILE=1`
resource smoke separate; timing must have neither profiler spelling set.

**Admission to long repeats requires all of:**

- C3 passes and all bounded runs exit successfully with matching actual
  primary exposure/face/pixel manifests and completed native readbacks.
- Candidate measured seconds/exposure beats the fresh speed comparator
  outside the observed three-run ranges (candidate maximum below comparator
  minimum). Overlap or a loss closes this proposal as noncompetitive.
- Measured startup, warmup and finalization are explicitly priced for the
  declared 1,200-exposure schedule. Any projected total is labeled an
  **extrapolation**, with assumptions, not an observed long-run speedup.
  If that accounting erases the window advantage, do not reopen long timing.
- Live-output/RSS caps hold; unknown true host/device/pinned peaks stay
  unknown. No capacity or quality claim follows from this fitting source.

On pass, reopen the existing three unprofiled 1,200-exposure repeats per
method with fresh matched controls and full lifecycle accounting. On failure,
record the stopping gate and retain B1's historical measured lead; another
cost change needs its own C1 justification and C2–C4 evidence.

**Completion artifact:** update this execution record and the scorecard with
inventory, candidate, correctness, all repeats/spread, resource limitations
and the stop/reopen decision; link exact commands/hashes/artifacts in the
local continuation manifest. B2, O1 implementation, G2, production recovery,
capacity and quality acceptance remain outside this plan.

## Starting evidence and identity

The scorecard is the authority for observed results and full hashes; keep commands, private paths, GPU UUIDs and artifact identities in the local experiment manifest, not this repository.

| Existing evidence | What it does not prove |
|---|---|
| Regenerated-source control **427.351 s**, B1 **337.416 s cold / 232.915 s reuse**, three-repeat medians | Production selection, equal optimizer clocks, capacity gain or time-to-quality |
| Three-step synthetic non-FPBO G1 parity | Native source-FPBO optimizer updates or partial-block correctness |
| Actual-source low/median/high nonfused gradient capture | FPBO owner updates; 236,000,000 B/view is captured dense gradient storage, not routed packet traffic |
| Completed pattern staging and local-copy measurements | Actual G1 packet protocol, overlap, shared-state returns or training wall |

Revalidate the retained native step-10k source against scorecard SHA-256 `1986EADAEEE4FE3EB86883CCB436797A2D63A01F7ACA9BEF2273EF64EFA507F5` and its config hash before writing outputs. Preserve the original 30k learning-rate horizon, 3DGS SH3, quantization level 1, packed projection and effective FPBO. There are 1,000,000 source rows, 992,850 finite means, and 7,150 inactive/nonfinite rows. Preserve row IDs and native storage; the diagnostic finite-row ranking does not authorize compacting the checkpoint.

Do not pool historical-source measurements with this source. If it must be regenerated, record a new identity and common preparation cost, and obtain matched baselines rather than attaching old medians to a new checkpoint. The recorded 61.74 GB free scratch is historical, not current admission evidence.

## Native code map

Read [Vulkan backend guidance](../../src/backend/vulkan/README.md) and [SH layout invariants](sh-quant-layout.md) before changing the seam. Reuse these locations; no second trainer, optimizer or generic transport framework:

| File / existing seam | Implementation responsibility |
|---|---|
| [EngineDataManager.cpp](../../src/engine/EngineDataManager.cpp): `_resolve_split_vs_fpbo`, `engine_resolve_managed_step_config` | Record the executed split/FPBO choice. Unequal faces or accumulation can disable FPBO; requested flags are not evidence. |
| [EngineTrainStep.cpp](../../src/engine/EngineTrainStep.cpp): `_engine_train_step_after_setup`, `_engine_step_optim_and_densify` | Preserve forward/loss/update ordering and one global update boundary. |
| [EngineLoss.cpp](../../src/engine/EngineLoss.cpp): `_alloc_grad_buffers`, `_engine_raster_proj_backward`, `engine_backward_from_render_grad` | Supply native float data gradients without silently selecting standalone optimizer state. Existing FPBO paths omit full Gaussian gradient buffers. |
| [EngineOptim.cpp](../../src/engine/EngineOptim.cpp): `_ensure_optim_state`, `engine_fused_proj_bwd_optim_step` | Keep native FPBO allocation, codecs, schedule and state; pass the alternative gradient input. |
| [FusedProjectionBwdOptim.cpp](../../src/backend/vulkan/kernels/FusedProjectionBwdOptim.cpp): `launch_fpbo_vk` | Extend the native launch contract minimally; retain internal-VJP execution as the default. |
| [fpbo.slang](../../src/backend/vulkan/shaders/fpbo.slang): `fpbo` | Choose the data-gradient source before the existing regularizer/Adam/codec update. Share that update body, not a copied optimizer. |
| [Tensor.h](../../src/core/Tensor.h): `sh_fpbo_cells` | Preserve FPBO addressing, padded storage and per-block bounds. |
| [g1_native_proof.cpp](../../src/backend/tests/engine/g1_native_proof.cpp) | Extend the existing proof for source-FPBO input/update/owner parity; retain useful synthetic edge cases. |

Inspect references before changing exported declarations, migrate every affected caller and regenerate generated declarations through their owning codegen tools. Do not hand-edit generated API surfaces. Shared source changes needed by Vulkan are allowed; no CUDA implementation/build or NVIDIA Vulkan validation is part of this work.

## Gate 1 — native numerical input, one device

Define the input at the Gaussian **float data-gradient** boundary, with explicit attribute shapes, row indexing, reduction order, weighting and scaling. Preserve the existing internal projection-VJP path and source checkpoint layout. No packed-code summation, layout override or conversion fallback.

Keep the existing FPBO regularizers, screen-size contribution, scale-agnostic mean behavior, color trust region, Adam betas/epsilon, learning rates, bias correction, values, moment codecs and bounds. Specify where each contribution enters so it is applied exactly once. Invisible or zero-data-gradient rows can still update through regularization and carried Adam history; visibility cannot be an update mask.

First use one effective-FPBO image/update with fixed image/face IDs, source loss weights, deterministic background and tile-skip policy, frozen topology and frozen shared appearance. The existing kitchen diagnostic disables random background and tile skipping; record such overrides rather than treating diagnostic configuration as the untouched source configuration. For parity, both arms must use identical overrides.

Prove two comparisons from identical source state:

- **Same-input reference:** resident all-owned external-input FPBO versus the owned external-input FPBO path supplied with the same reduced float gradients. This isolates ownership, state addressing and update semantics.
- **Native-path reference:** external-input FPBO versus source-native internal-VJP FPBO with matched data gradients, weights and exactly-once scaling. This prevents two copies of the same input mistake from passing the first comparison.

Gate 1 passes only after one and multiple updates agree for native render/loss, float gradients, parameters, packed values/moments, codec bounds and bias counters. Use existing numerical tolerances where applicable and record them before judging results. Require byte equality for same-input canonical updates and unchanged context. For internal-VJP versus nonfused float-order differences, report numerical errors and every resulting code/bound mismatch explicitly; do not silently relax packed-state acceptance or declare parity from visually similar renders.

## Gate 2 — bounded versioned two-owner step

Assign whole **256-row codec blocks**, stable original row IDs and one authority per mutable row. The source has **3,907 blocks; the last has 64 valid rows**. Preserve valid tail lanes, padded storage and native block reductions. The illustrative contiguous 499,968 / 500,032-row split is usable for this proof; it is not B1 spatial ownership.

One synchronous step:

1. Fix source/config identity, attempt, topology/model version, global step, image/face batch and weights before rendering.
2. Obtain each image's required world inputs across owners and perform native global depth/tile ordering. Never compose independently rendered shard RGBA as a replacement.
3. Route float data gradients to the authoritative blocks and reduce all required contributions before applying one native FPBO update. Owners share one global clock; all valid owned rows retain native history/bias behavior.
4. Publish the next version only after all required owners finish. Refresh render context from that version; unowned optimizer/context bytes must not change during an owner's local update.

Start with bounded host-staged binary payloads. Validate source, attempt, step/version, topology, owner, layout, row ranges and payload length before accepting a contribution; reject duplicate contributions as well as stale, mismatched and truncated packets. Do not introduce tensor JSON, speculative overlap, P2P requirements, retries or independent worker optimizer clocks.

Declare every shared/image-specific parameter and statistic either frozen, owned or reduced. Frozen appearance still participates in forward rendering. Topology freeze does not by itself specify radii/accumulation/statistic behavior: document and match the reference's treatment. Report actual return bytes; unmeasured is not zero.

A fitting full-world replica may serve the initial parity oracle, not a scalable candidate-discovery design or capacity result. For a candidate timing, identify actual owner/worker residency and charge discovery and context refresh. Full replication must be explicit if retained; never describe its memory as `N / GPU_count`.

Required cases, extending the existing proof rather than duplicating it:

- Actual source low/median/high views **1188, 1165, 760**, one and multiple updates, plus matched multi-image reduction after the single-image seam passes.
- Empty-image worker / empty owner visibility, overlap, unequal image sizes, exact depth ties and invisible rows with carried moments.
- Last partial block owned and updated, valid tail state checked, unowned bytes unchanged during each owner update.
- Stale/source/version/attempt/layout mismatch, duplicate and truncated contribution rejection before version publication.
- Cooperative stop at a completed global boundary, native output readback and source identity/state validation. A worker failure must not publish a partial next version.

A bounded proof may publish validated per-owner artifacts; do not label them a globally merged resumable checkpoint. Production retry/recovery and merged resume are separate gates. Gate 2 requires the same numerical/state comparisons as Gate 1 against a resident external-input reference using the identical batch, reduction weighting and global clocks.

## Gate 3 — real resources and fixed-work timing

The executed gate required fresh scratch admission, bounded live outputs,
host/staging limits and supported-device budgets before checkpoint-heavy
work. Dense gradients, full world inputs, render/codec scratch and transient
copies remained charged; source outputs were not overwritten.

Real routed world, float-gradient and frozen-state bytes, separate backend
copy/synchronization profiles, sampled RSS and phase/device observations are
recorded in the scorecard/local manifest. Pool samples are not true peaks;
unavailable host/device/pinned peak telemetry still limits memory claims.

Executed timing protocol: **three unprofiled 1,200-measured-image-exposure repeats per method**, each restarting from the same source:

- Predeclare the batch schedule, weights, warmup and timed boundaries. Record actual image/face IDs, dimensions, pixels and global optimizer update count. Match exposure accounting without pretending two images in one synchronous update equal two sequential updates.
- Include required cold discovery/preparation, startup/input, training, checkpoint-safe stop, output publication and readback/finalization. Report reuse separately. Charge method-specific discovery only to the method needing it; common source generation is separate.
- Report each repeat, median and spread, complete wall, exposure/s and MPix/s. Keep profiling/resource runs separate from unprofiled timing.
- Use fresh matched controls when source/configuration/work boundaries change. The accepted control median is **801.617 s** for this schedule. Earlier **427.351 s control**, **337.416 s B1 cold** and **232.915 s B1 reuse** remain labeled historical context only.

Performance acceptance is measured, not a universal percentage threshold: prefer the lowest complete fixed-work wall within the measured resource budgets; if differences are within observed spread, prefer the simpler design. Losing to B1 or the resident control is a valid G1 result. Parity success is not speed success, and B1's independent objective/clocks must remain separate from G1's synchronous semantics. Do not claim capacity, quality or final architecture selection from this fitting-scene proof.

## Build, evidence and completion

Use the repository dev entrypoint, enabling backend tests if needed:

```bat
build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_BACKEND_TESTS=ON
```

Existing proof modes below are **diagnostics, not new acceptance commands**; `CHECKPOINT` and device selectors must resolve from the local manifest. Run the built `g1_native_proof` executable in its configured output directory:

```text
g1_native_proof --synthetic uuid:DEVICE_A uuid:DEVICE_B
g1_native_proof --kitchen --resume CHECKPOINT --device uuid:DEVICE_A --images 1188,1165,760
```

The implemented Gate 1 acceptance command and CPU packet boundary check are:

```text
g1_native_proof --packet-check
g1_native_proof --fpbo --resume CHECKPOINT --device uuid:DEVICE_A --images 1188,1165,760
```

The implemented whole-image and native FPBO edge commands are:

```text
g1_native_proof --fpbo-two-owner --resume CHECKPOINT --devices uuid:DEVICE_A,uuid:DEVICE_B --images 1188,1165,760
g1_native_proof --fpbo-edges uuid:DEVICE_A uuid:DEVICE_B
g1_native_proof --fpbo-stop-check uuid:DEVICE_A uuid:DEVICE_B
g1_native_proof --fpbo-replay --resume CHECKPOINT --device uuid:DEVICE_A --evidence FAILURE_DIRECTORY
g1_native_proof --fpbo-producer --resume CHECKPOINT --device uuid:DEVICE_A --image 1188
g1_native_proof --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:DEVICE_A,uuid:DEVICE_B --output NEW_DIRECTORY --method g1
g1_native_proof --fpbo-timing --resume CHECKPOINT --devices uuid:DEVICE_A,uuid:DEVICE_B --output NEW_DIRECTORY --method g1
g1_native_proof --fpbo-timing --resume CHECKPOINT --devices uuid:DEVICE_A,uuid:DEVICE_B --output NEW_DIRECTORY --method control
```

The original unordered whole-image and overlap edge attempts failed native
canonical parity despite in-tolerance gradients. Retained native replay
reproduced both distinct saved outputs exactly from their respective input
gradients. A producer probe isolated the first divergence to raster screen
gradients, with identical renders and pixel cotangents.

The proof-only ordered raster path preserves fragment VJP and native update
math; its default-off control is enabled identically in source/edge parents
and children. After correction, the source command passed all six updates,
including three-image reductions and the partial tail; all three FPBO edge
updates passed. Both completed-boundary native checkpoint readbacks and
failed-worker/no-partial-publication checks passed. CPU packet checks,
existing FPBO split coverage and all 14 headless tests passed.

The Windows-only fixed-work runner uses two persistent native workers and
the existing hashed binary-file payloads, with one outstanding version.
Source/world refresh, native capture/update, packet validation, owned merge,
startup, final native output/readback and shutdown are charged. Owner
updates reuse the image's native stash; no extra owner forward/loss is run.
The full loop omits repeated resident oracles and per-step native archives;
smoke checks retain exact comparisons and failed-child publication checks.

Its predeclared schedule is 101 single-image warmup exposures, 1,200 measured
exposures, and one final single-image update. G1 pairs measured exposures
into 600 equal-weight updates (702 total); control uses 1,200 sequential
measured updates (1,302 total). IDs are ordinal modulo 1,195, with identical
ordered exposure IDs across methods. Optimizer trajectories are not equal.
`schedule.tsv` is declared before work; `exposures.tsv` records actual
faces/dimensions/pixels/weights/clocks. `result.tsv`, worker phase ledgers
and sampled RSS retain complete boundaries and real payload accounting.

Use `SS_PROFILE=1` only with smoke for separate resource/copy/sync evidence.
Full timing rejects any present profiler switch, including the legacy alias.
GPU snapshots are phase samples of live backend allocations and estimated
global driver usage, not true peaks. Pinned-host and true simultaneous peaks
remain unknown. Output must be a new directory; scratch/output is bounded
to 10 GiB and sampled parent-plus-worker RSS to 8 GiB. Historical medians
remain context; the new schedule requires three fresh matched controls.


All six accepted full commands exited 0, their primary exposure/face/pixel
manifests matched, and native checkpoint poison/reload checks passed.
Canonical/frozen-state/stable-ID payloads repeat byte-identically within
each method, excluding the distinct attempt headers. End clocks are
10,702 G1 / 11,302 control; cross-method state equality is not claimed.
The original strict source/edge gates, resource smoke, final CPU packet
rejection and 14/14 headless suite remain separate exercised checks.
Explicit supported non-NVIDIA device identities and all commands/hashes
are retained locally. True host/device/pinned peaks remain unknown.
The frozen kitchen proof is complete, not a production architecture,
capacity or quality acceptance. The measured decision retains B1's
historical lead and declines this file-staged G1 schedule as a speedup.

The completion record retained in the scorecard and local manifest contains:

- Source/config/build/patch/device identities, effective layout/mode, frozen-state and gradient/update contracts.
- Gate-by-gate commands, numerical tolerances/errors, packed-state/context results, partial-tail and rejection results, stop/readback outcome.
- Actual bytes, resource observations, timing boundaries, three-repeat totals and semantics-qualified comparison, or exact failed/unavailable gate and evidence.
- Closed decision: retain B1's historical measured lead and decline this staged G1 schedule as a speedup. The retained cost protocol above is a reproduction record; a different G1 proposal needs its own admission evidence.

**Out of scope:** O1 CPU-authoritative implementation; B2/G2; topology growth; retries and production publication/recovery; global merged resumable checkpoints; oversized-scene capacity qualification; held-out quality/time-to-quality acceptance; GUI/distributed-training product controls; general primitive/format support beyond this kitchen 3DGS source; NVIDIA/CUDA work. Existing B1 resume/sidecar/config/view-binding and consumer/boundary gaps remain open, not repaired by this handoff.
