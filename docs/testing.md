# Testing

Use CTest's `headless` label for the ordinary post-build feedback loop. GPU
integration and native parity tests are separate gates; desktop rendering and
input still require the real application.

**Support policy:** NVIDIA support is deprecated, including CUDA and Vulkan on
NVIDIA GPUs. Do not run or extend those lanes unless explicitly re-enabled by
the user. Active GPU checks use Vulkan on non-NVIDIA hardware; CPU-only checks
remain supported. Historical cross-backend results below are reference material,
not current acceptance requirements. See [AGENTS.md](../AGENTS.md).

## Quick headless loop

```bat
build_develop.bat -DSS_BACKEND=vulkan
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error -j 2
```

On Linux use `bash build_develop.bash` with the same options; on macOS the
build directory is `build`. Use `-L fast` for deterministic CPU checks,
`-L worker` for subprocess/scheduler contracts, and `-R <name>` to focus.
Keep `--no-tests=error`: an empty selection is not a successful check.
Do not use bare `ctest` when intending to avoid GPU workloads.

The Vulkan build registers 17 core headless tests and five additional GUI-adjacent
tests when GUI support is enabled. None opens a window or initializes a compute
device. `-DSS_BUILD_GUI=OFF` omits the batch, preset, argv and two stamp tests.
Historical CUDA builds also omit
`split_faces_test`, `frustum_size_test` and `bilagrid_selector_test` unless built
with `-DSS_BUILD_BACKEND_TESTS=ON`. Configure output identifies these differences.
Kernel parity and weight-dependent model tools are not automatically registered.

`cmake/SsRunTest.cmake` allocates a unique root for every CTest invocation of a
test, sets isolated temp/home/config/cache paths before launching it, and sets
`SS_NO_AUTO_FETCH=1`. This wrapper also isolates older tests that use fixed names
inside the system temp directory. Invoke those through CTest rather than assuming
direct executable invocations are concurrent-safe. Worker drivers receive the
target-resolved `spirula` path; no PATH lookup chooses another build.

`data_sampler_test` checks resume offsets against uninterrupted image scheduling
across epochs, CPU/disk caches, heterogeneous batches, prefetch/worker counts
and intervening validation reads, including ordered decode retry.
`b1_merge_contract` checks complementary owner merge/native consumer readback,
source-bound merged and canonical all-owned checkpoint appearance, no replacement,
and rejection of changed PLY bytes, foreign source/shared state, native step/config
mismatches, relocated/plain PLYs, different horizons, duplicate/incomplete IDs,
missing/tampered sidecars and unmarked/partial owners. `worker_request_test`
also rejects invalid block-owner payloads and
unequal, incomplete or stale group completion. These are headless contract
checks, not supported-device training recovery proof.

On Windows, `subprocess_test` cancels the nested helper after its `READY`
output, so the cancellation check does not depend on PowerShell startup speed.

On failure, read `Testing/Temporary/LastTest.log` and the printed directory under
`Testing/Artifacts/<test>-<unique-id>/`: it retains `process.log`, requests,
results, isolated application state and relevant outputs. Successful roots are
removed. CI runs the headless label on Windows, Linux and macOS and uploads its
job-owned `Testing/` directory on failure. Fast/worker/GPU outer timeouts are
60/300/600 seconds; subprocess drivers bound and reap their own children.

## Real training lifecycle

After building Vulkan, explicitly select a compatible non-NVIDIA device:

```bat
rem Prefer the non-NVIDIA device UUID printed by the device listing.
set SS_TEST_DEVICE=uuid:YOUR_DEVICE_UUID
cmake -E chdir build_vulkan ctest -L gpu --output-on-failure --no-tests=error
```

On POSIX prefix the CTest command with `SS_TEST_DEVICE=<selector>`. This
test-only selector is required and passed explicitly to the production CLI:
Vulkan accepts its normal selectors and canonicalizes to UUID.
Missing/unusable hardware fails, never skips. The driver does not change user
device preferences or download models.

`cli_training_smoke` generates six 64×64 images and 64 seed points, trains
12 steps with a 256-splat cap and full checkpoints every five steps, then resumes
through the scheduler/worker to step 24. It validates checkpoint contents using
the production readers and matches the request/result identity and output paths.
Longer finite attempts synchronize on a readable checkpoint before cooperative
stop or force-stop. The former must be resumable; the latter must be interrupted,
not successful, and retain a valid checkpoint. `checkpoint_resolution_test`
separately rejects incomplete newer siblings. Neither test claims a deterministic
kill during the atomic publication window.

The `training_gpu` CTest resource lock serializes this test within an invocation;
it is not a system-wide reservation against another trainer. Run hardware lanes
one at a time. No exact stochastic model bytes or loss trajectory are compared.

### Bounded native two-owner group validation

Use two distinct supported non-NVIDIA Vulkan UUIDs, a native full source
checkpoint, its validated two-owner view manifest and a new group directory:

```bat
build_vulkan\spirula.exe train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_GROUP --updates 4
build_vulkan\spirula.exe train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_FULL_GROUP --updates 4 --view-policy full
build_vulkan\spirula.exe train-blocks --resume-group GROUP
```

`--updates` is per owner and preserves the original optimizer horizon.
Resume uses immutable `group.json` and the validated latest full checkpoint
for each owner; it accepts no input overrides. While the first process is
running, send newline-terminated machine commands through redirected/piped
stdin (not interactive console input): `STOP` requests groupwide cooperative
save; `STOP 0` or `STOP 1` requests only that active owner's cooperative save,
after which terminal/refresh-abort handling stops the sibling. Missing or
terminal owner targets are ignored. Only these exact strings and `CANCEL`
are recognized, with either LF or CRLF; other strings are ignored.
`CANCEL` forces existing job cancellation; it is not cooperative save.
Then run `train-blocks --resume-group GROUP` for an incomplete group, without
policy or other overrides. Owners may stop at different steps. Run an
uninterrupted group separately to compare the owner image/face/pixel schedule
with STOP/resume.

Exit **0** and `B1_GROUP outcome=complete` mean both owners reached the bound
target and the native-readback merged PLY/completion binding was published.
`complete.json` binds the group manifest, owner checkpoints and merged PLY hash;
attempt directories retain owner outputs and scheduler records. Cooperative
incomplete exit **42** has no completion record; failure/refusal exit **2**
is not success. A foreign completion record must never be overwritten or
trusted as this invocation's result. Ordinary native `train` contract rejection
uses exit **1**. Do not treat merged PLY as globally resumable optimizer state.

The real kitchen step-10,000 four-update smoke on two AMD Radeon AI PRO R9700
devices completed to 10,004; STOP at owner steps 10,002/10,001 then native group
resume matched uninterrupted images/faces/pixels. Unowned packed state/bounds,
shared appearance and source-relative per-row clocks were exact. Owned decoded
parameters/moments/SH passed the reused `fpbo_split_parity` 1e-4 screen, **not**
byte identity; raw mixed-unit bound-screen outliers remain diagnostic.
Actual owner termination, duplicate-group/current-and-prior-owner writer
refusal, foreign completion publication refusal and explicit restart were
exercised. Wrong-owner/ordinary owner resume, config/horizon/view changes and
missing sidecars were rejected. Native merged render/eval and mesh-input
readback were exercised, not full meshing or a quality/performance gate.
See the [B2 behavioral evidence and limits](notes/multi-gpu-training-b1-execution.md#supported-device-behavioral-evidence-and-limits);
four-update owner recovery does not prove power-loss durability or arbitrary-long
exact floating trajectories. GPU smokes remain separate from the headless suite.

The [V0–V2 full-policy execution record](notes/multi-gpu-training-b2-view-policy-20261002.md#v0v2-execution-evidence-and-staged-admission)
adds an integrated dev Vulkan build, 17/17 headless checks, explicit AMD CLI 1/1,
backing/invalid-cap and FPBO split passes. Real full-policy four-update and
STOP/resume groups completed; each owner matched the bound all-owned reference's
images/faces/dimensions and 49,355,136 pixels, with exact source/frozen/clocks/
coverage oracles. Headless checks cover full 1,195-ID training coverage, held-out
exclusion, epoch/resumed sampler offsets, strict new versions and mixed merges.
Six native policy/manifest/environment/resume refusals preserved bound outputs.
Both policies require `--views`; new groups default to `partitioned`, while
`--view-policy full` selects all training indices without changing Gaussian masks.
Policy is bound in version-2 group/owner-checkpoint and worker payload schemas;
historical partitioned owners and all-owned references keep their semantics.
Resume and inherited-policy overrides are forbidden. These checks are structural
and sampler evidence, not a new numerical recovery or quality qualification.
Both fresh uninterrupted 10,000-update/owner P/F primary groups passed native
source/config/frozen/clocks/coverage/mask oracles. F's entire owner exposure
sequence equals retained all-owned N. Real mixed-policy `b1_merge` refused with
exit 1/no output; an existing-output sentinel was preserved (not coordinator
exit 2). Current native P/F/N evaluations all exited 0 on 132 named held-out
slots, showing partial full-policy gap reduction, not qualification.
See [matched metrics and identity limits](notes/multi-gpu-training-b2-view-policy-20261002.md#v4--matched-132-view-metrics).
Six bound native render runs exited 0; twelve full-resolution PNGs and two actual
contact sheets were reviewed for worst remaining slots 49/117, showing improvement
with residual error. Selected GT pixel equality is verified only for those two
views, not all 132. [V0–V5 is complete](notes/multi-gpu-training-b2-view-policy-20261002.md#v5--completed-disposition-and-next-decision);
native remains default/B2 experimental and Q/D failures are unchanged.

The [longer qualification](notes/multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results)
exercised later STOP/killed-owner recovery through step 20,000. Exact accepted
exposure replay, clocks and frozen/unowned state passed, but both owners failed
the unchanged decoded `1e-4` screen. Matched 132-view quality also regressed
against all-owned control. Successful publication is not numerical-equivalence
proof; the earlier four-update result does not qualify these longer histories.
External Vulkan query-process heap usage did not measure worker allocations.

### Experimental live context refresh (bounded A2 gates exercised; qualification pending)

The A2 cadence implementation is **experimental/unqualified**. Historical
17/17 headless checks and tiny two-AMD STOP/kill/resume workflow passed.
The resumed dev Vulkan build, retained-source native installation/refusal
proof and first source-scale group (16 updates/owner, cadence 8, source
10,000 → 10,016, horizon 30,000, two generations, exit 0/merged publication)
also passed; earlier integrated headless CTest passed 17/17 in 8.12 s.
The final current-build rerun after the owner-STOP repair passed **17/17 in
7.99 s with no exclusions**, bound by `A2-final-headless.json`/log.
The [continuation handoff](notes/multi-gpu-training-handoff-20261002.md#resumed-continuation-evidence--2026-10-02)
records exact current evidence and sampled resources alongside the historical
handoff. Both owners' 16 events exactly match each other and retained native
steps 10,000–10,015 in input/face/dimensions/pixels (197,712,576 pixels each).
This retained-prefix comparison is not contemporary timing/variability evidence
or full-horizon coverage proof.
Actual-source partial-install/transfer faults and resume, mixed-generation/pair
and no-replace refusals, independent owner-0 STOP before commit and owner-1 STOP
after commit, native eval and mesh-input consumers are now exercised.
The partial-install fault killed the exact attempt-owned process tree after one
owner's installation/local-commit acknowledgement while its peer was suspended,
before paired commit; it was **not** a native crash between install and local commit.
The one-byte-truncated transfer was refused, then resumed from original source.
Both STOP runs exited 42; owner-0's first resume exposed normal terminal `Stopped`
being mistaken for explicit STOP. `B1Group.cpp` now binds that decision to the
exact requested owner job; repaired owner-0 publication and fresh owner-1 resume
exited 0. All four final pairs have exact coherent render state, source-exact
unowned optimizer/shared state, exact clocks and complete 1,000,000-row stable-ID
coverage. Native eval scored 132 held-out views from original source (not treatment
quality); native mesh-input parsed the refreshed merged PLY (not full meshing).
New raw data was archived losslessly and native readback verified before removal;
retain `.compact.zip` files with their records and baseline/original-source dependencies.
Final application SHA-256 is
`c66ccb8088b46e3e08f592d6b08068fb0c9ca8942b07bd26f70783fc86708963`;
earlier consumer/headless records retain their own binary identities.
Cadence **1,000** is frozen before either A3 arm based on 21.3–21.94 s source
barriers: ten generations for source 10,000 → 20,000, horizon 30,000.
This is a predeclared diagnostic choice, not a winner; primary timing/quality
are unobserved. A3's fresh matched arms and A4–A6 remain required.
These bounded checks do not supersede historical B2/Q/D/V results, repair the
failed longer decoded `1e-4` recovery screen or establish a quality/performance
winner. Native remains the practical default.

On a supported non-NVIDIA Vulkan device, the explicit native proof command is:

```bat
build_vulkan\g1_native_proof.exe --b1-refresh-check uuid:YOUR_DEVICE_UUID
```

This proof uses 576 native rows (64-row codec tail and one inactive row), checks
exact installation of peer native render fields and SH codes/bounds, and checks
owned/non-render saved-state identity. It exercises stale boundary, changed hash,
wrong parent-commit and truncated-payload refusals. It uses proof-scoped ordered
raster; it does not prove the ordinary trainer's full-view sequence, two-worker
barrier, publication/rolling retention, long numerical recovery or quality.
The command is implemented in
[`g1_native_proof.cpp`](../src/backend/tests/engine/g1_native_proof.cpp); it is
not an automatically registered headless test.

For raw installation/refusal coverage on an original native optimizer source,
use a symbolic checkpoint path and canonical device selector:

```bat
build_vulkan\g1_native_proof.exe --b1-refresh-source-check --resume CHECKPOINT --device uuid:A
```

The retained source check passed with 1,000,000 rows, 3,907 codec blocks, a
64-row tail, 7,150 inactive rows (3,581 peer-inactive) and 1,195 training
indices, advancing only a synthetic boundary from 10,000 to 10,001 while
preserving the native 30,000 horizon. It checks exact peer-owned/non-render/
padding identity and full saved-state/committed-context preservation on
refusal. It performs **zero optimizer updates and no synthetic re-render**:
this is a raw installation discriminator, not trained-trajectory, quality,
long-recovery or primary cadence evidence. The tiny fixture still passed;
neither proof replaces actual group fault/event/consumer qualification.

Exercise the actual persistent two-worker path separately with a native full
source and its validated two-owner view manifest:

```bat
build_vulkan\spirula.exe train-blocks --resume SOURCE --views MANIFEST --devices uuid:A,uuid:B --output NEW_REFRESH_GROUP --updates 8 --view-policy full --refresh-cadence 2
build_vulkan\spirula.exe train-blocks --resume-group NEW_REFRESH_GROUP
```

Cadence must be a positive integer, no larger than 2,147,483,647, and divide
the per-owner `--updates`; explicit full policy is required. Boundaries are
relative to the original source step and retain its optimizer horizon.
Use two distinct supported non-NVIDIA Vulkan UUIDs and a new directory.
Omitting cadence preserves the existing frozen-context mode; cadence opts
into immutable version-3 group/owner contracts. Resume accepts no overrides.
Only peer render context is installed; owned optimizer state, moments/clocks,
topology and shared appearance stay authoritative.

For STOP/resume and killed-owner checks, compare with a separate uninterrupted
group. Unlike no-cadence independent latest-owner recovery, cadence resume must
select the **same published generation for both owners** using the validated
`refresh-commits` lineage and checkpoint/payload hashes. In-flight or separately
newer checkpoints are not recovery authority; uncommitted updates may be
discarded. Without a published pair both owners resume from the original source.
Check publication only after both installed checkpoints validate, exact peer
readback/owned-state identity, unchanged full-view image/face/pixel events,
tail/inactive-row coverage, stop/failure at each barrier phase, mixed/stale/
partial pair refusal and no-replace completion. Successful publication alone
does not prove floating trajectory equivalence; keep the existing decoded screen.
Merged PLY is never globally resumable optimizer state.

Rolling retention removes older committed checkpoint/payload pairs only after a
newer pair validates, retaining the source, latest pair, compact commit/install
lineage and scheduler evidence. Check those boundaries and budget concurrent
old/new pairs, payloads and native save staging. After validating recovery
authority, resume sweeps abandoned prior-attempt native staging, uncommitted
checkpoints and payloads inside scheduler-recorded owned attempts, refusing
symlinks/reparse paths/foreign hardlinks and retaining the checkpoint pair and
compact evidence. Existing device-backing admission still applies; fresh sampled host,
new-output and free-space admission is required, not inferred from retention.
See [source-backed usage and scope](notes/multi-gpu-training-b1-execution.md#opt-in-live-context-refresh-experimental-validation-pending)
and the [A0–A6 resource/qualification contract](notes/multi-gpu-training-plan.md#autonomous-execution-and-completion).
No NVIDIA/CUDA, remote-worker, GUI, quality or long-recovery support is added.

### Worker backing admission and bounded causal diagnostics

On a supported non-NVIDIA device, run:

```bat
set SS_VK_DEVICE=uuid:YOUR_DEVICE_UUID
build_vulkan\vk_runtime_smoke.exe --backing-cap
build_vulkan\vk_runtime_smoke.exe --invalid-backing-cap
```

The cap check exercises actual device/pinned/staging/params allocations,
pre-allocation refusal, concurrent reservations, overlap and free/reuse.
Invalid configuration must fail cleanly through shutdown. B1 workers bind at
most 24 GiB; direct training opts in with
`SS_WORKER_DEVICE_LIMIT_BYTES=25769803776`. The conservative reservation
high-water counts backing once; it is not a physical VRAM peak.
See the [runtime accounting contract](../src/backend/vulkan/README.md#memory-model).

`SS_B2_CAUSAL_STEP` enables the existing trainer's bounded diagnostic on a fresh
original-source marked-owner run (`SS_B1_OWNER=0` or `1`, source and validated
views bound through `SS_B1_SOURCE`/`SS_B1_VIEWS`). Ordinary runs leave it unset.
The diagnostic independently snapshots engine and sampler state before saving,
verifies each reset, and compares A/B/C1/C2 at immediate/1/8/64 updates:
three no-save arms and one native save/load arm. The full original horizon,
owner mask, unordered raster and shared appearance remain bound.

`SS_B2_CAUSAL_SELF_CHECK=1` additionally mutates real device state and a host
counter, requires both equality checks to reject them, and verifies independent
restoration. The actual-source step-10,001 smoke exercised that check and all
four 64-update arms. `engine_diagnostic_compare_test` covers immediate logical
changes, nonfinite codec bounds, post-update tolerance and padding classification.

Probe exit 42 and `diagnostic_complete.completed=true` mean the finite diagnostic
completed, not that numerical recovery passed or a group completed. Numerical
differences remain in `comparison.jsonl`; the saved checkpoint is the pre-save
boundary S. Synchronous native decoding and A's replayed timing inputs control
input/settings confounds; in-process reload does not prove all process-restart
state. The [causal execution record](notes/multi-gpu-training-b2-causal-20261001.md)
retains late-state results and their limits separately from the original Q failure.

The late original-owner step-15,040/15,002 probes completed all four arms with
exact immediate save/load and matching input/reset/clock/frozen invariants.
At 64 updates, save/load and both no-save repeats failed the unchanged screen;
earlier crossings were asymmetric. This demonstrates bounded native no-save
variability, not repaired long-Q recovery or a unique internal numerical cause.
Fresh all-owned source-to-20,000 validation committed 10,000 updates under the
original 30,000 horizon, and all three current-binary 132-view evaluations ran.
Equal-row B2 quality loss persists. The finite D0–D5 disposition is native
default / B2 experimental, with original Q failures and historical timings intact.

## SfM bundle-adjustment regressions

After the Vulkan build, run the CPU oracle and the dedicated production GPU
behavior test separately from the compute-free `headless` label:

```bash
./build_vulkan/sfm_ba_cpu_test
./build_vulkan/sfm_bounded_schur_test --device uuid:<supported-non-NVIDIA-UUID>
```

On Windows use the corresponding `.exe` files. The bounded-Schur test requires
an explicit physical, supported non-NVIDIA device with DF support; it refuses
software devices and CPU fallback. It exercises the actual `BundleSolver`
DenseObs path, not a replacement Schur implementation: more than 8,192
observations, partial final workgroups, camera/intrinsics/rig column tiers,
rejected-step restoration followed by acceptance, and cold/reused solver parity
against the CPU oracle.

`sfm_ba_cpu_test` checks every zero-rotation Jacobian column, including frame
and rig-member rotations; it does not skip the singular columns. CPU success
alone is not GPU coverage. Both checks are numerical/behavioral gates, not
full reconstruction, quality, performance or watchdog-causality qualification.
See [BA dispatch and precision policy](../src/sfm/ba/README.md).

## 1. Native numerical tests and historical cross-backend results

`src/backend/tests/*.cpp` — currently 20 tools covering projection (fwd, bwd,
quant-grad), rasterization bwd, tile intersect, warp, FPBO, optimizer (general
+ geometry), densify, per-pixel train, PPISP, bilagrid, multi-scale loss
(`mask_loss_semantics`, `reg_loss_underflow` and `fpbo_split_parity` are
self-checking rather than dump-then-compare: the first pins what an image mask
means in the loss, in both mask modes and with none; the second sweeps log
scales past every exp(scales) underflow threshold, down to -inf, and fails if
the per-splat regularizers hand the optimizer a NaN or push a splat below
kMinLogScale; the third steps FPBO and the non-fused optimizer path from the
same state and fails if they disagree, which is how the two update laws for
linear splat colour are held together),
meshing (activation, LBVH, occupancy/bisection/color, moment raster, the
per-camera samplers and the visibility cull), plus
`backend/tests/engine/` which drives the *real* engine end to end
(render parity, train parity, and two self-checking tools rather than
dump-then-compare: `engine_reset_state` trains the same scene twice across an
`engine_reset()` and the two must land in the same place, and
`gt_bilagrid_sentinels` pins the GT bilateral grids' invariants -- one depth
scalar per camera, and the no-GT sentinels passing through untouched).
`backend/vulkan/tests/` adds 3 Vulkan-only smoke tests (runtime, pipeline,
sort/scan).

`g1_native_proof` exercises the bounded kitchen-source FPBO input and
versioned owner proof. `--packet-check` is CPU-only; `--fpbo`, `--fpbo-two-owner`,
`--fpbo-edges`, `--fpbo-stop-check`, `--fpbo-replay` and `--fpbo-producer` require
explicit supported non-NVIDIA device UUIDs. Replay uses retained source-native
gradient/state packets, not a copied optimizer; the producer probe compares
render, pixel cotangents and raster screen gradients from frozen inputs.
The [G1 implementation plan](notes/multi-gpu-training-g1-implementation-20260929.md#build-evidence-and-completion)
lists source/config requirements and commands. Same-input native canonical
bytes remain exact acceptance. Proof-scoped ordered raster now passes Gates
1/2; timing/resource measurements are a separate gate, not these diagnostics.

For a finite production-unordered Gaussian ownership mechanism comparison:

```bat
set SS_B1_OWNER=all
set SS_B1_VIEW_POLICY=
set SSPLAT_B1_VIEW_POLICY=
set SS_B1_VIEWS=BOUND_NATIVE_VIEW_MANIFEST
build_vulkan\g1_native_proof.exe --native-mask-check --resume ORIGINAL_STEP10000_CHECKPOINT --device uuid:DEVICE --output NEW_DIRECTORY
```

Use the actual unmarked native FPBO source at step 10,000 with its unchanged
30,000-step horizon, dataset/config and bound full-view manifest on a supported
non-NVIDIA Vulkan device. Clear `SS_NO_TILE_SKIP` and `SS_B1_REFRESH_CADENCE`;
do not reuse the ordered/background/tile overrides of `--kitchen` or `--fpbo`.
The mode runs four real `TrainerSession::train_step` branches, eight events each
(32 native optimizer updates, plus 32 read-only native data-gradient captures):
two all-owned repeats, then genuine marked owners 0 and 1. Every branch restores
the same device/host/sampler prestate; native input payload hashes and replayed
native bin-policy timing bind the eight-event sequence. Source/contracts are
validated by the existing B1 setup/configure/bind APIs, not fabricated sidecars.
`SS_B1_VIEW_POLICY`/`SSPLAT_B1_VIEW_POLICY` overrides must be absent. Existing
`app::b1_worker_view_policy` resolves the genuine owner=all source to `full`;
the probe verifies this canonical resolution and explicitly binds the same full
training indices for marked arms through `engine_b1_bind_contract`. It does not
change or bypass the production prohibition on policy environment overrides.
The mode explicitly enables the existing synchronous diagnostic decoder before
DataManager setup; it retains the production seed, RNG and `next_train_step_spec`
schedule and does not require the unrelated `SS_B2_CAUSAL_STEP` trigger.
Every event checks common host/settings/sampler state against arm A. Cross-role
comparison exempts only `s.b1.owner`, `s.b1.frozen_hash`, `c.owner` and
`c.frozen_context`; each marked arm must retain its own starting role/context
identity and exact mask. Source/config/views/train-index hashes, topology, SH,
background/bin policy and generator state remain required equal.

All-owned references use a real owner=all contract to freeze shared updates and
topology. A diagnostic-only wrapper selects the ordinary unmasked Gaussian FPBO
launch, then restores its owner role and kernel mask; marked branches use their
real masks. This is **not ordinary whole-trainer equivalence**, a quality/speed
benchmark, long recovery qualification, or an attribution of A3/Q failures.
All arms use the same stage hooks and synchronous readbacks. Captured gradients
are never substituted into the update: every update recomputes its own native
internal VJP/regularizers/Adam/codecs.

At events 1 and 8, compare post-appearance RGB/depth/transmittance, post-native-hook
pixel cotangents, the current device weighted loss vector, the last native-scale
SSIM scalar, raster screen gradients, and full-row float data gradients before
regularizers. Loss display values are asynchronous, so the probe reads the
device loss reduction instead. Fused regularizer contributions are not separately
materialized. Existing diagnostic decoders report final native parameters,
moment codes/bounds/decoded moments, SH values/tail padding and exact clocks,
unowned and shared invariants. Numerical deltas are descriptive, with no new
tolerance; differing event-8 screen shapes are reported rather than falsely
compared. Native-repeat variability is reported alongside owner deltas, not
identified as a unique cause.

`comparison.jsonl`, four bounded initial host-state records, stage hashes and
stdout numeric deltas are retained; redirect stdout/stderr to retain the input
hash and stage-comparison log. No full-source array dumps/checkpoints are written.
Each snapshot is admitted at at most 512 MiB; the conservative snapshot workspace
projection is six times its reported size, plus a 2 GiB combined retained/current
stage-array cap and the native gradient buffers/device scratch. Output projection
is 64 MiB of metadata/first-mismatch details plus captured console logs and native
dataset disk-cache growth, not a measured physical peak. Parent admission must
monitor the existing 24 GiB worker backing, 12 GiB sampled host, new-output/free-disk
caps; cached images, driver/pinned memory and lazy native scratch are not covered
by these workspace estimates. Success and exceptions restore the exact baseline;
invariant failure retains evidence and exits nonzero. Build through the supported
Vulkan dev entrypoint before exercising this mode; it is not a permanent headless
wiring/default/source-wording test.

For the separately admitted ordinary-one-backward versus same-forward-primed
discriminator, use the same source/device/environment requirements above:

```bat
build_vulkan\g1_native_proof.exe --once-vs-prime-check --resume ORIGINAL_STEP10000_CHECKPOINT --device uuid:DEVICE --output NEW_DIRECTORY
```

The private mode runs genuine roles2(all)/0/1, reset endpoints1/8, five arms
`N_A`, `N_B`, `N_C`, `P`, `P_C`:135 native updates,54 extra prime leaf backwards,
54 auxiliary captures. `N_A/N_B` use one ordinary backward/Internal FPBO update;
`N_C` adds capture, `P` primes/restores four statistics before ordinary backward,
`P_C` adds capture to the primed arm. The diagnostic API's explicit optional-capture
boolean gates auxiliary observation only; every update still recomputes native
gradients. Original `--native-mask-check` four-arm32-update behavior and
`source_capture` are preserved; no reference-gradient substitution.

`once-vs-prime.jsonl` binds all-event forward/loss/pixel/screen/statistics/capture
stages,43 canonical endpoints (42 native runtime fields plus CPU implicit source
row-ID order, not GPU ID readback), all1M clocks/masks/tail, and exact initial/final
source reset including all28 physical TAR NPY payloads. At most the first complete
different-field pair is retained, up to360,000,000B; other fields retain full typed
hash/numerical/first-byte summaries. Total new-output reserve427,108,864B; no new
checkpoint/PLY serializer. Keep source/run/sealed declaration and retained pair
dependencies for the campaign's independent `A4-once-backward-verify.py` consumer.

Native exit0 means complete operational/reset evidence, **not numerical equivalence**.
Consumer0 also accepts a complete consistently negative numerical verdict;
`ordinary_repeat_exact`, `capture_equivalence_established`, and
`all_decoded_screens_pass` must be reported separately. The actual source run
completed native/consumer0 with all three flags **false**; ordinary first backward
already differed after exact forward stages, so capture/priming causality is not
isolated. Source reset was exact, not checkpoint/PLY consumer parity or a Q repair.
Unchanged decoded1e-4/nonfinite screens, Q long-recovery FAILED and native default
remain; no quality/recovery/speed qualification. Actual read-only private consumer
boundary smoke passed22 semantic cases with genuine binary hashing and no fake
scientific files; it is a validator-boundary check, not native numerical proof.

### Qualified-source PPISP microprice (experimental)

The private proof mode is positional `ppisp-price`, **not `--ppisp-price`**:

```bat
build_vulkan\g1_native_proof.exe ppisp-price --packet QUALIFIED_RETAINED_PACKET --device uuid:ADMITTED_AMD_UUID --output NEW_DIRECTORY
```

Use the same parent-sealed actual-source packet and a fresh output directory,
with parent-owned Job monitoring, current admission and the independent full
CPU consumer; native exit0 alone is not qualification. This mode accepts only
the two admitted AMD UUIDs and qualified1195-view/P9 packet. Keep the unchanged
24GiB worker backing/12GiB sampled host/8GiB root-output/10GiB free-floor gates.
It compares producer2's same three-axis common pipeline with nonnull joint
versus uniform runtime-null parameter-gradient selection. Eight warm pairs
precede32 alternating measured pairs,16/order; no configurable shortened matrix.
Timing is steady-clock launcher plus GPU sync, not GPU timestamps; first-warm
cold pipeline, parse/upload/readback, native process, monitor and independent
CPU times stay separate. Report **median paired joint/RGB ratios**, not ratio
of separate medians, and retain full RGB witnesses/input/packet hashes.

Actual both-AMD native/fullCPU acceptance and62 records/device are documented
in the [current master milestone](notes/multi-gpu-training-plan.md#current-ppisp-source-microprice--2026-10-03):
149,817,600B joint/RGB witnesses each match captured SHA, zero differing/
nonfinite elements, whole input/packet unchanged. It is one source-step10k/
horizon30k/B1/4080×3060 packet, **not statistical, full-wall training speed,
DCE, quality/recovery or release qualification**. This is distinct from the
historical six resident-owned exchange prices; native remains default,
Q decoded1e-4 remains FAILED, A4 ACTIVE/conditional families open/A5–A6 UNRUN.


The Windows-only `--fpbo-timing-smoke` checks the persistent native worker
protocol and checkpoint/failure boundary; it is not a shortened benchmark.
Without `--updates`, G1 smoke retains two measured pairs/four total updates.
`--fpbo-timing-smoke --updates 3` uses the same explicit finite schedule as
`--fpbo-timing --updates 3`: one warmup exposure, three measured G1 pairs/six
exposures, and one final exposure (five updates/eight primary events, IDs 0..7).
Only the value `3` is accepted once; it is mutually exclusive with
`--measured-exposures`. The existing source-native gradient/internal-external
and owner-publication exact comparisons, missing/stale-owner refusal,
failed-child refusal and native checkpoint readback gates remain enabled.
This diagnostic is not a benchmark or a claim that mapped divergence is repaired.
`--fpbo-timing` defaults to 1,200 measured exposures and rejects profiler
presence. `--measured-exposures N` selects a positive even bounded count,
retaining 101 warmup exposures and one final exposure; smoke does not accept
the option. Both modes require `--resume`, explicit `--devices`, a new
`--output` directory and `--method g1|control`. `--raster ordered` is the
numerical default; only full control timing accepts `--raster resident` for
the native speed comparator. G1 owner replies attest actual frozen-state
bytes instead of returning them; final native checkpoint replies remain full.
Run resource smoke separately with `SS_PROFILE=1`; do not substitute its
diagnostic wall for unprofiled benchmark repeats.

### Finite Windows mapped-packet discriminator (implemented, not qualified)

`--transport file|mapped` selects only the existing G1 packet staging;
file remains the default. Mapped uses two parent-created, attempt/owner-named
pagefile mappings before child launch, one event-acknowledged slot per worker.
The same serializer, header, field lengths and identity validator serve both
routes. `--packet-hash fnv|sha256` defaults to FNV-1a; SHA256 requires Windows
CNG, uses one process-local reusable hash object and stores only its first eight
digest bytes, little-endian, in `Header.hash`. This is **64-bit packet integrity**,
not full SHA256 scientific identity. Both algorithms hash each 8-byte field
length followed by its bytes; source/config identity and frozen-owner
attestation remain unchanged FNV. Unknown/duplicate selectors, non-Windows
SHA256 and state/hash selectors on ordinary native controls are refused.
There is no fallback. `.job` files and events still carry commands, not tensors.
Packet version/kind flags, worker launch, commands and phase/result records bind
both immutable selectors; mixed modes are rejected.

`--state-transport full|resident-owned` defaults to the old full-state path.
The experimental resident-owned path keeps one full CPU native source cache per
worker. Op 1 transfers only the peer's world fields 0–6 ownership slices and
uploads only those peer slices, then renders with owner=-1. Capture still
restores the full local cache afterward. Op 2 transfers all six dense reduced
Gaussian gradients, executes unchanged native masked FPBO math, and still
reads the **full** native GPU state back. Before projection it compares every
unowned core byte and frozen statistics/shared field against the local cache;
the reply contains owned fields 0–19 plus the unchanged frozen attestation.
Parent validates each exact slice length using the same padded-block range
helper as the old merge, and publishes canonical state only after both accepted
owners. Op 3 still receives/restores all 42 canonical fields and saves/returns
the ordinary full native checkpoint; final stable IDs remain field 43.
This reduces logical wire bytes, not all native H2D/D2H, and is not zero-copy.
Resident mapping capacity is the larger full op-3 or gradient-reply frame
(including smoke render/loss evidence), bounded by 1 GiB; the render maximum is
derived from every declared schedule image's retained dataset/post-split size
metadata, not the startup image. Fields retain the 256 MiB and 96-field limits.
Logical counters separate actual smoke render/loss payload from gradients and
record the capacity source, maximum render bytes/image, and per-image loss bound.
Native clocks, source IDs, horizon 30000,
learning rates, codec layout and ownership are unchanged.

Parent checks for this A4 bounded variant (implemented, **not yet qualified**):

```bat
build_vulkan\g1_native_proof.exe --packet-check --packet-hash fnv
build_vulkan\g1_native_proof.exe --packet-check --packet-hash sha256
build_vulkan\g1_native_proof.exe --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_RESIDENT_FNV --method g1 --transport mapped --updates 3 --state-transport resident-owned --packet-hash fnv
build_vulkan\g1_native_proof.exe --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_RESIDENT_SHA --method g1 --transport mapped --updates 3 --state-transport resident-owned --packet-hash sha256
```

Use fresh resource admission/output directories and supported AMD devices,
one run at a time. Replace `mapped` with `file` to exercise file persistence.
Resident smoke additionally returns actual render and loss bytes and checks
exact render, all six gradients, reduced gradients and merged native state
against the existing full-canonical same-input oracle each pair. Loss uses the
existing native-source comparison (atol 0.0003, rtol 0.003), without relaxation.
Its nonempty op-1 reply has nine fields: six gradients, render, loss, identity;
empty replies remain one identity and benchmark replies remain seven fields.
Existing missing-owner, malformed
packet, child-failure publication and poisoned native checkpoint consumer gates
remain active. No tolerance is weakened for stale peer optimizer state; an
oracle mismatch is a failed variant, not permission to bypass the check.
`--updates 3` still means five G1 updates/eight primary exposure events;
default smoke without that option retains its previous work unit.

Parent validation commands (do not run concurrently with compilation):

```bat
build_vulkan\g1_native_proof.exe --packet-check
build_vulkan\g1_native_proof.exe --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_SMOKE_FILE --method g1 --transport file
build_vulkan\g1_native_proof.exe --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_SMOKE_MAPPED --method g1 --transport mapped
build_vulkan\g1_native_proof.exe --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_FINITE_SMOKE_FILE --method g1 --transport file --updates 3
build_vulkan\g1_native_proof.exe --fpbo-timing-smoke --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_FINITE_SMOKE_MAPPED --method g1 --transport mapped --updates 3
```

The CPU packet check now exercises real Windows mappings, roundtrip,
unacknowledged reuse, stale/future header, damaged hash, empty/truncated/
oversized length, writer capacity overflow, oversized mapping and name
collision refusal. The source smoke retains actual native capture/reduction,
masked FPBO updates, compact owner/frozen attestations, merge, full native
checkpoint poison/reload readback and failed-child publication refusal.
Missing-file owner withholding is file-only; mapped empty-slot refusal is
covered by the packet check. No checks or timing qualification are implied
by documenting these commands.

Predeclare the following four arms before running; `CHECKPOINT`, `uuid:A/B`
must resolve to the campaign's bound unmarked step-10000 source and two
supported AMD devices, never to newly generated substitute inputs. Clear
`SS_PROFILE` for measurements. Keep source/config/views, native horizon
30000 and ownership unchanged. `--updates 3` selects the fixed finite work
unit: one warmup exposure, three measured paired work units (six exposures),
and one final exposure: IDs 0..7 in that order, real-source faces/pixels.
Each G1 run does five updates (pairs 1+2, 3+4, 5+6, half-weight gradients);
each resident control does eight sequential native updates. These are equal
primary exposures, **not equal optimizer trajectories or end clocks**.
All arms begin at the identical native world/moments/bounds/clocks.

```bat
build_vulkan\g1_native_proof.exe --fpbo-timing --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_FILE --method g1 --raster ordered --transport file --updates 3
build_vulkan\g1_native_proof.exe --fpbo-timing --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_MAPPED --method g1 --raster ordered --transport mapped --updates 3
build_vulkan\g1_native_proof.exe --fpbo-timing --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_ORDERED --method control --raster ordered --updates 3
build_vulkan\g1_native_proof.exe --fpbo-timing --resume CHECKPOINT --devices uuid:A,uuid:B --output NEW_NATIVE --method control --raster resident --updates 3
```

Run three fresh-process repeats, unique directories every time, alternating
order: file/ordered/mapped/native; mapped/native/file/ordered;
ordered/file/native/mapped. Compare per-arm three-run descriptive medians
and individual complete walls, never just measured regions. Keep the exact
eight primary image/face/pixel rows aligned; separately price startup priming,
worker contexts and smoke oracle forwards. The existing result/worker-phase
ledgers price source setup, capture/restore, packet transfer+validation,
reduction, update, synchronization, merge, checkpoint readback and shutdown.
Worker phases overlap parent barriers: do not add them to complete wall.
`parent_packet_hash_seconds`, `parent_map_copy_seconds`, and each child's
`workerN.transport.tsv` split hash and mapped memcpy as subsets of packet
phases, not extra wall. Read-validation still includes field allocation.
Logical map byte counters are not physical disk bandwidth measurements.
Worker `workerN.phases.tsv` reports decimal `read_payload_hash` and
`write_payload_hash`, followed by `packet_hash`, `state_transport` and
`loss_return_payload_bytes`: successful packet-body integrity fingerprints
over field lengths and bytes, excluding the header, using the named algorithm
(FNV-1a or SHA256 truncated to 64 bits), plus the actual loss payload accounting.
The op-2 reply body's final 8-byte frozen-owner attestation binds the attempt,
so its raw output fingerprint can legitimately differ across attempts.
No extra payload read, hash pass or allocation is added.
The input fingerprint is captured immediately after receive; the output
fingerprint follows the reply. Priming op 4 has no packet and records `0/0`;
failed packet validation does not publish a successful phase row. Named-column
TSV readers may ignore the appended columns; the native positional reader
consumes the fingerprints and validates both selectors.

These fingerprints are diagnostic, not SHA scientific-artifact identity,
evaluator evidence or long-Q recovery proof. With otherwise matched stage
conditions, identical input fingerprints with differing gradient-output
fingerprints point to capture. Identical op-2 input fingerprints with differing
native owner-output bytes point to native update, but **raw op-2 output hashes
must not support cross-attempt native-variation claims**: for FNV only, normalize
the known attempt-bound attestation suffix, or compare actual native field/slice
bytes. **Never apply FNV suffix normalization to SHA256-64.** Both 64-bit packet
integrity modes can collide and do not attest every numerical semantic or
transient backend state.
The source cause remains unresolved; this instrumentation does not establish
deterministic mapped R3 behavior or a repair.

Admission is conservative sampled parent-plus-child RSS below 12 GiB,
counting shared mapped pages twice; unknown physical peaks remain unknown.
The exact mapping reserve is `2*(capacity+16)` bytes, separately reported
from native device backing. Worker 24 GiB actual backing and 1 GiB heap
headroom remain campaign requirements. Parent admission is **per sequential
run**, including 64 MiB metadata, completed-state artifact and native endpoint
checkpoints (historical two-owner endpoints about 2*380 MB, not an exact
current projection). Refresh actual source-sized output projection, existing
new-live archives/dependencies/temp, within the campaign 8 GiB cap and leave
10 GiB free. Never retain all twelve large endpoints concurrently. Every run
must finish poison/reload byte checks, then retain metadata and endpoint hashes;
if endpoint payload retention is needed, parent losslessly archives and
verifies it before the next admission. Mapping reserve/logical bytes are
distinct from output disk bytes; local output cap is not global admission.
Retain no full per-event packet dumps.

Unrun gates: compiled integration, CPU mapping refusal, both actual-source
smokes, twelve finite runs, source packet/gradient/update/readback equivalence
between file and mapped, and admission versus both ordered and ordinary
best-native complete cost. This ordered-path transport experiment cannot
establish unordered native byte parity, repair Q recovery, establish long-run
quality, or finish A4's remaining justified candidate families after one
negative arm.


The one-device O1 residency screen is diagnostic instrumentation, not offload
training or a benchmark:

```bat
build_vulkan\g1_native_proof.exe --o1-screen --resume CHECKPOINT --device uuid:DEVICE --output NEW_DIRECTORY
```

Use the retained native source/config and a new output directory on a supported
non-NVIDIA Vulkan device. It fixes views 1188, 1165 and 760, runs three independent
source updates plus an ordered three-update sequence, discards six same-input
native internal/external canonical-byte oracle replays, and verifies exact
poisoned native checkpoint reload. Original row IDs, nonfinite rows, native
codec padding/tail and source-relative per-row clocks remain intact.
`fields.tsv`, `phases.tsv`, `summary.tsv` and `final.native` retain evidence;
phase retained capacity/epoch requests are not semantic-live peaks.
The [completed O0–O3 record and retained protocol](notes/multi-gpu-training-o1-screen-20260930.md#completed-o0o3-execution-record)
bind resource admission, field inventory and exclusions. O2 was not admitted
because bounded discovery/native chunk update and partial servicing are
unestablished; diagnostic wall does not substitute for performance repeats.

**Historical reference-generation workflow — not an active validation requirement.**
The same source can still build under both backends, but CUDA/NVIDIA execution
is deprecated. The following recipe documents existing reference provenance;
do not run its CUDA step or regenerate NVIDIA references under the current policy.

```bash
# on the CUDA machine
./build_cuda/projection_parity dump ref.bin
# on the target machine / device
./build_vulkan/projection_parity compare ref.bin
```

Inputs are deterministic, and comparison is tolerance-based — fast-math
exp/sqrt chains legitimately differ across compilers, and borderline-cull
flips change whole rows, so a small allowance for those is built in.

Two tests carry a **relative-RMS gate** alongside the per-element one, for the
same reason: they contain discrete per-pixel or per-splat decisions that flip
wherever an architecture's rounding differs from the reference's, so a handful
of large outliers is expected while the vector as a whole must still agree.
`msloss_parity`'s NMS / quantile / clip modes put an RX 7800 XT at 0.21% of
tight elements out of tolerance (max_abs 4.19) against 0% on NVIDIA Vulkan --
but 3.8e-4 relative RMS against 1.8e-7. A permuted or biased reference sits
orders of magnitude above that, which is what the RMS gate is there to catch.

`engine_train_parity` has two gates rather than one, because per-element
agreement is not something any implementation can hold across 12 optimizer
steps. The threshold-crossing kernels -- median depth, masked-tile skip, the
rasterize-bwd survivor batching -- flip a handful of pixels per step wherever
an architecture's rounding differs from the reference's, and Adam turns a
flipped gradient sign into a full-size parameter step, so the trajectories
separate. Measured against a CUDA reference (2026-08-25): NVIDIA Vulkan lands
0.003% of elements out of tolerance at 4.8e-7 relative RMS, while an RX 7800 XT
lands 4.4% at 1.4e-4 -- identically on amdvlk and RADV, and unchanged by
`RADV_PERFTEST=wave32`, so it is not a wave-size effect. The divergence starts
in `depth_loss` and `normal_loss` (discrete median-depth selection); `rgb_loss`,
`ssim` and `psnr` stay at 1e-7. An indexing or layout break, by contrast, puts
30%+ of elements out of tolerance at a relative RMS above 1. The RMS gate is
what keeps the test sharp; the element gate is loose enough to absorb the drift.

`msloss_parity` splits its reference into two channels. **Tight**: per-pixel
gradients (deterministic given the raw-loss sums, which enter them only through
smooth reduce math), the densification loss map in every mode, equal-shape
`v_ref_depth` / `v_ref_normal` scatters (one atomic per cell), and the quantile
outputs. **Loose**: `LossValues`, the SSIM display scalar, and scaled-GT
scatters, which accumulate atomically in a backend-specific order. Each config
runs twice and compares the second return, because the scalars come back
through a one-iteration-behind async readout on both backends. One expected
mismatch survives: the CUDA SSIM scalar sums over TILE-GRID positions, so an
image whose dims are not a multiple of the tile picks up zero-padded
out-of-image contributions that differ between the 24- and 16-wide tiles
(`ssim_cs` cfg). It is display-only; gradients and loss values are unaffected.

Several tools also take a `*_DUMP_GOT` environment variable
(`FPBO_DUMP_GOT`, `PPISP_DUMP_GOT`, `MSLOSS_DUMP_GOT`, `PWTRAIN_DUMP_GOT`,
`BILAGRID_DUMP_GOT`, `DENSIFY_DUMP_GOT`) to write the *actual* values
alongside the reference, which is how you diff a mismatch numerically instead
of guessing.

### Building them

```bash
# Active Vulkan tests are built unconditionally.
bash build_develop.bash -DSS_BACKEND=vulkan
```

Each `.cpp` becomes an executable of the same base name in `build_vulkan/`
(`build/` on macOS).

### Historical cross-machine / cross-vendor runs

The following transfer procedure describes how existing CUDA references were
produced. It is historical, not a request to run NVIDIA validation:

1. Transfer a matching `slangc` to the target and point `-DSS_SLANGC=` at
   it — SPIR-V is compiled at build time and never committed, so the target
   needs a compiler, and the version is pinned.
2. Dump references on the CUDA host.
3. Copy the `.bin` files over and run `compare` on the target.

Keep reference dumps out of git (`parity_refs/` is gitignored).

### macOS / MoltenVK

All 17 parity tools pass against a CUDA reference on Apple silicon, at
essentially the Linux numbers (`engine_render_parity`'s blit channel: 0.157%
of bytes on macOS against 0.152% on Linux, cap 0.2%).

`engine_render_parity` used to fail here on that channel, and the failure was
worth more than its number: the viewer's grid and frustum lines came out
*fragmented on macOS only*. It was not antialiasing, which is what this
section claimed for a while. `vis_blit`'s BVH descent read the popped node
inside a two-iteration child loop, and SPIRV-Cross re-materialized that
threadgroup read once per child instead of keeping it — so the second child
was read after the first child's push had overwritten the slot, and the
descent walked into the wrong subtree. The `[ForceUnroll]` on both child
loops is what keeps the pop a value; see the first MoltenVK rule in
`src/backend/vulkan/README.md`.

Three tools -- `msloss_parity`, `optimgeo_parity`, `meshing_parity` -- pass
only because `VulkanContext::init()` turns MoltenVK's default Metal fast-math
off. `SS_VK_FAST_MATH=1` puts it back, and they fail again; that is the knob
to reach for when measuring what the setting costs.

Speed is a separate question from parity, and macOS answers it differently. Two
kernel choices that cost nothing elsewhere cost an order of magnitude here, and
both are measured at run time rather than assumed: the GEMM tiling
(`OpGemm.cpp`, `SS_NN_GEMM_KERNEL` pins it) and the matcher's dot product
(`integerDotProduct4x8BitPackedUnsignedAccelerated`, `SS_SFM_NO_DOT4` pins it).
With those, an M2 runs SAM 3 image encoding at 12x an RTX 5070 (7.4 s vs 0.64 s,
was 140x) and brute-force matching at 22x (43 vs 1.9 ms per 8192x8192 pair, was
64x); the matcher's remainder is DP4A, which Apple has no instruction for. The
third is "slangc `[unroll]`" in `src/backend/vulkan/README.md`.

## 2. GUI / viewer checks

The web viewer can be driven headlessly over the Chrome DevTools Protocol.
Headless defaults to SwiftShader; to exercise a real GPU, run against a real
display (`DISPLAY=:0`).

A scripted run that serves the viewer needs **`--keep-viewer-alive 0`**, or
the process hangs at exit waiting on it.

### Desktop smoke checklist

Use a newly created scratch directory with isolated `APPDATA`/`LOCALAPPDATA`
(Windows) or `HOME`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME` (POSIX), and disable
unattended fetching. Do not point the smoke at the user's queue or private data.
Use the generated six-view scene from a retained integration fixture.

1. **D1 — graphics lifecycle:** launch the exact built `spirula` with the dataset
   directory as its argument. Observe a rendered frame with six camera frustums
   and the seed points. Close normally and verify exit code zero.
2. **D2 — actual input and scheduling:** through the real Batch form, choose that
   dataset, a short training configuration, explicit job GPU and scratch output.
   Submit; observe queue state and appended bottom-pane logs. Cancel an active
   attempt, verify its truthful stopped/interrupted state and no remaining child,
   then close normally. A first-frame screenshot does not satisfy this step.
3. Device-control changes additionally need a preview on A while a queued job
   targets B, with actual preview continuity—not merely the target label.

Capture the visible transition and process/state evidence. Canvas input that the
automation tool reports as delivered but that produces no visible change is not
successful interaction. Web-viewer CDP checks do not replace these desktop gates,
and the older scheduling/handoff acceptance backlog remains separate.

## 3. Deterministic application contracts

`dataset_parser_test` generates equivalent COLMAP text/binary, Nerfstudio and
Metashape scenes and checks canonical frame ordering, camera poses, intrinsics,
pinhole distortion, seed coordinates and train-frame scaling. Interval and
fraction train/eval splits must be disjoint and cover the complete source set;
both retain the same normalization. This is native coverage, not a replacement
for every historical lens/configuration golden.

`step_config_test` calls the shared training implementation around regularization,
supervision, median and distortion warmups, LR endpoints and an absolute resumed
midpoint. It also checks non-unit scene scaling and scale-agnostic mean updates.

`batch_process_test` checks preset/override resolution, frozen queued options,
independent foreground reservations, preflight errors and cancellation.
`scheduler_result_test` uses a test-only child executable to inject wrong
identities, malformed results and missing outputs; none may advance the workflow.
Real worker preparation remains covered by `scheduler_test`, while
`cli_training_smoke` covers real training. The protocol fixture is not a fake
successful compute workload. Rapid scheduler shutdown/pause cycles guard the
condition-variable lost-wake regression found by concurrent headless runs.

## What to run before calling a change done

| change | gate |
|---|---|
| any kernel | Vulkan build + relevant behavioral/numerical check on supported non-NVIDIA hardware |
| engine logic | Vulkan build + relevant real-engine workload on supported non-NVIDIA hardware |
| SfM BA math, shader flags or dispatch ownership | `sfm_ba_cpu_test`, then `sfm_bounded_schur_test --device uuid:<supported-non-NVIDIA-UUID>` on the fresh DF shaders; full application build and affected reconstruction gates remain separate |
| config field | add the row in `src/config/TrainConfig.h`; check `spirula train --help` and the GUI's All Options editor |
| training-loop logic | `step_config_test`, then `cli_training_smoke` on supported non-NVIDIA Vulkan hardware |
| build system | affected supported Vulkan modes in [build.md](build.md) |
| a comment you wrote | `python3 tools/check_comment_length.py` — the build runs it anyway ([lints](build.md#lints)) |
| `SS_FILE` or `SS_SOURCE_ROOT` | `source_path` on each supported host toolchain |
| a mesh format, or which colors it carries | `mesh_format_roundtrip` — writes every format and reads it back through the other implementation |
| a preset field, or a batch row's shape | `preset_roundtrip_test` + `batch_process_test`; actual form wiring also needs D2 |
| what a typed-in command line becomes, or what a message may carry into it | `command_argv_test` — the message stays one argument and stays JSON-safe |
| a per-cell optimizer launcher (Vulkan) | `SS_OPTIM_SLICE_CELLS=2048` on `optim_parity` / `optimgeo_parity`, which forces the multi-slice path only an SH buffer past ~24M splats would otherwise take ([SH layouts](notes/sh-quant-layout.md)) |
| scheduler / worker protocol | `ctest -L worker`; supported Vulkan train lifecycle when affected |
| desktop rendering or input | D1 + the affected D2 interaction, on a real display |
| ordinary host-only change | focused headless behavior test, then the headless label |

## Profiling

`SS_PROFILE=1` enables the env-gated per-stage timing breakdown
(H2D / D2H / D2D / memset / device / host). Header-only, works on both
backends — the right first tool when a backend is unexpectedly slow rather
than wrong.

Above that table both backends print **GPU time by kernel**, so the two are
directly comparable without a profiler. Vulkan brackets each dispatch with
timestamp queries; CUDA does the same with a CUDA event pair, injected by
`-Wl,--wrap=cudaLaunchKernel` (`backend/cuda/KernelProfilerCuda.cu`) so no
launch site is instrumented by hand and CUB's kernels are covered too. Rows
aggregate over template arguments / specialization constants, which is what
makes a CUDA row and a Vulkan row the same thing.

Two caveats on reading those numbers against each other. The intervals
include the gap before each kernel starts, so their sum runs a little over
the device-wait total. And a training run is **not** reproducible: atomic
order moves the trajectory, and the rasterization and sort kernels then see a
different scene — `rasterize_fwd` has been seen to move 70% between two runs
of the same binary. The image-sized kernels (losses, bilagrid, PPISP, FPBO)
hold to ~1%, so they can be A/B'd from a training run directly; for the rest
use the benchmark tools, which fix the workload:

```bash
./build_vulkan/raster_bench [num_splats] [iters] [macro_log2]   # raster fwd/bwd, binning
./build_vulkan/fpbo_bench   [num_splats] [iters]                # fused projection bwd + optimizer
```

A run that trains also prints a VRAM breakdown after the timing table: pool
capacity per `VramCategory` (`src/core/PoolSlots.h`), the scratch buffer, the
driver's process figure, and the twelve largest buffers. The pool never
shrinks, so those are training peaks, not the numbers at exit. Both front
ends emit it — the CLI at the end of the process, the GUI when its window
closes.
