# Keep the GPUs fed through mask publication (rank 1, PERF-020/021)

Status: revision 2, 2026-11-16, fork-only plan on branch `plan/storage-latency`. Revision 1 failed an expert review (Claude Fable); this revision adopts all eight findings (see "Review record"). Planning only; the executing session's prompt grants authority. The ranking it belongs to is [storage-latency-plan.md](storage-latency-plan.md). Line numbers refer to `upstream/master` at `8949092e`.

## Goal

Rank 1 of the storage-latency review: *mask results are accepted inside the dispatch mutex on the worker's own thread, and mask scratch lives in the workspace on the share.*

**Done:**
- With the workspace and images on an SMB share, the fixture's isolated-worker mask stage is **≥20% faster at the median**, and B beats A in each of three A,B alternations.
- With a local workspace, B's median is not slower than A's observed range.
- Per-GPU busy is reported for both.
- `masks/` and `feature_masks/` are byte-identical between A and B.
- Every listed test passes, including the new dispatch cases.
- One code review of the diff passes.
- The work branch is ready for an upstream PR, which is not opened.

**Non-goals:**
- `src/app/cli/geometry_main.cpp`, which the multi-GPU geometry plan owns;
- the serial pre-hash before workers start (PERF-022, rank 2), next in this lane;
- worker-side finalization (A3);
- PNG compression changes;
- default workers per GPU;
- new settings or UI text;
- temporal (video-mode) masking, which runs one task and so one worker by design;
- NVIDIA/CUDA;
- patented decoding.

**File ownership:** `src/app/InferenceWorker.{h,cpp}`, `src/app/tests/inference_worker_test.cpp`, and the isolated-mask section of `src/app/gui/DatasetPrep.cpp` (lines ~3700-3950).

## Branches

| Branch | Base | Holds | Goes upstream? |
|---|---|---|---|
| `plan/storage-latency` (origin) | fork `main` | This plan and the ranking. The execution record is updated here | Never |
| `perf/mask-publish-pipeline` (create) | `upstream/master` | Only the `src/` changes and tests from P1/P2 | Yes, later, as one PR |

- The fork's `AGENTS.md` (`git show origin/main:AGENTS.md`) governs the executing agent. The work branch carries upstream's own `AGENTS.md`; leave it untouched.
- Never commit plan files, fork instructions or measurement logs to the work branch.
- Before starting, check that nothing this plan cites moved:

  ```text
  git diff --stat 8949092e upstream/master -- src/app/InferenceWorker.h src/app/InferenceWorker.cpp src/app/gui/DatasetPrep.cpp src/app/MaskWorker.cpp src/app/gui/FilmReel.h src/app/gui/FilmReel.cpp src/sfm/core/Mask.h src/core/AtomicFile.h src/app/tests/inference_worker_test.cpp
  ```

  Any output means the anchors must be re-verified before P1.

## Evidence

**Production rates.** MEASURED from the production prep log, 2026-11-15, release 2026.11.12; two R9700s, one worker each:
- 3,332 masks took 2,834 s.
- Frames: 890 at 5760×3240, and 2,430 dual-fisheye lens frames at 3840².
- The inference period (first model load to last mask) is 2,759 s; excluding the 188.6 s pass boundary (rank 2), ≈2,570 s.
- GPU work per frame is `encode_image` (mean 207 ms, `src/sam/model/ImageEncoder.cpp:292`) **plus** `segmentConcept` (0.26–0.29 s, `src/sam/pipeline/Session.cpp:494`), ≈0.48 s in all.
- At ~1.3 frames/s across 2 GPUs, each GPU is ≈31% busy (DERIVED).
- The GPU-bound floor is ≈3,332 × 0.48 / 2 ≈ 800 s plus setup, so this plan's ceiling is ≈1,750 s (DERIVED).
- A local-NVMe benchmark with smaller frames and four workers keeps masks 88–93% busy.

**Per-frame path** (code, verified):

| Step | Where | Share traffic today |
|---|---|---|
| Parent hashes every source serially before any worker starts | `DatasetPrep.cpp:3771-3777` | Full read per source (PERF-022) |
| Worker task: one-ahead prepare (source SHA-256 + decode); GPU; staged `train.png`/`feature.png` via `WriterPool`; drains all writes before replying. The first frame of each task is not overlapped | `MaskWorker.cpp:159-217` | Source read; two staged PNG writes |
| Dispatch: claim under `mutex`; send/receive unlocked; then `accept` **on the worker's own dispatch thread under the same `mutex`**; bytes released after accept; only then the next claim | `InferenceWorker.cpp:133-160` | — |
| Parent accept per frame: EXIF turn; probe; **full SHA-256 of the source**; `stbi_load` of both staged PNGs; `StencilRaster::apply`; `turn_pixels` ×2; **`stbi_write_png` ×2 at 17 MP (level 8)** into scratch; `fs::remove` + `fs::rename` ×2; filmstrip; progress | `DatasetPrep.cpp:3832-3940` | Source re-read; two staged reads; two writes; two removes; two renames |
| Filmstrip: `wants()` is true whenever ≥0.12 s has passed; `add_loaded` decodes the source and both masks **on the calling thread** | `FilmReel.cpp:18-27, 64-73`; `DatasetPrep.cpp:3922-3930` | Effectively every frame: source plus two masks read inside the lock |
| Scratch `<workspace>/.spirula-mask-attempts/<nonce>-<attempt>/k/` lives until the stage ends, then `remove_tree` | `DatasetPrep.cpp:3742-3747, 3942` | ~3,332 folders deleted over SMB. The 47.4 s of silence after the last mask fits this [INFERENCE] |

**Consequences:**
1. Each worker idles while its own task is accepted.
2. Acceptances from all workers serialize.
3. No worker can claim a task during any acceptance.

**Admission today** (`DatasetPrep.cpp:3795-3829`; `InferenceWorkerDevices.h:74-77`):
- Budget = `max(4 GiB, physical RAM / 2)`.
- Per worker = `2 × (model + detector bytes) + 16 MiB`. SAM 3 q4_0 is 706,606,590 B with no separate detector, so ≈1.33 GiB.
- Per task = `256 × max(source pixels, max(1024, max_size)²)`, the maximum over its frames.
- On the reference workstation (31.2 GiB RAM): task budget ≈15.59 − 2 × 1.33 ≈ 12.9 GiB. A 5760×3240 task costs 4.45 GiB and a 3840² task 3.52 GiB.
- One task per worker fits; a third task does not (3 × 4.45 = 13.35 GiB). **Pipelining needs a smaller charge while a result is accepted** (D1).
- Recompute for the executing machine's RAM. Above ~64 GiB, three tasks fit even without D1's change, and test 5 still guards the accounting.

**State the callback owns, which must stay serialized:**
- `done`, `progress`, `_masks_tally`;
- filmstrip appends (internally locked, `FilmReel.cpp:75-81`);
- `StencilRaster::_cache`, which `apply` mutates (`DatasetPrep.cpp:439-460`). Its rasters are `std::map` nodes, so references stay stable.

**Indexing hazard:** `sfm::MaskIndex::buildIndex` claims every regular file under `masks/` after stripping one or two extensions (`src/sfm/core/Mask.h:298-325`). Temporary files must never live under `masks/` or `feature_masks/`.

**Dispatcher contract** (`InferenceWorker.cpp:83-174`; tests `src/app/tests/inference_worker_test.cpp:59-175`):
- callbacks are serialized;
- exactly-once;
- acceptance order = response order;
- bytes reserved through acceptance;
- first failure wins;
- cancellation kills children and overrides the error;
- no rollback.

There are two production callers: masks and geometry (`geometry_main.cpp:1458-1490`).

## Hypotheses and the measurement that decides each (P0)

| ID | Hypothesis | Decided by |
|---|---|---|
| H1 | Serialized parent acceptance CPU (17 MP PNG decode/encode, filmstrip decode) limits throughput | Per-step accept timings |
| H2 | SMB round trips (staged write/read/rewrite, source re-reads, scratch deletion) add to it | Same build and fixture, with workspace **and images** on the share vs local |
| H3 | Worker-side host time (cold first frame per task, write drain) limits it | Round trip − (`encode_image` + `segmentConcept`) per frame |
| H4 | Admission refusals idle a worker | Claim decisions logged with `resident` |
| H5 | One worker per GPU is the limit | One `SS_WORKERS_PER_GPU=2` arm; not a fix by itself |

## Design (each part applied only where P0 confirms it)

**D1: pipelined dispatch for callers that supply a `prepare` step** (`InferenceWorker.{h,cpp}`).

*Admission.*
- `WorkerTask` gains `accept_bytes`: the parent's footprint while accepting. It defaults to `host_bytes`, which reproduces today's accounting.
- On a valid response, release `host_bytes − accept_bytes` under the dispatch mutex. The child has already dropped the task's images before replying (`MaskWorker.cpp:200-216`).
- Masks charge 8 B/px of the task's largest frame. Two masks plus the PNG encoder are ≈4–6 B/px [INFERENCE]; P0's peak commit checks this.
- Two pipelining workers on 5760×3240 then need 2 × (4.45 + 0.14) ≈ 9.2 GiB of the 12.9 GiB budget.

*Pipelining.*
1. After that release, try to claim the next task **without waiting**. Only if one fits now, send it to the same child.
2. Run the caller's `prepare(i, response, why)` with no dispatcher lock.
3. Run `accept(i, response, why)` under a separate `accept_mutex`.
4. Release `accept_bytes` and read the next response.

If nothing fits, the order is exactly today's: accept, release, blocking claim. A thread never waits for budget while it holds an unaccepted result.

*Without `prepare`,* dispatch is today's exact sequence: `accept` under the dispatch mutex, no early release, no early claim. **Geometry dispatch is unchanged.**

*Gating.*
- Check `failed` and `cancel` before `prepare`, and again under `accept_mutex` before `accept`.
- The first failure from either step wins. The in-flight request is abandoned, and the cancellation thread kills the children as today.

*Contract after D1.* Exactly-once, serialized callbacks, bytes covered through acceptance, no rollback. Acceptance follows response order within a worker. Across workers it follows `prepare` completion; both callers only count and log.

**D2: mask callback split** (`DatasetPrep.cpp:3831-3941`).

*`prepare`:*
- check the reply and the output manifest;
- check the source: turn, probe, SHA-256;
- load the staged masks;
- `StencilRaster::apply`, with a mutex around its cache find/emplace only;
- `turn_pixels`;
- encode the finals into the workspace scratch `<workspace>/.spirula-mask-attempts/<nonce>-<attempt>/<k>/`. That is today's location and volume, never under `masks/`.

*`accept` (commit):*
- cancel check;
- publish each final with `spirula::replace_file` (`src/core/AtomicFile.h:15-32`), the repository's atomic replace, which also retries the GUI's transient readers. This removes today's remove-then-rename gap;
- delete `<k>`;
- `++done`; `progress.update`.

*Filmstrip:*
- In commit, call `wants(1.0)`. When it returns false, register path-only with `add`.
- When it returns true, call `add_loaded` **after** leaving `accept_mutex`, on the dispatch thread. The next task is already in flight.

Pixels and the encoder are unchanged, so output bytes are unchanged.

**D3: worker staging off the share** (same function).
- Manifests (`task-N.jsonl`, `.outputs`) and the worker's staged `train.png`/`feature.png` move to `fs::temp_directory_path() / "spirula-mask-attempts" / <nonce>-<attempt>`. The system temp dir is already used for LUTs, HEIF conversion and previews.
- The parent deletes each frame's local stage folder after its commit.
- Both scratch roots are removed at the end and on failure, as today.
- The share then carries only final writes, replaces and per-frame folder deletes; the end-of-stage bulk delete disappears.

**D4: larger mask tasks** (if H3 holds; after D1).
- Group 8 frames instead of 2 for non-video tasks (`DatasetPrep.cpp:3782`), which amortizes the worker's per-task tail (cold first decode, final write drain).
- Memory stays constant, since the per-task charge is the maximum over frames.
- Costs: progress arrives in bursts of 8, and a failure discards more in-flight work.

**Alternatives:**

| Alternative | Why not now |
|---|---|
| A1: unserialized acceptance, each caller locking its own state | Needs `geometry_main.cpp` edits (other plan) |
| A2: one publication thread with a queue | Heavy work stays serial; throughput capped at 1 / accept time |
| A3: the worker finalizes (stencil + inverse turn), and the parent verifies header + hash, then renames | Removes 2 decodes and 2 encodes per frame from the parent but weakens its decode check. Next lever if D1–D4 leave the parent CPU-bound |
| A4: more workers per GPU | Does not touch the serialized section; measured 12.5% slower for masks on one GPU |
| A5: lower PNG compression | Changes bytes |
| A6: final temp files beside destinations | Rejected: the SfM mask index would load a truncated `.part` after a crash |

## Phases

### P0: measure (~4 h, mostly machine time)

**Builds.** Make a worktree of `perf/mask-publish-pipeline` and build it:

```text
build_develop.bat -DSS_BACKEND=vulkan
```

Patented stays OFF. Add a **throwaway** timing patch and save it as `p0-timing.patch` beside the run logs; never commit it. Per task it logs, through the prep log:
- child id;
- each claim decision with `resident`;
- claim wait and round trip;
- accept total and its steps: source checks, staged loads, stencil+turn, encodes, publish, filmstrip.

**Fixture.**
- About 120 frames in two camera folders: 60 at 5760×3240 and 60 dual-fisheye lens frames at 3840². Keep each camera's frames in its own subfolder: border detection and stencils run per folder.
- Preferred source: frames from the production capture, supplied out of band (private; never committed).
- Fallback: real photographs of those sizes. Then compare A/B only, not against the production numbers.
- Make one copy on local disk and one in a writable SMB test folder.

**Mask settings.** These are the production preset's:

| Dataset-preset key | Value |
|---|---|
| `mask_enable`, `mask_features`, `mask_border` | `true` |
| `mask_model` | `sam3-q4_0` (fetched by the GUI after its licence prompt; no separate detector is loaded) |
| `mask_prompt` | `person; shadow of a person; reflection of a person;` |
| `mask_negative_prompt` | `poster;` |
| `mask_feature_prompt` | `sky; cloud; water; wave` |
| `mask_keep_subject` | `false` |
| `mask_dilate_ratio`, `mask_shrink_ratio` | `0.05`, `0` |
| `mask_max_image_size` | `1600` |
| `mask_threshold`, `mask_nms`, `mask_box_threshold` | `0.5`, `0.1`, `0.3` |
| `mask_memory`, `mask_detect_every` | `false`, `1` |
| Additional GPUs (GUI) | Auto |

**Driving the GUI.**
- Run with `APPDATA` pointed at an empty scratch folder, so the machine's own `spirula-studio` settings are untouched. `LOCALAPPDATA` keeps the model cache.
- Use `SS_GUI_AUTOMATION=1` with `tools/guictl.py` (`docs/notes/gui-automation.md`), or drive it by hand.
- Import the fixture folder as photos and prepare it with masking on. The measured window runs from `Found N images` to the last `images masked` line in `<workspace>/logs/prep_*.log`; later stages may be cancelled.
- No CLI masks a dataset.

**Probes per arm.**
- Per-GPU busy: sample Windows `\GPU Engine(*)\Utilization Percentage` once a second (for example with `typeperf`). Sum per adapter LUID.
- Per-process peak commit.
- Number of tasks per child; both children must have replied.
- For share arms, a 30 s sequential read of a fixed file plus 100 cold small-file opens, before and after, to catch server drift. The reference share measured ~7.5 ms first-open and ~159 MiB/s.

**Arms.** Local and share, alternating L,S three times; then one share arm with `SS_WORKERS_PER_GPU=2`.

**Decisions:**

| Go | When |
|---|---|
| D1+D2 | Serialized acceptance time is ≥30% of the inference period on either location |
| D3 | The share median is ≥15% slower than local |
| D4 | The per-task worker tail is ≥15% of the worker cycle |
| Stop | None holds. Record which hypothesis holds; this is a result, not a failure |

### P1: D1 (~0.5 day)

New `inference_worker_test` cases:
1. With `prepare`, a slow acceptance does not delay sending the next request to that worker (fails today).
2. Accepts never overlap.
3. Prepares of different workers overlap (fails today).
4. A failing `prepare` or `accept` stops later accepts and fails the run.
5. With budget = 2 × host + accept, pipelining engages, and admission never exceeds the budget (fails without `accept_bytes`).
6. Cancellation during a slow `prepare`, with a budget large enough to pipeline, ends the run as canceled.
7. Without `prepare`, the sequence is today's: the next request goes out only after `accept` returns.

No test asserts cross-worker commit order. The existing cases stay green unchanged.

### P2: D2 + D3, then D4 if P0 asked for it (~0.5–1 day)

- `mask_worker_parity_test`, `mask_worker_manifest_test` and `dataset_prep_test` stay green.
- No temporary file is created under `masks/` or `feature_masks/`; record this as a code read in the report.

### P3: prove it (~3 h)

- **A/B.** A = the work branch's base (`upstream/master`); B = the work branch. Build each in its own worktree. Alternate A,B three times, one worker per GPU, on the share and locally.
- **Report:** medians, ranges, per-GPU busy, peak commit per process, share probe rates. Apply the Done thresholds.
- **Parity:** `masks/` and `feature_masks/` byte-identical between A and B.
- **GUI path:** two device UUIDs in the log; the filmstrip still follows during masking (now at most once a second).
- **Full stage:** only with the owner's approval and data.
- **Review:** one code review of the P1+P2 diff, focused on concurrency.

**Expected** [INFERENCE, optimistic until P0]: a production mask stage drops by 800–1,200 s (ceiling ≈1,750 s). A3 is the next lever if the parent stays CPU-bound.

### P4: prepare the upstream PR (not opened)

- Rebase the work branch onto the current `upstream/master` and rerun the checks.
- Build once with `-DSS_ENABLE_PATENTED=ON`, as upstream CI does (`.github/workflows/ci.yml`), then reconfigure back to OFF.
- Follow the fork `AGENTS.md` sections "Upstream PRs: reproducible tests and fixtures" and "Upstream PR descriptions". Read `docs/testing.md` on `upstream/master`.
- Draft the PR title and body in the plan's execution record: problem, fix, the P3 numbers without capture names or machine paths, the test command, and limitations.
- `bash tools/check_private_paths.sh` passes on the work branch.

## Risks

- **Early release** assumes the child's per-task memory is free once it replies; P0's per-process peak commit checks this. If wrong, `accept_bytes` falls back to `host_bytes` and pipelining engages only where three tasks fit.
- **CPU contention:** the parent's prepares run on two dispatch threads beside two workers × `hardware_threads()/2` threads each. Watch whole-system CPU.
- **`replace_file` on SMB:** each replace is write-through, so its cost lands in the serialized commit; P3 records commit time. Falling back to remove + rename is a one-line change.
- **Filmstrip:** pictures refresh at most once a second during masking (it was up to ~8/s).
- **Temp space:** bounded by in-flight frames.
- **Geometry:** unaffected, since it passes no `prepare`. Its timing runs should still record the dispatcher commit. Do not land D1 between the geometry plan's A and B arms.
- **Local workspaces:** must not slow down; the local arms check this.
- **Machine differences:** RAM changes the admission arithmetic, and GPU count changes worker count. With one GPU, use `SS_WORKERS_PER_GPU=2` for two workers and say so in the results.

## Checks

The agent runs these on the work branch after `build_develop.bat -DSS_BACKEND=vulkan`:

```text
ctest --test-dir build_vulkan -R "^(inference_worker_test|inference_worker_devices_test|mask_worker_parity_test|mask_worker_manifest_test|dataset_prep_test|subprocess_lifecycle_test)$" --output-on-failure --no-tests=error
```

If ctest registers none of them, run the executables from `build_vulkan` directly. The comment, UI-text and i18n checks run inside the build; no new user-visible strings are expected.

## Review record

Revision 1 → 2, Claude Fable: verdict FAIL. All findings were accepted after checking the cited code.

1. **Pipelining never engaged under the real budget.** Added `accept_bytes` early release, test 5, and `resident` logging.
2. **GPU work per frame omitted `encode_image`.** Busy is 31%, not 18%; floor ≈800 s; ceiling ≈1,750 s.
3. **`.part` files under `masks/` get indexed** (`Mask.h:298-325`). Finals stay in workspace scratch; only worker staging moves to temp (A6 rejected).
4. **Done had no number.** Now ≥20% at the median, B beats A in every pair, local not slower.
5. **Ownership and geometry confounding.** This plan owns the dispatcher files; geometry dispatch is unchanged without `prepare`; added a sequencing rule.
6. **Contract wording.** Cross-worker order reworded; test 6 budget ≥3; test 7 added.
7. **P0 validity.** Images on the share in share arms; three alternations; drift probe; per-child task counts.
8. **Filmstrip cost.** `wants(1.0)`, and `add_loaded` outside the commit lock.

Fable's alternative (larger groups) became D4, conditional on H3. Added beyond the review: `spirula::replace_file` instead of remove-then-rename (vetoable).

## Execution record

2026-11-16: P0 started. Fetched `upstream/master`; work branch
`perf/mask-publish-pipeline` starts exactly at `8949092e`. The anchor diff is
empty. Original checkout edits are preserved; no push or PR is authorized.

The owner approved creating an isolated SMB test folder and copying 60 frames
from each production camera family; originals remain read-only. Fixture copies
contain 120 JPEGs (536,942,206 bytes), with SHA-256 parity verified between local
and SMB copies. Fisheye frames retain separate lens subfolders. Two R9700s,
31.18 GiB RAM, accepted/cached SAM 3 q4_0 and approximately 94 GiB local free
space are available. No Spirula process was running; sampled CPU load was 0%.

The owner selected GPT-6.1 Sol high for execution workers. Temporary P0 timing
instrumentation is in progress; no performance gate has passed and no permanent
optimization is authorized by measurement yet. Private fixture paths and hashes
remain in the local scratch evidence directory, not the work branch.

## Execution handoff

> **Goal and active phase:** keep both GPUs busy through mask publication on SMB workspaces (PERF-020/021). Start at P0; P1/P2 apply only the designs P0 confirms; P4 readies, but does not open, an upstream PR.
>
> **Acceptance and evidence:** the Done section above. Results, commands, device UUIDs and verdicts go in this file's execution record on `plan/storage-latency`. Bulky logs stay on the executing machine; summarize them.
>
> **Authority and exclusions:** set by the owner's prompt to the executing session. This plan assumes:
> - allowed: worktrees, local builds (patented OFF except the one P4 CI-parity build), GPU runs on fixture copies, commits on `perf/mask-publish-pipeline` and `plan/storage-latency`;
> - pushes to `origin` only if that prompt allows them;
> - not allowed: anything to `upstream`, opening a PR, `geometry_main.cpp`, default changes, new settings or UI text, writes outside the test folder on the share, the machine's own Spirula settings, NVIDIA.
>
> **Prerequisites:**
> - ≥2 non-NVIDIA Vulkan GPUs (the reference is 2× R9700 32 GB); one GPU works with the caveat under Risks;
> - an idle machine;
> - a writable SMB test folder (~3 GB);
> - the fixture;
> - SAM 3 q4_0 accepted and cached through the GUI;
> - ≥10 GB free on the local disk.
>
> **Stop/escalate:** after two failed attempts on one problem, climb `task` → `expert` → `expert-max` → owner. Ask the owner before any default change, a full-stage run on production data, or a scope change. P0 refuting every design ends the plan with a result.
>
> **Readiness:** ready once the prerequisites exist on the executing machine.
