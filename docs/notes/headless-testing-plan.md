# Headless behavior testing and desktop smoke

Status: **Headless implementation verified locally; deeper GPU and desktop interaction validation deferred by user.**

The NVIDIA lanes in this original plan are superseded by the current
[backend support policy](../../AGENTS.md#backend-support-policy). NVIDIA-specific
implementation, investigation and validation have stopped; no further CUDA or
NVIDIA-Vulkan checks are required. Existing legacy code remains untouched.

Observed locally on Windows:
- GUI-enabled Vulkan headless suite: 19/19 passed in 7.63 s; two concurrent
  invocations passed in 8.65 s and 7.54 s.
- GUI-OFF core suite: 14/14 passed in 6.65 s; GUI-enabled build restored.
- Empty selection and deliberate runner failure returned nonzero; failure logs
  and owned artifacts were retained. CI execution on Linux/macOS remains unobserved.
- The supported AMD Vulkan training attempt crashed in `amdvlk64.dll`; GPU
  acceptance remains open. Earlier NVIDIA results do not satisfy the new policy.
- Desktop first frame rendered the generated scene and normal close exited zero.
  Submit/cancel remains unverified: background and foreground canvas input did
  not activate the controls. Prior scheduling desktop backlog remains open.
- GUI-OFF-to-ON rebuild took 72.73 s, separate from test runtime; this was not a
  clean-build measurement.

Investigation stopped at the user's request before applying a GPU fix.
See [the debugging handoff](headless-validation-debugging.md) for observed
failure order, diagnostic variants, retained artifacts and bounded next steps.
Deferred validation is not a pass and does not block delivery of the headless
workflow improvements under the revised scope.

## 1. Goal and release boundary

Make the usual verification loop one build followed by one terminal command,
without opening the desktop application. Exercise the production behavior, not a
second implementation of it. Keep a short, real desktop check for presentation,
input wiring and graphics lifecycle.

This specification covers:

- Discoverable, bounded execution of existing native tests through CTest.
- Deterministic tests of batch submission, worker contracts and scheduling.
- Native replacements for the parser and step-configuration coverage gaps.
- One small, real GPU training/checkpoint/resume integration scenario.
- A repeatable desktop lifecycle and submit/cancel smoke checklist.
- CI execution, failure evidence and commands documented in `docs/testing.md`.

It does not introduce a GUI-driving CLI, a daemon, a new test framework, a Python
runtime dependency, a second scheduler, a second configuration parser, automatic
model downloads, or a universal end-to-end suite for every inference model.
Kernel changes require relevant numerical checks on supported non-NVIDIA Vulkan
hardware. A passing headless suite does not certify layout, live previews or
real-GPU rendering.

### Paused-work boundary

The planning baseline is the working tree on
`integrate/device-job-scheduling-upstream-d579cf8c755f`, HEAD `9fc6dde5`, including
its existing staged and unstaged integration work. HEAD alone does not describe
that baseline. This document is the only planned write in the planning session.

Do not start implementation by resetting, stashing, committing or finishing the
paused work. Before implementation, recheck the branch, index, worktree and stash
list; reconcile the current source with this plan. Existing dirty paths remain
owned by the paused work and cannot be assigned to isolated writing lanes.
Implementation touching those paths waits for an explicit ownership handoff or
integration of that work. Independent new-file work may proceed only when its
source dependencies are available in its isolated checkout.

The outstanding desktop checks in [the handoff](../../HANDOFF.md) and the
[device scheduling plan](device-job-scheduling-plan.md) are not waived, replaced
or marked complete by this plan. Their historical branch names and implementation
tables are not a reason to overwrite the current integration tree.

## 2. Existing seams and actual gaps

| Surface | Current source | Consequence |
|---|---|---|
| Batch inputs | `src/app/gui/BatchProcess.h`: `batch_check_row`, `batch_build_dataset_job`, `batch_build_train_config`, `batch_config_args` | Test these functions; do not extract a parallel request builder from the GUI. |
| Orchestration | `src/app/JobScheduler.h` and `src/app/Subprocess.h` | Drive queueing, reservations, stop and recovery without ImGui. |
| Worker execution | `src/app/cli/worker_main.cpp`; `spirula worker --request <file>` | This is the existing process boundary for prepared requests. |
| Result protocol | `src/app/WorkerRequest.h`: `Request`, `Result`, `parse_result`, `publish_result` | Reuse atomic result JSON and scheduler validation, not localized stdout. |
| Training | `src/app/TrainerCore.h`: `TrainerSession`, `build_step_config` | CLI and GUI already share the training implementation. |
| Existing host checks | `src/core/tests/`, `src/mesh/tests/`, `src/app/gui/tests/` | Extend useful checks instead of recreating them under another runner. |
| Existing scheduler checks | `src/core/tests/scheduler_test.cpp` | Real worker dispatch and tiny RGBA prep exist; reconstruction/training deliberately stop before GPU work. This is not successful-training coverage. |
| Test discovery | `CMakeLists.txt`, `cmake/SsApps.cmake` and backend/subsystem build modules | Executables exist, but CTest registration does not. Do not register every `main()` blindly: some require references, assets or GPUs. |
| CI | `.github/workflows/ci.yml` | Build jobs currently run `spirula train --help`, not the behavioral test binaries. Add actual execution. |
| Documented coverage debt | `docs/testing.md`, section 3 | Dataset-parser equivalence/splits and `build_step_config` boundaries lack their native replacement. |
| Desktop state | `src/app/gui/GuiApp.cpp`, `GuiMain.cpp` | Actual controls, state presentation and rendering still need a desktop check. No general GUI test protocol exists. |

Headless means no window, display or GPU initialization for the `headless` test
label. It does not mean the executable is independent of backend libraries, or
that building it is free. Current core tests link the engine. Build time and test
runtime must be measured separately.

## 3. Test execution contract

### 3.1 Runner and labels

Use CTest, already supplied with CMake. Call `enable_testing()` at the top level
and include a small `cmake/SsTests.cmake` after application targets are defined.
That module explicitly registers selected runnable targets and their labels,
timeouts and dependencies. Keep existing source lists and executable ownership in
their current build modules. No new test DSL or automatic source-name classifier.

| Label | Required environment | Contents |
|---|---|---|
| `headless;fast` | CPU, writable isolated scratch | Parser/configuration, batch preflight and artifact/codec invariants. No device initialization. |
| `headless;worker` | Same, plus the matching built `spirula` executable | Real subprocess and scheduler/worker integration without GPU work. |
| `gpu` | Explicit compatible hardware and the matching backend build | The new small real training/checkpoint/resume scenario. Serial execution. |

Do not assign `headless` to tests that fall back to CPU after a failed GPU probe,
need downloaded weights, require ungenerated fixtures, or merely return success
when prerequisites are missing. Do not label a GUI-adjacent data test `gui` just
because its source lives under `gui/`.

Initial registration uses a reviewed allowlist: `source_path`,
`split_faces_test`, `frustum_size_test`, `bilagrid_selector_test`,
`frame_motion_test`, `mesh_format_roundtrip`, `delaunay_degenerate`,
`worker_request_test`, `checkpoint_resolution_test`, `scheduler_test` and
`subprocess_test`. In the normal GUI-enabled build also register
`command_argv_test`, `preset_roundtrip_test`, `recon_stamp_test` and
`frames_stamp_test`; they do not open a window. New tests join the same labels.

Keep the existing GUI build guards. Batch/preset headers currently reach GL
headers through the runner/FilmReel includes; removing that coupling is not
necessary to run their tests without a window. The complete suite uses the
normal GUI-enabled build. A GUI-OFF build runs the explicitly documented core
subset, not full batch/preset coverage. Configure output and test listings must
make that difference visible. A future GL-free header refactor requires its own
scope and paused-path ownership handoff; it is not part of this implementation.

Required targets must not silently disappear from the promised baseline suite.
Guard genuinely optional subsystem targets with `if(TARGET ...)`; do not use that
guard to hide a missing required target. A label selecting zero tests is an error.
The initial suite does not register dump/compare parity tools or weight-dependent
model tests; retain their existing documented invocation and prerequisite rules.

The documented routine invocation always selects a label. Bare `ctest` is not a
promise to avoid GPUs once GPU tests have been registered.

### 3.2 Isolation, bounded lifetime and evidence

- Give each invocation a newly created scratch root. Never use a private dataset,
  the source tree, user presets, the existing job queue or a real output folder.
- Before any application-path lookup or background thread starts, isolate
  `APPDATA` and `LOCALAPPDATA` on Windows, and `HOME`, `XDG_CONFIG_HOME` and
  `XDG_CACHE_HOME` on POSIX, including macOS. These are the paths `AppPaths.cpp`
  reads today. Use child-local environment overrides for child processes.
- Use the exact built executable, not a PATH lookup or an old binary in another
  build directory. CTest supplies its target-resolved path to integration drivers.
- Disable unattended fetching with the existing `SS_NO_AUTO_FETCH` policy where
  applicable; the initial fixtures require neither models nor network access.
- Each driver has bounded waits and cancels/reaps its own process trees before
  reporting a failure. CTest's outer timeout is a backstop, not the subprocess
  cleanup implementation. Start with 60 seconds per fast test, 300 per worker
  test and 600 for the small GPU integration test.
- Preserve requests, results, captured diagnostics and relevant failed artifacts
  under the scratch root on failure; print its location. Remove owned temporary
  inputs on success. Never delete a caller-supplied dataset or unrelated process.
- Retain CTest's `Testing/Temporary/LastTest.log` in CI. Tests must identify the
  failed invariant, not just print a final failure count. Upload retained test
  artifacts only from the job-owned scratch/output location, not user caches.
- Missing prerequisites, missing binaries, timeout and a requested GPU scenario
  that did not execute are failures, not a green `SKIP` message. An unavailable
  hardware lane is reported as not run, never as accepted.
- Do not retry failures into success or add sleeps to hide races. Use observable
  state/artifact transitions with deadlines. Investigate the scheduler timeout
  noted in the handoff if it recurs; do not disable its platform coverage.

Use a small shared test-only scratch/fixture helper only where multiple tests
need the same code. No production abstraction is required for test convenience.
Parallel headless execution is allowed only after tests prove isolated. GPU
integration tests share a CTest resource lock and run one at a time; this is not a
system-wide lock against an unrelated desktop trainer.

### 3.3 Stable assertions

For a worker attempt, check the OS process outcome and parsed `app::worker::Result`
together: schema, `job_id`, `attempt_id`, `phase`, outcome, exit code and usable
outputs. The scheduler remains responsible for rejecting mismatched/stale
results. A result file alone is not proof of success; exit zero alone is not
proof of published work. For malformed requests or forced termination, a missing
result can be correct, but must not be mistaken for a successful phase.

Validate output contents through the existing production readers, including
checkpoint validation/resume. Check that failure does not advance the workflow,
lose the last good checkpoint or overwrite a competing owner's output.

Do not grep translated log messages or pin exact wording. Logs are diagnostics.
Do not compare GPU model bytes or an exact stochastic loss trajectory. Use finite
metrics, meaningful step advancement, readable artifacts and established numeric
tolerances. A device UUID copied into a request proves routing, not independently
that a runtime initialized that device; keep those claims distinct.

## 4. Required behavioral coverage

| ID | Scenario and observable contract | Preferred owner |
|---|---|---|
| H1 | Batch preflight rejects unsupported scheduled work and conflicting outputs before launch. Resolved presets/overrides survive serialization; editing the source row after submission does not change the submitted work. Distinct future-job targets do not alter desktop reservations. | New `src/app/gui/tests/batch_process_test.cpp`, using existing BatchProcess and scheduler APIs; retain existing preset tests. |
| H2 | Real RGBA preparation publishes usable masks. Explicit no-mask overrides beat discovered masks; queued retargeting changes remaining phases without rewriting completed/running attempts. Existing tests covering these cases are retained, not duplicated. | `src/core/tests/scheduler_test.cpp`, `worker_request_test.cpp`. |
| H3 | Current-attempt results are accepted; wrong job/attempt/phase, malformed or incomplete results cannot publish success or release dependent work. Cancellation/reaping and reload preserve truthful terminal/interrupted states and exclusive output ownership. | Existing scheduler, worker-request and subprocess tests. Extend only missing cases. |
| H4 | Deterministic equivalent COLMAP text, COLMAP binary, Nerfstudio and Metashape fixtures describe the same canonical scene. Validate frame identities, poses, intrinsics, distortion, seed points and applicable per-frame scalars; train/eval are disjoint and their union is the source frame set for a disjoint split mode. | New native parser test using `src/data/parsers/` production APIs. |
| H5 | `build_step_config` produces intended learning-rate/loss/feature transitions immediately before, at and after configured warmup/decay/activation boundaries, including start/end and resumed-step behavior. | New native step-config test calling the real function. |
| G1 | A tiny generated dataset trains through the real CLI, writes a valid full checkpoint, then resumes through the scheduler/worker path and advances beyond the saved step. Request/result identities and published outputs agree. Stop-and-save produces a resumable checkpoint; force-stop never reports successful completion or publishes a partial checkpoint as valid. | New `src/app/tests/cli_training_smoke.cpp`, linked to existing subprocess/scheduler/checkpoint APIs. |
| D1 | A real desktop launch creates its graphics context, reaches a visibly rendered frame, accepts normal close and exits cleanly. | Manual/tool-assisted checklist, not a headless rendering claim. |
| D2 | A real form submits the intended small job, queue state/logs update, cancel reaches a truthful state, and the desktop remains responsive through close. | Manual/tool-assisted interaction with the actual desktop application. |

H4 uses generated temporary files, not an installed dataset or a download. Start
with six cameras and small images, using exactly representable pinhole/OPENCV
intrinsics rather than iterative lens fitting. Call `parse_dataset` through
`src/data/DatasetParser.h` with explicit `DatasetParserConfig`; use a disjoint
interval/fraction split. Reuse the synthetic-scene patterns in
`engine_train_parity.cpp` and temporary-image pattern in `trainer_exposure_test.cpp`,
not a new fixture framework. Expectations come from the canonical fixture
definition, not the parser output. Account for format coordinate conventions and
COLMAP text's required second POINTS2D line. Add malformed-input checks only for
plausible boundaries not already protected by retained tests.

H5 pins behavior, not every copied config field. Call `build_step_config` and
`build_loss_weights` with explicit config/run state. Cover `reg_warmup_length`,
`supervision_warmup`, `median_warmup`, `distortion_reg_warmup`, the configured
learning-rate endpoints, and non-unit `RunState.train_frame_scale`. Use analytical
expectations immediately before/at/after each boundary, including a resumed
absolute step, rather than a large golden or a second schedule implementation.

G1 starts with six 64x64 generated views, a small splat cap, a short completed
training run, and a resume target beyond its saved step. Disable viewer/keep-alive,
enable full checkpoints, set explicit output paths and device, and use a real
evaluation split if asserting final metrics. No learned geometry/masking models
are needed. For stop cases, use a short `--steps-per-save` interval and a much
higher finite iteration limit; wait with a deadline until the production
checkpoint reader accepts a newly published step before requesting stop. Fail if
the worker finishes before the requested stop transition is exercised. The
existing checkpoint artifact is the synchronization signal: no arbitrary sleep
or new production observation/GUI scripting hook.

The force-stop case checks interruption after a known published checkpoint and
reopens that checkpoint afterwards. It does not claim to hit the write/rename
fault window deterministically. Extend `checkpoint_resolution_test.cpp` with an
incomplete newer staging/checkpoint sibling and assert that the production
resolver rejects it and retains the last valid resume candidate. Together these
cover truthful termination and reader safety, not crash consistency at every
publication instruction. Deterministic mid-publication fault injection belongs
to the existing checkpoint/scheduler fault-window acceptance, outside this small
smoke; no timing-based kill is accepted as evidence for that separate claim.

The baseline GPU check targets supported non-NVIDIA Vulkan hardware and is
currently deferred. CUDA/NVIDIA checks are excluded by the backend policy.
Vulkan scheduler/device acceptance remains governed by the scheduling plan.
Multi-GPU concurrency, cross-vendor numerical parity and SfM/model quality are
separate acceptance workloads, not claims made by G1.

## 5. Intended developer workflow

These CTest commands are **proposed**, not available until Phase 1 is implemented.
Build with the existing entry points; no build-time Python requirement is added.
Do not reconfigure the paused development tree as part of writing this plan.

```bat
build_develop.bat -DSS_BACKEND=vulkan
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error
```

```bash
bash build_develop.bash -DSS_BACKEND=vulkan
cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error
```

On macOS use `build` instead of `build_vulkan`. `-DSS_BUILD_GUI=OFF` remains
supported for the core subset, but does not provide batch/preset coverage. The
normal build above supplies the complete suite without launching a GUI. Keep
`SS_ENABLE_PATENTED` at its default OFF; patent-enabled paths are separate checks.

After a build, developers can run the `fast` or `worker` label, or select a focused
test with `-R`. Explicit hardware validation uses `-L gpu`, with target selection
supplied to the integration driver through the existing backend-specific
selection environment. The driver must reject an unspecified or unavailable
explicit target for the hardware acceptance run. Its commands and platform
selection syntax are documented with Phase 4, not guessed by a test wrapper.

`cmake -E chdir ... ctest` deliberately works with the project's CMake 3.18
minimum; it does not rely on newer `ctest --test-dir` syntax. The
[CTest 3.18 manual](https://cmake.org/cmake/help/v3.18/manual/ctest.1.html)
documents label filtering and `--no-tests=error`.

Target feedback budget after compilation: the fast label in tens of seconds and
the complete headless label within two minutes on a normal development machine.
These are initial engineering targets, not measured claims or timing assertions.
Record cold/incremental build time separately from test runtime before optimizing.

## 6. Phased implementation plan

### Phase 0 — Reconcile ownership and freeze the baseline

- Re-read the active handoff, current build wiring and the affected source.
- Record branch/status/stashes. Resolve ownership of every pre-existing dirty path
  with the paused work before assigning edits; do not assume isolation contains
  another session's uncommitted changes.
- Inventory the selected existing cases and their actual input/hardware needs.
  Preserve useful coverage and identify additions against H1–H5, not test counts.

Acceptance: a file-ownership map and commands refer to the reconciled source;
paused work is accounted for, with no accidental reset, stash or implementation.

### Phase 1 — Make existing headless checks easy to run

Ownership: `CMakeLists.txt`, new `cmake/SsTests.cmake`,
`cmake/SsApps.cmake`, `.github/workflows/ci.yml`, `docs/testing.md`;
`src/core/tests/scheduler_test.cpp` for executable-path injection;
`src/mesh/tests/mesh_format_roundtrip.cpp` and
`src/app/gui/tests/{preset_roundtrip_test,recon_stamp_test}.cpp` for invocation
isolation; other allowlisted test entry points only if the same bounded safety
audit finds shared scratch or an executable fallback.

- Add explicit CTest registration, labels, timeouts and exact executable paths.
- Before enabling CI or parallel use, replace fixed scratch directories in
  `mesh_format_roundtrip`, `preset_roundtrip_test` and `recon_stamp_test` with
  exclusively created per-invocation roots. Apply config/cache isolation before
  their first application call. Check the remaining allowlist for the same
  footgun; do not register an unsafe test and postpone its isolation.
- Retain existing production/test build guards. Register the GUI-adjacent data
  tests in GUI-enabled builds without launching a window; document and verify
  the smaller GUI-OFF core subset. No runner-header refactor is required.
- Use CMake build dependencies (`add_dependencies`) for the matching `spirula`;
  CTest's test `DEPENDS` property does not build an executable. Pass the
  target-resolved executable path explicitly and remove integration-test PATH
  fallbacks. Keep dev scripts as the build entry points.
- Teach `scheduler_test` to accept the exact worker executable supplied by CTest
  and fail if it is absent. Audit `subprocess_test` for its own child mode versus
  application-worker use; pass the correct target explicitly where required.
- Run the headless label after the existing Linux, Windows and macOS CI builds.
  Retain the CLI help check as a dispatch check, not behavioral acceptance.
- Upload CTest failure logs and document fast/worker selection and artifact paths.

Acceptance: the complete allowlist is discoverable and executes without a display
or GPU initialization in the normal GUI-enabled build on each supported host
platform; the declared core subset also works with GUI OFF. Missing required
targets/empty selection fail. Two concurrent invocations use distinct owned
scratch/config/cache roots. A deliberately failing check reaches a nonzero CI
step with useful diagnostics. Restore the deliberate failure before integration.
Existing patent-enabled CI configuration is outside this plan; do not change it
incidentally or make it a requirement of the new tests.

### Phase 2 — Cover application requests and orchestration

Ownership: new `src/app/gui/tests/batch_process_test.cpp`; existing
`src/core/tests/{scheduler_test,worker_request_test,subprocess_test,checkpoint_resolution_test}.cpp`;
minimal test-only support if shared; Phase 1's registration module via Main.

- Add missing H1–H3 cases around the real BatchProcess, scheduler and worker APIs.
- Build `batch_process_test` under the existing GUI-enabled data-test gate; this
  is a build dependency, not a requirement to create a window or graphics context.
- Isolate config/preset/cache paths before application calls and retain failures.
- Cover both a successful real CPU prep and bounded failure/cancellation paths.
- Test serialized/resolved behavior and artifacts, not private members, source
  text or translated labels. Keep synthetic before-GPU failures explicitly named.
- Do not change scheduling/device policy to make a test convenient. A discovered
  behavior defect gets a scoped regression and source fix with its own acceptance.

Acceptance: selected regressions fail on the corresponding broken behavior;
normal runs pass in the headless label; two simultaneous test invocations do not
share user state, output leases or artifacts; timed-out children are reaped.
No GPU-success claim is made from this phase.

### Phase 3 — Close deterministic parser and schedule gaps

Ownership: new `src/core/tests/dataset_parser_test.cpp` and
`src/core/tests/step_config_test.cpp`, a small shared generated-scene helper under
`src/app/tests/` if required by both the parser and G1, and registration through
Main. Production owners remain `src/data/parsers/` and `src/app/TrainerCore.cpp`.

- Implement H4 against all four representations of one generated scene, with
  explicit coordinate/split expectations and small tolerances.
- Implement H5 against the actual warmup/decay/activation boundaries in the current
  source. Include relevant config variants and resume state without multiplying
  combinations that exercise the same path.
- Add both to `headless;fast`; update the two gap entries in `docs/testing.md` only
  when their stated contracts have actually been covered.

Acceptance: both run offline without a GPU, display, Python or private dataset;
format-equivalence/split and boundary regressions fail the respective test.

### Phase 4 — Prove a real CLI and worker workload

Ownership: new `src/app/tests/cli_training_smoke.cpp`, shared generated-scene
support from Phase 3, test build/registration and `docs/testing.md` via Main.

- Implement G1 using production subprocess, scheduler and checkpoint readers.
- Give the driver the exact executable and explicit device selection; reject a
  missing prerequisite instead of falling back or claiming a pass.
- Verify checkpoint publication, resume advancement and stop outcomes rather than
  exact losses. Reopen the last valid checkpoint after forced termination. Keep
  the explicit distinction between this check, Phase 2's incomplete-sibling
  rejection, and deterministic mid-publication fault-window testing.
- Register only this self-contained scenario under `gpu` initially, with a shared
  resource lock. When deferred GPU validation resumes, run it on actual supported
  non-NVIDIA Vulkan hardware; no hosted-CI software-renderer result substitutes.
- Document the exact command, toolchain/device identity, fixture settings
  and retained failure evidence. Do not auto-download models or parity references.

Acceptance: real direct training and scheduled resume succeed, cooperative stop
is resumable, force-stop is truthful, and a corrupt/missing expected artifact
fails the check. Both backend results are recorded, or the unavailable backend
is explicitly left unverified rather than marking this phase accepted.

### Phase 5 — Establish the small desktop gate

Ownership: `docs/testing.md`; application source only for a discovered, separately
verified defect. No `gui --script`, fake renderer or startup-only automation hook.

- Run D1 on an actual display/graphics driver with isolated application state.
- Run D2 with the small fixture from Phase 3: submit, observe visible state/logs,
  cancel, confirm final state, then close normally and verify process-tree exit.
- For device-control changes on a two-GPU host, additionally keep a preview on A
  while submitting to B. Observe desktop/queue independence; do not infer it from
  the selected-device label alone.
- Record screenshots and process/state evidence plus which interaction was
  exercised. A first-frame/close pass is only D1, never D2.
- Map changed surfaces to the smallest gate. Backend-only edits normally need no
  desktop clicking; UI wiring/presentation edits do. Preserve all already-open
  handoff acceptance cases until they have their own evidence.

Acceptance: D1 and D2 have explicit observed outcomes and a concise repeatable
checklist; the remaining prior GUI acceptance backlog is still represented
truthfully. Web-viewer CDP checks remain separate from this desktop gate.

### Dependencies and delegation

Phase 0 precedes writing. Phase 1 establishes the registration/CI integration
owner. Phases 2 and 3 can then run in isolated, disjoint writing lanes once their
source baseline is available; Main alone integrates shared CMake/CI/docs edits.
Phase 4 depends on the fixture and process-contract work. Phase 5's checklist can
be drafted earlier, but its integrated run uses the final binary and fixture.

Every writing lane owns exact files and its smallest isolated smoke check; no
lane runs repository-wide formatting, builds or suites. Main performs shared
validation after integration and checks branch/index/stash preservation. Tests
added by a lane must defend a named observable contract, not merely increase the
number of green executables.

## 7. Completion and remaining boundaries

This work is complete when:

1. The documented post-build headless command runs real behavior checks in CI and
   locally, cannot succeed with zero tests, and clearly distinguishes the complete
   normal-build suite from the supported GUI-OFF core subset.
2. H1–H5 have executable, isolated coverage with useful failures and bounded child
   lifetimes; existing useful cases are not duplicated or weakened.
3. G1 has actually trained, resumed and exercised stop behavior on the required
   hardware backends, with artifact-based evidence.
4. D1 and D2 have run on the real desktop surface; their scope is not overstated.
5. `docs/testing.md` describes the commands, prerequisites, labels, artifacts and
   change-to-gate mapping; resolved coverage gaps are updated accurately.
6. No private paths, datasets, weights or generated runtime artifacts are
   committed, and no paused-work changes have been discarded or silently claimed.

No change in this plan proves every GUI workflow, every learned model, numerical
parity across vendors, or the broader scheduler's throughput/fault-window
acceptance. Those retain their existing focused gates. The improvement is a much
shorter ordinary feedback loop, with the expensive or visual checks invoked only
for the surfaces they actually validate.
