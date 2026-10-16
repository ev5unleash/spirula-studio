# B2 view-policy ablation — 2026-10-02

Status: **V0–V5 COMPLETED on 2026-10-02. Full-view policy reduces, but does not eliminate, the observed quality gap.**
The user approved this separate experiment after [D0–D5 completed](multi-gpu-training-b2-causal-20261001.md#execution-evidence).
The immutable full-view policy is implemented and passed native validation;
see the [execution evidence](#v0v2-execution-evidence-and-staged-admission).
Accepted P/F primary work, matched metrics, native visual evidence and the
[qualified disposition](#v5--completed-disposition-and-next-decision) are recorded below.

**Future-evaluation authority (2026-10-07):** The
[selected raw dataset policy](multi-gpu-training-plan.md#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
governs all future evaluations: freshly admitted full-resolution `.insv`
originals, not `.lrv` previews or existing LRV-derived images/masks.
Kitchen and Source v5/readback 2,688/298 acceptance, measurements and protocols
are historical, not admission for the new selection. Fresh input/source/binary
bindings and matched native references are required; execution remains blocked
pending current approval within the master's unchanged execution/custody budget.

## Question and single intervention

Does replacing B2's per-owner training-view subsets with the **same complete
training-view schedule for both owners** reduce its observed quality gap?

Compare partitioned-view B2, full-view B2 and the corresponding native all-owned
reference at equal per-row update counts. Keep Gaussian ownership complementary,
full-scene frozen render context, shared appearance, topology, source, horizon,
loss/optimizer settings and unordered native raster unchanged. Only the view
policy changes. Support, frequency/weighting and order change together as that
policy; do not claim this experiment isolates those three effects individually.

Native training remains the practical default. B2 remains experimental; Q's
failed longer recovery and D's no-save variability stay recorded. This is not a
recovery repair, globally exact training, production release or new architecture
benchmark. No context refresh, finetuning, topology growth, view-dependent row
ownership, deterministic-raster switch, quality threshold, numerical relaxation,
O1/G1 work or GUI/remote integration is included.

The V0–V5 sections below preserve the executed experiment's original contract,
not an active handoff. Completed evidence and disposition follow those sections.

## V0 — bind the comparison and admit resources

Resolve the local Q qualification and `b2-causal-20261002T024609Z/causal.manifest.json`
records. Bind original step-10,000 source/config, the validated camera/view
assignment file, ordered dataset training/held-out IDs, owner masks, shared-state
identity, native sampler and 30,000-step horizon. Preserve every source row,
including inactive rows and the 64-valid-row codec tail. Keep private paths and
UUIDs in a new local experiment manifest. Rehash protected inputs and binaries.

Predeclare these arms before implementation results are inspected:

| Arm | View policy | Work from source step 10,000 | Output boundary |
|---|---|---|---|
| P, fresh matched baseline | Existing partitioned views | 10,000 updates per owner; 20,000 aggregate exposures | Step 20,000 per owner |
| F, new treatment | Full native training-index schedule for each owner | Same per-owner/aggregate update counts as P | Step 20,000 per owner |
| N, native reference | All-owned B1-sampler native reference | 10,000 total updates/exposures | Step 20,000 |

Use the retained D3 step-20,000 all-owned reference and its actual exposure trace
when identities and numerical implementation still apply. It is **not** an
ordinary differently seeded native run or the growth-enabled source's historical
step-20,000 checkpoint. Run a fresh N only if its identity/schedule cannot be
bound or a necessary fix changes numerical training behavior. A metadata schema
revision alone is not a reason to repeat its training. Reevaluate retained models
with the current evaluator when evaluator/model/view-order identities differ.

P is a contemporaneous baseline for the new policy and build, not a repeat of Q's
failed recovery check. Retain old P/D metrics as historical context; do not pool
interrupted Q output as an independent no-save repeat. P and F use one common
build, source, per-owner device assignment and all non-policy settings. N has
half the aggregate exposure count of either two-owner arm. F duplicates each N
training exposure across GPUs; disclose this work difference even if its quality
improves. Capture exact input/face/dimension/pixel sequences for all new arms.

Refresh admission: **24 GiB worker backing**, **12 GiB sampled aggregate host**,
**8 GiB new live outputs**, **10 GiB minimum free scratch**, and the existing
worker-local heap-headroom rule. Preserve stricter inherited limits. Include
build/smoke artifacts, both owner checkpoints, merged outputs, all attempts,
evaluation/oracle workspace and raw/archive coexistence; run GPU arms sequentially
apart from each normal two-owner group. D's old free-space sample is not admission.

Use the implemented `SS_WORKER_DEVICE_LIMIT_BYTES` enforcement and actual worker
reports, not separate-process budget samples. Direct workers set
`SS_WORKER_CONTROL=1` and keep writable stdin for STOP. Backing allocation peaks
are not physical VRAM peaks. Source/B2/Q/D checkpoints, manifests, ZIP dependencies
and `evidence-bases` are protected. Only newly created redundant expansions may
be removed after verified lossless packaging; no blanket Temp cleanup, assumed
future compression savings, cap increase or silent reduction of the work.

**Exit:** identity/change inventory, complete storage/coexistence ledger, exact
work and resource-monitor contract. Unsafe resources block dependent launches;
finish reachable implementation/checks and report the exact missing capacity.

## V1 — add one explicit immutable view policy

Implemented interface: `train-blocks --view-policy partitioned|full`, with
`partitioned` as the unchanged default. Help and errors use existing i18n.
The implementation reuses the native trainer, manifest, sampler and merge path.

| Existing implementation | Change/use |
|---|---|
| `src/app/B1Group.cpp` | Parse the policy on new groups; persist it in immutable group identity and derive trusted worker configuration from that identity. No resume overrides. |
| `src/app/TrainerCore.cpp` | Validate views before DataManager creation. Apply filtering only for partitioned owners; retain the original full training-index list in full mode. |
| `src/data/DataManager.cpp` | Reuse the existing fixed-seed independent B1 RNG, epoch grouping and source-relative sampler offset; do not write a new schedule implementation. |
| `src/engine/EngineB1.cpp` | Preserve row ownership and frozen context; bind effective view policy and actual train indices into owner identity. |
| `src/checkpoint/B1Contract.h`, `src/checkpoint/B1Contract.cpp` | Persist/validate view policy separately from the existing numerical policy; enforce compatible source/sampler/policy on resume and pair validation. |
| `src/checkpoint/B1Merge.cpp` | Reuse shared contract validation and native readback/no-replace publication; never merge mixed-policy owners. |

Both modes still require `--views MANIFEST`. Parse and hash the existing exact
one-0/1-entry-per-source-camera manifest with the same integrity checks. In full
mode its assignment bits are provenance, not a training-index filter: select
exactly the dataset parser's **training** indices, never held-out cameras. Bind
the manifest digest, full ordered train-index digest and policy. Do not infer
full mode from missing/empty/malformed assignment data or rewrite its contents.

Keep row masks identical to P for each owner. In full mode both owners must have
the identical full ordered train-index set and effective B1 sampler identity as
N, with the same source step, seed, batching/face metadata and configuration.
Owner identity must not salt that schedule. Partitioned mode retains its existing
owner-specific train-index sets and scheduling behavior.

Version the existing strict group/checkpoint/worker schemas only where required.
Read historical owner-group versions as partitioned, preserving old all-owned
reference semantics. Never rewrite retained groups, checkpoints or source metadata.
New versions require an
explicit valid policy; unknown/missing new-version fields fail closed. Policy
cannot be changed by `--resume-group`, inherited environment, a worker request
or edited sidecars. Validate actual train-index coverage, not just two equal
hashes that could both describe an incomplete subset. All-owned references keep
their native all-owned meaning; do not impersonate owner 2 in a two-owner group.

Inspect references before changing exported interfaces, migrate their callers
and preserve the existing ordinary single-GPU path. The group contract remains
the authority; avoid a second independent policy setting in worker plumbing.

**Exit:** minimal native policy implementation with strict input, checkpoint,
pair/merge and consumer enforcement. No long training before V2 passes.

## V2 — prove that only view policy changed

Extend relevant behavioral checks rather than adding another test framework:
`data_sampler_test`, `checkpoint_resolution_test`, `worker_request_test`,
`b1_merge_contract` and the native CLI lifecycle checks. Cover:

- Partitioned default and historical artifacts retain their exact semantics.
  Missing/invalid policies in new schemas, malformed/changed manifests, held-out
  leakage and incompatible source/sampler/owner inputs are rejected.
- Full-mode owner 0, owner 1 and the **all-owned B1-sampler reference** produce
  identical input/face/sub-batch sequences across an epoch boundary and resumed
  offsets. The target here is not an arbitrarily seeded non-B1 sampler.
- Full owner train sets each cover all 1,195 source training IDs in native order;
  owner masks remain complementary, and frozen/shared state remains unchanged.
- Mixed-policy checkpoints/merges and policy changes on resume fail before
  publication, preserving existing output. Complete owned/excluded source-row
  coverage and native final PLY readback remain required.

Build through the dev Vulkan entry point, run relevant headless/worker checks
and supported-AMD runtime/CLI/FPBO checks once after integration. Exercise a real
small full-mode group plus STOP/resume to establish policy persistence, sampler
replay, checkpoint validation and no-replace completion. Reuse existing sampler
checks for the whole epoch rather than adding a long GPU smoke just to count IDs.
Do not call a structural/sampler smoke a longer numerical recovery qualification.

Bind `SS_M0_EXPOSURES=1` and actual worker backing reports in the native smoke.
Disable causal diagnostic modes; they replay timing or use synchronous decoding
and would introduce another intervention. Clear stale owner/evaluation/stop and
legacy `SSPLAT_` switches before coordinator launches. Verify actual numerical
configuration, faces/resolutions and any adaptive settings; source-derived
sampler equality is not evidence that worker invocation passed those settings.

**Exit:** native accepted full-policy workflow and refusal evidence, with matching
full-view sequence and unchanged ownership/numerical settings. Fix attributable
implementation defects and repeat affected checks before advancing.

## V3 — execute the matched policy arms

Launch P and F in separate fresh output directories using the validated source,
same manifest/devices and `--updates 10000`. P selects `--view-policy partitioned`;
F selects `--view-policy full`. Both preserve the original 30,000 horizon.
Do not extend a completed group or start F from trained P owner checkpoints.
Choose and record run order before launching; no claim of randomized or repeated
performance follows from this single matched pair.

Use full/latest-only checkpoints, the source's unchanged save cadence and native
source/contract/context/clocks/coverage oracles. Each owner must commit 10,000
updates to step 20,000 and publish only the validated complete group. Compare
F's entire 10,000-event sequence **per owner** with N's matching reference trace;
compare original-row ownership against P. Record P's intentionally different
view schedule, per-view frequencies and actual aggregate pixel work. Do not
force those counts equal by dropping events or changing resolution.

A resource STOP or worker failure produces an incomplete primary arm, not an
accepted quality sample. Preserve its evidence. After the cause is resolved and
resources are admitted, use a fresh uninterrupted group for the primary sample;
do not pool a resumed trajectory into P/F because Q's long recovery is unqualified.
Reuse validated preparation, charge actual finalization and record observed wall
and resource data. No profiler in primary runs; evaluation stays separate.
These single-run costs are context, not a new cold/reuse median or speed claim.

If fresh N is required by V0, use the established native control: original source,
`SS_B1_OWNER=all`, `SS_B1_STOP_AT_STEP=20000`, `SS_WORKER_CONTROL=1`, full/latest-only
checkpoint and original horizon 30,000. Verify actual 10,000 events and step-20,000
checkpoint; cooperative exit 42 is expected but insufficient on its own.
Do not set the horizon to 20,000 or substitute a different sampler.

**Exit:** accepted P/F artifacts and bound N reference, work/config/mask/sequence
comparisons and worker-local resource evidence. No repetitions merely to find a
favorable quality outcome; failed/incomplete attempts remain excluded and visible.

## V4 — measure the policy effect without overclaiming it

Evaluate P, F and N through the same current source-bound native path on the
exact **132 named held-out views**, frozen appearance and evaluator settings.
Use existing native metrics/identity mapping and valid all-owned/merged model
contracts; no forged PLY metadata or new metric implementation. Reuse N metrics
only with matching model/evaluator/source/view identities, otherwise reevaluate
its retained model. Clear training-only switches during evaluation.

Report mean/median raw and color-corrected PSNR, SSIM and L1, all per-view deltas,
regression counts and worst views for F−P, F−N and P−N. Include the historical
Q/D P−N gap separately and disclose any fresh-P variation, not as a statistical
noise bound or a rerun that erases the old failure. Review bounded native-render
examples for the worst remaining regressions under the admitted image budget;
these are post-results diagnostics, not a newly invented boundary test set.

For raw and color-corrected mean PSNR separately, report the descriptive gap
closure `(F - P) / (N - P)` when the denominator is meaningful. Do not clip it,
average per-view ratios, call it an isolated causal percentage or hide a negligible
denominator. Report absolute dB changes alongside it. No PSNR acceptance floor,
confidence interval from one pair or arbitrary improvement threshold is implied.

A consistent observed improvement implicates **view policy in this experiment**;
it does not separately identify coverage, weighting or ordering, exclude native
variance, establish a population effect or show that frozen context is harmless.
Little improvement means full-view sampling alone did not resolve the observed
gap. Mixed metrics or gains too ambiguous for one pair remain inconclusive;
do not selectively repeat the winning view or silently broaden the experiment.

**Exit:** complete source-bound per-view report and a qualified policy conclusion,
with work/cost and uncertainty visible. Better quality alone is not acceleration:
F duplicates N's exposures across both GPUs and may lose any useful wall advantage.

## V5 — close this experiment and name the next decision

Update this execution record, master status, scorecard and usage limitations.
Keep Q/D failures and the original throughput table intact. Bind exact commands,
policy/schema/sampler identities, artifacts, actual event sequences, numerical
invariants, resource reports and metrics in the new local manifest.

| Observed evidence | Disposition |
|---|---|
| Full-view policy consistently improves the observed quality gap | Report the magnitude and remaining per-view loss. A cheaper coverage-preserving schedule is a possible next research decision, not an automatically authorized implementation. |
| Full-view policy leaves substantial observed loss or worsens output | View policy alone is insufficient in this run. Keep native default; do not tune view partitions indefinitely or add context refresh in the same experiment. |
| Quality improves but required work/cost removes a useful apparent benefit | Record the tradeoff; do not call the variant a practical speedup. A comparable-quality complete-wall benchmark would be separate work. |
| Results are mixed or inconclusive | Close with precise uncertainty and a bounded follow-up recommendation; no confidence or release pass is invented. |

Autonomously complete V0–V5, including in-scope contract/sampler fixes and actual
native verification. A bug that prevents the defined intervention is actionable
work, not a reason to stop with another plan. A genuinely unavailable source,
supported device or safe storage blocks dependent runs; finish reachable work
and state the exact missing prerequisite. Never make a shorter or resumed run
stand in for the declared primary arm.

**Completion:** implemented explicit policy, passing native contract/sampler
checks, accepted matched P/F plus bound N, full per-view/cost report and one
recorded disposition. A scientifically negative/inconclusive result completes
the experiment; unimplemented policy, untested code or missing primary data does
not. Quality acceptance, broader numerical recovery, context-refresh experiments,
new speed benchmarks and release approval remain outside this finish line.
Leave changes local; no push, PR or unrelated cleanup is authorized.

## V0–V2 execution evidence and staged admission

V0 bound the original step-10,000 source/config, 30,000 horizon, validated view
manifest, complementary original-row masks, training/held-out order and retained
D3 all-owned N reference. The predeclared primary order is **P then F**, sequential
two-owner groups. Exact private commands, identities and artifacts remain in the
local experiment manifest; none are published here.

V1 implemented immutable `partitioned|full` policy through the existing native
group/worker/checkpoint/merge path. New group and owner-checkpoint schemas are
version **2**, with explicit valid policy and full ordered training-index binding;
bound worker payload version **2** derives policy from the immutable group.
Historical group version 1 and owner 0/1 checkpoint version 1 remain partitioned;
historical owner 2 retains its all-owned native/full semantics. Retained artifacts
are not rewritten. New-schema missing/unknown policy, edited view data, mixed
policy/schema pairs, resume overrides and inherited `SS_B1_VIEW_POLICY` /
`SSPLAT_B1_VIEW_POLICY` overrides fail closed. A historical-owner writer defect
found during review was fixed without changing numerical training behavior.

V2 accepted the integrated binary SHA-256
`8f385f5ab4e65df63dc33021ab61811a96b8589e0a27083cbd5661450491f97c`:

| Observed check | Result |
|---|---|
| Dev Vulkan build | PASS, 88.73 s |
| Headless contracts/sampler/worker checks | 17/17 PASS, 7.33 s; includes strict versions, mixed merge, held-out exclusion and epoch/resumed offsets |
| Explicit supported-AMD CLI lifecycle | 1/1 PASS, 1.72 s |
| Supported-AMD backing-cap/invalid-cap and FPBO split checks | PASS, 0.71 s |
| Real full-policy group, both owners 10,000→10,004 | Complete exit 0, 27.1934424 s wall |
| Real full-policy STOP/resume | Monitor threshold 10,001; actual full checkpoints 10,002 for both owners, exit 42; explicit resume exit 0, both owners 10,004, 30.4705891 s resume wall |
| Six native refusal cases | Exit 2 with bound outputs preserved: missing/unknown policy, changed manifest, both inherited-policy spellings and resume policy override |

Each full owner covers all **1,195** parser training IDs in native order, excluding
held-out IDs; ordered digest is
`2f660ac95c1e296fde2496688061be684b4d0af96effbef472b389a03925253a`.
Each four-update owner trace has four events and **49,355,136 pixels**, exactly
matching N's images/faces/dimensions/pixels. Existing native oracles verify
complementary masks, unchanged source numerical configuration, exact frozen/shared
state, source-relative clocks and complete included/excluded original-row coverage.
STOP/resume proves policy persistence and structural/sampler/publication behavior,
**not** longer numerical recovery qualification. Q/D failures remain unchanged.

Actual committed/reservation backing peaks for owners 0/1 were
**1,883,821,840 / 1,883,823,744 bytes** (full),
**1,815,472,352 / 1,815,471,680 bytes** (STOP) and
**1,864,190,992 / 1,864,192,928 bytes** (resume). All six worker reports bound
the **25,769,803,776-byte** cap and 1 GiB headroom, with zero allocation failures
or cap violations. These are application-owned backing, not physical VRAM peaks.

### Storage staging and admission record

The initial conservative all-phase raw/archive coexistence ledger was
**12,676,561,758 bytes**, above the 8 GiB new-live limit. Initial free scratch was
14,503,288,832 bytes: only 3,765,870,592 bytes above the 10 GiB floor. Unstaged
V2 (4,688,190,542 bytes) and V3 raw work even without smoke residual/archive
(4,283,865,328 bytes) were not admitted by that sample.

The implemented staged contract reserves **3,438,635,060 bytes** for one V2 group
and **2,699,601,541 bytes** for one primary-arm lifecycle without build reserve.
Run and validate one stage, losslessly package only new redundant owner expansions,
retain merged outputs and all evidence dependencies, then remeasure free/live
storage before the next launch. Raw/archive coexistence is included; packaging
upper bounds use raw × 1.01 + 1 MiB per invocation, not assumed compression credit.
Protected source/B2/Q/D evidence is never deleted or rewritten.

Measured first-smoke ordinary ZIP reductions were only **2,358,848** and
**2,279,148 bytes** for the two owners; new live evidence after both was
**1,249,411,880 bytes**. The larger observed volume free-space change is not
attributed to those reductions. Subsequent lossless compaction produced a
165,294,032-byte full-smoke archive (33 native files) and a 526,424,994-byte
STOP/resume archive (58 files). Six exact checkpoint restorations passed native
oracles; both restored final pairs reproduced the original whole merged PLY hash
through native merge. Protected source/evidence dependencies remain unchanged.
Fresh measured free scratch **15,020,220,416 bytes** and new live evidence
**1,190,286,742 bytes** admitted sequential P. The larger volume delta is not
all attributed to compaction. Fresh uninterrupted P and F completed after separate
admission; current-binary P/F/N evaluations all exited 0.
All caps remain 24 GiB actual worker backing, 12 GiB sampled aggregate host,
8 GiB new live outputs and 10 GiB minimum free scratch, plus worker-local headroom.
V0–V5 are complete. No new throughput median, comparable-quality acceleration
or release/recovery qualification is implied.

### V3 — accepted fresh partitioned P and full-view F

Fresh uninterrupted P exited **0** with both owners at **20,000** and exactly
**10,000 updates/exposure events each**. Native source, frozen/shared state,
source-relative clocks, included/excluded coverage, original masks and numerical
configuration oracles passed. Owner 0/1 pixel work was
**123,427,264,320 / 123,442,742,016**, aggregate **246,870,006,336** pixels.
The bound work/resource report retains exact events, view frequencies and native
backing reports. Complete observed wall was **3,244.3435476 s**, sampled aggregate
RSS maximum **1,824,309,248 bytes**, minimum free scratch **10,840,924,160 bytes**
and maximum new live evidence **2,664,536,917 bytes**. These are one-arm costs,
not a repeated throughput benchmark or physical memory peak.

Fresh uninterrupted F also exited **0**, both owners at **20,000**, with
**10,000 updates/exposure events each**, passing the same native source/config,
frozen/shared, clocks, original-row coverage and complementary-mask oracles.
Both F owners match N's **entire** 10,000-event input/face/dimension/pixel sequence:
SHA-256 `2bb0631b2052e0e3b0f932b8385d54c0d5b3340eacbcbb313a43d63f95e3022b`.
Each F owner processed **123,433,689,024 pixels**; aggregate
**246,867,378,048**. P's aggregate was **246,870,006,336**, not forcibly equalized.
P/F each have 20,000 aggregate exposures versus N's 10,000, while every owned row
receives 10,000 updates under the unchanged 30,000 horizon.

F complete wall was **3,244.5429766 s**, sampled aggregate RSS maximum
**1,871,175,680 bytes**, minimum free scratch **11,798,421,504 bytes** and maximum
new live evidence **3,220,885,606 bytes**. P owner 0/1 actual backing peaks were
**2,152,096,336 / 2,088,431,664 bytes**; F peaks were
**2,123,712,384 / 2,123,010,672 bytes**. All four workers bound the 24 GiB cap and
1 GiB headroom with zero reported allocation failures or observed cap violations.
Backing is not physical VRAM; sampled host memory is not a true peak.

P compaction retained merged output/sidecars and reduced new owner evidence from
**901,699,982** to **393,152,267 bytes**, a **508,547,715-byte** logical reduction.
All four reconstructed file hashes and bound native oracles passed. Lossless
NTFS compression of five newly created baseline paths retained hashes/sizes/inodes;
measured unique allocated reduction was **66,393,901 bytes**. Fresh free scratch
**13,518,446,592 bytes** admitted F. Its corrected source-derived raw/save-stage/
workspace bound was **2,675,620,080 bytes**, below the stricter retained
**2,699,601,541-byte** runner guard. No protected original was changed.

The real mixed P-owner-0/F-owner-1 native merge refused the unmatched pair with
exit **1**, no output; the existing-output case also exited **1** and preserved
the sentinel. These are `b1_merge` refusals, not coordinator exit-2 results.

### V4 — matched 132-view metrics

P, F and retained D3 N were evaluated sequentially with the same current native
binary, frozen appearance and source/config/COLMAP/name ordering. All evaluations
exited **0**, each with **132 named held-out slots**. N was reevaluated, not
retrained; all **792 metric values** (six arrays × 132) match retained D3 exactly,
not a claim of whole metrics-file byte identity. Models/source/config hashes are
separately bound. The evaluator does not emit independent per-slot input/RGB
digests: output metric fingerprints and exposure pixel counts are not RGB identity
proof, and encoded/decoded source RGB was not independently prehashed.

| Metric | P mean / median | F mean / median | N mean / median |
|---|---|---|---|
| Raw PSNR (dB) | 19.119402 / 19.275200 | 20.168279 / 20.232600 | 20.765146 / 21.136850 |
| Color-corrected PSNR (dB) | 22.952313 / 22.449500 | 24.240206 / 24.111850 | 25.290608 / 25.080200 |
| Raw SSIM | 0.789396 / 0.811381 | 0.801070 / 0.830130 | 0.807975 / 0.834226 |
| Color-corrected SSIM | 0.823133 / 0.834520 | 0.834384 / 0.847808 | 0.842222 / 0.855878 |
| Raw L1 | 0.090871 / 0.079745 | 0.082215 / 0.071032 | 0.078694 / 0.064125 |
| Color-corrected L1 | 0.048419 / 0.048083 | 0.041527 / 0.039600 | 0.035317 / 0.033874 |

Deltas below are candidate minus reference; lower L1 is better. Regression counts
are per-view, out of 132. Delta medians are medians of paired deltas, not
differences of the model medians.

| Metric | F−P mean / median; regressions | F−N mean / median; regressions | P−N mean / median; regressions |
|---|---|---|---|
| Raw PSNR | +1.048877 / +0.780450; 28 | −0.596867 / −0.476900; 103 | −1.645745 / −1.152350; 111 |
| Color-corrected PSNR | +1.287893 / +0.944650; 21 | −1.050402 / −0.917050; 125 | −2.338295 / −1.963600; 127 |
| Raw SSIM | +0.011674 / +0.012938; 26 | −0.006905 / −0.006484; 107 | −0.018580 / −0.017928; 115 |
| Color-corrected SSIM | +0.011251 / +0.008702; 16 | −0.007838 / −0.007207; 128 | −0.019090 / −0.016110; 129 |
| Raw L1 | −0.008656 / −0.006778; 32 | +0.003521 / +0.004313; 97 | +0.012177 / +0.009097; 105 |
| Color-corrected L1 | −0.006892 / −0.005218; 22 | +0.006209 / +0.005876; 127 | +0.013101 / +0.010304; 127 |

The local per-view CSV/report retains every paired delta and worst-view ranking.
Worst F−P raw/cc PSNR are slots **112 / 82**, **−1.3028 / −1.5774 dB**;
worst F−N are slots **49 / 117**, **−4.0630 / −5.6718 dB**;
worst P−N are both slot **49**, **−10.0040 / −7.9508 dB**.
For F−N, slot 49 is `IMG_20260906_194034.604.jpg`, slot 117 is `IMG_2160.jpg`.
Their union was selected after metrics for bounded native-render diagnostics,
not as an independently chosen quality test set; native visual review is complete.

Descriptive mean-PSNR gap closure `(F−P)/(N−P)` is **63.732684% raw** and
**55.078309% color-corrected**, with denominators **1.645745 / 2.338295 dB**.
These ratios are unclipped differences of means, not averaged per-view ratios,
isolated causal percentages or population/confidence claims.

Fresh P exceeds retained D's P by **0.028618 / 0.019620 dB** raw/cc mean PSNR,
not a noise bound or a reason to erase D's **−1.674363 / −2.357914 dB** equal-row
gap. Q's **−1.602366 / −2.534380 dB** gap and failed long recovery remain intact.
All six aggregate metrics improve F versus P, but all remain worse than N.
Full policy materially reduces this observed gap without eliminating it;
support, weighting and order change together, and no unique remaining cause is proven.

P/F complete walls **3,244.3435 / 3,244.5430 s** are single-run context.
Retained N training wall **3,224.7341 s** is noncontemporaneous historical context,
not a new speed comparison or median. Current P/F/N evaluation walls were
**191.7039 / 181.4079 / 204.7263 s**, separate from primary training; sampled
RSS maxima were **6,401,421,312 / 6,770,012,160 / 6,616,461,312 bytes**.
Better quality alone is not acceleration; F duplicates N's exposure work.

### V4 native visual evidence

Six sequential source/model/config/binary-bound native render runs exited **0**.
Twelve full-resolution native PNGs and two reviewed GT/P/F/N contact sheets occupy
**124,215,311 bytes**, within the admitted **700 MiB** image budget. The contact
helper verified equal native GT pixel bytes across P/F/N for the two selected
slots only. This is not all-132-input identity or retrospective RGB prehash proof.
Contacts are resized for presentation; unchanged native originals are retained.

At slot **49**, P showed conspicuous mottled/smeared door, floor-mat and foreground
surfaces. F substantially reduced those gross artifacts and restored cleaner
edges, while wall/door shading and edge errors remained relative to N/GT.
At slot **117**, F reduced P's cabinet/shelf and surrounding-surface smearing;
broad shading and fine-boundary differences remained. Slot 117 was chosen by
color-corrected PSNR, but these images are **raw RGB**: no color-correction
failure mechanism is inferred. These post-results examples support improvement
with remaining error, not systematic visual acceptance or numerical root cause.

### V5 — completed disposition and next decision

**Full native view policy materially reduces the observed B2 quality gap, but is
insufficient in this one matched pair.** All six aggregate metrics improve versus
contemporaneous P; raw/cc mean PSNR gains are **1.048877 / 1.287893 dB** with
descriptive gap closure **63.732684% / 55.078309%**. F still loses
**0.596867 / 1.050402 dB** versus N, with **103 / 125** per-view PSNR regressions
and substantial worst-view loss. The render examples retain visible errors.
Coverage, weighting and order changed together; native variability and remaining
context/trajectory effects are not isolated, and no population effect is established.

The implemented immutable policy, native checks/refusals, admitted uninterrupted
P/F, bound and reevaluated N, complete per-view/cost report and visual review meet
the finite V0–V5 completion contract. Exact commands, hashes, events, resource
reports, verified archives and metrics are retained in the local manifest.
No new primary sample or favorable repeat was substituted for this declared pair.

**Native training remains the practical default; B2 remains experimental.**
Original Q longer decoded recovery remains **FAILED**, D's numerical/quality
negative evidence is unchanged, and the historical three-repeat throughput table
is untouched. This result establishes neither comparable-quality acceleration
nor a clear useful apparent cost advantage: F doubles N's aggregate exposures,
and its single-run wall does not establish a new benchmark. No release, capacity,
broader recovery or quality qualification is granted.

Full-view B2 context refresh, cheaper coverage-preserving schedules and remaining
context/trajectory alternatives are prospective research directions, not a
standing execution instruction. Any future evaluation uses fresh selected-data
admission, bindings and matched native references under the policy above, and
requires current approval within the unchanged blocked custody budget.
Existing O1/G1 schedules remain parked/closed; materially different candidates
have the same admission/approval requirements. V0–V5 is closed historical
evidence, not an active handoff, execution of A0–A6 or a recommendation to tune
partitions indefinitely.
