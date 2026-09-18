# Device job scheduling: reconciled upstream continuation plan

**Status:** primary reconciled plan as of 2026-09-18. This repository document is
the source of truth for continuation. The external legacy plan and handoff are
historical evidence, not commands to rerun.

## 1. Precedence and non-negotiable contracts

This plan supersedes old instructions to restart a pending merge, require all
nine workflow rows through the desktop, or forbid every local commit until GUI
acceptance. Programmatic checks of real production behavior are first-class
acceptance for contracts observable at the CLI, worker, scheduler, parser,
persistence, checkpoint, or application seam. A real desktop remains required
for actual UI wiring, rendering, visible state, and input. A mock protocol child,
synthetic device identity, copied request field, or successful process exit does
not prove real compute.

The user paused deeper GPU and desktop debugging. This documentation task does
not reopen that work or declare it passed. Broader scheduler throughput,
fault-window, parent-death, and multi-device work is not automatically complete
because the headless suite is green. Historical reports, screenshots, old CI
claims, and old command transcripts are evidence and limitations, not requests
to rerun completed work.

The scheduler contracts remain fixed:

- `Prep → SfM → optional geometry → optional linked training → publish` is the
  only scheduled workflow; the single-phase submission API is a compatibility
  wrapper around it, not a second executor.
- Shared preparation and process execution remain in `src/app/`; GUI code owns
  presentation and editing, not a parallel preparation implementation or
  foreground scheduler.
- Desktop binding, the default target for new jobs, and each queued phase target
  are separate. Queued work may be retargeted by canonical identity; running
  and completed attempts are immutable.
- External COLMAP, Python masking, and meshing remain explicitly interactive;
  they are not silently added as scheduler phases.
- Acceptance uses Vulkan on supported non-NVIDIA hardware and CPU host checks.
  No CUDA/NVIDIA implementation, testing, compatibility, or optimization lane
  is authorized, including Vulkan running on NVIDIA.
- Keep `SS_ENABLE_PATENTED=OFF` for acceptance and ordinary ffmpeg fallback for
  video. Patent-enabled CI is not proof of the fallback-disabled path.

Use these status terms: **complete** means current evidence covers the stated
contract; **partial** means named surfaces remain open; **blocked** means a
prerequisite or paused investigation prevents proof; **historical** means old
candidate evidence; **diagnostic** means useful but excluded from acceptance.
Blocked and deferred are not passed.

## 2. Reconciled snapshot (not remote-live)

No fetch or live remote verification was performed during reconciliation. The
verified record was:

| Item | Value |
|---|---|
| Integration branch | `integrate/device-job-scheduling-upstream-d579cf8c755f` |
| Starting `HEAD` | `6f5ce221af358710a2356a768248139bdf35d160` |
| Starting status | clean |
| Starting stash list | empty |
| Starting merge state | no `MERGE_HEAD`; no unmerged index entries |
| Existing merge | `4847da4e5831012fa6b3fefe533806e31632408d` |
| Existing merge parents | `9fc6dde5a641d4cdd4d94d408c3023f87ebc59e7`, then `d579cf8c755f475d43eebaf7e2f0874eb633e595` |
| Original feature tip | `2850f729d1da98e8fb90449d54f9f6d5bee8aebd` |
| Cached upstream tip | `d579cf8c755f475d43eebaf7e2f0874eb633e595` |
| Backup ref | `backup/pre-upstream-device-job-scheduling-2850f729d1da` |

`9fc6dde5` is the preserved ValidationSkill commit based on original feature
`2850f729d1da98e8fb90449d54f9f6d5bee8aebd`; `6f5ce221` finalizes headless
work. Both the original feature and selected upstream are verified ancestors of
the existing merge. Local feature, cached origin feature, and backup refs point
at `2850f729d1da98e8fb90449d54f9f6d5bee8aebd`; cached upstream points at
`d579cf8c755f475d43eebaf7e2f0874eb633e595`.

The parent relationship is structurally valid but is not an ownership approval.
The first parent includes formerly protected user-owned ValidationSkill work,
and `.clangd` is now tracked user-owned tooling configuration. Treat both as
preserved additions outside the original merge baseline. Do not infer owner
intent merely because they are committed. Before fast-forward/push, obtain
recorded explicit approval to include both paths, or use a separately approved
corrective follow-up commit; never rewrite, drop, reset, or silently relocate
them.

After this document lands, capture the actual continuation candidate anew:
branch, full status, stash list, current `HEAD`, merge state, and relevant ref
OIDs. The table above is not a promise that `HEAD` stays unchanged.

## 3. Completed/source ledger

These items are already implemented or verified in the reconciled history. Do
not redo them merely to recreate old evidence:

- Conflict resolution and shared single-executor prep/process integration are
  present. Shared app preparation/process paths serve worker and foreground
  callers; do not reintroduce a GUI-local runner.
- `adaptive_fps` and `adaptive_range` round-trip through `WorkerRequest`, with
  accepted range `[1,16]`.
- The queued-only GUI picker resolves canonical UUID then calls `set_device`;
  running/completed jobs remain immutable; labels include `[index]` to
  distinguish identical model names.
- Shared `app/TextFile.h` is used by worker and foreground SfM and preserves
  Windows hidden attributes. A throwaway hidden-manifest worker smoke reached a
  deliberately invalid SfM flag while retaining `A H`; it did not prove full
  update or SfM success.
- CTest registration, the isolated runner, CI wiring, `batch_process_test`,
  `scheduler_result_test`, parser/step-config tests, `SceneFixture`,
  `cli_training_smoke`, and incomplete-newer-sibling checkpoint coverage exist.
- Scheduler dispatcher shutdown/pause lost-wake handling and its 200-cycle
  regression are implemented.

The original mechanical, code-generation, build, and source-policy results are
historical execution-record evidence, not new runs for this document.
`scheduler_result_test` uses a test-only protocol child, not a successful GPU
worker. Synthetic `gpu-*` identities prove routing only.

## 4. Evidence and command discipline

### 4.1 Recorded headless evidence

Recorded Windows evidence is GUI-enabled Vulkan **19/19 in 7.63 s**; two
concurrent invocations passed in **8.65 s** and **7.54 s**; GUI-OFF core was
**14/14 in 6.65 s**, after which GUI-enabled configuration was restored. Empty
selection and an intentional runner failure returned nonzero and retained
failure artifacts. Linux/macOS CI execution is unobserved; configuration is not
execution proof. Configured CI jobs run headless and retain `Testing`, but the
workflow triggers push to `master` and pull request, not a feature push alone.

The GUI-enabled headless suite needs no display or GPU. GUI-OFF omits batch,
preset, argv, and stamp coverage. Do not call the result 20 tests: the separate
`gpu` scenario did not pass. The 19/19 and 14/14 results do not certify a full
model pipeline, SfM, geometry, masks, notification, new background modes,
PPISP-consumer arithmetic, or physical multi-GPU execution.

See [headless testing plan](headless-testing-plan.md),
[deferred GPU/desktop handoff](headless-validation-debugging.md), and
[testing instructions](../testing.md) for the detailed boundaries.

### 4.2 Current build and CTest commands

Use the build directory selected by the current script and an explicit label or
focused regex. The ordinary Windows command is:

```bat
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error -j 2
```

Focused examples are:

```bat
cmake -E chdir build_vulkan ctest -L fast --output-on-failure --no-tests=error
cmake -E chdir build_vulkan ctest -L worker --output-on-failure --no-tests=error
cmake -E chdir build_vulkan ctest -R scheduler_result_test --output-on-failure --no-tests=error
```

Never use bare `ctest` when avoiding GPU work. Keep `--no-tests=error`; an
empty selection fails. On Linux use the bash build entry point with equivalent
Vulkan/GUI/SfM/SAM/patented-disabled options; on macOS use its `build`
directory. Do not add a custom `-B`: Windows `build_develop.bat` now targets
`build_vulkan` and its later dependency-log/build steps use that directory.

The acceptance build is:

```bat
build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_GUI=ON -DSS_BUILD_SFM=ON -DSS_BUILD_SAM=ON -DSS_ENABLE_PATENTED=OFF
```

For a source-changing candidate retain this explicit final list:

```bat
python tools\codegen\generate_headers.py
python tools\codegen\generate_kernel_instantiation.py
python tools\codegen\generate_backend_api.py
bash tools/check_ss_prefix.sh
bash tools/check_i18n.sh
bash tools/check_file_macro.sh
bash tools/check_comments.sh
bash tools/check_private_paths.sh
python tools\check_font_coverage.py
python tools\check_comment_length.py
```

The script's optional codegen is not proof of idempotence; inspect generated
drift explicitly and repeat generators against the reviewed index. This
 documentation-only wave does not rerun the list.

## 5. R1-R9 acceptance matrix

The nine original rows remain stable crossreferences. Each row distinguishes
programmatic production evidence from UI-only evidence and keeps an honest
status.

### R1 — Image/video/adaptive/ffmpeg/previews

**Status: partial.** Worker/request serialization, range validation, headless
CPU preparation boundaries, and ordinary ffmpeg fallback are covered. Earlier
photo/video/mask GUI observations are diagnostic where the device/candidate was
excluded; the hidden-manifest invalid-flag smoke is not full SfM.

Programmatic continuation: verify real image/video artifacts, selected frames,
adaptive settings, ffmpeg fallback, preview data, and mask/depth/normals inputs
through production readers; a request field or label is not extraction/model
proof. UI continuation: exercise the real image-folder/video flow, responsive
preview, correct mask colors, and actual input transitions.

### R2 — Existing masks/depth/normals/features/reuse/rerun/presets/preflight

**Status: partial.** Host tests cover request/preflight/preset serialization,
ownership, and result validation. The shared writer smoke retained `A H` and
reached the deliberately invalid SfM option; a manual `attrib -H` workaround
was diagnostic only and cannot satisfy this row.

Programmatic continuation: verify masks, depth, normals, counts, existing
feature/model reuse, explicit rerun controls, persisted outputs, and immutable
request/preflight agreement without clearing the hidden manifest condition. UI
continuation: fresh hidden-manifest update, feature retention, reuse/rerun,
preset save/load, and plan/preflight summary must agree with execution.

### R3 — Native scheduled workflow and single-phase wrapper

**Status: partial.** Scheduler, request/result, lease, persistence, and lost-wake
checks cover real boundaries and identity/output handling; a protocol child is
not model-compute success.

Programmatic continuation: exercise real `prep → SfM → [geometry] → [linked
train] → publish`, checking artifacts, identities, output paths, ordered
transitions, child target identity, and no duplicate foreground executor. Also
exercise the train-only single-phase wrapper through the same scheduler. UI
continuation: Batch UI submission, visible queue/log/phase transitions, result,
and linked output require the actual desktop surface.

### R4 — Queued A→B targeting and ownership

**Status: blocked for two-device proof; partial for routing/UI.** Canonical UUID
resolution, queued-only picker wiring, `set_device`, index-disambiguated labels,
and scheduler retarget regression prove routing semantics; synthetic identities
do not prove compute.

Programmatic continuation: with two eligible physical non-NVIDIA GPUs, prove
queued A→B retarget, next worker on B, running worker and desktop on A,
default changes not rewriting pending targets, and identity in request/result/
state. UI continuation: picker, labels, and unchanged desktop context must be
visible. Only one eligible non-NVIDIA physical GPU is known, so this row is
blocked; do not substitute NVIDIA or an index.

### R5 — Metric/partial/nonmetric/failure SfM outcomes

**Status: partial and GPU-blocked.** Parser, manifest, rig, live-match, and
scheduler result tests cover deterministic/host boundaries, not real model
quality or successful GPU SfM. AMD failure and excluded-device GUI runs are
failure/identity evidence, not success.

Programmatic continuation: separately check real manifest/live-match, rig/device,
and actual metric success, preserved partial, preserved nonmetric, and genuine
failure with correct phase/error/artifacts. UI continuation: show each outcome
and artifact truthfully. Never turn failure into success to preserve a path.

### R6 — Training stop/save and non-training force-stop

**Status: partial; supported-device lifecycle blocked.** Process, leases,
checkpoint-reader, and incomplete-sibling tests cover boundaries. The AMD
`cli_training_smoke` execution failed before valid numerical acceptance;
readable checkpoints or lifecycle-only evidence are insufficient.

Programmatic continuation: on a verified supported non-NVIDIA UUID, prove real
finite training, cooperative stop-and-save, and a valid resumable checkpoint.
For prep/SfM/geometry, force-stop must reap the whole process tree before
termination and lease release, with truthful stopped/interrupted state. UI
continuation: only training offers Stop and save; non-training controls say
force-stop; progress, terminal state, and close must be real.

### R7 — Recovery, atomic state, staging, resume, Later/Retry, second owner

**Status: partial; actual Later/Retry UI remains open.** Atomic scheduler/checkpoint
state, completed-prefix handling, resume validation, incomplete-newer-sibling
rejection, leases, and fail-closed second-owner checks have host coverage.

Programmatic continuation: isolated interrupted workflow must preserve atomic
state, completed prefix, source/workspace/options/device, restart the interrupted
phase, retain the last valid checkpoint, resume from a run directory, and reject
a second owner without duplicate writers. UI continuation: real restart/recovery
must show Later preserving the pending flag and Retry/recover creating an
explicit attempt; completed phases must not appear forgotten.

### R8 — Final-publish notification and safe failure

**Status: blocked/not covered.** Scheduler final-result routing is intended, but
there is no existing headless notification coverage and no claim in 19/19.
`CommandRunner`/`GuiApp::run_batch_command`/`poll_batch_command` remain the
targeted path for review; historical notification reports are not accepted.

Programmatic continuation: add the smallest production-seam smoke/regression
only if a plausible missing behavior warrants permanence; otherwise use a
throwaway run. Prove notification only after final publish, safe failure is
separate, and a completed artifact stays completed and uncorrupted. UI
continuation: visible completion/failure separation after the whole workflow.

### R9 — Interactive-only external COLMAP/Python masking/mesh

**Status: partial.** Request validation and preflight reject external COLMAP and
Python masking from the native queue; mesh remains explicit. Host rejection is
not visible-label/input proof.

Programmatic continuation: verify actionable scheduler rejection and retained
interactive paths, with no hidden mesh phase or revived foreground executor.
UI continuation: actual menus/labels/clicks must keep these actions explicitly
interactive and out of the native queue.

## 6. Focused native gates

### 6.1 Background modes, PPISP, and SfM

Exercise every newly retained background mode through actual rendering **and
training**, including pseudorandom mode. A parity name or render-only harness
is not engine-level proof. Exercise arithmetic-mean PPISP exposure centering
through the actual consumer with an independent expected value; historical
`trainer_exposure_test` evidence counts only where hardware/provenance is
compatible. Keep SfM manifest/live-match, rig/device, and real outcomes as
separate checks. A host-only geometry check, protocol child, copied UUID, or
exit zero does not prove inference.

Prefer existing production CLI/worker/app seams. Do not add a GUI-driving API,
second scheduler/framework, or parallel implementation merely to script a gate.
Permanent tests require a plausible missing observable regression; otherwise use
a disposable smoke and remove it after evidence capture.

### 6.2 Parity invocation and references

Parity tools require `dump|compare <reference.bin>`; no-argument usage is not a
pass:

```bat
build_vulkan\engine_train_parity.exe dump <reference.bin>
build_vulkan\engine_train_parity.exe compare <reference.bin>
build_vulkan\engine_render_parity.exe dump <reference.bin>
build_vulkan\engine_render_parity.exe compare <reference.bin>
build_vulkan\ppisp_parity.exe dump <reference.bin>
build_vulkan\ppisp_parity.exe compare <reference.bin>
```

Use protected compatible supported-device references or an independent numerical
oracle. Candidate dump versus itself is not regression parity; older NVIDIA
references are unacceptable. If claiming masked-tile-skip equivalence, pair the
comparison with `SS_NO_TILE_SKIP=1` and restore the old environment value. If
inputs/layout/contracts changed, establish a defensible compatible reference or
oracle rather than overwriting a golden.

## 7. Blocker ledger and dedicated later GPU work

Only one eligible non-NVIDIA physical GPU is known, so real two-device proof is
blocked but headless review is not. No NVIDIA/CUDA implementation/testing,
including Vulkan-on-NVIDIA, is authorized. The AMD byte-conversion path is the
first suspect before the later driver pipeline crash; no GPU patch is
integrated. Disabled native-int8/atomics/int64 variants may exit zero but show
PPISP `inf`/invalid training and are not accepted workarounds.

When the user explicitly resumes, follow the detailed [debugging handoff](headless-validation-debugging.md)
instead of duplicating its large reproduction. First isolate production
`uint8_image_to_float_raw` with bytes `0,127,128,255` across workgroups,
comparing every scaled float to an independent CPU expectation with default
native bytes and `SS_VK_NATIVE_INT8=0` on an explicit verified UUID. Then
separately investigate PPISP inputs, reduction, ABI, and readback. Do not begin
from the later crash or globally disable capabilities.

Only after finite numerically valid training, rerun the real lifecycle with an
explicit UUID (never an index):

```bat
set SS_TEST_DEVICE=uuid:<verified-non-NVIDIA-UUID>
cmake -E chdir build_vulkan ctest -L gpu --output-on-failure --no-tests=error
```

Lifecycle alone, readable checkpoints, or zero exit is insufficient numerical
proof. Preserve diagnostic artifacts under `build_vulkan/Testing/Artifacts` as
indexed by the handoff before cleanup.

D1 first-frame/normal-close evidence exists but was not AMD-specific. D2
submit/cancel remains unverified. Tool-reported input success or hover is not
widget activation. Retain focused UI gaps for queue picker, bottom logs,
preview matching/inverted masks, busy-training completion, and recovery
controls. Do not add production automation solely to bypass input failure.

## 8. Ownership and hygiene

The historical ValidationSkill dirty file is represented by the preserved
`9fc6dde5` parent, and `.clangd` is tracked user-owned tooling configuration
(`CompilationDatabase: build_vulkan`). Neither is missing WIP or disposable
fixture output; `.clangd` is separate from the five generated batch-fixture
files below and must never be lumped into their cleanup. Preserve both and
require explicit inclusion approval (or a separately approved corrective
follow-up) before feature ff/push; a commit does not imply owner consent.

Five tracked batch-fixture runtime outputs from the old direct test were committed
in `4847da4e`: `batch-fixture/dataset/transforms.json`,
`batch-fixture/queue/job-state.json`, `batch-fixture/queue/job-state.lock`, and
the two `batch-fixture/output/queue-job` runtime artifacts including
`.spirula-output.lock`. Flag them for deliberate source-hygiene follow-up:
confirm no live owner, preserve useful evidence, and remove only in a normal
follow-up commit. This plan removes nothing; it is not untracked cleanup. Never
use `git clean` or broad `git add`.

Before future writing waves, record branch/status/stashes. Main owns dirty paths
and shared integration; isolated lanes are disjoint and begin from a clean
available baseline. Main runs one shared integration check after lanes settle.
Do not speculate about a transient CMake race fix; stable configured build plus
the lost-wake fix are current.

## 9. Next runnable work and publication preparation

This documentation task authorizes documentation only: no commit, push, GPU
debugging, or desktop interaction. When work is explicitly resumed:

1. Capture actual post-document branch/status/stashes, `HEAD`, merge state, and
   refs; do not replay equal-tip/start-merge commands or abort the committed
   candidate.
2. Review candidate/source/acceptance and obtain explicit disposition for
   ValidationSkill and `.clangd`; inventory the five tracked runtime outputs.
3. Run focused existing CTest host checks (`-R`, then `-L fast`/`-L worker`, then
   full `headless` as appropriate) against changed behavior. Do not require
   desktop unlock for host work; do not claim model compute from `--help`,
   synthetic IDs, protocol children, or copied result files.
4. Run only applicable focused native gates: background rendering/training,
   independent PPISP expectation, real SfM/geometry outcomes, finite supported
   training, and compatible-reference parity.
5. On resumed desktop work, exercise only outstanding R1-R9 UI surfaces and
   require a second eligible non-NVIDIA physical GPU for R4. Isolate profile and
   sacrificial fixtures; close/reap processes and resolve state locks.
6. Make ordinary scoped follow-up commits only after their checks. Keep deferred
   behavior explicitly deferred; it is not a pass.

Dedicated GPU, multi-GPU, notification, and UI gates block complete
integration/publication claims, not continued host review or delivery of
headless improvements.

## 10. Publication safety

Publication is a later owner-approved operation. Before publication, perform a
fresh explicit refetch:

```bat
git fetch --multiple --prune origin upstream
git rev-parse feature/device-job-scheduling
git rev-parse origin/feature/device-job-scheduling
git rev-parse upstream/master
```

The local feature and cached published feature must still equal original
`2850f729d1da98e8fb90449d54f9f6d5bee8aebd`. If either moved, preserve local and
published tips under distinct backup refs and stop for explicit reconciliation;
do not auto-pull, reset, rebase, prefer the remote, or merge a new feature tip.

If freshly fetched upstream differs from selected `d579cf8c...`, verify normal
advancement, not rewrite/rewind. If not already an ancestor, pin that actual
new tip and append a normal `--no-ff --no-commit` merge using the current
candidate and new upstream parents. Resolve, review, build, run applicable
headless/native/UI gates, and record actual parents. Re-fetch again after any
additional integration; the final candidate must contain the latest upstream
tip observed at the successful gate. Never pretend future pushes are known.

Before ff/push require every applicable behavior gate to pass or an explicit
user scope change naming the deferred gates, plus explicit ValidationSkill and
`.clangd` inclusion approval, reviewed index/artifact hygiene, source-changing
codegen checks where applicable, current exact-candidate evidence, no live
owned jobs/processes, no unexpected stashes, no unresolved merge, and no
unaccounted dirty paths. Do not imply current readiness.

Only then:

```bat
git switch feature/device-job-scheduling
git merge --ff-only <integration-branch>
git push origin feature/device-job-scheduling:feature/device-job-scheduling
git ls-remote --heads origin refs/heads/feature/device-job-scheduling
git status --short --branch
git stash list
```

Require the advertised remote OID to equal the accepted candidate. Never
force-push or merge the feature into `master` to trigger CI. Feature push alone
does not trigger the configured workflow; verify an applicable PR run or record
CI pending. Keep backup/integration refs. Master refresh is a separate approved
task with its own baseline and gates.

Abort guidance applies only to a future genuinely pending merge: preserve
resolutions/evidence, then use `git merge --abort` and verify status/stashes.
Never abort, reset, or rewrite the current committed merge candidate.

## 11. Relative references and completion boundary

- [Headless behavior testing plan](headless-testing-plan.md)
- [Deferred GPU and desktop debugging handoff](headless-validation-debugging.md)
- [Explicit-device scheduling plan](device-job-scheduling-plan.md)
- [Testing commands and coverage boundaries](../testing.md)

This plan is complete as a continuation document because it preserves R1-R9,
separates production-programmatic from UI-only proof, identifies complete/
partial/blocked status, records ownership/publication safeguards, and does not
claim paused GPU, desktop, notification, throughput, fault-window, or
multi-GPU work has passed. It is not a completion certificate.
