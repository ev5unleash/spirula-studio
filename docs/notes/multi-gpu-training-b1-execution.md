# B1 fixed-topology block proof — execution checkpoint

Status: **B2 was selected for both cold and valid reused-preparation
fixed-topology/appearance kitchen work on two AMD Radeon AI PRO R9700 Vulkan
devices; not broad production acceptance.** All B2-0 through B2-5 gates are complete.
The [fresh integrated scorecard](multi-gpu-training-comparison-20260929.md#2026-10-01-integrated-b2--selected-cold-and-reuse)
records 357.438379 / 250.754732 s cold/reuse medians versus 431.199014 s native
control: 17.105938% / 41.847100% less complete wall. The
[historical B2 contract](multi-gpu-training-plan.md#5-b2--active-bounded-implementation)
governs historical reproduction of that bounded mode.

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
See the [handoff](multi-gpu-training-handoff-20261002.md). The
[timing ledger](multi-gpu-training-timing-20260929.md), commands and original
protocols below remain historical evidence, not authority to rebind old inputs.

**Longer qualification:** [Q0–Q4 executed](multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results).
The step-20,000 owner outputs passed fault-safety, exact committed exposure
replay, clocks and frozen/unowned-state checks, but both failed the existing
decoded `1e-4` recovery screen. Matched B2 raw/color-corrected mean PSNR was
1.602366 / 2.534380 dB below the all-owned control; release qualification is
withheld. Those Q runs did not prove worker device-allocation enforcement.
Historical short-run evidence below remains unchanged and does not override this failure.

**[D0–D5 causal investigation completed](multi-gpu-training-b2-causal-20261001.md#execution-evidence).**
Actual backing caps are now enforced and exercised. Late immediate save/load
was exact; no-save native variability also failed the unchanged decoded screen.
Fresh equal-row control leaves B2 mean raw/cc PSNR −1.674363 / −2.357914 dB
lower. Native training remains the practical default, B2 experimental and
longer recovery/release unqualified. Existing commands and bounded historical
throughput evidence remain valid; no unique long-Q cause or repair is claimed.

**[V0–V5 view-policy ablation completed](multi-gpu-training-b2-view-policy-20261002.md#v5--completed-disposition-and-next-decision).**
Explicit immutable full-view policy and fresh P/F primary groups passed native
checks. Matched mean raw/cc PSNR improves F over P **1.048877 / 1.287893 dB**,
but F remains **0.596867 / 1.050402 dB** below native N. Native renders confirm
improvement with residual errors; native remains default, B2 experimental and
original Q longer recovery FAILED. Full policy is not a practical acceleration
or release qualification. Context refresh and justified further research are
prospective directions requiring fresh selected-data admission and current
approval. Existing G1/O1 schedules remain closed; materially different candidates
have the same admission/approval requirements.

## Historical bounded native B2 path

```bat
spirula train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_GROUP --updates 4
spirula train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_FULL_GROUP --updates 4 --view-policy full
spirula train-blocks --resume-group GROUP
```

Use a native full source checkpoint, the validated two-owner view manifest,
two distinct supported Vulkan device UUIDs, and a new output directory.
`--updates N` is **per owner**, bounded by the original optimizer horizon;
it does not shorten or rewrite that horizon. Resume accepts no overrides:
immutable `group.json` binds source/config/view/shared-appearance identities,
devices and target, and the group validates the latest full checkpoint for
each owner independently before dispatching remaining work through the local
native scheduler/workers. No GUI, remote-worker or CUDA/NVIDIA path is claimed.

`--view-policy partitioned|full` applies only to a new group; omitted policy
defaults to `partitioned`. Both modes require the validated `--views MANIFEST`.
Full uses all parser training indices, excluding held-out cameras; manifest
assignment bits remain hashed provenance, not a full-mode filter. Ownership,
frozen context and numerics are unchanged. New group/owner-checkpoint and bound
worker payload schemas are version 2; old group/owner 0/1 contracts remain
partitioned, while old owner 2 references retain native all-owned semantics.
Policy is immutable: `--resume-group` forbids `--view-policy` and all other
overrides, and inherited `SS_B1_VIEW_POLICY` / `SSPLAT_B1_VIEW_POLICY` is rejected.
Missing/unknown new-schema policy and mixed-policy merges fail closed.
Full-policy validation is not a quality or longer-recovery qualification.

Bounded group workers enforce at most 24 GiB of actual runtime backing with
`SS_WORKER_DEVICE_LIMIT_BYTES`; a stricter positive inherited cap is retained.
Ordinary direct training opts in explicitly. Reservations precede allocation;
device, pinned, pageable staging and params categories are counted once.
Pool/scratch attribution is not added again. Worker-local driver-budget
headroom is separate, and backing high-water is not a physical VRAM peak.
See [native cap/diagnostic checks](../testing.md#worker-backing-admission-and-bounded-causal-diagnostics).


Each worker retains the full source scene as frozen render context and updates
only its complementary whole 256-row codec blocks. Shared appearance and
topology remain frozen. The standard kitchen step-10,000 source has 1M rows,
3,907 blocks with 64 valid rows in the last block, and 7,150 nonfinite original
means; original row IDs and heterogeneous per-row clocks are retained.
Marked native full checkpoints bind source, effective optimizer config,
original horizon, view manifest and owner train indices, ownership mask,
completed step, sampler version, sidecar/PLY identities and frozen context/shared
state. Missing sidecars, wrong-owner or ordinary native owner resume, changed
config/horizon/views, and treating a merged PLY as globally resumable training
state fail closed. Merge accounts for every source ID as included or excluded,
requires complementary owners at the declared target, reads back natively and
publishes without replacement.

`attempts/<attempt>/owner-0` and `owner-1` retain native full owner checkpoints
and scheduler evidence. Only completed, validated groups publish `complete.json`,
binding `group.json`, both owner checkpoints, the merged PLY and its hash.
Send `STOP` followed by a newline on stdin to save after coherent updates;
send `CANCEL` followed by a newline to force existing job cancellation.
Completed groups exit **0**; cooperative incomplete groups exit **42** without
a completion record; failures/refusals exit **2**. Ordinary native `train`
contract errors exit **1**. An incomplete group is resumed explicitly with
`--resume-group GROUP`, not by overriding its inputs.

The longer qualification does **not** support numerical equivalence after
later-history recovery. Valid completion publication proves bound artifacts,
not equality with an uninterrupted optimizer trajectory. Keep the reported
quality/decoded-recovery limitations when using this approximate path.
For source-bound evaluation, a canonical all-owned native checkpoint PLY can
also be read through its validated source/shared/config/body/sidecar contract;
a plain source PLY still cannot substitute for that binding.

### Opt-in live context refresh (experimental, validation pending)

A2 adds an opt-in cadence mode. The dev Vulkan build, 17/17 headless checks,
native field proof and tiny two-AMD STOP/kill/resume workflow passed; see the
[handoff evidence and limits](multi-gpu-training-handoff-20261002.md).
Full-source validation, quality, speed and long recovery remain unqualified.
Native training remains the default; historical frozen-context results are
separate from these bounded checks.

```bat
spirula train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_REFRESH_GROUP --updates 8 --view-policy full --refresh-cadence 2
spirula train-blocks --resume-group NEW_REFRESH_GROUP
```

Use the same native full source, validated manifest, two distinct supported
non-NVIDIA Vulkan UUIDs and new output directory as above. Cadence is a positive
integer (at most 2,147,483,647) measured in updates per owner from the original
source step. `--view-policy full` is required explicitly and `--updates` must
be divisible by cadence; the original optimizer horizon is unchanged. Omitting
cadence retains the existing version-2 frozen-context path. Cadence opts into
immutable version-3 group and owner-checkpoint contracts; neither cadence nor
policy may be overridden on `--resume-group`. Direct owner training and
inherited group/refresh environment overrides are not substitutes for the
bound coordinator.

Workers stay alive between refreshes. Both pause after completed updates at the
same source-relative boundary, export authoritative owned native render fields
and SH codes/bounds, then install only the complementary peer render blocks.
Owned optimizer state, moments, clocks, topology and shared appearance are not
exchanged or averaged. Both full native checkpoints are saved after installation;
the coordinator validates and publishes the generation commit before releasing
the next updates. The full training-index policy still excludes held-out views.

Recovery differs from the independent latest-owner selection of the no-cadence
path: resume uses **one generation-bound checkpoint pair** from the latest
published `refresh-commits/<generation>.json`, validating its parent-commit
lineage, source/group/config/view identities, both payload digests, masks,
checkpoint state hashes and installed generation. It never combines separately
newer owner checkpoints. `STOP`/`CANCEL` or owner failure can discard work since
the last published pair; absent any published generation, resume starts both
owners from the bound original source. An in-flight payload, installation or
one-owner checkpoint is not recovery authority. Use the existing group exit
codes; no incomplete invocation publishes `complete.json`. The merged PLY
remains a rendering/evaluation artifact, **not merged optimizer resume state**.

Retention is rolling: after validating a newer pair, the coordinator removes
older committed owner checkpoint pairs and their large payload files only
inside this group's owned attempts. Compact commit/install lineage and scheduler
evidence remain; the source and latest committed pair are retained. Admission
must cover simultaneous old/new checkpoint pairs, old/new payload pairs and
native save staging, not just steady-state storage. After validating recovery
authority, resume also removes abandoned prior-attempt native staging,
uncommitted checkpoints and payloads inside scheduler-recorded attempts.
Cleanup refuses symlinks, Windows reparse paths and foreign hardlinks; it does
not delete the source, retained checkpoint pair or compact lineage/evidence.

Source contracts: [CLI, paired publication and retention](../../src/app/B1Group.cpp),
[native field installation](../../src/engine/EngineB1.cpp) and
[persistent trainer boundaries](../../src/app/TrainerCore.cpp).
The [native proof and remaining validation gates](../testing.md#experimental-live-context-refresh-bounded-a2-gates-exercised-qualification-pending)
separate bounded evidence from full-source acceptance. Supported scope remains
local non-NVIDIA Vulkan, not CUDA/NVIDIA, remote workers or GUI integration.

### Supported-device behavioral evidence and limits

The native Vulkan dev build, **16/16 headless**, AMD `fpbo_split_parity` and
**1/1 `cli_training_smoke`** passed. The real two-worker kitchen group completed
four updates per owner to step 10,004 with native full-state, contract, clocks,
shared-state and merged-output validation. Cooperative STOP retained unequal
owner steps 10,002/10,001 (exit 42, no completion); explicit native group resume
advanced each to 10,004 with the **same images, faces and pixels** as uninterrupted
four-update execution. Unowned packed state/bounds and shared state were exact;
each row's clock matched its original clock plus its owned updates.

Owned floating state is **not byte-identical** across interrupted and
uninterrupted execution. Native decoded parameters, moments and SH passed the
reused `fpbo_split_parity` 1e-4 screen; four raw bound-coordinate outliers
reached 4.05e-4 in mixed bound units and are diagnostic, not a decoded
failure or an exact-byte guarantee. The cause is not established. This is a
bounded four-update recovery observation, not arbitrary-long exact trajectory
or power-loss durability proof.

Actual owner termination triggered cooperative sibling stop (exit 2, no
completion); explicit restart safely reached 10,004. An unrelated
`complete.json` inserted while owners ran caused no-replace hard-link publication
refusal (exit 2), preserved its bytes and retained validated owner outputs;
after controlled removal of that test sentinel, explicit resume published
without retraining. Concurrent duplicate groups were refused by the root lease;
unrelated native writers were refused at current and prior exact owner output
directories. Source checkpoints and dataset remained unchanged.

The actual merged PLY passed full native readback and source-bound native
render/evaluation on all 132 held-out images. After only four owner updates,
raw PSNR was 20.3301 dB and color-corrected PSNR 24.6987 dB: a separate consumer
observation, **not** a quality gate or matched baseline. The native mesh-input
reader read 986,784 rows with finite geometry/DC while skipping SH; full
meshing and oversized-scene capacity are not proved. These four-update smokes
are correctness/consumer evidence; fresh performance selection is separate.

## Historical B1 proof progression

The experimental Vulkan path reuses the native trainer, quantized 3DGS FPBO
and full checkpoint. A deterministic y-spatial ranking assigns entire
256-row codec blocks to one of two owners; a non-owned workgroup exits before
FPBO's codec reductions and writes. The same mask skips per-splat bias-step
increments. Both workers keep the full source scene as frozen render context;
shared appearance and densification updates are disabled in this first
fixed-topology proof. The original DataManager consumes disjoint image-index
lists from a local, validated two-owner manifest. No separate numerical
trainer or dataset transformation was added.

The first AMD-device run resumed a 1M-active full checkpoint with owner 0,
514 of 1,195 training views and 1,953 owned 256-row blocks. It ran 183
steps and stopped cooperatively, retaining a non-full output. An in-process
hash of unowned world parameters, quantized FPBO value/moment state and
bias counters matched before and after the run. The 200-step all-owner
mask path previously completed with a 65.313 s measured interval, close to
the M0 single-device middle-window median of 65.492 s; that is not yet a
numerical one-block parity result. A stopped run does not establish merged
quality or resumability of a block-specific checkpoint.

The PLY writer now filters unowned rows in B1 mode without changing full
`state.tar` serialization. A native binary-PLY merge probe validates matching
schemas, exact byte lengths, complementary owner masks and unique source-row
IDs before concatenating two owned outputs. Its output is staged and
published by rename after native readback. A two-row synthetic merge passed;
a duplicate/unowned-ID input was rejected without publishing output. It
has **not** been exercised on a completed two-block pair. Owner masks and
original row IDs now accompany a B1 checkpoint as sidecars; a B1 resume
validates and reloads the mask rather than re-ranking moved means. The
per-image view manifest and its version remain separate local input, and
topology changes remain outside this fixed-state proof. The earlier owner-0
run predates these sidecars and cannot be used as a merge input.

Supported checks: Vulkan dev build, 13/13 headless tests excluding the
unrelated previously failing `delaunay_degenerate`, AMD `cli_training_smoke`
1/1, `fpbo_split_parity` pass, and `git diff --check`. One intermediate B1
attempt rejected nonfinite means at the retained middle snapshot; the
ranking now skips 7,916 nonfinite rows for centroid calculation while still
assigning every row to an owner. Another early attempt stopped during setup
before an output was created. Neither was counted as a timing result.

**Historical physical gate:** C: had about 758 MB free at that checkpoint.
Additional owner output, a merged 1M-row PLY and a matched one-GPU control
would not have fit with margin. Subsequent externally observed cleanup freed
space while retaining the source/full checkpoints; this agent's attempted
deletion did not act because both named redundant eval runs were already gone.

The hardened sidecar/merge path built under the pinned Vulkan toolchain and
passed a tiny native merge/readback check, including rejection of a tampered
owner ID. A new owner-0 run is still required because the earlier kitchen
artifact predates sidecar publication. The existing checkpoint publisher's
power-loss durability is not established by these B1 changes. No further
checkpoint-producing kitchen run was started once free space fell below the
needed margin.

## Two-owner fixed-topology screen

Both processes resumed the **same full step-10,000, 1M-active kitchen
checkpoint** on explicit AMD R9700 UUIDs. Owner 0 held 1,953 of 3,907
256-row blocks, owner 1 the complementary 1,954; their view manifest assigned
514/681 distinct training images. Each native run rendered the full scene,
updated only owned rows, froze shared appearance and topology, and completed
302 steps. The unowned world/quantized moment/bias-state hash was unchanged
in both workers. Owned PLY output and sidecars were checked by the native
merger: 484,096 + 484,305 = 968,401 valid rows in the first pair. Later
sidecars also account explicitly for filtered invalid/low-opacity/dead-scale
owned rows, so the merger requires unique complete coverage of all 1M source
row IDs across included and excluded lists.

For timing, the fixed all-owned control ran 502 steps from that checkpoint.
The progress-line interval excludes the first ~100 steps and compares **400
input-image exposures on one device** with **200 per block concurrently**.
Three independent repeats yielded:

| Repeat | All-owned 400-step interval | Two-block 200-step bottleneck | Aggregate steady exposure throughput |
|---|---:|---:|---:|
| 1 | 130.916 s | 64.553 s | 2.03× |
| 2 | 130.010 s | 64.930 s | 2.00× |
| 3 | 128.819 s | 65.248 s | 1.97× |

The median is 130.010 versus 64.930 s, **2.00× aggregate exposures per
steady-state wall time**, not 2× globally coupled optimizer updates. Each
block used 302 optimizer updates versus the control's 502, so the objective
and update counts differ. Setup, checkpoint save, partition construction,
merge and evaluation are not in those intervals. In one timed repeat the
two-process wall was 101.540 s versus 164.216 s for the all-owned control;
native merge/readback took 0.552 s. Held-out evaluation took 182.776 s for
the merge and 183.248 s for the control. Charging process+merge+evaluation
gives about 284.868 versus 347.464 s, **18.0% less wall** on this short
screen, before charging partition discovery/staging. The historical
provisional 20% short end-to-end signal was not reached; that threshold is
**superseded as a benchmark stop rule**, not a benchmark pass/fail criterion.
It is not a measured from-initialization time-to-quality gain.

The merged PLY was evaluated with the native renderer and the frozen
step-10,000 shared appearance against the same 132 held-out views as an
all-owned PLY. Three paired repeats had mean raw PSNR deltas of +0.096,
+0.050 and +0.019 dB; color-corrected means were likewise close. The
20-view overlap/large-projection diagnostic set stayed within 0.974 dB
of its paired control in every repeat. One other view regressed 2.668 dB in
the first pair but not the next two; an isolated-view quality guarantee is
**unverified**. This set was chosen from the frozen visibility trace but
was not a user-approved production boundary/quality envelope.
The later fully covered-ID pair merged 968,419 valid rows and scored 20.299
dB raw versus 20.363 dB for the available all-owned control; its worst
diagnostic-set delta was -0.641 dB, while one view outside that set was
-2.185 dB. These additional observations do not remove the per-view risk.

After hardening, B1 checkpoint sidecars carry the 256-row ownership mask,
included original row IDs and excluded original row IDs. Their latest
block header also binds the original source-checkpoint content identity and
the published step; the merger rejects mixed sources or steps. A full
owner-0 checkpoint in the earlier sidecar format restored its mask and ran
one further step with its unowned context hash unchanged; wrong-owner
resume failed closed. The latest source-bound sidecars were verified with
a tiny synthetic merge/rejection check and a real zero-step complementary
owner pair: 492,658 + 492,956 = 985,614 valid PLY rows, with all 1M original
IDs accounted for as included or explicitly excluded. At that historical
checkpoint, a **full-checkpoint resume in that sidecar format had not been
rerun**, and sidecars did not bind effective optimizer configuration or the
per-block view manifest. The merger could not detect mismatched training
contracts with the same source and step, and merged-PLY evaluation did not
independently bind shared appearance to the PLY source. Losing all sidecars
could make resume treat an owner checkpoint as a fresh source and re-rank
ownership. These were release blockers, not accepted recovery semantics.
The current marked native B2 contract and supported four-update resume proof
above supersede those missing-contract/resume statements; they do not upgrade
the historical measured runs to current-format recovery evidence.
Disposable timing runs explicitly opted into non-full checkpoints; ordinary
B1 runs required full resumable checkpoints. The merger rejected duplicate,
missing or foreign row IDs and published by no-replace hard link after native
readback. Power-loss durability, topology changes and a globally resumable
merged checkpoint remain unproved; current concurrent-writer refusal evidence
is scoped to the native group/owner paths above.

**Disposition at this checkpoint (historical):** the experiment shows a native two-device *approximate
block* throughput gain and a renderable merged model, **not** production B1,
B2 or a globally synchronous speedup. The 18.0% short-wall observation excludes
partition discovery/staging and is not grounds to stop at an arbitrary 20%
threshold. Its historical recommendation was matched fixed-kitchen B1/O1/G1
work, with setup, partition/staging, checkpoints and merge/evaluation charged,
throughput and wall/memory reported separately, and differing optimizer counts
disclosed. That recommendation does not require historical inputs for new work:
future comparisons use freshly admitted selected-data bindings/references and
current approval under the policy above. Time-to-quality, boundary quality,
recovery and oversized-scene capacity remain separate production/release questions.
See [benchmark selection and separate release gates](multi-gpu-training-plan.md#10-benchmark-selection-and-separate-release-gates).

The final observed scratch capacity after the source-bound zero-step proof
and merge was about 1.01 GB on C:. No further checkpoint-heavy validation
was started. The two redundant evaluation-image runs disappeared after the
user authorized narrow cleanup but before this agent's deletion command
could act; the actor is not established by that command. The original
dataset, retained full checkpoints and measured B1 outputs were preserved.
