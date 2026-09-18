# GPU controls and scheduled dataset previews — review handoff

## Status and scope

Nonvisual remediation and Vulkan verification are complete. The two known GUI state gaps below are fixed, and desktop validation has resumed with the partial results recorded below. This is **not** a desktop acceptance sign-off or completion of the broader scheduling plan.

Branch: `feature/device-job-scheduling`. Review baseline: `d76f7318` (`fix(gui): restore scheduled dataset previews`), following `f2becb97` (`fix(gui): compact dataset GPU controls`). The broader plan is `docs/notes/device-job-scheduling-plan.md`.

## Next session

Continue **native desktop GUI validation and remediation**, not another headless-only review. Finish the unchecked desktop cases below, especially the matching view, inverted live overlay, failed/stopped logs and busy-training dataset handoff. Actual visible behavior is the acceptance gate.

The review and GUI state fixes are committed on this branch, together with this handoff and generated-artifact cleanup. Continue from the current branch tip; no separate uncommitted review patch is required.

Desktop input and capture worked after the Spirula window was foregrounded. The small reconstruction runs can complete while a click action is still waiting for the application to become responsive, so use a longer-running dataset for live-stage captures.

## Relevant files

- `src/app/gui/GuiApp.cpp` / `.h` — scheduled monitor routing, recovery modal, dataset completion and pending open state.
- `src/app/gui/SfmRunner.cpp` / `.h` — direct execution and scheduled argument production; provisional mask absence differs from explicit disabling.
- `src/app/JobScheduler.cpp` — completed prep output binding, explicit mask precedence and all-remaining-phase retargeting.
- `src/app/DatasetPrep.cpp` / `.h` — shared preparation, planned/actual image and mask outputs, polarity.
- `src/app/gui/SfmProgress.cpp` / `.h` — common status mapping and current-attempt readiness.
- `src/app/gui/FeatureWatcher.cpp`, `PairPreview.cpp`, `Picture.cpp`, `FilmReel.cpp` — preview discovery, live/final match freshness and mask rendering.
- `src/i18n/catalog/Dataset.h` — translated GPU help.
- `src/core/tests/scheduler_test.cpp`, `src/sfm/tests/sfm_live_matches_test.cpp`, `src/sfm/tests/sfm_manifest_test.cpp` — retained regressions.

## Remediated findings

- Job GPU help now says that new jobs capture the selection **at submission**, not at start. All 13 translations were updated; compact control layout and Desktop GPU semantics were retained.
- Scheduled previews consume completed preparation outputs and frozen preparation metadata, rather than mutable GUI source/import settings or a guessed workspace mask folder.
- Preparation and scheduled argument generation share path/polarity planning. Untouched external masks retain their convention; gathered/stencil-folded masks use the prepared convention. Feature enumeration excludes nested masks, and feature/pair pictures honor mask polarity.
- Actual preparation outputs bind the SfM request at dispatch, including masks derived from RGBA photos. Both `--masks` and `--mask-dir` overrides are consumed before canonicalization. Explicit `--no-masks` wins even when preparation publishes masks; absent masks also remove inversion. Provisional scheduled arguments do not mistake unknown outputs for an explicit disable.
- Explicit `--no-masks` overrides manifest masks as well as sibling discovery.
- Retargeting a queued job updates every remaining phase, without changing completed phase targets or the foreground reservation. Future targets survive scheduler reload.
- Scheduled workers initialize both the progress directory and status-event sink. Old progress snapshots are cleared before publishing the current attempt ID in `.progress/attempt`. Readers reject old/partial/missing attempt IDs; queued/prep/retried jobs cannot authorize old SfM snapshots. Reusable matches and resume artifacts are preserved.
- Direct and scheduled runs share status-to-progress logic. Scheduled stage rows no longer read idle direct-run progress. Preview selection handles feature loading and pair selection separately from mapping.
- Pair previews prefer the freshest usable live/final source, refresh a hovered pair as matches arrive, preserve final-file tie precedence, and reject results from superseded worker configurations.
- Starting a direct run retires the previous scheduled monitor ID. Dataset opening distinguishes an omitted mask path from an explicitly empty one, so completion does not inherit a previous mask-directory default.
- Existing dedicated scheduler log routing was retained; no second scheduler or preview implementation was added.
- Recovery retry now clears stale preview state and returns to the New Dataset screen, where the normal scheduled polling path restarts feature, pair and model monitoring.
- Completed direct reconstruction handoff now passes image, mask, polarity and log-retention metadata through the shared deferred-open path when training must stop first.

## Verification performed

Run build commands from a Windows developer environment with MSVC, CMake and Ninja available.

| Check | Observed result |
|---|---|
| `build_develop.bat -DSS_BACKEND=vulkan` | Passed, including the comment gate and final GUI executable link. |
| `build/scheduler_test.exe` | All checks passed, including multi-phase retargeting, persisted future targets and scheduled mask-override regressions. |
| `build/worker_request_test.exe` | All request-contract checks passed. |
| `build/sfm_live_matches_test.exe` | Passed: three pairs, 152 truncated live-file prefixes, progress reset/preservation checks. |
| `build/sfm_manifest_test.exe` | Passed. The new no-mask precedence assertion failed against the pre-fix library and passed after rebuilding. |
| `python tools/check_font_coverage.py` | 10,994 characters across five fonts; none missing. |

After the two GUI state fixes, `build_develop.bat -DSS_BACKEND=vulkan` and the four targeted test executables above passed again. `git diff --check` and `python tools/check_comment_length.py` also passed.

The new scheduler regressions failed against the pre-fix scheduler on future-phase retargeting, persisted future targets and explicit mask disabling, then passed after integration.

Additional temporary native smokes exercised actual production code, not source-text assertions:

- RTX 3060 worker, 40 photographs: the pre-fix worker produced no progress directory. The repaired worker published status, live matches, pair matrix and model snapshots **before exit**, registering 40/40 images and 6,280 points. Exit 3 correctly retained the partial result; this is not a metric-quality acceptance claim.
- The same worker started with four stale snapshots and an old attempt marker. The production reader rejected that marker. Old model/pair snapshots were gone before the new marker became readable, and the production reader accepted the current attempt.
- Earlier GPU integration smoke: actual scheduler prep → SfM → publish with four RGBA photographs and a stale trailing `--mask-dir` succeeded. SfM consumed generated masks for 4/4 images and removed 24 masked keypoints. Final provisional-versus-explicit precedence was then checked through the combined submission smoke below.
- Combined submission smoke linked against the final application objects: `SfmRunner::scheduler_args()` → real RGBA preparation → scheduled SfM requests. Implicit masks selected the actual generated directory; explicit `--no-masks` survived and suppressed masks. Reconstruction intentionally stopped on an invalid flag before GPU work. The old argument producer failed the provisional-mask assertion.
- Shared progress helpers: raw stages 0–9, load/select preview choices, count/fraction transitions, and missing/old/partial/current/extra-token attempt markers passed.
- PairPreview/Picture native smoke: same-hover live append, newer final-file handoff, timestamp ties, and normal versus inverted mask RGB passed.
- Preparation planning and canonical mask-argument smokes covered in-place external masks, stencil folding, gathered masks, absent masks, both aliases, malformed values and unrelated arguments.

Vulkan is the acceptance target; no CUDA validation was performed.

Desktop verification on the final binary:

- Recovery `Later` dismissed the prompt for the session and the same interrupted job returned after restart. `Retry job` restored the New Dataset screen, resumed SfM on the RTX 3060, advanced visibly through Features and Mapping, completed, and opened 152 cameras / 23.2k points in the trainer.
- Desktop GPU and Job GPU rendered as compact controls with right-hand labels. Changing Desktop GPU to AMD left Job GPU on Auto; Desktop GPU was restored to Auto. Both tooltips rendered in English and Japanese, including the Job GPU submission-time wording.
- Scheduler output stayed in the bottom text area during the resumed run and after completion/handoff.
- A disposable 20-image beside-source mask dataset completed on the desktop. The source row reported `photo folder + masks`; the completion log reported masks for 20/20 images and 107,088 keypoints removed (65.4%), then opened 20 cameras / 3.6k points in the trainer.
- A repeated 40-image run visibly showed the Features preview with a translucent red overlay on the removed black half and green keypoints on the retained half. It completed with masks for 40/40 images and 214,176 keypoints removed (65.4%).
- The inversion control was visibly enabled for a fresh output. That run completed with masks for 40/40 images, 113,504 keypoints removed (34.6%), and opened 40 cameras / 2.1k points. The changed count confirms the inverse convention reached reconstruction; the inverted live overlay was not captured before the run completed.

## Desktop pass still required

- [x] Validate both localized tooltips, compact widths/right-hand labels, and Desktop versus new-job GPU independence on the final binary.
- [ ] Observe Features → matching matrix/pair hover → live model, including loading/selecting stages, retries, queued/prep waiting, and a failed scheduled run followed by a direct run.
- [ ] Validate beside-source/in-place masks, normalized and inverted masks, overlay color correctness, and masked-to-unmasked completion in the trainer. Beside-source normal overlay/color and both completion polarities passed; in-place masks and a live inverted overlay remain.
- [ ] Confirm logs stay in the bottom textarea across queued/running/failed/stopped/completed transitions. Running and completed/recovered handoff passed; failed/stopped remain.
- [x] Desktop-check recovery restart/retry and Later without disturbing foreground work. The code restores `Screen::NewDataset` and resets the monitor state on retry.
- [ ] Desktop-check the direct completion confirmation path while training is busy. The deferred open now retains image/mask paths, mask polarity and reconstruction log placement.

The larger plan also still lacks full pending dataset/phase retarget controls and its two-device, restart/recovery, throughput and fault-window acceptance. This handoff does not close those milestones.

## Environment and workflow notes

- Desktop capture/input recovered after the user foregrounded the window and remained usable through the final run. Capture did not sample the live inverted stage because the short run completed before the click action returned.
- The disposable desktop-test dataset was deleted after the user authorized cleanup and its location was verified within the user temporary directory.
- One stock scheduler-test invocation timed out at 180 seconds. The unchanged binary subsequently passed seven runs in about 1.3 seconds, and the final rebuilt binary passed again. No root cause was established and no speculative mutex/runtime fix was made.
- `vswhere` discovery is broken locally; using the installed MSVC developer environment allowed the normal build script to succeed.
- An isolated merge produced an automatic newline-only stash and displaced the original untracked handoff. Its content was recovered before updating this document; the verified session-created stash was removed. No user stash was present at the initial baseline.
- A later index-lock failure required manual integration of the preserved scheduler lane. The inactive lock was quarantined after checking process/file holders; all 13 substantive Main WIP paths were restored and compared with the automatic stash before it was dropped. Unrelated newline-only changes were not applied.
- Generated root artifacts `^` and `job_scheduler_mask_smoke.obj` are removed from the branch tip; neither is present in the net patch from the review baseline.
