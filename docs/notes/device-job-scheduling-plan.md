# Explicit-device process scheduling

Status: Implementation through Phase 4 is present; Phase 4 desktop acceptance and Phases 5-6 remain open.

Target branch: `feature/device-job-scheduling`, based on `feature/native-gpu-selection`.

## 1. Goal and boundary

Schedule independent native workloads as separate operating-system processes on explicitly selected physical GPUs. Two independent jobs may run simultaneously on two GPUs; each worker uses exactly one GPU. Change the target of a later dataset, training, or not-yet-started phase without restarting the desktop application.

**Do not make `EngineState` multi-device, instantiate multiple trainers inside one process, or split one training run across GPUs.** Process isolation is the mechanism, not an interim workaround to remove later.

### Scope decisions

- One local scheduler owned by the desktop application, not a daemon, cluster manager, or remote execution service.
- One active scheduled GPU process tree per physical GPU. A worker may have CPU threads and sequential helper children; it may not independently allocate work to another GPU.
- Cover native dataset preparation, built-in reconstruction, optional geometry, and independent training. Reuse existing mesh child routing when integrating resource admission; do not add a separate mesh workflow editor.
- Preserve the interactive foreground trainer, previews, and finished-model viewer. They remain single-device in the desktop process and participate in admission control. Scheduling a child must not reconfigure their device.
- Reuse the existing batch editor and configuration resolver for training submissions. Add dataset submissions to the same scheduler; do not create independent queues with conflicting ownership rules.
- Scheduled jobs initially expose logs, phase/progress, device, output, stop-and-save/cancel, and recovery. Scheduled training is unattended by default, matching the current batch configuration's disabled HTTP viewer. The foreground trainer retains its native live viewport and pause/stop/save controls. The job UI must distinguish these surfaces rather than imply a remote `TrainerSession*` can attach to the native viewport.
- External COLMAP and external Python masking do not currently honor the native GPU selector. Reject those paths in the explicitly routed native queue with an actionable explanation; preserve their existing separately labeled interactive paths. The ordinary ffmpeg CPU decode fallback remains allowed. Do not silently launch an unassigned external GPU operation from a scheduled native job.
- Keep `SS_ENABLE_PATENTED=OFF` by default. Native video coverage is a separately opted-in acceptance row.
- Vulkan is the scheduling acceptance target. Preserve CUDA source/build guards and its separate ordinal semantics, but add no CUDA validation or cross-API identity-mapping project.

Not included: distributed training, shared optimizer state, inter-GPU splat exchange, multi-GPU kernels, GPU hot-switching inside a live worker, automatic migration/retry, priorities/preemption, memory-packing multiple jobs onto one GPU, a general dependency-graph framework, or system-wide management of unrelated applications.

## 2. Current implementation and reusable seams

The source is authoritative. The historical gap table in `gpu-selection-plan.md` predates its implementation; its application-lifetime picker lock is superseded here **only for future child-job targets**, not for live runtime contexts.

| Area | Observed implementation | Planning consequence |
|---|---|---|
| Identity and precedence | `src/core/VulkanDeviceSelection.h`: `parseRequest`, `requestFrom`, `resolveRequest`; Auto, ordinal, unique case-insensitive name substring, canonical UUID; explicit selection beats `SS_VK_DEVICE` | Reuse the parser and resolver. Do not add `--gpu`, a second identity parser, or ordinal-to-ordinal forwarding. |
| Native runtime ownership | `src/backend/vulkan/VulkanContext.cpp::device_select_identity`; `src/nn/vk/Context.cpp::configure/get/shutdown`; SfM `VkContext` | Backend and NN selection are pinned within a process. NN shutdown preserves configured identity. Keep those invariants. |
| Engine | `src/engine/EngineState.cpp::engine/engine_reset`; `src/app/TrainerCore.h` | One process-global engine and one live training session. Independent workers solve the ownership conflict. |
| GUI device state | `GuiApp.cpp::freeze_native_device`, `propagate_frozen_device`, `draw_device_picker` (approximately 3344–3602) | Today a selection configures the desktop runtimes and is copied into every job. Split desktop binding from pending-job target data. |
| GUI serialization | `GuiApp.cpp::native_work_busy`, `start_dataset_job`, `launch_training` | Global busy gates are appropriate for shared in-process consumers, not independent children on different GPUs. Replace only the latter restrictions. |
| Training queue | `BatchTrain.{h,cpp}` and `GuiApp.cpp::advance_batch` (1005–1066) | Batch rows run sequentially through one in-process `TrainRunner`. Reuse row editing, preflight, presets, and `batch_build_config`, not this execution loop. |
| Training worker path | `src/app/cli/main.cpp::spirula_train_main`; `TrainerSession`; `ckpt::build_resume_config` | The complete native trainer and resume path already exist. Extract/reuse entry glue where necessary; never write a second training loop. |
| Native child selection | `SfmRunner`, `GeometryRunner`, `MeshRunner` already pass `--device` explicitly to this executable | Child processes can select a different device from their parent now. Keep explicit argv routing and independent runtime resolution. |
| Dataset preparation | `DatasetPrep::run`, `PrepJob`, `PrepResult`; both reconstruction runners call the same implementation | Prep remains in the GUI process even when reconstruction uses its child mode. Merely setting `SfmJob::subprocess` does not isolate a dataset job. |
| Prep build coupling | `DatasetPrep.h` includes `FilmReel.h`/GL; `DatasetPrep.cpp` uses a GUI-only generated mask-script embed; `cmake/SsApps.cmake` globs GUI sources | Make the existing prep core callable headlessly, retaining UI adapters. `spirula sam extract` does not cover multi-input photo/video preparation and is not a replacement. |
| SfM state/progress | `sfm::RunContext`, `Pipeline`, `core/Progress`; `gui/SfmProgress` | SfM also has process-global sinks/cancellation/progress state. Reuse its existing child progress artifacts and one-run-per-process seam. |
| Subprocesses | `gui/Subprocess.{h,cpp}::run_process(argv,cwd,on_line,cancel)` | One blocking runner already handles quoting and line streaming. Extend/move this implementation rather than introducing another launcher. |
| Process lifetime gaps | Windows uses `CreateProcessA`, inherited handles, and direct-child `TerminateProcess`; POSIX uses `fork/execvp` and group kill on explicit cancel | Concurrent handle inheritance, descendants, cooperative stop, parent death, and safe reaping need explicit work. A POSIX process group alone does not terminate on parent death. |
| Environment | `GuiApp.cpp::set_ss_env` changes `SS_UNREG_LOG` per run; children inherit the parent's environment | Per-job paths must not be conveyed by concurrent parent `setenv`/`_putenv` calls. Add child-local overrides or direct options at this existing boundary. |
| Training output | `TrainerCore.cpp::setup_engine` (763–778) uses second-resolution timestamps unless an exact name is supplied | Timestamp names alone cannot prevent concurrent output collisions. Allocate and lock the run directory before launch. |
| Checkpoint safety | `TrainerCore.cpp::save_checkpoint` writes and validates a unique sibling staging directory, publishes it by rename, and only then prunes; archive readers reject incomplete/corrupt payloads and fall back to the newest valid step | Reuse this seam for worker stop/recovery. Scheduling still needs exclusive run-directory ownership so two processes cannot publish the same step. |
| Recovery | Current branch has CLI resume but no durable multi-job recovery collection or GUI recovery modal | Implement/integrate one authoritative recovery state; do not assume another branch is already a dependency. A singleton last-run marker cannot represent concurrent jobs. |
| Viewer | CLI enables a fixed-port viewer by default and may remain alive after training; `batch_build_config` disables the viewer | Scheduled training must set `disable_viewer=true` and `keep_viewer_alive=false`. Do not allocate ports by bind-then-close probing. |

Existing examples worth reusing: `src/nn/tests/device_selection_test.cpp`, `src/backend/vulkan/tests/vk_runtime_smoke.cpp`, the existing per-workflow status readers, `TrainConfigJson.h`, `data/Json.h`, `AppPaths`, `ReconStamp`, and existing workspace/resume checks. None of these proves concurrent scheduling by itself.

## 3. Architecture and contracts

### 3.1 Ownership

```text
Desktop application
  ├─ existing foreground trainer / preview / finished-model viewer
  │    └─ one fixed desktop GPU; existing singleton ownership rules
  └─ local job scheduler
       ├─ job A: ready phase → worker process tree → GPU A
       ├─ job B: ready phase → worker process tree → GPU B
       └─ queued/blocked jobs; one durable recovery collection

Each worker:
  immutable request → resolve explicit UUID → existing native workflow
                   → validated phase result → process exit/reap
```

A worker is one phase attempt and is never reused for a different GPU. The scheduler holds the device lease until the process tree has exited and been reaped, not until a progress message says "done". Different phases of a dataset may use different GPUs only by ending one worker and launching the next.

Use a fixed ordered phase list, not a general DAG:

- Native dataset: prepare frames/masks → built-in SfM → optional geometry → publish dataset result.
- Training: train/resume → existing finalization/evaluation in the same worker.
- Dataset-then-training: an optional linked training submission becomes ready only after the dataset result is validated and published.
- Existing mesh work: reserve its selected device and output while its existing native child runs.

Prepare combines the existing extraction/masking sequence initially. Do not split kernels or force expensive model reloads within a phase merely to create more queue items. Phase boundaries match recoverable artifacts and existing pipeline seams.

### 3.2 Device assignment

1. Separate **desktop device binding** from **default target for new jobs** and **each job/phase target**. Only the first may configure desktop backend/NN contexts.
2. Show driver-provided GPU names. Disambiguate identical names with a short identity suffix or stable physical identifier; retain full UUID in diagnostics and stored execution data, not as the primary label.
3. At submission, resolve the selected/default request through the existing precedence rules and snapshot a canonical UUID plus display name. Explicit Auto uses existing ranking once; it is not a new "least busy GPU" policy. Missing selection follows the established environment/Auto rules.
4. A busy selected device queues the job. Do not rerank Auto or silently migrate to an idle GPU. Refresh inventory and validate UUID again before dispatch and recovery; each actual runtime validates it in its own Vulkan instance.
5. Allow editing pending jobs and not-yet-started phases, saving the change atomically. A running attempt's UUID/configuration is immutable. To move it, stop it, wait for exit, then explicitly restart/recover on the new target.
6. Treat capabilities as workload-specific. A listing marked usable does not prove SAM weights fit or that every SfM arithmetic path is supported. Preserve legitimate same-device fallbacks; a missing/wrong explicit GPU is never a fallback condition.
7. Keep device choice out of checkpoint tensor/model compatibility. Recovery may select another GPU; it must still use the existing checkpoint configuration/adaptation rules.

### 3.3 Admission and fairness

Keep a small pending list and a map of active device leases. On the owning thread, scan ready entries in queue order and start the first eligible entry for each free device. A blocked GPU-A job must not prevent an unrelated GPU-B job from starting. Preserve FIFO order among equally eligible jobs for the same GPU.

Admission requires all of:

- Required preceding phase succeeded and its output validates.
- Target UUID exists and satisfies the phase's preflight requirements.
- No active scheduled tree or incompatible foreground work owns that GPU.
- Exclusive mutable-workspace/run-output ownership is available.
- Required inputs, model weights/terms acceptance, native build modules, and disk destination are available.

One supervising thread per active process is sufficient with the current blocking process runner. The UI thread owns queue state; callbacks publish bounded progress/log updates rather than mutate UI objects. Bound in-memory log tails and stream complete logs to per-attempt files.

The lease covers scheduler-managed work, not every process on the machine. Do not claim exclusive physical access or guaranteed VRAM. External load can still cause OOM; report it without changing devices automatically. No memory estimator or packing algorithm is required.

### 3.4 Foreground integration

- Preserve `TrainRunner`, `TrainerSession`, native viewport attachment/detachment, `CompareView`, and preview sequencing for interactive work.
- Foreground training, GPU previews, resident model allocations, and mesh/render activity must be accounted for before admitting a child onto the same physical GPU. Do not treat `busy=false` as proof that retained pools or model allocations are gone.
- Reuse existing detach/unload/reset/shutdown paths to release heavyweight foreground ownership. Keep the desktop's configured UUID pinned even after its allocations are released. Small retained context/display allocations are overhead, not permission to switch the desktop device.
- Prove release before removing a reservation; otherwise keep the device unavailable with an explicit reason. Do not reset a live foreground engine to make room for a child.
- An unrelated worker on GPU B must not be blocked merely because the desktop has a foreground session on GPU A. Conversely, interacting with a scheduled row must not call `note_engine_taken()` or bind its state into the foreground viewport.
- Scheduled targets remain editable after a preview has pinned the desktop device. This is how GPU changes between datasets/trainings avoid requiring a desktop restart.

### 3.5 Worker boundary and configuration

Add a small, internal self-worker command in the same `spirula` executable, registered through `Main.cpp`/`Tools.h`. Proposed spelling: `spirula worker --request <file>`. It is not an arbitrary command executor or a plugin protocol.

The request carries a schema version, job ID, attempt ID, phase, canonical target UUID, language, exact paths, and that phase's immutable typed options. Use the existing JSON facilities. Reuse `TrainConfigJson.h` for training fields and existing native argument builders/parsers for SfM/geometry; introduce one serialization of prep options, not another prep implementation. Preserve explicit-option/touched metadata where macro and resume precedence require it.

The worker dispatches only allowlisted native phase kinds:

- Prep → the extracted/headless `DatasetPrep::run` core.
- SfM → the existing shared `parse_auto_args`/`run_auto` path, with its progress directory.
- Geometry → existing CLI/application implementation and `GeometryModel`, without another model-specific path.
- Train → existing CLI/session-driving implementation after separating argv parsing from execution where needed. Preserve config checking, device selection, setup, progress, final save, evaluation, and error behavior.

Do not construct a second config-to-training loop in the worker. Ordinary CLI commands remain usable and share execution with the worker. Optional-module/headless builds reject unavailable worker kinds explicitly.

Freeze settings before submission; later preset edits, GUI changes, current-directory changes, or environment changes cannot alter a running request. Resolve relative input paths against the submission directory once. Validate payload types, ranges, schema version, job/attempt identity, and owned output paths before GPU initialization or destructive filesystem work. Unsupported/corrupt requests fail visibly, never fall through to the GUI.

The parent owns durable recovery state. Workers write separate, attempt-scoped progress/result artifacts, not competing copies of the recovery collection. Reuse SfM status artifacts and existing progress fields; use a small atomic terminal result carrying phase outcome and published paths where no existing result contract suffices. Human output remains localized; parse required human messages with `i18n::scan`, never English substrings. Ignore stale status from earlier attempts.

### 3.6 Process supervision

Extend the existing subprocess implementation and migrate its consumers if it moves out of `gui/` for headless linking. Preserve argv semantics and line streaming.

Required behavior:

- A typed outcome distinguishes launch failure, ordinary exit, cooperative stop, forced cancellation, and crash. Do not infer success merely from exit zero: validate the current attempt's expected result.
- Child-local environment overrides inherit unrelated settings without mutating the parent. Remove per-job `SS_UNREG_LOG` changes from the global environment path. Route native devices explicitly, not by environment mutation.
- Windows: Unicode executable/cwd/argv/environment handling; restricted handle inheritance for concurrent launches; suspended creation/assignment to a kill-on-close Job Object before execution; terminate/reap the owned tree on escalation. Handle nested-job restrictions explicitly rather than silently dropping tree ownership.
- POSIX: construct argv/environment before launch; use `posix_spawn` where applicable or keep the post-fork path async-signal-safe. The current post-fork vector allocation is unsuitable as the foundation for concurrent GUI launches. Close unrelated pipe descriptors and establish reliable process-group ownership.
- Provide a cooperative parent-to-worker control pipe for stop/cancel and parent-liveness EOF. Forward cancellation to existing phase cancellation tokens. Signals, where supported, feed the same safe control path; signal handlers must not perform checkpoint I/O.
- Stop-and-save requests `save_on_stop` and `stop_requested` through the existing training controls, waits for checkpoint publication and process exit, then releases leases. Force-stop is distinct and warns that only the last published checkpoint is recoverable.
- After a bounded grace period, terminate the owned tree. On parent death, prevent workers/helpers from becoming silently unsupervised owners. Windows uses Job Objects; POSIX needs the control/liveness channel plus process-group cleanup, including propagation to nested helpers. EOF alone is not sufficient if a hung worker cannot respond: preserve its output ownership and block relaunch until death is confirmed.
- Do not kill a recovered PID merely because its number matches a saved one. Verify process identity/start token and ownership; ambiguous survivors block recovery rather than risk killing unrelated processes or writing concurrently.

Process-tree grouping must include helpers spawned by the worker. Existing nested `run_process` calls that create new POSIX groups must not escape the root worker's cancellation ownership. Complete this platform behavior before enabling concurrent GUI dispatch.

### 3.7 Output ownership and crash recovery

One scheduler-owned, versioned atomic state file in the application config directory contains the job collection. Proposed name: `job-state.json`. It is the authoritative recovery record, not a second copy of `batch.json` execution state. Existing saved batch rows can remain editing templates or be migrated once; they must not independently claim jobs are running.

Each record contains:

- Job and attempt IDs; kind; state; current phase; completed phases.
- Source paths/input options; workspace; engine/model selection; exact training configuration or prep/reconstruction options.
- Device UUID/display name for each planned phase and the actual attempted target.
- Exact run/output/progress/log locations; resume source and last validated published checkpoint.
- Error/outcome and a pending-resume flag. PID/start identity is optional diagnostic/ownership evidence, never authority to restart or kill.

Use temp-write/flush/atomic replacement on the same filesystem. Retain the previous valid record on failure; never replace malformed input with an empty queue silently. Parent is the only writer. If durable state cannot be saved, stop dispatching new work, report the error, and continue supervising already-live workers.

State machine:

```text
Queued → Starting → Running → Succeeded
                   Running → Stopping → Stopped
Starting/Running/Stopping → Failed or Interrupted
Queued/Interrupted → Blocked (missing GPU/input, ownership conflict, invalid recovery)
Blocked → Queued only after resolution
Interrupted/Stopped/Failed → Queued only by explicit recover/retry action
```

`Starting` is saved before spawn; attempt identity ties launch, progress, exit, and recovery together. Commit a completed phase only after a matching result, validated artifacts, and process exit. A crash between artifact publication and state update is reconciled using that attempt's result and existing resumability checks; otherwise restart the interrupted phase. Do not rerun completed destructive import/move operations blindly.

Allocate collision-proof output names with an exclusive create, not just a timestamp. Hold OS-backed exclusive locks for mutable workspaces and resumed run directories across the whole workflow, including gaps between phases. Hold/read-check source ownership when another queued job can mutate that dataset. Canonicalize existing ancestors and account for Windows case folding, symlinks/junctions, and ancestor/descendant overlaps; string equality alone is insufficient. Concurrent read-only training from a completed immutable dataset may share its input; two writers to one workspace/run may not.

Lock lifetimes must also protect against a worker surviving the scheduler. Use worker-held ownership or a verified lifetime handoff; releasing the parent's handle on crash must not let another writer start beside a surviving child. Acquire scheduler-state ownership as well: a second desktop instance must not drive the same queue. These locks coordinate participating Spirula processes, not arbitrary external programs.

Recovery behavior:

- After restart, do not treat saved `Running` as still running or automatically launch duplicates. Reconcile survivors/locks, then mark interrupted work recoverable.
- Persist source paths, workspace, engine, options, and completed phases together. Recover by restarting the interrupted phase through existing resume/reuse logic, not checkpointing inside GPU kernels.
- For training, resolve and validate the last published checkpoint. For prep/SfM, reuse existing workspace/stamp/signature rules and reject incompatible reuse. A partial file's existence is not proof of completion.
- GUI recovery accepts a run directory as well as an explicit checkpoint, using the existing checkpoint resolver.
- **Later** dismisses the prompt without opening the run and preserves that job's pending-resume flag for the next application run. Dismissing one job must not clear another's recovery state.
- Reselect a missing GPU explicitly. Never substitute an ordinal or similar name. A new device is an execution choice, not a reason to invalidate completed dataset phases or hardware-bind a checkpoint.

The checkpoint prerequisite is present: each autosave uses a unique sibling staging directory, validates the complete payload and associated config, publishes by atomic rename, then prunes older checkpoints. Resume ignores incomplete staging and corrupt final directories. An existing valid destination is retained; an invalid destination is moved aside and restored if publication fails. Concurrent scheduling must preserve this behavior and add exclusive output ownership rather than introducing another checkpoint path.

## 4. Implementation phases and acceptance gates

### Phase 0 — Establish the implementation baseline

**Ownership:** Main; source and dependency inspection only.

1. Record branch/status/stashes and confirm the native-selection dependency is present.
2. Determine whether atomic checkpoint/recovery work has since landed. Treat any other branch as an explicit reviewed dependency, not an assumed merge or a source of files to copy blindly.
3. Inventory native phase serializers, UI-only prep dependencies, foreground resource-release paths, and all process-runner callsites. Confirm exact symbol references before exports move.
4. Select public/synthetic fixtures, licensed local model weights, two suitable GPUs, and Windows/POSIX test hosts. Record unavailable prerequisites by acceptance row.
5. Freeze the contracts in section 3 before parallel writing lanes: job/attempt identity, phase request/result, device leases, cancellation, output locks, and durable-state ownership.

**Gate:** executable file/symbol ownership is assigned, prerequisites are explicit, and no phase relies on adding multiple engines or resetting live device selection.

### Phase 1 — Safe process and checkpoint foundations

**Ownership:** subprocess/lifetime lane; independent checkpoint-publication lane. Main owns shared target/build integration.

- Extend/move `Subprocess.{h,cpp}` without creating a second implementation. Add process identity, cooperative control, child-local environment, robust tree cancellation/reaping, and platform-safe concurrent launch behavior.
- Migrate existing subprocess callers cleanly; preserve ffmpeg/curl/COLMAP behavior outside the native queue.
- Retain the existing atomic checkpoint publication and last-valid fallback in `TrainerCore`/checkpoint code. Keep training math and kernels unchanged.
- Separate per-process crash logs and per-attempt human logs from the application's global crash destination where concurrent workers would otherwise overwrite one another; reuse `CrashLog`/`AppPaths` rather than add telemetry infrastructure.

**Gate:** a real helper process tree streams output, receives cooperative cancellation, survives neither an owned forced-stop nor an unhandled Windows parent exit, and cannot leak sibling pipe handles. POSIX parent-death/survivor behavior is verified and blocks unsafe relaunch. Fault injection during checkpoint creation/publication leaves the previous checkpoint resumable. Both gate results are required before concurrency is exposed.

### Phase 2 — Headless phase workers using existing implementations

**Ownership:** dataset/prep lane and training-worker lane after the common request/control contract exists. Shared worker dispatch registration has one owner.

- Decouple prep data/progress from `FilmReel`/GL and GUI lifetime. Move only the execution core and needed data types; retain GUI adapters for film/progress updates. Fix generated-resource/CMake ownership so native prep works with `SS_BUILD_GUI=OFF`.
- Serialize all meaningful prep inputs/options: multi-input layout, frame selection, pano/lens settings, supplied masks, prompts/stencils, import/move behavior, native models, and resume/redo flags. In-process callbacks/live edits become immutable submitted data for scheduled work.
- Dispatch prep, SfM, geometry, and training through their existing shared implementations. Preserve phase result classifications such as incomplete/nonmetric reconstruction rather than treating every exit as success.
- Training requests reuse resolved `TrainConfig` semantics and resume overrides exactly; avoid reapplying quality macros to an already-resolved snapshot. Set exact output directory, disabled viewer, and no keep-alive.
- Connect stop-and-save to `TrainerSession` controls; connect cancellation to SfM/prep/geometry control paths. Apply it before first GPU use as well as during execution.
- Prevent scheduled prep from falling back to external Python/COLMAP GPU execution. Preserve native SAM families, geometry models, and CPU ffmpeg fallback.

**Gate:** each phase runs once through the actual headless self-worker with a non-default explicit UUID and conflicting inherited selector. Valid artifacts and actual runtime device identity agree. Complete one multi-input prep case, not only `sam extract`; resume a short training run from a run directory; stop it cooperatively and reopen the checkpoint. GUI interactive paths still use the same implementation.

### Phase 3 — Scheduler, admission, and authoritative recovery state

**Ownership:** scheduler/state lane; integrate only after phase requests and result semantics are stable.

Proposed new application-level files: a small `JobScheduler.{h,cpp}` and job request/state types/codec if needed. Keep them outside `gui/`; no base-class runner hierarchy, backend plugin registry, or thread-pool framework.

- Implement the fixed phase sequence, per-device admission, first-eligible FIFO scanning, immutable attempt snapshots, and bounded callback handoff.
- Allocate/lock exact output destinations and reject conflicting source/workspace/run ownership. Guard the single queue owner and survivor windows across crashes.
- Persist the full job collection atomically at transitions. Import existing batch editing data without introducing a second executable queue state.
- Complete phase results transactionally; preserve every interrupted job independently and support explicit retry/recovery with a new attempt ID.
- Enforce failure isolation: job A failing or stopping cannot clear job B's lease, log, output, or pending-resume flag. No implicit device failover or automatic restart.

**Gate:** three actual child attempts demonstrate A1/A2 serialization on GPU A while B1 starts on GPU B; all terminal paths free the correct lease only after reaping. Duplicate outputs/resume targets are refused. Restart with two interrupted jobs preserves both records, their completed phases, and target selections without launching duplicates.

### Phase 4 — GUI integration and between-phase selection

**Ownership:** one GUI integration owner for `GuiApp`, `BatchTrain`, shared catalogs, and launch wiring.

- Split pending-job GPU choice from the fixed desktop GPU. Remove `propagate_frozen_device` behavior only from scheduled submission paths; preserve it where in-process consumers require the desktop binding.
- Route batch execution and queued native dataset creation into the one scheduler. Keep `batch_check`/`batch_build_config` and existing editors; remove the obsolete sequential batch launch state after callers are migrated.
- Show queued/running/stopping/blocked/interrupted state, phase, human-readable device, logs, output, and actionable block/error reason. Device changes affect only pending work.
- Add queue actions: submit, cancel pending, stop-and-save training, force-stop with warning, retry/recover, and pause new dispatch. "Stop after current" becomes stop new dispatch and let all currently running jobs finish; explain this multi-job meaning.
- Preserve foreground training/viewer/preview controls. Integrate their device reservations, resource release, and cross-device concurrency without weakening engine handoff guards.
- Add recovery prompt/list with per-job Later semantics and run-directory resume. Opening a completed model remains an explicit foreground action subject to normal engine ownership.
- Follow `ui::` wrappers, all 13-language catalogs, stable ImGui IDs, plural-safe copy, and font coverage. Keep masking preview colors unchanged.

**Gate:** launch the real desktop, pin its preview device, then choose another GPU for a queued dataset/training without restarting. Observe two independent jobs, change a pending phase target, stop one without disturbing the other, recover both after restart, and verify Later leaves the intended job pending. Interactive training pause/stop/save and mask-overlay color correctness remain intact. CLI-only evidence cannot satisfy this gate.

### Phase 5 — Integrated correctness and throughput proof

**Ownership:** Main; shared builds and acceptance after integration, not inside concurrent writing lanes.

Run the matrix in section 5 through the actual scheduler and worker paths. Use real native workloads, not mocked GPU success or command-string assertions. Include a headless build and optional-module guards; keep patented decoding disabled except for its explicit opt-in row. Fix integration failures before declaring the feature complete.

Measure a bounded serial baseline and scheduled execution of the same independent jobs on the selected hardware. Report makespan, per-job duration, overlap, and failures; do not promise linear speedup. CPU image processing, storage, RAM, model loading, and shared host bandwidth may be the limit even when device assignment is correct.

**Gate:** all mandatory rows pass with recorded commands/settings, fixtures, device identities, outputs, and results. Any unavailable two-GPU/model/platform/GUI row remains explicitly unverified; a single-GPU smoke does not prove the goal.

### Phase 6 — Post-proof cutover and documentation

After the smoke/acceptance evidence exists:

- Remove obsolete batch execution state, old scheduled-device propagation, temporary probes, duplicate launch/serialization paths, and throwaway orchestration scripts.
- Retain only small regression checks defending real ownership, cancellation, recovery, and checkpoint bugs.
- Update existing app/architecture/build/testing and GPU-selection documentation for scheduled versus foreground semantics, supported native scope, queue persistence, output locking, recovery, and known limits. Do not label external routing or distributed training complete.
- Confirm all affected callers, optional targets, localized copy, and changed comments match current behavior; no compatibility aliases or abandoned alternate scheduler remain.

**Gate:** one scheduler, one prep implementation, one training driver, one process launcher, one authoritative recovery collection, and no running test workers or unresolved integration state.

## 5. Required verification matrix

Build using the repository entry points, for example:

```text
build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_SFM=ON -DSS_BUILD_SAM=ON -DSS_ENABLE_PATENTED=OFF
bash build_develop.bash -B build_headless -DSS_BACKEND=vulkan -DSS_BUILD_GUI=OFF -DSS_BUILD_SFM=ON -DSS_BUILD_SAM=ON -DSS_ENABLE_PATENTED=OFF
```

Run the corresponding entry point on each target OS. Do not add a CUDA validation requirement. Keep native-video verification in a separately configured opt-in build.

| Check | Observable pass condition |
|---|---|
| Explicit targeting | Scheduled request conflicts with inherited `SS_VK_DEVICE`; actual created NN/SfM/backend context uses the request UUID. Selection banners or copied argv alone do not prove GPU work occurred. |
| Missing/ambiguous device | Missing UUID, ambiguous identical names, malformed input, or unsupported phase blocks/fails the correct job without rerouting or poisoning the next valid attempt. Reuse existing selector regression cases. |
| Two-GPU concurrency | Two independent native workloads have overlapping execution intervals on distinct physical GPUs, valid outputs, and distinct process identities. Include training on both suitable GPUs, not only two inventory commands. |
| Same-GPU serialization | A2 cannot start before A1's tree is reaped, including stop/failure paths. A blocked A job does not starve a ready B job. |
| Pending target change | Change a queued phase from A to B after a prior phase finishes; next worker uses B, completed work is reused, and the desktop never restarts or changes its live context. |
| Native preparation | Multi-input photos/video, supplied/generated masks, frame selection and CPU ffmpeg fallback behave like the existing path. Native SAM 2/3 and relevant tracking semantics remain correct. No external Python GPU fallback slips into the queue. |
| Reconstruction and geometry | Classical and learned SfM contexts remain on the selected UUID; MoGe-2 and Metric3D produce finite/aligned results. Preserve existing reconstruction outcome/resume semantics. Host-only `geometry --check` is not inference proof. |
| Training fidelity | Submitted resolved configuration and resume precedence match existing execution; bounded training, eval, and checkpoint reload succeed. Compare meaningful tolerances, not bitwise trajectories across nondeterministic devices. |
| Cooperative stop | Stop-and-save publishes a valid checkpoint, reports Stopped rather than Succeeded, and releases resources only after exit. Another device's job continues. |
| Forced stop/crash | Mid-phase and mid-save interruption retains the last valid checkpoint and marks only that attempt interrupted/failed. Descendants do not escape supervision. |
| Parent death/startup windows | Kill the scheduler before spawn, after spawn/before Running persistence, during work, and after result publication/before state commit. No duplicate writer starts; survivors are owned or explicitly block recovery. |
| Recovery collection | Two interrupted jobs retain independent paths/options/phase lists/GPUs. Later dismisses without opening or clearing the pending flag; it returns on the next app launch. Resume from a run directory works. |
| Atomic storage failure | Interrupt state/checkpoint writes and simulate disk-full/publication failure. Old valid state/checkpoint remains readable; no pruning before successful publication, no silent reset to an empty queue. |
| Output/source ownership | Same explicit run path, two resumes of one run, duplicate workspaces, aliased/case-variant paths, and overlapping source mutation are rejected. Read-only shared datasets with distinct run directories remain allowed. |
| Environment and handles | Concurrent children receive their own log/settings paths; parent environment is unchanged; Unicode/space/quote paths work. One child's pipe stays independent when another starts/exits. |
| Foreground coexistence | Existing native viewport/plots/pause/stop/save and finished-model viewing work; resident foreground GPU ownership blocks conflicting work, while an unrelated GPU can run a child. Mask preview remains color-correct. |
| Exit/viewer behavior | Unattended training does not bind the shared default port or hang waiting for a viewer. Natural completion, stop, and cancel each have correct terminal state. |
| Platform/build guards | Windows and POSIX process tests pass; Vulkan GUI/headless and relevant optional modules build; unsupported worker kinds fail clearly; native-video defaults and external scope remain honest. |
| Throughput | Same finite job set run serially and scheduled, repeated under stated conditions; report overlap and makespan with host-resource contention, not just theoretical device utilization. |

Permanent checks should be few and behavior-oriented: admission/lease transitions, real process-tree/environment isolation, output collision prevention, multi-job recovery/Later persistence, and interrupted checkpoint publication. Use existing native test conventions, not a new framework. Keep GPU workload orchestration throwaway unless a reproducible regression justifies retaining it. Every retained assertion must fail on a plausible observable bug, not a source-text or field-forwarding change.

## 6. Execution ownership and integration order

Main owns decomposition, contracts, shared-file integration, and final acceptance. After plan approval:

1. Record current branch, `git status --short`, and `git stash list` before each writing wave. All pre-existing dirty paths remain Main-owned.
2. Use isolated writing lanes. Keep the parent checkout read-only until lane merges settle. If isolation is unavailable, parallelize read-only research and serialize parent writes.
3. Wave A: process supervision and atomic checkpoint publication may proceed concurrently with disjoint ownership. Assign CMake/common application integration to Main, not both lanes.
4. Publish the worker request/result/control contract. Wave B: headless dataset core and training worker integration may proceed concurrently; one owner handles `Main.cpp`/`Tools.h`/worker dispatch and build files. No lane invents another serializer or status schema.
5. Wave C: scheduler/state/ownership implementation consumes integrated worker contracts. Serialize any checkpoint/recovery overlap. GUI integration follows the scheduler API; catalog and `GuiApp` ownership remains singular.
6. Every lane runs its smallest isolated smoke, skips project-wide formatting/lint/build/test sweeps, and aborts rather than claims success if that check cannot pass. Shared acceptance belongs to Main after integration.
7. Inspect every lane's actual check result and integration metadata; reconcile branch/status/stashes and user work, including stash-restore warnings. Run the combined integration checks after the final wave and leave no running jobs or unresolved merges.

Do not parallelize by shared file or pretend a serial dependency is an independent lane. The plan is complete only when the real process/device/recovery behavior passes—not when worker entry points compile.

## 7. Risks and deliberate limits

| Risk | Required mitigation / limit |
|---|---|
| Prep extraction duplicates logic or drags GL into headless builds | Extract the existing core and adapt its progress/film boundary; execute identical multi-input fixtures through both surfaces. |
| Old desktop lock defeats per-job choice | Separate stored job identity from parent runtime configuration; never call `freeze_native_device` solely to submit a child. |
| Nested helpers outlive cancellation | Root process-tree ownership plus inherited control/ownership rules; test helpers and parent death, not only direct-child exit. |
| Last checkpoint lost on interruption | Atomic staged publication, validated payload, prune-after-publish, recovery ignoring staging. |
| Shared recovery marker loses jobs | One atomic collection with independent job/attempt records; no child writes a global last-run marker. |
| Duplicate writers corrupt workspace/resume | Exclusive OS ownership and collision-proof output allocation; survive the parent-death ownership gap. |
| Integrated GPU cannot run a chosen model/training fixture | Workload capability/fit checks and explicit failure; use two suitable devices for the acceptance claim, never substitute CPU work as GPU proof. |
| Concurrency slows all jobs | Measure CPU/RAM/storage contention. Keep the simple one-job-per-device policy; add host-resource limits only when measurements justify them. |
| Existing interactive features are accidentally removed | Keep the foreground path; explicitly label scheduled monitoring rather than pretending worker memory is local UI state. |
| Other applications consume the GPU | State the local-scheduler scope; report OOM/load, do not promise machine-wide reservation. |

Deferred by design: remote live native-viewport attachment for scheduled workers, custom HTTP control endpoints, automatic load balancing, multi-process GPU packing, auto-retry, and external-tool GPU mapping. The initial queue needs none of them to execute independent processes on explicit devices correctly.

## 8. Completion definition and later distributed-training gate

This milestone is done when:

1. Two independent native jobs genuinely execute concurrently on two explicitly chosen GPUs.
2. Same-device work is serialized, unrelated devices make progress, and foreground ownership is respected.
3. Later jobs/phases can choose a different GPU without restarting the desktop or changing a live `EngineState` device.
4. Stop, crash, restart, output collisions, missing devices, and multiple pending recoveries are safe and observable.
5. Native workflows reuse their existing implementations; both desktop and headless surfaces satisfy their stated acceptance rows.
6. Commands/results and remaining limitations are recorded honestly; no distributed-training capability is implied.

**Distributed training remains a later, separate proposal.** Only revisit it after this scheduling milestone is tested and proven and profiling shows that a single training job on one GPU remains the limiting requirement—not lack of job concurrency, preprocessing throughput, host I/O, memory pressure, or avoidable single-GPU work.

Use existing `SS_PROFILE=1` stage/kernel timing and bounded benchmarks on representative scenes. Record hardware, scene/config, model size/splat trajectory, GPU memory, host/device time, data movement, and end-to-end latency/throughput. Distinguish throughput from running more independent experiments versus latency of one training run. Require an explicit target and evidence that simpler single-GPU optimizations cannot meet it before authorizing distributed design.

That future plan must separately address dynamic splat creation/deletion, stable ownership/IDs, optimizer moment and gradient synchronization, reproducibility/numerical behavior, partition/load balance, checkpoint/recovery, and communication cost. None of those structures or dependencies should be added speculatively to this scheduler.

## 9. Implementation status and evidence

The implementation landed on `feature/device-job-scheduling` in commits
`00f25860` through `0c4d590a`. It provides:

- one durable local scheduler with per-device admission, workspace/run leases,
  fixed dataset and training phases, atomic state recovery, and bounded event
  delivery;
- one phase worker protocol for native preparation, built-in SfM, geometry, and
  training, using the existing preparation and training implementations;
- process-tree supervision, cooperative stop versus forced termination, output
  lease handoff, and parent-death handling;
- native dataset and batch submission in the GUI, distinct lifecycle states,
  pending target selection, per-job retry/Later recovery, and all catalog copy
  in the 13 supported languages.

The Windows Vulkan integration build passed for `spirula`, `scheduler_test`,
`worker_request_test`, and `subprocess_test`. The three executables then passed:

- `scheduler_test`: output/state ownership, schema migration, path-claim
  conflicts, same-device serialization, unrelated-device progress, foreground
  reservations, pause/resume, persistence, recovery, and failed-publication
  behavior;
- `worker_request_test`: request validation, path freezing, preparation payload
  round-trip, and rejection of external Python masking;
- `subprocess_test`: environment isolation, line capture, cooperative and
  forced stop, spawn failure, and inherited output-lease lifetime.

The rebuilt Vulkan GUI target also passed the comment/i18n/font/build gates
after the integration cleanup. A fresh `SS_BUILD_GUI=OFF`,
`SS_BUILD_SFM=ON`, `SS_BUILD_SAM=ON`, `SS_ENABLE_PATENTED=OFF` configuration
built `spirula` successfully, and its `--help` command exited successfully.

### Acceptance still open

The milestone is not complete until these external/manual rows are exercised
and recorded:

- the real desktop Phase 4 gate: preserve a live foreground device, run two
  independent jobs, retarget pending work, stop one, restart, retry, and verify
  Later plus mask-overlay color correctness;
- real preparation, SfM, geometry, bounded training, evaluation, checkpoint,
  and run-directory resume fixtures with the required accepted model weights;
- overlapping native training on two suitable physical GPUs, followed by the
  serial-versus-scheduled makespan measurement;
- POSIX process-tree/parent-death validation and the remaining startup,
  mid-save, disk-full, Unicode-path, and state-publication fault windows;
- the optional patented native-video row in a separately opted-in build.

The rebuilt headless binary's `sam devices` command listed an NVIDIA GeForce
RTX 3060 and AMD Radeon(TM) Graphics as usable. That is inventory only: it does
not prove that both fit the required models, that GPU work overlapped, or that
any Phase 5 workload passed.
