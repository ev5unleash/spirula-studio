# O1 execution record — bounded native residency and writeback screen

Status: **CLOSED, O0–O3 executed on 2026-09-30: O0 passed, O1 diagnostic
completed, O2 NOT ADMITTED, first O3 branch selected. O1 training remains
unverified; no offload/eviction implementation was admitted by this screen.**
This follows [G1 C0–C4 closed](multi-gpu-training-g1-implementation-20260929.md#completed-stagingcontext-cost-continuation--c0c4-closed).
It does not reopen G1, admit B2 or authorize long timing repeats.
The [M0–M4 mechanism continuation](#next-plan--m0m4-bounded-mechanism-proof)
below executed through M0 and rejected its declared 7 GiB schedule before
implementation; M1–M4 did not run. O0–O3 remains closed and unchanged.
The [roadmap](multi-gpu-training-plan.md#6-o1--early-cpu-residency-alternative)
and [scorecard](multi-gpu-training-comparison-20260929.md) retain selection policy
and measured history.

**Current direction: O1 is parked.** Continue the
[bounded B2 implementation](multi-gpu-training-plan.md#5-b2--active-bounded-implementation)
for split-GPU throughput, not another O1 retention or ordered-scratch experiment.
The closed protocols below preserve evidence and do not authorize new runs;
reopening requires the [master plan's evidence gate](multi-gpu-training-plan.md#autonomous-execution-and-completion).

## Decision and rationale

The screen examined whether **one supported GPU with CPU-authoritative native
state and a bounded GPU working set** has a credible next proof on the existing
kitchen source. Actual residency and dirty-state evidence is now retained,
but no coherent bounded discovery/update/gradient-servicing schedule is established.

G1's digest-return candidate passed correctness but lost: measured medians
471.415 s G1 / 85.738 s resident (5.498× slower), complete medians
1,181.404 / 165.305 s (7.147× slower). Its 5.41% logical-byte reduction did
not justify long repeats. That candidate and the original G1 proof stay closed;
these measurements do not rule out all transports.

O1 remains an unverified training/residency alternative, not a predicted speedup.
The scorecard's 312,441,656 B canonical-state subtotal is derived allocation
accounting, not achievable VRAM savings: full world, images, render/codec scratch,
shared appearance and retained pool capacity can dominate. B1 remains a
historical approximate-objective benchmark lead, not a matched O1 comparator
or production winner. No new dataset, quality threshold or oversized failure
is required for this screen.

## Completed O0–O3 execution record

### O0 — admitted identity and resources

**Observed:** the retained step-10k source and config rehash to the original
identities, without regeneration: source SHA-256
`1986EADAEEE4FE3EB86883CCB436797A2D63A01F7ACA9BEF2273EF64EFA507F5`,
config `0AC7B6A45A69F9591082DCF3B601C731758EC334D2C5AAD29DA7DAE5A7CB6FA6`.
One explicitly selected AMD Radeon AI PRO R9700 Vulkan device was admitted.
Device-local budget/usage at admission was 33,250,242,560 / 0 B; free scratch
was 19,553,894,400 B. Limits were 10 GiB live attempt outputs and 8 GiB
sampled aggregate RSS, with at least 10 GiB free scratch before launch.
Before-runtime planning estimates were 3.2 GB host / 6 GB device, not measurements.
The actual retained pool reached 8.02 GB, exceeding that device estimate but
remaining within the admission budget; no monitored cap breach occurred.

Native checkpoint FPBO layout, quantization 1, SH3, source loss weights and
30k learning-rate horizon were retained. Topology and shared appearance were
frozen; random background and tile skip were disabled, with ordered raster.
Private paths, device UUID, exact commands, code/patch/binary/toolchain hashes
and driver binding remain in local `o1.manifest.json`.

### O1 — accepted native diagnostic and accounting

**Observed:** `g1_native_proof --o1-screen --resume CHECKPOINT --device
uuid:DEVICE --output NEW_DIRECTORY` exited 0 in **21.3637501 s diagnostic
wall only**, not a performance sample. Views 1188, 1165 and 760 each ran one
independent source update, then an ordered three-update carried sequence from
that same source. Six additional same-input native internal/external canonical-byte
parity oracle replays were discarded, not additional trajectory updates.
Final native checkpoint reload after deliberate parameter poisoning was exact.
All 1M original rows, including 7,150 nonfinite-mean rows, and all 3,907
blocks (3,906 full plus 64 valid tail rows) were preserved. Source per-row
clocks ranged 99–10000; every corresponding final row is source +3
(102–10003), with global completed boundary 10003.

| Arm / view | Projection candidates | Nonzero data-gradient rows | Changed canonical bytes | Means changed on zero-data-gradient rows |
|---|---:|---:|---:|---:|
| Independent / 1188 | 14,208 | 14,179 | 97,460,773 | 934,189 |
| Independent / 1165 | 139,742 | 121,697 | 103,435,138 | 830,816 |
| Independent / 760 | 822,825 | 744,979 | 123,673,965 | 229,854 |
| Carried / 1188 | 14,208 | 14,179 | 97,460,773 | 934,189 |
| Carried / 1165 | 139,741 | 121,686 | 104,829,397 | 828,274 |
| Carried / 760 | 822,764 | 744,837 | 123,233,657 | 228,779 |

Each arm covers 36,870,336 pixels: 12,192,768 / 12,192,768 / 12,484,800.
Dense captured gradients are 236,000,000 B/update: means 12 MB, quaternions
16 MB, scales 12 MB, opacities 4 MB, DC 12 MB and SH 180 MB.
In **every one of six updates**, each packed-plus-bounds codec pair's dirty
block union is 3,907, and all million bias clocks change. Visibility is
projection-candidate coverage, not exact raster contribution; row changes
include packed storage or dependence on changed bounds, not decoded-value deltas.
Semantic dense update obligations and observed changed bytes are distinct;
neither is a measured memory-bus load/store count or candidate transfer schedule.

`fields.tsv` captures native backward statistics **before** diagnostic freeze
restoration: radii and accumulation weights change. Optimizer statistics/shared
appearance remain frozen/restored for parity, not evidence of no training writes.
SH addressing accounts for transposed paired cells and explicit padded storage
and padding-byte changes; the tail is not treated as 256 valid source rows.
`field-inventory.tsv` binds authority, phase, shape/format, storage, semantic
read/write/block coverage, changes and unestablished upload/writeback frequency.

| Resource evidence | Bytes | Interpretation |
|---|---:|---|
| Maximum phase retained pool + scratch | 8,019,788,176 | High view, independent `after_backward_prime`; retained allocation, not semantic-live peak |
| Maximum last-requested/current-epoch sum | 7,946,903,176 | Logical epoch requests, not proven simultaneous liveness |
| Maximum alias arena capacity | 37,463,312 | Counted once; scratch allocation was 0 |
| Canonical snapshot storage | 387,983,064 | Diagnostic storage, not offload savings |
| First 20 world/optimizer/clock fields | 300,441,656 | Storage subtotal; original 312,441,656 B adds 12 MB radii/accumulation |
| Sampled native process RSS maximum | 3,019,374,592 | 0.25 s samples, not true peak |
| Full-attempt live output maximum | 627,089,379 | Failed and accepted artifacts retained; not physical disk traffic |

The pool table distinguishes retained capacity, current requests and aliases;
172 stale-alias accounting checks passed. **Unknown:** true GPU/host/pinned
peaks, image-cache/nonpool peaks and semantic simultaneous liveness.
The original derived 312,441,656 B subtotal remains **not achievable VRAM saving**.
Diagnostic restores/readbacks and oracle overhead are not O1-required traffic.

### O2 / O3 — dependent gate closed, one missing-mechanism handoff

**O2 NOT ADMITTED:** no established coherent bounded render discovery plus
native chunk update and partial gradient/state servicing seam exists. No
timing repeats or new native-resident speed reference ran; no historical
B1/G1 median substitutes for one. Candidate query/workspace/refresh and
required completed H2D/D2H/writeback costs remain unknown.

A **prospective**, unimplemented host spatial index could conservatively
stage a render superset using native projection-support bounds (not center-only
frustum membership), conservative invalid/nonfinite-row fallback, original
row IDs and source/topology/parameter-generation refresh. False positives,
workspace and query/refresh cost are unmeasured. Independently, every optimizer
block must stream densely. Existing GPU candidate IDs require full-world
projection first; they do not establish bounded host discovery. Native FPBO
has no block-range contract preserving global-N regularizer normalization,
per-row clocks, whole-block extrema/reduction order and row-ID mapping, nor
bounded partial-gradient/state servicing. The full-capacity six-buffer external
gradient seam does not supply it.

**Exactly the first O3 branch is selected:** keep O1 unverified and stop
implementation admission. The single handoff is this precise missing-mechanism
record, **not** an admitted eviction/reload parity micro-plan. This is not
architectural impossibility or evidence against all offload schedules. Retain
the native resident baseline and historical B1 benchmark lead; no speed,
capacity, quality, recovery or production-winner claim. G1 remains closed.

### Retained evidence and failure history

Local `o1.manifest.json` binds `observed-accounting.json`, `field-inventory.tsv`,
`diagnostic-complete/{fields.tsv,phases.tsv,summary.tsv,final.native}`,
`feasibility-inspection.md` and the accepted native/build/packet/headless logs.
The feasibility inspection is source analysis: its statement that views had
not run describes that inspection's earlier boundary, not the completed diagnostic.
The initial build failed the comment budget, then was corrected. The initial
diagnostic completed six logical updates but failed an invalid homogeneous
10003 row-clock assumption; its artifacts remain rejected/preserved. The
corrected check requires each original row clock +3, **not relaxed acceptance**.
Final Vulkan build exited 0, CPU packet check exited 0 and headless passed
14/14 with exit 0. These are correctness/build evidence, not benchmark repeats.

<a id="next-plan--m0m4-bounded-mechanism-proof"></a>

## Completed M0–M4 continuation — M0 resource admission rejected

**Status: CLOSED, executed through M0 on 2026-09-30; M0 NOT ADMITTED,
M1–M4 NOT RUN (dependent stop).** The declared schedule fails its retained
device-allocation cap before implementation. No transfer pricing, timing
repeats or eviction/reload implementation was admitted. This continuation
does not reopen or rewrite the completed O0–O3 screen.

### M0 — observed identities, resource contract and admission ledger

Fresh source/config hashes match the original identities; the native binary
and all 14 source bindings are unchanged. Fresh `vulkaninfo` summary/full
commands exited 0 on the supported AMD R9700. The available-budget snapshot
was **33,250,242,560 B budget / 0 B usage**, with **18,821,275,648 B free scratch**.
That available budget does not override the independently declared residency cap.

The schedule was declared before implementation:

| Resource / schedule item | Declared contract |
|---|---|
| Device retained allocation | **7 GiB = 7,516,192,768 B**, below the retained resident 8,019,788,176 B baseline; a residency target, not device exhaustion |
| Host / pinned | Accounted host storage and sampled aggregate RSS each at most **8 GiB**; pinned buffers at most **16 MiB** |
| Output / scratch | Live outputs at most **10 GiB**; at least **10 GiB** free scratch |
| Optimizer servicing | Sequential chunks of **128 or 256 whole 256-row codec blocks**; dense coverage including inactive rows and tail |
| Render servicing | At most **850,000 candidate rows / 2,000,000 tile intersections**; preserve existing full-image loss, shared state and native ordered-raster buffer retention |
| Copies / bookkeeping | No full Gaussian device copy; some full-row projection mask/bookkeeping retained intentionally |

The optimistic floor uses the retained high-view independent update 2
`after_backward_prime` allocation accounting; it removes **all** full Gaussian
state/moment/auxiliary/gradient allocations and unrelated B1 ownership.
Required projection candidates number **822,825**, within the row cap:

| Retained allocation floor | Bytes |
|---|---:|
| Native pool + scratch baseline | 8,019,788,176 |
| Remove full Gaussian category `splat` | −560,441,660 |
| Remove unrelated B1 ownership | −15,628 |
| Retained native non-splat allocation | **7,459,330,888** |
| Candidate non-SH fields | 46,078,200 |
| Native candidate SH: 3,215 blocks / 823,040 padded rows × 46 cells × 2 B | 75,719,680 |
| Candidate bounds | 25,720 |
| Candidate render rows subtotal | **121,823,600** |
| Optimistic retained-allocation floor | **7,581,154,488** |
| Declared device cap | **7,516,192,768** |
| Excess before optimizer chunks, mapping, additional staging and unknown overhead | **64,961,720** |

**Decision:** M0 resource admission fails on this specific capacity ledger.
The floor already exceeds the cap; implementation cannot repair it while
preserving this declared retention schedule. This is not a measured simultaneous
peak, device exhaustion or an architecture-impossibility result.

Local `mechanism.manifest.json`, `schedule.json` and `admission-ledger.json`
under `o1-mechanism-20260930T175846Z` bind exact commands, file identities and
arithmetic assertions. Private paths and device UUIDs stay local. Fresh evidence
is limited to device queries, input/native file hashes and ledger assertions.
No new dev build, headless run or native six-update repeat was needed or run:
relevant code and inputs are unchanged. Earlier O0–O3 checks remain historical.

| Gate | Execution result |
|---|---|
| M0 | **CLOSED, NOT ADMITTED**: 7 GiB schedule exceeds its cap by at least 64,961,720 B |
| M1 — dense native block-range updates | **NOT RUN**: dependent on M0 admission |
| M2 — conservative render discovery | **NOT RUN**: dependent stop |
| M3 — bounded gradient/state servicing | **NOT RUN**: dependent stop |
| M4 — transfer pricing and decision | **NOT RUN**: no admitted schedule to price |

There is **no admitted next implementation** for this rejected 7 GiB schedule.
Another concrete cap/retention schedule requires a separate admission gate;
do not silently increase this cap. O1 training remains unverified; retain the
native resident baseline and historical B1 lead. G1 remains closed.

## Next plan — R0–R3 same-cap retention admission

**Status: CLOSED, NOT ADMITTED — R0–R3 executed below.** The one different
retention schedule was investigated under the unchanged **7 GiB =
7,516,192,768 B** device cap and rejected before implementation. The rejected M0
ledger and completed O0–O3 evidence remain intact. The retained R0–R3 contract
below does not authorize offload, a cap change or M1–M4.

### R0 — bind evidence and identify actual release boundaries

Create a separate local retention manifest referencing, not overwriting,
`mechanism.manifest.json`, `admission-ledger.json` and the original
`diagnostic-complete/phases.tsv`. Recheck source/config/native identities;
invalidate dependent accounting if relevant code or inputs changed. Retain the
8 GiB host/accounted-storage and sampled aggregate RSS caps, 16 MiB pinned cap,
10 GiB output cap and minimum 10 GiB free scratch. Refresh resource admission
before any subsequently authorized run; old available-device budgets are not
current headroom.

Source inspection identifies these candidates, **not proven savings**:

| Retained allocation | Observed high-view bytes | Last consumer / next use to establish |
|---|---:|---|
| Ordered-raster partials | 5,929,015,296 | Ordered reduction, after raster backward and key/permutation sorting; needed again by the next ordered backward. |
| Ordered keys/permutations | 4 × 7,720,072 | Sort/reduction chain; next ordered backward recreates them. |
| Render RGB; depth, transmittance and last IDs | 149,817,600; 3 × 49,939,200 | Loss/backward and retained forward-result consumers; next forward produces them again. |
| RGB cotangent; depth/transmittance cotangents | 149,817,600; 2 × 49,939,200 | Backward hooks and raster backward; next loss/backward produces them again. |
| Bilagrid and PPISP post-image buffers | 2 × 149,817,600 | Enabled transform-order hooks, including color-shift inputs; next forward needs the applicable buffers. |

These capacities come from retained `independent / 2 /
after_backward_prime` rows in `diagnostic-complete/phases.tsv`. The earlier
677,256,192 B / 881,844 B ordered-scratch and 146,313,216 B /
48,771,072 B image examples were smaller-view capacities, not this high-water
edge. Bilagrid post-image storage is distinct from background sky-blend storage.
Trace these in `src/backend/vulkan/kernels/RasterizeBwd.cpp`,
`src/engine/EngineLoss.cpp`, `src/engine/EngineForward.cpp` and
`src/engine/EngineTrainStep.cpp`. Complete a per-slot table with producer,
all conditional consumers, last GPU access, retained engine/tensor views,
next producer, capacity and proposed release/reacquire point. Do not free loss
maps or statistics without checking densification consumers, even under the
fixed-topology contract. Do not discard caller-visible render results.

`src/core/PoolSlots.h` and `src/core/Tensor.h` define ownership: ordinary
slots retain their high-water capacity when resized smaller; phase changes
invalidate arena views but do not free the arena. Tile-intersection/screen-gradient
aliasing is already counted. Count the arena once and never claim clearing an
alias slot as an independent physical saving.

**Exit:** source-bound lifetime table with explicit unknown consumers. Unknown
lifetimes get no release credit. Ordered scratch is specific to the strict
ordered-raster path, not an allocation assumed for every production run.

### R1 — specify one release/reacquire schedule

Start with independently owned scratch, not a new allocator or generalized
cache. Specify releases only after all consumers complete, clear or prove dead
every non-owning view, and recreate/reinitialize storage before its next use.
Use the native backend's completion/free contract; C++ wrapper destruction,
pool locking and return from a dispatch wrapper do not establish GPU completion.
Account for physical-free synchronization and allocation/zeroing work. An arena
proposal must establish non-overlapping consumers and backing capacity rather
than summing nominal slot savings.

Track the whole cycle: compact staging → forward/intersections → full-image
loss/hooks → ordered raster backward/reduction → projection backward →
dense optimizer chunks → completed publication → next view. Include bootstrap,
carried updates, checkpoint/readback and release/reallocation transitions.
Choose one concrete schedule from the lifetime table; do not combine mutually
exclusive release credits from different schedules.

**Critical constraint:** releasing the 5,929,015,296 B partials after reduction
can help later phases, but cannot remove their allocation from the backward
phase that needs them. Reallocating them before the next backward restores
that obligation. A post-backward saving alone does not repair the rejected
high-view floor. Identify allocations genuinely absent during that high-water
phase, or reject the candidate without implementation.

**Exit:** one explicit phase/ownership schedule, including completion and next-use
obligations. No code change or timing run is required to reject an incoherent one.

### R2 — reconcile the complete phase allocation ledger

Use **7,581,154,488 B** as the rejected schedule's reference floor, not a new
candidate's measured peak. Its **64,961,720 B** excess is only the minimum
deficit: required savings must also cover optimizer chunks, maps, staging,
alignment and allocation overlap. Never subtract a release globally when it
applies only after one phase.

For every phase and transition, enumerate retained owned capacity, the arena
once, nonpool scratch and all new buffers. Account for:

- Compact render storage at the **850,000-row cap**, with native padded SH
  cells/bounds, not merely the observed 822,825-row subset; intermediate arrays
  at the declared **2,000,000-intersection cap**.
- Both 128- and 256-codec-block optimizer chunk configurations, native packed
  values/moments/bounds, float gradients, per-row clocks, auxiliary state and
  the final partial block. Use native layouts, not a bytes-per-row shortcut.
- Original-row maps, discovery workspace, retained full-row bookkeeping,
  CPU-authoritative backing state/gradients and host/device transfer buffers.
- Full-image/shared-state costs, allocation granularity and simultaneous old/new
  storage during reacquisition. Do not overlap stages unless the ledger also
  includes both live allocations; preserve image/loss semantics.

For phase p require `retained_owned[p] + arena[p] + nonpool[p] +
new_buffers[p] + transition_overlap[p] <= 7,516,192,768 B`, with disjoint
categories and no omitted additions. Report the maximum and remaining margin
for each chunk configuration. Separate accounted capacity from requested bytes,
semantic liveness and unknown driver peaks. Unresolved required allocations
block admission; they are not zero.

List required free/sync/reallocate/clear events and their frequencies. Their
latency remains **unmeasured** until an admitted proof; future pricing must
charge them, along with discovery/refresh and transfer costs. No hypothetical
overlap or allocator speed is credited now.

**Exit:** executable arithmetic reconciliation and a complete conservative
phase ledger, with host/pinned/output limits checked separately.

### R3 — separate admission decision

If any phase exceeds the unchanged cap or required lifetimes/allocations remain
unresolved, close this retention candidate with its exact excess or missing
dependency. Preserve its evidence separately; do not raise the cap, rerun the
rejected ledger as a new result or infer that CPU offload is impossible.

Only a complete fitting R2 ledger may re-enter M0 as a **new schedule** with
fresh identity/resource admission. Passing R3 is accounting feasibility, not
observed memory savings, speed or capacity. After that M0 passes, use the retained
M1–M4 contract: bounded native proofs must also exercise the proposed releases
and reacquisitions across the independent/carried views, preserve strict
render/gradient and canonical parity, and pass stop/poisoned-checkpoint readback.
Observe allocations at transitions as well as phase endpoints; a runtime cap
breach rejects the schedule. Price actual lifecycle costs only at M4.

Update the scorecard with the candidate's status and evidence, leaving the
closed histories unchanged. No G1 repeats, production implementation, G3 tiled
loss, topology growth, NVIDIA/CUDA work or numerical relaxation is admitted.

### Completed R0–R3 execution — ordered-scratch-only candidate rejected

**R0 — OBSERVED bindings/resources; SOURCE-BOUND lifetimes.** Separate local
`o1-retention-20260930T183046Z/retention.manifest.json` references and hashes
the original screen/mechanism manifests, schedule, rejected ledger, phase TSV,
field inventory and observed accounting without changing them. Fresh source,
config and native binary hashes match; all prior relevant source bindings
match. Additional loss/forward/appearance/densify/checkpoint/runtime/layout
owners are hashed. No source regeneration, build or native replay ran. LSP was
not available among mounted tools; source reads/searches were used.

Fresh `vulkaninfo --summary` and `vulkaninfo` exited 0: two supported AMD
R9700 Vulkan devices, 26.Q3 (LLPC), API 1.4.349; each device-local budget
33,250,242,560 B / usage 0. Scratch was **18,819,186,688 B**, above the
10 GiB minimum. These are snapshots, not reservations or runtime peaks.
Host/accounted-storage and sampled RSS remain 8 GiB, pinned 16 MiB, live
output 10 GiB. No candidate process ran, so candidate RSS/pinned/driver
peaks are **UNKNOWN**, not observations of compliance.

The local `slot-lifetimes.json` enumerates each of the five independent
ordered slots and every image/cotangent/post-image family, including explicit
producer, conditional consumers, last GPU access, non-owning views, next
producer and proposed release. Source anchors:

| Slot family / ownership | Producer → conditional consumers / last access | Views and next-use/release decision |
|---|---|---|
| Five ordered scratch slots, independently owned | `RasterizeBwd.cpp:139–196,252–266`: zero partials, initialize keys, raster writes, ping-pong sort, ordered reduction reads selected keys/perms/partials | Local vectors, `DoubleBuffer` and queued params addresses; wrapper return/destruction is not completion. Only after reduction completes and locals are dead: free all five; next nonempty ordered backward must resize/reinitialize. |
| `Renders`, `RenderTs`, `RenderLastIds` | `EngineForward.cpp:380–442`; `EngineLoss.cpp:511–866`: transforms, multiscale losses/SSIM, conditional depth-normal/median/distortion, background, raster backward | `engine.fwd` and downstream `fwd_pre` aliases; `EngineQuery.cpp:40–70,213–240` later copies/derived depth-normal. Caller-visible lifetime is not bounded by backward. **Retain; no release credit.** |
| `EngVRgb`, `EngVDepth`, `EngVTs` | `EngineLoss.cpp:402–430,613–616,696–866`: loss/copy producers; color-shift, inverse PPISP/bilagrid/color-space, overexposure/background, depth-normal adds, raster backward | Local pixel-gradient/TorchTensorView/tuple aliases; last internal GPU reader raster backward. Next loss/copy produces again. **Retain in this schedule.** |
| Sky-blend / PPISP / bilagrid / color-space post images | `EngineBackground.cpp:223–272,293–381`, `EnginePpisp.cpp:88–102,140–162`, `EngineBilagrid.cpp:306–355`, `EngineColorSpace.cpp:72–103`; enabled transform order determines downstream pre-image and final loss/color-shift readers | `fwd.renders`, background pre-blend/background image and transform `fwd_pre` views can point at earlier storage. `EngineLoss.cpp:696–819` selects pre/post by transform order. Final render readback remains possible. **Retain; no speculative release.** |
| Background conditional extra scratch | SH image→blend/SH backward; `VBg`→SH backward; `VTsScratch`→add; `VSh`→later optimizer (`EngineBackground.cpp:302–388`) | Background-disabled diagnostic does not bound enabled SH capacities. **UNKNOWN conditional dependency, no credit.** |
| Loss maps / accumulated statistics | `EngineLoss.cpp:683–692,851–866`, `EngineTrainStep.cpp:287–298,377–445`, `EngineDensify.cpp:227–254`: preview, raster weighting, subbatch folds, average finalization/score selection | `fwd.accum_weight/mode`, radii/accum/gradient views survive raster return. Frozen topology does not authorize skipping native statistics. **Retain.** |

`Tensor.h:142–184,295–319,387–394,651–690` establishes high-water
pool ownership, shallow non-owning tensors and arena phase invalidation.
Arena backing is **37,463,312 B counted once** in the reference; clearing
an alias is not a physical saving. `VulkanRuntime.cpp:456–473` flushes all
streams and waits the timeline before destroying an independently owned
allocation; pool locking/dispatch return alone does not establish that.

**R1 — DERIVED one schedule, not implemented.** Retain all native image/loss,
shared state, statistics and arena capacities. Free only the five independent
ordered slots after ordered reduction/local-wrapper scope, before projection/
optimizer/publication. Bootstrap loads CPU authority without a full device
Gaussian copy; compact staging/discovery precedes each full-image forward.
Loss and inverse transform hooks run with their inputs intact. Reacquire the
five slots immediately before every next ordered backward, zero partials and
initialize sort inputs, then reduce before freeing again. Independent reloads
and carried updates obey the same cycle. Dense sequential 128/256-block
servicing preserves native clocks/row IDs/padding; complete transfer/update
semantics remain unresolved, so this is not an admitted implementation.
Checkpoint/readback requires completed CPU publication and bounded native
serialization; stop/poisoned-readback obligations are not waived.

No stage overlap is credited. Free-before-reallocate avoids intentional
old/new overlap for these five slots only; other allocation transitions remain
unknown. Each nonempty update requires five frees/queue completion boundaries,
five next-use acquisitions, a full partial zero and key initialization/sort.
Dense chunks require 31 or 16 sequential visits per million-row update.
Allocation, zeroing, synchronization, discovery, refresh, transfer and
checkpoint costs are **UNMEASURED**, with no savings or speed claim.

**R2 — DERIVED auditable lower bounds, NOT a complete ledger.** The native
SH3 transposed layout (`Tensor.h:1195–1228`, `EngineOptim.cpp:134–214`)
uses 46 cells per padded row, not 45: 850,000 rows require 3,321 blocks /
850,176 padded rows. Native compact values cost 47,600,000 B non-SH +
78,216,192 B packed SH + 26,568 B bounds = **125,842,760 B**.
The unchanged retained non-splat reference plus this cap is
**7,585,173,648 B**, already 68,980,880 B over cap before new additions.

At the declared 2,000,000 intersections, the retained ordered shape's 64
subtiles and stride 12 (10 screen fields + two average-accumulation lanes)
require **6,144,000,000 B partials + 32,000,000 B keys/permutations**.
The source allocation formula and observed 1,930,018-intersection scratch
agree. Replacing only the reference ordered capacities, without globally
subtracting post-reduction releases, yields:

`7,459,330,888 + 125,842,760 − 5,929,015,296 − 4×7,720,072
+ 6,144,000,000 + 4×8,000,000 = 7,801,278,064 B`.

Thus ordered backward alone has a **285,085,296 B minimum excess** for
**both** chunk choices; post-reduction release cannot remove mandatory
partials from that phase. The cap shape is a declared maximum allocation
obligation, not a newly observed 2M-intersection execution.

| Chunk lower-bound components | 128 blocks / 32,768 rows | 256 blocks / 65,536 rows |
|---|---:|---:|
| Native values including padded SH/bounds | 4,850,688 | 9,701,376 |
| Native moments: five non-SH float4 bounds + SH float4 bounds/block | 4,861,952 | 9,723,904 |
| 59 float-gradient cells/row | 7,733,248 | 15,466,496 |
| Per-row clocks | 131,072 | 262,144 |
| Radii/accum/weight auxiliary lower bound | 655,360 | 1,310,720 |
| Row-map lower bound | 131,072 | 262,144 |
| Additional optimizer-phase subtotal | **18,363,392** | **36,726,784** |

Both chunk choices end with 67 codec blocks / 16,960 valid rows, including
the 64-row final source block and native padded SH tail. These disjoint
subtotals are not full allocations or backward additions: optimizer chunks
are sequential after ordered scratch release. The phase ledger explicitly
covers bootstrap, compact staging, forward/intersections, loss/hooks,
backward/reduction, projection, chunks, publication, checkpoint/readback and
next reacquisition, each labelled **incomplete lower bound**. Complete maximum
and remaining admission margin are **UNKNOWN**; the backward lower-bound
margin is **−285,085,296 B**, not a peak measurement.

Missing dependencies are enumerated locally, not assigned zero: conservative
discovery; capped projection/screen/intersection/sort workspace and arena
growth; native dense range optimizer/global normalization/reductions/clocks;
bounded gradient scatter/writeback; complete row bookkeeping; dedicated
`VkMemoryRequirements.size`/alignment and heap placement; params/sort scratch/
transfer allocations; image cache/decoder/prefetch and aggregate RSS;
all pinned readout/params/staging; checkpoint serialization/readback/output
overlap; completion/publication/stop semantics and transition driver peaks.
The default Vulkan pageable staging ring is **64 MiB**
(`VulkanRuntime.cpp:221–234`), already larger than the 16 MiB declared pinned
budget if charged there; selecting a bounded ring would still require all
other pinned allocations/heap requirements. Known host authority +
gradient backing + declared maps + 16 MiB transfer allowance totals
648,760,280 B, below 8 GiB only as a subtotal, not complete host admission.

**R3 — CLOSED, NOT ADMITTED.** Quantified backward excess and unresolved
required allocations independently block this one scratch-only schedule.
No separately fresh M0, M1–M4, offload implementation, build, native replay,
timing or numerical relaxation ran. Other retention architectures are not
ruled out; native baseline/historical B1 lead and closed O0–O3/M0/G1 histories
remain unchanged. Local `verify-retention.ps1` binds hashes and recomputes
caps, phase-scoped obligations, both native chunk layouts and tail arithmetic.
The final arithmetic smoke exited **0** and reported the same rejection for
both chunk choices; `verify-retention.log` retains its output. This is
identity/accounting verification, not GPU execution or peak-memory evidence.


## Retained M0–M4 reproduction / conditional contract — not executed beyond M0

The original plan below is preserved as a conditional contract, not active
implementation work. M1–M4 require a separately admitted concrete schedule.
Dense dirty coverage rules out visibility-only writeback skipping, not dense
streaming. Repeat the six-update diagnostic only if its inputs or relevant native
implementation change.

### M0 — retained binding and memory contract

Reuse the retained source/config and O0 resource policy: fresh hashes and
supported AMD device admission, at least 10 GiB free scratch, at most 10 GiB
live outputs and 8 GiB sampled aggregate RSS. Preserve the original manifest
and failures; bind new commands, code/build identities and outputs separately.
Keep topology/shared appearance frozen and retain the native FPBO layout,
original learning-rate horizon and identical effective configuration in both arms.

Before implementation, publish a byte-accounted schedule with explicit device,
host and pinned-buffer caps, codec blocks per chunk and overflow behavior.
Choose the device cap from fresh available-budget observations; require the
candidate's accounted working set to be below the retained resident allocation
on the same accounting basis. The observed 8,019,788,176 B is not a measured
peak or an automatic safe cap. Include full-image loss/render intermediates,
image caches, shared state, native pool retention, mapping tables and staging.
Sequential oracle and candidate processes must not hide a full resident device
copy inside the candidate. Unknown driver peaks remain unknown.

**Exit:** one concrete schedule/resource ledger. If fixed image/scratch costs
already exceed the chosen cap, stop this schedule before building it; do not
quietly enlarge the cap or introduce tiled-loss/G3 work.

### M1 — prove dense native block-range updates first

Use the existing native optimizer and the diagnostic's same-input oracle,
not a CPU optimizer or alternate codec. Establish the smallest block-range
interface in the existing native update path: global row count and original
row IDs remain distinct from local chunk capacity. Preserve global-N
regularizer normalization, native per-row clocks, learning-rate step, whole
256-row codec reductions and padded SH addressing.

Serve all 3,907 blocks, including zero-data-gradient/inactive rows and the
64-valid-row tail. Use at least two legal chunk sizes and multiple chunks;
no allocation in the candidate update path may scale as a full device copy
of values, moments or the six Gaussian-gradient arrays. A bounded host slice
of retained native gradients is allowed **only for this isolated update proof**,
not as evidence that end-to-end gradient servicing exists.

Compare each independent view 1188, 1165, 760 from the restored source and the
ordered three-update sequence against the native same-input oracle. Require
exact canonical values, packed moments, bounds, clocks and frozen state after
every update. Compare each row's clock with its own source clock, not a uniform
global clock. Use the actual field schema and native SH address helpers:
packed/bounds pairs must be counted together, never inferred from bytes/row.

**Exit:** exact dense chunk-update parity and bounded allocation evidence.
Any numerical mismatch blocks dependent work; do not relax equality, change
Adam epsilon, convert the checkpoint or omit invisible updates.

### M2 — establish conservative render discovery

Specify discovery independently of optimizer coverage. Start with the smallest
conservative host scan/query using native projection-support conventions;
an accelerated spatial index is not a prerequisite. Avoid a third projection
implementation. If the host/native support contract cannot be reused safely,
record the exact missing interface before adding one.

The query must not first project a full device-resident world. Preserve source
row IDs and deterministic native ordering, extended splat support and native
invalid/nonfinite handling. Bind results to source, topology and parameter
generation; refresh after every update in the carried sequence. Charge scan,
mapping, false positives, refresh and query workspace. A result that includes
all rows is conservative but does not prove bounded render residency.

Use resident projection/render captures only as the oracle to check required
row inclusion and native renders/gradients for the three independent views
and the carried sequence. Predeclare the candidate-row/intermediate cap; reject
overflow without publishing a step. If a view needs more than the cap, stop
this schedule with its measured required set. Do not silently truncate, use
center-only culling, fall back to full residency or start G3 partitioning.

**Exit:** conservative coverage plus render/gradient agreement under the declared
cap, with measured query/refresh costs. This is not sparse optimizer permission.

### M3 — prove bounded gradient and state servicing

Join the accepted discovery and dense-update interfaces in the existing native
diagnostic path. Keep one immutable source generation for forward/backward;
map native float gradients back to original rows with the native reduction
order, then service each whole optimizer block exactly once. Missing data
gradients are zero, not permission to skip moments, regularizers or clocks.
Account for any host gradient backing store in the 8 GiB limit and use bounded
device slices; the existing full-capacity external-gradient interface alone
does not meet this gate.

Write updated native blocks to CPU-authoritative state; make the next generation
available only after all blocks and required statistics complete. Do not reuse
an old query after publication. Reuse sequential buffers; no speculative overlap,
cache framework, second trainer or general transport layer.

Repeat M1's independent and carried schedule with actual candidate-generated
gradients, strict native render/gradient checks and same-input canonical byte
equality after every update. Include tail blocks, zero-gradient rows, cap
overflow/refused work and a stop at a completed boundary. Poison resident state,
reload the completed native checkpoint and verify exact readback. A refused or
partial step must never be reported as a completed checkpoint.

**Exit:** one coherent bounded schedule with phase allocation/retention evidence
and native parity/readback. Failure leaves O1 unverified and closes only this
schedule with the exact failing dependency; no transfer-only benchmark replaces it.

### M4 — admit pricing, then make one decision

Only after M0–M3 pass, execute the retained O2 component protocol for this
specific schedule: actual completed transfers, packing, dense writeback, query
refresh and synchronization; three fresh short repetitions per selected view,
medians/ranges and bytes by direction. Keep diagnostic/oracle cost separate.
Use fresh matched native-resident speed windows, not historical B1/G1 medians.

Apply the retained O3 decision table to the **new continuation**, without
rewriting the completed screen's first-branch result. A noncompetitive schedule
with no demonstrated memory benefit closes. A credible residency result can
admit one integrated eviction/reload parity micro-plan with a concrete cap;
it does not authorize long timing, production offload or a capacity claim.
Any memory-only continuation must name its capacity objective before expansion.

Implementation verification uses the dev Vulkan build, relevant native
supported-device checks and the headless suite once after integration; retained
G1 timing runs stay closed. Record each gate's observed result and blocker in
the scorecard. No NVIDIA/CUDA work, topology growth, multi-GPU consumers,
approximate update skipping, quality gate or new dataset is introduced here.

## Retained O0–O3 protocol

The following original gates remain the reproduction/conditional admission
contract; the execution above stops at the first O3 branch.


## O0 — bind evidence and admit one bounded attempt

- Resolve the retained step-10k source, config and supported device identities
  from the original local G1 manifest and `continuation.manifest.json`. Rehash
  source/config; record current code/patch, executable, toolchain and driver.
  Missing artifacts block their dependent comparisons; do not regenerate a
  source silently or borrow an old median as a fresh control.
- Preserve original evidence. Write a new local manifest and disposable output
  directory; keep private paths and device UUIDs out of committed documents.
- Start with one of the supported AMD devices, explicitly identified. Admit
  at least 10 GiB free scratch, cap live attempt outputs at 10 GiB and sampled
  aggregate RSS at 8 GiB. Record device budget/usage and estimated incremental
  allocations before launching. Stop on budget breach or unsupported hardware.
  Samples are not true peaks; unknown pinned/driver peaks stay unknown.
- Fix native source FPBO layout, quantization, SH degree, loss weights and
  learning-rate horizon. Freeze topology and shared appearance as in the
  retained diagnostic; record background/tile-skip overrides in both arms.
  Do not convert checkpoint layouts or use visibility as an update mask.

**Exit:** identity, evidence-availability, effective-config and resource manifest.
No claim that the old C0 headroom still holds.

## O1 — establish the unavoidable working set and dirty coverage

Use actual source views **1188, 1165, 760**, each from the same source, then an
ordered three-update sequence to expose carried moments and reuse. Retain
original row IDs, all 3,907 native 256-row codec blocks and the 64-valid-row tail.
Inactive/nonfinite rows remain in native storage and are not silently compacted.

Extend existing native diagnostic facilities only where observations are
missing. This is instrumentation around resident training, **not an offload
implementation**. Reuse native update/capture and checkpoint readback; inspect
current references before changing any exported seam.

| Existing file | Use in the screen |
|---|---|
| `src/backend/tests/engine/g1_native_proof.cpp` | Native source loading, strict FPBO diagnostic, field inventory and readback patterns; reuse without changing closed G1 timing behavior. |
| `src/engine/EngineOptim.cpp` | Native FPBO update, regularizers, moment/bias and codec-bound mutation boundaries. |
| `src/engine/EngineTrainStep.cpp` | Actual forward/backward/update ordering and completed-step boundary. |
| `src/engine/EngineQuery.cpp`, `src/core/PoolSlots.h` | Phase residency, aliases and retained pool capacity; do not count arena views twice. |
| `src/core/Tensor.h` | Native codec layout and whole-block storage dependencies. |
| `src/engine/EngineDataManager.cpp` | Exact view selection and visibility observations, not a sparse-update authorization. |

Produce a local field table: authority, consumer phase, shape/format, allocated
and live bytes, row/block coverage read and written, semantic write obligation,
observed byte changes, upload/writeback frequency and evidence binding. Separate
render values, optimizer values/moments/bounds/bias, statistics, frozen shared
state, image caches and scratch. Distinguish semantic writes from byte changes:
unchanged bytes in three views do not prove a field can remain stale later.

Trace zero-data-gradient and invisible rows through native regularization,
Adam history, bias clocks and codec reductions. Count actual dirty **codec
blocks**, not just visible rows. State how dense native updates would be served
in bounded chunks and identify global reductions or full-world dependencies
that prevent doing so. No CPU optimizer or alternate numerical implementation.

A conservative candidate-query proposal must find required rows without first
loading the complete model onto the GPU; include host indexing, false positives,
refresh/invalidation and query workspace. Existing finite-row-median visibility
statistics are only a proxy, not such a query. If no safe bounded discovery or
native chunk-update seam is established, report the exact missing dependency;
do not fill the gap with hypothetical savings.

**Exit:** actual field/block inventory, full phase residency accounting, and one
explicit prospective working-set schedule or a documented feasibility blocker.
Diagnostic readback overhead is separate from candidate-required traffic.

## O2 — price the proposed schedule, not synthetic transport

Proceed only if O1 identifies a coherent schedule. Reuse bounded buffers and
existing backend copy/profiling facilities on the selected GPU. Measure actual
native payload packing, completed H2D/D2H transfers, synchronization, dirty
writeback and required dense passes for the three selected views. Do not time
only enqueue, substitute pattern packets, sum overlapping timers, or claim
file-ledger bytes are physical disk traffic.

Measure three fresh short repetitions for each selected view/schedule and
report medians and ranges, bytes by direction, host/pinned buffers and observed
phase residency. Keep profiled observations separate from unprofiled native
resident windows. Charge candidate lookup if implemented; otherwise leave its
cost unknown and the total incomplete. Do not build a new query framework just
to fill a timing table.

Use two references: ordered native execution for strict numerical/readback
checks, and the normal native-resident path on the same device, source,
views/pixels and update count for speed. The existing G1 paired-update schedule
is not an O1 reference. Historical B1/G1 medians are context only.

**Exit:** measured component costs, required bytes, memory accounting and a
qualified bound. A measured serial cost may exclude only a schedule that
requires that serialization. A pessimistic prototype is not an architectural
lower bound; hypothetical overlap is not an observed saving.

## O3 — close the screen and choose exactly one handoff

| Evidence | Decision |
|---|---|
| No coherent bounded working set or native update/discovery seam | Keep O1 unverified; record the precise missing mechanism and stop implementation admission. |
| Required costs rule out this schedule versus the fresh resident reference, with no demonstrated memory benefit | Close this schedule as noncompetitive; no full O1 implementation or long timing. |
| Credible residency reduction with measured costs, or an unresolved close comparison that a small proof can decide | Write one source-native O1 eviction/reload parity micro-plan, with a concrete working-set cap and charged missing work. No speed/capacity approval yet. |

The conditional proof must preserve native renders/gradients, packed values,
moments, bounds, clocks and frozen context across eviction/reload, partial codec
tails and invisible rows. Require same-input canonical byte equality and the
existing strict native oracle; no acceptance relaxation. It must include safe
completed-boundary stop and native checkpoint readback before fixed-work timing.
Only that subsequent admitted proof can justify three fresh matched timing
repeats, not this component screen. A memory-only result needs a separately
stated capacity objective before a larger implementation is selected.

Update the scorecard with observed/derived/unknown labels and the next decision.
If no candidate is justified, retain the resident baseline and historical B1
lead without claiming a production winner. A failed gate stops that dependent
candidate; preserve failure evidence and finish the decision record.

## Verification and exclusions

Execution rebuilt through `build_develop.bat -DSS_BACKEND=vulkan
-DSS_BUILD_BACKEND_TESTS=ON` (one command), ran the native diagnostic on the
admitted non-NVIDIA device, CPU packet checks and the headless suite.
The local manifest records exact commands, exit codes and measurement boundaries.
Correctness/failure checks are not accepted unprofiled timing samples.

Excluded: rerunning closed G1 gates as routine work; another digest/transport
optimization; G1 1,200-exposure repeats; multi-GPU O1 consumers; B2/G2;
topology growth; retries/production publication; GUI controls; oversized capacity
or quality/time-to-quality claims; NVIDIA/CUDA work. Existing B1 recovery,
sidecar/config/view-binding and consumer/boundary gaps remain open.
