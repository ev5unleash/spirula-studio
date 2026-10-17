# Multi-GPU training plan

**Rewritten 2026-10-16 for speed.** Everything under
[Frozen ledger](#frozen-ledger-2026-09-29--2026-10-16-history-not-instructions)
is history: its "current authority", budgets, ceilings, admissions and
approvals are superseded by this section wherever they differ. Do not append
to it, and do not read it unless a specific measurement is needed.

## Goal and definition of done

Decide whether two-GPU block training beats one GPU on the selected dataset,
and leave a usable headless path behind. Done means exactly one of:

- **selected:** a B2 schedule, three repeats, passes the quality rule and the
  timing rule below against ordinary native at step 20,000 from a
  step-10,000 source, **and** passes the recovery check (step 6); or
- **experimental win:** same, but step 6 was not run or did not pass. Native
  stays the default; the note says what was and was not shown; or
- **closed:** no qualifying winner among the tested B2 schedules, each with a
  measured reason. Native stays the default.

Either way the code is committed and
[the scorecard](multi-gpu-training-comparison-20260929.md) carries the numbers.

### Quality rule (A1, frozen)

Six metrics: PSNR, SSIM, L1, each raw and colour-corrected, on the held-out
split. Three native repeats define an envelope; for every metric the
candidate's median-of-run means and median-of-run medians must be no worse
than the least favourable native repeat, and for every named view the
candidate's median must be no worse than that view's least favourable native
repeat. Every failure is reported; one metric never compensates for another.
Compute the envelope from these runs, never from older ones, and never widen
it after a failure. Same endpoint (step 20,000) for every run and metric.

### Timing rule (A5, frozen)

Complete wall from launch to published output: process start, checkpoint
load, training, every checkpoint and refresh, merge and readback. Evaluation
is outside the boundary for every arm. A speed win needs a lower median and
non-overlapping observed ranges over three repeats; if the ranges overlap,
one further batch of three, then all six count. Otherwise there is no speed
win.

## What is real today

- **Code** (commit `342f63c3`, the first commit of this work): two-owner block
  training `src/engine/EngineB1.cpp`, the `spirula train-blocks` group driver
  `src/app/B1Group.cpp` with the v3 live-refresh contract, `checkpoint/B1Contract`
  and `B1Merge`, native proofs `g1_native_proof --b1-refresh-check`,
  `b1_merge`, `b1_merge_contract`. SfM: the zero-rotation autodiff fix in
  `src/sfm/shaders/common/camera.slang`, DF shaders at `-O0`
  (`cmake/SsSfm.cmake`), `sfm_bounded_schur_test`.
- **Old-dataset result:** B2 won a bounded fixed-work comparison (357 s cold /
  251 s reuse vs 431 s native) but full-view B2 sits 0.60 / 1.05 dB raw/cc
  PSNR below native. Live refresh is implemented and bounded-validated
  (`docs/testing.md`, "Experimental live context refresh"); cadence **1,000**
  is frozen for the experiment. Whether refresh closes the gap is the open
  question. G1 measured 7× slower; O1 was rejected on its resource bound
  before it was ever timed. Neither is reopened here.
- **Data:** 4,416 full-resolution JPEGs already extracted from the two `.insv`
  originals (one per three frames, four camera folders of 1,104). The corpus
  was copied to durable share storage and all 4,443 files verified before
  deleting the Temp copy. Originals and corpus remain read-only. The private
  handoff beside the corpus records paths; never commit them.
- **Reconstruction:** the approved 1,104-view ordinal subset completed with
  CPU BA in both phases. Its sole component (`sparse/0`) contains **1,083
  registered images (98.10%)** and 286,599 points; mean reprojection 1.258 px.
- **Machine:** two AMD Radeon AI PRO R9700, 16 threads, no COLMAP. Check free
  space afresh; run outputs belong on the share.

## Decisions taken 2026-10-16

1. **Reconstruct every fourth frame (1,104 views) with CPU bundle adjustment,
   run to completion.** Same originals, same resolution; only the frame count
   drops. Training is fixed-step, so this does not make a run 4× cheaper; it
   makes reconstruction, caching and evaluation cheaper. The 1,195-view
   kitchen dataset already ran this whole pipeline end to end. GPU BA
   optimization (the "Source19" shared-pair solver) is parked.
2. **The custody/admission process is retired.** No call ledger, no
   charge-before-call, no per-artifact SHA receipts, no per-run approval, no
   private source snapshots. Code changes are commits on this branch; builds
   are `build_develop.bat -DSS_BACKEND=vulkan`; a run's evidence is its log,
   its output directory and the commit hash.
3. The working tree was committed as-is (`342f63c3`) so the tree is the single
   source of truth.

## Working rules

- **No wall-clock deadlines.** A run is killed only when it demonstrably
  stops progressing, and the judge is stage-aware: registration prints a
  line per image, but CPU bundle adjustment is silent for its whole duration
  and reports afterwards, so silence during BA is not a stall. Watch the
  process (CPU time still rising) rather than the log there. Mapping writes
  `sparse/` only at the end, so a killed mapping run has no partial model.
- **Disk:** keep ≥10 GiB free on every volume you write to *after* the run's
  own growth: features + matches for 1,104 views (~1 GB), and per training
  run every retained full checkpoint (`--save-only-latest-checkpoint 0`
  keeps one per `--steps-per-save`; size one, multiply). Check before a run.
- **Never write into** the originals or the 4,416-image corpus directory.
  The subset is a sibling directory of hardlinks on the same share volume;
  preserve the four `original-{0,1}/cam{0,1}` relative directory names.
- **One process per GPU**, and SfM (which uses a GPU for features and
  matching even with CPU BA) never overlaps a training run.
- **Stopping a native run at a step** is done by the launcher, not by
  shortening `--num-iterations` (the schedule horizon stays 30,000). Set
  `SS_WORKER_CONTROL=1` **only** when the process's stdin is a pipe the
  launcher holds open: on Windows the listener treats a non-pipe or closed
  stdin as an immediate stop-and-save. The checkpoint is validated before
  its final directory rename; `state.json` is inside `state.tar`, not loose.
  `reference/python/stop_at_checkpoint.py` waits for that publication, checks
  the embedded step/full-resume marker, writes one byte, and waits with stdin
  open. It propagates the child's exit code (42 for saved stop) and prints
  complete wall. The stop-save may land later; use the explicit target.
  `SS_B1_STOP_AT_STEP` works only under `SS_B1_OWNER`.
- **Evaluate in a clean environment.** `SS_B1_EVAL_PLY` refuses while
  `SS_B1_OWNER` is set; never leave B1 variables exported between arms.
- **No new candidates, SfM solver work or NVIDIA/CUDA work** unless step 4's
  table calls for one.
- **Commit after each step that works**, local only, no push.
  `bash tools/check_private_paths.sh` before every commit.

## Critical path

Each step starts when the previous one's output exists. Nothing else is on
the path.

### 0. Build and smoke

`build_develop.bat -DSS_BACKEND=vulkan`, then
`ctest -L headless --output-on-failure --no-tests=error` in `build_vulkan\`.
Commit if anything had to change.

### 1. Reconstruct the 1,104-view subset (unattended, hours)

Corpus layout is `images\original-{0,1}\cam{0,1}\` (1,104 each).
The approved selection is **sorted positions 0,4,8,... in each folder**:
276 each, 1,104 total. Sharpness-selected source-frame numbers differ across
the folders; only 121 filenames are common to all four, so this selection
does not claim exact-frame pairing. Hardlink into `<NET>\ds-1104\images\`
with the same four-folder layout. Use no `--rig` and no manifest.

The full-corpus feature/match caches were deleted; extract fresh subset
features. Corpus and source-preservation archive locations are recorded
only in the private handoff, outside git. The verified run's commands:

```bat
spirula sfm extract <NET>\ds-1104\images -o <NET>\ds-1104\features --features sift ^
    --max-image-size 1600 --max-features 4096 --decode-threads 4 --decode-budget 1024 --device uuid:A
spirula sfm match <NET>\ds-1104\features -o <NET>\ds-1104\matches.bin --pairs prefilter ^
    --prefilter-neighbors 24 --matcher bruteforce --camera-mode folder ^
    --camera-model thin-prism-fisheye --device uuid:A
spirula sfm map <NET>\ds-1104\matches.bin <NET>\ds-1104\features -o <NET>\ds-1104\sparse ^
    --images <NET>\ds-1104\images --device uuid:A --ba-real cpu --ba-real-coarse cpu
```

(`spirula sfm --help` is authoritative if a flag has moved.) Not `--quiet`.
Output: `<NET>\ds-1104\sparse\0`. The registration count that matters is
the component training will use, not the sum over disconnected models. If
fewer than ~95% of 1,104 register in it, read the log before changing
anything.

Observed step 1 (native build at `428793fd`): extract 1,104 images /
4,415,107 features; match 16,591 verified pairs / 1,917,071 inliers;
map exit 0, sole component 1,083/1,104 images, 286,599 points.
Logs: `<NET>\logs\step1-{extract,match,map}.log`. No deadline or killed run.

### 2. Native source checkpoint at step 10,000

```bat
python reference/python/stop_at_checkpoint.py <NET>\runs\native-src 10000 ^
    spirula train --data <NET>\ds-1104 --data-format colmap ^
    --output-dir-prefix <NET>\runs --output-dir-name native-src ^
    --eval-mode fraction --train-split-fraction 0.9 ^
    --num-iterations 30000 --steps-per-save 2000 --save-full-checkpoint 1 ^
    --save-only-latest-checkpoint 0 --disable-viewer 1 --keep-viewer-alive 0
```

`--eval-mode fraction` is required: the default `all` trains on every image
and the evaluator returns without evaluating. One GPU; the launcher stops it
once `step-000010000.ckpt` is complete (working rule above). Record the
named train/eval views from the log and the per-step time; every later run
uses `--resume` of that checkpoint and therefore the same split.

Observed step 2 (build `c1fcd4c0`): the explicit step-10,000 checkpoint has
1,000,000 splats, full resume state and horizon 30,000; 975 training inputs /
108 held-out inputs. The launcher exited 42 after saving step 10,001;
use step 10,000, not that later save. Complete wall 2,700.410222 s;
trainer elapsed 2,668.792 s / 10,001 completed steps = 266.853 ms/step.
The explicit checkpoint is 609,594,503 bytes. Named split and run evidence:
`<NET>\logs\step2-split.log`, `step2-native-src.log`.

### 3. Pilot: one of each arm, 10,000 → 20,000

Four arms, each started from `SOURCE = <NET>\runs\native-src\step-000010000.ckpt`,
each timed by the timing rule. Views manifest: one `0`/`1` per training
image; under `--view-policy full` it is provenance only (alternating is
acceptable for the pilot).

```bat
:: (a) ordinary native continuation, one GPU; launcher stops it at step-000020000.ckpt.
::     This is the baseline the quality and timing rules compare against.
spirula train --data <NET>\ds-1104 --data-format colmap --resume <SOURCE> ^
    --output-dir-prefix <NET>\runs --output-dir-name native-1 ^
    --save-full-checkpoint 1 --disable-viewer 1 --keep-viewer-alive 0
:: (b) all-owned frozen-state control, one GPU. Same code path as B2 with the
::     split removed; densification and shared appearance are frozen, so it is
::     a causal control, NOT the baseline. No SS_WORKER_CONTROL here.
set SS_B1_OWNER=all& set SS_B1_STOP_AT_STEP=20000
spirula train --data <NET>\ds-1104 --data-format colmap --resume <SOURCE> ^
    --output-dir-prefix <NET>\runs --output-dir-name frozen-1 ^
    --save-full-checkpoint 1 --disable-viewer 1 --keep-viewer-alive 0
set SS_B1_OWNER=& set SS_B1_STOP_AT_STEP=
:: (c) B2, frozen context (both GPUs)
spirula train-blocks --resume <SOURCE> --views <MANIFEST> --devices uuid:A,uuid:B --output <NET>\runs\b2-ctl-1 --updates 10000 --view-policy full
:: (d) B2 with live refresh (both GPUs)
spirula train-blocks --resume <SOURCE> --views <MANIFEST> --devices uuid:A,uuid:B --output <NET>\runs\b2-ref-1 --updates 10000 --view-policy full --refresh-cadence 1000
```

Evaluate all four on the held-out split with the native evaluator
(`SS_B1_EVAL_PLY=<merged.ply>` for (b)–(d), clean environment). Record
per-view PSNR/SSIM/L1 raw and colour-corrected and the complete wall.

Observed pilots (build `ce9ab00e`, endpoint 20,000, 108 identical named
held-out views, all six metric arrays finite; evaluation untimed):

| Arm | Complete wall (s) | Mean PSNR | Mean CC PSNR |
|---|---:|---:|---:|
| (a) ordinary native | 2616.195639 | 18.974123 | 20.649956 |
| (b) all-owned frozen causal control | 1337.948733 | 19.118900 | 20.775652 |
| (c) B2 frozen | 1335.371426 | 19.026793 | 20.635204 |
| (d) B2 refresh 1000 | 1785.884423 | 18.919929 | 20.386606 |

Evidence: `<NET>\logs\step3-*.log`, `step3-pilot-summary.json`, and
`<NET>\runs\{native-1,frozen-1,b2-ctl-1,b2-ref-1}` plus each `-eval` output.
Both B2 arms pass only the screening heuristic. No final qualification yet.

### 4. Decide from the pilot

In order; the first row that applies wins. "Promising" is a screening
heuristic only: mean raw PSNR within 0.3 dB of (a) **and** complete wall
below (a). Final selection is the frozen rules above, never this threshold.

| # | Pilot result | Next |
|---|---|---|
| 1 | A crash, refusal or non-finite state | Fix the bug, rerun that arm. Do not route around it. |
| 2 | (c) or (d) is promising | Two more repeats of that arm and of (a); apply both rules. The pilot counts as a repeat only if nothing in its configuration or timing boundary changed. |
| 3 | Neither is promising, but (b) vs (c) vs (d) names one cause (e.g. refresh closes the gap but not enough → cadence 500; split cost, not refresh → A4 reconciliation stage) | One predeclared variant, then back to this table once. |
| 4 | Otherwise | Close the tested schedules. Record the numbers. |

Completed execution: row 2 matched for both B2 arms; three repeats of native,
B2 frozen and B2 refresh 1000 used unchanged configurations and boundaries.

| Additional completed repeat | Complete wall (s) | Mean PSNR | Mean CC PSNR |
|---|---:|---:|---:|
| native 2 | 2614.295323 | 18.951683 | 20.639723 |
| native 3 | 2605.292171 | 18.953360 | 20.641679 |
| B2 frozen 2 | 1323.576282 | 19.028731 | 20.634828 |
| B2 frozen 3 | 1316.036778 | 19.027596 | 20.634706 |
| B2 refresh 2 replacement | 1624.831338 | 18.920056 | 20.387788 |
| B2 refresh 3 | 1619.962679 | 18.922141 | 20.388800 |

**Outcome: closed.** Both candidates passed timing with non-overlapping ranges
against native; neither passed quality. B2 frozen failed 3 aggregate and
197 named-view metric checks; refresh failed 8 aggregate and 351 named-view
metric checks. All 1,320 checks, including every failure, are reported in
`<NET>\runs\quality-checks.csv`; the frozen envelope was not widened.
The [final scorecard](multi-gpu-training-comparison-20260929.md#2026-10-16-original-derived-subset--closed)
contains the six-metric thresholds, medians, spread and release limits.

The first refresh repeat-2 attempt failed during step-18,000 staging PLY
writes (both workers exit 1, group exit 2, 1,284.611717 s). Share free space
fell from 30.383 GiB at launch to 0.517 GiB after failure; the last committed
generation was 17,000. Space loss remains unattributed. Preserve that failure
in `<NET>\runs\b2-ref-2` and `<NET>\logs\step4-b2-ref-2.log`.
After storage was restored, `b2-ref-2-retry` ran fresh from step 10,000 and
completed; only the complete replacement counts. No run was killed.
B2 frozen repeat 3's timed training completed before a controller interruption;
its untimed evaluation was confirmed in `b2-ctl-3-eval-confirm`, exit 0.

### 5. Settle and document

Scorecard row with medians and spread, the outcome word from the definition
of done, release limits (durability and capacity are unqualified), and a
one-paragraph summary at the top of
[the comparison note](multi-gpu-training-comparison-20260929.md). Delete
scaffolding and diagnostics that nothing uses. Commit.

Completed: the comparison note records **closed**, both three-repeat timing
ranges, all six aggregate thresholds and named-view failure counts, with the
complete 1,320-check CSV retained alongside outputs. The cleanup review found
no additional unused source scaffold: the named diagnostics retain real
build/runtime or manual-test consumers. Final headless CTest passed 17/17
in 13.47 s; no Spirula processes remain running.

### 6. Recovery check — finalist only, untimed

Only if step 4 produced a candidate that passed both rules. The A5 recovery
procedure in the ledger, unchanged: from the same source, a late STOP/resume,
one killed worker, and for refresh a failure immediately before and
immediately after a generation commit, each compared with the uninterrupted
run's exposure lineage, owner coverage and generation lineage. The known open
failure in `docs/testing.md` ("Experimental live context refresh") is the
bar; short smokes do not clear it. Pass → **selected**; otherwise
**experimental win**, with the failure recorded. Final commit.

Disposition: **not applicable**. Both candidates failed the frozen quality
rule, so neither is a finalist and no recovery workload was launched.
The final outcome is **closed**; native stays the default and recovery,
durability and large-scene capacity remain unqualified.

## Parked, deliberately

GPU bundle-adjustment optimization (Source19 shared-pair), the 4,416-view
full corpus, GUI/remote-worker integration, topology growth, O1/G1/G3, and
any CUDA/NVIDIA validation. None is on the path to the goal; each needs a
measured reason to reopen.

## What still applies from the ledger

Only the technical contracts: section 3's cross-mode numerical contract,
section 5's B2 contract and exclusions, and A5's recovery procedure (quoted
by step 6). The quality and timing rules are restated above and the live
copy wins. Gating language (budgets, ceilings, admission, approval) does not
apply.

---

# Frozen ledger (2026-09-29 → 2026-10-16): history, not instructions

## Approved independent new-producer route

**Current controlling decision:** the user selected **“Replace the old custody
requirement.”** The old93-call obligation is superseded as an unmet historical
requirement, not passed, consumed or transferred. Its declarations, requests,
failures, receipts, originals, archives and aliases remain unchanged. No old
artifact deletion/release is authorized by this amendment. Earlier instructions
to reserve those93 calls or complete the old C9 route before unrelated new-corpus
work are historical, not current prerequisites.

The global ledger is **280 charged;pending0;all native jobs closed;26/33 tasks done. User selected unchanged-limit GPU BA optimization;Source19 admission in progress,uncompiled/unqualified;no extended CPU-run authority**.
The user's **“/goal Continue until the issues has been resolved and the has been
fully executed.”** and **“Continue”** authorize remaining prerequisite-gated plan
execution; the exhausted212 ceiling is historical, not a new stop. All earlier
prospective no-execution/no-full-map-authorization statements in this document
are historical and superseded **only by this current goal**, not by prior
diagnostic allowances. Keep cumulative accounting, fresh per-launch admission,
scientific criteria and deadlines; no automatic60-minute approval or CPU
production fallback. Approval850B SHA
`92748b6e25ad6fb3b9a79ae141556788ab2f1045542c74aadf417e6b6ff3225c`.
Neither history nor accounting resets. Historical input loss remains explicit;
do not reprobe deleted paths, filter old requests, redirect old bindings,
reconstruct historical process handles or promote old acceptance to new-corpus
readiness. The failed9,704-row admission remains preserved and finally published.

### Initial finite allocation

| New named invocation | Maximum SDK calls | Required outcome |
|---|---:|---|
| N01: authentic current Main admission | 1 | Original finite SDK boundary, fresh native Main identity, actual owned root exit and complete native family closure; runtime admission only |
| N02: selected-original stream-metadata probe | 1 | Both selected originals, existing configured decoder tooling, exact native/SHA input and executable bindings, bounded captured metadata and actual child statuses; no frame extraction |
| **Initial tranche** | **2** | **102 +2 =104 maximum charged;102 calls remain unallocated under206** |

These are bounded slots, not a claim that preparation or A0–A6 fits two calls.
No retry, diagnostic, repair, build, installation, extraction, reconstruction,
training or artifact retirement is hidden in either slot. A failed/cancelled/
interrupted SDK entry consumes its slot. No unused slot silently changes role.
Each launch additionally requires its exact command, current native input/runtime
closure, immutable charge-before-call record and fresh resource quote. N02 is
not launch-admitted merely by being listed: locate the actual configured tool
and bind its metadata-only command first; no assumed `ffprobe` installation.

The current retained-output ceiling is **26GiB combined across local and network
outputs**, applied to both logical bytes and native allocation. The earlier
22→26GiB **“Keep10GiB floor; free space”** approval and subsequent explicit
network-output selection do not create a second allowance. Keep a **10GiB free
floor on EACH volume**. Network growth must be explicitly selected; existing
local-growth pricing semantics remain unchanged. Retain12-GiB aggregate host,
8-GiB native Job,4-GiB Main/baseline and9-GiB available-commit gates.
Scientific/build deadlines remain1200s; preparation allows up to3600s.
N48's3600s exception is consumed; the separate DF-route one3600s approval is
consumed by N60, completed negative; that no-further-full-map authorization state
is historical under the current goal, not a renewed3600s allowance.
GPU stages additionally retain24-GiB per-device backing and
1-GiB heap headroom. Old/new coexistence and temporary outputs count; history
does not give the new route a fresh storage allowance. Admission reads and
definition loading may use existing in-process native APIs without an SDK
child, but this is not permission to bypass the SDK with another launcher.

### Executed admission and next resource probe

At N01, the host's loss notice was contradicted by the actual retained globals and
original native Main handle: PID/create identity was unchanged. Reused those
original handles; closed the unnecessary duplicate bootstrap; **N01 spent zero
calls**. The failed historical admission was finalized without deleting its
fixture or other artifacts:4,698B SHA
`4d053f40e2ac497d3ebb2e25d8a06bc67f11efe3fa6a70740c0ab3765fd09102`.

Both originals passed unchanged strict full-native/content checks:
19,777,805,933B,24.9417468s; receipt1,668B SHA
`243752336637eab65f2e9440b0e27ec343361acb4aef725f38eb78d7257b6b80`.
**N02/SDK103 passed** through the original finite boundary:55.6849127s,
native/Popen exit0, one exact-owned native witness, Job total1/active0/terminated0.
Complete receipt16,565B SHA
`a75d47bca6628087f085b4f16ff6d445cac1d26a765014b6a71ab900ed92eaf0`;
metadata acceptance4,730B SHA
`82d88415a1fe200f7339366ac1c55dda962082f756597ba400932dd90d0f8514`.
The configured FFmpeg launcher was resolved to its actual decoder, invoked
directly with both inputs and the documented
[metadata-only muxer](https://ffmpeg.org/ffmpeg-all.html#Metadata-1).
Original A has two3840×3840 HEVC Main10, limited-range BT.709 tracks;
original B has two2880×2880 HEVC Main, full-range BT.709 tracks.
Both report24fps; container durations551.64s and551.58s. No frame extraction,
exact frame-count, calibration, reconstruction or corpus acceptance follows.

The final SDK quote observed13,333,629,840 native retained bytes and
23,371,878,400 free bytes: only about3.58GiB remains under the16-GiB shared cap.
The active build configuration is Vulkan, headless, patented decoding OFF.
Its shared DatasetPrep fallback stages one complete compressed video track
before candidate JPEG extraction; tracks are sequential, not simultaneous.
Do not assume that stage fits, or enable patented decoding to bypass the gate.

**N03/SDK104 passed**:13,239 packet rows,6,205,715,645 compressed payload bytes
(5.779523GiB),132,409 stdout bytes, no stderr; native/Popen exit0 and exact
Job1/0/0 closure in34.9011314s. Complete receipt15,480B SHA
`092428ada6f79f7c7452514ecac31094bed8a464d57cf39c7134503950692db0`;
resource result2,451B SHA
`55404338f9cb4f6ea82b00aee4df0d1030107b7b27d92af760951e2c733d3b52`.
The observed payload alone exceeds then-current3,846,046,832B native headroom
by2,359,668,813B. No whole-track completeness claim is needed to reject
unchanged whole-track staging. No decoded frames, build, GPU or preparation
launch occurred. N01 remains unused; no slot or accounting reset.

### Isolated preparation fix — not yet admitted

The proposed shared fallback reads the selected original video stream directly
instead of first copying its entire compressed track. Per-track timestamp
normalization, FPS selection, rotation and encoded candidate equivalence still
require actual differential verification. Candidate JPEGs, masks, reconstruction
and checkpoints need separate resource pricing; removing the track copy does
not establish that the complete preparation phase fits.

An implementation edit touched the historically bound `DatasetPrep.cpp` before
its ownership was checked. No build or SDK invocation used that edit. The exact
proposal was preserved privately; original bytes were restored, then only the
attributable last-write timestamp was restored on the same native file object.
The unchanged strict reader now matches the full historical binding.
**This is restoration, not continuous preservation.** Incident receipt1,427B SHA
`9ce4d85934b4b5df1f15ad5333c85b7949f33c33ca25e09207788971a8b2d224`;
unadmitted proposal123,038B SHA
`1e22a9db4eeda8862dad027ff27bfedf02e82674c521de63853bbf6b2ea8034e`.

Further edits and builds must use a separately owned development snapshot,
leaving protected source and existing build outputs untouched. Read-only source
metadata inventory found roughly32MiB across the required source/support trees;
it is not a content binding, complete snapshot or build-output estimate.
Price the target-only dev-script build before launch. The user explicitly selected
**Build-specific admission**: keep native assigned-before-resume Job containment,
terminal closure, existing resources/deadline, bound inputs/outputs and successful
native/Popen dev-script root exit. CMake interprets its own probe statuses;
individual compiler-child handles are diagnostic, not complete acceptance evidence.
This changes build admission only; scientific and custody-worker gates remain
unchanged. Approval receipt669B SHA
`2f5364bcf37b47d1e741567c895d5995ffe95055efb0458569cfbd501a61d920`.
The separately owned snapshot contains1,287 files/31,671,059 source bytes,
including the preserved extraction proposal. Only its copied dev script caps
jobs at2 and selects `spirula`; its copied Slang selector refuses a mismatched
explicit compiler instead of downloading. Protected originals remain untouched.

**N04–N07/SDK105–108 executed** the build-only boundary's handled-child,
root-failure, deadline and output-overflow cases. N04 accepted root0 and the
root's actual child17 result; native Job total3/active0, not the fixture's
assumed total2. Only the root handle was observed, so the extra member is not
identified retroactively. Exact family size was not a build-contract criterion.
N05 refused root17 and closed the Job, but also recorded two unexplained
retained-child witness assertions. Preserve those failures. N06 refused its
deadline with root/child/conhost native43 and active0. N07 refused overflow:
1,048,576 bytes retained,65,536 additional bytes observed/discarded, active0;
truncated output was not accepted. V1 summary3,551B SHA
`45f8e76d535828fcbd69873a8749b2c29d966b210ca05c591d5b8d352b74a293`.

V2 adds finite waits for each retained process handle within the existing
cleanup budget before unchanged identity/membership/exit predicates.
Job active0 and pipe EOF alone are not process-handle synchronization.
**N08/SDK109's actual regression passed the expected refusal:** root17,
Python child43, conhost43, all three exact-owned/signaled, Job3/0/0,
no extra witness failure;5.6838211s. Complete receipt16,053B SHA
`c8ea02712ded590705c14f9f4be5d1d53d0b819760c60e00f87bb4b35c94482d`.
N05's original assertion values are unavailable; do not invent them.
Through N08, no compiler, full dev-script build, frame extraction or GPU run
had occurred. The subsequent build attempts below do not admit extraction.

### N09–N13 build evidence and approved next helper closure

**N09/SDK110 refused:** the full dev-script attempt under build V2 with
`DETACHED_PROCESS` timed out at1200s on `cmd`'s `echo|findstr` pipe before
configuration, retaining zero output. **N10/SDK111 refused** the identical
detached pipe probe at5s. **N11/SDK112 accepted** that probe with
`CREATE_NO_WINDOW`: root0, `BUILD_PIPE_OK`, Job4/0,4.6491638s. Differential
receipt1,763B SHA
`329616a0fa21f970addbbd5f747f94d6aa5485a6a5a5ae501dc8d00d8c515188`.

**N12/SDK113 refused:** the V3 real dev script exited root1 during configuration
because private source1's vcvars selector was incorrectly space-separated.
Private source2 corrects it to `-vcvars_ver=14.44.35207`.
**N13/SDK114 compiled but remained UNADMITTED:** source2 completed all1035
Vulkan/headless build steps, native/Popen root0 and `Build complete`; an exact-Job
`vctip.exe` lingered, forcing the1200s deadline refusal and Job kill43.
Job total2932/active0;2,038 diagnostic witnesses were retained. Metadata exceeded
1MiB and was losslessly chunked as negative evidence, not accepted or truncated.
Both N12 and N13 checked all21,956 pinned source/toolchain/launcher inputs
unchanged before/after. Candidate101,195,776B SHA
`fefbce8e67fc7ef32a302cc4e292ce2792e6e1c3282bb8b65b86d2aa2a9613c9`;
N13 compact receipt1,743B SHA
`a41fdb539e94f85f78cfa28690184aa47686cb5f8aaaf3a5e2a348687987da5f`;
stdout68,151B SHA
`d236e2e365253ae2f513bd22f678f57698095220c9503217e66dc2897feb70aa`.
No GPU, extraction or scientific evaluation occurred. Public `DatasetPrep.cpp`
remains restored, not promoted; the private whole-track-bypass proposal's
behavior remains unverified.

The user approved **“Stop only the bound helper”**: only after successful
build-root0 and a complete current Job inventory containing exclusively exact
bound `vctip.exe` helpers may individual native handles be terminated with
nonzero44. Recheck the same creation identity/native handle/image/Job binding;
verify each helper signaled and Job active0. Any other member must never be
killed as an accepted closure. Approval1,277B SHA
`e4d9f335d07f2330c9f16042651de7d0d990d1804f1f3045285e423b5435dfa8`.
This does not retroactively accept N13 or change the scientific
`main_native_run_v5` boundary. At N13, build-only V5 and receipt chunking had
static review only; native ABI signature correction and runtime validation were
pending. The subsequent evidence below supersedes that pending status.

### N14–N20 build admission and original-derived smoke

Build V5's actual metadata-only large-positive chunk transport and
serialization-error refusal both passed; neither was an SDK or scientific run.
**N14/SDK115 accepted** a genuine CL compiler invocation: root0, Job3/0,
exact bound `vctip.exe` handle exit44 verified,9.7259911s, native Job
peak44,404,736B;20,670 tool/source input checks unchanged before/after.
Receipt72,690B SHA
`69c57edcc7ab7a3be3a9c134271ec88d76c5051d5d46ddd580dec74ca582c903`.
**N15/SDK116 accepted** the real incremental dev script: root0, Job87/0,
8.8810651s, native Job peak217,468,928B;21,957 input checks unchanged
before/after, including the pinned N13 candidate. Receipt81,180B SHA
`112a0c7a47c22d41dc9871b7721e65327d07f4ad9b0d570bc62c3221035fa8f2`;
admission4,902B SHA
`bc34bd488d8ca2655c80bdf2501dada059994352c489eefffc1fd29d2e086234`.
The candidate is **newly build-admitted**, not a fresh full compilation or
retroactive N13 acceptance. Public source remains restored/not promoted.

**N16/SDK117 accepted** stream copies of2-second prefixes from each actual
raw original, not LRVs: A44,417,073B, B26,319,909B.
**N17/SDK118 and N18/SDK119 accepted** the prefix probes: each has two tracks,
48frames/track, PTS start0,2s duration; A3840×3840, B2880×2880, no
display-rotation side data. This does not admit proprietary trailer,
calibration, rotation semantics or the full corpus.
**N19/SDK120 accepted** exact legacy per-track remuxes, four files.
**N20/SDK121 refused** four simultaneous legacy JPEG framehash outputs:
root4294967284 (-12), FFmpeg9.0.1 `ff_frame_thread_encoder_init` /
`Cannot allocate memory`; native peak8,662,712,320B exceeded the8-GiB
Job cap, terminal Job1/0. Preserve partial outputs and the failure receipt.
Through N20, no comparison had passed and no direct differential or worker had
been invoked. Preserve that failure; subsequent bounded parity is separate.

### N21–N23 bounded parity and preparation-admission blocker

**N21/SDK122 accepted** production-like sequential legacy extraction across
four tracks: Job9/0,13.9422369s, native peak3,661,488,128B.
**N22/SDK123 accepted** direct extraction of those same four tracks
sequentially: Job9/0,13.968979s, peak3,661,963,264B. All48 JPEG packet
SHA/sizes/PTS/DTS/durations and entire framehash headers/files were
**byte-identical**:12frames/track at6fps, four tracks from the actual2-second
raw-original-derived prefixes. Differential6,824B SHA
`45f8f88c776b73e4f6437a98aff067295f9308c4514d4e31de282c2d3a6b45a9`.
This is bounded command parity, not full-corpus, rotation, proprietary-trailer,
calibration, adaptive-selection or real-worker proof.

**N23/SDK124 refused** the exact production metadata CLI contract probe,
replacing an unissued worker slot. `DatasetPrep.cpp` lines585–640 invokes
`ffmpeg -nostdin -hide_banner -i path` without an output, parses the stderr
stream table and ignores its return code. That same argv on the admitted
fixture produced a valid two-stream table followed by
`At least one output file must be specified`, actual native exit1.
The unchanged strict boundary refused; Job1/0,5.1588696s. Receipt12,774B SHA
`bfa5e3c89a578e781a3bcff6f8fe4707dc3d8f18ac49aa1a44ed5e759c49f545`.
No real worker was invoked. The current scientific boundary requires every
child exit0 and therefore cannot admit ordinary preparation's metadata probe.
Build-only approval does **not** apply to preparation.

**Decision cleared — preparation-specific admission approved.** The user chose
this boundary rather than changing the production probe: require native/Popen
root0, a matching successful typed worker result, verified inputs/outputs and
exact-owned Job active0. Child statuses are diagnostic, not a complete
fast-child witness set. There is **no helper-stop privilege**; scientific and
custody runners remain unchanged. Approval648B SHA
`ec775e9d637fb76305579cf8648b30ec10088c0b3e2db3876c0673a70aed2b33`.
At approval, ledger124/206,82 remaining; attempt125 had not been allocated.
The subsequent smoke used two bound byte-identical short-remux derivative copies
with `.insv` extensions solely to activate the existing multitrack branch:
not original-container/proprietary-trailer/calibration proof. N23 stays refused.
At this approval, shared root16GiB and deadline1200s remained unchanged;
the later explicit resource amendment below supersedes only those named limits.
Charge-before-call and206-total ceiling remain unchanged.

### N24–N28 worker smoke and full-source sizing blocker

**N24/SDK125 passed the actual preparation-only boundary:** real worker root0,
matching successful typed result,16images in four camera folders, Job18/0,
14.8438698s, native peak3,655,729,152B. All16 native JPEG files matched
reference SHA/size at the corresponding zero-based candidate index from N21/N22;
total output20,411,406B. Boundary25,657B SHA
`ec63b2f295cfc8a0a303d94b17eb1f6e1288ec15bb9cd0522b2286918c82a619`;
smoke24,066B SHA
`14be7d12fa0a424bf39bbc5dab67c62aa5d1f7062c065aab16473bd7e0a993ef`.
This validates only the nonadaptive prefix/no-orientation-matrix case on the
private candidate; public `DatasetPrep.cpp` remains untouched. No full-corpus,
proprietary-trailer, calibration or GPU/science qualification follows.

**N25–N28/SDK126–129 passed** complete-original-track decode/encode framehash
surveys, not JPEG-corpus creation:3310frames/track at6fps, all root0/Job1/0.
Ordered walls611.8752723 /604.7573734 /326.1993351 /326.9456156s;
native Job peaks4,843,327,488 /4,766,916,608 /2,588,463,104 /2,591,617,024B.
Exact full candidate JPEG byte totals4,557,384,684 /4,017,417,318 /
3,593,552,302 /3,599,856,345B. The existing `SourceFrameSelect` lines233–240
one-per-group-of-three rule implies4416selected frames and measured encoded-size
bounds4,987,164,937–5,601,066,225B final,7,482,724,235–7,867,445,079B
sequential peak stage, **before filesystem/metadata overhead**. These are
full-source size bounds, not an actual selected corpus or measured worker timing.
Survey9,256B SHA
`bbf758432cc664d5fed4c02e0370f48c0da4bd13e8f5909c010af667f9cde08a`.

Fresh shared-root native14,566,791,320B against17,179,869,184B cap leaves
2,613,077,864B headroom; free scratch21,692,977,152B. Final minimum alone
exceeds headroom, as does first-camera staging4,557,384,684B. Removing the
old6GB whole-track remux does not make full JPEG staging/final corpus fit16GiB.
Measured root plus stage upper bound was20.8935GiB before overhead, with
projected free12.876GiB against the10GiB floor; not itself admission.
Four survey walls total1869.78s, not a real-worker upper bound.

**Resource decision approved:** the user chose both **22-GiB shared-output
ceiling** and **up to3600s for preparation only**, retaining2fps, sharpness
group3, full resolution and historical artifacts. The shared cap changes globally
16→22GiB; RAM/GPU/10-GiB-free/numerical/quality/custody gates remain unchanged.
Scientific/build deadlines stay1200s. The three shared resource-pricer constants
were updated; readonly source899B SHA
`c75613e09d13c776df6d212718e9f0a53a1bf07840e73f1bdbc7794be6bec043`.
The original scientific function object is unchanged, but uses the new shared cap.
Fresh8-GiB-growth reserve quote passed: root14,566,819,992B,
free21,692,252,160B. Preparation-only V2 static review passed; its only changes
are name/format and1200→3600s,28,064B SHA
`c1895a0e1f2de8ff6b060a5726b3fddf1c22b859ac17cabc8cf44c92885002ea`.
The full-raw two-input worker request was2,846B SHA
`615ac504e41f229e78c3275069b5342dfd11e4215b5fa39aaa5af702dfafdbee`.
N29/SDK130 used freshly priced3300s full-raw preparation, below approved3600s,
with8-GiB growth, same CPU external FFmpeg and source2 candidate.

### N29: full-original image extraction and lineage verified

**N29/SDK130 completed and passed preparation V2:** native/Popen root0,
schema2 successful typed result with4416images, exact Job18/0/0,
1912.6359599s, native Job peak4,846,612,480B. Receipt27,266B SHA
`959e8480781c99a61c93a19fcbc378ed104869f14533562b451322984bd23c66`.
Main exhaustively checked each of4416files twice with native identity/SHA.
Every JPEG exactly matched size/SHA of its indexed packet in the complete
raw-original surveys: exactly one per group3,1104images per camera.
Ordered camera JPEG bytes1,553,262,725 /1,370,717,215 /1,267,004,060 /
1,267,831,801 total5,458,815,801B;23chunk manifests.
Corpus proof20,033B SHA
`ca4c6427c73885707b250a99dd95a88123e3bde77800f295ee040940b60fa8a5`.

Full-original image extraction and packet-to-image lineage are now verified,
not merely prefix parity. The preparation-decision/storage blockers above are
cleared for this accepted run, not future resource admissions. This used only
the private admitted candidate; protected public `DatasetPrep.cpp` is unchanged.
No masks, SfM/camera calibration, native parser/configuration, splits or
scientific qualification follow. Preserve the22-GiB shared cap,3600s
preparation-only ceiling,1200s scientific/build deadlines and all other
resource/numerical/quality/custody gates; retain all earlier failures.

### N30: CPU help admitted, reconstruction gates unresolved

**N30/SDK131 accepted** the admitted candidate's `sfm --help`: root0,
Job1/0,6.8036295s. CPU help only, no GPU launch or reconstruction admission.
Native SfM's `VkContext` bypasses the backend
`SS_WORKER_DEVICE_LIMIT_BYTES` ledger; existing bundle-adjustment estimates and
static heap size do not enforce24-GiB backing plus1-GiB heap headroom.
Shared-policy integration design remains under review before any GPU launch.
High SIFT8192/2400's maximum serialized feature cache is
4416×8192×147 =5,317,853,184B plus headers/matches: current residual22-GiB
root space cannot guarantee that upper bound. Actual feature counts are unknown;
no lower-preset choice/approval or frozen reconstruction configuration exists.
CLI `--check` is map-only consistency, not prospective native-parser admission.
GUI `.insv` defaults are thin-prism-fisheye, folder grouping and focal factor0,
not factory calibration. Native reconstruction/calibration/parser/config/splits
remain unqualified. Do not reimpose the superseded old C9 custody route.

### N31: device query passed; source3 then unbuilt

**N31/SDK132 accepted** the actual Vulkan instance/physical-device query:
root0, Job1/0,5.088345700s; query0.2437267s. Both exact AMD Radeon AI PRO
R9700 UUIDs, driver8389003, reported heap-guideline-minus-own-usage
33,250,242,560B each; planned24-GiB backing plus1-GiB margin passed.
Receipt15,716B SHA
`b35412c5c7ef2fb36797cb5749a36a4b4fbc048391983acb00b99453e536437b`.
This is not allocation enforcement, global physical VRAM peak or SfM science.

Source3 is installed in a new privately owned tree:1289files/31,695,772B.
Public source/source2 stayed unchanged with1287+17 native/SHA before/after
checks. Shared GPU-budget and generic `SS_TRAIN_STOP_AT_STEP` proposals had
static-review defects corrected before installation; **then unbuilt/unverified**.
Target retains30000-step horizon; planned finite10k/20k success is distinct
from STOP42. Scientific runner unchanged. Source3 build intermediates are
declared temporary, final source/images/proofs retained; no retirement occurred
and no old-artifact deletion permission follows.
**At N31, next pending:** freshly price/admit the source3 build, then verify
budget/finite-target behavior and feature/match storage/configuration admission
before SfM or scientific qualification. The following evidence supersedes the
then-unbuilt status, not the scientific admission gates.

### N32–N38: Source3 build and dataset-free device contracts

**Through N38:139/206,67 remaining. N32/SDK133 refused before launch:** the initial
declaration was11,135,484B against512KiB. No Job/compiler launched; evidence
is preserved in11chunks. This charged refusal remains separate from the retry.

**N33/SDK134 accepted** the genuine Source3 development build: root0,
Job2950/0, wall645.840134s. The exact approved `vctip` helper44 was used;
21,957 source/toolchain files were pinned and checked before/after. Four
binaries built: `spirula`, `vk_runtime_smoke`, `sfm_sift_test` and
`cli_training_smoke`. Source3 snapshot SHA
`2779a760b88061ff95a091549077ba0ff0dab327d5a7aacaf480a3e0fb9836a8`;
`spirula` SHA
`0c7c39a51bc0eabf9fa745fd06fb485312bbebecdd55a5c65e227acc07ac6260`.

| Invocation | Actual dataset-free result |
|---|---|
| N34/SDK135 | Backend backing-cap PASS,5.589029s |
| N35/SDK136 | SfM backing-cap PASS,5.547021s |
| N36/SDK137 | Backend invalid-cap PASS,5.5627101s |
| N37/SDK138 | SfM invalid-cap PASS,5.3744918s |
| N38/SDK139 | Normal `vk_runtime_smoke` ALL PASSED,5.7898966s under24GiB cap; committed peak79,691,776B |

The backing-cap checks used192MiB; after-free current/reserved backing was0,
active contexts0 and failures3 in both backend/SfM AMD contexts. All these
contract checks ran on the first AMD; the second AMD was queried only. N38's
final report is **before shutdown free**, with64MiB cached staging still
present: do not claim a final zero ledger for N38 or a global physical VRAM peak.

These are dataset-free contract checks, not full-corpus/scientific qualification.
Source3's finite training endpoint compiled but was **not exercised**;
`cli_training_smoke` was not run. The same4416 full-resolution original-derived
images remain prepared. SfM reconstruction/model, native parser/config/splits,
source/reference admission and A0–A6 remain pending. Scientific runner unchanged;
no deletion/retirement/release occurred or is authorized by these results.

### Source3 temporary cleanup and N39: measured Medium SfM extraction

The user explicitly selected **“Medium SfM, measured stages”**, retaining all
4416 full-resolution original-derived images and every scientific gate.
After the verified build/GPU checks,203 new Source3 temporary `.obj`/`.lib`/
`.res` files were deleted through the existing same-handle native/SHA guard:
394,766,302 logical /395,195,080 native-allocated bytes. All1289 source files
and four executable images were pinned/checked before/after. Cleanup receipt
3251B SHA
`e9854393e93d92d6d3b7f73c196ff8649a8896ccc97b423ea4ca73588587f6e9`.
No old artifacts, aliases or runtime assets were retired. Source3 link
intermediates are now missing: **no incremental-build closure claim**; any future
build requires a fresh admitted namespace.

**Through N39:140/206,66 remaining. N39/SDK140 accepted** actual full-corpus native
SIFT extraction: root0, Job1/0, wall110.66509339993354s. Exact
medium-equivalent extraction flags:
`--features sift --max-image-size1600 --max-features4096 --decode-threads4 --decode-budget1024`.
`--quality` is auto-only; extraction takes no camera flags. The embedded SIFT
shader ran on the first AMD under24GiB GPU backing cap: peak840,450,184B,
failures0; terminal after-free current/reserved0, active contexts0.
5706 actual image/source/binary files were pinned/checked before/after;
environment restored.

All4416 feature files passed native/SHA/header/full-coverage validation:
17,669,462 features, per-image min1756/max4096; U8/128D descriptors, original
3840/2880 dimensions,1600 extraction, finite keypoints and exact EOF.
Feature storage is2,597,636,130 logical /2,614,222,848 native-allocated bytes.
Feature admission20173B SHA
`375aaf6b61ecadc2059163d1eddef6f3bf70a6e37a1ea331e07377e7d040ae9b`;
completed report34189B SHA
`6cc95318169b06f974fc75cb1ceaa464b4fa0382bebcff79982b7dc4605ea40a`.

After-run root usage23,403,597,152B /cap23,622,320,128B and free
12,679,888,896B are observations, **not current guarantees**. Matching was not
launched: only about0.22GB root headroom remained. Matching/model/calibration,
native parser/config/splits, source/reference admission, finite training
endpoint exercise and A0–A6 remain pending. Feature extraction is not
full-scientific qualification; all prior failures and restrictions remain.

**Historical next-stage matching blocker (source-grounded, then not launched):** standalone
`match`, not auto/resume, supports explicit
`--pairs prefilter --prefilter-neighbors24 --matcher bruteforce --camera-mode folder --camera-model thin-prism-fisheye`.
`CMD_MATCH` supports these camera flags; the prior only-auto/map claim was
incorrect. There is no pairs-only/dry-run mode. The conservative upper bound is
4416×24 =105,984 pairs ×(16+4096×8) =3,474,579,456B plus metadata.
Standalone matching has **no `matches.part` journal**; do not double this into
the6.95GB auto/resume-only bound. Actual file size is unknown until matching.
The4GiB growth reservation now has an explicitly approved26GiB ROOT ceiling;
the10GiB free floor remains unchanged. Earlier observed free11.80449GiB and
ROOT21.80013GiB /cap22GiB are historical; only local volume C was found.
The initial decision receipt3417B SHA remains preserved:
`96073d63e67e35246ded3582a3ccba8385cf332f5ad370dd0ce7f1cda302572f`.

**Resource decision before the separately authorized cleanup below:** user chose
**“Keep10GiB floor; free space”**, approved shared ROOT22→26GiB, and approved
no other deletion at that point. A new owned
resource26 helper was installed/pinned; byte comparison confirms only three
`22*G`→`26*G` ceiling constants changed. Scientific runner and every other
guard remain unchanged. Actual positive1MiB pricing passed; the actual4GiB
matching quote correctly **refused solely on the free floor**: free
12,674,670,592B versus required15,033,434,112B (14GiB +1MiB receipt),
shortfall2,358,763,520B (about2.20GiB). User must free space outside preserved
campaign assets; recommend at least3GiB. No matching allocation/run or quality
reduction to force fit. Installation receipt3672B SHA
`1f790eeefe9c9486065d88b6e8f889c704d3e74586543963f13ba16e40818a34`.
No SDK call charged; ledger remains140/206,66 remaining. These observations
do not replace fresh admission after space is freed.

### Authorized older temporary cleanup; historical matching storage blocker

The user subsequently requested removal of older temporary files no longer
needed. Retained same-handle native/SHA deletion removed218 files:
200 accepted Source2 dev2 build-only `.obj`/`.res`/nine static libraries,
16 N24 successful output-only smoke JPEGs and two old B2 Python caches.
Removed406,647,455 logical /407,102,152 native-allocated bytes (388.24MiB).
12,314 retained files were pinned/native-SHA checked before/after, including
all Source3 source/four executable images, current4416 JPEGs +4416 features,
all other dev2 files/runtime assets, original `cache.py` sources and
`compilerproof.obj`. Dev2 accepted provenance is N15/SDK116, root0, Job87/0,
receipt SHA
`112a0c7a47c22d41dc9871b7721e65327d07f4ad9b0d570bc62c3221035fa8f2`.
Cleanup receipt38491B SHA
`d654fd9da8a2e3f831dc73eecefb75c608b09dde6335e0974a8743c133c018c2`.

Preserve the entire failed dev1 build, prefix MP4/`.insv` validation inputs,
raw originals, framehash lineage, historical failure/custody archives, aliases
and every receipt. Source2 incremental-relink closure is now incomplete;
N24 raw16-JPEG smoke replay is intentionally unavailable, but its authentic
receipts remain. The full4416-image corpus/features are unchanged.

**Post-cleanup measured storage before network routing:** free12.1659317GiB,
ROOT23,008,467,608B /cap26GiB. Actual post-cleanup4GiB matching pricing
**still refused** under the retained10GiB free floor: shortfall1.83504486GiB
against required14GiB +1MiB receipt free. Earlier resource observations above
are historical. Storage status1835B SHA
`88fe429b564a25e299f30cf8fa89386dab1fdbf24b1ac4a8d23237015f524264`.
No SDK/GPU/compiler/matching run; ledger unchanged140/206,66 remaining.
No broader deletion, quality reduction or scientific qualification follows.

### Network output route and N40–N42: full Medium matching accepted — 2026-10-09

The user explicitly selected the network output share. **Routing is operational;
the local-storage blocker for matching is resolved.** Existing inputs, datasets,
sources, evidence and aliases remain local and unchanged; no relocation,
deletion, old release or historical acceptance credit follows. Private share
paths and hostnames remain outside these public records.

The26GiB cap covers **combined local/network logical bytes AND native allocation**,
not26GiB per location. Preserve10GiB free on **each** volume, explicit selection
of network growth and unchanged local-growth reservation semantics. NetworkAPI
observed the shared identity of mapped and UNC access and proved flushed
native-SHA readback plus pin/rename refusal. These are observed client-side
identity/storage proofs, not a server-crash or outage-durability claim.

This continuation required an actually **fresh Main17360**, full creation
`134360386574936322`, parent26760: previous retained globals were absent.
The old Main19788 query yielded only `OpenProcess` error87, **not an observed
exit or retained-handle/family closure**. No old process handles or old-family
closure were reconstructed. The earlier N01 retained-runtime admission remains
historical evidence, not identity continuity into this continuation.

Original scientific V5 source remains unchanged. The separately versioned
**NETWORK_V1** runner additionally bounds read-only SMB observations with the
min5s/remaining-deadline budget. A pending observer is retained, and the next
run is refused until its actual completion; timeout is not proof that the
observer stopped. No network outage or server crash was exercised or claimed.

**N40/SDK141 accepted** the actual child's256KiB network write, fsync and readback
plus fresh physical-device query: root0, Job1/0,5.8501866s. Both exact AMD
R9700 UUIDs were freshly queried with33,250,242,560B available each.
This was instance/physical-device observation only: no application device or
kernel execution.

**N41/SDK142 passed the intentional deadline-negative contract**, not science:
60.4130132s; actual native43, signaled same-Job process, Job1/0.
There were no pending observers at completion. Preserve this negative separately
from subsequent matching acceptance.

**N42/SDK143 accepted** actual full4416 original-derived Medium native standalone
`match` output to UNC: root0, Job1/0,186.1343987s. Exact matching options were
`--pairs prefilter --prefilter-neighbors24 --matcher bruteforce --camera-mode folder --camera-model thin-prism-fisheye`,
with `--device` selecting the first freshly bound AMD and `--quiet`; GPU backing
cap24GiB. All4418 actual feature/binary/loader bindings were pinned and checked
unchanged before/after; environment restored.

Output148,881,632 logical bytes /148,885,504 native-allocated bytes, native SHA
`8a880c6504385df53aebd159fb47b47288a140447d19d3d6d084152ce789950c`.
The full binary audit verified:

- Exact4416 names/counts and exact EOF/native SHA.
- 67,632 verified pairs /18,456,658 inliers; indices unique and within their
  feature bounds; all4416 images covered in **one connected component**.
- Four folder cameras,1104 images each, model10 with12 finite parameters,
  original3840/2880 dimensions and `focal_measured=1` for all four.
- Pair selection68,034 of **9,748,320** exhaustive possible pairs.

GPU backing peak1,829,765,120B, terminal after-free current/reserved0, failures0;
peak native Job committed5,292,777,472B. Match-audit receipt5683B SHA
`89f13fe763da7ce2bdb20b3f86f46692b404d786a37057925d1a45798fc4c590`;
completed run52367B SHA
`3b56288f129b974e465ae572e338d95411c5975a37b40ae9624f2b38019bc1f8`.

Historical N42 combined output21.567535GiB, local free13.05158GiB and network free
265.75437GiB. These are observations, not future resource guarantees; do not
attribute an external free-space delta to this campaign.
**Through N42: ledger143/206,63 remaining.** Mapping/native parser/config/splits,
source/reference admission, finite endpoint exercise and A0–A6 remain pending.
Matching and connected coverage do **not** qualify the corpus or training.
**Historical N42 mapping blocker:** read-only Source3 map tracing could not prove all
sparse outputs fit the roughly4.43GiB remaining under the combined26GiB cap.
`--max-models50` caps the **initial mapper**, not final output: default
`finishModels` calls `splitInconsistentModels`/`splitFoldedModels` in `Assemble.h`,
which can increase model count. Later growth/reseed also invalidates the proposed
`2*N+21*50` total-membership bound. Neither that expression nor52GB is a proven
final-output bound. Native pre-publication output-budget enforcement was required
before map, preserving all4416 images and default scientific settings.
No map invocation or SDK charge had occurred through N42; ledger was143/206.
The Source4 writer proof below resolves this output-guard blocker, not science.

Map must **omit camera-override flags** to reuse the four measured
thin-prism cameras embedded in matches; repeating those flags rebuilds calibration.
Map reads keypoints/colors without descriptors, uses `--images` with the private
full-original image path, and retains default BA auto and all quality defaults.
Match-graph connectivity does not prove one output model or registered coverage.
All old receipts, negatives and scientific gates remain unchanged.

### Source4 and N43–N45: output guard proved; full mapping deadline failure — 2026-10-09

Source4 is a separately owned1,289-file snapshot copied with native/SHA checks.
Its five changes include private dev-build targets `spirula` and `sfm_map_test`;
base Source3 and public source remain unchanged. Final source SHA
`f2cb143169414533d912ac6172294be99cea236c4d519d3520bcd033ed872682`.
Original scientific V5 and NETWORK_V1 remain byte-unchanged.

The native sparse writer enforces strictly positive `SS_SFM_OUTPUT_LIMIT_BYTES`
before publication against actual final models and actual binary/formatted
output, including gauge/rig extras. Pricing uses64KiB per-file rounding plus
directory allowances. The actual share allocation unit was4096B; this is
ordinary-metadata qualification on that share, **not a universal filesystem
allocation guarantee**. No initial-model-count bound substitutes for final output.

**N43/SDK144 accepted** the actual Source4 dev build:667.2763842s,
native/Popen root0, Job2938/0, peak8,466,911,232B below8GiB.
Only the exact bound `vctip` helper was stopped with approved44; all21,958
inputs were checked unchanged before/after. Full chunked build receipt1,664B SHA
`c558db320ed44cf768cdf4dce703366af0defc5d2fbdd1fdc5a0f2c40344a552`.

**N44/SDK145 accepted** the actual CPU real-writer proof on the share:
6.5864175s, root0, Job1/0. Output reported
`PASS CPU sparse output budget exact emitted bytes refusal preservation`.
No GPU probe occurred; the fixture was removed. Receipt35,668B SHA
`7b64c93e29d8f9a14390518215bb38f404709a53553d4cf46faddd72f4126ac5`.
The native output-guard blocker is resolved; this is not reconstruction acceptance.

**N45/SDK146 attempted full4416 mapping and FAILED.** It retained default
scientific settings and omitted camera overrides to reuse stored measured
calibration. A fresh immutable physical-AMD probe ran inside the owned Job
immediately before the native map child. Admission selected a3GiB native output
limit,3GiB +16MiB growth reservation and unchanged1200s deadline.

The deadline assertion was followed by NETWORK_V1 negative-cleanup assertion
line183, combining creation identity, exact-Job membership and signaled wait.
**The failed constituent is unknown.** Cleanup escaped before a completed runtime
receipt; original native handles were closed and no frame was retained.
Do not reconstruct witnesses or claim full native-family closure, mapped coverage,
GPU peaks or an exact elapsed wall time. No usable model exists; the requested
output does not exist. Captured stdout contains only the fresh physical-device
query; stderr is0B. All8,838 payloads remained native/SHA unchanged after failure;
environment restored, no pending SMB observers. Logs and all negative history
are preserved. Parent failure receipt4,773B SHA
`0cfe7958e505ddad804cfefc751bd53d125e1b83918148207a5835784c374fa1`
is failure evidence, **not a completed runtime/closure receipt**.

**Current280 charged,pending0;all native jobs closed;26/33 tasks done. User declined longer CPU execution and selected “Keep current limits and optimize GPU BA.” Independent Source19 admission from immutable Source18 is in progress for reviewed shared-parameter DensePair candidate;not compiled/qualified,automatic shared production selection OFF pending qualification. Three candidate tasks added,four original integration/model/A0–A6 tasks remain blocked. No CPU3600/14400 authority,no cap/deadline increase or new full-map claim;N179 failure/consumed one-use approvals and all scientific gates retained.**
NETWORK_V2 remains frozen and unadmitted; NETWORK_V3 has actual negative/positive
native-family proofs below. Frozen scientific V5/NETWORK_V1 unchanged. The user's
**“Allow60minutesforreconstruction”** authorized **one full4416 mapping
attempt with a3600s deadline after network-runner repair validation**.
Approval552B SHA
`709c8975fd9bff3432201e15c81a285639f814e2e49a0235405a80bae632247a`.
All other limits remain unchanged, including other scientific/build1200s
deadlines. No automatic retry, completion guarantee or silent corpus/quality reduction.
That single approval was consumed by N48/SDK151; no unchanged retry/extension.
Combined output observed after the build was22.4657GiB; the earlier21.567535GiB
observation is historical, not current headroom. Native parser/config/splits,
source/reference admission, finite training endpoint and A0–A6 remain unqualified.

### N46: NETWORK_V2 prelaunch refusal; V3 then unadmitted — 2026-10-09

**N46/SDK147 did not pass the native-family test.** NETWORK_V2's initial
full-retained-root resource census exceeded a generic5s asynchronous wait.
**No Job, Popen process or native child was created.** The complete negative
wrapper was retained:808B SHA
`36b9483ed39169cbe3ff53fe62329c1c22fb6720895513bda848e13082d25a6c`.
Original observation tasks were held at this point; timeout is not completion.
One pending-observation/resume-publication attempt also hit the5s census bound;
its negative wrapper810B SHA
`135c0cdc3b01aabe6b9aa0054bed7cfdd8b4c2e0057d600d994f3e296b35736c`
and parent prelaunch-refusal SHA
`8c95c23e69eb1648c1fc05e5a58d1d44dc1cf704f530c46dbed88b72d5d6406f`
are preserved. These are prelaunch negatives, not native cleanup validation
or closure receipts; no family-test pass follows.

Frozen NETWORK_V2 source37,641B SHA
`299b413021dbc13162ee19db55d0b226ffffa4229b1229675d39a1c42f4c02f0`.
The separate NETWORK_V3 repair distinguishes5s SMB free-space observations
from a full-root census bounded at30s within its phase, and repairs authentic
old prelaunch pending-observation closure publication. **At N46 this was in-progress
design, not exercised native cleanup or admitted execution.** Original pending
tasks and all failures/receipts were retained, not inferred closed.
Through N46:147/206,59 remaining; approved3600s map remained unused.
The actual task recovery and V3 validation below supersede that pending status,
not the historical failures or other deadline/resource/quality constraints.

### V3 qualified: task recovery and N49/N47/N50 through SDK150 — 2026-10-09

NETWORK_V3 source43,159B SHA
`58afd519e0ff7b76d38832c0ca44145a566d2b6fd446cbe128ffb6a439f6d950`.
It separates5s SMB reads from30s full-root census within the phase, bounds
negative publication at30s, aggregates successful routine observations with
constant metadata, and checks the strict pending registry across V2/V3.

Actual recovery reused the original V2 prelaunch tasks and published a complete
negative27,582B SHA
`ba2d62fb305ab40a01b427c12ca5d144455a75822dfd2ce822c893484f1d0884`.
The original V2 finalizer was released only after parent acknowledgement.
No Job or Popen process was reconstructed; this is authentic prelaunch task
completion/publication, not retroactive N45 native-family closure.

**N49/SDK148 passed the newly allocated60s CPU root+child deadline-negative
contract.** Three actual members—root Python, child Python and conhost—each had
native43, matching creation identity, exact-Job membership and signaled state;
Job3/0. Row wall60.0476358s excludes subsequent publication. Receipt44,204B SHA
`295c97d8d4bf0c3766af5e0bd8b78ce72f77e432759746e2cb804bdee094113b`.
An actual Main-versus-child-Job bad predicate (false membership/WAIT258) was
rejected; the shared pending guard refused launch, then parent acknowledgement
and release passed. This is failure-lifecycle validation, not scientific output.

**N47/SDK149 intended positive fixture FAILED:** root1 due to inline-quoting
`SyntaxError`. Job1/0 and negative publication passed, but **no positive credit**.
Receipt43,612B SHA
`7bb938e2fe4afb8ecdd0e38587914ad8e82b838116c81786d93afd860e899fd1`
is preserved; the correction does not erase this failure.

**N50/SDK150 accepted** the corrected separately owned file; root and embedded
child syntax were compiled before SDK entry. All three actual members had
native0, exact-Job membership and signaled state, Job3/0.
Row wall11.6525075s; measured outer including receipt16.9653855s.
Receipt48,153B SHA
`f1feca9d01dabeed8543fbb1cd479bfb6542dfb626219b8ecfbeaa3eb0018640`.
Parent acknowledgement/release passed. Repair admission SHA
`eb8b7d778f4c461db424a05a7cd040246f0f64904ef3a197f8bff5151118fc11`.
All pending registries are now empty. Historical N45's failed predicate is
**still unknown**; these new proofs supply no retroactive closure claim.

**Through SDK150:150/206,56 remaining.** The single approved3600s map was then
unused; N48 was preparing, not issued or charged. The execution below supersedes
that status. No automatic scientific retry or completion guarantee; no change
to other limits or A0–A6 qualification.

### N48/SDK151: full4416 mapping timed out; approval consumed — 2026-10-09

The actual approved **one3600s full4416 mapping attempt issued and finished
negative** under NETWORK_V3. No output directory or usable model exists.
Complete negative receipt52,216B SHA
`61b661be69c97370a3f223419cd8a9a57cade337195a2db59ec9daea8990e140`.
Row wall3600.6978683s is before publication; parent outer including receipt
3606.167526s. All three actual members—Python, `spirula` and conhost—had native43,
matching creation identity, exact-Job membership and signaled state, Job3/0.
**N48 closure was observed true**; parent acknowledgement/release passed,
pending registries empty. This does not identify N45's failed predicate or
retroactively close its historical family.

All8,838 payloads remained native/SHA unchanged; environment restored.
Stdout2,791B contains fresh physical-device admission only; stderr0B.
Before closing its original retained Job, the parent additionally queried
user49,771.28125s /kernel274.609375s and peak committed7,581,425,664B.
Across6,288 samples, maximum aggregate RSS was7,737,954,304B.
These observations show busy CPU execution, **not an identified bottleneck
or GPU peak**, and provide no registered-view coverage or scientific model proof.

**Through N48:151/206,55 remaining; one60-minute approval CONSUMED.**
No automatic unchanged retry or deadline extension. Parent then researched
a bounded progress diagnostic within1200s; the actual N51 diagnostic follows.
Reconstruction/parser/config/splits/source/reference/finite endpoint/A0–A6
remain unqualified. Preserve every older negative and all scientific gates.

### N51/SDK152: bounded progress diagnostic observed CPU BA fallback — 2026-10-09

Actual120s full4416 diagnostic, **not another long mapping attempt**:
removed `--quiet` and set only `SS_SFM_MAP_PROF=1`; preserved quality,
default precision, measured cameras and fresh AMD admission.
Native output was redirected to an owned network log with a hard1MiB cap:
actual38,495B SHA
`a792537ebedf79803a9893cb1b3211c19b6d1a92250726910a113495260cad48`.
No snapshots or unbounded logs. Expected deadline negative: row120.4378101s,
outer125.6930917s, Job3/0, observed closure true; parent acknowledgement/release
passed. Receipt48,028B SHA
`83f5e405f51d9e6b29aa96ace1ef7cef050069000009e111283b9b88bf38c654`.
All8,839 actual input bindings remained unchanged; no output model.

Observed exact log line:
`[ba] device does not support double arithmetic; falling back to cpu`.
The56 completed BA calls summed100.854s of reported solve time.
Last completed BA56:228 images/88,554 points/569,138 observations, solve8.050s,
including CPU Schur6.855s; last printed registration count251 images.
This prefix confirms CPU fallback and BA cost, **not exact whole-hour
bottleneck attribution or full mapped coverage**.
Default unused-feature compaction17,669,462→8,324,330 retained all4416 images,
zero-active images0: not a manual dataset reduction. The log camera summary
“fromEXIF” is not provenance proof; matches carry measured-focal flags,
with no factory-calibration claim.

Source4 documentation recommends opt-in AMD double-float BA; **both**
`--ba-real df --ba-real-coarse df` are needed for fine/coarse stages.
This is different approximately48/49-bit arithmetic. Historical documentation's
approximately0.01px observation is **not validation on this corpus**;
speed/equivalence remain unverified. **Through N51:152/206,54 remaining; precision
choice was pending, with no DF execution or extra60-minute approval then.**
The earlier one60-minute approval remains consumed; no automatic scientific
retry or silent precision/quality change. The subsequent approval/N52 follows;
A0–A6 remains unqualified.

### AMD DF route approved; N52/SDK153 missing shader, not DF validation — 2026-10-09

The user explicitly approved **“Validate AMD GPU double-float”**, with both
`--ba-real df --ba-real-coarse df`: bounded validation, then **one conditional
3600s full4416 map if warranted**, with all corpus/quality/resource caps unchanged.
Approval SHA
`55a251868a45ba00502a6368dcdb5b492c95b3f17cce3c804af73530c7a872a9`.
The conditional long-map authorization is **unused**, not an unconditional retry
or completion guarantee; N48's older approval remains consumed.

**N52/SDK153 actual bounded120s probe ended early on missing `ba_df_huber`.**
This was **not a timeout or a DF solve**. Source4's available shader list contains
only float/double variants for all three losses, matching its binary.
Actual AMD context initialized and freed: backing peak135,165,000B,
after-free current/reserved0. Row13.8034074s /outer19.0414246s;
observed closure true, Job3/0. All8,839 inputs unchanged; parent
acknowledgement/release passed, all pending registries empty.
Complete receipt48,752B SHA
`23499f6e84ddea0696dda79f120a6fc18b8429f730216d0da8dafbdf018b484d`;
bounded log5,989B SHA
`ba70e6f967e30c168a25bbceedda35083c137a5882fb100737287bf80cdd4df0`;
diagnosis1,696B SHA
`feb75200b9fd2c11adb1e5fefb3c5957dac67d5411568ca70335b2a54099bec8`.

Read-only Source4 documentation finding: `docs/build.md` lines173–181 records
Windows Slang2026.12.0.1 `E99998` compilation failure for all three DF shaders,
explaining Windows defaults of float/double. **Simply flipping CMake is not a
proven fix.** Through N52, compiler investigation was pending and no Source5
existed, ledger153/206,53 remaining. N53/N54 below refine that compiler evidence,
not DF numerical-equivalence/performance/model or A0–A6 qualification.
Preserve N52 and all failures; actual shader/compiler/build admission and bounded
DF validation precede consideration of the unused conditional long map.

### N53/N54: unchanged DF Huber compiled under debugger; build still unqualified — 2026-10-09

**N53/SDK154 native compiler-exception probe refused:** original pinned Slang,
unchanged production DF Huber source, O2 and dump-repro. Its90s wrapper cap was
too short; only the initial debugger breakpoint was captured, **no compiler
error**. Peak3,619,323,904B; root1/compiler3221226324 from debugger exit,
Job3/0 with observed closure true. All3,705 inputs unchanged.
Complete56,224B SHA
`5339a40b2f78efd4b12c953352de5913d23ea7af2552a6e3b87de92f0b67be80`;
negative1,163B SHA
`33d2a2da47f9e1ea893ce3c87d29739145a8fa84576a1c45dbd33bb028824d95`.
This is not reproduction of historical `E99998`.

**N54/SDK155 fresh named probe succeeded** with original unchanged compiler
and shader flags, wrapper1100s/native1200s standard limit. Root/all three
members native0, Job3/0; row190.2287838s /outer195.4877253s,
peak6,432,997,376B. All3,705 native/SHA inputs unchanged; parent finalized,
all pending registries empty. Complete59,483B SHA
`8885fe29785f865a9c1ababe874dc8a9fa2e021eacbe18f26b273e0b1d3856d4`.
Raw Huber SPIR-V10,580,012B SHA
`330867c75c50c4a10e203110400f9274db63d56880f75ad36fde50c78aa37ddf`;
compiler repro107,376B SHA
`07d052d86ebcd3a551c741ec55086efc25579a8202c7659f59bdac5ad73fc01e`.

This disproves a blanket assumption that current DF cannot compile, **not the
historical `E99998` failure or its unknown cause**. Debugger-conditioned success
is not ordinary-build proof. No stack overflow was observed; PE stack reserve
1MiB is a fact, not established root cause. Through N54, no-contract/all-three-
loss/plain compilation, complete Source5 build and GPU DF validation remained
required. N55 below qualifies the shader phase only; no scientific credit.

**Through N54:155/206,51 remaining; Source5 had not been created or built.**
Planned serial all-shader dev-script and application phases each retained1200s,
8GiB Job and application jobs≤2. Single-DF peak6,432,997,376B ruled out two
concurrent DF compiles under that cap. The following Source5/N55 supersedes
the then-unexecuted shader-phase status, not the historical unexplained failure.
Conditional DF3600s full-map authorization remains unused.

### Source5 and N55/SDK156: ordinary serial shader phase accepted; no application yet — 2026-10-09

Source5 is a separately frozen1,289-file Source4 copy. Only its
`build_develop.bat` conditional serial-SfM-shader phase and `docs/build.md`
scope changed; math and compiler unchanged. Base/public sources stay protected.

**N55/SDK156 ordinary dev-script shader phase accepted, without debugger:**
serial compilation of all14 SfM blobs, including three DF raw shaders,
no-contract processing and embedding. Root0, Job356/0;
row617.7798877s /outer628.4928210s, peak6,699,962,368B.
All21,958 source/toolchain/launcher inputs native/SHA unchanged before/after.
Complete133,706B SHA
`7dfb2bd20d8b0774b15ad71a1bdb8013f510f8168af686561764c73969fdea92`;
admission5,573B SHA
`cea623b6671e68d72ac2d5af721dd801c72260b2251c4510c9c69c3a77b05fbb`.
Huber raw10,580,012B SHA exactly matches the N54 debugger probe:
`330867c75c50c4a10e203110400f9274db63d56880f75ad36fde50c78aa37ddf`.
All six DF raw/final artifacts total80,148,408B; embedding184,127,179B.
Actual N55 native growth302,758,224B.

**Through N55:156/206,50 remaining; Source5 application was NOT built.**
The generic banner named `spirula`, but N55's actual target was shader-only,
not executable/GPU DF admission. The next application phase then had fresh1GiB
local admission with8GiB Job/1200s/max2 jobs; its actual N56 result follows.
Historical `E99998` remains unexplained; no compiler-fix or scientific credit.
Bounded GPU DF/model/A0–A6 gates pending; conditional DF3600s map unused.

### N56/SDK157: application build accepted; Main lost during post-build scan — 2026-10-09

**N56 Source5 application build V5 returned ACCEPTED**: native/Popen root0,
Job2679/0, row612.75609s /outer623.8783918s, peak7,967,461,376B.
`empty_job_verified_before_close=True`; native build-Job closure was verified
before the later Main loss. Compact receipt1,648B SHA
`7d298b77fb9d9558c31516c3ec625894112d9bf7474d68048575210b5b3d3d26`
and full ordered chunks were preserved. Retain this build-specific acceptance;
do not narrow it into a compiler failure or promote it to runtime admission.

Fresh parsing of the actual ordered N56 detail payload confirms format
`BUILD_NATIVE_JOB8_V5`, accepted true: **1,947 actual process witnesses,
732 explicit unobserved exited processes, four observed nonzero exits**.
These sum to Job total2,679; **not a complete individual-process roster**.
One exact bound `vctip.exe` was verified owned, then stopped with exit44 after
root0 under existing helper policy. This is the owned **build-specific**
`runner.py`, not generic `main-native-boundary-v5`. Earlier “no reported witness
count” was a documentation-only scout limit, not absence from the receipt.
Do not rewrite N56 artifacts or replace the732 unobserved exits with assumptions.


After return, the parent started a full21,965 source/toolchain/launcher/DF-shader
native/content integrity scan in Eval with a60s tool deadline. The tool reported
timeout, unresponsive interrupt, then **KERNEL KILLED /EXIT130: genuine
Main17360 runtime loss**, not the earlier false-absence notice.
**The interrupted postscan's completion remains UNPROVEN.** Recovery checked
durable evidence rather than assuming that cell completed. The fresh independent
Source5 integrity/image evidence below does not complete the old scan.
No child/native invocation occurred in that scan cell; the build Job was already
closed and verified. V3 pending registries were empty before N55/N56.

**At Main loss:157/206,49 remaining; fresh recovery/admission was required.**
No reconstructed old handles or identity continuity. The read-only recovery below
supersedes the then-unverified Source5 integrity status, not runtime qualification.
Main loss during postscan is not an established Source5/GPU failure.
No additional SDK/GPU DF solve/map; conditional3600s mapping remains unused.
Execution context/reconstruction/parser/source/A0–A6 remain unqualified;
no automatic scientific retry or completion guarantee. Preserve all failures.

### Fresh reader: Source5 integrity verified; historical frozen-bootstrap failure — 2026-10-09

Fresh read-only **Main8948**, creation `134360562270835901`, parent26760,
is a new observed identity, not Main17360 continuity.
Full execution bootstrap **FAILED**: the frozen `process_api` ancestor was
missing; separately bound `process_api_dependency` (`bench.py`) also returned
`WinError2`. Other seven frozen roles passed native/SHA checks. No cause/actor
inferred, old identity rewrites, restoration, waiver or SDK invocation.
At this stage authentic archive recovery was under investigation; missing bindings
did not authorize historical-dependency substitution or identity restoration.

Using only authentic Source6/producer4/rootc3 native-file primitives and
identity/ledger functions, the parent performed a fresh read-only full21,965
Source5 prebuild-input comparison: **all native identity/attributes/SHA matched**.
Receipt2,064B SHA
`01f9bd20c7dc61f4200e3a1ce609452608353e874caf9733d263f7b17a75af53`.
Original Source5 owned-root identity matched; fresh executable bindings:
- `spirula`:149,618,176B SHA
  `75a73db447260f5c3391eb013c3da032d10e06061e582657c9a9a3f5e5af4abb`.
- `sfm_map_test`:56,534,016B SHA
  `18ab371a41ef8cb94a28309c58ecf8ed854461ed6c98f9aec1530a32596b1c3e`.

Build-integrity/images receipt3,205B SHA
`20e14d6fc35432d45df296399a0a8a529b5326bee953dcfebab3a6c805182f49`.
This is **fresh integrity evidence, not old-scan completion, continuous custody
or runtime qualification**. Parent closed the fresh21,967 input/image pins after
publication; isolated-reader dependency pins then remained, with no execution context.
Incident6,693B SHA
`97faa99251ad4cff891e358cd1321366dae5123b711415b49785862298a98011`.

Recovery observed ROOT native bytes25,388,156,280→23,267,332,816 and
files81,464→80,632, local free28,032,126,976B. **No agent deletion/retirement
credit or actor attribution** follows; these are observations, not fresh combined
resource admission. Selected Source5 inputs are intact; current corpus has
**not been freshly checked**. Through read-only recovery:157/206,49 remaining,
no additional SDK/DF solve/conditional3600s use; execution dependency recovery
was then blocked. The explicitly authorized replacement below supersedes that blocker.

### Authorized replacement bootstrap entered; qualification still pending — 2026-10-09

The user explicitly selected **“Authorize replacement bootstrap.”** Missing
original helper bindings remain **FAILED historical prerequisites**; old native
identities are not restored, rewritten or treated as satisfied.
Exact archived bytes were recovered and only the two helper files copied into
a newly owned namespace with new native bindings. Current controls are freshly
constructed and do not include the missing old helper rows. Historical receipts
remain unchanged; this is an approved replacement route, not old-route repair.
Static review reported no findings; it is not native runtime qualification.

Actual **Main8948**, creation `134360562270835901`, parent26760, entered the
new versioned execution-bootstrap V2 and retains that context. A runtime-loss
notice was contradicted by actual PID/globals verification; reuse the observed
context, not a guessed reconstruction. This bootstrap version is separate from
the historically unadmitted NETWORK_V2 runner.
Bootstrap16,036B SHA
`66fa28304abc4b90ccb37ea903f19bf0cc203f172adef9afb8b785fa280705ad`;
fresh-entry receipt6,959B SHA
`45703682b2460bb8539b22056c5e705360afb7dbb72241808f215260f2a1f850`.
Unchanged NETWORK_V3 and26GiB scope policy were installed into the actual new
globals; fresh resource quote passed. Installation receipt6,924B SHA
`914a5cf67c91775374eed4b7c4c0db0a175d17068427e653161e2e6e88912f51`.
Exact-ID queries returning87 with a successful positive control do **not**
establish permanent absence or old native closure.

**At bootstrap entry:157/206,49 remaining; zero SDK since N56.** Fresh native
positive/deadline-negative/refusal qualification was then pending; actual N57/N58
below supersedes that status, not scientific admission. No DF GPU validation/
full map; conditional DF3600s authorization unused. Preserve historical bootstrap
failure, interrupted postscan and all scientific gates.

### N57/N58: fresh CPU boundary qualified; scientific input admission next — 2026-10-09

**N57/SDK158 positive accepted**: Popen root0, all three actual members native0,
Job3/0; wall11.2393478s /outer16.2634s. Receipt46,715B SHA
`2c2a852dd7d32c3d45c4c939542f8ecdbad2e4de156e346172987f0dc378a6e6`.
**N58/SDK159 passed expected60s deadline-negative contract**:
wall60.0440465s /outer65.1292s, all three native43, Job3/0, closure verified.
Receipt43,336B SHA
`8a44d1ac70272f7992c2c6624c31f2bc96a32ecabf86b6032175d282664a0114`.
Both were durably parent-acknowledged, finalized and released; current pending
registry empty, actual execution context remains open.

An actual wrong-Job witness on running Main was refused:
membership false, wait258, exit259. Pending-launch guard refused while preserving
original objects. Owned fixture write/delete opens were refused with Win32=32
while pinned; incorrect SHA/native identity refused, fixture unchanged.
Qualification receipt4,627B SHA
`f408802eff5c5fb9d57479c783f24270161cfe3a96705e749d105cab3b5335f8`.

**Through N58:159/206,47 remaining; fresh CPU boundary only was qualified.**
Scientific-input scan/DF probe were next and the conditional3600s map then unused.
Actual N59/N60 below supersedes that status, not quality/equivalence/model proof.
No automatic scientific retry or A0–A6 credit; historical missing-helper bindings
remain failed, not repaired by fresh-control acceptance.

### N59/SDK160 DF functional prefix; N60/SDK161 full attempt charged — 2026-10-09

**N59 actual300s full4416 DF probe reached its deadline**, root43, Job3/0,
all native closure observed. Complete47,842B SHA
`58fa1a9428c981d9b4da534d844da068212c2eedc58f35f6c6314770f81bd1cd`.
Fresh10,195 native/SHA input bindings passed before/after:
4,416 images +4,416 features +1,289 source files plus manifests/runtime/two
executables; pins continuously held. Log23,100B SHA
`cb7c890cc661d5a3e617768ba4cc05a2c67c3e7fbf0fae7aac14104e8552c29d`.
Prefix:42 GPU initializations,41 completed BA calls, reported solve sum260.955s;
last BA70 images/34,664 points/202,948 observations,12.834s,10 LM;
last printed registration77 images. **No CPU fallback/error lines in this
prefix**, not a completed quality/equivalence/speed claim or full coverage.
Diagnosis2,866B SHA
`8dd854f83ea527f3405c6089f1b8ecbddcdc2dd9843eb9c00f7f9a695dc3ac23`.
Durably parent-acknowledged/released.

Historical `NativePins.summary` still calls the missing original helper through
its original declaration; that observed failure is preserved. **No summary/
custody waiver or source-helper change.** Fresh pin counts above are direct
observations of the actual new instance, not old-summary acceptance.

**N60/SDK161 was CHARGED and its retained asynchronous task started.** At launch
completion was pending; it consumed the **one approved DF3600s full attempt**
after functional proof and fresh3,238,002,688B network-growth quote.
Same Source5/both DF flags/all4416 inputs and quality; standard `--quiet`,
`SS_SFM_MAP_PROF` unset for the unprofiled full run, original bounded1MiB log
wrapper, freshly owned network namespace. Allocation9,386B SHA
`1e0cc606b68a7ad14504eb6565e85cf8a2028728fcf643169dadae5c2e597068`.
At launch Main8948 task/context/native input pins were retained. Parent prepared
native-parser read-only admission while awaiting the task. The actual negative
completion below supersedes that pending status; no full-model or scientific/
A0–A6 credit, and no further full-DF approval.

### N60/SDK161 completed negative: device loss and commit-floor refusal — 2026-10-09

**N60 FAILED after300.8147016s /outer306.2656644s, not the3600s deadline.**
Complete50,969B SHA
`5ec5cf8f41d0a53410c4cb6c3a3c1a5539d388493319f4fd6f2b32e9ee0dd1dd`;
log4,049B SHA
`7a6fe572a1dcd14c6e1524cdc5129ed11f3f4857b766dc01bd1efe71c93a67f3`.
The log records `VK_ERROR_DEVICE_LOST` at `src/sfm/vk/VkContext.h:804`
and a CPU-fallback warning.
Guard observed available commit9,566,687,232B, below the9GiB
(9,663,676,416B) floor by96,989,184B. Simultaneous Main RSS141,328,384B,
aggregate RSS1,556,676,608B, local free27,787,415,552B,
network free284,744,888,320B; other guards passed.
**No causal relationship between device loss and the commit-floor observation
is established; do not attribute commit pressure or an actor.**

All three actual members native43, Job3/0, closure observed true.
All10,195 inputs again passed full native/SHA unchanged; sparse output observed
empty, no usable model. Durably parent-acknowledged/finalized/released;
all original input pins closed and verified. Main bootstrap context remains
held, **no pending Job**. Diagnosis4,758B SHA
`d653725d856088584a83c4a8791d817ed2023efda18c257db8182d821721659c`.

**Current161/206,45 remaining; ONE full-DF approval consumed.** Resource/device-
loss evidence and absence of another full-map approval are current blockers.
No automatic retry or lowering of the commit floor. The user subsequently selected
bounded AMD diagnosis below. Parser research located existing `SS_DUMP_CAMERAS`
native parse/bake before engine initialization, but full split/effective-config
metadata is missing. The ordinary finite endpoint is the `TrainerCore`
environment variable `SS_TRAIN_STOP_AT_STEP`, **not a test CLI flag**.
These are read-only source findings, not exercised parser/config/endpoint
admission. Model/parser/source/A0–A6 remain unqualified; preserve all negatives.

### Read-only Windows event diagnostic before route selection — 2026-10-09

Zero additional SDK calls. A last20-minute System-event filter for Display,
WHEA and resource exhaustion returned no events **within that queried window/
filter**, not a universal absence claim. Application WER returned two records
with the same ReportID `bfde0f0c-cfb9-4d3d-a494-7206414e798c`, same
`LiveKernelEvent` code141 at `2026-10-09T23:03:14Z`, statuses4/2049:
**one reported incident, not two faults**. A WATCHDOG dump was referenced,
but **no dump analysis or causal root-cause finding** follows; the queried
report-queue folder is currently empty.

Postclosure `GlobalMemoryStatusEx` available commit28,970,024,960B versus
N60 initial28,699,017,216B and minimum9,566,687,232B. This recovery does
**not attribute the earlier pressure or authorize another full run**.
Read-only diagnostic4,372B SHA
`ffec22dc2ba7911cc56c302891711d70460d07340bd3b631bbfb756d504c358f`.
At this diagnostic:161/206,45 remaining; route choice was pending between bounded
AMD investigation and explicitly longer CPU mapping. The user's selection below
does not authorize a new full retry/extension; retain all inputs/caps/gates.

### User selected bounded AMD device-loss diagnosis; full reconstruction blocked — 2026-10-09

Explicit user selection: **“Diagnose AMD device loss.”** Approval1,731B SHA
`f1d0728755c2c0d324b1cbfdcb253d7afb459672ad4a3f4f93589e7e73a0a3ea`.
**Bounded diagnostics ONLY; no full retry until understood and explicitly
authorized.** No longer-CPU-map route or fresh full-DF approval was selected.
Root cause remains open; full reconstruction and downstream admission blocked.

Exact frozen Source5 `src/sfm/vk/VkContext.h:804` is **`vkQueueSubmit`**;
fence wait is line806. The observed failure is **not an observed fence hang**.
Public-checkout line773 attribution is withdrawn; frozen executed source,
not current-checkout line numbering, controls the error location.
WATCHDOG-directory reading returned `EPERM`; no privilege escalation,
watchdog-setting change or dump analysis.
Existing `SS_SFM_MAP_PROF` is host-stage profiling only, **not GPU dispatch
tracing**; no actual BAProblem serializer/replay seam was found.

At diagnostic preparation, the installed API-dump layer was being investigated
for the current binary/actual corpus; no synthetic replacement/frozen Source5
edit. Then161/206,45 remaining, no additional native calls. Actual N61 below
supersedes that prelaunch status, not root-cause or layer-activation proof.
All resource/quality/input gates and negative history remain unchanged.

### N61/SDK162: repeat device loss; no API trace in captured stdout — 2026-10-09

Actual bounded600s diagnostic ran311.9703494s /outer317.2640107s.
`VK_ERROR_DEVICE_LOST` repeated; versioned wrapper stopped the native child
on error, root1 before continued CPU fallback, **not a deadline refusal**.
Job3/0, native closure true; durably parent-acknowledged/finalized/released.
All10,203 inputs full native/SHA unchanged before/after; pins closed.
Complete52,131B SHA
`1256c00c3a28b7ee0d5c8aaffaf6ef6c6b7d408534b4856fa37d5b44e1940177`.
Capture4,049B SHA
`cbf769382bb2fedbae3de82469f275f1ebe195303b6979dff3e5bf49c0b070b0`
captured stdout contains **application messages only, no API trace there**.
This did not prove absence in redirected files; source-proven correction below.

Minimum available commit27,351,928,832B in both guards and PSAPI observer,
above9GiB at all sampled points; **unobserved short transients not excluded**.
613 actual roughly0.5s system/process snapshots; all observer tasks closed and
results observed, no observer error/cap,39 chunk receipts. Worker private bytes
approximately1.495GB at minimum commit; protected-process coverage gaps remain
explicit. Native counter helper V2 uses documented limited-query rights,
without privilege changes. Diagnosis23,553B SHA
`0e4ff08e275ae4848b78b6a3b89569dcf7400430a8df91f25f9bff776ee9d996`.
These observations do not establish the device-loss root cause.

Separate zero-SDK preparation incident: caller unpacked a fresh empty-Job
factory's four returns into three, losing assignment. Exact current-Main handle
inventory found only one Job handle1052, zero ever-assigned/active, expected8GiB
flags `0x2200`; `CloseHandle` succeeded. Receipt1,011B SHA
`a7cdb38e3a2502542d6e4f53d5656630862c6927176b0d6c0a312bd6ddcb8b42`.
No old Main/Job reconstruction or child calls. Subsequent actual sampler smoke
verified exactly one handle-count decrement for closed empty Job and all process
handles closed. Preserve the incident, not inferred earlier-family closure.

**Through N61:162/206,44 remaining; instance-only activation gate was next.**
Main token non-elevated
(`TokenElevation=False`, type3); no Source5 CLI environment-clearing or
console-reroute path found. API `output_range=0-0` means **all frames, not none**;
frame counts are **not a byte bound**. Versioned capture wrapper retains hard
256MiB stdout cap and stop-on-device-loss; original Source5/data/runner unchanged.
SDK-tool DLL/JSON new bindings admitted, not layer-activation proof.
Full-map approval remains consumed; no quality/source/model/A0–A6 admission.

### N62/SDK163: loader inserted API layer; two missing native witnesses, pending retained — 2026-10-09

Instance-only actual Source5 `sfm ba` used an unused owned empty directory and
a deliberately unmatched device UUID. `resolveSelector`/`listDevices` returned
expected no-match **before BAL/model read or solver initialization**:
no logical device or GPU dispatch. Root1.
Loader debug positively recorded the exact API-dump DLL inserted and layer
enabled via environment; **zero API records in captured stdout**, not proven
absence elsewhere. Main non-elevated; later file discovery corrects the interpretation.

Native Job accounting total3/active0, but only the **root native witness**
was retained. Two short-lived descendants disappeared before polling.
All task results/capture complete, **strict full closure FALSE**; accounting
active0 does not replace the missing native witnesses.
Complete47,994B SHA
`bf14a34c55c06a3445f3646c4ac8c941bff1c07c9a957ab187014c7629d97183`;
pending diagnosis3,149B SHA
`1f71cfc3e962378a87a16b3f096999ac32e5963e2281e055f8045cbde900ddb5`.
Run identity `1e1b4c17883ebfaa8a43c7f29decb2a6`.

**Before disposition:163/206,43 remaining; N62 originals were HELD and launches
blocked.** Root-only observation did not satisfy strict closure, despite empty
accounting. No missing-handle reconstruction/automatic waiver. The user-authorized
negative-only release below supersedes pending resources, **not acceptance**;
missing two exit witnesses remain UNPROVEN. GPU root cause/full reconstruction/
model/parser/source/A0–A6 remain unqualified; preserve all negative evidence.

### N62 authorized failed-case resource release; short-child capture repair required — 2026-10-09

User selected **“Authorize failed-case release and repair.”** Parent published
the scoped N62 approval, freshly queried actual retained root/Popen native
witnesses, exact original Job3/0/0, completed tasks/capture and all unchanged
control native/SHA bindings. Then released **only actual known handles, the
empty Job/Popen and control/network pins**. Approval/release receipt1,360B SHA
`a67ce924f8d34fa88946c1c259fbcd40a270a30a34114cce55604ced9f054722`.

Actual original N62 state remains preserved in
`H2.network_v3_failed_released_runs`. Original `closure_verified=False` and
`accepted=False` are **unchanged**, and both missing descendant exit witnesses
are **permanently UNPROVEN**. Active pending registry became empty **after this
authorized negative-only disposition**, not normal V3 finalization or a pass.
No scientific/native-family acceptance credit, historical identity reconstruction
or waiver is granted by resource release.

**After N62 disposition:163/206,43 remaining; no additional call then.**
Required repair: suspended child/durable PID+creation/Main acknowledgement after
actual handles and Job accounting cover current family, then resume, followed by
short-positive/negative qualification. Actual N63/N64 below supersedes the
then-unexercised status, **not N62's permanently unproven exits**.
Frozen V3/math/data and all gates unchanged; full map NOT authorized.

### N63/N64: held-child capture qualified; instance-only file-output diagnostic next — 2026-10-09

New separately owned held-capture helper7,757B SHA
`0ecf7c54da1ef15266da6bbabf40dfd64e298f9efe5abe397eaecbc9a620a1c5`;
controller4,552B SHA
`0026325729061463affef47834ead9e5de7cd05a0b798202b010073e8f29783b`.
Frozen V3/Source5 unchanged. Child flags `CREATE_SUSPENDED|DETACHED_PROCESS`
avoid console attachment with explicit pipes; declaration binds actual original
Popen PID/creation. Main captures actual family handles and Job accounting
before atomic acknowledgement/resume. Source5 has no `AllocConsole` path.

**N63/SDK164 actual short Source5 `--help` accepted**: two actual native0,
Job2/0, wall11.4130555s /outer16.5922224s. Complete51,214B SHA
`f85b9a2d8cf961c7e002912afbea4d15fa5359d6577a8b3b2f2372d66a13028f`;
qualification3,441B SHA
`d158bda20a50c77c79bce2de8298838592a660c931de04fb942f4a38552fc5c1`.
**N64/SDK165 actual unavailable-UUID instance-only negative**: root/child
both native1, Job2/0, `closure=True`, expected `accepted=False`;
wall6.5838032s /outer11.8449579s. Complete50,253B SHA
`4b5f1e334429604db203fd9361c4142339fa49cfb3532c6d24c679a387dab981`;
qualification3,530B SHA
`cd0a0f0ecf3b3c3b63423799601b6ba0d9fdabe16b04d9ec13256047b251a489`.
Both tasks actually observed; external pins closed, ordinary parent
acknowledgement/finalization released all resources for these runs.
No conhost observed in these two families, **not universal future-descendant
coverage proof**. N62's missing exit witnesses remain permanently UNPROVEN.

N64 API stdout capture:12,519B loader log, **zero API calls in that stream only**.
Output SHA
`96b85a44dd22884a88c1a94f1eedbfe0b10c2a4eb61e9799c9799a43567c10b3`.
**Through N64:165/206,41 remaining; owned-file CPU instance diagnostic was next.**
N65/source findings below supersede the API-absence interpretation, not GPU
root-cause/full-trace/quality/source/A0–A6 qualification.

### N65/SDK166: API records redirected to suffixed files; earlier absence claims corrected — 2026-10-09

Actual CPU instance-only `file=true` diagnostic: root/child1, Job2/0,
closure true; complete49,640B SHA
`815dbb24267296fc49e2b597d2cf8b302d3375b33260df063bbac3010ee33e8f`.
Initial diagnosis4,019B SHA
`510cc856906184bfba149def144dc6295cfd5a5c0c2bc45f883da428f3f339fe`
recorded configured `api-dump.log`0B. Preserve that **file observation, not
actual absence of API records**.

Exact [Vulkan SDK1.4.350.0 source, `api_dump.h` lines556–601](https://github.com/LunarG/VulkanTools/blob/vulkan-sdk-1.4.350.0/layersvt/api_dump.h#L556-L601)
processes explicit `log_filename` **independently of `file`**, appends `.txt`
for text format and opens the selected output with truncation. This explains
redirected output; **not an AMD layer bypass**.
Two actual files first observed and now bound/pinned unchanged:
- Unexpected repository `stdout.txt`:159,578B SHA
  `be75f2f3eb6d29437dbf3c21f5b112c7a4a53c3d5acd77f5b912575fc7348486`.
- N65-owned `api-dump.log.txt`:159,579B SHA
  `d007b3e83d7d323d3ad4378479164cdb9504d9049676cf395e7c595fe76f7b99`.
**Both contain instance-create/enumerate/destroy API records.** No prewrite
file custody claim. The unexpected repo file lies outside owned output roots;
N61's hard stdout cap **did not bound the redirected API file**.
Later instance-only probes overwrote earlier output; N61 GPU trace is unavailable,
with **no historical trace/resource-qualification credit**.
Correction2,753B SHA
`31d63827cb11645f3b81466e2dd2b6e2ba58a76106c44fbe5d9a725590ad0a0a`.

**Through N65:166/206,40 remaining; all native Jobs finalized.** No artifact
deleted/moved; reserve159,744B out-of-root allocation in future growth prices
pending disposition. Removing `log_filename` entirely with `file=false` and
actual bounded CPU stdout qualification were then required; N66 below qualifies
that output route. Actual token non-elevated(False/type3), elevated assertion
withdrawn; Source5/V3 unchanged, no GPU/full-map or scientific/root-cause credit.

### N66/SDK167 bounded stdout qualified; N67/SDK168 ACK read failed before GPU work — 2026-10-09

**N66 actual corrected settings omit `log_filename`, `file=false`.** Bounded
stdout172,097B SHA
`162621b58d846e4309d2ad95154cebf27ee5abc98704683600f265c85f3fd866`
contains `vkCreateInstance`1, `vkEnumeratePhysicalDevices`4,
`vkDestroyInstance`1 records. Two actual native exit1, Job2/0, closure true;
wall6.7715601s /outer12.1570864s. Complete51,249B SHA
`54f371f2b0dd8172bcabb3f12c659e5efa7e873d23bab596d568266dba426524`;
qualification4,644B SHA
`6c32c788faaca572121fb47194910b9f504fb1d5221df8d0c2f9ef446ab14df3`.
Prior unexpected files unchanged/pinned; this qualifies CPU instance capture,
not GPU execution, failure attribution or historical N61 byte custody.

**N67 attempted600s full-input traced diagnostic FAILED BEFORE CHILD RESUME.**
Root ordinary `open(ack.json, "rb")` raised `PermissionError` errno13 immediately
after atomic acknowledgement publication. Child still suspended, killed during
cleanup; trace0B, **no child mapping or GPU work**. Both actual native1,
Job2/0, closure true; wall6.7111125s /outer12.4592478s.
All10,209 scientific/source/tool bindings native/SHA unchanged before/after;
12 memory samples, all tasks observed, no observer failure.
Complete54,220B SHA
`64042c940b9d6e77ba9b756862ed48d104e2c1828c55f1f0fc4dfb5244834d4e`;
failure4,852B SHA
`2e736497e0ce60310c3c515cf8f4d13bf83908cf1d5c9d0a33da4a5645cb1d7e`.
All actual resources/pins/tasks normally finalized/released.

**Through N67:168/206,38 remaining.** Exact native error unavailable; rename/
CRT share-delete mismatch remained a hypothesis. Owned native ACK reader V2 and
controlled actual-DELETE-handle fixture were then pending. Actual fixture/N68
below supersedes that status, **not exact N67 transient-owner attribution**.
No PermissionError retry/identity waiver; frozen V3/Source5 unchanged.
Bounded diagnostics only, no full-map/model/source/A0–A6 admission.

### Sharing fixture passed; N68/SDK169 captured bounded device-loss API trace — 2026-10-09

Controlled **zero-SDK** fixture passed: held actual DELETE-access file handle;
ordinary CRT read refused errno13, native READ/share7 returned exact payload,
close checked and absent-file `WinError2` preserved.
New owned held-reader V2 9,085B SHA
`8dbd2b4f3372a04cc8168688d6750f5087354138acf4857a694b91196b705831`;
fixture2,250B SHA
`c58d363d6ca5fbdc10b05bb16e2443022b327831167c1418d49025f5a1bf535f`.
This demonstrates compatibility, not exact N67 incident-owner attribution.

**N68 actual bounded GPU diagnostic captured device loss**:
wall443.2986158s /outer449.0396737s, below600s;
API trace188,168,197B SHA
`d5351a40c6649246627e09d52f1eb39bd2a091553c47f887cf3021302d7a9105`,
below256MiB cap. Root `device_loss_text=True`, `cap=False`;
two native exit1, Job2/0, closure true. All10,211 native/SHA inputs unchanged
before/after, full4416 images/features +Source5; sparse output empty.
855 read-only memory samples,54 chunks/10,633,952 serialized bytes;
all actual tasks observed, no cap/error. Guard minimum commit27,171,192,832B,
PSAPI minimum27,187,781,632B, no sampled floor breach; short transients
between samples not excluded. Complete54,935B SHA
`615440b418130479d9a09b35b0cebce47eb74da2477b53ed938486bcc08254e9`;
initial observed failure59,284B SHA
`bb4c9a9519c881bd8bbb663ae356705ada4f996f9286b050ef6af25017cf46ad`.
Native Jobs/input+handshake pins normally finalized/released;
trace pin remains retained long-term.

Actual parent trace parsing:128,164 API records,13 pipeline creations using
one module,1,137 submits/1,136 `VK_SUCCESS` fence waits.
Only error: final `vkQueueSubmit`, line2,775,174/time429,725,602µs,
command buffer `0000021E06C853A0`. Previous successful83-dispatch LM batch
submitted427,019,917µs, fence wait succeeded429,605,005µs; following failed
submission has identical83-dispatch pipeline grids.
Actual **DenseObsW**, not pair-Schur for this batch:
`jac_thin_prism_fisheye`2134, `point_prep`180, `schur_obs_w`2134,
`chol_diag`1;18 panel/update pairs and19 each triangular forward/backward;
`dp_accum_w`1067, `point_update`180, `cam_update`3,
`intr_update`1, `cost_thin_prism_fisheye`1067 (dispatch-grid values).
Actual device enabled `shaderInt64=1`, `shaderBufferInt64Atomics=1`,
`shaderSharedInt64Atomics=0`, `shaderFloat64=0`.
**Queue-submit failure neither attributes a shader nor proves that submitted
buffer caused loss; the prior batch may be implicated.** Source5 `real.slang`
lines63–70 CAS compares integer bits, not NaNs; contention remains plausible,
**unproven**, not a fix/root-cause finding.

**Through N68:169/206,37 remaining.** Marker/per-dispatch investigation was next.
N69 below adds instance-only capability evidence, not device-loss shader cause
or marker activation. Full retry/quality/source/model/A0–A6 unqualified;
frozen V3/Source5/input gates and historical negatives preserved.

### N69/SDK170: instance-only AMD diagnostic capabilities; no marker activation — 2026-10-09

Actual instance-only extension query plus held-reader V2 Source5 `--help`
**accepted**: root/child native0, Job2/0, closure true,12.065309s.
Both freshly bound AMD R9700 devices expose221 extensions, including
`VK_AMD_buffer_marker`, `VK_AMD_device_coherent_memory` and `VK_EXT_device_fault`.
**No logical device, GPU dispatch or crash-diagnostic-layer activation.**
Query9,562B SHA
`ae8b34063724afc9e63db0c332cd467ec1ca7f390f9dd30f5fbbbe7b00d9103b`;
complete53,025B SHA
`2037f152a0259dc36b180006e7145693fa1e98a80f93a5e872855f7c642c099e`;
capability25,677B SHA
`ec38b64b9d27f2f6c576f3f71824fa9fb876e04d3c8effd1063b061270caa341`.
Finalized; no pending V3 runs.

Actual frozen Source5 remains DenseObsW with integer-bit `uint64` CAS retry,
**not a NaN floating-point comparison**; contention hypothesis remains unproven.
Five queried `Tdr*` registry values were absent, read-only/no changes;
this is **not a finding about effective timeout thresholds**.
Matched SDK crash-diagnostic-layer source automatically enables AMD marker/
coherent features and uses a fixed64MiB per-device heap. Running queue/command-
buffer history drains, but separate timestamped YAML has **no hard byte quota**.
The layer has **not been activated**; heap bound is not an output-byte bound.

**At N69:170/206,36 remaining.** Independent bounded-marker Source6 was proposed,
with no new source/build/GPU call then. The zero-SDK copy below supersedes
source-creation status, not compilation/admission. Output bounds/full-family
capture remain prerequisites; bounded diagnostics only, no full-retry/scientific
authority. Preserve all failures and resource/quality/input gates.

### Independent bounded-marker Source6 copied; no build or GPU admission — 2026-10-09

New separately owned Source6 contains1,289 files/31,713,767B copied from frozen
Source5. All Source5 native/SHA bindings checked unchanged before/after; every
copy byte/SHA matched. Copy writers/pins closed. Copy-complete7,405B SHA
`f934ea763da49501460da1b96c87498b85352d45ca45e55cbcde74b0297f4dd4`
binds thirteen exact ownership-before-bytes chunks.
Two preparation errors remain preserved: missing `contextlib` corrected before
copy files, and first strict bind refused while its own writer was open;
writer then closed and all copy bindings checked. These are resolved preparation
incidents, not silently omitted or promoted into scientific qualification.

**Current170/206,36 remaining; zero new SDK/build/GPU calls.** Source6 bounded
marker header is prepared with reviewed host-snapshot-before-loss, fail-on-missing-
marker and query-readback/dump fixes. New debugger-held build-family helpers
remain unqualified; Source6 uncompiled/unadmitted. Frozen old source/math/
shaders unchanged, no full-retry authority.

Existing crash-diagnostic layer cannot currently be admitted: matched source
may write driver-sized `vendor_binary.dat` and YAML lacks a hard byte quota.
Historical N56 Job2679/0 has1,947 retained witnesses and732 explicitly
unobserved exited processes: preserve build-specific acceptance, **not a
complete retained roster or strict scientific V3 qualification**.
Current held-reader V2 qualification covers only actual observed two-process
GPU/help families, not a prospective large build. Fresh build-family qualification
and bounded-marker admission remain required; no model/source/A0–A6 credit.

### Strict V3 build-exit/reporting prerequisite; explicit disposition required — 2026-10-09

Frozen `runner-v3.py` line460 requires **every process witness
`true_native_exit==0`**, not merely root0 and complete native closure.
Configure/build families may intentionally use nonzero compiler/git capability
probes; **no suppression, exit relabeling or acceptance bypass** is permitted.
The scientific runner remains unchanged. A dedicated **build-only acceptance/
reporting contract needs explicit disposition before Source6 build**; future
capture must preserve actual nonzero exits and the original V3 refusal.

Line464 also requires aggregate serialized row below **1MiB−16KiB**.
An observed N69 witness was approximately364B before enclosing indentation;
historical N56's2,679-member family could exceed this transport bound.
Do not assume witness completeness or receipt publication fits from Job closure
alone, or reinterpret old compact receipts as strict V3 acceptance.
Build-family helper qualification and policy decision remain pending; diagnostics
are active, **not source/build/scientific admission**.

`SS_SFM_OUTPUT_LIMIT_BYTES=1048576` is a possible diagnostic storage cap only.
Actual Source5 `Pipeline.cpp` lines638–690 checks it **at publication after all
mapping/finalization**: no BA arithmetic change, shorter work or early execution
bound follows. It is not a completed run or evidence that this cap was used.
Current170/206,36 remaining, no SDK/build/GPU call/full-retry authorization;
preserve all gates and actual historical failures.

### Reachable Source6/helper preparation complete; build-only policy decision required — 2026-10-09

Source6 changed only `VkContext.h`:71,825B SHA
`39820e45c8de13f582c193c71076af1c766afcfa58d1115bf9d9b7b7890c2d65`.
Other1,288 copied sources native/SHA checked unchanged; both launchers differ
only by exact Source5→Source6 root substitution.
Marker-source-prepared-not-built3,174B SHA
`0e13e16c4e7cd7447abf25a5c9ad4beecc77013e6d010a99aff2c5d165c251e4`.
Debugger helper35,236B SHA
`ed391dd94cb924b42145592e00e8db69b66c62781ad067e04db7b6f672ede771`;
controller20,852B SHA
`409bba8eff968a622945aa60f6246e2d0c4fd4c8feeab25ada791891b3eca8a5`.

Static review's two failure bugs were corrected: never resume an unacknowledged
CREATE without checked termination; retain unresolved handles/processes until
Main-owned Job termination. Current review findings empty, **STATIC ONLY**.
Actual zero-process smoke passed Python compile/load for both helpers,
`DEBUG_EVENT`176B/create-info72B layout assertions, cohort127/128 mapping and
fourteen bounded input/path/cap refusals. Receipt2,384B SHA
`2b44d571fe452fef43f008b449b95762b01b1e9e5e5cc8628a907a32565fa338`.
**No native capture/startup/WOW64/timeout/build qualification.** An unknown
modern-ntdll startup site is still forwarded and may fail. Candidate is
**not installed for runs**; this is not operational build admission.

Concrete legitimate-negative probe: Source6 `tools/check_comment_length.py`
lines215–217 returns0 when `git rev-parse` returns nonzero outside a checkout;
the dev script invokes it. Original strict scientific V3 remains untouched.
**Before disposition, the user was asked to choose explicitly** between:
1. Reuse successful build-root plus natively empty Job proof, keeping all caps
   and input identities; explicitly **not** a full individual-child roster.
2. Retain full individual witnesses, with further actual debugger qualification
   and explicit legitimate-probe/nonzero-exit and report-chunk semantics.
Neither route was selected at that preparation boundary. The explicit approval
below selects the build-only root/empty-Job route; preserve original V3 refusals/
raw exits and do not reinterpret earlier runs as satisfying the new contract.
At that boundary170/206,36 remaining, no SDK/build/GPU/process launches;
diagnostics were policy-blocked. Source6 uncompiled/unadmitted; full retry
separately unauthorized, root cause/scientific/A0–A6 open.

### User selected build-root and empty-Job verification; verifier qualification next — 2026-10-09

Explicit selection **“Build root and empty Job.”** Durable approval717B SHA
`e49ab22d0096c772d651acda06f1f92f3f39afdf526638fef00edcaff181ce6b`.
Scope is **CPU build/qualification only**:
- Build root native and Popen exit0; kernel-owned Job active0.
- Actual original root and every **observed** process handle retain checked
  creation identity, exact-Job membership, signaled state and recorded exit.
- Preserve observed nonzero children; **no complete individual-child roster
  claim** and no scientific acceptance from root success alone.
Resource caps and input/output identities unchanged. Scientific/GPU diagnostic
V3 is unchanged; no full-map retry or relaxation of scientific gates.

A new versioned build-root/Job verifier uses retained V3 lifecycle/network guards,
**not old fallback cleanup that closes unresolved objects**. The earlier
all-zero/full-count attribution to N56 V5 was from the wrong generic helper
and is **withdrawn**: actual `BUILD_NATIVE_JOB8_V5` permits its documented
build-specific semantics. Only its existing exact-bound-`vctip` stop policy is
reused; the new verifier keeps observed nonzero exits and retained V3 cleanup.
User choice aligns actual prior build semantics, without claiming a full roster,
mutating scientific V3 or reinterpreting the732 missing witnesses.
New debugger helpers were **not selected**, remain uninstalled and have no
native qualification.

**At policy approval:170/206,36 remaining.** New verifier qualification and
fresh Source6 build were next, not launch-admitted by approval alone.
N70–N73 below supersedes that preparation status, not scientific admission.
Source6 remains uncompiled/unadmitted; full reconstruction retry unauthorized,
root cause/quality/source/model/A0–A6 unresolved. Preserve all historical failures.

### N70–N73: build-only verifier qualified; Source6 build-entry deadline failure — 2026-10-09

New build verifier V2 with local-growth pricing actually qualified under the
explicit root/empty-Job policy, **not scientific V3 acceptance**:
- **N70/SDK171 accepted root0, Job6/0**, two observed exits0/7 and four honestly
  unwitnessed exits. Scientific finalizer refused partial roster unchanged;
  global pending interlock refused, then build-specific finalization completed.
  Complete50,330B SHA
  `cfc58dacf4faaab3362a4afbe8ad7e5371db1896d8019ee12780ee51698e909c`.
- **N71/SDK172 root3 correctly refused**, Job1/0, closure true/finalized.
  Complete45,232B SHA
  `87926af387050d6ddd16b238d707ed01e647cb1adec94d4c6e5953805fb600bd`.
- **N72/SDK173 actual30s deadline negative**, root/child43, Job2/0,
  closure true/finalized. Complete45,737B SHA
  `2f0ba3fa1b0967866efa15d9cf5eff3cf474f4bdd22c28c5a921f6f1c7f7425e`.
These actual tests retain nonzero observed children, explicit roster gaps and
strict cross-contract refusal, not an all-zero/full-individual-roster claim.

All21,957 Source6/toolchain inputs were pinned and native/SHA checked before
build in102.38s; **pins remain held**. No post-build integrity conclusion assumed.
**N73/SDK174 shader-build attempt FAILED at entry**:
1200.1086925s /outer1206.0298s, zero stdout/stderr.
Job5/0; three observed `cmd`/`findstr`/conhost native43, two unobserved exits;
build-contract closure true/finalized. Complete71,983B SHA
`38153a8ad030ea8746fb36a3e15b3110afe3934d5542a4ee8e62347ba5ac1057`;
failure1,800B SHA
`c7da30d2f5725bbf756d9a0ac0927d8485da2cd1112347c954e085a998655e9d`.
No GPU run/model/new Source6 binary; preserve this failed build separately.

Actual historical N56 build V5 launch flags were
`CREATE_NO_WINDOW|CREATE_SUSPENDED`=134217732, while derived build V2 used
`DETACHED_PROCESS|CREATE_SUSPENDED`=12.
**Through N73:174/206,32 remaining; no-window repair/pipeline qualification
were pending.** N74–N76 below supersedes that status. Flag difference observed,
no exact causal attribution; scientific/GPU V3 and all caps/identity/quality
gates unchanged. Full reconstruction retry remains separately unauthorized;
source/model/A0–A6 unqualified.

### N74–N76: no-window pipeline and Source6 builds accepted; runtime pending — 2026-10-09

**N74/SDK175 exact echo/findstr no-window pipeline accepted**, root0,
Job4/0,11.8771594s; stdout matched, finalized. Complete48,315B SHA
`9aafbe2f3406d74345b5f092524df1d90caad484a2b280e524a036de5fc6af6c`.
This qualifies the observed corrected pipeline, not exact N73 causal diagnosis.

**N75/SDK176 serial shader build accepted**, root0, Job356/0,
107 observed/249 unobserved members,624.3656131s /outer630.4359918s,
peak6,700,838,912B. Exact owned `vctip` stopped44; all known handles/tasks
finalized under approved build-only contract. Complete123,465B SHA
`8ce5a338a3a75e34bf3b1bb4caa1d7ff61d333ee0084ec2a5960dda8925c28dc`.
All21,957 inputs native/SHA unchanged.
**All17 SPIR-V outputs** (14 blobs +3 raw DF) and184,127,179B embedded C++
are byte/SHA equal to Source5. Admission22,943B SHA
`784402a7e5e4885a0b4355d6158759d9da2da93f46e9ee1cbdc17762198c415e`.

**N76/SDK177 application build accepted**, root0, Job2679/0,
1,883 observed/796 unobserved,627.0561534s /outer633.0324443s,
peak7,564,914,688B. Exact owned `vctip` stopped44; finalized.
Complete863,119B SHA
`529b8d85ec64987f201ce62221ddde77c2b29f776501d09edfc973e7a4ca24ca`.
Post-build21,975 source/toolchain/shader inputs unchanged; **external build
pins CLOSED**, superseding earlier held-build-pin state. Fresh Source6 images:
- `spirula`:149,620,736B SHA
  `5ba1cee7a13389623b5ef8ff99cb4a067239d9802a23e98e0e6b5ba6e958c182`.
- `sfm_map_test`:56,536,576B SHA
  `69fc3f72ee1a69a48f6d71145f1fe37d023dff12047c8d392e5728a8c866340c`.
Compiled manifest7,339B SHA
`a36295b4c53e45543a92133a8dc8a50b6a90999972ca83775dc2663ba14da5d5`.

**Through N76:177/206,29 remaining; build-only compilation, marker runtime
then unexercised.** N77/N78 below supersedes that status, not scientific proof.
Build verifier V3 restored no-window pipeline/build successes, not exact N73
causal attribution. N73/local-growth V1/V2 history, scientific V3 and all
resource/input/quality gates remain preserved; full reconstruction retry unauthorized.

### N77/SDK178 CPU test; N78/SDK179 GPU markers observed completion gap

**N77 existing Source6 `sfm_map_test` PASS**: root/child0, Job2/0,
strict closure/finalization;23.9479897s /outer29.7782692s.
Actual log: `[ba] device does not support 'double' arithmetic; falling back to 'cpu'`.
No marker initialization; **CPU-only test, not GPU marker/DF proof**.
N77 runtime PASS/closure stands, but artifact custody/scientific acceptance is
**FALSE** due to the newly observed fixed-selftest output ownership gap below.
Complete53,864B SHA
`18d3c2644cbd8bae097b766fe17ad28535250c1f0f0207ddde79644cec12e95b`;
result3,796B SHA
`a64942895f281c680b952120031249e887734ef781a7c9c9a3e7e989d38a7799`.

**N78 bounded600s diagnostic**, same full4416 inputs/DF in both phases,
Source6 markers ON, no API-dump/CDL.1MiB sparse-output cap only at late
publication, **not BA work reduction**.
Actual324.4445366s /outer330.6674081s; root/child1, Job2/0,
strict closure true/finalized. Complete51,326B SHA
`84f3dd576e5b24f2ffda1543e26790a2126a0872b50d2c3c5fbd94c20f1e8f5a`.
Marker GPU actually enabled and completed many batches.

**First observed missing completion: batch989**. Fence success was reported,
yet top3/83 and bottom2/83: Jacobian/point-prep complete,
`schur_obs_w` top3/bottom0, following80 dispatches zero.
Previous988 had all83 complete in a retained host-owned snapshot.
Current grid2097,268,358 observations/48,264 points/dimension584,
lambda float bits `3a91a2b4` (previous `3b5a740e`).
Exited on **diagnostic validation failure after reported successful fence**,
**not `VK_ERROR_DEVICE_LOST`** (no literal in log). Missing markers do not
by themselves prove a TDR, shader hang or root cause.
Raw log251,004B SHA
`6e64ccbe00a4c441d286e95179b32f0ab4b1bdb0ab93d4fc47dbcd6f959acef7`;
analysis23,606B SHA
`99cac01221b2a4b699b859d5cbe9ffe9d0853f3daa5fe1989bc1e2b859d5a3e4`.

All11,502 native/SHA inputs unchanged before/after; input pins closed.
628 memory samples/40 chunks, minimum sampled commit25,586,479,104B above
floor, peak Job2,532,405,248B; **no between-sample guarantee**.
**Current179/206,27 remaining; Source6 now frozen executed, no in-place edits.**
Read-only marker/integer-CAS review and capture feasibility now recorded below.
No full-map retry approval, model or quality/source/A0–A6 admission; preserve negatives.

### N78-window Windows event evidence; N77 fixed-selftest artifact custody gap — 2026-10-10

Additional **zero-SDK read-only** native event query, N78±120s:
System Display/DxgKrnl0, Application WER6 records. **Not six fresh incidents**:
old reports were republished; fresh ReportID
`87adf718-fe8d-4569-aa39-94baa50426f9` has `0x141`, status4 at
`2026-10-10T05:50:22.5181870Z` and status2049 at
`2026-10-10T05:50:22.6299811Z`, naming a new WATCHDOG dump with consistent
local-time naming. Other IDs include the historical N60 `bfde…` incident.
This supports a **contemporaneous display-engine timeout**, not root cause or
individual-kernel causality. [Microsoft's0x141 primary documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x141---video-engine-timeout-detected)
defines display engine failing to respond timely and states this code identifies
**live dumps, not a real bug check**. Event receipt14,172B SHA
`acd88e40b20143b5d12f7ef713fb810b30bde1b9583230890001755df7206bd6`.
Exact new dump native metadata query **ACCESS DENIED**; no dump contents read,
elevation or privileges changed. Metadata receipt926B SHA
`1566206bdbf4c3e6b035f0faca09365fe95281881474fff6681e081daf872fec`.

Static marker review found no concrete bank-reuse/visibility defect; dimension
bounds show no OOB. This is **not proof of defect absence**. DF atomic uses
integer-bit `uint64` CAS, not NaN float equality; contention/timeout/compiler
remain hypotheses. Read-only feasibility is complete: no existing pre-Schur
state capture/replay seam; only generic `VkContext` transfers. No fix admitted.

**N77 ownership gap:** selftest lines420–428 writes three COLMAP files to a
fixed temporary model outside owned ROOT. Directory/file births predate N77,
modification times fall within it; **no prewrite ownership or original bytes
were captured**, so old-output preservation cannot be claimed.
Three current files are only post-observed native/SHA bound/pinned:
50,528 logical/53,320 native-allocated bytes; no deletion/restoration.
Gap4,628B SHA
`4ea3d488c136bd2625d3581edae87dd1ad83f7aaf7e026169401464b1de29feb`.
CPU PASS/native runtime closure remain authentic; **artifact custody/scientific
acceptance FALSE**. Every future growth quote must reserve this extra53,320B
**in addition to159,744B prior spill** pending disposition. Future selftest must
not repeat the fixed output path. Current179/206,27 remaining; no new SDK/full
retry authority/model/source/A0–A6 admission. Preserve all negative history.

### Historical feasibility/route decision before exact-state GPU selection

An exact future GPU replay requires a **new independent source identity** and
a narrow `BundleSolver` capture/replay seam: actual push constants/grid,
module/device identity and input bytes, with unique alias buffers bounded.
About115MB was an **estimate**,512MiB then a **proposed cap, not measured or
approved**; subsequent selection below bounds implementation, not measured bytes.
N78 transient buffers were not captured; **counts/lambda cannot
retrospectively reconstruct its exact state**. Submission/copy instrumentation
perturbs execution; future capture/replay is not automatically equivalent to N78.
**At this feasibility checkpoint no Source7 existed; frozen Source6 stayed uneditable.**

The alternatives presented at that checkpoint were:
- Existing native **CPU BA**, bounded1200s/full4416-input trial, without input
  or quality reductions and with unchanged caps; **not authorized or selected**.
- Continue AMD **exact-state capture/replay**, requiring independent source and
  bounded seam; **subsequently selected**, as recorded below.

No further native call pending/performed by this feasibility step.
All179 charged-call jobs finalized; pending registry empty, **not a retroactive
closure claim for historical unproven failures**. Ledger179/206,27 remaining.
Fixed-selftest external allocation53,320B remains pinned/reserved in addition
to159,744B prior spill. Root cause, production model and A0–A6 remain unqualified.

### User selects exact-state GPU diagnosis; historical Source7 clone/implementation checkpoint

The user selected **“Continue exact-state GPU diagnosis”**, not the CPU trial.
Durable approval2,604B SHA
`97fef32e8462c84967ec2974bee6a0623d8bb291abdd073b39c02419fd92f025`.
At approval charged ledger179/206,27 remaining; N79/N80 below advances it.

New owned **Source7** was cloned from frozen Source6:1,289 files,
all source native/SHA unchanged before/after, all new clone bytes SHA-equal
at copy admission. Copy8,001B SHA
`f96adaba0bbdb0c7270fc7b9502102a415d52dac2c4829b30826c279346b62b3`.
This is clone evidence, **not a final edited-source binding**.
At clone checkpoint implementation began **only in private Source7**, with narrow
read-only review; Source6/public unchanged. N79/N80 build evidence follows below.

Implementation contract:512MiB per pre-Schur capture, maximum4 preowned
immutable slots/2GiB; minimum260,000 observations is **diagnostic capture
selection, not dataset/quality reduction**. Retain original DF pipeline/no
shader changes; flush actual snapshot before Schur, explicitly acknowledge
fenced capture perturbation, and check source SHA/GPU UUID/aliases/ranges.
Replay-only early mode must avoid the fixed-selftest temporary output.
These are **implementation bounds, not measured capture sizes or qualification**.

No new full-map approval, CPU-route authorization, model or scientific/A0–A6
admission. Capture cannot retroactively reproduce N78 state. Existing caps and
both external allocation reservations remain; preparation/build/replay only
once qualified and priced. Preserve all failed/unproven historical evidence.

### Source7 prepared; N79/SDK180 shaders and N80/SDK181 applications built

Final six-file Source7 implementation has **no remaining concrete blocker in
static review only**, not runtime qualification. Prepared9,313B SHA
`cf8a6c5f3341e6fa831de013cda6cab02cdc73ec55482894483ade59d652b529`;
1,290 source+20,668 toolchain=21,958 pre-build inputs.
New helper preownership notification race resolved by actual ID/birth matching
parent's zero-byte precreation; **no helper custody gap**. Receipt1,327B SHA
`707de249e48dec4e8ee0ec797300ca70fb1d9bcf6f824da368583a0d924b7422`.
This does not repair the separate historical N77 fixed-output gap.

**N79 serial shader build accepted/finalized**, root0/Job356/0:
114 observed exits/242 unwitnessed,625.0115466s /outer631.0262504s,
peak6,703,964,160B. Complete127,274B SHA
`bcc4dea0d58e75b56f399ff1c354c91891b232f6e13f754ff882b697b08822bb`.
All17 SPIR-V files and embedded C++ **byte/SHA-equal Source6**; admission22,445B SHA
`62c365aacc9c23684d1ddb7929e9e4764345e3aaa690ce6e6391c7bd48f8437f`.

**N80 application build accepted/finalized**, root0/Job2679/0:
1,938 observed exits/741 unwitnessed,632.3367481s /outer638.47703s,
peak7,701,663,744B. Complete888,273B SHA
`80e71d06b6ad3a50a95c79df123d9dd1b58df02e7a26219d2966b99fd46f9690`.
21,976 source/toolchain/generated inputs unchanged after; **build pins CLOSED**.
Both builds use CPU build-root/empty-Job admission, **not full individual-exit
roster or scientific/GPU qualification**; unwitnessed exits remain explicit.

Compiled Source7 manifest4,740B SHA
`6bfdcb7339fc1966f131b9ecf1903e54a2ebea5541a10412eb6e72e8bf204c4f`.
`spirula`149,638,656B SHA
`8714193ec6bc67537f073ed784241d622bd27214226e0ebb038c1ecddf6658c6`;
`sfm_map_test`56,576,000B SHA
`c045cd57b100e715a678b95c0644a72f6e3b9389997157b2a4876a615eaaa338`.
New parser-only `--validate-schur` and early `--replay-schur` bypass default
selftest; armed capture refuses CPU fallback/final-model publication.
**At N80 capture/replay runtime was unexercised; N81–N83 below supersedes this.**

Four preowned empty capture slots native-pinned with access0/share read-write/
no DELETE, plus all ancestor directory pins. Actual in-process exclusive writer/
pricing-metadata open compatibility passed **zero SDK**; this tests sharing,
not actual GPU capture/flush/replay. Slots7,523B SHA
`fdf2cd7fa7241284f71017e7c754b892122875e40211377ae8c13dcca2537aa2`.
**Through N80:181/206,25 remaining**. Caps/reservations unchanged, no model,
scientific/A0–A6 credit, root-cause proof or new full-map/CPU approval.

### N81/SDK182 parser refusal; N82/SDK183 capture; N83/SDK184 isolated replay

**N81 `--validate-schur` eight-byte fixture** actually refused truncated snapshot
prefix,root/child1 Job2/0 strict closure/finalized,7.6914196s.
No child GPU dispatch/default selftest; **wrapper physical probe only**.
Result1,619B SHA
`a1afc227eebecc9998ce5769ec5171e0d84b100ea06a35b4a317fa50c91a6d19`.

**N82 actual full4416 DF capture**,1200s cap, ended by expected **four-slot
exhaustion**, not GPU failure.317.2569965s /outer324.0839552s,
root/child1 Job2/0 strict closure/finalized. Complete53,173B SHA
`7841b1114afa87214bbb33e475af67ed2c045e89a023071081b3fa3254318359`.
All four snapshots durably closed before Schur; corresponding post-split
batches81/81 complete; **no GPU failure observed**.
Each105,937,273B,total423,749,092B;39 unique buffers/
105,936,420 payload bytes+47 aliases per snapshot.
270,647 observations/47,379 points/94 images-frames/2 groups/dim584,
poseDim564/intrinsic scalars24/grid2115.
Slots0–3 lambda bits respectively`3c23d70a`,`3b5a740e`,`3a91a2b4`,`39c22e45`.
Same preowned native IDs/births/all native-SHA inputs unchanged.
Slots long-term read-pinned; initial write/namespace guards closed after binding.
Sparse output directory actually read **empty**.
612 memory samples,min sampled commit25,320,480,768B,
peak Job2,537,975,808B; samples do not prove between-sample headroom.
Result27,621B SHA
`a8c8d76dcdbb31fdedbf2769bd76b99ba775f77ceb92f65b120aebcf2c0afe7c`.

**N83 isolated original DF `schur_obs_w` from actual slot2**, same GPU/source,
accepted root/child0 Job2/0 strict closure/finalized,
17.9148609s /outer24.0169782s. Complete55,808B SHA
`a2efc4f6324a736e4521de6f8f2b6c8e3cc40d9e81d605ef2f92d2e53719c0e8`.
Actual single dispatch TOP1/BOTTOM1; GPU profile1942.12ms,
host fence1.957615500s. S170,820 finite/0 nonfinite,
maxabs28,737,584,863,262.062;g584 finite/0 nonfinite,
maxabs375,528.39042866975. GPU peak240,219,684B,after-free
current/reserved0;peak Job1,532,465,152B.
Result4,741B SHA
`6df64fa7a15a6159b1a445bed6e5f4d700fa86d2e560407cd88fed3ed7d715cc`.

**Through N83:184/206,22 remaining; capture/replay exercised, not N78 state**:
observation/point counts differ, notwithstanding matching slot2 lambda bits.
Neither full-model numerical equivalence nor root cause proven. Long kernel
about1.94s supports **watchdog-exposure hypothesis only**; threshold unmeasured,
no exact cause established. Subsequent identical replay/negative guards follow;
no new production/full-map retry/model/scientific/A0–A6 approval.

### N84–N88/SDK185–189 repeat replays and refusal guards; Source8 comparison in progress

**N84 same actual slot2, Source7 markers ON** accepted root/child0 Job2/0,
15.6380699s. GPU1923.86ms/host fence1.926157700s,
S maxabs28,737,584,863,224.875,g375,528.39042865485,all finite.
Result SHA
`d0f53ba54e207002d1077bff547dbd5184f619c723f04c181816531ae30800a1`.
**N85 identical slot2, markers OFF** accepted root0 Job2/0,16.3540411s,
GPU2103.11ms/host fence2.105558900s,
S maxabs28,737,584,863,275.938,g375,528.39042813331,all finite;
GPU peak240,154,148B/after-free0. Result SHA
`64474ebda539f133d4a957afdf927661ca610aedd9abaa9256ca200b78368bb4`.
**Successful dispatch over2s means no established hard2s threshold**.
These outputs are **aggregate-only**; differences are not full numerical
comparison or equivalence evidence, nor marker overhead attribution.

Zero-SDK workshape: maximum track82,sum track²4,521,917,
unordered including diagonal2,396,282. Receipt1,935B SHA
`eecd32891fa22f9703d23f359ab2abeda4b2a6982c700c51b6388742a7d4d9e5`.
These are workshape counts, **not CAS retry counts or root cause**.

New owned **Source8** clone1,290 same-SHA files from Source7; copy7,896B SHA
`70fed16a926c8fd64e3d22032460f60ce69ff92435488597029695801cc989c9`.
At clone checkpoint three-file private diagnostic implementation was in progress:
`VkContext.h`,`SchurCapture.h`,`sfm_map_test.cpp`.
Optional `vkCmdDispatchBase`/pipeline flag, unchanged SPIR-V, baseline versus
64-workgroup chunks/full S/g comparison planned. **No tolerance/PASS/equivalence
claim, no default dispatch change; then unbuilt/unfrozen, later evidence below.**

Actual negative guards, all root/child1 Job2/0 strict closure/finalized:
- **N86 invalid-point fixture**: `observation point disagrees with range`,
  8.0730506s.
- **N87**: `snapshot source-manifest SHA mismatch`,8.1725138s.
- **N88 second AMD**: `selected GPU UUID differs from snapshot`,8.1343947s.
N86/N87 refused in parser before logical device; N88 initialized second AMD
context but dispatched **no captured kernel**. After-free current/reserved0,
peak134,283,264B. Original inputs/fixtures native-SHA unchanged.
Observed reasons4,154B SHA
`d7622dce54c5712b1adac6873f719abf00ab4c8944a5c1ae6e65a813051e4381`.

Main PID8948 remained alive, verified despite false absent-runtime notice;
pending0. **Through N88:189/206,17 remaining**; then N89 shader build
not allocated. N89–N93 below supersedes that status. No model/full retry authority;
root cause/A0–A6 unqualified; preserve N77 custody gap and external reservations.

### Source8 frozen/built; N91–N93/SDK192–194 comparison refusals/failures retained

Source8 freeze5,884B SHA
`6a815746850750fdaf77474b44ab00549def2f48084e5e46d9e35e7bf4628b98`.
**N89 shader build**, root0 Job356/0,626.1251249s,
99 observed/257 unwitnessed exits,peak6,701,244,416B.
Complete122,901B SHA
`9116b11bd0cc4c049fd17407bf6244d6b630a89ccd4c9da23a2e5199a3541051`.
All17 SPIR-V+embedded C++ exactly Source7; admission11,285B SHA
`386782679a612ba4e4fbca9947c03e8b4d294114c599dff42bcfe6cb4ce82dde`.
**N90 application build**, root0 Job2679/0,621.9370362s,
1,903 observed/776 unwitnessed exits,peak7,630,942,208B.
Complete873,928B SHA
`6206984f4a7419a5310cbf15b2e0cec7fb385d70ae875abfa00131993af44d09`.
All21,976 post-inputs unchanged; build pins closed/finalized.
CPU build-root/empty-Job contract, **not complete individual-exit/GPU proof**.
Compiled manifest4,116B SHA
`4e86a3e563e4d2f4059154dcb3a22b8166e2a79edb657266a8456caf0f4b471b`;
`spirula`149,639,168B SHA
`eb5b58c9779760bef672f799ad042b3fb5b65ec311657f7e1f0af6923876a8b2`;
`sfm_map_test`56,585,216B SHA
`ec2a20fdbb251be57d46c4ba8703c81c4ac49c8efaddec2043c508fff7edf5ce`.

**N91 `--groups-per-dispatch 0`** expected actual refusal
`groups-per-dispatch must be positive`,root1 Job2/0,8.7617882s.
**N92 marked comparison FAILED**,root1 Job2/0,13.0926693s.
Baseline original `vkCmdDispatch`2115 **on opt-in pipeline** completed,
GPU2222.95ms/host2.2253222s. First three64-group chunks, bases0/64/128,
host0.0892326/0.0865663/0.0866716s; **fourth base192/count64**
reported fence SUCCESS but TOP1/BOTTOM0. **Failing-chunk duration unknown**.
After-free GPU0,peak Job1,528,934,400B;
23 memory samples,min sampled commit26,096,730,112B.
Complete53,112B SHA
`de236009bb9b99317d3640279a83727a987b142fa414b58ad3a0be31841cec83`;
log26,674B SHA
`7fbd0ecc9d36fb204458ba4d520e14e4e84381c6893abcfaea27d4a3d87134ab`.

Event window14:49:35–14:53:48UTC: System0/Application23 WER records,
**not23 incidents**; old reports republished. Fresh141 ReportID
`8b60d074-2cc5-4c5e-97b1-2662f1030ffb`,status4
`14:51:42.0901329Z`,status2049`14:51:42.3810980Z`;
fresh AMD-watchdog ReportID`98099f84-ef08-4095-a0ae-8a773141b1bb`.
Event50,059B SHA
`759fae3c167f0ba31d9acb556ba5ce0c28884b8f2564ff0e68a11d9428d675f0`;
24,130B SHA
`5521bf889d449f87fe4313d10732f6bd8112f551afd62e2a85e14de2f17b9d2f`.
Static review found no concrete bug; required combined marker coherence valid,
**no cache-invalidation fix proven**. [Vulkan `vkWaitForFences` specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html)
explicitly permits SUCCESS **or DEVICE_LOST when device loss occurs**; fence
SUCCESS alone therefore does not prove healthy completion.
Chunk4 pairs295,567<chunk3 363,295, **not CAS-retry/timing/root-cause proof**.

**N93 separate markers-OFF control FAILED120s deadline**:
root43 Job2/0,120.4978266s,strict closure/finalized.
Log stops at baseline original dispatch submitting; **no chunks/full S/g output**.
Peak Job unavailable; **no after-free GPU ledger**.
232 memory samples/29 chunks,min sampled commit27,007,393,792B;
sampled minima do not prove sub-sample headroom.
Complete53,026B SHA
`ecf5c952801db32170f304ae2082443826c943f3f740250ba82271e2bc288d80`;
log4,259B SHA
`3ac909fa2c124f47410f9043704156d9d9c84b5468a5954c8bc3bfa99e438146`.
Combined durable N89–N93 result10,608B SHA
`427b19c8de2c71da5d44944c279f1f8e2a2cd4f03fdecf559db622dcd2bb9ab8`.

**Through N93:194/206,12 remaining; no pending jobs.** Preserve both GPU failures;
no full S/g comparison/runtime qualification/root cause/model/A0–A6 credit.
Blind GPU repeats stopped. The then-proposed route recommendation was a
bounded CPU BA arithmetic trial on **same4416 features/matches**, both
`--ba-real cpu --ba-real-coarse cpu` (N60's corresponding flags were both DF).
**Not GPU-free**: mapper BA context and GPU-capable focal bootstrap remain.
CPU route **not yet approved**; no new full-map/production retry authorization.
Source8/producers remain pinned, old inputs/N77 scratch unchanged,
external reservations and all historical negative evidence retained.

### Fresh runtime qualification and N94–N96/SDK195–197: restored-GPU comparison failed — 2026-10-15

The user's **“GPUs should be restored, let's continue”** authorized continued
GPU diagnosis, **not CPU BA or full reconstruction**. The kernel genuinely
replaced oldMain8948: fresh **Main18096**, creation`134365630996751114`,
parent16924, is a new identity; **no old-handle continuity is claimed**.
All10 frozen bootstrap roles and the unchanged bootstrap passed native/SHA
verification; no helper replacements. Fresh context, unchanged NETWORK_V3
and the26GiB guard were installed. Parent `H`/`S`/`fresh_context` are now alive;
old `H2` variables are gone. Reuse only the observed fresh context.

Fresh owned namespace`gpu-restored-runtime-0f296f94fbaa.owned`,
case`post-restoration-d52f68884963.owned`; private absolute paths stay local.
Entry SHA
`81325a49371d825f56a8aa5e195a439960c71099064d62c70dee41c381ad47b7`.
All37 prior controls and1,290 Source8 source files were native/SHA checked
and pinned; executable, Source8 and original snapshot unchanged.
Before qualifiers, native scope admission observed combined logical/native
allocation23,865,848,546/23,995,602,344B, available commit43,897,810,944B,
Main RSS56,844,288B. These are admission observations, not measured peaks.
All existing caps and213,064B side-effect reservations remain unchanged.

**N94/SDK195 CPU-positive native-family qualifier passed**:
actual root0 Job3/0,wall22.4243696s; complete SHA
`ff60aa5965e1a8a82d7cf006473062dd06ee496c7d83ae7ba21085062e7dbad5`.
This qualifies the fresh runner, **not a CPU reconstruction route**.
**N95/SDK196 intentional deadline-negative qualifier passed**:
actual root43 Job3/0,wall60.044225s; complete SHA
`18915745c52b96966abb38a9be9609cb196a1197eeac25b76a39357d36ad4a96`.
Wrong-Job, live-Main and pending-launch refusals passed.
Qualification3,927B SHA
`b290916d5908f9f7fac05f0df1c46f1133817d4681717ed4e0d7957aa4c63b0d`.

**N96/SDK197 same-Source8 marked comparison after restoration FAILED**:
baseline2115 groups completed, GPU2360.96ms/host2.3633272s;
chunk1 base0/count64 passed,host0.0933367s; **chunk2 base64/count64
reported fence SUCCESS,TOP1/BOTTOM0**. Failing duration unknown;
**no full S/g comparison**. Actual root1 Job2/0,wall15.5033703s,
finalized; after-free GPU current/reserved both0B. As in N92, fence SUCCESS
does not prove healthy GPU completion.

N96 wrapper's actual physical probe observed both AMD R9700 selectors,
vendor4098/device30033,api_version4211037,driver_version8389003,
matching the recorded N93 field values. These fields establish **neither
unchanged driver binaries nor healthy GPU operation**. Current and old
stdout were native/SHA checked; durable`N96-device-observation.json` SHA
`492e4313f343c82f0798dbf8c5ff62b33297b34d8162482c6b37437594fde2d5`.
No additional SDK calls; ledger197 unchanged.

The actual complete WER Application window contains14 events, **not14 new
incidents**; old reports were republished. Fresh141 ReportID
`33c045cd-1383-4e12-b1f3-937c305f7dca`,status4
`2026-10-15T18:46:37.3590726Z`,status2049
`2026-10-15T18:46:37.6541123Z`,dump`WATCHDOG-20261015-1446.dmp`.
Event receipt SHA
`2c7bf348bbf539beab441eb789afbfcc9a0bcc2cba221ac511dbed500889945d`.
Finalization1,472B SHA
`f235d6173a6da17f410fa7dc8cf8c128ac9423519e3dfe1ffc26e2b89f5872da`.
Durable`N96-result.json`14,055B SHA
`c9773b59b1bf34f5fcc70f67ccac268b1e90b797e2c4855e6d999d97c6d83589`.

Read-only shader review found **no concrete global-index/bounds/barrier/
CAS-termination bug**. The failed range moved from N92 base192 to N96
base64: **not fixed-range proof**. A potential next controlled diagnostic
is chunk-only execution from the fresh snapshot **without the preceding
long baseline**, to distinguish baseline carryover/delayed reset from chunk
execution. At N96 this was **not implemented or approved**, not a fix or full-map
authorization. There is no literal S/g output-slice oracle: observations
accumulate into shared outputs.

**Through N96:197/206,9 remaining,pending0. Further GPU launches paused after
the fresh watchdog; no blind reruns.** User-reported restoration did not
qualify the chunk path. Preserve every old failure, original, pin and
reservation. No comparison-runtime/numerical/root-cause qualification,
CPU route/full-map/parser/checkpoint/A0–A6 approval or model credit;
no code changes.

### N97–N103/SDK198–204: isolated chunk-only diagnostic completed; numerical validation still required

The user's **“Continue the investigation.”** approved continued bounded GPU
investigation, **not full mapping or a CPU route**. No operator GPU restoration
after N96 was independently observed; qualification below is by actual completed
runs, not an assumed restoration. All prior failures/caps/reservations remain.

Private Source9`dev9-abd936786b8a` added the reviewed-clean two-file
`--chunk-schur` diagnostic in`SchurCapture.h`+`sfm_map_test.cpp`: reuse the
chunk loop, upload the original39 buffers, skip preceding baseline and its
readback/restore/comparison, stricter early CLI. Source freeze SHA
`168e67973fde193f6abd286ff21942a9804661ed1088f00638869095d56b0701`.
**N97/198 shaders accepted** root0 Job356/0,626.5512476s,
queried Job peak6,699,106,304B,103 observed/253 unwitnessed exits.
Complete118,668B SHA
`16aa553c43434f8ff0bc575dd8970a9c24278a4cc08f5bda1e69cd63fbce4f03`.
**N98/199 application build REJECTED despite root0 Job2683/0**:
queried Job peak8,623,439,872B exceeded8GiB=8,589,934,592B by33,505,280B;
wall625.4610868s,1,891 observed/792 unwitnessed exits. Complete861,560B SHA
`73026c1d9e254f4a512d8cff06c9d597366fa63ea365aaec067319eb077504c8`;
refusal SHA
`87915f08661f949159dc7adad16d0e8d1dd45e236eed6ec51428552ed2ec3218`.
Closed/finalized **NEGATIVE**; entire Source9 failed build retained;
**no GPU use of its binary**. Root0/empty Job does not waive the cap.

An initial fresh-build quote was refused by the **local10GiB free floor,
not the shared retained-output cap**. Under existing user older-temp-cleanup
authorization,594 `.obj`/`.res`/static`.lib` compiler/link outputs were retired
**only from successful Source6/7/8 builds**: logical1,487,795,396B,
native allocation1,489,051,648B. All29,993 other existing files across owned
local/network roots were native/SHA checked/pinned before/after, including
Source9's failed tree, original inputs/snapshots/sources/binaries/SPIR-V/
embedded C++/probes/aliases/logs. Cleanup5,266B SHA
`0f25e2eeb2aed17e3ccae0711a358919e41aa41b6cf042067b712f015c5e957c`.
Those successful old trees now need intermediates rebuilt before relinking;
runtime proofs and failures remain. **Do not count retired files again.**
Fresh serial quote admitted local free13,372,428,288B; not future headroom.

Source10`dev10-0c851296ac8a` copied Source9 with **all C++/shaders unchanged**.
Only its private`build_develop.bat` changed job cap2→1 and application target
to`sfm_map_test` only; **no `spirula` built or claimed**. Source freeze SHA
`aa16fc6393ce9b5a136e662a61f121f9b69f69e758c47693a533d60b7f5a77eb`.
**N99/200 shaders accepted** root0 Job356/0,619.2085028s,
peak6,699,253,760B,93 observed/263 unwitnessed exits; complete114,553B SHA
`11d5dceb398469745e6399e4492b6bf3442d8aaf74f4cd83df77d4bf209ffd6f`.
All17 SPIR-V+embedded C++ byte-identical Source9/8/7.
**N100/201 serial diagnostic-target build accepted** root0 Job113/0,
58.2155524s,peak1,314,103,296B,28 observed/85 unwitnessed exits;
complete96,860B SHA
`100346c3b9d09a11aeaad187594d26f37fae74dbc249fbd2efa31341a65b6a56`.
Consumer3,527B SHA
`a206a0e00c855b18a8c293b4d8e298459f2d23234ae6dd800883f15be039ec35`;
`sfm_map_test`56,586,240B SHA
`d9bc9c75945a9bf7ab4c100b4156909760c099874804860a6333e56300be0227`.
All21,976 post-inputs unchanged. These build-only root/Job/resource results
are not complete individual-exit or GPU proofs.

**N101/202 strict `--chunk-schur` groups0 actual refusal**:
`groups-per-dispatch must be positive`,root1 Job2/0,wall6.1534741s;
no default selftest. **N102/203 markers-ON chunk-only SUCCESS**:
root0 Job2/0,wall13.3729282s,all34 compute TOP/BOTTOM1/1.
**N103/204 independent markers-OFF chunk-only SUCCESS**:
root0 Job2/0,wall11.1971517s. Both uploaded39 original buffers and covered
the full[0,2115) range,270,647 observations/73 padded,final chunkbase2112/
count3, **without a preceding baseline**.

| Actual isolated diagnostic | N102 markers ON | N103 markers OFF |
|---|---:|---:|
| GPU profile | 1288.77ms | 1267.49ms |
| Total chunk host time | 1.3030421s | 1.2829365s |
| Maximum chunk host time | 0.1051146s | 0.1012151s |
| Finite S/g elements | 170,820/584 | 170,820/584 |
| Nonfinite elements | 0 | 0 |
| S maximum | 28737584863077.438 | 28737584863121.438 |
| g maximum | 375528.3904278446 | 375528.39042945765 |
| GPU backing peak | 240,219,684B | 240,154,148B |
| After-free GPU current/reserved | 0/0B | 0/0B |
| Queried Job peak | 1,533,386,752B | 536,936,448B |

These are **aggregate-only diagnostics, not full-output numerical parity**.
N102 complete64,834B SHA
`a70246a4137d6c571c5f0e1c49ca510e07909f045dff43c539add0881d763e77`;
N103 complete64,975B SHA
`0c0cbd8a2a073efa76b39a3e71ca8ff7553a7613fb6a92401680b23473a0cdbe`.
Complete bounded Windows System Display/DxgKrnl and Application WER window
`2026-10-15T19:58:26Z`–`2026-10-15T20:01:06Z` returned0/0 events,
**not global absence proof**; receipt SHA
`d31a7519e7eeb4696469c63c1a2751fd6615232f0f560a1042fe974e96053631`.
Durable`N97-N103-isolated-chunk-result.json`19,745B SHA
`867d61a3d9a4ea21060ba0556ce5feccfec9a8b02a061b3b7ea6d1b6fc6a923d`.

The completed isolated runs **support a preceding-baseline/comparison-sequence
contribution**; delayed-reset causality is **not proved**. Full S/g comparison/
numerical tolerance remains untested; no production mapper integration,
throughput/quality/model/A0–A6 approval or qualification. Next meaningful
step is **full-output numerical validation before mapper integration**, not
another blind long baseline. No public production code changed.
**Through N103, before the extension below:204/206,2 remaining,pending0.**
At that point extending the diagnostic allowance required a user decision.
No full-map/CPU-route approval; preserve all failures and unchanged caps.

### Approved bounded GPU diagnosis extension: ceiling212 — 2026-10-15

The user selected **“Extend bounded GPU diagnosis”**, explicitly raising the
SDK ceiling **206→212 at204 charged:8 remaining,pending0**. This is a
prospective diagnostic allowance, not an accounting reset or retroactive
change to the historical206 counters. No new SDK calls have been made under
the extension at this receipt; full-output diagnostic design is commencing,
not an implemented or executed numerical validation.
Durable`user-approved-diagnostic-ceiling-212.json`433B in the owned Source10
root, SHA
`1849a366d7ea47da9f003c82b78e9fa4ebe2a90c9cf2a1a61b85d04f725bbbe5`.
All storage, memory, deadline, evidence and supported AMD-only constraints
remain unchanged, including26GiB combined retained output,10GiB floor per
volume and213,064B side-effect reservations. No full reconstruction, CPU
route, production integration or quality/model/A0–A6 authorization follows.
Preserve all historical failures and exact prior counters.

### N104–N111/SDK205–212: full-vector chunk comparisons completed; historical allowance exhausted

The approved eight-call numerical-diagnostic batch finished at
**212/212,0 remaining,pending0; all jobs closed; Main18096 then alive**.
At that checkpoint further SDK calls, full mapping and a CPU route were not
approved; the current goal supersedes that prospective execution stop only.
The pre-execution state in the approval receipt above is historical.

Source11`dev11-6e5180039fd1` changed only`SchurCapture.h` and
`sfm_map_test.cpp` versus Source10. Diagnostic CLI:
`--compare-chunks-schur SNAP --source-sha PRODUCER --groups-per-dispatch N --other-groups-per-dispatch M`.
It compares all171,404 logical S/g values from two bounded schedules,
restoring all39 buffers/47 aliases before each; **no long baseline**.
Static review found no blocking issues. Operational admission was restricted
to groups32/64/128; **the CLI itself does not watchdog-cap counts**.
Old execution modes remain. No shader/math/production code or tolerance changed.
Source freeze4,788B SHA
`f92544dbf1df1741849b7303e596f198bbeef7ffdb91c77dacef0050653e1ac3`.

Initial Source11 Publisher setup **failed before journal/source copy**:
a`Path` was passed as JSON parent. Empty failed namespace preserved;
receipt717B SHA
`974d0a683865c4264cb499cf636a4e12b2716751c5310d3473ac48a274a252c0`.
Fresh independent1,290-file copy SHA
`4af48b8295f36b9de70e40abbfec90fcbc9e9a56bffe096bb6bb3db55aa53479`.
No extra SDK charge for setup; failure retained, not silently accepted.

**N104/205 shaders accepted** root0 Job356/0,619.8520193s,
queried Job peak6,698,385,408B,100 observed/256 unwitnessed exits;
complete118,167B SHA
`ca1db75199b841766b4a312c0ae3683c4c5d8f4559b6d0a8fdddd17bc1b540c3`.
All17 SPIR-V+embedded C++ byte-identical Source10 through Source7.
**N105/206 serial `sfm_map_test`-only build accepted** root0 Job113/0,
58.7130387s,peak1,313,259,520B,32 observed/81 unwitnessed exits;
complete99,063B SHA
`af0011ce02bf9dab447f4a213dac3734fa9c73b419476b05e50ec17c176bb65e`.
Expected21,976 inputs unchanged; image56,591,360B SHA
`ab05b096c87516c688bac403c0f05ad6f05c299495efb6d9c15bd08e5ce1001a`;
consumer manifest2,521B SHA
`32457511c1af744e301bf465fbae01b13e1ff56af5baafdad61f54fc85b84da8`.
No production`spirula` built. Build-only root/Job/resource acceptance
does not establish every individual descendant exit or GPU correctness.

**N106/207 strict groups0 refusal** actually reported
`groups-per-dispatch must be positive`,root+child1 Job2/0,5.844146s;
complete57,909B SHA
`4c554b31469697a45281959e84401f34a06312ed249566d6a57a692e75627a4c`.
**Only this invalid CLI case was exercised in Source11**; no claim of
all-invalid-case coverage.

All five positive pairs below returned actual root/child0,Job2/0,
compared **all171,404 logical values**, restored39 buffers before each
schedule and covered all2115 groups contiguously exactly once per schedule,
270,647 observations/73 padding. All values finite; after-free GPU
current/reserved0 for every pair. Groups32/64/128 used67/34/17 chunks.
Across the pairs355 compute dispatches completed:220 marker-ON with
valid TOP/BOTTOM1/1,135 marker-OFF.

| Pair / slot | Markers | Outer wall(s) | First/second GPU(ms) | Maximum host interval(s) |
|---|---|---:|---:|---:|
| N107/208:64 vs64 | ON | 13.9893812 | 1284.03/1273.61 | 0.1042985 |
| N108/209:64 vs128 | ON | 11.8572532 | 1277.61/1250.01 | 0.1625142 |
| N109/210:64 vs32 | ON | 12.8977311 | 1263.40/1966.47 | 0.1085761 |
| N110/211:64 vs128 | OFF | 11.9608027 | 1256.41/1274.01 | 0.1762274 |
| N111/212:32 vs128 | OFF | 12.4629467 | 2004.37/1239.28 | 0.1702418 |

Second GPU times are **derived from rounded cumulative profile times**,
not independently precise timestamps. These timings are not benchmark
medians, throughput or speedup claims.

| Pair | S relative RMS | g relative RMS | S max absolute difference | g max absolute difference |
|---|---:|---:|---:|---:|
| N107 same64 schedule | 6.1788903060741915e-13 | 1.4805432814882645e-11 | 19.8125 | 7.6647847890853882e-6 |
| N108:64 vs128 | 4.0045378368437299e-13 | 8.9549359697547337e-12 | 13.25 | 3.8526486605405807e-6 |
| N109:64 vs32 | 6.5224783523304593e-13 | 4.6016822328868585e-12 | 17.75 | 1.7131824279204011e-6 |
| N110:64 vs128 | 4.6976761527820914e-13 | 1.0352118888174728e-11 | 15.5 | 4.5592896640300751e-6 |
| N111:32 vs128 | 1.2270232297176558e-12 | 9.5592748094102186e-12 | 47.3125 | 5.0584640121087432e-6 |

**Same64 schedule is not bitwise repeatable**: N107 S139,415/g580
differing values, S154,234/g1,161 differing bytes. Cross-schedule variation
is quantified, **not a tolerance verdict**. No threshold is introduced or
relaxed because relative RMS is small.

| Complete receipt | Bytes | SHA-256 |
|---|---:|---|
| N107/208 | 61,359 | `96ffba1e4325d809cbe00e6a13d2a81d64cc731317e7789755932491f6753775` |
| N108/209 | 61,349 | `04054badfb354b3b243db54d7c1446fc3b9b498f5cfe765ce36fe1036f363f26` |
| N109/210 | 61,347 | `f5d44375212824374120f821b46e5c0ffeeff00704406c6e8116a7dd0bf45d3a` |
| N110/211 | 61,343 | `9d8b2bc25cd57c9e59131a83037f25699ca29782296be362cb5c9569ac13b706` |
| N111/212 | 61,347 | `577ba41f4b3c466669058dc99b42cb8c855e001f3fd773ecd715fde6b7a3e0e2` |

GPU backing peaks240,219,684B ON/240,154,148B OFF.
Queried Job peaks N107–N111 respectively1,535,238,144/542,433,280/
541,396,992/539,910,144/540,213,248B.
Complete bounded Windows System Display/DxgKrnl+Application WER window
`2026-10-15T22:25:43Z`–`2026-10-15T22:30:17Z` returned0 each,
**not global absence proof**;726B receipt SHA
`4605a7f923de2159938187cfd499ca38afc16676564f44bd089e0c3d119c612b`.
Fresh final scope local/network free11,841,331,200/256,704,098,304B,
combined native allocation18,327,336,112B; unchanged26GiB cap/10GiB floors
and external213,064B reservation retained. These observations are not future
admission or true physical-peak claims.
All Source11 frozen inputs/image/snapshot were native/SHA checked afterward.
Aggregate`N104-N111-full-vector-comparison-result.json`150,467B SHA
`f5886621be3a62b03bb0456a0db76e717803296a6278de5e530c6a0a6181a920`
binds all receipts/metrics/events/resources.

**Disposition:** a bounded-chunking candidate is supported on **one snapshot**,
not independent correctness, original-baseline parity, numerical-tolerance,
driver-root-cause, production-mapper, model or A0–A6 qualification.
Existing CPU oracle`bacpu::Solver::assembleOnly` in`SolverCpu.h` regenerates
Jacobians/point preparation/private vectors; it cannot directly consume
these captured intermediates. The then-proposed snapshot→BAProblem/damping/
loss/layout reconstruction required separate verified work; the input-level
kernel-oracle boundary below supersedes treating that route as a prerequisite.
The CPU synthetic`1e-10` tolerance is **not a valid acceptance threshold here**.
Next meaningful gate remains **trusted-reference plus solver-effect validation
before integration**, not another blind baseline. The then-required new allowance
and no-full-map state are historical under the current goal; CPU production
fallback remains unapproved. Preserve prior failures and all scientific gates.

### Goal-directed continuation; fresh Main and N112–N116/SDK213–217 — 2026-10-15

**At N116:217 charged,pending0; all native jobs then closed.** Current goal
authority permits remaining prerequisite-gated execution without a fixed212 stop.
No GPU compute work or production GPU fix/correctness/model/CPU production
route/A0–A6 completion was qualified; N117–N119 continuation follows below.

Actual fresh **Main17808/create`134365826515304975`/parent18180** replaces
Main18096, whose`OpenProcess` returned87. **No handle continuity claimed**.
All10 existing bootstrap roles passed native/SHA checks; unchanged bootstrap
SHA`66fa28304abc4b90ccb37ea903f19bf0cc203f172adef9afb8b785fa280705ad`,
NETWORK_V3 and26GiB scope policy were reused without helper replacement.
New owned runtime label:`goal-runtime-d7d7b2c8546e.owned`.

**N112/213 positive CPU native family passed:** root0,Job3/0,10.3872504s;
complete SHA
`70f48a639601f18174cf6686d07256726b1975cb3f7aee4302e0f994708508fc`.
**N113/214 expected CPU deadline refusal passed:** root43,Job3/0,60.0637093s;
wrong-Job/live-Main/pending refusals passed; complete SHA
`9a2d4dde669a99d2c3ffcae938c6e0a2b0a8924cbf34a009fa78a99689eb400b`.
Fresh qualification3,804B SHA
`537a339b4d9ec8ddc751b23be17b4491590236a1358fca31d2c98ea113a9092f`.
This qualifies the fresh CPU boundary, not a GPU or scientific result.

Windows external-tool bytes changed; old binding refusals remain recorded,
not rewritten. Current`cmd`339,968B SHA
`15cd209c995b7d3009c91ca40892d5b8f1fb162f3e5f10716642ea278e8e0d7b`
replaces expected344,064B SHA
`97ac98b1a92c286054cce55239cfccdfc23a5517bd07fe693072c9ca96c7dabb`.
Current PowerShell495,616B SHA
`60c9b29843624dd8af6b6a7b26147753be6aeef65098b9a503cc50850afe410f`
replaces expected same-size SHA
`8bb6fa8c283b4d92120b1ef249a9b311b0f804d4cabbe9981159976c8be76a5e`.
Other20,666 inputs unchanged. **N114/215 current`cmd` functional smoke passed:**
root0,Job1/0,actual`SS_CMD_TOOL_OK` and Windows10.0.26100.9711;
admission2,437B SHA
`5c4c6c276143522046dd540da475fa737d4ea9e05b2a2ad956b8a0d12a5f628e`.
**N115/216 PowerShell root0,Job1/0 but stdout empty; N116/217 root0,Job1/0
but precreated owned output file stayed empty. Neither is functional admission.**
`PowerShell-functional-admission-refused.json`4,076B SHA
`4ca77867ac0f74dd5dc6e07becd567016d8b9307cec0ae2e2b6be93831c5e661`
binds both negative functional probes; all jobs closed at217 charged.
No cause inferred; preserve both negatives. Source12's private build is already
serial1; removing its redundant PowerShell RAM probe is in flight instead of
expanding into Windows diagnosis, not a claim that PowerShell works.

**At the implementation checkpoint, Source12`dev12-0d20788d9bb4.owned` was
finished, unbuilt and under static review.** Independent copy:1,290 source files+18 qualified Source11 shader
artifacts; copy receipt8,916B SHA
`ce5cf511a6aa6d2da5a30eedfe5e5ffffae33e32b61ef40ea3d83b70362e5e3a`.
Corrected reference boundary: **captured`Push.f0` is actual damping**;
`Solver.h:888–901` assigns`p.f0=damping` and forwards it to capture. No missing
lambda or recapture prerequisite. Immutable captured W/Bp are legitimate inputs
to the changed Schur kernel; an independent point-streamed host reference need
not rebuild BAProblem or upstream Jacobians. The existing`assembleOnly` API
limitation remains true but does not block this input-level kernel oracle.
No tolerance may be fitted to observed RMS; actual GPU Cholesky-step validation
is planned, not executed. Trusted-reference and solver-effect gates still precede
production integration; resource/scientific limits, consumed long deadlines,
213,064B side-effect reservations and every historical failure remain unchanged.

Before any Source12 build/reference/step results, parent froze
`reference-solver-acceptance-before-results.json`2,586B SHA
`ad628565984b136c896a18e567e995e55d283e6d0b240c3fab6634c7d68891e9`.
Actual GPU step versus the stored independent CPU binary64 reference SPD system
must have **relative S-energy error upper bound≤0.001**, implying optimal damped
linear-model reduction loss≤1e-6. This explicit application-level one-part-per-million
budget is **not derived from Source11 RMS**, bitwise/universal-DF acceptance or
exact-real assembly proof. An outward-rounded factor/residual/conditioning
certificate is required; unavailable means **INCONCLUSIVE**. CPU assembly
roundoff outside that certificate must be explicitly reported. Full logical
S/g audit, basis/finite/SPD checks and later full-map/A-gates remain required;
existing quality criteria are not relaxed. No new execution calls or result
qualification follows from freezing this acceptance contract.

### N117–N119/SDK218–220: Source12 rejected; fresh Source13 shader-build launch

**At launch:220 charged; N119/220 shader phase was in flight.** Completion follows
below. No Source12 GPU run or new reference/step/correctness qualification occurred.
Source12 static review corrected coupled expected S/scale to6.86/8.14,
used readback floor guard`nextafter(2*sqrt(double(float(1e-30))),+inf)` and scoped
MSVC`fenv_access(on)` for the certificate. Freeze20,473B SHA
`139009431a4e1374dac368c265f5ef01cafda73326f432689655ff1594a104df`.

**N117/218 read-only physical-device query passed:** root0,Job1/0; actual both
AMD UUIDs,API4211037/driver8389003,available33,250,242,560B each.
Receipt6,002B SHA
`4bcd44a5e122052ea8d8d267a88b343ad691198936604d3bfcf6e62cce079c2d`.
This is physical-query evidence, **not compute-health proof**.

**N118/219 normal application build REJECTED:** root1,Job323/0,936.4070143s,
peak6,703,308,800B,76 observed/247 unwitnessed exits. Compiler reported it could
not write`ba_df_cauchy.raw.spv`. Parent mistakenly pinned copied qualified shader
**outputs** against writing before fresh Ninja regeneration; legitimate writes
were denied. This is **not an observed Slang compiler/math regression**.
All21,960 source/tool inputs+18 initial shader artifacts unchanged.
Complete113,523B SHA
`93584f2685b341d48e4e105d7cc76a3f072b2ba30a00a4301b5bbb64efd8f330`;
finalized negative closure/rejection12,674B SHA
`6ab6923510b3cc472a667a2f78cfb9cf533f7345e734ff22c2fd41dbe542da09`.
**Entire failed Source12 tree preserved; no outputs reused.**

Fresh **Source13`dev13-01c683884446`** contains1,292 source files byte-identical
to Source12, no source change; fresh build tree, **no copied shader outputs**.
Shader producer writes unpinned outputs first; pin only after success before
application stage. Freeze3,236B SHA
`20f51f1f579c3d015d10aee4e705d41de46cac2f14d57e68410ea5d745ece545`.
N119/220 shader phase was launched under1200s finite deadlines for both phases
and unchanged caps;1.5GiB projected growth was resource-admitted. Results follow.

**Original lineage recovery, not old-manifest restoration:** exact read of the
archived original-selection manifest path returned not-found. Keep that artifact
historically failed; do not restore or claim its old identity/custody.
History JSONL record`184526b3`(`2026-10-08T01:38:56.415Z`) contains both complete
selected raw-member/native fields and the user's full-resolution INSV choice.
Parent sealed its historical semantic context in a **new owned receipt**, not
old manifest bytes. Fresh pins and double whole-file SHA reads covered both
actual raw files,19,777,805,933B; native ID/birth/mtime/links/attributes/size and
SHA matched archived members **and actual N29 inputs**. Admission2,055B SHA
`7d9af30279a30938b4364dc432c117010de0a1b46965f552f971487d3776d495`.
N29 originals/images→N39 features→N42 matches joins native identities+SHA across
seven bound receipt-metadata files. New
`selected-original-image-feature-match-lineage.json`16,100B SHA
`da2c03143974de7f865cde5ae30f727604f1c5364913672d84ece64b9cc32ab9`.
No chronology-based rejection or corpus regeneration is needed from this chain;
**image/feature/match payloads still need fresh native/SHA checks and pins before
actual mapping**. Semantic recovery is not restored historical manifest custody;
private paths remain only in owned receipts. All scientific/resource gates remain.

### N119–N123/SDK220–224: builds and CPU checks passed; captured-input preflight refused

**224 charged,pending0; all native jobs closed; no GPU compute in this
continuation.** Source13 did not initialize a GPU context in N123.
**N119/220 shaders accepted:** root0,Job354/0,620.8265746s,
peak6,699,663,360B,97 observed/257 unwitnessed exits; complete128,102B SHA
`c37cac03dfa69f32dea6fd72d25033ba558bcb2334c7d38ac2b65f4414c4ece4`.
All17 SPIR-V+embedded C++ byte-equal Source11 through Source7: the output-pinning
fix worked. Shader admission10,832B SHA
`891ac5de5495202da355dd8723e4e2f2f38666458c4e84878594823845518ba0`.
**N120/221 application accepted:** root0,Job117/0,60.0606259s,
peak1,312,849,920B,29 observed/88 unwitnessed exits; complete109,562B SHA
`4ebb025efd0bd75f758378bf5c52437436f0f0f05d3dfccefb93dd4a7dee6b19`.
All21,960 inputs+18 shader artifacts unchanged; compiled-consumer3,621B SHA
`9a4b7abefe2acfc1eb162c264efcb3e643bdee6a6571db6ee40c8170db01196f`.
Only`sfm_map_test`+`sfm_ba_cpu_test` built, **no production`spirula`**.
Images respectively56,627,712B SHA
`71be002b793277d45c8732d8e5155258ca73912c0eeb9c6c638726a95cc086d1`
and351,232B SHA
`9a3b29f2c7f2132c689f33daf9f6de6e1b6d44f3aa165c600997c5d4912fcf64`.

**N121/222 reference Schur self-check:**10 PASS,root0,Job1/0,9.2381633s;
actual stdout680B SHA
`061f16517d00d784eb6a3861fbbd720b0cadcaa7fc1d04492484cf2b11da0ebe`.
**N122/223 CPU BA suite:** actual **212 behavioral checks+final PASS**,
root0,Job1/0,12.7990569s; stdout14,901B SHA
`8f85751be5e4ccc846fd35c8b3dcc6976a58bdb80481d8a7b8c947d5c9d8d42e`.
The212 count is semantic`err ... (tol...) PASS` lines;216 preceding stdout
lines included other status, **not216 checks**. CPU passes do not prove GPU Jacobians.
Existing CPU`testZeroRotation` explicitly skips the first three rotation columns;
N122 therefore **does not establish zero-rotation camera-Jacobian coverage**.

**N123/224`--reference-chunk-schur` original slot2 STRICT REFUSAL:**
root+child1,Job2/0,7.2345154s,347B log,`nonfinite DF input component`,
**before GPU context initialization/dispatch**. Actual captured lambda
0.0011111111380159855/bits`3a91a2b4`. Read-only forensic parse verified unchanged
snapshot SHA: Jc8,660,704 scalars include20,454 nonfinite DF pairs, both high/low
`0xffc00000`, covering3,409 observations/3,409 points. Jp1,623,882,
res541,294,W426,411,Bp142,137 all finite; no empty per-point ranges.
Every bad Jc observation belongs to image/frame43, exactly local positions0–5
(three rotation columns×two rows). Pose:
`[0,0,0,-0.01857263539461107,-0.005664830655386095,-0.05950276362282669]`.
First bad scalar32/observation1/point0,range`[0,5]`.
Input audit9,447B SHA
`2043734809adea61043daf6ba5b0ca1c8e4f5bf37b1a28ee2424b1fbc1f58444`;
pattern receipt5,622B SHA
`d4e88e6922946ee93d9a80c2147c2176d51a7eafde1c7a8f46bb213f255f3131`.
Earlier finite S/g reflected shader finite guards skipping bad contributions;
**not producer correctness or evidence these NaNs caused the watchdog**.
Preserve bad inputs/old outputs; do not filter NaNs or replace with CPU Jacobians.
Zero-rotation DF camera-Jacobian producer is the current blocker under read-only
trace/regression-seam investigation, **root-cause hypothesis only** until code
and bounded fresh GPU regression establish it. Correct producer, then regenerate
the **entire pre-Schur state from unchanged raw snapshot inputs** before the
trusted-reference/solver gate; no arbitrary tolerance relaxation.
Aggregate`N112-N123-continuation-result.json`12,963B SHA
`dae2e0f84fb8251b94b8eaa60501e35a3124fd0f4eeead8480eb85e660adfe1c`
binds all continuation proof/negatives; goal authority continues without fixed stop.

**Production candidate, read-only planning only:** unused Schur`Push.u1` as global
observation offset; existing`vkCmdDispatch`≤64 groups with fenced prefix, every
chunk and fresh continuation; supports global group counts>65,535 without
`VkContext`dispatch-base/AMD-marker API. **Not implemented or accepted**; requires
new SPIR-V qualification and migration of all command-buffer-owner callers.
Public default scientific settings, historical failures and all gates unchanged.

### N123 diagnosis boundary; Source14 harness-only preparation — 224 charged

Read-only trace: shared`camera.slang`'s`angleAxisRotate` differentiates
`length3(axis)=sqrt(dot(axis,axis))`;`df_sqrt` returns zero at zero, while
`df_sqrt_bwd` then divides by zero. The normalization epsilon protects primal
division, **not the norm derivative**. This strongly matches the observed
zero-rotation Jc pattern, **not yet fresh GPU before/after or watchdog-causality
proof**. CPU`CpuCamera::angleAxisJacobian` has a related zero-norm singularity;
its existing zero-rotation test skips rotation columns, limiting N122 coverage.

New independent Source14`dev14-89d1d3e6fd2b`1,292-source copy SHA
`c25cdbe63faca84563405ef0ebf36f816d37846b662a74a93def2853bfeb13e0`.
**At this checkpoint harness-only implementation was in flight; no CPU/GPU math fix or execution.**
Strict GPU-Jacobian mode will use the **exact same consumer** against old
qualified and later fixed`ba_df_trivial`SPIR-V, forbid CPU fallback, and exercise
the raw one-observation actual image43 fixture plus zero/nonzero/rig controls.
The CPU zero-rotation test will no longer skip rotation columns.
Whole original snapshot inputs remain unchanged; no finite-filter repair.
Ledger224,pending0; all native jobs closed; all prior acceptance/resource gates
and failures retained. No GPU-Jacobian root cause or watchdog causality qualified.

### Fresh Main15416 recovery; N124–N125/SDK225–226 CPU qualification

Latest **“Continue”** encountered actual missing`H`/`charged` globals in fresh
**Main15416/create`134365895945817240`/parent9540**, not an inference from a host
notice. Previous jobs were all closed at224. All10 existing bootstrap roles
passed native/SHA checks; unchanged helpers were re-entered for the new identity.
Old Main17808 observation retained; **no fabricated handle continuity**.
New owned runtime:`goal-runtime-3182f27c4f3c`.
**N124/225 CPU positive passed:** root0,Job3/0,8.7049894s; complete SHA
`3a09e6156f59a5a451d3137ae339ebef3a3561755a30f437f84310faacf08fdf`.
**N125/226 expected CPU deadline refusal passed:** root43,Job3/0,60.0815373s;
wrong-Job/pending refusals passed; complete SHA
`81a4a753460bb1e67916e88a83568ccebf65c720f014ffa6b42998431dc71a8e`.
New qualification4,332B SHA
`9d3a1494dd8076f6cd48fbd1ff013566cf19b3265635246b108827db104494b9`.
**At N125:226 charged,pending0; all native jobs closed. Source14 harness was
finished/unbuilt, under static review; no math fix or new GPU compute then.**
All20,668 toolchain bindings revalidated against the new Windows bindings.

Read-only metadata from the native-pinned original snapshot, **not a fresh
whole-payload rehash**, records94 images/two groups: group0 free10/full12,
model8,intrinsic offset0/column564,71 images; group1 offset12/column574,23 images.
Both fixed principal points are1920.0/1920.0. Metadata1,327B SHA
`f7fcb7d7427add66a287284a185cd8eba2259c16aced2fff6b64b5d0c38f66bb`.
N42 source camera IDs1/2 are3840-square/scale2.4/principal1920; IDs3/4 are
2880-square/scale1.8/principal1440. Source13 captured free prefix10 leaves the
last two principal parameters fixed. Coarse/fine principal-policy/source
provenance was then under tracing; deterministic loss derivation follows below,
**not an observed captured Jacobian push**. Schur damping0.0011111111380159855
remains separate. No GPU/root-cause/quality gate is qualified by metadata.

### N126–N129/SDK227–230: unchanged-harness old-module red proof; Source15 unbuilt

**230 charged,pending0; all native jobs closed; Main15416 qualified.**
**N126/227 Source14 shaders accepted:** root0,Job354/0,618.5758706s,
peak6,699,487,232B,95 witnessed/259 unwitnessed exits; complete119,213B SHA
`df33a37a9f596e58aeb56456132a62e865194babed18fe0c0eeabf6ba7cf8e42`.
All17 SPIR-V+embedded C++ byte-equal Source13 through Source7.
**N127/228 application accepted:** root0,Job117/0,58.2665614s,
peak1,313,935,360B,29 witnessed/88 unwitnessed; complete101,224B SHA
`6b3679dd0305aebf286f2f01871bf0c8a6e0a5750e28ae8977a1fcb09b9d494e`.
No production`spirula` built.

**N128/229 Source14 same harness+OLD Source13 DF trivial module, AMD R9700
UUID1:** root+child1,Job2/0,26.1532331s; **all58 cases executed,46 FAIL/12 PASS**.
Captured zero-rotation Jc26/32 finite,first6 NaN; Jp6/6,residual2/2 finite.
Analytic pinhole zero Jc14/20 finite,first6 NaN; tiny1e-16 rotation wrong but
finite. Both rig rotations nonzero1e-4 PASS,normalized6.110067204e-8.
All58 TOP/BOTTOM1/1; after-free GPU current/reserved0,backing peak134,286,988B.
**Additional finite failure remains unresolved:**`captured_axis2_+1e-04`,
normalized1.2229534659656418e-6 exceeds frozen1e-6; Jp row0/column16 GPU
11.004374984136234 versus finite difference11.004401899846206.
**Do not widen tolerance or hide this failure behind the zero-rotation fix.**
Log219,844B SHA
`8a11a06f77598ce49f5a55b2c305afbb3fe614274670e97098d69546b3f81c42`;
red proof27,528B SHA
`d75fddbe5c590714fa7bf6d0531c3a378fce04b3cbc65089d29d07dd6e7a35d5`.
**N129/230 CPU zero self-check:** root1,Job1/0,**5 FAIL**: OpenCV,Snavely,
ThinPrism,Equirectangular each6 rotation NaNs; rig frame/member12 rotation NaNs.
Finite differences/Jp/residual finite. These expected-red scientific failures
are preserved, **not waived qualification or successful fixes**.

Complete bounded event window`2026-10-16T02:32:32Z`–`02:42:33Z` returned
0 System Display/DxgKrnl and0 Application WER, **not global absence proof**;
740B SHA
`07a810ac2f9d46f74170153061d4260aa0d27a8b2ee3566ff70dd81d8600562e`.
Loss provenance resolved by deterministic lineage/source derivation:
N82 default Huber2.0×N42 scale2.4 for both94-frame groups yields float
4.800000190734863/bits`4099999a`. This is **not observed Jacobian push data**
and **not Schur lambda**`3a91a2b4`. Receipt11,755B SHA
`fcbda9514b2eb728afc3e219d0cbaacf886b7e0ca5aaf842ffc2bd77ed2bf1f5`.

**At this checkpoint Source15 was UNBUILT/UNQUALIFIED; harness frozen unchanged.**
Only`camera.slang`/`CpuCamera.h` math changes: analytic zero branch **before sqrt**,
positive-angle epsilon-free axis/theta normalization and stable half-angle
Rodrigues`oneMinusCt=2*sin(theta/2)^2` from one sin/cos pair. Review caught that
naive`1-cos(theta)` would amplify tiny-rotation DF cosine bias; corrected before
build. No fixed-module before/after acceptance, watchdog causality, model or
A0–A6 qualification follows. Current goal continues prerequisite-gated execution;
all original inputs, failures, scientific tolerances and resource limits remain.

### N130–N134/SDK231–235: zero-rotation repair observed; overall GPU qualification FAIL

**235 charged,pending0; all native jobs closed; Main15416 qualified.**
Source15 freeze5,022B SHA
`6d81da61d71362b1d73406651e5eda1df141f23f9b6bb2e1d2b6d7eaf446d376`
binds exactly two math-file changes versus Source14. All1,292 source+20,668 tool
bindings (21,960 native/SHA checks) unchanged after both build phases.
**N130/231 shaders accepted:** root0,Job354/0,619.5178597s,
peak5,928,058,880B,97 witnessed/257 unwitnessed; complete118,880B SHA
`d4dab6c23f143e52f3c218f19fddddc4dba084c160cd2eeed7866ac4e9a2fd3d`.
Expected12 BA files changed (nine final/three DF raw),other five modules unchanged;
embedded C++ changed. Outputs unpinned during production, then bound/pinned.
**N131/232 application accepted:** root0,Job117/0,60.6079154s,
peak1,574,162,432B,32 witnessed/85 unwitnessed; complete101,007B SHA
`3ba68f9e1e681a93a64f540701e4a73e0b5ef2b2a2a1e7b4c58f6682bcacadc0`.
No production`spirula` built. Consumer manifest3,050B SHA
`98f093a433d9a1d662c0d893db23c5f66351b42cadbe030edee6e7d5e49654b9`.

**N132/233 SAME FROZEN Source14 GPU harness+external Source15
`ba_df_trivial.spv`,actual AMD DF:** root+child1,Job2/0,28.5815537s.
All58 cases' Jc/Jp/residual now **finite;57 PASS/1 FAIL**,frozen1e-6 unchanged.
Zero/tiny/rig cases now pass; analytic pinhole exact-zero Jacobian matches all
six values. **Original zero-rotation NaN issue observed resolved in these
controls, not overall GPU math acceptance.**
Remaining pre-existing`captured_axis2_+1e-04`Jp row0/column16:
GPU11.004374114283678 versus FD11.004401899846206,
normalized1.2624765007512878e-6,**FAIL**. No threshold waiver;
**aggregate GPU qualification FAIL**. No finite full-snapshot regeneration,
Schur retry or mapping followed.58 compute TOP/BOTTOM1/1,after-free0,
backing peak134,286,988B.
Log172,739B SHA
`431f341ced8f03ce2f2e35c518ee5253576ce6c0417a66dd9c7007ffaf105648`;
complete83,728B SHA
`b6996034a6df06fcd1f45d69b67bcf99149087d7e240bc2a0cdcb09a076f0b77`;
parsed proof27,823B SHA
`2d7a863fca846e7e9a6399aa34b86b56c596a6704868f38196c5c79a0099f7b2`.

**N133/234 Source15 CPU focused zero:** root0,Job1/0,8.3797393s,
all5 checks PASS/all rotation values finite; complete45,354B SHA
`3a4da37c5f54e20a8cec5517e884072a1e0ce1af1899ccea45b089160381b3f8`.
**N134/235 CPU full BA:** root0,Job1/0,11.5834834s,actual217`err/tol`checks
PASS including10 reference/certificate checks; complete45,313B SHA
`64a1ef0c878c7c41a802deb22bd7bc6a7868d63f9ee4f388ed5b03f69c4de3a9`.
Complete bounded`2026-10-16T03:11:01Z`–`03:14:49Z`event window returned0
System Display/DxgKrnl+Application WER, **not global absence proof**;
1,602B SHA
`0b1607b6dd3c0835126a3eb192fd474feb9981213df1e35f32370ef8ee83d11a`.
At235 the finite GPU/FD discrepancy remained under expert read-only investigation;
the Source16 optimization-only comparison below supersedes that blocker status,
not historical failures or watchdog attribution. All unchanged tolerances,
quality/resource and trusted-reference/solver/full-map/A-gates remain.

### N135–N136/SDK236–237: optimization-only DF comparison passes focused Jacobian gate

**237 charged,pending0; all native jobs closed; Main15416 qualified.**
Source16 changes **one CMake file only: DF compile O2→O0**. Mathematical sources,
Source14 frozen harness and no-contraction contract unchanged. Freeze2,249B SHA
`659e53325b3fa56a99110009c2136c37b87de5002bc2ee4e56601d53760b458c`.
**N135/236 shader-only build accepted:** root0,Job354/0,620.0856741s,
peak750,460,928B,96 witnessed/258 unwitnessed; complete117,662B SHA
`583d56a14937cba172833e7b65e371301d9a2e6a1f52fc5c9bca15eb66dbfe54`.
All21,960 inputs unchanged afterward. Only six DF files (three final/three raw)
changed;11 other SPIR-V modules unchanged. Admission11,439B SHA
`fa4554ccf85b72832c1ab63bfd74301e838168054a5f5ffd50a762faf103a7ed`.
No application build.

**N136/237 SAME frozen Source14 consumer+Source16 O0 DF trivial module,actual
AMD DF:** root+child0,Job2/0,29.9148053s,Job peak504,184,832B.
Module1,384,832B versus Source15 O2 module19,484,304B.
**All58 cases PASS/all Jc/Jp/residual finite**,unchanged1e-6 threshold;
maximum normalized error1.6606132896298566e-8. All58 TOP/BOTTOM1/1,
after-free GPU current/reserved0. Last finite discrepancy corrected by changing
**only optimization level**. GPU-only identities independently improve:
pure-z`Jp_z−Jt_z` O2`[4.81114966532914e-7,1.30345540583221e-7]`→O0`[0,0]`;
zero-pose`Jt·pc` O2`[0.00019668656420535058,0.0000654760105138763]`→
O0`[1.2210676914037322e-11,1.4219736499399005e-12]`.
This establishes **optimization-dependent precision failure**, not localization
to an exact compiler pass/AD/driver stage, watchdog cause or production performance.
Log172,361B SHA
`0cba08f7bda5d00140cc21873572dc512577c83e36de28f6159be2c4846832ce`;
complete86,991B SHA
`ff8599a786689e7736232ab4d485c8e90278836f215a4a5fc1b5a0c485035594`;
parsed proof30,257B SHA
`ca27df91acbc5c12806c71c0f8c6df02e21faab80c44021df16d529e679b3dde`.
**Focused Jacobian prerequisite PASSED**, together with Source15 CPU5+217;
full-state/Schur/solver/production/model qualification remains pending.

Preserve first Source16 pre-native copy refusal: parent journal reached1MiB cap
at949 copies,next file0B untouched; entire partial tree retained. Fresh copy used
256-file journal cohorts, **no guard relaxation or SDK charge**. Refusal SHA
`e6ced80348a0d8d42256626861574ac43f48b76d2dcef45b7e98c216f8c918b0`.
At237 Source17 independent1,292-file copy was admitted; implementation was in
flight across three existing files+shared`ProducerDispatch.h`, strict full-state
Jacobian+point-preparation capture v1 (39 buffers/47 aliases), before GPU execution.
No Source17 math/shader/CMake changes; require17 SPIR-V+embedded C++ byte-equal
Source16,exact loss`4099999a`/lambda`3a91a2b4`/new-producer-SHA lineage.
Price1GiB+213,064B: observed Source15 full686,964,648B+Source16 shader/copy
139,519,488B+source31,834,462B fit below1GiB; all caps/floors unchanged.
At237 the prerequisite chain was full finite producer→independent Schur/solver
certificate→production bounded dispatcher→original reconstruction/A0–A6.
N140–N141 below clear only the first two gates; all old evidence/failures retained.

### N137–N141/SDK238–242: finite full state and scoped solver gate passed; integration active

**242 charged,pending0; all native jobs closed; Main15416 qualified;21/26 tasks
done, production integration ACTIVE. No public code cutover/reconstruction/A0–A6.**
Source17 freeze5,201B SHA
`390afe60c8141cc1d2652ca91cd30412084e75b2cf960ba6db86b603876b5121`.
Four files use shared`ProducerDispatch`in namespace`spirula`(seven external+
one internal calls),streaming finite audits/full-v1 save and strict producer CLI;
review qualification fix applied before build, no remaining review blocker.
**N137/238 shaders accepted:** root0,Job354/0,621.3915205s,
peak749,981,696B,97 witnessed/257 unwitnessed; all21,961 inputs unchanged;
all17 SPIR-V+embedded C++ byte-equal Source16. Complete118,058B SHA
`67465c795e48a29e9aa12d3535e19113e919eb438b1fee34f3297ce82c66f708`;
shader admission11,264B SHA
`9988f9b4610d250235df479f29442745e4eb8b750c758b79646a6add6e4f563c`.
**N138/239 app accepted:** root0,Job117/0,55.7959503s,peak446,595,072B,
26 witnessed/91 unwitnessed; complete97,904B SHA
`21bbe64cd1a4aaa401da99850012be56c001457f66c1b0ddaff92bcb1892f5db`.
Consumer manifest3,029B SHA
`ec5407c334242e8ff74c805c3984f1c52624fab21ebbdf0959d5afd758165ccb`;
`sfm_map_test`14,284,288B SHA
`0f1be864590c937b740600c8d9763d19bef5fcd1e73d65630095e9377ef72802`.
No production`spirula` built.

**N139/240 authentic NEGATIVE:** parent supplied`SOURCE_SHA`instead of
required`SOURCE_SHA256`; configuration refused **before GPU initialization**,
four slots0,root+child1,Job2/0. Long label also produced261-character stdout
scratch/MAX_PATH failure. Original16,538 stdout+0 stderr bytes/native objects
retained; exact bytes published in a short owned namespace, real bindings passed
through the existing frozen SHA-checking retry seam. Unchanged`resume_pending`
reobserved original family,published complete NEGATIVE,then actually finalized.
No patched helper,invented output/witness or recovery charge; whole failure kept.
Complete70,041B SHA
`8dfc8fa4dec8385a4291efc3a2cfb0f573c2f021d5d82f08c0e35da5f24853f5`;
byte recovery1,996B SHA
`be121bcd1f6568f8369239c725625abb56ecec5891ca1cd155492e7618100762`.
Use short future labels.

**N140/241 new owned slots/correct`SOURCE_SHA256`,same Source17 image:**
root+child0,Job2/0,14.768335s,peak631,795,712B; actual AMD DF embedded
`ba_df_huber`,restored39 buffers/47 aliases. Jacobian270,647 observations/
2,115 groups128+point-prep47,379 points/186 groups256; exact loss`4099999a`/
lambda`3a91a2b4`; **no Schur/mapper/CPU fallback**.
Six complete DF audits Jc8,660,704/Jp1,623,882/residual541,294/App426,411/
Bp142,137/W426,411 **all finite,total11,820,839**.2/2 compute markers valid;
GPU peak240,219,684B,after-free0. New full snapshot105,937,273B SHA
`eba4a2db3b4549ad3ef67639fcb378e65f8214eb5f19cd523a6c3f51cfa66eb9`.
New Source17 producer header; v1 header byte-equal except65B producer field;
all30 nonproducer buffers byte-identical,only seven output IDs11/14/15/16/19/22/23
changed,S/g remain zero,old input unchanged. Sidecar14,717B SHA
`91f603c969b0fcbd21f83256ab84ad5c9ccdcf37377cb6ca23772cfff3c42f7e`;
complete76,133B SHA
`865dd06bf0880808ab5dcd494f0a91780c93863e2d7b7e58ecd8407fa8d975ef`.

**N141/242 independent reference+actual GPU solver:** root+child0,Job2/0,
21.166317s,peak662,614,016B. All170,820 S+584 g+584 actual GPU-step values
independently stream-parsed,sequential/complete/finite.34 contiguous Schur chunks
(33×64+last3),no long baseline;109 compute markers validated (34 Schur+75
actual Cholesky/triangular). Schur1,359.55ms/all profile1,362.22ms,max host
chunk0.1080394s,**not performance qualification**. S/g relative RMS respectively
5.2925147161009344e-12/5.7454839230719633e-12: **raw vectors have no general
tolerance verdict**. CPU reference S∞5.7417174765348703e13,diagnostic condition∞
5.8083507809617065e11,min unfloored pivot1,784.3667934206533; actual GPU
min factor diagonal42.241765983618507>floor band2.0000000031710774e-15.
Step relative L2 difference8.5511572999093185e-12 is diagnostic only.
Preregistered outward certificate **CERTIFIED_SCOPED_LINEAR_MODEL_BUDGET**:
stored independent CPU binary64 S/g target,relative S-energy upper
0.00059348521643083076<0.001;optimal predicted-reduction loss upper
3.522247021219499e-7<1e-6;whitened matrix perturbation upper0.081257814657393193<1.
**Scoped trusted-reference/actual-solver gate PASSED**,not exact-real assembly,
universal DF bounds,upstream whole-BA correctness,watchdog cause or model proof.
One ancillary cancellation field at index579 was marker-interleaved; all critical
index/reference/GPU/error fields and vectors independently parsed complete;
raw50,567,981B log retained unchanged,SHA
`c571cdc34a3b599bca1c1a161e8d1ba9fbe21461e2236247f553cc1b4f40e92f`.
Complete77,247B SHA
`72566408d5a3360484bd9392bff61b12f49138aa7cb1904386e1fe276c93c3c7`;
gate12,864B SHA
`7bce9991ed59f054fe76a7311ef9924a4ce2e4a01ed124ca6956ca114e5c8da3`.
Bounded N140–N141 Windows query0 complete System Display/DxgKrnl+Application
WER,not global absence/cause proof;2,247B SHA
`d69c99be3adc8b16b3c1306fe668beb70996b5821369d341dc4c30bff7c9bfeb`.
Continuation28,123B SHA
`b2e1ddb942fe1b2f1a4cccbff8e05e44dc72e4985e831352b31b00b28adae25c`.
At242 read-only production-delta/native full-build scouts supported selected64-group
`u1`observation-offset dispatch,fresh fences,clear command buffer before consume,
all owner-callers migrated+new SPIR-V tail/reference qualification before mapping.
Production implementation had not started then; current Source18 work follows.
Reconstruction/model/A0–A6 remain prerequisite-gated under current goal.

### N142/SDK243: original/payload readmission and mapped-cwd preflight; Source18 in flight

**243 charged,pending0; all native jobs closed; Main15416 qualified;21/26 tasks
done, production integration ACTIVE. Source18 UNBUILT/UNQUALIFIED.**
In-process CPU-only readmission (no child/no charge) covered the two selected
full-resolution INSV originals,19,777,805,933B,with **two whole-content passes
while fresh native read-deny-write/delete pins were held**. Native/SHA identities
match the selected historical pair; **not a same-pinned-handle hash or old-handle
continuity claim**. Missing historical selection JSON remains FAILED/unrestored.
New receipt6,426B SHA
`294824388536bdc210b13d3215e81f1e14321ca5c7c64344014e8ae63c7272be`.
Fresh payload scan24.1855956s admitted4,416 original-derived JPEGs+4,416 feature
files+matches,**8,833 native/SHA-exact bindings**;53 manifest chunks (23 image/
30 feature) bound/pinned,complete source/track/candidate join4,416 exact.
Receipt34,253B SHA
`8c884eb833718e999c81dafa0bb8c73ee2e88ce2e5eaf16e479a2cc2f4acc3b4`.
All current read pins held in Main15416. **Full-map resources/argv not admitted**;
no LRV substitution,regeneration or corpus replacement.

**N142/243 read-only native`cmd`mapped-cwd preflight passed:** root0,Job1/0,
9.0283885s; child changed cwd to the correct owned mapped alias and printed
Windows10.0.26100.9711. Mapped/network base and owned-root aliases matched native
identity before/after; **no drive mapping/config change or build**.
Complete SHA
`6a7565866327e87e329ba117199032451c272796efeb208950a238c430bc5fc7`;
final SHA
`b5ba052b42cf8ed05bb6ae54e60ffd87de9512cec13b097e2d02c2d2b05401b8`.

Source18 independent mutable1,293-file copy exactly matched Source17 **before
edits**,no build outputs reused; copy SHA
`f6f9e5787100cc4ae8c1dc875d3e929c79f5417f5c77f1daa65547715e44f04a`.
At243 the fresh owned network build tree was prepared under the qualified mapped
alias before CMake/build. Implementation then covered permanent public
SO3/O0/DenseObs64/`u1`+shared dispatcher,matching private candidate/reference/
strict GPU-only guard,CPU+actual BundleSolver DF GPU regressions and private-only
network-output-dir/target-selection batch. Native smoke results follow below.
Public`VkContext`marker/budget/capture instrumentation is **not transplanted**;
no-contraction already public,no new public CPU-fallback policy.
Fresh native full SPIR-V/reference/tail/command-buffer-owner/reuse validation
and full headless`spirula`build were prerequisites; N143–N147 clear smoke gates
below, **not the still-pending full production build or mapping**.
Network staging retains26GiB combined/10GiB floor EACH volume,8GiB Job/4GiB
Main/12GiB aggregate/9GiB available commit/24GiB per-device backing; existing
GPU headroom and all other limits unchanged. No allowance growth or completion
claim; all failures and scoped scientific acceptance boundaries retained.

### N143–N147/SDK244–248: production dispatcher smoke and scoped solver gate passed

**248 charged,pending0; all native jobs closed; Main15416 alive. Production
integration ACTIVE; full headless`spirula`NOT yet built. Reconstruction/A0–A6
remain blocked; no finished-model claim.** Source18 freeze4,790B SHA
`90d9f6dc9a53a339a6abbfcdf1068fc77a414f305d8dd278d32e4b51726901c7`.
**N143/244 actual network SfM shaders:** root0,Job354/0,654.1077681s,
peak750,170,112B;complete135,516B SHA
`7040cb1a030fbd9cb4029972f786ce9db9ba2561bdefe07c472ca0ce821a3a7c`.
**N144/245 actual network three SfM test targets:** root0,Job123/0,
97.8391033s,peak446,267,392B;complete104,537B SHA
`eb388a509097a8114066963387b20c25faff8e196caee8d9c3f688c15b73a35a`.
**N145/246 full CPU BA:**210 actual checks PASS,root0,Job1/0,31.4623639s;
complete44,625B SHA
`f78e3751595fb94c6cfe30335b0253d9bd430cd66c43ca93b00a229dc2e01d15`.

**N146/247 standalone production GPU regression:** root0,Job2/0,
63.8705271s,peak584,900,608B;76 audited checks all PASS across`c/m/w/x`,
>8,192 observations,49/50-observation partial tails,seven rejected steps restored
then accepted,cold/reused CPU/GPU parity;597 valid compute markers.
Complete74,824B SHA
`114d32c2c7503812737933034ae94622d2773631a5e12ebc181d81eb2a660615`;
behavioral qualification8,934B SHA
`e8e2602a239891e9d5c582a5529c2bc6a8e35d28fecbb3cc316bb0ddd8b38274`.

**N147/248 entire finite original-derived snapshot through SAME production
`spirula::submitObservationSchur`,absolute`Push.u1`offsets,not dispatch-base:**
pipeline flags0,root0,Job2/0,45.2988051s,peak613,494,784B.
All170,820 S+584 g+584 actual GPU-step values independently streamed
complete/finite;109 valid compute markers,34 Schur chunks.
Current durable`scoped-reference-solver-qualification-v2.json`records
**PASS_SCOPED_TO_STORED_INDEPENDENT_CPU_BINARY64_S_g_AND_CAPTURED_INPUTS_USING_PRODUCTION_OFFSET_DISPATCHER**:
outward relative S-energy upper0.00059368334932740697<0.001 and fractional
optimal predicted-reduction loss upper3.5245991926860785e-7<1e-6.
Certificate excludes CPU assembly roundoff,exact-real Schur proof and universal
DF bounds; upstream whole-BA correctness,bad-baseline parity,watchdog cause,
production mapper/reconstruction and A0–A6 are **not qualified**.
Raw S/g differences have no general tolerance verdict. One marker interleaved
at logical g row/log line171511; all critical numeric fields/vectors complete.
Current v2 audit13,578B SHA
`8883198f7a713d9b9ecf97a0e703cc34f0ce2458d224cf6d44504c25f8a8057e`
supersedes preserved v1 (12,739B SHA
`bab12f1771f2d89fc31923ee0d64c0725d8d36e5021b55c0745f5ce70e2fb41a`).
V1 mistakenly serialized an unused zero step-error accumulator; independent
all584-step computation gives maximum absolute GPU-step/CPU-reference error
2.04537081716083e-12. S/g maxima182.78515625/1.6938065527938306e-6;
counts/finite/markers/certificate unchanged. **Use v2 for current qualification**,
not v1's zero error; no dropped vector values or changed numerical acceptance.
Complete76,727B SHA
`afb873ff681edf0ee5497a6c53b58372f3f2285fb8225768a7a9482d624b037e`;
full50,560,966B log SHA
`3aaa94f6d27299d83975017053ee7c3c27ed2717c289f5c9ed34fb7536c2f47f`.

Permanent public behavior is now documented: DenseObs≤8,192 observations per
fenced chunk,prefix ownership/clear-before-submit,fresh downstream buffers;
exact-zero analytic SO3+stable half-angle form;DF-only O0/no-contraction,
default Windows DF build availability without changing mapper scalar policy.
CPU tests include all zero-rotation columns;`sfm_bounded_schur_test`requires an
explicit supported physical non-NVIDIA UUID and refuses CPU fallback.
These are smoke/numerical gates, **not full production-build or end-to-end proof**.
At248 the full headless build and separately admitted original reconstruction
were next; actual full-build/N175 deadline evidence follows below.
No private operational knobs/custody paths enter public docs.

### N148–N175/SDK249–276: full production build passed; admitted original map timed out

**276 charged,pending0; all native jobs closed;22/26 tasks done. Pricing/executing
the admitted trial is done, production integration is NOT complete.**
Source18 full headless`spirula`is now actually BUILT/native-SHA bound:
107,266,560B SHA
`bd1195b50fda49badc002eecadd1b42565a243143cabd878c6021cdf5c038e31`.
N148–N166 bounded CPU phases built all800 engine SPIR-V modules,89,811,052B;
all magic-checked/native-SHA bound/read-pinned. Metadata469,148B SHA
`0ac5876f349e28b120be82e481d819a1b737200d88ed6e694855c9d4e2ebad58`.
`production-consumer-qualified-build.json`38,502B SHA
`2a6b154d20b84269a22ab49885fac1907660d08d990dbba7c41c15b297ae8209`
binds exact26 build-phase receipts. All21,963 source/tool bindings unchanged
after **each** phase; all phases below1200s/8GiB; no end-to-end performance claim.

| Phase | Native root/Job | Wall(s) | Queried Job peak(B), where recorded |
|---|---|---:|---:|
| N167 engine embed | 0/87/0 | 46.19674 | 949,809,152 |
| N168 NN embed | 0/85/0 | 35.02791 | — |
| N169 backend | 0/116/0 | 93.63524 | 1,592,168,448 |
| N170 portable | 0/144/0 | 122.41214 | — |
| N171 NN | 0/85/0 | 38.09255 | — |
| N172 model libraries | 0/132/0 | 68.68462 | — |
| N173 final spirula | 0/116/0 | 145.82269 | 561,647,616 |

N173 complete106,833B SHA
`920b36b59dbfddabc1c591042c3f92f59a376263b026ee7e287290a819c54c2d`.
**N174 actual full-production`spirula sfm --help --lang en`,network image:**
root0,Job2/0,47.66514s;log1,252B SHA
`bdc9883230c92dd4ef53f65a06fec3fc60fa8a106c7b7fb811e8fbf19273e9b3`;
complete75,688B SHA
`3c8c7b7564d00310fb75ca24412c914c6f3821d6c3d53ffc75ad10365531b47c`.
Preowned isolated crash directory/log,0 crash bytes; operational isolation only,
no new public knob.

**N175/276 fresh full4,416 original-derived map was actually admitted/executed**:
unchanged scientific defaults/measured camera metadata,explicit DF fine/coarse,
strict no CPU BA,quiet/markers OFF,capture/profile unarmed. No old3600s reuse.
3GiB native output cap+full256MiB log reserve+4MiB metadata+213,064B external
reservation,total growth3,494,068,296B;fresh SMB allocation quantum4,096B;
network resource limits unchanged.
**1200s native deadline HIT:** root43,Job2/0,actual wall1,201.23608s;
closed/finalized,pending0. **New output root EMPTY,crash log0,NO model.**
Complete74,879B SHA
`4f47d065b9022ae7d1e1523200c5c2c051c9dbc5ac45aa1b622707ce299f616a`;
raw7,566B log SHA
`7fac5e89d1f1f123eb3668c91edb968deee63adae877c434eb5729565dac15f0`.
2,218 samples:max Job RSS1,507,831,808B,max aggregate1,778,794,496B.
True native Job committed peak **not queried on failure**;last logged GPU backing
481,229,728B,after-free GPU ledger **not observed after hard kill**.
Quiet log has **no trustworthy registered-frame count**; do not infer normal
progress,healthy GPU,completion or root cause from deadline/zero crash bytes.
Complete bounded16 October`07:37:08Z`–`08:00:33Z`Windows System Display/
DxgKrnl+Application WER query returned0,**not global absence proof**;
740B SHA
`b7d8d53e3e166c936fab6337db5252b71f5d756af929827f721e8676891d96ab`.
Observation5,255B SHA
`a8bd9101d0e703e4386dbf27b9829eeaf536285f137d6728d6d9bdd66ca805b1`;
continuation through N1754,390B SHA
`cc0415ade8922308ea1cb162cc07278cd6920729e506bd2cf71660338984fa12`.

Current blocker is **no completed original reconstruction within the admitted
deadline**,not a missing production build or broader proof from N147.
At the N175 checkpoint, read-only scouts inspected exact checkpoint/resume/
cancellation and older-success-only obj/res/lib cleanup guards; review alone
did not authorize deletion or retry. Subsequent user-requested safe cleanup is
now its own active item below. **NO retry/CPU-route/NVIDIA authorization now.**
Old3600s allowances remain consumed; preserve all failures/source/originals/
snapshots,the six documentation fixes and model/parser/checkpoint/A0–A6 criteria.

### Existing continuation routes do not checkpoint mapper state; pre-cleanup checkpoint

**At this checkpoint:276 charged,pending0;22/27 tasks done.** User-requested
actual safe old-intermediate cleanup was a separate active item; execution
follows below. Production integration remained blocked.
Read-only source findings show **no existing executable exact persistent mapper
continuation across the1200s attempts**:

- Standalone`--resume`loads completed COLMAP models then`finishModels`,not a
  saved mapper search/growth continuation (`src/app/cli/sfm_main.cpp:894–925,983–1024`).
- GUI`VKPM model.bin`is a preview:≤50,000 subsampled points,no complete tracks
  (`src/sfm/core/Progress.h:33–50`, `Progress.cpp:266–345`),not a checkpoint.
- Cancellation unwinds before`cmdMap`'s final`writeModels`; catch returns130
  (`sfm_main.cpp:1023,1227–1230`). Worker/GUI stop likewise does not persist a
  resumable model. N175 has no saved model to resume.
- Internal`Mapper::continueFrom`adopts one in-memory model,rebuilds scores,grows
  and final-refines (`Mapper.h:496–520`,`Assemble.h:605–608`); it is not a CLI
  persistent seed/search/manager-state route. Repeated`max_reg`calls have **not**
  established equivalence to the unchanged default trajectory.
- Rig restart adds another gap:`writeModels`writes separate`rigs.txt`,but
  model readers do not restore`Reconstruction.rigs`/`rig_detached`used by adopt;
  a COLMAP model is not complete rig-aware mapper restart state.

Normal nonquiet map output logs registrations (`Mapper.h:3117–3138`).
`--progress-dir`can expose preview progress but creates/deletes four files on
startup;`SFM_MAP_PROF`reports **only on return**,not live progress. None repairs
N175's absent frame-count evidence or supplies resumable checkpoint state.
At this review the next step was to finish safe older cleanup before requesting
new authority. Cleanup is now complete below; no valid persistent continuation
was found. Do not reuse consumed3600s approval,pretend`--resume`continues N175,
or infer model/A0–A6 qualification. This review did not authorize a retry or
change any scientific default/quality criterion.

### Safe older-intermediate cleanup complete; explicit next-budget decision pending

**276 charged,pending0;23/27 tasks done. Cleanup DONE; all reachable work before
the next budget decision complete, not the overall campaign.**
Current native/SHA reread proved existing`A4-source-canonical-reference-v3.py`
19,464B SHA
`55842a59c9956b720cf3f01cce0d3d1f6262dd46983372f7e6dc711f2d50897e`
present,with matching cached function origin. Unchanged
`A4-ppisp-price-producer-v4.py`60,489B SHA
`77494ce35fdb97e25d9946d826c0200da4c845eb82a5415b46786033de624ce0`
provided direct`delete_owned`,without unnecessary`NativePins.summary`/
`custody_scope`calls; those historical paths remain unqualified.
**No helper patch/monkeypatch/missing-path recreation.** Fresh Main15416 guard
qualification: correct handle+SHA deletion passed;wrong native identity,wrong
SHA and held deny-delete pin each refused with bytes unchanged. Fixtures retired
by the same guard. Qualification5,010B SHA
`f0738b78ad7772ffa9ecbba0ba701a1e151f38466762a4885b5b041307636db9`.

Exactly59 successful-producer-log-supported COFF/archive intermediates from
each Source13/14/15/17: **236 files actually removed**,770,919,059 logicalB/
771,452,928 native-allocatedB,**not physical disk-recovery proof**. All236 passed
native DELETE-access/native-ID/SHA preflight before first retirement;handle-bound
SHA checked again at delete. Intent523,761B SHA
`138bb457b06d39298bce05c705fb65a15c6d90176171347b38be8c40036aa46a`.
All44,587 other paths across owned local/network roots preserved:before native/
SHA records under read-only deny-delete pins,after full native/SHA identical and
whole post-file namespace exact;protected21,353,350,845 logicalB.
Chunked witness106,165B SHA
`9b05073a793da9e898ed1b955cd954a5a9f4de256581afc443fe91d246142a1d`.
All temporary protection handles actually closed/live0. Prior594 retirements
**not recounted**;Source16/18 and every failed tree/source/SPIR-V/embedded C++/
image/log/receipt/original/snapshot retained. Cleanup result9,272B SHA
`d9f78f20abf93709549453e28776562e797a5099e561fbb6893e0ef278d031cf`.
Post C free12,705,906,688B;combined retained logical21,378,820,163B including
fresh witness metadata,not future admission/native-physical-peak proof.
Parent filesystem-API cleanup launched **no native process/no charge increment**.

At the cleanup checkpoint N175's1200s deadline remained the blocker,no longer
build/old intermediates;no exact persistent continuation. The presented choice
was one fresh3600s full map with normal registration visibility or a bounded300s
progress diagnostic. **The user subsequently selected the former below.**
Consumed old3600s approvals stay consumed;production integration/full model/
parser/checkpoint/A0–A6 remain unqualified and all failures/gates retained.

### New explicit one-attempt3600s approval; N176 prelaunch admission

User explicitly selected **“One 60-minute mapping attempt.”** This is **new**
authority,maximum attempts1,not revival/reuse of prior consumed3600s approvals.
`new-one-attempt-3600s-approval.json`1,808B SHA
`c8c3e71eaf330af895763b7f7e24d61cf37f9c949349ff6f9dedeffdc3a57989`.
**At prelaunch:276 charged,pending0;N176 not started/consumed;23/28 tasks
done.** Actual execution/consumption below supersedes that historical state.

Frozen`main_native_run_network_v3`already accepts deadline≤3600s with
`FULL_ORIGINAL_NATIVE_MAP_WITH_BOUNDED_PUBLICATION`,pinned`mapping_approval`
and exact`explicit-reconstruction-deadline-approval-v1`format;**no frozen helper
change**. The parent-held scientific helper was derived only to forward optional
approval and enforce one-use durably before launch;unchanged native-V3/ACK
protocol,not a scientific/resource waiver. Extended admission7,745B SHA
`8d141fb3847a59f5e0f401d2cc9289bc0ba770db3f85968124c3195b335d8060`.
Repriced network growth3,494,068,296B:3GiB output+256MiB full log+4MiB
metadata+213,064B external;fresh preowned empty`map18-long`output.
Full4,416 original-derived corpus,measured camera metadata,DF fine/coarse,
strict no CPU BA and scientific defaults unchanged. **Only`--quiet`removed**
for normal registration visibility;markers/profile OFF,all caps/floors unchanged.
Do not infer started/completed/model from admission. Preserve N175/all failures
and exact current scoped numerical qualifications;full model/parser/checkpoint/
A0–A6 require actual subsequent evidence.

### N176/SDK277: new3600s allowance consumed; real partial progress, no completed model

**277 charged,pending0;all native jobs closed.** New one-use approval was durably
**CONSUMED before launch**,2,584B consumption SHA
`100386b6efd0af41fd56f1b9f381fe3361d68bfadd5f89ff6364f2175f11866a`.
**N176 actual deadline failure:**root43,Job2/0,3,601.0694249s,
closed/finalized;crash log0,new output root **EMPTY,NO model**.
Complete79,257B SHA
`0ba5dc1ae480bc776c6b20690abf54883bd71f51e71ee722e47ebe4b549f8087`;
raw94,104B log SHA
`ed2b3284d86d2c44ee1982f0766fd2a1f11a33ef476c44d94c9fa82311cded3a`.

Audited876 distinct registration log rows+two seeds give **last-reported active
model878 images**.79 global BA log rows all finite costs;last cost2.763e6,
filtered2,145 observations/925 points,381,104 points remaining.
Full4,416 images/four cameras/measured metadata logged;default unused-feature
compaction17,669,462→8,324,330. This establishes **partial real mapper progress**,
not persisted/completed model,full coverage or quality.
Last logged GPU backing918,192,576B;after-free ledger **unobserved**.
6,726 samples:max sampled Job RSS1,488,842,752B/aggregate1,902,841,856B;
true native Job committed peak not queried on failure.
Complete bounded16 October event window`09:59:00Z`to observation end around
`11:00Z`:System Display/DxgKrnl+Application WER each0 events,
**not global absence or watchdog-cause proof**. Observation60,913B SHA
`ffb172fdf6449ba3bd10709c0a78e4abb8c6283173d6c06b417395c26078d20b`
in`N176-extended-map-deadline-observation.json`.

Approved-attempt task DONE at24/28;new bounded-throughput-diagnosis task added,
**24/29 done,diagnosis ACTIVE**. Production integration/model/parser/checkpoint/
A0–A6 remain blocked;**NO further full-map trial/new deadline authority**.
Read-only scouts inspect frozen Source18 finite-state replay modes and existing
Schur alternatives for **≤1200s component diagnostics,not another full map**.
Hypotheses only:shared-intrinsics atomic contention,DF preparation/JIT,or CPU/
solver overhead. N147 Schur1,385.89ms versus Cholesky+triangular4.08ms describes
**that captured component**,not whole-map runtime or established bottleneck.
No permanent code/scientific-default changes yet;diagnose bounded components
rather than blindly request more map time. Prior236 cleanup retirements/
44,587 protected paths,all sources/originals/snapshots,limits and failures retained.

### N177–N178/SDK278–279: bounded schedule pilots; shared-column feasibility review

**279 charged,pending0;all native jobs closed;24/29 tasks done,throughput
diagnosis ACTIVE.** User status remains explicitly incomplete,no finished model/
A0–A6,no ETA. No further full-map/deadline authority or permanent/default change.
**N177/278 marked64-vs4-group finite-state pilot:**root0,Job2/0,48.77698s;
all171,404 S/g values compared finite,markers validated,after-free0.
Experimental dispatch-base,`u1=0`,one pipeline: **not production-offset timing
or independent solver certification**.64 groups/34 dispatches:host1.3993141s,
GPU1,385.17ms;4 groups/529 dispatches:host11.2187665s,GPU11,020.60ms
(rounded cumulative delta). **About8× slower:reject4**,no confirmatory pairs/
public change. Complete78,408B SHA
`04bc24f4e548a24985552dbdd46593bed73e32f825afaefcd2b44c1eca74ee75`;
log255,852B SHA
`da1315650a513e0053359e6155a8113768ad6943ef1cd23993e83ff8795e2ba7`;
pilot3,688B SHA
`81b48c5c69125de39a95885b4b135512f648356c74c4c83277354bbe74836c67`.

**N178/279 separate predeclared stepwise128-group pilot:**no long baseline;
one increment motivated by observed64-group max host0.1086s.
Root0,Job2/0,39.642957s,all171,404 values finite,after-free0.
64 groups:host1.3934028s,GPU1,377.46ms,max host0.1168628s;
128 groups/17 dispatches:host1.3214117s,GPU1,314.73ms rounded cumulative
delta,max host0.1840734s. **Only about5% difference in one marked pilot**,
not production tuning,full-map speedup or promotion. Raw48,205B log SHA
`9699ce7ba4e9a6162cd7f21d4827011f7ecba60c94766523601b4c1f1ecb7f5c`;
pilot3,463B SHA
`d0ba5a96cede8c43f81ad927da62fe23736f6bc7b5466f2e5875dcc24fa3cff2`.

Captured CSR47,379 points/270,647 observations,median track3/max82,
sum squared lengths4,521,917,unordered cross-observation pairs2,125,635.
Host chunk duration versus neighbor-traversal correlation0.96668288 across34
64-group chunks,0.99072265 across17 128-group chunks: **correlation,not
causality/GPU-time/whole-mapper proof**. Exact per-chunk work metadata
`finite-point-track-work-observation.json`SHA
`a33500415486455a16d27a764d090734245c1345b7a5f81b5bcf8de4566a3e40`.
DenseObs visits each observation's whole track/global DF CAS;shared columns are
not current DensePair-eligible,CG not a drop-in snapshot route. The completed
read-only review below resolves the candidate contract,not implementation or
performance. No new edit,Source19,builder or full-map attempt created;all
scientific gates and retained failures unchanged.

### Bounded diagnosis complete; existing CPU trial versus shared-pair GPU work decision

**279 charged,pending0;all native jobs closed;25/29 tasks done.** Bounded
throughput diagnosis DONE;four integration/model/A0–A6 tasks remain blocked.
No Source19/new repository code/build/full-map attempt or approval.
Actual finite captured state has16 DoF/image,94 unique frames,no members,
two groups/free10 intrinsics each. Source/topology accounting gives631,486,962
potential S accumulation sites and4,330,352 gradient sites per assembly **before
finite guards,not actual CAS retry counts or whole-map timing**.
There are623 same-image distinct-observation pairs across491 tracks,max
multiplicity2: **preserve duplicate observations,do not deduplicate**.

Completed334-line read-only`shared-pair-candidate-review.md`requires reuse of
existing pair machinery with homogeneous self/cross buckets,forward **and**
reverse contractions preserving raw W's packed-lower target despite
8.3267e-17 asymmetry,bounded Y+pair`u1`dispatch,and separate legacy CG eligibility.
Atomic flag alone misses one ordered shared-global diagonal term;current
diagonal-image buckets mix self/cross observations and mis-add normal/gradient
terms for duplicates. Candidate adds`x`entry and up to9,728B DF local staging.
Seven-file private candidate plus substantive numerical/lifecycle/large-offset/
scale regressions and unchanged whole-snapshot certificate are needed.
Topology-derived upper output sites1,722,928 S+5,728 g versus631M/4.3M old
are **not measured atomics,CAS retries or speedup**. Pair tables+Y≤32,216,504B
payload for this state,excluding host capacity/alignment/scratch/other retained
state;all host/device admission remains required. Two schedule pilots are not
promoted;no assumed throughput win.

Existing compiled native binary64`bacpu::Solver`can be selected with **both**
`--ba-real cpu --ba-real-coarse cpu`;210 CPU checks cover camera models/shared
intrinsics/rigs. Duplicate observations are processed,not deduplicated,but lack
a dedicated regression. Dense packed/blocked Cholesky and auto implicit-Schur
CG (dimension>8,192/over budget) already exist. Its half-physical-RAM estimator
is **not** whole-map/current-available-RAM/8GiB-Job admission;global over-budget
can warn/proceed,so external caps remain mandatory. Intentional CPU route
requires`SS_SFM_REQUIRE_GPU_BA`and its SSPLAT alias absent.
Other stages may still use Vulkan,including private mapper context/bootstrap;
**not GPU-free;no CPU trial or full-map performance/memory/ETA proof**.

At diagnosis completion the material choice was one default1200s full4,416-image
CPU-both-phase attempt using the existing implementation,or continued GPU-only
shared-pair implementation/qualification. **The user selected the former below**;
the earlier CPU-unapproved state is historical,not a standing production fallback.
No speed/ETA promise;preserve N175/N176 failures,all originals/snapshots/scoped
gates and consumed full-map deadline approvals.

### New explicit one1200s CPU BA trial approval; N179 prelaunch admission

User explicitly selected **“One 20-minute CPU BA trial.”**
`one-CPU-BA-trial-approval.json`1,917B SHA
`3d3bb2a21667a81b312a8854b7c5055c85e4d766357edf60193b26e8eed0c344`.
**At prelaunch:279 charged,pending0;25/30 done;N179 not started.**
Maximum attempts1,1200s only;durable consumption before the single call and
actual outcome follow below;no future CPU/full-map retry implied.
Admission7,876B SHA
`864f6cc0add1c4d890ecc8ebfec089d228aaf383a3df467ac19579bd7c53f95a`.
Use existing Source18 full executable/native binary64 solver,**both**
`--ba-real cpu --ba-real-coarse cpu`,no new code/Source19/GPU candidate.
Full4,416 corpus,measured cameras and all other defaults/resources unchanged;
both`SS_SFM_REQUIRE_GPU_BA`and its SSPLAT alias planned asserted **ABSENT**
in parent/child. Normal registration output,markers/profile OFF.
Fresh preowned empty`map18-cpu`output,network growth3,494,068,296B:
3GiB native output+256MiB log+4MiB metadata+213,064B external,fresh price.
Training remains Vulkan;other mapping stages may still use Vulkan,
**not GPU-free or a general CPU production-fallback policy**.
Await actual receipt before completion/model/throughput claims;all previous
failures,scoped gate exclusions and model/parser/checkpoint/A0–A6 criteria retained.

### N179/SDK280: CPU trial consumed; partial progress,1200s deadline/no model

**280 charged,pending0;all native jobs closed;26/30 tasks done.** One-use CPU
authority **CONSUMED before launch**;no future CPU/full-map authority follows.
Existing Source18 executable unchanged,explicit CPU fine **and** coarse/native
binary64,SS+SSPLAT GPU-BA-required guards absent;intentional route,not implicit
fallback. Training remains Vulkan,other mapping stages may still use GPU.
**Root43,Job2/0,1,200.9008417s hard deadline;new CPU output EMPTY,crash0.**
Complete76,480B SHA
`fbab94bcfdc6593dfc9193c60cbf339785288775016bf9fc5f3067ee1608d53f`;
raw126,341B log SHA
`97aea05201c502682f6873c7425753e87402839856d727ab8b4b6ab43882856d`.
Audited1,414 distinct registration rows+two seeds=**last-reported active model
1,416 images**;90 global BA finite-cost rows,last logged cost4.110e6,
filtered2,245 observations/973 points,587,676 points remaining.
Full4,416 images/four measured cameras retained at startup;default unused-feature
compaction17,669,462→8,324,330.2,133 samples:max sampled Job RSS2,850,635,776B/
aggregate3,266,560,000B;true native committed peak not queried on failure.
Observation93,009B SHA
`1bd2855df0fe5fd6f4c4917f37ec9d1c2c59b97936563882e6405d19115ecb8c`
in`N179-CPU-map-deadline-observation.json`.

CPU1,416 images/1200s versus GPU878/3600s is **observed partial progress with
different precision/trajectories,not a matched performance/quality benchmark,
linear extrapolation or ETA**. CPU route is promising,but deadline still prevents
final writer and no exact persistent restart exists;**no completed model/parser/
checkpoint/A0–A6 credit**. The then-proposed request for new3600s CPU approval
was **declined by the user**,who selected unchanged-limit GPU optimization below.
Old GPU3600s/CPU1200s stay consumed;no new mapping attempt or GPU-free claim.
All inputs,scientific/resource limits and prior failures/scoped gates preserved.

### Selected next route: keep limits and optimize GPU BA

User explicitly declined a longer CPU run and selected **“Keep current limits
and optimize GPU BA.”** **No new CPU3600s/14400s authority granted.**
**280 charged,pending0;26/33 tasks done**:three candidate tasks added alongside
the four original blocked integration/model/A0–A6 tasks.
Parent is admitting an independent Source19 from immutable Source18 for the
reviewed shared-parameter DensePair candidate in`shared-pair-candidate-review.md`.
**Admission in progress,not compiled or qualified. Automatic shared production
selection stays OFF until qualification**;this is not a measured speedup or
completion/default-promotion claim. All existing resource/deadline caps remain
unchanged;the reviewed numerical/lifecycle/scale/whole-state-certificate gates
remain prerequisites. Preserve N179's actual deadline/empty output and all
consumed approvals/failure evidence;longer CPU execution is not the selected route.









































### Replacement completion contract

Proceed from current runtime/resource and original-file admission to exact
stream/track/timestamp/projection/calibration lineage, original-derived frames,
masks and native reconstruction; then admit the native parser, effective config,
fresh splits/counts, source checkpoint and independent native references.
Bind a new versioned producer declaration to its actual required inputs and
consumers, not the inherited historical dataset closure. Prove full isolated
restoration and four actual restored CPU consumers before candidate changes.
Preserve numerical, optimizer, six-metric native-relative quality, coverage,
matched timing, long recovery, failure/cancel/publication and A0–A6 settlement
criteria unchanged. Native remains default.

Price each dependent preparation/custody/science tranche from observed corpus
and command/resource requirements before execution, within the remaining global
ceiling. This amendment supplies replacement-route execution intent, not
invented costs, relaxed gates or guaranteed completion within206.

## Replacement dataset plan — 2026-10-03

**Current authority: the selected raw-dataset policy below. A0–A6 is not
settled; the former dataset's accepted Source v5/readback is historical only.**
The 2026-10-03 replacement selection and its executed records remain immutable
history. They do not qualify the newly selected corpus, and no old input/source
binding may redirect to it. Native remains default; historical Q's decoded
`1e-4` failure remains FAILED.

For prospective work, read the selected-raw policy below, the
[approval and testing gates](#autonomous-testing-and-production-promotion),
[new-dataset execution order](#new-dataset-execution-order), then
[A0–A6](#autonomous-execution-and-completion). Dated execution records preserve
historical evidence and permissions; they are not instructions to resume.

### Selected raw dataset for all future evaluations — 2026-10-07

The user selected a new recording directory for **all future dataset-backed
evaluations**, and explicitly chose **full-resolution `.insv` originals**.
Exact paths and recording names are sealed privately in
`A4-next-dataset-selection-20261007.json` (7,786B, SHA
`538b4c8f368dff4e07226776c3895f5e682d83fce6d1d2f6a787046d85486bb3`),
not repository documents. This supersedes the earlier dataset and the
integration-only restriction for prospective work. Integration, coverage,
numerical/quality/recovery checks, matched performance and full A0–A6
qualification must use newly admitted inputs derived from these originals.
Cheap historical SDK argument fixtures remain contract tests, never new
dataset evaluation or replacement native references.

Existing preparation is explicitly **`.lrv` preview-derived**, including
its extracted frames and current masks; it is not full-resolution-original
admission. Preserve the user's live preparation unchanged. No relabeling,
proxy fallback, reuse of previous-dataset checkpoints or silent decode-source/
resolution change is permitted. Freeze the selected original-file set,
decoder/extraction/projection, tracks/timestamps/calibration/colour and
raw-to-frame/mask/reconstruction lineage before preparing evaluation inputs.
Then require finalized native camera/pose/point-cloud reconstruction, native
parser readiness, effective config and fresh train/held-out ID/name/post-camera
mapping. Newly derive and freeze global/train/held-out counts, owner masks,
coverage/exposure totals, source/binary/evaluator/native references and every
whole-phase resource bound; **2,986 /2,688 /298 and old byte bounds are history,
not the new workload**. Observed original-file presence is not content hashing,
decoding or admission.

Six-metric matched native envelopes, numerical/optimizer/quality/recovery,
failure/cancel/publication/custody and resource criteria remain unchanged.
Full qualification covers the complete newly admitted corpus; a bounded
integration subset cannot substitute. Historical sources/failures/receipts
remain as qualified by their actual retention records, never re-bound.
**Plan update only:** no preparation/parser/compiler/GPU/SDK/evaluation/training
launch or budget increase is granted. Global **101 charged /193 authorized**
and the existing93-call custody obligation remain; derive the new matrix's
actual shared costs and obtain the missing finite envelope/execution approval.

**Execution request — 2026-10-07:** the user subsequently requested execution.
This supplies execution intent, not a numeric budget increase or fresh admission.
Readonly preflight verified the selection manifest's exact size/SHA and read both
selected originals: **19,777,805,933 bytes**, with stable same-open-file
`fstat` observations before/after each whole-content SHA-256 calculation:
`fd45922b323d4fba6369ca259424ca90b333efb5df8069cc3ead9aa069efd0e8` and
`fbecea892b103ff110d4b4a0d3a196094e0f2fe7e17767667b19ca45def28b8a`
(manifest order). This is content preflight, not full native custody, decode,
calibration, reconstruction or corpus admission. No SDK/build/GPU/training
launch occurred. The finite matrix/ceiling and staged preparation admission
remain required before their dependent runs.

### Current cleanup and testing revision — 2026-10-06

**Current authority: dependency-safe cleanup and plan updates ONLY; execution
remains blocked.** The three testing lanes remain, but the
[selected raw-dataset policy](#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
now controls every future dataset-backed evaluation, not just integration.
Earlier campaign authority and present raw-file availability do not authorize
SDK retries, successor declarations, preparation/compiler/GPU/science work or
a budget increase.

Fresh original-recording preparation, native-parser/config/split admission
and source/executable/reference bindings are pending. Historical Source v5,
2,688/298 counts and earlier small-preview outputs are not readiness for the
new corpus. Native remains default; A0–A6 is not settled.

**Dependency-safe cleanup completed:** three disposable local interpreter
caches were retired through the existing exact-native-handle deletion path,
with same-handle SHA verification and durable intent/completion records.
Removed **108,892 logical bytes / 114,688 allocated bytes (112 KiB)**.
Immediate observed free-space increase was110,592B, not attributed exclusively
to cleanup; free space after the final receipt was **25,847,500,800B
(24.072 GiB)**. These observations do not admit a future run.

Post-cleanup readonly full-native/SHA preservation passed **147.6712326s**
over all **33,167 current protected bindings**, including the unchanged
archive, all3,743 originals and each3,743-file V9/V8 alias; native physical
owners and triple-byte equality passed. Original cache sources, active
binaries, compile inputs and historical science remain intact. No high-value
archive/build deletion was proven eligible. Cleanup receipt10,806B SHA
`34e67157e01018c351dcea4db4d63804054f46ea2a3769f8cebebae59814daa2`;
its final journal is sealed. No SDK attempt, compiler or science run occurred.

Continue applying the [three-class rule](#aggressive-dependency-aware-cleanup)
and [resource/coexistence caps](#a0--bind-the-retained-evidence-and-fresh-resources):
no future-deletion credit or namespace-based cap reset. Preserve frozen
Source3–17, actual receipts/journals/full native identities and old V6 retention.
Source17 final facts and journals remain sealed; SDK99 is unresolved, and
process absence cannot authorize retiring its runtime namespace.

### Deeper cleanup and source availability — 2026-10-07

An artifact-specific audit separated ordinary generated test executables from
actual scientific/helper/admission bindings. The broad postbuild product
inventory and ordinary CTest path/pass logs do not require retaining every
leaf binary. Actual scientific helpers and named admission targets remain.
**66 disposable leaf executables were retired**, removing **3,598,446,080
logical / 3,598,548,992 native-allocated bytes (3.351 GiB)**. Each deletion
used the existing exact native handle, same-handle SHA and durable intent/result
journal. Immediate observed free-space increase was3,598,598,144B; final
observed free space was **21.229 GiB**, not future admission or an exclusively
attributed volume delta.

The active app, scientific helpers, compiler, source/object/library/DLL inputs
and build metadata remain. All **171 actual relink prerequisites** are present;
**27,232 retained bindings** passed readonly full-native/SHA before/after
checks. Original/archive/both-alias bytes and native alias owners are unchanged.
Receipt141,079B SHA
`e5dd368c6bbf284e1d993a75f57c165e12f66ef7ebf16c53c135955dfad55629`;
its46,250B journal is sealed. No rebuild, test, SDK or science run occurred.
Ordinary removed tests need regeneration through the dev build script before
the next CTest invocation; intact prerequisites are not an exercised rebuild
claim. Preserve bound compile inputs during any later relink/build.

The user separately reported deleting older source-data files to make room
for the small run. Static reference mapping places **6,056 frozen encoded
image/mask/reconstruction bindings** under that reported source location;
the exact deleted leaves were not supplied. Treat that report as ground truth,
not a cleanup performed by the assistant. Those6,056 inputs were not reprobed
or included in this preservation claim. Full-dataset availability is now
unverified: require fresh input admission before any full qualification or
consumer that needs them. Historical positives remain historical; retained
checkpoints do not replace missing encoded inputs or recreate native identities.
No old input binding may silently redirect to new selected originals. User
dataset folders remain outside cleanup. The reported deletions affect historical
reproduction, not entry to a distinct freshly admitted corpus. The later
all-future-evaluations raw-dataset selection explicitly supersedes the old
integration-only/2,688–298 prospective scope; it changes no acceptance tolerance
or SDK budget and does not make the new corpus admitted.

### Temp allocation and historical retention — 2026-10-07

A fresh whole-Temp inventory measured **53.416 GiB of unique file allocation**,
not the sum of logical sizes or duplicate hardlink paths. The current
continuation occupies **12.382 GiB**; historical G1 cost/execution trees occupy
**14.373 GiB combined**, after counting their shared1.006GiB only once;
the four large B2 historical trees occupy **13.939 GiB**. These are occupied
bytes, not automatically reclaimable scratch.

Removed **22 inactive, repository-attributed Clang preamble caches**:
360,340,172 logical / **360,386,560 allocated bytes (343.691 MiB)**.
All had the CPCH signature, one link, current-repository content and no
current/historical native binding or exact text reference. Fresh producer-idle
checks and exact-handle/SHA deletion preceded retirement. Two unattributed
preambles, unrelated installer/cache files and all scientific payloads remain.
Actual target absence, **27,232 readonly full-native/SHA checks**, unchanged
alias owners and **2,778 other Temp file native-stat checks** passed.
Receipt33,037B SHA
`a329ed4c9fc21aa17e79b0400e7be4b868a556921199d5ee7d6d61d1c28c7e1c`;
its30,495B journal is sealed. No SDK/compiler/test/science run occurred.

The second inventory measured **53.080 GiB allocated files** plus32.441MiB
of directories; one transient RPC-output path disappeared during traversal and
was not measured or deleted. Later receipt bytes are additional, and observed
free space is not future admission. Large completed historical G1/B2 outputs
are executed replay witnesses, not disposable merely because they live in Temp.
An explicit artifact-scoped historical-replay retirement decision is needed
before deleting them without a proved restoration/consumer cutover. Preserve
current Source3–17, accepted native Source5, first failures, source/archive/delta
dependencies, sealed receipts, old V6 retention and unresolved SDK99. User
dataset folders and the unchanged full-qualification contract remain outside
this cleanup.

The user explicitly selected **“Retire reviewed G1 payloads”**. Exactly
**41 physical owners /63 paths** were retired, removing **11,627,208,864
allocated bytes (10.829 GiB)** from closed positive historical G1 timing/
transport outputs. Same-handle whole SHA/full native identity, readonly-safe
disposition, complete-link ownership and post-close alias decrements/absence
were recorded. Current dependencies and outside links were excluded; two older
source-proof roots and five mixed failed-child/native-readback smoke sets remain,
including their **22 owners /5.495GiB**. No receipt/source/command ledger was
rewritten. Selected old raw packet/native-state/FNV reinspection and byte-exact
replay are deliberately retired; retained receipts do not reconstruct those
payloads. No current qualification, optimizer, resource or SDK-budget gate changes.

After retirement, **27,157 current protected bindings +2,617 nonselected
historical project files** passed full-native/SHA checks. Alias owners,
original/archive/both-alias bytes, old V6, first failures and sealed receipts
remain exact. **75 System32 DLL readers refused the unchanged strict
attribute-transition guard**; subsequent query-only observations matched every
expected native field for all75. No persistent drift is established, and no
whole-native/SHA pass is claimed for those75 calls. Initial attribute values and
cause were not captured; no mask, preparation override, retry or rebind was used.
Preserve those refusals and require the original strict environment contract
before later qualification. Unrelated locked Temp files and a running GUI/build
were neither forced unlocked nor stopped.

Final Temp inventory: **44.251 GiB file allocation**, plus33.207MiB directories,
no unmeasured paths. Cache plus approved G1 deletion removed **11.164 GiB**
of native file allocation; concurrent activity and publication metadata added
a net approximately2.000GiB to Temp. Occupancy, reclaimed allocation and volume
free-space deltas are separate facts. Receipt142,812B SHA
`d2ff8c4ec67d019be3d54d439e2c05c4f61d68d6e44ae08e0b49c34f03652bfc`;
its140,551B journal is sealed. No compiler/build/test/SDK/science launch by Main.

### Autonomous testing and production promotion

Reuse existing checks and the repository [testing policy](../testing.md);
new production paths remain platform-native C++, with no Python runtime/build
dependency, duplicate SDK/consumer or new test framework. Test the **exact
production code** through its real interfaces, not mock trainers, replacement
consumers or an AST/API smoke promoted into runtime acceptance.

| Lane | Required evidence and boundary |
|---|---|
| Cheap contract regressions | Use authentic accepted history plus disposable negative fixtures. Permanently cover valid Main-owned SDK95 arguments outside worker scratch; reject wrong full binding/argv/closure/stable caller identity. Exercise real TEMP/TMP scoping, exact restoration on success/failure/cancel and non-overlap of pending invocations. |
| Small native integration | Use an explicitly bound subset of the newly admitted selected full-resolution-original corpus. Exercise the actual trainer/checkpoint/save/restore/eval/worker lifecycles, bounded owned failure/cancel, native family closure, genuine negatives, environment restoration and safe cleanup. Subset passes do not qualify the full corpus. |
| Full qualification on selected originals | Use the complete newly admitted selected corpus with its fresh frozen train/held-out mapping, source and native references, not the old 2,688/298 bindings. Preserve all A0–A6 numerical, quality, coverage, resource, timing and recovery obligations and the six-metric matched envelope. Existing custody obligations still require both actual child pairs, all four full unchanged C9 computations, Root + four true native family witnesses and genuine Main ACKs before destructive authorization/release; new data cannot rewrite old custody declarations. |

Before each test, bind an immutable executable/source snapshot, exact inputs,
config, commands and expected positive/negative outcomes in a named local
manifest. Preserve executed snapshots and actual failed receipts; never patch
executed code or sealed Source17. A prospective repair must pass accepted95
compatibility, environment/restoration/non-overlap, real failure/cancel closure
and cleanup gates, then the actual both-pairs/four-C9 custody gates, followed
by fresh production admission. Static review or the recorded 2.5417302 s API
smoke alone satisfies none of the missing child/full-C9 promotion requirements.

**Autonomous repair is prospective and finite, not currently executable.**
Before approval, choose and bind the named test matrix/manifest, each test's
actual SDK-call cost (including failures/cancellations), diagnostic allocation,
production-retry allocation, total ceiling and stop conditions. Count every
SDK call globally; reserve the unchanged **93 production calls**. Currently
**101 charged / 193 authorized / 92 left**, so even production-only 194 needs
new approval. **206 = 101 + 93 + 8 diagnostics + 4 retries is an illustrative
choice, NOT approved or a final allowance**; derive allocations from the
chosen tests, not this example.

Within an explicitly preapproved envelope only, the agent may diagnose an
observed failure, make an attributable prospective repair and rerun affected
gates without asking at each ordinary step. Stop before an unbudgeted call,
allocation/deadline/publication-cap breach, missing dataset/input binding,
unclosed owned family, unresolved custody/dependency or any proposed change
to acceptance. Preserve the actual negative and ask for the missing material
authorization; do not silently consume the production reserve. Deadlines,
numerical tolerances, resource/publication caps, all 90 destructive
authorizations and original-plus-both-alias retirement rules remain unchanged.
Required approvals are the finite diagnostic/production-retry envelope and
its global ceiling, plus explicit execution permission. Fresh selected-original
corpus admission/configuration is also required; bind any integration subset
separately without substituting it for full qualification. Neither admission nor
this document review supplies execution approval.

#### Derived custody-envelope proposal — 2026-10-07

The user requested a complete finite envelope, then explicitly **approved the
206-total custody/promotion tranche**. The named allocations below are now
authorized; each invocation still requires fresh launch admission. This is
derived from named calls, not the earlier illustrative allocation. It does
**not** price or authorize selected-raw preparation, native integration or
A0–A6. Those lanes remain in scope and blocked on their own input/command/
resource admission and call matrix. Opening charge remains101; no call is
credited merely by this approval.

**Executed admission / current stop:** approval was native-sealed (2,767B,
SHA `81dee465d7e46091432249b83545850f2c4f134a3622c0d8dc764992ee35d4da`).
Fresh authenticated Main ran **SDK102 / M01**, accepted in **4.4903515s**:
root native/Popen exit0, signaled original witness in the exact owned Job,
terminal **total1 /active0 /terminated0**, peak Job commitment5,722,112B.
Receipt12,898B SHA
`52e675af5472adccf017e65ec754bcadbb840740b803b0bcf8714b5aa667fb46`.
Current ledger is **102 charged /206 authorized /104 remaining**; all93
production calls and three production replacement slots remain unconsumed.

P02's **ten independent full-binding refusals passed**, with the sacrificial
fixture's exact native/SHA binding accepted before and afterward; zero SDK calls.
Receipt4,184B SHA
`8e6cdb83467e66bd40d8f3f1333eaec14a5ee77aec53d7c1ac9949ca7baee0b4`.
Source17's declaration itself passed exact native/SHA validation. Its protected
dependency preflight then passed **92 of21,970 rows** and stopped at index92,
the AMD DriverStore `amd-vulkan64.json`: **“unexplained native attribute
transition; no mask/retry.”** Receipt1,318B SHA
`bbbb6258ce49dd2687b04d7121c8c70bf5b5c0d0470932d1d5d7e7ae7027bfd0`.

A query-only follow-up matched every expected current native field, including
attributes128; it is not a successful strict read or a continuity proof.
Initial/opened transition values were not retained, so neither hydration nor
another cause is established. The reader's preceding double-read/internal
state checks do not establish expected-SHA acceptance after its refusal.
Follow-up1,404B SHA
`49721045bafd557e7c28caef55dd86366bf7cfd65002bfd5127046ab411efc72`.
No retry, attribute mask, preparation override, rebind or further SDK call
followed. Contract-wrapper/successor development is not sealed runtime
acceptance. **BLOCKED, NOT SETTLED:** require attributable resolution and fresh
strict dependency admission before R01–R03 or production. No original member
or alias was retired/released; original scientific and SDK99 limitations remain.
Final blocked-state receipt5,807B SHA
`4b800e2e7024531568408c9df501ce0a9a9efbde6e3399717097f247292ced2d`;
its18,511B ownership journal is sealed, SHA
`a64030a6378b22c78e6529f2c02cae7911bc1509d53f50e9255185cd8f1afae3`.

**Approved diagnostic amendment:** the user approved updating and executing the
failure-observability plan. Bind a new versioned observer around the unchanged
strict reader; preserve all four snapshots, raw/decoded attribute bits and XORs,
both actual hashes and expected SHA, exact open/query methods, timestamps and
the failing stage before re-raising the same refusal. Validate unchanged reads
and deliberately introduced attribute transitions on owned disposable fixtures.
Then perform **exactly one instrumented diagnostic observation** of the affected
AMD manifest. This is an explicit diagnostic exception to the prior no-repeat
stop, not retry-to-pass or production acceptance. No target attribute writes,
hydration override, rebind, driver reinstall or reboot-as-a-fix. A non-reproducing
observation is inconclusive, not resolved. Attribute any repair to demonstrated
evidence and preserve the original failure and frozen reader.

This amendment uses **zero SDK calls** and does not repurpose D01, which remains
restricted to R01–R03. Ledger102/206, production reserves, scientific scope,
resource limits and all acceptance predicates are unchanged. The one target
observation cannot authorize R01–R03 or retirement; fresh strict dependency
admission remains a separate gate after attributable resolution.

**Diagnostic executed:** the versioned observer calls the original strict
reader once with `preparation=False`; it does not replace its bytecode or
acceptance predicates. Three owned fixtures passed: unchanged attributes32
returned successfully; readonly-bit transitions32→33 after the initial snapshot
and after opening each raised the original attribute-transition refusal.
All four snapshots and both hashes were captured, without truncation/observer
errors; prior tracing and fixture attributes were restored. Fixture summary
4,086B SHA `0b5dc7c7e4ea4742026fabc2d10fd3422752ea35252b18af2571e21a2288e377`.

The **single authorized target observation did not reproduce the refusal**.
All four native snapshots matched the historical expected identity, size and
mtime, with attributes **0x00000080 throughout** and all attribute XORs zero.
Both complete703-byte reads matched expected SHA
`5f3395c67a3f2a93b4aa09f55a0c673568db9d2457065520886ab3d77394193a`.
There was no observer error/truncation. Complete observation200,316B SHA
`cdd26f529a13cbb4a26a42a8902a5488a71524d39592cf88e7978ac7b9d7a6f6`.
This establishes the file's observed current state, **not the earlier cause,
historical continuity, a repair or production admission**. Tracing itself can
perturb timing. Classification: **INCONCLUSIVE NON-REPRODUCTION**.

The [Microsoft attribute constants](https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants)
also expose a frozen-reader naming mismatch: its preparation comment names
`RECALL_ON_DATA_ACCESS`, whose value is0x00400000, while the predicate tests
0x00040000 (`FILE_ATTRIBUTE_EA` / directory-enumeration `RECALL_ON_OPEN`).
The diagnostic records that API-context ambiguity correctly. Frozen source and
predicate remain unchanged; this mismatch is **not established as the cause**
of the earlier strict-live refusal.

Interpretation2,292B SHA
`30ab398362d6498bb88a5564dd20e2e6a69c32aff718e561994fc9d06b074fcc`.
Only the three disposable fixture payloads were exact-native/SHA retired after
attribute restoration; reports/source remain. No target writes, override,
rebind, additional target observation or SDK call occurred. Ledger **102/206**
and production/scientific gates are unchanged. The single-observation allowance
is consumed. Any next reproduction needs a separately bounded diagnostic
amendment; the useful next discriminator is a first-touch observation with
failure snapshots armed before admission, not another warm retry for a pass.
Final diagnostic receipt6,133B SHA
`1cdf8e89cc93caf8b1d5243c48f73e54583ef1c00daa92b04ce75e2928389856`;
its33,253B journal is sealed, SHA
`0664da56da2abd7426be954fac5ad2dcb22aff75e72371d4047f9a61d7c48862`.

**First-observation diagnostic continuation approved:** the user's “Continue”
authorizes one bounded diagnostic series over the next **eight unique System32
dependencies after protected index92**, in original declaration order:
indices94,15964,16205,16206,16207,16208,16333,16379. Use the already validated
observer, one original strict read per selected file, stopping at the first
refusal or expected-binding mismatch. Do not reread the prior AMD manifest.
Selection is from frozen declaration metadata, not exploratory target reads.
“First observation” means before this continuation's content checks, **not
OS-cache-cold, never accessed by another process, or a fresh process**.
No new process, SDK invocation, target write/override/rebind or acceptance
credit. Reserve4MiB for diagnostic records under unchanged aggregate caps.
If the bounded series does not capture a cause, preserve it as inconclusive;
no repeated warm probes or automatic expansion.102/206 remains unchanged.

**Series result:** all eight distinct files returned through the unchanged
reader; all four snapshots per file retained attributes0x20, both complete
hashes and final native bindings matched their frozen expectations. There were
no observer errors or truncated traces. Exactly8 observations covered7,286,384
distinct content bytes; the original AMD manifest was not reread. Result9,778B
SHA `d48e09e5cf00e7eb20537895e8e613d87264daa9fb1c294bb9bca115a9942098`.
The bounded allowance is consumed. **Inconclusive: no transition or repairable
source defect established.** No code predicate changed, target writes,
acceptance credit or SDK calls;102/206 unchanged.

**Current-readiness amendment approved:** the user approved one complete fresh
strict admission and continuation until the plan is complete. A clean complete
pass may establish current dependency readiness without explaining the earlier
transient; the earlier failed observation remains failed and unexplained.
This changes only the causal prerequisite. All original native/SHA/attribute
comparisons, source identity, deadlines, resource caps and206 SDK ceiling stay
unchanged. Run the complete21,970-row protected declaration in its original
order, with failure snapshots armed before each original strict read; stop at
the first refusal, expected-binding mismatch or resource/deadline breach.
Do not mask, override, rebind, skip or retry a failed row. Success is a fresh
dependency gate, not R01–R03, destructive authorization or scientific acceptance.
This direct in-process admission costs zero SDK calls; subsequent work still
uses only its admitted named allocations, never an implicit budget increase.

**Full admission result:** the actual unchanged `Source6.check → bind →
PR.stable_observation` chain was exercised once per row with a versioned compact
observer. Owned fixtures proved valid return, wrong-SHA refusal, wrong-native
refusal and attribute-transition refusal, including comparator errors after the
reader returns. No source/check replacement or predicate change occurred.
The complete attempt stopped after **9,704 passed /9,705 attempted of21,970**
in **140.7572582s**: index9704 is a missing encoded image from the historical
dataset the user previously reported deleting. The original AMD-manifest row
passed in this attempt; its earlier transient remains unexplained.

Result13,147B SHA
`89c85b560cd1af935d0b957ae715fbced895cab29246d867adc1b128c05ceccc`;
first-negative trace62,440B SHA
`3094f757d330fad05ef51c2be55120410f00e47617009781458209b847f03a1f`.
This is **failed current admission**, not a partial pass promoted to readiness.
No missing-path retry, source reconstruction, row omission or later-row check
followed. Static declaration mapping identifies6,056 bindings /728,460,132
logical bytes under that historical dataset, not6,056 newly observed deletions.

The exact missing image's SHA/native ID is not a member of the3,743-member
current custody archive; targeted retained manifest/archive/index searches did
not establish a payload recovery reference. A query-only `OpenFileById` lookup,
without content access or a directory/recycle-bin scan, passed an exact native
positive control but returned WinError87 for the historical image ID. This
does **not** prove permanent absence; it establishes no usable surviving owner.
An ordinary byte copy would not restore the recorded native identity.
Recovery query1,389B SHA
`eb334d80bcdbdd9b804cf342965851b981aa2edff30462b4c1402b06977c68ba`.

The old93-call route requires the complete inherited protected union before
combine. The image is also an intrinsic original C9 request dependency:
`source_phase_inputs[9504]` and `bindings[9504]` both name its SHA. The original
consumer's aggregate projection visits every leaf; a non-archive-member leaf
must be declared protected and pass decoded/full-native checks. Removing only
global protection therefore produces an undeclared binding, not an admissible
consumer. This is not a claim about raw95's direct SDK input list.
Do not filter the request, drop the row or redirect it to new recordings.
Preserve the old route's failed/non-replayable status and all retained
originals/aliases.

This historical loss must not become an inherited prerequisite for an unrelated
fresh selected-original producer. Replacing the old custody obligation with a
separately defined new-producer route requires an explicit disposition and
new finite invocation allocation; the existing93-call reserve is not silently
repurposed. **102/206 unchanged**, no further SDK or destructive operation.
After publishing the failure/recovery records, the host reported that no
retained eval runtime was registered. That is not evidence that the original
Main process terminated. Its admission publisher was still open at last
observation; no final seal, fixture deletion or reconstructed-handle append is
claimed. Preserve that namespace and its journal as unfinished publication.

| Named allocation | Maximum additional SDK calls |
|---|---:|
| M01: fresh authentic Main admission through the original SDK, plus one repaired rerun | 2 |
| R01: isolated corrected full combine, both child pairs/four unchanged C9 consumers, real success/environment restoration | 1 |
| R02: actual owned-child nonzero exit, native family closure, negative receipt/environment restoration | 1 |
| R03: actual owned-family cancellation, native family closure, negative receipt/environment restoration | 1 |
| D01: one instrumented repetition of the failing R01/R02/R03 scenario, only if offline evidence cannot resolve its cause | 1 |
| T01–T03: one retest per failed R row after one reviewed repair generation | 3 |
| Production combine with both child pairs/four unchanged C9 consumers | 1 |
| Original-member pruning: 29 ×128 members, then31 members | 30 |
| Separate V9 and V8 full fused usechecks; both pass before any release | 2 |
| V9 alias releases: 29 ×128, then31 | 30 |
| V8 alias releases: 29 ×128, then31 | 30 |
| One replacement each for production combine, V9 usecheck and V8 usecheck | 3 |
| **Maximum new calls** | **105** |
| **Previously charged + maximum new calls** | **101 +105 =206** |

Every failed, cancelled or interrupted invocation is charged. The93 production
calls remain reserved; development R01 is separate and earns no implicit
production credit. There are **zero automatic retries for the90 destructive
calls**, zero unlisted recovery/cleanup/smoke calls and no reserve borrowing.
Diagnostic/retest slots cannot fund unrelated scenarios. An unused slot is not
permission to add a test.

Zero-SDK contract checks accompany these calls: authentic SDK95 arguments
outside caller scratch; independent full-binding, argv, closure and stable
caller/workspace identity mutations; immutable SDK100/101 negative readers;
real exact-owned sacrificial cleanup/refusals; environment absent/empty/value
restoration and prelaunch rejection of nested/overlapping scopes. These are
direct existing-reader/native-API checks, not native-child or trainer acceptance.
Historical inputs, readonly controls and failure witnesses must not be mutated.

Source basis is the private frozen `main-native-boundary-v5` runner (one SDK
entry, one root launch), `main-recovery-v1/bootstrap.py` (`main_rehydrate`, then
`main_finite_run`), Source17 caller/workflow/consumer/observer and the
`actual100-charged-explicit193-cap-and93-complete-remaining-route-ledger.json`.
The latter was parsed directly: **93 rows =1 combine +30 prune +2 usecheck
+60 release**. Its literal101–193 schedule predates failed SDK101 and is not
the next schedule. The historical fresh-Main smoke used the bound interpreter
with `-B -c pass`, one SDK invocation; bootstrap itself does not invoke the SDK.
No historical successful smoke is a current admission.

The current evaluation process is not historical Main22496 and has no retained
H/S6/PR globals. Fresh bootstrap must establish its own true native identity,
Publisher and owned Job; it cannot recreate old handles or run frozen Source17
as that Main. A new immutable successor must incorporate actual SDK101 and the
approved ceiling while preserving historical validators and SDK99's unresolved
status. M01 does not settle lost families or authorize their namespace retirement.

Before launch, seal the actual wrapper, genuine failure/cancel fixtures,
commands, input closures, expected outcomes and per-row resources in the local
manifest. R02/R03 have no existing fault-injection CLI option: use a separately
bound contract fixture through the real SDK/owned-family path, not invented
options or mocked native receipts. This tests orchestration, not trainer
integration. Keep Main TEMP/TMP scoped until SDK and observer joins complete;
verify exact restoration before another scope.

Replacement calls require a normally closed family negative, attributable
repair, new immutable admission and fresh controls. Stop on any partial
destructive chunk/publication ambiguity: the current prune/release code deletes
members individually before publishing the chunk receipt and is not a blind
retry transaction. Also stop on unclosed family, identity/input drift, unpriced
call, exhausted row allowance or any unchanged deadline/resource/publication
limit. Remaining budget cannot cure these conditions. Exhaustion is **BLOCKED,
NOT SETTLED**, not evidence that a scientific family is exhausted.

**Full-campaign pricing remains unresolved:** selected originals still lack
decoded track/calibration/projection/configuration and original-derived corpus
admission. New-source generation, integration, references, coverage and
candidate-specific recovery need exact commands and invocation accounting.
A5's initial three native plus three candidate runs and conditional extra
matched batch are scientific run counts, not SDK-call counts. A4 remains
evidence-led; do not silently cap candidate families to make206 a purported
end-to-end budget. Propose the next finite scientific tranche after the required
admission, retaining every A0–A6 acceptance criterion.

### Authorized replacement execution — 2026-10-03

**Historical earlier replacement dataset only.** The executed evidence,
accepted Source v5/readback, fixed view counts and then-pending next-action
snapshots in this section are preserved unchanged. They are not current raw
dataset admission, source/reference acceptance or permission to run them again.
Prospective work follows the
[2026-10-07 selection](#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
and [new-dataset execution order](#new-dataset-execution-order).

- Final preparation and the native CPU parser passed. The selected reconstruction
  has **2,986 views**, with **2,688 training / 298 held out**, **508,234 points**
  and one post-camera per input. All **6,056 encoded input/mask/reconstruction
  files** are freshly bound; every selected PNG mask header matches its native
  image dimensions. This is input readiness, not a trained source or full A0 exit.
- Native configuration serialization and the six-metric envelope boundary
  selfcheck passed. Native metadata v3 also passed the actual post-load
  config/dataset/runtime CPU gate **before engine creation or GPU work**:
  **2,688 training appearance rows (J) / 2,986 global views / 298 held out /
  508,234 points / K1**, cap **1M**, horizon **30,000**, planned source step
  **10,000** unchanged. Root exit 0, resource acceptance and Job active 0
  establish this CPU gate, not a trained checkpoint or fresh native quality.
- The private `cl`+link route actually passed: ordinary object compilation
  exited 0; the earlier guard failure remains preserved. The repaired actual
  wait-signaling route captured the **88,095,744-byte** source executable.
  Public C++ source, object/library inputs and configuration are unchanged.
- The historical redundant packet was independently restored, completely hashed
  and consumed by its unchanged five-field CPU parser before exact native-handle
  retirement: **299,678,937 logical bytes** removed. The archive and historical
  scientific proofs remain unchanged. A second restore/consume/release cycle
  passed with the old physical packet absent; every contained Job ended active 0.
  No physical reclaim or new-dataset numerical qualification is inferred.
- Producer v8's full isolated restoration passed for all **3,710 members /
  417,274,876 bytes**, followed by **four actual CPU consumer exits 0** and
  accepted full exact-owned pruning; root exit 0, resources accepted,
  **Job 5 / active 0**. Its recovery actually removed the prior owned partial
  workspace after the preserved v6 prune-observer negative. Executed failures,
  scientific witnesses and the **124,915,844-byte** archive remain retained.
  This earlier CPU proof does not cover new Source v5 products or nine new
  runtime DLLs. The separate delta v1 archive and isolated restore exited **0**,
  exact **25 members / 212,683,845 bytes**, ZIP **64,833,744 bytes**. Its restored
  CPU metadata native exit was **0**, but guardian **1**: **16 actual runtime
  modules** included `apphelp` outside the bound closure. The explicit v2 route
  retry now passed **native/guardian 0 / Job 1 / active 0**, parent **52.1171217 s**,
  with **15 actually loaded modules**, all bound; the optional DLL need not load.
  V3 scoped cleanup passed **0 / Job 1 / active 0 / 55.5531085 s**: the consumed
  restore tree and accepted temporary outputs were retired after bound retention.
  The three original failed metadata JSONs (**1,835,438 bytes**) and their native
  ancestors remain for active PE/future capture consumers. Original source
  products, archive and scientific witnesses stay unchanged. Earlier metadata
  v3 pinned/admitted `apphelp` but did not load it; no retrospective observed-load
  claim or change to Producer v8's positive is made. This is CPU custody, not A1.
- Source v4 actually ran ordinary native **0 → 10,000** under horizon
  **30,000**, native root exit 0. Full resume v2 at step **10,000** passed
  all saved-slot/native-clock checks: current/max rows **1M**, clock count
  **1M**, minimum **99**, maximum **10,000**. Its checkpoint TAR is
  **464,397,824 bytes** and filtered PLY **236,925,355 bytes / 955,338 vertices**;
  the filtered PLY count does **not** establish the inactive-row count.
- The source v4 guardian failed **final runtime custody only**: **15** startup
  modules versus **47** post-engine modules, with **20 unprepared Windows/AMD
  dynamic DLLs**. Job **1 / active 0**; completed parent wall **456.8927 s**
  includes admission and is not a native update-cost benchmark. Protect the
  full first-failure checkpoint and all witnesses; native exit/readback success
  does not qualify the source, full A0 or A1, or repair historical Q.
- Parent v5 failed before `Popen` or GPU spawn: directory attributes changed
  from Python **0x10000010** to native/post **0x10**, while full 64-bit volume
  and 128-bit file identity stayed equal. Preserve that negative separately.
  Parent v6 uses directory identity/kind/non-reparse semantics; its actual owned
  CPU boundary selfcheck exited **0 / 0.19 s**, with exact-owned cleanup.
- Under that versioned parent, actual cold **Source v5 and fresh Vulkan
  readback v5 both passed**, native exits **0**, each **Job 1 / active 0**.
  Source runtime **15 startup / 47 post-engine** modules and readback
  **15 / 46** were all bound. Original source executable, source C++ and public
  C++ remain unchanged; Source v4's first complete failure is not relabeled.
  Source v5 ran **0 → 10,000**, horizon **30,000**, current/capacity **1M**,
  clock count **1M**, min **99**, max **10,000**, FNV **1666320210201407970**.
  Native TAR **464,397,824 bytes**, SHA-256
  `932ae4ce3d4a1d5f0a7b010f1b0bb4e78d693257c9662afbe95a41477616f35e`;
  filtered PLY **236,634,699 bytes** is not an inactive-row counter.
  The unfiltered native mean scan found **972,211 finite / 27,789 nonfinite**
  rows; all **1M stable IDs** remain in the source/optimizer domain.
  **3,907 codec blocks / 64 tail lanes** are derived from this source, not
  imported with the kitchen's historical **7,150** inactive rows.
- Actual parent phase walls were source **452.0025814 s** / readback
  **74.9158446 s**, including admission/startup/after checks; complete-command
  walls **479.40 / 101.65 s** also include separate prehash. Keep source
  generation separate from continuation cost: none is a native update benchmark.
  Committed backing peaks were **921,877,168 / 601,928,448 bytes**, host-visible
  **68,157,696 / 67,108,864 bytes**, allocation failures **0**; true physical GPU
  or host peaks are not established. Post-source ROOT/free
  **9,383,473,378 / 26,614,841,344 bytes**, post-readback
  **9,385,453,434 / 26,574,012,416 bytes** are snapshots, not next-phase admission.
  Full saved-slot canonical/startup, owner masks and coverage remain pending.
  The pre-run cap-1M checkpoint bound was **712,434,584 bytes** for **2,688**
  appearance rows; with **32 MiB** evidence, reserved growth was **745,989,016
  bytes**, not the observed checkpoint size.
- The post-CPU ledger was blocked under the then-authoritative **8 GiB**
  shared retained-output limit: ROOT **7,966,803,377 bytes** + source/evidence
  **745,989,016 bytes** + **1 MiB** receipt allowance projects
  **8,713,840,969 bytes**, exceeding **8,589,934,592** by **123,906,377 bytes**.
  The user then explicitly selected **“Raise to 16 GiB” for shared retained
  output only**. That old refusal remains valid historical evidence.
  Source v4 and accepted Source/readback v5 used the authorized **16 GiB**
  shared limit; every subsequent phase still requires fresh admission.
  The increase is not source qualification or guaranteed campaign completion.

Current private records remain under the existing campaign in continuation ROOT:
`A0-dataset-readiness.slice.json`, `A0-native-source-metadata-v3.parent.json`,
`A0-producer-v8-source-only-recovery.parent.json`,
`A0-producer-v8-source-only-proof.parent.json`,
`A0-native-source-post-CPU-resource-blocked.json`,
`A0-user-retained-output-cap-16GiB.authorization.json`,
`A0-native-source-v4.completed.parent.json`,
`A0-native-source-v4.runtime-negative.json`,
`A0-native-source-v4.first-complete-failure.protected.json`,
`A0-native-source-v5-parent-attribute-negative.json`,
`A0-native-source-v5.accepted-slice.json`, `A0-native-source-v5.rows.json`
and the `A0-packet-after-retire-*` parent records. Executed negative attempts remain
negative; neither this restart nor byte custody repairs historical Q.

Private reference preparation now separates genuine ordinary native async
seed 0 (no data-manager replacement), a causal async B1-matched arm using the
native `0x423153414d504c31` sampler constant and independent training RNG
(initial 0), and a fresh-restore sync seed-42 diagnostic without independent
RNG. Seed 42 alone was not B1 matching; no historical host-RNG equivalence,
slower-control substitution or automatic reference win is claimed. Direct
native evaluation uses an ordering-only scoped flag to bind all 298 names/
global IDs, not CLI PLY evaluation, B1 ownership or an optimizer change.
These preparations are unexecuted; all six named metrics and the existing
least-favorable native envelope remain required unchanged.

Capture's PE/API-set closure is now accepted at v15. Next actual work is owned
safety gates, fresh GPU admission, source-canonical and
native no-forward precursors, both installed owner masks and complete **2,688
training-view coverage on each AMD**, then fresh native step-20,000 reference/
six-metric repeats and candidate A2–A6 qualification. Actual capture/reference
v1 compilation failed: compiler exit **2**, auxiliary compiler process retired
with exit **43**, **Job 2 / active 0**, guardian accepted **false / return 1**,
parent wall **28.4997631 s**. The generic capture helper incorrectly used
`.size()` for multidimensional Tensor2D/5D; those require `.numel()`, unlike
DeviceVector's `.size()` (null quantized-float holders have size 0).
Executed v1 C++/helpers/logs remain immutable negative evidence. Corrected
capture/reference v2 **actual compile and link passed in separate owned CPU
Jobs**, each **Job 2 / active 0**: compiler/linker exits **0**, auxiliary compiler
processes retired **43**. Parent walls **74.0263452 / 70.8323038 s** are build
phase costs, not training timings. Executable **88,220,672 bytes**, SHA-256
`8a484621673540ef75a2cb1ad542f3c149901182d995cb21370ebc44fefc74f1`;
`A1-capture-native-build-v2.parent.json` binds the complete wrapper proof.
Frozen v2 includes the vector/Tensor root fix, separate ordinary async and
native-constant causal arms, independently admitted sync-42 source-pair/CPU
checks, a fresh live-endpoint **32 MiB** witness reread and the existing
`M0_EXPOSURES` real training-step logger. Those GPU/scientific phases are
unrun. PE guard v2 actually failed while pinning writer logs that remained open,
root **1 / Job 3 / active 0**, with only **two observed family processes**.
PE guard v3 then hit its debug deadline before the first creation event due to
suspended-root resume ordering: root **1**, dumpbin **43**, both processes
signaled, **Job 2 / active 0**. V4's repaired debugger actually captured three
frontend/linker families, all exit **0**, before the whole audit failed on
flattened DLL-candidate ambiguity: root **1 / Job 7 / active 0**. V5 corrected
ordered search and captured nine families, all exit **0**, but rejected an
absent **delay-loaded Kernel32 API-set contract**: root **1 / Job 19 / active 0**.
V6 advanced through **84 native tool exits 0**, then rejected a delay-contract
name ending `l1-1-0` against the native schema's unique empty-host entry ending
`l1-1-1`, with the same recorded hashed family. Guardian/root **1**,
**Job 85 / active 0**, all retained processes signaled, parent **102.6643618 s**.
All negatives remain immutable; none accepts PE closure. Microsoft's
[API-set contract](https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-apisets)
and [native availability query](https://learn.microsoft.com/en-us/windows/win32/api/apiquery2/nf-apiquery2-isapisetimplemented)
distinguish unavailable delayed calls from required startup imports. V7's native
hash-family successor then reached **186 native tool exits 0**, but rejected
`shell32.dll`'s absent concrete delay-loaded library: guardian/root **1**,
**Job 187 / active 0**, all signaled, parent **138.9326763 s**. The actual
delay descriptor and eight-policy absence witness remain immutable.
[Microsoft's delay-load contract](https://learn.microsoft.com/en-us/cpp/build/reference/linker-support-for-delay-loaded-dlls?view=msvc-170)
loads a concrete library only when called; treating every unused delayed edge
as a startup prerequisite was an audit-model error. V8's generic absence proof
then rejected a real PATH directory junction: guardian/root **1**, **Job 187 /
active 0**, all signaled, parent **140.3592895 s**. Separate read-only lexical
metadata identified the junction; no successful absence proof is inferred.
V9's native lexical junction/target-chain and resolved-directory proof advanced
the read-only DLL search, reaching **726 native tool exits 0**. The shared PE
parser then incorrectly rejected the standard `WINSPOOL.DRV` import using a
`.dll`-only name grammar: guardian/root **1 / Job 727 / active 0**, all
signaled, parent **348.3974341 s**. Keep that negative. V10 introduced a generic
safe basename and explicit native-image/non-image roles, but a role-name set
overwrote the debugger's process-image allowlist before its first child ACK:
guardian/root **1 / Job 2 / active 0 / 78.6330094 s**, only **one observed
family process**. That native family is incomplete, not admitted. V11's
allowlist fix reached **804 observed tool exits 0**, then the **112 MiB phase
output bound** stopped root **43 / guardian 1 / Job 806 / active 0**, with
**805 observed processes**, parent **405.6491822 s**; one family member remains
unobserved. Its 134 policy files contain **103,453,297 bytes**; same-information
compact JSON still needs **77,469,946 bytes**. V12's bounded, full-witness gzip
successor then exhausted its **8 MiB publication-journal sub-budget**; both
root output and final negative receipt publication failed. Guardian **1**,
parent **944.9567497 s**, journal **8,387,819 bytes**, only an empty pending
receipt: final root exit/zero-active/full-family proof is **not established**.
Unexecuted v13 re-prices that internal journal allocation to **32 MiB**, with
an exact guardian-receipt reserve inside unchanged **112 MiB phase / 128 MiB
growth / 16 GiB shared ROOT** hard bounds. This journal is not the **8 MiB
TEMP** budget. All full witness fields, bounded independent decoding, typed
descriptor custody and native-image-only loaded credit remain required.

V13 actually published its reserved negative receipt: guardian **1**, root
**43**, **1139.3647814 s**, **Job 1814 / active 0 / known 1813**, incomplete
family. Its last captured phase size was **37,408,573 bytes**, not a measured
112 MiB exhaustion. A scoped inventory reader still imposed 8 MiB on the
32 MiB journal; that code defect is established, its exact triggering callsite
is `[INFERENCE]` because the old negative lacks a traceback. V14 aligned the
reader and reached aggregate publication: guardian/root **1**, parent
**1727.2707945 s**, **Job 2245 / active 0 / known 2245**, complete family and
retained signaled exits. The root traceback establishes that publishing
`inspection.json` exceeded the **16 MiB per-publication bound**. Its exact
decoded byte count was not retained. Preserve both negatives; complete-family
retirement is not PE acceptance. V15's full-schema bounded gzip aggregate
actually passed **guardian/root 0 / 2008.0797955 s / Job 2245 / active 0 /
known 2245**, complete family and all retained signaled exits. It inspected
**374 images / 14,580 resolutions / 84 conditional delays**. The full inspection
is **856,550 encoded bytes**; independent pure in-memory codec assertions also
passed, not a native scientific boundary. `A1-capture-native-pe-v15.accepted.parent.json`
binds this accepted PE slice. Source capture/precursor/CPU proof and ordinary
quality references remain required; user caps and numerical behavior are unchanged.
No DLL-specific exception, dropped edge, fake file, PATH change or general
reparse waiver is allowed. Normal missing imports, ambiguous existing hits,
native call failures and unknown loaded modules retain full fatal custody gates.
Next are owned native-cap/STOP safety gates, fresh GPU admission, source capture,
second precursor/CPU checks and ordinary native step-20,000 references. Existing
**128 MiB compiler-product** object/executable/PDB budget applies only to the
current namespace family, not campaign-global storage. Raw Source v5
executable/object/science remain retained; no retirement need or cap change
follows. Shared ROOT **16 GiB** still counts all history,
with all other caps unchanged. New A2 coverage remains private/
unapplied. Source v5/readback acceptance is
unchanged; no source-canonical, coverage, A1 quality, A2, candidate timing or
long-recovery acceptance is implied.

The ownership v1 preparation actually exited **0 / 2.0404064 s**. Its actual
compiler exited **0** and auxiliary compiler process **43**, **Job 2 / active
0**; the parent boundary nevertheless failed **1 / 99.8499332 s** after a
ROOT inventory observation changed during the before/native/after file check.
The affected pathname was not recorded; no scientific failure or particular
writer is inferred. The later guardian wait also failed: static inspection
shows its resource-query handle lacks `SYNCHRONIZE`. Preserve the unadmitted
object, executed helpers and complete negative. New unexecuted ownership/
capture parents require a bounded exact-owned inventory-race retry and the
retained native `Popen` wait handle, without changing immutable Source v6,
caps or scientific acceptance. `A2-ownership-native-compile-v1.negative.parent.json`
binds this distinct compiler-success/custody-failure outcome.

Ownership v2 preparation passed **0 / 2.3387209 s**; its actual native compiler
and guardian passed **0 / Job 2 / active 0**, including true synchronized
native exit witnesses. The whole parent still failed **1 / 101.5882313 s**:
its required-live coordinator handle map also included the normally retired
guardian. Static inspection and the retained full PID/creation/exit-0 witness
establish that role error, not the exact dynamic failing callsite. Preserve
the accepted native compiler receipt/object and separate parent negative;
`A2-ownership-native-compile-v2.negative.parent.json` binds both. Unexecuted
ownership v3/capture parents separate mandatory-live coordinator checks from
guardian RSS, permitting explicit non-live zero accounting only after strict
retained-handle completion proof. No source/physics/cap change is implied.

Ownership v3 preparation passed **0 / 2.7766686 s**; fresh compile and link
each passed both native guardian and whole parent **0 / Job 2 / active 0**,
parent **104.9300733 / 103.9815239 s**. The live-role correction is now exercised,
not merely reviewed. `A2-ownership-native-build-v3.accepted.parent.json` binds
literal native products, compiler/linker receipts and both parent completions.
Ownership execution v4 retains that physical build ancestry without recompiling
or adopting v2's parent negative. Its actual executable-specific PE closure,
ordinary source CPU proof and both AMD coverage lanes remain required.

Ownership execution v4's CPU PE handoff passed **0 / 102.1810726 s**.
Its actual inspection parent failed **1 / 295.8403709 s** on a real owned
process-JSON pending-to-final alias outside the frozen refresh/TEMP retry
policy. It published a bound STOP and waited the retained guardian's true
exit **1**; native root **43 / Job 154 / active 0 / known 153**, incomplete
family, all retained signaled. Preserve this separate negative in
`A2-ownership-native-pe-v4.negative.parent.json`. Parent v5 recognizes only exact
current-PE publication aliases with full native ownership and closed destination
proof, not arbitrary pending files.

Ownership v5's CPU handoff passed **0 / 103.0590039 s**. Its native PE producer
passed **guardian/root 0 / Job 2245 / active 0 / known 2245**, complete family
and all **2,245** individual synchronized exit-0 witnesses. The whole parent
still failed **1 / 2467.0930672 s**: its recursive file check compared the
producer's pre-receipt publication-journal SHA with the journal after the
guardian appended its two reserved terminal publication events. The guardian's
retained native handle proved exact PID/creation, wait 0 and exit 0; retirement
custody is safe, but the parent negative stays negative.
`A2-ownership-native-pe-v5.negative.parent.json` binds both outcomes. A distinct
CPU consumer must prove the immutable journal prefix and exactly the owned
terminal suffix, publish truthful current-file bindings and retain a separate
historical-prefix proof. No PE rerun or in-memory receipt rewrite is required.

The distinct ownership consumer v6 now actually passed **native/root 0 /
116.9289126 s / Job 1 / active 0**. Main queried the same native Job's hard
**8 GiB / flags 0x2200**, assigned the suspended exact child before resume,
sampled full ROOT/native-allocation and scope-3 prices, and proved original
retained-handle PID/creation/wait-0/exit-0 retirement. The sole shared validator
consumed the immutable producer receipt and exact prefix/terminal suffix;
truthful current bindings and the separate prefix proof were published.
Its new Main completion is **29,146 bytes**, SHA-256
`4309cf6bf12018089666425506486b906074493c90d1b9456fd4d9c0f96b3b5d`.
Parent v5 remains negative; this is post-custody CPU acceptance, not GPU,
ordinary source, coverage, A1 or A2 qualification.

Capture's actual CPU safety fixture v1 failed at its bootstrap comparison
before the child; v2 passed that bootstrap but failed while pricing an absent
PDB before native-cap execution. Neither proves cap refusal or STOP. The accepted
build's actual mandatory products govern the unchanged **128 MiB** product
budget; a symbols file not produced or declared is not a mandatory product.
The two failed parent records remain immutable. Fresh fixture v3 actually passed
both owned CPU parents: **8.2207738 s** native-cap / **12.2463277 s** STOP,
each **Job 1 / active 0**, with true retained full-ID signaled exits. The
test-only **128 MiB** Job refused native **256 MiB VirtualAlloc**, address 0 /
Win32 error **1455**, child exit **42**. STOP followed a real **16 MiB** committed
READY allocation and an exact bound parent/supervisor/guardian intent; child
exit **43**. These safety proofs do not change the production 8 GiB Job cap.

Actual source-parent v3 then failed **1 / 3.7536229 s before guardian/GPU launch**:
the frozen PE validator's conditional-edge command/stdout/exit comparison
asserted during `parent_document`. The root cause is established: producer
`Path.read_text()` uses universal newlines, but the consumer decoded raw UTF-8
without CRLF/CR conversion. All **84** retained conditional commands/exits
match; their stdout equals the exact universal-newline producer view.
The versioned shared consumer v16 preserves raw bytes/SHA/native bindings and
uses that same strict UTF-8 text view. Its pure mixed-newline/Unicode/invalid-UTF-8
check and actual full-pinned **84-edge** text projection passed, the latter
**0.8039536 s**. This is not full descriptor/source acceptance. Executed Parent
v3/PE v15 and the separate negative remain unchanged; fresh parent/guardian
consumer dispatch, source capture, precursor and CPU proof remain required.

The fresh parent/guardian v4 consumer cutover was actually exercised. Its owned
CPU fixtures passed **8.2701674 s** native-cap / **12.2462797 s** STOP, each
**Job 1 / active 0**, signaled child exits **42 / 43**. Source capture v4 then
failed **1 / 71.1153736 s before guardian/GPU launch**: `parent_document`
completed, but `prehash` rejected a **conflicting duplicate binding**. The cause
is established: Windows case-equivalent `oleaut32.dll`/`OLEAUT32.dll` and
`comdlg32.dll`/`COMDLG32.dll` records have identical full native identity,
size/SHA and all non-path fields; whole-dictionary equality still compared
their literal path spelling. Fresh Parent v5 checks every literal record,
compares all non-path fields on a normalized-key collision and retains the first
checked spelling. Its actual native two-pair regression passed **20 cases /
0.3599058 s**, including every native-field, size and SHA conflict rejection.
Executed v4 and its full negative remain unchanged.

Source-parent v5 then failed **1 / 112.7058969 s before guardian/GPU launch**:
the complete parent-binding metadata declaration exceeded the frozen publisher's
**1 MiB receipt bound**. A separate actual, read-only declaration reconstruction
measured **2,735,118 bytes** pretty / **2,167,579 bytes** compact JSON;
**3,763** full PE phase-input bindings dominate it. That diagnostic ran no phase.
The correction must publish the full metadata under the existing bounded JSON
consumer contract and charge its actual bytes plus publication growth before
writes; terminal receipt limits and ROOT/free/host/Job caps must not change.
Preserve executed v5 and its negative. Source capture, precursor/CPU proof,
both AMD ownership lanes and A1–A6 remain incomplete.

The shared metadata-role helper v2 passed an actual Main in-process native
publication smoke **4.6787506 s**: the complete **2,167,579-byte** declaration
was published and independently read back with every JSON field unchanged.
Before final-path visibility, the required callback observed the closed,
fsynced, fully bound owned scratch and ran a fresh whole-ROOT/native-allocation,
host, free-space and commit gate. Atomic rename follows that gate and freshness
checks, so a waiting GPU guardian cannot launch before admission. Post-write
pricing also passed. This is **metadata I/O only**, not an owned-Job CPU
regression, source capture or GPU/scientific acceptance. Source and ownership
callers share the helper; ordinary receipt/journal limits remain **1 MiB**.

The five-case native metadata CPU regression passed **15.7883594 s** under a
fresh exact-owned **8 GiB** Job. Corrected Main boundary v3 accepted
**18.9345666 s**, **Job 2 / known 2 / active 0**, with full native identity,
same-Job membership, signaled retained handles and true exit **0** for both
members and the original root handle. The observed second image in that fresh
attempt is `conhost.exe`; no one-process assumption is valid. Its preceding
boundary v2 remains **negative / 17.2400115 s** despite five passing child cases:
the hardcoded count rejected a two-member Job before complete final witnesses.
The first attempt's second image is not retroactively inferred. Actual cases
cover complete large-declaration publication, pre-visibility callback refusal,
stale originals, exact **16 MiB** acceptance / one-byte-over refusal and unchanged
**1 MiB** ordinary receipt refusal. This clears the metadata CPU prerequisite,
not source capture, AMD coverage or A1–A6 qualification.

Parent6 then actually passed native source capture and the no-forward source
precursor on the supported Vulkan lane: complete Main walls **231.2164217 s /
225.4803378 s**, each **exit 0 / Job 4 / known 4 / active 0**, all retained
native member exits **0**. The literal Execution4 science outputs remain bound.
Independent CPU proof subsequently failed **exit 1 / Main 173.4065021 s**,
with the four-member family retired. The frozen canonical consumer asserted
`state.camera.num == dataset.post_cameras == 2688`; actual native metadata has
`camera.num = 1` and `dataset.post_cameras = 2688` (global **2,986**).
The native trace establishes the consumer domain error: `EngineCheckpoint.cpp`
writes the current `CameraTable`, whose `num` is `viewmats.shape[0]` in
`EngineSetup.cpp`; `TrainerCore.cpp` allocates appearance tables from
`post.n_post`, separately. Bound `max_batch_per_epoch = 10000`, train **2,688**
and all **K = 1** derive active batch **1**. Actual persistent PPISP
parameters/accumulator are each **96,768 bytes = 2,688 × 9 × 4**; the
quantized RGB-grid fields remain full-table shapes, not one-camera shapes.
Preserve accepted capture/precursor and the CPU negative; repair a versioned
CPU consumer and every downstream
prerequisite without rerunning or relabeling the accepted GPU science.
Source canonical/ownership qualification and A1–A6 remain incomplete.

The corrected camera/table API passed all **19 real-metadata CPU cases**.
An initial Main boundary stayed negative **2.0662277 s**: **Job 2 / active 0 /
retained 1** missed an ephemeral member. Fresh detached-root execution then
passed **2.1855746 s / Job 1 / retained 1 / active 0 / native exit 0**.
The camera consumer and both receipts remain immutable. Parent7/Execution5
independent CPU proof next failed **163.3007071 s** before full source proof:
its generic scientific JSON decoder rejected native effective configuration's
`Infinity` values (`outlier_threshold`, `max_world_size`). Main retired the
four-member family but retained only three complete direct handle witnesses;
no missing witness or acceptance is inferred. The successor must use the
native configuration domain's decoder without admitting nonfinite scientific
metadata, exercise the **full real canonical API**, and migrate all active
prerequisites to its genuine accepted CPU proof. Accepted Source4 GPU science
is still reused byte-exact; A0 source qualification and A1–A6 remain open.

The native-config successor's **full actual startup-pair API** and **35**
camera/config/scientific-JSON boundary cases passed **11.5727430 s** under
Main's queried **8 GiB** native Job: **total 1 / retained 1 / active 0 /
native exit 0**. Consumer4 proves all **43** persisted fields and independent
startup, complete original **1,000,000-row** storage/clocks and full POST
appearance shapes; it does not claim unsaved historical host RNG recovery.
Configuration decoding allows only schema-declared flat positive-infinity
upper limits; scientific JSON remains strict. This clears the real API
prerequisite, not the separately versioned CPU6 guardian route, ownership
coverage or A1–A6 qualification. Close dependent controls only after this
observed result; preserve all executed negatives and accepted Source4 science.

Parent8/Execution6 CPU6 now actually passed **173.6573679 s / Main hard
8 GiB Job / total 4 / retained 4 / active 0**, every original full-ID handle
signaled with true native exit **0**. Consumer4 V3 and exact Science4 ancestry
are genuinely qualified. Own10 parent binding passed **191.2804373 s /
Job 1 / retained 1 / active 0 / exit 0**. Its first AMD precursor then failed
**337.4577773 s / Job 3 / retained 3 / active 0** before native launch:
guardian snapshot referenced undefined `products`; native command list is
empty. Preserve this prelaunch negative and executed Own10/Guard7 controls.
Repair the shared product-family snapshot in a versioned successor and
exercise its real snapshot caller before retry; no GPU numerical failure,
ownership coverage or A1–A6 acceptance follows from this error.

Own11/Execution8 shared compiler-product snapshot now actually passed its
CPU-first real-caller check **5.5676387 s / Job 1 / retained 1 / active 0 /
native exit 0**: four real observations, **90,194,604 product bytes**, seven
full native identity/metadata mutation refusals. These observations are
diagnostic-only, not native phase or coverage acceptance. Fresh Own11
full parent binding passed **193.4548999 s / Job 1 / retained 1 / active 0 /
native exit 0**. Own11/Guard8 controls are now executed immutable; continue
their fresh supported-AMD precursor/proof/probe gates before A1 references.

The fresh Own11 first AMD precursor remains **negative, 333.7401705 s /
Main Job 4 / retained 4 / active 0**. Native child truly exited **0**, but
strict guardian consumption failed: `coverage.jsonl` held two pretty-printed
multiline state objects rather than JSONL. This is a producer framing defect,
not a numerical GPU failure or coverage qualification. Preserve executed
Build3/Own11/Guard8 and the full negative witness. Build4 changes only a shared
private state/view record emitter; the public writer, math and source custody
stay unchanged. Actual Build4 preparation passed **4.5608085 s / Job 1 / active 0**.
Native compilation and linking each exited **0**, producing executable
**87,688,704 bytes**, SHA-256
`29d93e433dbf4b3a8ab5c2ec7e9a5bc69d90f5f2031df4332d8854e41b3b7fd2`.
Both original Main V5 completions remain **negative**: compile **112.5877294 s**
rejected the explicitly retired compiler auxiliary's exit **43**; link
**82.1194916 s** failed inner auxiliary retention. Each outer Main boundary
retained **all five actual processes / active 0**, including the linker **0**
and auxiliary **43**. The link inner Job reported two processes but retained
only one; do not infer complete inner-family acceptance. Fresh typed physical
compiler-payload CPU qualification passed **19.0567485 s compile /
20.6239052 s link**, each **Main hard-8-GiB Job 1 / retained 1 / active 0 /
native exit 0**, with **14 actual-evidence mutation refusals per phase**.
It preserves original negative statuses and missing inner auxiliary; no
compiler or linker was replayed and no positive old receipt was invented.
Actual private JSONL executable smoke passed **2.2744636 s / Job 1 /
retained 1 / active 0 / native exit 0**: exact state/view physical records,
native parsing, nested/escaped/empty siblings, concatenated-document rejection
and oversize rejection before publication. Its diagnostic compiler control
remains unqualified: v1 aborted on a zero-growth callback mismatch; v2 failed
whole-family retention and then negative image-query publication. Raw witnesses
and products remain retained; the independent runtime proof does not repair
those compiler negatives or identify the missing process.
Own12's current product-family snapshot CPU passed **5.5330812 s / Main
hard-8-GiB Job 1 / retained 1 / active 0 / native exit 0**, with four actual
snapshot boundaries and seven native-field mutation refusals.
Fresh executable-specific PE6 preparation passed **78.9298319 s / Job 1 /
retained 1 / active 0 / native exit 0**. Its genuine direct-parent inspection
then failed **287.1701643 s / Main Job 135 / retained 135 / active 0**:
the parent resource observer rejected an owned output packet's pending path
during atomic publication. Actual guardian/root wrapper exits were **1**;
the stopped inner root, console and dumpbin exited **43**; all remaining
**130** retained processes exited **0**. The explicit nested Job closed
**134 / active 0**. Original-handle guardian identity and all terminal
witnesses remain retained; this is not PE, GPU or scientific acceptance.
The publisher's durable scratch and closed events corroborate the actual
same-native-ID rename. A separate controller argv serialization refusal
occurred before any native child or Job creation and remains distinct.
The shared exact-owned observer/current ownership and reference caller cutover
passed independent static review, but its first actual controlled publisher
fixture remains **negative, 9.3175114 s / Main Job 1 / retained 1 / active 0 /
native exit 1**. The fixture failed before controlled rename. Static inspection
found its reduced `os` facade omitted a publisher method; the readiness wait
did not preserve the initiating producer exception. The zero-byte owned packet, journal,
source and whole-family negative remain immutable. Fixture2 then failed
**4.6924892 s / Main Job 1 / retained 1 / active 0 / native exit 1** on
an invalid single-callback assumption. Distinct fixture3 passed
**12.8277380 s / Main Job 1 / retained 1 / active 0 / native exit 0**:
actual unresolved rename, exactly one producer release across repeated resource
callbacks, nine refusals, retired producer and both durable publication quotes.
Current ownership/reference metadata cutover passed **124.2769624 s**, preserving
literal original CPU6 and compiler origins. Own13's four actual resource
snapshots/seven native-field refusals passed **40.2247731 s**; each closed
**Main Job 1 / retained 1 / active 0 / native exit 0**.
Fresh PE7 preparation passed **79.5899118 s / Job 1 / retained 1 / active 0 /
native exit 0** after a separate **3.1528885 s** wrong-parent input refusal
before namespace creation. Its genuine inspection is **interrupted, not
qualified**: the host's 1,260-second deadline killed the original Main kernel
before terminal receipts or original-handle witnesses were retained. Main passed
an absolute monotonic deadline to a boundary expecting relative elapsed seconds.
The new observer found the original Main and four recorded process IDs
unavailable to `OpenProcess` (error 87); this is not an all-zero family proof.
Preserve the incomplete journals and outputs. Next: re-establish genuine native
supervision with correct finite timeout semantics, then a distinct inspection
and post-custody consumer. No cap increase, arbitrary missing-path waiver,
compiler replay or accepted source GPU replay is authorized by these negatives.

The restored same-Main finite boundary actually passed a **2.188390 s**
positive and retained a **60.504318 s** deadline negative, each **Job 1 /
retained 1 / active 0**. The negative's original root exited **43**; its
native terminal accounting reports terminated **0**, not an inferred kill
count. Relative integer watch seconds are enforced before namespace/child
creation. Neither proof repairs PE7's missing original terminal witnesses.

The new A1 endpoint consumer was compiled and linked exactly once from its
sealed current source/runtime inputs. Both original compiler roots exited **0**,
each **Job 2 / retained 2 / active 0**; Main walls were **74.885014 /
73.120506 s**. Owned VCTIP auxiliaries exited **43**, so both whole-family
receipts remain **negative**. Separate physical-product qualification passed
**12 actual-evidence mutation refusals per phase**, retaining direct-parent,
full creation-time, original-handle, stream and fresh publication-price proofs.
The new executable is **87,687,168 bytes**, SHA-256
`147a470f537604201c6154166598aff2352635e7ff52794bb89ecfb7093dc7ce`.
Its original **9,971 input rows** are preserved through a byte/native-exact
metadata projection; redundant outer identity fields were checked before
projection, not rewritten in the original inventory. No old compiler replay,
new endpoint runtime, training/evaluation, reference retention or A1 quality
acceptance follows. Fresh Own14/Exec11/PE8 controls have two static clearances.
Their actual metadata builder v2 stopped before initialization: generic binding
traversal mistook a bound interface's `binding_shape` documentation for a live
file descriptor (**12.954207 s / Main Job 1 / retained 1 / active 0 / root 1**).
The separate builder v3 validates and excludes only that exact schema field;
actual current metadata publication passed **85.078273 s / Job 1 / active 0 /
root 0**. Original declarations and the negative are unchanged.
Own14's actual shared observer passed **60.654903 s / Job 1 / active 0**,
four real resource snapshots and seven native-field mutation refusals.
Fresh PE8 preparation passed **157.728103 s / Job 1 / active 0 / root 0**,
using the existing Build4 executable without a compiler/source replay.
Two distinct Main-facing preparation documents correct stale snapshot/prose
selectors while retaining the original emitted metadata as immutable ancestry.

Genuine PE8 inspection then hit its **7,200-second** watchdog: Main
**7,272.703656 s / Job 677 / retained 677 / active 0**, wrapper root **1**,
guardian original-handle exit **1**, inner native root **43**.
Guardian **675 total / 674 retained / active 0** is an incomplete inner family;
the final suspended `dumpbin` is independently retained by Main, exit **43**,
but does not repair the guardian's negative. Wrapper streams completed with
**1,694 bytes / no reader errors**; no stream failure is inferred.
Retained guardian cycles are approximately **10.5–10.9 s**; three fresh plain
Main whole-ROOT walks measured **1.710072 / 1.706411 / 1.691703 s**.
These plain-walk timings alone do not establish the slowdown's cause.

The real stationary observer comparison measured **2.154683 s** with no
subtree labels versus **10.155839 s** with the guardian's exact **15 targets**,
identical complete ROOT/native allocation and measured **temp 0 /
phase 15,602,917 bytes**. The privately versioned helper4 computes lexical
target membership once per walk and file ancestry once per observation;
native observations, full inventory, retry/alias proofs and caps remain unchanged.
Two independent static reviews passed. Main's actual old3/new4 comparison
measured **10.154413 / 2.868695 s**, with complete ROOT, native allocation,
unique counts, retry observations and every label exactly equal.
Real exact-file/Windows-case/nested-directory/overlapping-label/sibling-prefix
checks matched; empty-label walks also matched (**2.092898 / 2.037042 s**).
This is CPU observer proof, not deployed PE or science acceptance.
The distinct current-caller cutover is required before fresh inspection;
no timeout/cap widening, cached proof or omitted acceptance edge.

The distinct Own15/Exec12/PE9/Wrapper4/Consumer10 callers were independently
reviewed twice and actually sealed. A pre-existing future-receipt pathname
gap was removed: every Own15 phase now consumes the **actual original completed
Consumer10 MainV5 fullNative binding**, supplied only after that run, and Exec12
independently validates it. The random SDK namespace is unchanged; no receipt
alias/copy, future acceptance, RNG override or MainV6 was introduced.
Actual metadata4 preparation passed **87.076201 s**, the deployed shared
observer passed **61.743236 s** with all four actual snapshots and **7 native
identity mutation refusals**, and fresh PE9 preparation passed **159.363113 s**.
Each closed **Main hard-8-GiB Job 1 / retained 1 / active 0 / native exit 0**.
The existing Build4 executable was reused; no compiler or source-GPU replay.
PE9/Wrapper4/Consumer10 acceptance is not inferred from these CPU preparations.
First Wrapper4 invocation then refused a caller-produced incomplete closure
before launching a guardian: **74.164615 s**, **Job 1 / retained 1 /
active 0 / native exit 1**. Two actual prepared metadata edges appeared in the
request/declaration but were absent from the supplied closure.
That input and negative remain immutable. The input-only correction binds both
actual edges and checks every literal edge in all four required documents;
no executed code, prepared source, compiler output or science was changed.
The corrected input-only PE9 retry also hit the unchanged **7,200-second**
watchdog. Main measured **7,279.402281 s**, closed **Job 2,054 / retained
2,054 / active 0 / native root exit 1**; wrapper **7,210.717429 s**, guardian
exit **1**, complete **1,694-byte** streams/no errors.
The inner guardian reports **Job total 2,052 / retained 2,051 / active 0 /
root exit 43**: incomplete family, therefore negative. Progress beyond PE8's
675 inner processes does not establish complete inspection or acceptance.
Retain both PE9 negatives. Investigate the real per-child resource/acknowledgment
cadence and remaining whole-inventory cost; no widened watchdog, cached native
proof, omitted publication price or missing-family waiver is authorized.
Ownership coverage, endpoint runtime and all six A1 references remain unrun.

The separate helper5 removed only measured lexical ancestor-Path allocations.
Two independent static reviews cleared its Windows parser boundary cases and
two more cleared the ten Own16/Exec13/PE10/Wrapper5/Consumer11/Builder5 callers.
Actual frozen4/new5 complete native inventories measured **3.291853 /
2.430509 s**: ROOT logical **12,279,748,981 bytes**, native allocation
**12,281,866,120 bytes**, **30,173 files / 601 directories**, all 15 target
labels, directory observations and retry observations were exactly equal.
Fresh native exact/case/ancestor/overlap/sibling and empty-label comparisons
also matched; **22** separately exercised lexical boundary cases matched.
This is observer CPU proof, not family, PE, GPU or scientific acceptance.
Current metadata5 passed **93.565452 s**, the deployed Own16 observer passed
**63.061223 s** (four snapshots / seven native identity mutation refusals),
and fresh PE10 preparation passed **161.559612 s**. Each closed actual
**Main Job 1 / retained 1 / active 0 / native exit 0**, reusing Build4.
Two caller-input refusals remain negative: missing pre-existing snapshot
namespace (**2.499666 s**) and the old PE9 namespace supplied to PE10 prepare
(**2.610027 s**); each closed **Job 1 / active 0 / native exit 1** before
its corresponding work. Distinct input-corrected calls produced the positives.
The retained helper5 proof's initial zero-growth price request was refused
before visibility; only the staged publication step was completed under a
fresh valid positive-growth quote. No old science/compiler replay, native
cache, receipt alias, timeout/cap change or retrospective positive occurred.
The fresh inspection closure included every actual prepared literal metadata
edge, including bootstrap/owner records. Genuine inspect-v19 then completed
its workload within the unchanged watch, but **remains negative**.
Main measured **6,709.468759 s**, closed **Job 2,248 / retained 2,248 /
active 0 / native root exit 1**; wrapper **6,639.747855 s**, guardian
**6,620.861085 s / exit 1**. The guardian retained its complete
**2,246-process** family, all native exits **0**:
**1,122 dumpbin / 1,122 link / 1 Python / 1 conhost**.
Its final strict image check rejected the unadmitted Windows console host;
the request admitted only Python/dumpbin/link. Complete **1,114-byte** wrapper
streams had no errors. This is a real image-manifest admission failure, not
a watchdog failure, and must not be converted into acceptance retrospectively.
Trace the native spawn and declaration/consumer contracts; only a separately
reviewed versioned correction may bind the legitimate dependency before launch
or prevent it without weakening debug custody. Consumer11, ownership coverage,
endpoint runtime and all six A1 references remain unrun.

The prospective Own17/Exec14/PE11/Wrapper6/Consumer12/Builder6 correction
received two independent static clearances and was actually sealed.
Main's native system-directory query and fullNative/SHA observation bind the
exact platform console-host image before launch; the identical record reaches
metadata/inspection closures, execution, prepared request/declaration/interface
and wrapper/consumer custody. It is a guardian process image, **not** an added
debug frontend. The original PE10 negative and every final strict image,
native-exit and complete-family gate remain unchanged.
Actual metadata6 **97.812774 s**, shared Own17 observer **64.177317 s**
(four snapshots / seven native mutation refusals), and PE11 preparation
**164.643194 s** each closed **Main Job 1 / retained 1 / active 0 /
native exit 0**, without compiler/source replay. Prepared literal edges and
the exact new process-image binding are closed before genuine inspect-v20.
These preparations do not establish PE11/Consumer12 or scientific acceptance.
Genuine inspect-v20 hit the unchanged **7,200-second** watch:
Main **7,281.953126 s / Job 2,185 / retained 2,185 / active 0 /
native root exit 1**; wrapper **7,212.002732 s**, guardian exit **1**.
The inner guardian measured **7,191.694871 s**, retired on the bound parent
request with root **43**, and reports **2,183 total / 2,182 retained /
active 0**: incomplete family, not accepted. Streams **1,701 bytes / no errors**.
Preserve this negative and do not run Consumer12 from it. No console-image
or scientific acceptance follows from the positive preparation boundaries.
A fresh actual inventory now measures **3.016268 s** at **37,690 files /
636 directories**, versus helper5's earlier **2.430509 s / 30,173 files**.
Separate profiling identifies duplicate rooted-path normalization; native APIs
are already initialized/memoized. Investigate only a separately reviewed
redundant-normalization successor and dependency-qualified phase-exit cleanup.
All native queries/lstats, whole-ROOT/resource proofs, prices and caps/watch
remain unchanged; no historical output is retired without its class's proof.
The narrow helper6 rooted-WindowsPath fast path received two static reviews.
Main's alternating full native walks and **14** actual bound fallback cases
matched exactly; three-walk medians **helper5 3.263303 / helper6 3.071139 s**
are modest CPU savings, **not** a complete-watch qualification.
The reviewed/sealed Own18/Exec15/PE12/Wrapper7/Consumer13/Builder7 cutover
passed actual metadata7 **108.528707 s**, shared observer **66.786406 s**
(four snapshots / seven native mutation refusals), and fresh preparation
**169.265749 s**, each **Job 1 / retained 1 / active 0 / native exit 0**.
Inspect-v21 and Consumer13 have not run.
Dependency review established that even empty per-command stderr, process JSON
and ACK evidence are literal fullNative receipt dependencies, not disposable.
No such files were deleted. The bounded **PE10-only** retained-copy route
received two independent static reviews. Main decoded all **374** original
policy witnesses and bound the exact **3,743-member / 33,393,189-byte** set.
The separately sealed v2 route preserves literal Windows path spellings and
annotated historical metadata; selected aliases still require exact fullNative
records. Its first real CPU proof failed **before archive creation**:
**27.030489 s / Main Job 1 / retained 1 / active 0 / native root exit 1**.
The original guardian's journal binding describes a published prefix, not the
whole file after its own terminal publication: the **10,673,457-byte** prefix
still matches exactly; the closed current journal is **10,675,027 bytes**.
The **1,570-byte** suffix consists of the two exact terminal-publication events
for the original negative run. All ten other historical journal records remain
whole-byte/native exact. Preserve the v2 failure and its literal inputs.
The v3 snapshot reader received two final independent static clearances after
fixing actual-consumer/terminal-validator refusal coverage, two deferred fixture
captures and a missing-path refusal. Authentic Main setup and fresh native
identity/resource admission were recovered; the new original-MainV5 smoke passed
**2.959747 s / Job 1 / retained 1 / active 0 / native exit 0**.
Under the unchanged **1,200-second** finite boundary, the real all-member v3
attempt ended **1,200.602664 s / Job 1 / retained 1 / active 0 / native exit 43**
with raw `AssertionError()`. Deadline attribution is **[INFERENCE]**: the failed
SDK row does not record the assertion site, sampled resources or peak commitment.
Its failure fields remain `terminal_accounting_after_failure` and
`root_failure_native_exit`; do not invent normal completion fields.
Unqualified archive/restore artifacts exist, but no proof was published and no
original outputs were retired. The archive's native birth was **767.915892 s**
after the worker's birth; setup, native pins and original consumption jointly
precede that boundary. A separate genuine owned-Job cost check passed
**3.240021 s / Job 1 / retained 1 / active 0 / native exit 0**: median lease
JSON decode **0.041432 s**, fresh whole-native/SHA check **0.012035 s**.
Repeated lease decoding is measured, not established as the sole deadline cause.
The reviewed v4 separately bounded original SDK stages. Actual complete original
capture **773.645769 s**, all-member original/archive/restore equality
**147.392200 s** and unchanged restored consumption **750.020641 s** each closed
**Job 1 / retained 1 / active 0 / native exit 0**. Refusal batches **0–4 /
4–8** passed **52.527801 / 649.465621 s** with the same terminal accounting.
Batch **8–12** ended **1,200.305989 s / Job 1 / retained 1 / active 0 /
native exit 43**, raw `AssertionError()`. Deadline-site attribution remains
**[INFERENCE]**; the original raw failure reports no assertion site. No final
proof was published, no original output retired and no science accepted.
The sealed v5 received two final source and actual-declaration-summary
clearances: **82 one-case refusal Jobs / 86 qualification Jobs**, separate full
pre/post consumers, **30** exact original-retirement chunks and **60** alias
release chunks across two namespaces. Its **183** planned stage indexes fit a
**192-Job** envelope; actual sealed declaration **8,577,051 bytes** gives growth
**1,344,328,832 bytes** under the unchanged caps.
The first actual v5 capture was interrupted by **Python kernel exit 130**.
Only owned namespace/lease metadata exists: no original root/family witness,
capture proof or completed SDK receipt was published. Preserve this scope as
unresolved and excluded; external kernel exit is not a native child exit.
Fresh authentic Main recovery has a different native identity; its original-SDK
smoke passed **3.494111 s / Job 1 / retained 1 / active 0 / native exit 0**.
A current process snapshot found no former-Main PID or parent-PID candidates;
this is not complete historical-family proof. The SDK's retained family/Job
handles and samples were in memory; no durable recovery channel was found.
Reviewed v6 passed fresh original capture **687.111459 s**, full archive/restore
equality **149.259336 s**, unchanged restored consumption **687.120458 s** and
**46 of 82** one-case refusals, each **Job 1 / retained 1 / active 0 / native
exit 0**. The next case, **46**, was interrupted by a second **Python kernel
exit 130**; its SDK namespace has no completed receipt. Preserve it unresolved,
not a failed native exit or completed family.
New authentic Main's original-SDK smoke passed **3.763178 s / Job 1 / retained
1 / active 0 / native exit 0**. The unchanged frozen v6 validator freshly
authenticated all **49** literal positive receipts, coverage cursor **46** and
full capture/restored-consumer equality. Fresh pinned byte/native checks also
passed for every **3,743** original/archive/restored member. Completed cases
need not be replayed; the interrupted case contributes no positive evidence.
The user explicitly approved retaining the old v6 aliases as immutable evidence
(**33,393,189 payload bytes**, plus filesystem overhead). This is an exception
only for that old alias scope: original-member retirement and full release of
the new continuation's temporary alias namespaces remain required. Never report
the retained old scope as cleaned up.
V7 kept the historical 49-receipt prefix literal and read-only, with fresh alias
custody and only **36** remaining refusal cases. Its reviewed first actual
restore failed before preparation: **82.099010 s / Job 1 / retained 1 /
active 0 / native exit 1**. The traceback identifies whole-dictionary comparison
of a historical **14-field** directory observation against its **six-field**
stable-identity projection; all six stable values match. Preserve the frozen
source, declaration and genuine negative, including both normal and failure
fields. The unchanged original negative-receipt reader accepted that raw record.
V8's frozen narrow directory comparison fix actually passed restore
**271.231526 s**, unchanged consumer **792.173595 s** and refusal cases
**46–68**, each **Main Job 1 / retained 1 / active 0 / native exit 0**.
Case 68's completion SHA-256 is
`947b9d705674067d538d24bee1666639fd29d8d2f2c8f436b05c4d029a030db5`.
Case **69**, `actual-consumer-current-journal-sha256`, was interrupted by actual
**Python kernel exit 130**. Fresh pinned inspection finds only its **643-byte**
SDK ownership journal and no completed receipt. Native Job creation, native exit
and whole-family terminal accounting are **not established**; keep it unresolved.
Authentic Main recovery now observes **PID 22496 / full creation time
134357151254629342 / native parent 16404**, different from V8's sealed Main.
**Do not resume D8/V8 unchanged or edit executed declarations/helpers.**
The unchanged frozen V8 validator freshly authenticated all **25** literal V8
positives and the separate **49** V6 positives in **66.530103 s**, coverage cursor
**69 of 82**; accepted cases must not be replayed or merged into V6's prefix.
Private `third-interruption-main-facts-571e01ead6456ae5.owned/recovery-facts.json`
binds the recovery, arguments, interruption and exact prefixes, SHA-256
`8809671519f8438e4c2c672dc90c8e2e6e0db76e23ec9f9b89ed01b76a74666e`.
Fresh ROOT logical/native allocation/free observations were
**12,865,959,068 / 12,908,403,848 / 19,143,897,088 bytes**, not future admission.
The previous admitted plan was **189 / 192**; a separately bound, source-reviewed
successor must account for the unresolved attempt and every additional stage
before launch, without widening the ceiling or inventing terminal witnesses.
No originals have been retired and no new aliases released. Only old V6 alias
retention is user-authorized. Preserve V3/V4 negatives, V5/V6 interruptions,
V7's genuine negative and V8 case 69; A1 and later scientific gates remain unrun.
All caps, finite **1,200-second** boundary and **7,200-second** watch are unchanged.

Main's bound successor schedule conservatively charges **79 prior attempts**:
**49 V6 positives + unresolved V6 case 46 + two original Main smokes + V7
negative + 25 V8 positives + unresolved V8 case 69**. The prospective **113**
stages end at **192/192**, with no separate preparation/smoke/failure spare.
Private `third-interruption-main-facts-571e01ead6456ae5.owned/actual-prefix-and-complete-route-budget-v9.json`,
SHA-256 `fce66f1385c935328163951c2ca67536d4c5a17db9f5723ef069458c41b1f470`,
binds every prior positive/negative obligation and the complete pending schedule.
V9's separately frozen sources and actual declaration passed two independent
static reviews. Actual restore **80** then closed **484.424632 s / Main Job 1 /
retained 1 / active 0 / native exit 0**, SDK accepted. Its unchanged receipt
SHA-256 is `254ffba49fc70f1e9bf561df5779502cba5df0db8d4a808b15750a694d6db28e`.
Post-run frozen `cpu_output` / `invocation_link` **rejected** that receipt:
Main omitted the declared Python executable from the SDK's physical input seals.
Do not repair the receipt, weaken the guard or count restore 80 as a qualified
positive. Its physical products exist but are not yet independently qualified.
Private `actual-restore80-Main-image-input-join-negative.json`, SHA-256
`53dbbe0bc993f0717ba912a21d848d778c0cf4b6178682f38d48e30bf789921a`,
binds the exact missing-input failure separately from the genuine closed family.
There are now **80 charged attempts + 112 scheduled stages = 192/192**;
an extra restore retry would exceed the ceiling. No stage 81 has launched.
The source-review proposal is a separately bound successor whose correctly
image-sealed stage 81 independently qualifies all present archive/original/V9
alias bytes and fully consumes them; it must not retrospectively admit job 80.
Two independent source reviews found this present-product route feasible,
without promoting the rejected invocation. Main's fresh full-pinned readback
passed **3,743 original/archive/V9-alias** byte comparisons, **3,743** original
file-owner checks and all **three** directory owners in **33.865370 s**.
Private `actual-present-products-readiness-no-invocation-waiver.json`, SHA-256
`c7a4ff3bdd23b41bdc2d54ef39fdc536b0ffa56855c456f122ac52a30455ab0d`,
binds readiness only: no full consumer, positive successor qualification or
additional Job. V10's five sources and actual **12,995,537-byte** declaration
passed two independent source and two actual-declaration reviews. Declaration
SHA-256: `075efb6a181dff6cdc12f030370f9035bf90110d83261f94278a80db5c57dce0`.
The corrected shared launcher sealed **43** physical inputs, both images and
the literal origin file; actual initial SDK JSON was **26,723 bytes**, with
fresh **3 GiB** no-cleanup-credit growth admission under the unchanged caps.
Scheduled qualification **81** nevertheless returned a genuine SDK negative:
**1,200.853006 s / retained 1 / failure terminal total 1 / active 0 /
native failure exit 43**, raw `AssertionError()`, empty stdout/stderr.
Completion SHA-256:
`cb0ad3710e43bf020d101382655bd260997f9772206f3ff1c2296ee4434785d8`.
Normal terminal/native/Popen exit, sampled/final resources and peak commitment
fields are absent; do not manufacture aliases or attribution from those gaps.
Deadline-site attribution is **INFERENCE** only. The V10 control scope contains
only its **12,996,157-byte** lease and **1,442-byte** journal; no consumer result.
Private `actual-qualify81-negative-raw-lifecycle-and-present-control-leaves.json`,
SHA-256 `fa2c16afec5595649310a31d6080dea4b3b7f10cad340397917f201f1e7510c9`,
preserves the raw family and control inventory. Current qualified prefix is
empty; **81 charged + 111 scheduled attempts = 192/192**.
A separately bound V11 decomposition now has two independent final-source
clearances and an actual **13,063,493-byte** declaration, SHA-256
`4b40f9f651ac272a958fa62805b0884ede0f9c30982ea1051b55078496a574a1`.
Scheduled 82 runs the complete original consumer plus genuine refusal 69;
its source-part result remains provisional. Scheduled 83 runs the complete
alias consumer plus refusal 70 and must establish both complete consumer
equalities through the genuine positive 82 invocation and unchanged V6/V8.
The actual raw-81 checker, exact current budget and literal 25-row V8 prefix
readback passed **111.298196 s**, no Job. All **21,036** old protected rows
remain an exact prefix, and all executed D10 protections remain retained.
Both actual-declaration reviews passed the complete actual change envelope;
unchanged inherited rows were checked through exact lineage evidence, not
individually reread. Scheduled **82** passed **1,083.534494 s / total 1 /
retained 1 / active 0 / native and Popen exit 0**, genuine source consumer
and refusal 69. Completion SHA-256
`fa2518b207685cea707af5cc81e09c4a3648ddf0220657b75480f23a1c27085b`;
the **2,461,098-byte** source-part SHA-256
`5bcda0bc2ef30f408026521240432f87ce63c4693f7c50946ab0f9efb8a3a1dc`
passed the fresh original SDK/output/invocation join in **122.549827 s**.
Peak Job commitment **751,161,344 bytes**. This result remains provisional:
whole-product qualification requires scheduled 83's complete alias consumer.
**82 charged + 110 pending = 192/192**; no retirement/science acceptance.
Combined pre-prune 98 runtime cost is unverified.
Scheduled **83** subsequently passed **1,013.139468 s / total 1 / retained 1 /
active 0 / native and Popen exit 0**. Completion SHA-256
`2684486867d3e3dd9002b95bc62b93c48dd493f516f4b34db4f643f1e4a23e28`.
The **2,904,938-byte** `qualified-products.json`, SHA-256
`ac949dc6f9fe993b3eb4c60a3f88a8bb41b86d2926d17389a57f7725819a1936`,
passed the fresh SDK/invocation/source-82 join in **135.771114 s**.
Both complete consumers equal unchanged V6/V8 results; genuine refusal 70
passed. Peak Job commitment **756,228,096 bytes**. The source/whole-product
pair remains immutable; whole-product custody acceptance is not scientific
phase acceptance.
Scheduled **84–94** and Main's fresh invocation joins passed all remaining
refusal cases **71–81**, native/Popen exit 0 and one retained, inactive-at-end
process per Job. Actual SDK walls **272.861691–477.087232 s**.
Finalize **95** passed **416.893845 s / total 1 / retained 1 / active 0 /
native and Popen exit 0**, completion SHA-256
`5ee69794c7f1b990df45037ee18ff6d623d648638c5945c60c3861d44148678b`.
The **2,507,983-byte** proof, SHA-256
`241f946bdfd2f5d2440d5ec292a2ae1056bd0268f382c3e301cda176b19ab72c`,
passed Main's fresh full invocation/coverage join **328.183499 s**:
all **82** refusal cases and complete consumer equality, literal V6/V8 prefixes
and the genuine 13-row V11 prefix. Bound Main acceptance SHA-256
`68e5f89fc874c65bf48e145a9ca9893a17a6f526d3bf128b2ad9f04b01e62584`.
Scheduled authorization **96** failed **1,200.466926 s / failure total 1 /
retained 1 / active 0 / native failure exit 43**, raw `AssertionError()`.
Completion SHA-256
`1b868f8bffe0a7d53b9568dbc5b587daa309fd00a8657f4b2c8932b8ea73c3b2`.
Normal terminal/native/Popen exit, sampled/final resources and peak commitment
fields are absent; preserve the raw negative without aliases or site attribution.
No authorization, historical route or active route was published. Private
`actual-authorize96-negative-raw-lifecycle-and-present-controls.json`, SHA-256
`3b5e04af8e86e669c56887b01c4b29db51f2ed11e0a76d879c234e5c02b96662`.
**96 charged + 96 remaining indices 97–192 = 192/192**. V11 and all accepted
results are executed/frozen. Independent V12 reviews found structural fusion
of authorization/installation into 97 feasible, conditional on the actual
successor contracts and unchanged-reader bridges. Source inspection supports
two isolated, joined **direct C9 consumer** threads inside one 8 GiB Job;
concurrent workflow wrappers are unsafe because they use `runpy` and shared
lease caches. Sequential initialization/module loading, private owner caches,
continuous readonly pins and all original per-consumer/native checks remain
mandatory. Both complete consumers must run freshly at 97 and again at 98.
V12's final four worker helpers and preparation interface passed both source
reviews. The first Main constructor failed before publication/Job on inherited
`oleaut32.dll` / `OLEAUT32.dll` literal paths: a new normalized-key union rejected
rows whose full native/size/SHA fields are equal. Frozen files and the negative
remain unchanged. Separately reviewed `prepare_literal_paths.py` changes only
that union to exact literal keys, preserving both old rows and exact-path
conflict refusal; no normalization, projection or old-binding alteration.
Actual **13,357,008-byte** D12 declaration, SHA-256
`d6886d1bcd6d084ff8831c80acb5fde7bdc71b1ec18fa6803b71bc0580b5d640`,
was published under original admission: **3 GiB** growth exceeds required
**2,863,838,186 bytes**; lease upper bounds **13,358,396 / 13,358,418 bytes**.
All **21,080** inherited protected rows remain an exact prefix.
Main's continuously pinned unchanged-W11 finalize95/stage-jobs bridge, all
82 cases/complete-consumer equalities and separate raw81/raw96 readers passed
**340.914154 s**, no Job; readback SHA-256
`b2454ef59ad874695059e387bd6ffc72ba6983058cd8263837c5abe55ef36ac2`.
Both independent actual-declaration reviews accepted the complete changed
contract, all **100** new protected bindings and exact inherited-prefix evidence
without unresolved findings. This was static clearance, not runtime approval.
The separately published literal 13-row V11/95 capsule, raw-96 failure capsule,
current Main facts and exact **96 + 96** ledger preserve old provenance rather
than synthesizing V12-positive history. Thread wall time and peak capacity are
unverified; no extra timing/retry Job is available.
Post-negative full-native/SHA and triple
byte readback passed all **3,743** originals/archive/V9 aliases and **three**
directory owners, **21.224454 s**, no Job; bound readback SHA-256
`5c788db4a553399296e43451757e59ced6657a5ee37ed5f691a618bb7c670f8f`.
The two release scopes remain **V9 and existing V8**, with fresh successor
controls/results; no third alias namespace or extension of old V6 retention.
No original retirement, new-alias release or A1 scientific acceptance occurred.

**Executed V12/97 negative; separately versioned V13 preparation.** The genuine
fused installation attempt ran once under unchanged limits and failed:
**1200.8043805 s**, raw `AssertionError()`, failure accounting
**total 1 / active 0 / terminated 0**, exact root **10044 /
134357384503152246**, one same-owned-Job signaled witness, native failure exit
**43**. SDK completion **95,784 bytes**, SHA-256
`71dc6612f2220c801b0b98b538ae8178f79fe4d488d7a6ad57c70f378b685618`.
Both streams are empty; normal terminal/native/Popen exits, peak and sampled/
final resource fields are absent, not normalized. Fresh native observations
found all three V12 control scopes empty; no route, authorization, predelete,
retirement or scientific acceptance. The failure site and whether either
consumer started are **not observed**: preparation's first durable write follows
the frozen95 gate, repeated full checks and both complete consumers.
Full native/SHA and triple original/archive/V9-alias readback passed all
**3,743** members and **three** directory owners, **20.7378571 s**, no Job,
SHA-256 `1d2a2b52b0ddea3fb396fc3497a87d28b6d35ae7ea4eea1f6edffe414e2d78c4`.
Preserve executed V12 and the raw negative unchanged. Its fixed positive97
prerequisite cannot be met by this negative. The ledger is now **97 charged +
95 pending 98–192 = 192/192**. Static original-contract review permits a NEW
combined authorization/installation/predelete98, retaining **two sequential
fresh complete original/alias pairs**, genuine combined-publication/positive98
joins before any99 deletion, all **90** destructive authorizations and the
unchanged99–192 suffix. No phantom97 success or self-completion input cycle.
V13's final five roles passed both full source/interface reviews after repairing
the second-pair publication pin barrier, four dangling branch references and
the versioned recovery-facts shape. The new recovery document stores exact
integer indices **[81,96,97]**; authorization/stage/current-idle registries retain
exact completion bindings, and old V12 facts stay literal.
Literal-path full-row indexing and bounded two-thread fresh-check traversals
preserve every native/SHA check and pin/join barrier; actual native duplicate
checks passed and a forged SHA was refused. All **534** interface references
resolve. Frozen source-role record SHA-256
`a17a01f0a74cdab3697f593a278f16069cb393a4b9fdbea5c6f010db13a8dc17`.
Actual D13 **13,547,609 bytes**, SHA-256
`056c8bc0eccd2dfb5783a9faa49c3c7c09ebbbe8a426242f5f1b7bdb1a760cd5`,
was published under full native pins/admission, **114.6053171 s**, no Job.
All **21,180** inherited protected rows remain an exact prefix; **23** new
rows produce **21,203**. Declared **3 GiB** growth exceeds **2,864,219,388 bytes**;
lease bounds **13,548,997 / 13,549,019 bytes** remain below 16 MiB.
Main's unchanged-W11 finalize95/all82/history49/frozen25/prefix13 full bridge
and separate raw81/raw96/raw97 readers passed **338.3881563 s**, no Job,
SHA-256 `c36171b52412e1b11b27245b648a289f9fb307d9426189c9c1a1e4d7261072e2`.
All **363** lossless actual-field views passed native/SHA/value readback.
Both independent actual-D13 reviews accepted all changed contracts and **23**
new protected rows without findings; static clearance only, no runtime,
speedup/deadline clearance, checksum/verdict cache or timing/retry Job. Actual Main
raw97 capsule (**27** exact fields,
**167** physical closure rows), current facts, exact ledger and fresh empty
controls are separately bound; integration summary SHA-256
`b9b89282a2b1790a1585fca20f35049e40def89fc32451d17439082a4f102e4b`.
**Executed V13/98 negative.** The genuine combine98 ran once:
**1200.7490307 s**, raw `AssertionError()`, failure terminal **total1/active0/
terminated0**, exact root **24340 / 134357424852077727**, one same-Job
signaled witness/native failure **43**; both streams empty. Completion
**106,894 bytes**, SHA-256
`8b0a6998447d57b550aca69326e701c52a06f8d5ddfc9d2f37bdccf85221ba32`.
Normal terminal/native/Popen exits, peak and sampled/final resources are absent.
All three V13 scopes remain empty under current native observation; no
installation/predelete/destruction/science acceptance. Site and pair start
remain **not observed**, and GIL contention is **[INFERENCE]**, not a diagnosis.
Full original/archive/V9-alias native/SHA/triple-byte readback passed **3,743**
members and **three** directory owners, **22.2730524 s**, no Job, SHA-256
`715f55f97ae8c3f8c99fbb46fab1b8e014aa51d2f894c76c5eef865e4a495f92`.
Freeze V13 and negative98 unchanged; **98 charged +94 pending99–192=192**.
A NEW V14 contract retains combine99's two temporally
sequential complete pairs, with two independent inherited-Job CPU children
inside each pair, then30 original prunes100–129, fused V9 full-use/full-check130,
V8 historical131, V9 releases132–161 and V8 check162/releases163–192.
Fused130 retains **two sequential complete same-alias consumers** and a genuine
single-invocation linked manifest; actual positive130 must qualify both outputs
before132, not masquerade as two completed Jobs. All **90** typed destructive
authorizations and every file/directory obligation remain.
Four consumer children plus root require **five actual original-SDK witnesses**
in the same aggregate8-GiB Job. Children remain suspended until a separately
reviewed Main observer proves the ORIGINAL live SDK sampler retained their
exact handles/full identities; no extra Job, SDK mutation, sleep-as-proof,
reconstructed witness or closed-handle query. The proposed readonly live-task
observer, child native/result protocol and bounded caller-owned phase journal
are source-sealed; actual child/runtime qualification remains required. Missing
phase END locates an unresolved interval, not its cause.
Actual raw98 capsule **27** fields/**188** physical rows, current recovery24/
ledger65, fresh empty controls and Main-owned retention-ACK control are bound;
integration summary SHA-256
`1caab4206a8ec8eb8ca850b273dd384cb0289f8e7672d59fba1187e1d642fd1b`.
The retention journal is mutable owner evidence, **not an immutable SDK input**.
SDK99 unrun; all caps, deadlines and full native/SHA/consumer gates unchanged.

**Actual sealed D14 preparation.** Seven corrected source roles have two complete
static source/interface reviews. Main's definition/metadata check exposed the
130/131 ledger-field mismatch; the unfrozen source and argument schema were
aligned to the immutable ledger before sealing. Genuine130 arguments require
exact-int `release_namespace:0`;131 contains only its index and stage.
D14 is **13,764,083 bytes**, SHA-256
`9ac250dc2b52c00e5636f033f4f410e0561ae170fa4566ef649793efe3646099`;
**21,229** protected rows preserve the exact **21,203**-row D13 prefix, with
**26** new rows. Actual publication passed **120.4842739 s**; unchanged full
W11/95, literal49/25/13 prefixes/all82 cases and separate raw81/96/97/98 readers
passed **355.2630804 s**, no Job. Main native/SHA-pinned first parsing of all
**545** lossless pages reconstructed the complete declaration exactly.
Growth remains **3,221,225,472 bytes**, above actual whole-coexistence minimum
**2,956,927,024**; lease upper bounds **13,765,471 /13,765,493** fit16MiB.
Both actual-declaration axes accepted statically. Actual99 prelaunch passed
**22.0682568 s**, **118,976** initial indent2 bytes/**202** controls/
**nine** argv/**13** bootstrap rows;1200s/3GiB unchanged. This prelaunch boundary
does not qualify deadline, child lifecycle, science or retirement.

**Executed SDK99 remains unresolved.** The first actual original-SDK invocation
hit deadline assertion82, then failure-witness conjunction150; no
`completed.json` was published. Do not reconstruct the failed operand, terminal
accounting, native exits, peak or wall from these assertion sites. The genuine
pair1 live retention ACK is **12,818 bytes**, SHA-256
`57de4774a1866b5aab8ae2f7bdc9826f23773766a3fb4552d08d5820ef0311d2`:
Root10160/134357481441218441 and children17768/134357487056493767,
22940/134357487057112193, actual inventory3/3/0 and all unsignaled wait258.
This is live sampler retention, not a completed-family witness. Root's20
phase records stop at pair1 native-join BEGIN; both children's nine fragments
reach C9 BEGIN without END/result. Root frozen95 measured328.799566s and the
overlapping child audits404.982241/408.655240s; missing END is an unresolved
interval, not a C9/GIL diagnosis.
Main sealed **36** actual metadata/phase bindings, SHA-256
`df0f5e11bdfe3c5ad764f42a98a26418406ea617c0fde96310349627313141f6`.
Postrun original/archive/V9 all3743 triple-byte/native/SHA readback passed
**21.7319142s**, three sealed directory owners, SHA-256
`bad228d4b40fc2fefeeaac2c56306029c2b0424f4fa38a79dd9a544371d69b64`;
V8 all3743 passed **21.0146111s**, SHA-256
`df8c18499f9ac58e7395eab1525e056c734b0ab5aa9dfb6da95ee471de1db212`.
An earlier Main Publisher-class prerequisite failed before SDK task creation;
that uncharged negative is preserved, and the genuine retained Source6
namespace/Publisher was reused, without source or SDK edits.
Preserve all V14 frozen/executed artifacts. **99charged+93pending100–192=192**.
A conditional NEW93-job route uses combine100, prunes101–130, individually
fused V9use/check131 and V8use/check132, V9releases133–162/V8releases163–192.
All90 authorizations and complete retirement obligations remain. Root-owned
full95 history qualification plus independently computed full C9 in each
child, and unresolved99 current-live exclusion, have a bound Main decision and
narrow independent authority clearance; no cached consumer verdict, relaxed
native checks or synthetic lifecycle witness. Main's fresh
current event-loop registry has no SDK/observer await chain; cleanup-success
reasoning from propagated exception/finally ordering is **[INFERENCE]**, not a
recorded accounting/exit witness. No successor/runtime/destruction/science
acceptance.
Main subsequently qualified the V8 **three** original restore-directory owners
and complete3,743-file inventory while pinned: **6.9156377s**, SHA-256
`3e72fe6532bd30dd81bf463f5288218aa4f1818ba64a50b275f2a10561414ac9`.
Original review/facts/retention journals are final immutable history; new
Source15 uses distinct controls and a genuine new retention Publisher.
Actual unresolved capsule **44** fields/**73** physical bindings, recovery31
and ledger74 are sealed. Embedding the new capsule made the unpublished
ordinary ledger1,125,440B and was refused before writes; the exact separately
bound capsule remains whole, and the binding-qualified ledger **1,022,239B**
fits the unchanged1MiB cap. Main integration summary SHA-256
`efac2fc5a0b3aaae198b1fcc74f9bbe4604402d024e783816df575e9f3922e4b`.
Seven-role Source15 was frozen after both complete source reviews, but its
actual Main declaration-publication guard failed before any write or SDK100:
the capsule's73-binding closure omitted the literal Python image already in
`protected`. Preserve that negative and all Source15 source/facts/journal seals.
Every later admission/authorization still needs a fresh actual Main registry;
the old empty snapshot is not perpetual live-work clearance.

**Source16 actual declaration and complete historical bridge passed.** The
narrow successor retains the exact old73 capsule rows and adds the authentic
full-native/SHA Main image as row74; recursive mandatory membership is not
weakened. Both full seven-role source reviews accepted. Main published
**13,889,538B** D16, SHA-256
`02d69aeec5eae446b9d20ab563f1f2a0a2b6a7922904a4668f29e25dd291fad8`,
through the original typed owned publication, physical-owner, budget and fresh
current-registry guards. Protected21,312 preserves the exact old21,229 prefix
and adds83, including all18 frozen Source15 evidence/source rows.
Unchanged complete95/49/25/13/all82/consumer/byte qualification, separate raw
81/96/97/98 and unresolved99, literal ledger74 and full native before/after
pins passed **433.2384094s** without a new Job. Bridge SHA-256
`b335f322e3a58055ece516bd9d6e65bddd7e4c44edf95e98ba7e8590e89d54b5`.
All597 lossless D16 pages across19 genuine Source6 Publishers were first-parsed
under readonly native/SHA pins and reconstructed from empty to whole-D16
equality; final journals max61,037B. Manifest SHA-256
`137659890cbdd37181b75bf7d613dffb0c04021258ebc58c9c5ffdeade570a47`.
Both actual D16 reviews accepted static integration. **SDK100 executed once
and closed negative**, not a timeout: original SDK **14.1185974s**, one
retained/signaled witness, total1/active0/terminated0, true native/Popen exit1,
peak Job commitment75,317,248B. Actual stderr rejects caller.py:442:
`TEMP` and `TMP` must both name the literal caller namespace. Main omitted
that temporary environment setup; the frozen original SDK inherits its parent
environment. Do not weaken the correct caller guard or repair Source16/SDK100.
Completed159,981B SHA-256
`11993e59829bf0443f839968d53fa6163a0e0f24d458e28fa7477d34c3332fb1`.
The unchanged original normal-negative reader accepted that receipt. Complete
postrun full-native/SHA pins over protected21,312, every actual SDK input,
all3,743 originals/archive/both3,743 aliases, triple-byte equality and physical
owner checks passed **76.1098064s**. Main's temporary environment assignment,
native caller-identity check and exact restoration passed without a child/Job;
no corrected native retry has run. Combined failure/preservation evidence SHA
`0bc29ba45ad9f25577fb7949c040a48ae988cf24f34d69f037c90123d55f40b1`.
No ACK/C9 child, destructive authorization or retirement occurred; SDK99
remains separately unresolved, never promoted to a completed failure.
**100 charged leaves92 under192, but the full remaining route requires93**
(one combine, two complete fused consumers,90 exact destructive chunks).
The user explicitly selected **“Authorize 193 total attempts”**. Only the total
ceiling changes192→193; all other caps,90 authorizations and full consumer/
retirement obligations remain. No diagnostic slot or weakened acceptance.
A0–A6 is neither settled nor silently narrowed; A1 masks/coverage/six references
and A2–A6 remain unrun. Native stays default; no deadline/science acceptance.

New Source17 integrates normal closed SDK100 separately from raw81/96/97/98
and unresolved99. Main sealed the exact nine-field193 authorization,985B SHA
`0c58d7e718db4c3d2f5bfcf2c2dbb8f47d88f4a1dfad2df07585c9e9ea07124f`,
and the32-field normal100 capsule with all271 unique full-native/SHA bindings,
336,593B SHA
`8eb42b6fbbeba07ba2e410b0beb98202ebe4a8b9a1eca6fec1d15c11ff397b9b`.
Original SDK100 completed publication legitimately uses `last=True` and has
before-byte/complete-scratch seals but no `PUBLISHED` event; preserve that exact
journal, not a reconstructed event. Recovery36 and ledger82 are native-sealed;
the binding-qualified ledger1,026,077B fits1MiB, SHA
`fddcfe578817b3bbfc2b1b52e26a4fb85133b0087a965671d8ebe6326f84e1e3`.
The authorized93 route is combine101, prunes102–131, complete fused V9/V8
132/133, releasesV9 134–163/V8 164–193. BOTH full fused positives precede134.
Fresh three controls and a genuine new Main retention Publisher exist.
The shared Main stage awaiter must hold literal caller TEMP/TMP through the
unchanged finite SDK/observer joins and restore the prior environment afterward.
**Source17 seven roles are frozen after both complete source reviews.** The
actual pre-freeze constructor stale-name negative and normal100 transitive
closure negative are preserved, uncharged. The current32-field v2 capsule adds
nine decoded prelaunch/final-facts native bindings to the exact old271 prefix:
**280** rows,342,279B SHA
`4cc3ed426a1498f9f9d4327bb7b69773e58873b6cbf12338a2dadb75653509dc`.
No recursive membership, native/SHA or genuine last-publication check weakened.
New linked36/82 preserve first-generation capsules/facts/ledger; current ledger
1,026,060B SHA
`158fbb8086d57eeff06fcc35b0fe22aa4050696a1936b5d3005887daf0586ebb`.
Actual normal100/unchanged historical W16 budget/current193 joins passed
**75.0036158s** before freeze. Main then published full D17 **14,462,963B**, SHA
`11a1ac45aac75e3c50bba1b98d1ee1595cd7e1a6f0d30d8841556e44b4b45a8d`,
through original typed owned/physical-owner/budget/fresh-registry guards.
Protected21,970 preserves exact old21,312+658, including all648 frozen Source16
evidence/597 pages/19 journals and complete normal100280 closure.
Complete unchanged95/49/25/13/all82/consumer/byte qualification plus raw4,
normal100, unresolved99, literal old192/current193 budgets and full physical
before/after pins passed **439.6360103s**, no new Job. Bridge SHA
`5b6828e7c82815c815fdd6776939b36de2006ae9f2ce241e77d4b5d281f6e54f`.
All649 lossless D17 pages/21 genuine Source6 Publishers passed readonly native/
SHA first parse and empty-to-whole-D17 equality; final journals max61,018B,
manifest SHA
`9cabc210d596af728fcfd0dbbb7187ee1c49fada7ba931279469697bb82dccd1`.
Both actual D17 reviews accepted; **SDK101 ran once and closed normally
negative**, not at the deadline: **610.5715812s**, Root and two retained
same-Job children each signaled with true native exit1, Popen Root exit1,
terminal total3/active0/terminated0, actual peak commitment **673,976,320B**.
The corrected Main TEMP/TMP wrapper passed the actual Root checks and restored
the exact prior environment. Genuine first-pair Main ACK exists; no second
pair, completed C9, authorization, retirement or science acceptance.

Both children failed `raw_normal95` at `consumer_child.py:337`: a new predicate
required the immutable SDK95 argument file's parent to equal the historical
worker caller namespace. Actual accepted SDK95 arguments were instead
Main-published in the static-review namespace; the worker caller directory is
its separate scratch destination. The accepted W11 invocation/input/native
contract imposes no such placement constraint. Independent readonly review
confirmed this cause and recommends removing only that predicate in a new
successor, retaining every full-native/SHA, argv, stable6 caller and closure
guard. Source17, SDK95 and all executed arguments/receipts remain frozen.

Main exercised the complete prospective `raw_normal95` API with exactly that
one AST predicate removed in memory, under readonly native before/after pins:
**2.5417302s**, returning the exact original immutable95 proof. No process,
SDK attempt or C9 ran; corrected child execution and all four C9 remain
unverified. Smoke SHA
`ad5c02125ddcd1a061c65cf1e3d90e5b62f037a64eaad577b7ebd88e75864040`.
Normal-negative original SDK reader and full post-run native/SHA custody
readback passed **79.2044406s**: all21,970 protected rows, actual SDK inputs,
31 runtime artifacts, all3,743 originals, archive and both3,743-file aliases,
triple-byte equality and unchanged physical owners. Failure proof340,379B SHA
`5666516425f5c27e4a02fa8d6fcb2fbf4c90abfa42a419f91bf516b0fd2e5256`.
Source17 final49,378B state and all final journals are native-sealed, SHA
`5bc33d8712542ac93471275f42422d98e2186c246c51a3e6fc773767da907be6`.
SDK99 remains unresolved; SDK100/101 are separately normal closed negatives.

**Current blocker:101 charged + unchanged93 required =194 total**, but the
user-approved ceiling remains193, leaving92. No extra SDK Job or successor
declaration is authorized. Production-only continuation would require194;
the [current testing revision](#autonomous-testing-and-production-promotion)
also requires an explicitly approved finite diagnostic/retry envelope, not
an implicit194 or206 authorization;
all deadlines, resource/publication caps,90 destructive authorizations, both
full fused positives before releases, original-member retirement and both
alias releases remain unchanged. The prospective route would be combine102,
prunes103–132, full fused133/134, V9 releases135–164, V8 releases165–194.
A0–A6 remains authorized but blocked/not settled; A1 and A2–A6 remain unrun.


The negative's **468,425,728-byte** duplicate reset TAR was actually retired
after full original/restored byte equality, unchanged native canonical
consumers and independent static reviews. Actual selfcheck **5.2367859 s**,
native publisher smoke **8.2711110 s**, full proof **18.5440268 s**,
prune **14.4071714 s** and temporary restore removal **10.3449508 s** each
closed **Main hard-8-GiB Job 1 / retained 1 / active 0 / native exit 0**.
The argument-only initial prune refusal remains negative. The retained original
source TAR, all negative metadata and both active Source4 archive identities
are unchanged. Post-cleanup ROOT logical/native allocation/free observations
were **11,992,456,119 / 11,985,345,512 / 22,860,480,512 bytes**; no physical
reclaim is inferred. The fresh-namespace historical full restore/consumer
passed **18.0741433 s**, followed by exact-owned alias release **8.8775693 s**,
each **Main hard-8-GiB Job 1 / retained 1 / active 0 / native exit 0**.
The original retired identity was not regenerated; the retained source root
and immutable route remain exact. No source science was replayed, no
coverage/A1 acceptance follows, and fresh phase admission remains mandatory.


### New-dataset execution order

1. **Establish original-recording readiness and fresh A0 bindings.** Start
   from the privately selected full-resolution `.insv` originals. Freeze native
   identities/SHA, decode/extraction/projection and camera/track/timestamp
   settings plus raw-to-frame/mask/reconstruction lineage; leave live preview
   preparation untouched. Finalize the original-derived native reconstruction,
   verify parser/effective configuration and freeze newly derived global,
   training/held-out/name/post-camera mappings. These gates are pending, not
   satisfied by old Source v5 or `.lrv` frame completion. Recompute all data/
   source/owner/exposure/coexistence bounds. No previous dataset's fixed counts,
   masks, source checkpoint or native references can satisfy this admission.
   A0 admission is staged: approve the preparation/source-generation inputs,
   effective configuration, immutable producer and whole-phase resource bounds
   before those runs; bind their actual outputs afterward. Freeze A1's source/
   reference-step and horizon contract before source generation. Fresh checkpoint,
   owner-mask and native-reference outputs are not prerequisites for producing
   themselves, but must be bound before their dependent comparisons.
2. **Establish a new source and A1 contract.** Use a genuine native optimizer
   checkpoint trained on these exact inputs, never the kitchen checkpoint or a
   merged PLY. Retain the planned source-step 10,000 → reference-step 20,000
   comparison under horizon 30,000, but freshly admit source generation and
   freeze its effective native configuration before running it. Record common
   source-generation cost separately from continuation timings. Derive actual
   row counts, inactive rows, codec tail and installed owner masks from this
   source. Obtain all baselines and quality envelopes on the replacement data.
3. **Preserve the actual producer before candidate edits.** Use a new, versioned
   custody declaration for the exact current source/binary/runtime and actual
   new-campaign consumer dependencies; prove complete isolated restoration and
   all four restored CPU consumers. Reuse the repaired custody machinery, not
   its old dataset-bound declarations. Preserve missing historical dependencies
   as explicit historical limitations, not prerequisites copied into unrelated
   new work or silently removed from old proofs.
   This step protects the admitted producer before candidate edits; it does not
   waive any existing custody obligation needed by preparation/source generation.
   Close those dependencies before the run that needs them, without redirecting
   historical declarations to new data.
4. **Rebind coverage and bounded candidate checks.** The prepared footprints v2,
   no-forward precursor, supervisor and verifier are kitchen-bound preparation,
   not ready-to-run new-dataset artifacts. Replace their fixed counts, hashes,
   dimensions, source references and output/admission bounds in new versions;
   independently review and build only after custody. Prove the new source's
   canonical/native restoration, clocks, frozen/shared state, host/radii and
   complete stable-ID domain. Capture both actual installed masks, then probe
   every training view on each supported AMD device under separate admission.
   The old 1,195/132 views, 1M/7,150 rows, 64-row tail and 512 MiB reservation are
   historical facts, not new-input assumptions or accepted storage bounds.
5. **Resume evidence-led A4–A6 on the new workload.** Reuse implemented mechanisms
   and historical hypotheses, not old dataset acceptance. Run the relevant
   new-source ownership/consumer checks before candidate comparisons. Reassess
   the queue from fresh coverage/cost evidence; do not blindly repeat all old
   experiments. Preserve A1's six-metric native-relative rule, A5's fresh matched
   repeats and long-recovery requirements, and A6's settlement conditions.

The current **shared retained-output cap is 16 GiB (17,179,869,184 bytes)**,
including all historical shared ROOT data and every campaign namespace; this
is not a fresh 16 GiB allowance. Only this limit changed by explicit user
selection. **24 GiB worker GPU backing**, **12 GiB scope-3 sampled host**,
**8 GiB private native Job**, **10 GiB free-space floor**, **1 GiB worker heap
margin**, **4 GiB baseline reservation** and **9 GiB commit reserve** remain
unchanged, as do non-NVIDIA scope, shapes, native objective, layout, source/
reference steps, horizon, tolerances, ownership safeguards and three-class
cleanup. Fresh per-phase/coexistence admission must include retained history,
all dependencies and temporary costs. Historical 8 GiB observations retain
their then-valid limits; no old estimate or result admits this dataset, and
16 GiB does not guarantee completion. The current revision is plan-only;
earlier authorized execution recorded above remains historical and grants no
permission to prepare or evaluate the selected originals.

## Restart execution — 2026-10-03

**Historical kitchen attempt: BLOCKED, NOT SETTLED.** All 1,416 protected inputs
were unavailable at their manifest-bound location, totaling 3,484,499,427 bytes.
The original checkpoint remains available; 17 protected source/config/reference
bindings and the current C++ proof source plus both executables were rehashed
exact. No source cutover, build, native footprint precursor or GPU probe ran.
Native remains default; Q's decoded `1e-4` recovery screen remains FAILED.

- Repaired all four custody defects below and obtained independent review.
  The real owned-Job CPU boundary check passed with exit 0 and resource
  acceptance: supervisor wall 2.3220143 s, two Job processes, zero active at
  completion, sampled aggregate RSS 162,336,768 bytes. It exercised bounded
  partial-ZIP failure, full-width native file/directory deletion and refusal,
  ROOT-local consumer scratch, and normal/reproduction inventory races.
- Retired ten exact-owned, non-scientific PLY-analysis ID expansions after their
  final consumer: **39,467,000 logical bytes**. Recipes, archives, numerical
  witnesses and native checkpoint sidecars remain. Post-cleanup ROOT measured
  7,980,890,668 logical bytes and 7,883,004,328 native allocation bytes, with
  29,492,670,464 bytes free. These are point-in-time observations, not peak or
  future-phase admission claims.
- The freshly admitted producer prototype exited 1 during protected-input
  verification, **before creating its declaration or archive**. The dataset
  root was absent; all 1,416 bound inputs were missing. Its Job finished with
  zero active processes. No known byte-identical dataset restore source was
  found in the declared locations or recorded custody routes; checkpoints and
  derived outputs cannot replace those inputs.
- Footprints v2 and a minimal native no-forward mapping/mask precursor,
  owned-Job supervisor and independent verifier are prepared and independently
  static-reviewed. The CPU metadata/cap selfcheck passed (0.11 s); this is not
  the unbuilt native counter/refusal gate or GPU/source qualification.

Local records: `A4-restart-20261003-custody-cpu-parent.receipt.json`,
`A4-restart-20261003-analysis-cleanup.receipt.json`,
`A4-ppisp-cpu-price-producer-v1-prototype.run.json` and
`A4-restart-20261003-blocked.receipt.json`. The new CPU receipt's credential
value was redacted before its parent binding; future supervisor receipts redact
secret-named environment values without changing the child environment.

**Historical reproduction only:** restoring those exact 1,416 inputs remains
necessary to retry this failed prototype or its dataset-bound 1,195-view probe.
Preserve the failed attempt. Future work instead follows the
[replacement-dataset plan](#replacement-dataset-plan--2026-10-03); no old result
is rebound or upgraded. A4 remains incomplete and A5/A6 remain unrun.

## Immediate restart order — 2026-10-03

**Historical kitchen restart order; superseded for new-dataset work by the
[replacement-dataset plan](#replacement-dataset-plan--2026-10-03).** A2/A3 are
complete only within their recorded kitchen scopes; A4 remains incomplete and
A5/A6 are unrun. Native remains default and Q's decoded `1e-4` recovery screen
remains FAILED unchanged. PPISP evidence is component/single-packet microprice
only; the 43-field, 383,983,064-byte CPU proof is source-before, not after-state
or GPU qualification.

**Historical scope:** this order describes the kitchen continuation. Its
[three-class cleanup policy](#aggressive-dependency-aware-cleanup) remains in
force for both campaigns. Historical measurements, immutable manifests, failed
receipts and evidence bindings below remain unchanged.
Retiring truly disposable data is distinct from retiring protected science or
data still needed for reconstruction; this policy is not permission to relabel
old failures, discard required witnesses or waive preservation contracts.

### Fix the four prepared-custody defects first

The current-price producer custody v1's four prepared defects were repaired,
independently reviewed and exercised by the real CPU boundary gate above.
Its prototype is now blocked on missing protected dataset inputs, not accepted
producer custody. The required repairs were:

1. Close the partial-archive writer before pinned hashing and writing its
   failure receipt. Preserve the original negative cap/write error and partial
   archive; do not replace the original failure with a later hashing error.
2. Bind the owned final target's full 64-bit volume and 128-bit file identity
   through deletion using a native handle to the same object, not a filepath
   check/fsync/unlink window. Cover files and empty directories in normal,
   recovery and reproduction routes; pin ancestors, refuse reparse traversal
   and never chmod protected files.
3. Contain FNV/SHA packet consumers' `fs::temp_directory_path` scratch in an
   exclusive owned/admitted namespace under ROOT using TEMP/TMP for initial
   and reproduction consumers. Track every generated scratch object and retain
   or identity-prune it; nothing may escape to shared Temp.
4. Make bounded inventory retries route reproduction to its own exact restore
   namespace, while normal/recovery use their existing owned namespace.
   A missing-entry race must not invalidate a valid prune; mismatched identity
   must refuse.

### Preserve the current producer, then probe full coverage

After the custody repair/review/CPU gates, follow this sequence; do not blindly
retry known-inadmissible phases:

1. Perform dependency-aware cleanup under the policy below, then refresh the
   whole-phase admission ledger. Preserve the full current compile/runtime and
   transitive actual-consumer closure before any GPU candidate source edit.
2. Prototype-pack the current-price producer, measure the actual archive size,
   admit the complete restore phase, restore every member in isolation and run
   all four actual restored consumers. Only then exact-owned-prune its temporary
   restoration. Predecessor archive/proof references may be externally immutable
   rather than recopied once their acyclic restoration is validated; do not
   omit a real dependency or copy every old helper/log/archive by default.
3. Independently review prepared footprints v2, then source-cutover/build and
   exercise CPU counter/refusal gates. Its patch is prepared only, not applied,
   built or run. Full source cutover preservation proof is still required.
4. Freshly admit and run the complete 1,195-view coverage probes on supported
   AMD devices. Colmap fraction compacts 1,327 to 1,195 before the manager:
   use manager IDs 0..1194 with explicit original-global/name/post-face mapping
   and the 132 held-out exclusions, not parser-global IDs as manager IDs.
   Capture native flattened screen ID → complete Gaussian ID → installed B1
   256-row owner mask separately for owners 0/1. Preserve all 1M stable IDs,
   7,150 inactive rows and the 64-row tail; footprints are a superset, not exact
   pixel contribution.
5. Independently report coverage, cost and resources, clean up immediately
   after final consumers, and make the next A4 decision from those results.
   Preserve full43 source-before/restore checks, frozen18, clocks, contracts,
   host/radii and the exact same native N/source horizon. The v2 scheme aliases
   source-native28 to full43 raw once only after actual type/length/SHA equality,
   with complete extras bound. Its 510,130,496-byte worst output and
   536,870,912-byte cap are prepared bounds, not accepted actual reservations;
   producer custody itself consumes ROOT space, so the previous 512 MiB fit
   is stale. C2v3's 3,517,362,683-byte bound is neither waived nor executed.

An admission refusal is **BLOCKED, not family rejection**. A finite probe with
zero candidates establishes zero Gaussian raster VJP only for its bound
source/view/mask; it does not skip statistics, regularization, PPISP, Adam or
clocks, establish an end-to-end win, or exhaust a family. A5/A6 remain downstream
of an evidence-qualified A4 decision, not automatic consequences of this probe.

### Aggressive dependency-aware cleanup

Do not keep everything by default. Give every artifact a class, its next actual
consumer and a concrete retention reason; clean up after that final consumer,
on success, failure or abandonment, once owned Jobs have zero active processes,
handles are closed and the objects are idle/unleased.

| Class | Retention and retirement rule |
|---|---|
| Protected/required science | Retain original source/encoded inputs, indispensable historical science, the first full numerical failure and unresolved root-cause witnesses required by acceptance. A hash alone never permits deleting negative raw science. |
| Result-backed or reconstruction-needed material | Keep one exact bound copy of executed producer/helper versions needed for reproducibility, or a proven acyclic archive reference. Before retiring result-backed raw data or a protected-contract dependency, prove the successor's full-byte isolated restoration and required actual consumers, then atomically update mutable active references before exact-owned retirement. Never rewrite immutable old manifests/failed receipts or grant unearned acceptance. |
| Truly disposable non-evidence | Immediately remove unneeded owned superseded prepared/unexecuted scaffolds, synthetic fixtures, canceled partial scratch after the correct failure receipt, restored/decoded trees after their final consumer, redundant proved raw expansions, previous disposable builds and repeated helper outputs. Retain a small inventory/deletion receipt with path, full identity, reason and bytes; no archive/restore/compression exercise is needed for material that backs neither evidence nor a protected/reconstruction contract. |

Apply this as a phase exit gate, not a disk-full reaction. Reserve the complete
negative path, coexistence, pack plus restore, logs, consumer temp and cleanup
workspace before launch. Measure actual ROOT unique-inode logical bytes,
allocated physical bytes and free space separately, then refresh admission;
no projected compression credit, future deletion credit or cap waiver.
Prefer deleting obsolete scratch, duplicates and proved raw copies over new
compression research; archive only necessary retained witnesses/dependencies.
If safe cleanup cannot fit, name the exact protected dependency and capacity
deficit and continue independent reachable work. Do not enter an indefinite
compression loop or infer A4 exhaustion from a storage refusal.

## Historical status and measured results

Status: **A0–A2 exercised through bounded actual-source gates; A3 diagnostic
complete; A4 active; continuation resumed on 2026-10-02, NOT SETTLED.**
User-authorized read-only hardlink cleanup
reclaimed 10,507,497,472 allocated B; post-cleanup free space was
24,947,101,696 B, above the 14 GiB admission bound. The first actual source-scale
A2 group completed 16 updates/owner at cadence 8 from source 10,000 to 10,016
under horizon 30,000, through two generations with exit 0 and merged publication.
Retained-source raw installation/refusal, partial-install/transfer recovery,
mixed/no-replace refusals, independent owner STOP/resume and native eval/mesh-input
consumer checks are now exercised; mesh-input parsing is not full meshing.
All four recovered final pairs have exact coherent render state, source-exact
unowned optimizer/shared state, exact clocks and complete 1,000,000-row stable-ID
coverage. Owner-0's first resume exposed normal terminal `Stopped` being treated
as explicit STOP; `B1Group.cpp` now binds that interpretation to the exact
requested owner job, with repaired publication and fresh owner-1 resume exiting 0.
The final current-build headless rerun after that repair passed 17/17 in 7.99 s
with no exclusions (`A2-final-headless.json`/log).
Both primary arms now prove complete retained native step-10,000–19,999 event
sequences, not merely A2's 16-event prefix: **10,000 accepted events and all
1,195 training IDs per owner**, 123,433,689,024 pixels each. Both split arms
have **20,000 aggregate exposures**, versus native N's 10,000; trajectories
and aggregate work are not equivalent to N. Source-relative clocks,
immutable masks/configuration, source-exact shared/unowned optimizer state
and complete original-row coverage passed. Refresh also has exact final
paired render/FPBO state and **ten committed generations**. Primary cadence
**1,000** was frozen before either arm, from 10,000 to 20,000 under horizon
30,000 (`A3-frozen-contract.json`); it remains diagnostic, not selected.
Fresh native evaluations of both arms and bound historical N cover all
**132 named held-out views**. Refresh raw/cc mean PSNR is **20.1918447 /
24.3842932 dB**, versus control **20.1452341 / 24.2488492** and N **20.7651462 /
25.2906076**. Mean-gap closure is **7.5189% / 13.0015%**, leaving **−0.5733015 /
−0.9063144 dB** versus N and **101 / 128** per-view regressions. Raw SSIM and
L1 worsen versus control; all three cc means improve, but all six refresh
means remain worse than N. The handoff records all six means coherently.
Control native process lifetime **3,245.6087134 s** and refresh monitor wall
**3,450.7866036 s** are one pair with nonidentical scopes, not A5 speed
qualification. The control monitor's exit 130 did not interrupt native training,
but its resource-sampling gap remains unverified. Measured refresh barriers
sum to **227.6657683 s** using each generation's maximum owner; phases overlap.
Logical export and install traffic are **1,480,813,380 B each**.
Observed resource samples/worker backing passed the declared caps, not true
host/storage or physical-device peaks and not a headroom claim.
All six selected worst-view GT/render runs at slots 5/118 exited 0; each complete
132-view evaluation's 792 values exactly match its primary arm, with GT PNG
bytes equal across arms per slot. Parent downsampled visual inspection found
apparent illumination/texture/edge differences, not a root cause or full meshing.
All 1,416 encoded input sizes/hashes remained exact afterward.
`A3-completion.json` closes the diagnostic and binds the executed A4 decision:
mixed response and residual loss do not justify blind cadence refinement or A5.
A4's corrected same-input native all-owned/marked-owner FPBO check exited 0:
four arms, 32 updates on the same eight native events, source-exact resets,
role/unowned/shared/clocks/tail invariants passed. Initial forward/cotangents
were byte-exact; reduction/screen-gradient differences were the same order as
native repeated-control variation, not a demonstrated mask-semantic bug.
Larger event-8 frozen-peer render/data-gradient deltas support investigating
coupled trajectory/staleness, not A3/Q cause attribution or recovery parity;
descriptive `decoded_errors=0` does not waive the unchanged screen.
The repaired helper preserves native policy validation after the first pre-arm
environment-override refusal. Parent build/headless results (17/17, 13.39 s)
bind a newer diagnostic binary, not A3's `c66ccb…` evaluator.
**A4 is active:** materially different single-mapping-slot/worker memory-resident
G1 is implemented/static-reviewed; `A4-build-mapped.json` records dev build
exit 0 (8.962 s), CPU packet/mapping checks exit 0 (0.188 s) and headless
17/17 (12.96 s). Ordered/unordered source-capture/internal-FPBO controls are priced;
Actual-source mapped two-AMD smoke and independent verification exited 0:
native gradient/reduction/state/checkpoint and refusal gates passed within
the retained ordered numerical variant, not production/ordinary-raster equivalence,
long recovery or speed qualification. The unchanged frozen **12/12 finite arms
are executed and operationally verified** (`A4-finite-price-report.json`).
Each uses eight source events/six measured exposures; G1 five updates to10,005,
sequential controls eight to10,008: optimizer trajectories differ.

| Finite method | Complete-wall median / observed range (s) | Measured median (s) |
|---|---|---|
| File G1 | 54.4959209 / 54.3585746–54.760452 | 24.1235971 |
| Single-slot mapped G1 | 41.1911613 / 39.9639309–41.7571689 | 16.1803535 |
| Ordered-raster source-capture/internal-FPBO control | 12.406858 / 12.3875173–12.4483902 | 3.8364355 |
| Unordered-raster source-capture/internal-FPBO control | 13.0982101 / 13.0880138–13.1199239 | 4.3062366 |

Mapped full-state G1 is slower than both controls across nonoverlapping bounded
complete-wall ranges; this is **finite pricing, not A5 quality/speed qualification**.
**Control-scope correction:** both harness controls use `source_capture`'s two
loss/backward passes with backward-stat restoration, then one internal FPBO update;
ordinary `TrainerCore`/CLI uses one loss/backward per event. Frozen matrix JSON
names stay unchanged; its `native` label is the unordered-raster diagnostic
control, **not the best ordinary CLI price**. Actual eight-event CLI arm is
not run. Backward-stat priming/reset is proved; pixel-qgrad-transient cause is
unproved and `source_setup` has `grad.quantize_grad=false`.
All three file arms and first two mapped arms have exact first-file 43 fields.
**Mapped r3 differs in fields0–18**; own native checkpoint readback, frozen/shared
state, IDs/clocks and exact archives do not establish cross-arm numerical parity.
Direct world-float post-five-update maxabs: scales **0.0652447**, opacities
**0.1165082**, quaternions **0.00990278**; cause unresolved, **not original Q's
decoded recovery screen**. Unordered diagnostic-control r2/r3 fields0–18,23 differ from
r1 (r3 from both earlier repeats); no numerical repeatability claim.
Original mapped-r3 cause remains unresolved. Resident-owned cached-worker FNV
and SHA five-update native oracles have complete independent science receipts.
No family exhaustion, selected winner, quality/recovery or release.

The frozen matrix source/proof executable are retained read-only under
`A4-frozen-matrix-source-binary.custody.json`; subsequent diagnostic changes
do not alter that matrix's declarations, source or acceptance.
`A4-native-repeat-world-variation-diagnostic.json` restores retained native
repeats: r2/r3 versus r1 scale maxabs **0.000462532 / 0.000593662**, opacity
**0.000490189 / 0.000597477**. This descriptive comparison does not explain
the larger mapped-r3 variation or change Q's recovery screen.
The minimal `--fpbo-timing-smoke --updates 3` extension exercises the finite
eight-event/five-update path with existing internal/external, complete-owner
publication and refusal gates; default smoke is unchanged. Supported dev Vulkan
build/CPU packet-mapping/headless17/17 passed (`A4-build-five-update-smoke.json`);
actual `A4-five-update-mapped-oracle.verification.json` passed all five updates,
eight events, full1M IDs/clocks/source-shared and exact first-file43/TAR/PLY.
That is one red-capable native oracle, **not a mapped-r3 cause fix**.
`A4-build-stage-hashes.json` records the later dev/CPU/headless17/17 pass;
reference/repeat stage probes both pass exact first-file43/TAR/PLY. Existing
computed packet-body FNV values add no new native hash pass. Raw op2 reply FNV
is intentionally attempt-specific: final `frozen_owner_attestation` binds
`packet_attempt`. `A4-stage-owner-attestation-normalization.json` reverses the
known16-byte suffix with selfchecks; all10 first20-native-field fingerprints
match, as do read canonical+gradient and non-op2 write fingerprints.
Current `A4-stage-pair-diagnostic.json` supersedes retained
`.pre-attestation-normalization.json`; **not all raw body hashes match**.
FNV is diagnostic, not full SHA proof. Neither probe reproduces or explains
original mapped-r3; independent final SHA/custody proofs remain authoritative.
Oracle/probes retain exact packet archives, full isolated packet/serial native
restores, exact first-file native aliases and raw read-only PLY hardlinks.
Restore/install source → first-file packet → target packet or first-file native
TAR members → explicit targets; no circular or approximate aliases.
Historical stage-diagnostic proof SHA-256
`45f555176a0a09b8ce3a211c3d64c46e60328ede778237e99eab68dcd689c6c9`,
not frozen matrix `a92cae…`; its application was `77088eb…`.
Stage source/executable now retained in literal ZIP **26,303,630 B**, SHA-256
`b30c17384fef7ca9ae1117f3fadae3fdb002d02dd28c40a2ab0cbee4936d184b`;
full isolated source/executable SHA restores and restored `--packet-check` exit0
passed (`A4-stage-diagnostic-source-binary-custody.json`), matrix snapshots raw.
Source scout `EngineOptim` → Vulkan FPBO launcher → `fpbo.slang` proves all
six Gaussian gradients are consumed, including owned DC/SH updates/regularizers.
Frozen shared appearance is **not frozen Gaussian appearance**: omitting DC/SH
from the current external-gradient contract is unsafe. This rejects only that
omission, not residency/sparse protocol families. A4–A6 remain incomplete.
`A4-hash-cost-probe-report.json` binds three actual **380,013,056 B** raw native
TAR CPU passes: FNV median **0.2791278 s**, CNG SHA-256 **0.1461911 s**,
all3 complete original-source SHA exact. Failed quoting invocation/exit1 and
corrected batch amendment retained; native probe exit0. **Not actual wire hashing
or training wall**, no extrapolated speed claim.
Resident-owned worker-local cached native-state exchange and optional Windows
CNG SHA256-64 packet digest are implemented, not GPU-qualified. All6 gradients,
native full D2H readback, unowned exact checks and postcapture full-cache restore
remain: **not pure GPU residency or zero-copy**. Read-only capacity finding
(largest scheduled RGB input) repaired; final review reports no findings.
`A4-build-resident-owned.json` records dev build0, CPU packet-check FNV0/SHA2560,
headless17/17. Current proof **88,132,608 B**, SHA-256
`57d75710519dfc9c854fce29ecd85c3c41324c505ee2257d077728ceeba824c9`.
Application relinked **100,905,984 B**, SHA-256
`739d126c019e4233ea60ae06f4ec4d9b388d04e0b7d553b058f9c506db4503e7`:
**replaced, not unchanged `77088eb…`**. Build log has23 app objects/relink and
no engine/shader-library rebuild; byte-change cause unresolved, not invented.
`A4-resident-owned-fnv-oracle.run.json` records native exit **0**, no monitored
STOP, complete wall **43.6991079 s**, sampled owned-tree-plus-monitor RSS
**7,842,668,544 B**, free minimum **14,657,773,568 B** and new-live maximum
**8,450,455,561 B**. The refusal child exited the expected **1**; not a native
failure. Standalone verification/coordinator RSS was outside that sampled scope.
**Independent five-update FNV science gate passed.** The original private validator
first missed imported utility bindings, then omitted resident flag bit32 from its
exact `header.version` check. Original execution declaration SHA-256
`05b3b66048807eeb8b6da1ad0d043add81904af39ef3467a0f2e7b5dcd879d2b`
is immutable; `A4-verify-resident-probe.pre-wire-version-fix.py` preserves the
old verifier. `A4-resident-probe-verification-declarations.json` is an honest
**postrun verification-dependency/protocol/source-contract supplement**, not a
prelaunch declaration or changed native producer/numerical acceptance.
Corrected verification reached all43 final fields exact versus first-file, then
failed an overbroad original-source-TAR assertion on `eng.radii`. Source tracing
established preview-forward priming before `prior_stats`: runtime statistics
must match the initialized first-file native reference, not original TAR.
The established old-stage contract compares only immutable source appearance
(`eng.bg.`, `eng.bg_sky.`, `eng.ppisp` prefixes) to original TAR; all43 fields,
including all4 statistics, remain mandatory exact first-file comparisons.
The validator also corrects the invalid assumption that nonfinite-mean rows are
optimizer-frozen: exclusion is spatial ranking only. All **7,150** inactive
memberships/IDs and means remain unchanged; native-expected trainable changes
affect quats **7,113**, scales **7,150**, opacity **7,149**, DC **6,927** rows.
All43 final fields still match first-file byte-exact; native TAR/PLY full SHA,
stable IDs and bias clocks pass.
`A4-resident-owned-fnv-oracle.verification.json` is **57,125 B**, SHA-256
`ee7367f55e7b31fabae21cef8b8064a477711c63af0b15520ef89a938ddcd51b`.
Independent verifier exited0 without GPU reruns. Resource screening reads parent
cap log plus both actual worker backend-profile logs, covering all3 observed native
PIDs at24GiB backing/no allocation failures/maxima within cap, not physical peaks.
These are scientific smoke gates, **not benchmark prices**: smoke has20 forwards
with extra RGB/loss/native oracles versus11 in accepted `--fpbo-timing`; no
`--fpbo-timing-bench` switch. Current ordinary CLI8 outcome is recorded below.
FNV full lossless retention is complete: packet ZIP **381,724 B**, SHA-256
`b0caf33fb2d81df591ad65c2dab8b7bb55cc7775666d7b5c330767dc27026c4f`;
`.resident-retention.science-retention.json` binds all5 isolated full-SHA proofs.
Original source → first-file packet → target packet/native TAR explicit acyclic
restore chain; raw packet/TAR retired only after proof, logs/read-only PLY aliases
retained. Cutover reclaimed **1,163,980,096 B** in that phase, including temporary
base packet; prior PLY/TAR dedupe **489,432,054 / 380,013,056 B** are separate
phases, not an unqualified net sum. After-cutover unique-inode/free metadata
**6,810,030,534 / 16,293,023,744 B**, not physical peaks or current admission.
SHA then ran with prelaunch `A4-resident-sha-execution-declaration.json` successor
(**11,383 B**, SHA-256 `9d6432c2d7aa8a533a5bd5186e109fb70e7eb95f5a02bc2fe75ec973d85f0b57`);
earlier3d9 prelaunch version was not executed, original FNV05b immutable.
Fresh prelaunch encoded-input receipt covers all1,416 SHA/size entries,
**3,484,499,427 B**. SHA native exit0/stopNone, expected refusal child1,
wall **37.8549575 s**, sampled owned-tree-plus-monitor RSS **7,819,489,280 B**,
free minimum **14,651,187,200 B**, new-live maximum **8,451,616,258 B**.
Complete SHA scientific receipt requires independent SHA64 integrity and all43,
native TAR/PLY, IDs/clocks/inactive membership exact; no quality/Q/A5/R3 fix.
SHA independent verifier exited0 (**4.06 s**); full lossless retention now complete:
packet ZIP **381,740 B**, SHA-256
`ba1e150dd73cfacb303745b2d8af2f397aa01f51e6dbf38899b64a12f415c815`;
science union **6,743 B**, SHA-256
`7967f80ea2e85093f3ee28d784de418cafe83c84f515c589603b36ce7cc75ece`.
Full5 isolated proofs precede raw retirement; logs/read-only aliases and acyclic
dependencies preserved. SHA retire-phase reclaimed **1,163,980,096 B**, including
temporary base; after unique-inode/free **6,810,693,954 / 16,291,020,800 B**,
metadata not physical peaks or fresh admission.
**Six-arm resident benchmark batch complete, candidate not selected.**
`A4-resident-bench-execution-declaration.json` sealed35,882B/`cc124282…`
froze alternating FNV1/SHA1/FNV2/SHA2/FNV3/SHA3 with accepted `--fpbo-timing`.
All6 independent science gates and full5 isolated restoration proofs/cutovers
passed; all raw retirements followed proof, read-only PLY/logs retained.
Batch coordinator exit0 **349.06 s**, all59 helper commands0.
`A4-resident-bench-causal-report.json` **49,823 B**, SHA-256
`6a1acb8aa3b59d893cbb577090612829185381783584ec8afc989c25a3ed988e`
binds current three-repeat medians (seconds):

| Current resident hash-only discriminator | FNV | SHA256-64 |
| --- | ---: | ---: |
| Complete internal native wall | 30.58593 | 26.241224 |
| Measured work | 9.5604241 | 7.4451034 |
| Parent packet hashing | 6.511663 | 3.4801411 |
| Worker packet hashing | 5.9528755 | 3.3636145 |
| Capture/restore/priming | 7.4842896 | 7.5065947 |
| Upload/update/readback | 2.7783921 | 2.8103114 |

Within this same-stage hash-only comparison, SHA reduces complete/measured wall
**14.2% / 22.1%**. Hash/copy are overlapping wall subsets, not additive.
Do not assign combined new protocol versus old evolving-source matrix difference
to hash or residency alone; no practical speed, quality/Q/A5 qualification.
Actual ordinary CLI8 native run now returned expected **42**, monitor STOP null;
sampled monitor wall **11.8680671 s**, not internal native process wall.
RSS/free/new-live **854,286,336 / 15,023,972,352 / 8,072,395,377 B**.
Corrected independent ordinary verifier exited0 **2.06 s**, no native rerun.
The original helper's flat1D NPY/semantic `(n,3)` error was postrun verifier-only;
old helper/executed61ba declaration preserved, correction provenance separately
bound. `A4-ordinary-native-price8.receipt.json` verifies source horizon/config,
all1M clocks+8, frozen shared appearance, sidecar included/excluded union1M,
caps and unchanged7,150 nonfinite memberships. Application backing **1,883,824,608 B**,
not physical GPU peak. Included export IDs **986,920→986,642**, excluded
**13,080→13,358** are serialization membership, **not lost population/topology**.
Actual input/face IDs **590,123,982,794,1117,758,510,424**, pixels **99,002,304**,
versus G1 frozen IDs0..7/pixels **97,542,144**: **not same-input/pixel/exposure
equivalent**. One-arm11.8680671s actual CLI price is not a median; **no26.24/11.87
speed-ratio qualification**. Native lossless archival is now complete (below);
current producer custody has passed; bounded once-versus-prime implementation
completed bounded discrimination/source reset; numerical equivalence not established.
Original ordinary-retention preparation passed; its whole-offset XOR TAR admission
then refused **before archive/prune**, projected **719,788,787 B** at live/free
**8,072,889,705 / 15,015,772,160 B**; resource refusal, not native failure.
Separate `A4-ordinary-native-retention-aligned.py` reuses existing exact TAR
record byte alignment (`c.tar_layout`/`c.recipe`), preserving original prepared
helper/contract/recipe. Later full ordinary scientific union is **lossless**:
`A4-ordinary-native-price8.ordinary-retention-remaining.closed.json` binds all4
full isolated proofs and two TAR/NPY consumer screens; configs/IDs/logs preserved.
Initial TAR ZIP **373,995 B**, SHA-256
`b09702b2a65ee59c9062bbd6962433d96ae7c504445e2dc55f45abde3e1e7fff`;
final TAR ZIP **171,592,356 B**, SHA-256
`dcacacfbd6295b3198ee02f5b986f69cdf454e7099d43b9ad653ffb1f0f6b4a2`;
final PLY ZIP **218,984,974 B**, SHA-256
`cde7f22b041bc9bdfca47167c278efab75f83ad12391d7df87ef7715db8c3c25`.
Initial PLY remains exact read-only source alias with full-copy SHA proof.
Restore original TAR→initial/final TAR whole opaque NAME-aligned records,
original read-only PLY/exact alias→final opaque PLY XOR; raw targets retired
only after full isolated SHA/proven union, not sampling.
Both earlier packaging failures retained: offset-XOR cap refusal and final-PLY
missing reserved original-TAR anchor grammar failure, each before archive/prune;
future helper/contract corrected anchor only, actual PLY reference unchanged.
Current57 producer custody preflight refused mutable future-plan binding **before
phase write**; original helper/declaration preserved. Successor excludes only
that mutable future plan;95 source/executable and executed contracts/runtime DLL
bindings unchanged. Successor exit0 **10.86 s**: all96 members (95 source+literal57
executable /92,567,017B) streaming ZIP SHA/full isolated restores exact.
Restored real FNV/SHA CPU packet consumers exited0 **0.1418754 / 0.0609911 s**,
stderr0. `A4-current-producer-custody.record.json` **85,797 B**, SHA-256
`92f7254e3716ae5f9112201272f5c6673a6bc5b4a95a55a86da25a363acfd28e`.
Only own96 restored files/empty dirs pruned after proof; source/exe untouched,
read-only ZIP retained; System32 DLL bindings external, not portable redistributable.
Post-prune live/free **7,487,701,224 / 15,843,753,984 B**, metadata not RSS/peaks.
`A4-once-backward-implementation-admission.json` admits **implementation only**,
live/free **7,487,784,820 / 15,843,594,240 B** and bound **427,108,864 B**
=(67,108,864 records +360,000,000 largest SH field pair), no new checkpoint;
native reset uses existing TAR/member-digest descriptors/runtime initialized
baseline, no new PLY serializer claim.135 updates prepared, not executed.
Reconciliation source gap (native owner-pair→all-owned full checkpoint assembler)
is not a family rejection; A3 barrier subtraction spans nonidentical scopes,
**not a rigorous new5k+5k economic bound**. Conditional families remain open.
Once-native versus primed implementation **landed**, only in `EngineDiagnostic.h`,
`EngineDiagnostic.cpp`, `g1_native_proof.cpp`: explicit third capture boolean,
all3 old callers migrated, original four-arm `--native-mask-check` and
`source_capture` untouched. New private `--once-vs-prime-check`: roles2/0/1,
endpoints1/8, five arms/135 updates/54 primes/54 captures. Protocol
`A4-once-backward-implementation-protocol.json` and private CPU verifier prepared,
static review closed; actual native old-mask/once/independent consumer completed (below).
No TrainStep/Loss/Optim/kernels/sampler changes.
`A4-once-backward-build.receipt.json` records supported dev Vulkan exit0
**13.22 s/59 actions**, headless **17/17/13.44 s**, CPU FNV/SHA packet checks0
(combined13.64s). Earlier relative-batch invocation exit1 before compile retained;
absolute entry/forward-slash cwd corrected tooling, not native code.
New proof **88,207,360 B** SHA-256
`00c43bf7d228d1ba9ca4142535f728e96485fb7c68f0521c5609f8fa931ca6c6`,
application100,905,984B SHA-256
`e2a39903af793ec73d67dde9190266b386b05efeb6827a082da8e065b31107cd`.
These replace literal57/739 **after their complete custody**, not old science
producers. `A4-once-backward-review.receipt.json` closes static completeness/
resource and JSON-Path/transient Avg2M8MB field23 consumer fixes; versions preserved,
no runtime proof or unchanged-binary claim.
Fresh producer/build/source/input/resource binding/admission required before launch.
Preserved old mask actually exited0/resource accepted:32 updates/same8 inputs,
restored source/role/footer invariants; **six** B/owner0/owner1 summaries at1/8,
all invariant errors0, native wall **67.2240675 s**, wrapper72.46s
(`A4-original-native-mask-source.receipt.json`), not source-repeat qualification.
Actual once native exit0/noSTOP/resource accepted; wall **849.9833316 s**
(wrapper855.23s), scope3 RSS owned native+monitor+declared coordinator:
**4,947,775,488 B**, free_min **15,768,514,560 B**, live_max **7,527,544,902 B**,
commit_min **27,551,854,592 B**, backing peak **2,119,824,608 B**/24GiB,
failures0; sampled/accounted not physical peaks.
Independent consumer0 **6.03 s**, original28NPY/source reset+42runtime fields+
CPU implicit43ID exact, but **ordinary_repeat_exact=false,
capture_equivalence_established=false, all_decoded_screens_pass=false**.
Reset is not serializer/PLY parity; CPU0 means complete negative evidence,
**not qualification or unique source priming cause**.
At owner2/end1 N_A/N_B forward RGB/depth/Ts/projection IDs/stats exact;
ordinary backward weighted-loss **2 bytes**, screen **312,625 bytes**, accum-weight
**57,480 bytes** already differed **before auxiliary FPBO capture**. Repeat
variability defeats capture/priming attribution; mixed unchanged decoded1e-4/
nonfinite endpoint screens do not waive original Q.
Unchanged private consumer boundary0 **65.98 s/22 cases** validates actual
consistent-negative baseline/read-only targeted JSON substitutions/genuine binary
hash reads/no fake scientific files; validator boundary, not numerical success.
Next candidate-specific contemporaneous controls need own repeatability/capture
gates before attribution; A4 active/no A5/A6/family closure.
`A4-once-vs-prime-source.run.json` and `.verification.json` bind actual evidence;
`A4-once-consumer-boundary-smoke.json` binds the separately exercised22-case screen.
Exact pre-capture forward/loss/pixel/screen/stats and endpoint gates preserve
native criteria; no reference-gradient substitution, no asserted priming cause.
Frozen-PPISP scout maps discarded parameter VJP versus required RGB gradient;
profile48.4% stage is **not removable fraction**. No family/Q/A5 closure.
**Once producer custody now complete before the new PPISP build**:
`A4-once-producer-custody.record.json` full **1,169-member** isolated SHA restoration,
four genuine restored CPU consumers0 (proof FNV/SHA and app main/train help);
ZIP **64,239,526 B**, SHA-256
`1c66329495fe225d8c95f8e3065558b86a665cb4f94d00be28a8bd604ce78075`.
Failed bg92 protection preflight1 is retained: mutable navigation index was
actually sealed as input169,641B/SHA-256
`c167b8c2c3441a1a18e5340856a0706cd1e76aed9c10ae46c80fa1c8abd3b3d0`.
Exact CRLF literal recovered by reversing actual edits, accepted only after
matching complete bytes/SHA; retained read-only
`continuation.manifest.sealed-input-c167.recovered-literal.json`.
Successor explicit original-path→literal alias preserves original science binding;
current cumulative index **not rewound/rebound**. New seals must bind an
**immutable literal index**, never the mutable campaign/navigation index.
Only own restoration files pruned; old00/e2 source/producers/science immutable.

**Historical operator1 implementation/build and failed component (superseded by
operator2 common-pipeline evidence below, not retroactively passed):**

Frozen-PPISP parameter-gradient omission **implemented/reviewed/build+CPU passed;
first real AMD component crashed, equivalence/timing not established**: engine>=0
passes null parameter gradient while retaining canonical RGB VJP,88B ABI,
unchanged3-axis regularizers and new fourth image specialization boolean.
Legacy CUDA behavior unchanged; no CUDA validation. Private probe borrows actual
one-ordinary-backward pre-PPISP RGB/cotangent and replays both modes; full18-field
shared-frozen checks must cover source owner2, not rely on B1 verifier early return.
`A4-ppisp-build.receipt.json`: supported dev Vulkan0 **88.14s**, regenerated1/20
`PixelWise.cuh`; headless **17/17/13.19s**, FNV/SHA packet0 (combined13.42s).
New proof88,237,568B
`aafa3ddfd1366371965d6dd5459e9625dd5b4de352ee8f42061adafdc4e7167d`,
app100,907,008B
`b01fd06f1fa2b093e9bfd261bfbcd2f9634bee71b891c98aa8c8d732cfe44e61`.
These are new build producers, **not retroactive once00/e2 science producers**.
No GPU null-output/RGB equivalence, measured removable48.4% fraction, speed/Q/A5
qualification. Safe PLY headroom custody now **all four phases closed** (below).
**Historical v1 failure snapshot below; its unknown-exception status is superseded
by first-chance localization and version2 component evidence afterward.**
Actual PPISP shell prelaunch127/bg101 retained; bg102 feature/heap/seal0 then
pre-Popen declaration category refusal: path dedup suppressed direct source/config
semantic rows. Explicit-source-binding successor static review closed, old
helpers/declaration/failure preserved; bg103 fresh admission0/seal0 launched
real `ppisp_parity.exe` PID14908: **3221226505 (0xC0000409)** after
**0.644753899978241 s**, no dump/no numerical evidence; monitor resource accepted
**false** (`A4-ppisp-source-binding-parity-a.dump.bin.run.json`).
Root exit observed; WerFault child0. Generic owned-PID-exit-coverage assertion is
secondary monitor failure, **not native crash cause**. Stderr budget highwater
67MB/zero allocation failures is not a locator (64MB reporting threshold).
Cause unknown; static four-specialization overflow theory refuted by SpecMax16,
no actionable source bug established/no sourceCPP/math mutation.
Initial debug target10416 stayed suspended and was explicitly aborted before
user-code/GPU launch after unavailable lldb-dap refusal; receipts/helper preserved.
Private codelldb successor preparation is not successful exception localization.
No component RGB-equivalence/source capture/replay/timing/quality/Q approval.
Four PLY custody phases bg96–99 exit0/full isolated SHA+stride/count/EOF consumers
and prior scientific-union protection closed; **38 own aliases** retired,
aggregate logical payload savings **354,548,345 B**, **not physical savings**.
`A4-ply-headroom.aggregate.receipt.json`: live **7,252,222,127 B**,
free **15,260,786,688 B** at receipt, refresh before next admission.
Exact opaque ZIP recompact helper is static-only/prepared, not executed/retired.
Debug native cause before component gates; A4 active/Q unchanged/A5–A6 unrun.
**Actual v1 native fault localized** (`A4-ppisp-native-fault-localization.receipt.json`):
bg107 Win32 first-chance C++ **0xE06D7363**; actual raw ASCII
**`nullable PPISP RGB exact parity failed`**. Ordinary0xC0000409 was an uncaught
native parity assertion, not established GPU-driver/stack-buffer failure.
Mathematical/compiler rounding cause remains **[INFERENCE]**, not diagnosed.
Failed producer custody bg108 full **1,216-member** isolated SHA/four actual CPU
consumers0 (**55.91s**), archive **91,124,218B**
`eb85f029241b1202c0d5c50d909c50f9d2808974116ca7407c8fdc9928b9f47e`;
bg109 own-restore-only prune0/**9.64s** completed **before source cut**.

**Current operator producer2 common-pipeline runtime-null successor**: original
three image specialization axes, uniform runtime null parameter-VJP reduction-only
guard; applied source/execution static review closed. No retrospective v1 PASS.
`A4-ppisp-runtime-null-build.receipt.json`: dev Vulkan0/**33s**, generated0/20,
800SPIR-V blobs; headless17/17/**13.68s**, FNV/SHA0, combined13.88s.
Current proof88,526,336B/
`e1e73e3cfc9e908a0ce8e0d13b7a60ca7a8bf30b5dbb40ab7510b6cf9abca588`,
app101,196,288B/
`849edb8128876a2a17e881153a5c2074243d716a4a5c19bacc4cf5e28335e286`,
parity86,319,616B/
`5e9258121f7b6c5d9594e87f4bd8d96091e1679d42fecf19d4b1129b05876d3c`.
Private prepbg1120/.44s copied read-only independent literal indexes, **no live
navigation-index execution binding**. bg113 actual fresh feature/heap/1416source/
seal gates+consumer rejection selfcheck0+first AMD native exact six-layout
component/fullCPU0 (**34.63s**);
`A4-ppisp-runtime-null.parity-a.cpu.json` **accepted=true**, dump566,632B/
`3092f79f11a1970fd9808af3e13afbfb961d864e09328609b518ff94e19bf42f`.
This is direct component evidence **not genuine source capture/replay or price**.
Second AMD bg114 now **native/CPU acceptedtrue0/33.26s**, exact six-layout direct
component/fresh1416source admission (`A4-ppisp-runtime-null.parity-b.cpu.json`),
dump566,632B SHA-256
`ee9f1ddbe3b36e3ee39adcf42f5f357bd69c48cd248ff139aca3d25047498200`.
Both supported AMD direct unit cases bitwise pass; actual native source one-step,
capture/replay/timing/Q long recovery remain unverified/FAILED. Resource snapshot after
cleanup/prep root7,409,091,822B/free15,053,565,952B, not physical peaks.
Oldb01 price proposal permanently blocked; monitorv3 post-exit custody hashing
exclusion applies only prospective price wall, no retrospective old timing change.
Native default/A4 active/A5–A6 unrun/conditional families not exhausted.
**Historical pre-retention chain snapshot below; pending-probe/physical ZIP/lease
states are superseded by actual closed reconstruction and cutovers afterward.**
**Whole-chain admission/negative retention scope:** actual read-only CPU analysis
finds a capture-only reserve may pass but strand archive/full isolated restore/
replays. Source-derived `[B=1,H=3060,W=4080]` packet **299,678,937B**, potential
full RGB failure pair **299,635,200B**; root **7,411,081,895B**/free15,048,810,496B,
cap8,589,934,592B at analysis. Parent actively pursues exact-bit retention and a
new whole-chain reservation, **not a permanent external host-capacity blocker**.
Reviewed DC3+SH45 appearance-key-aligned literal smoke bg115 actually **failed1/
13.48s**, zero exact key matches at encode117; **old208,123,194B ZIP intact**,
no proof/prune/capacity gain. Failed partial `A4-ply-aligned-literal.smoke.zip`
**209,904,979B**/
`5b536d0310025d0699cccee5c698c3be990709f78edb12e379d5585f584995cc`,
active lease/journal preserved, not science success or a backwards-fabricated
ZIP recipe. CPU six-key whole-target probe bg116 running/no result at this update.
Typed literal-custody supersession resolver static preparation only; old component
seals never rewritten. Any old opaque prune needs actual full original ZIP+PLY
isolated restoration, full62float **real Python** consumer and ZIP stream SHA;
no C++ PLY consumer claim.
**Actual retained-key/geometry probes and exact-custody successors**:
bg116 CPU source/all3target SHA/EOF0/**27.82s**; full DC3+SH45/SH45 matches0,
DC3high16 coverage676,862/652,929/546,544 target rows: heuristic only/no lineage.
bg1170/**16.13s** fully independently reconstructed failed209,904,979B original ZIP/
SHA5b536... **before** temporary original/isolated release; closed failed recipe
retains exact old identity, **no science upgrade**.
Smoke DC16 bg1190/**44.01s** full original ZIP+PLY/real Python62float consumers/
positive custody/owned retirement/release: old208,123,194B literal exact restored,
current191,648,577B/SHA
`4600b34558599c75e331e6e31cefc1de11bb38990c6cff5a7006eae63a920ec0`,
logical gain **16,474,617B** (`A4-ply-aligned-dc16.smoke.released.json`).
Geometry bg1200/**19.43s** source/canonical/baseline SHA/EOF;
XYZhigh16 matches919,910/986,752 and872,032/986,323, sourceunique790,508,
biggestbucket45; heuristic/no row lineage or compression claim.
Canonical XYZ16 bg1240/**48.86s** complete original ZIP+fullPLY consumers/
positive custody/owned retirement/release: old208,114,077B exact restored,
current169,828,375B/
`ef656071d0437683263ae025916e64dbbbbfb5a1b34610d3ea4c9c51b4d0936f`,
logical gain **38,285,702B** (`A4-ply-aligned-xyz16.canonical.released.json`).
Both net **54,760,319 logicalB**, **not physical allocation**. Old smoke/canonical
ZIPs now **reconstructable historical identities, not physically present**;
full proof/closed/journals/helper/runtime/source dependencies retained. Baseline
old208,021,752B ZIP still physical unchanged. Root7,357,673,406/free15,056,322,560B
after release; source capture unrun. Complete worst-negative capture+archive+
isolated restoration reserve **1,299,400,843 newB** still insufficient including
ready16MB. Baseline reversible uint8 byte-plane XYZ encoding static prepared,
pending actual; **no assumed compression gain/permanent external blocker**.

**Historical bg125/bg127 synthetic failures (kept FAILED; corrected acceptance below):**
bg1254KiB wrapper1/.48s mutex refusal before primitive (parent concurrent custody);
only known throwaway files created before lock. bg1260/.35s explicit owned pair
recovery cleanupcomplete=true/**accepted=false**, no primitive proof.
Sequential v5 bg127 primitive fullbits/coalesce/aliasprune completed per journal,
but whole smoke1/.37s cleanup guard failed: both files gone/two owned empty
directories retain ID/birth/nlink, attributes0x10000010→0x10 after own operations.
v6 semantic stable-directory guard/exact recovery under review, **no accepted
primitive receipt/native source work**. Private source-v4 static closed but typed
custody subset graph/worst-negative peak/atomic coalesce still pending real smoke.
Source-C2 read-only scout confirms homogeneous native batch/camera-loop seam;
SCR2 install transport missing, not family rejection. Same-batch2 oneclock/update
comparison mandatory;56MB/view112MB/two before other fields, ordinary8 not proved
homogeneous. No C2 implementation/native/test evidence.
**Current corrected primitive/baseline custody/certificate milestone**:
fresh OS coordinator PID5628 confirmed; bg129 exact two empty-directory recovery0/
.34s cleanuptrue/**accepted=false**, not primitive science.
bg130 corrected actual4KiB Win32 atomic coalesce/prune0/**.37s**,
`A4-ppisp-alias-smoke-v4.receipt.json` **accepted=true** and owned synthetic
cleanupcomplete=true, **not native source capture**; oldbg125/bg127 not relabeled.
Baseline XYZ16 uint8 byte-plane bg1310/**49.01s** fullPLY62float/EOF+complete
old opaque ZIP restoration/unchanged CPU consumers/owned retirement/release:
old208,021,752B literal reconstructed; current **182,565,961B**/
`c1e7f2a528aaaacb3c507e2100084ab43945410533f2ee55b79f743c20981c73`,
logical gain **25,455,791B** (`A4-ply-aligned-xyz16-plane.baseline.released.json`).
All three new gains total **80,216,110 logicalB**, not physical; old baseline
ZIP now exact-reconstructable historical identity, **not physically present**.
Root **7,332,734,782B**/free **15,077,937,152B** at bg131 end.
Typed end-to-end private v5 static review closed (source9/newprivate9/primitive1,
runpy import/stale future-v4 routes repaired); bg137 actual certificate0/**10.43s**
`A4-ppisp-runtime-null-v5.custody.json` binds all3 released typedformats/source/
runtime/complete consumer proofs, **no graph bypass**; only packet-alias primitive
intentionally v4. bg138 actual v5 preparation **FAILED1/46.40s** at full worst-
negative **1,299,400,843B +16MiB prep** admission against8GiB live/10GiB free,
**before readiness creation**. No native capture/replays/price. At least65MB more
exact lossless retention research pending; **no budget waiver or assumed gain**.
C2 actual scout pair **[590,794]**, camera5, identical4080x3060 OPENCV params,
JPEG sizes/EXIF1. Native multiscale loss uses **batch-global raw row0 normalizer**
for all terms except DepthSup; cannot claim exact independent *.5 gradient or
per-image NumTrainImages=2. Minimum gate: genuine native **C=2 batch screen**
export/import **before one update**. Independent render/loss orchestration needs
common batch raw stats or exact render join into existing C2loss. Read-only scout,
no C2 implementation/native/test evidence; source/Q/A5/A6 status unchanged.
**Actual reference-codec samples, no retirement/full-archive projection**:
bg139 v1 probe failed1/**9.91s**, unhashable bytearray key line75; no accepted
result/retirement, old readonly helper/archives retained. V2 freezes full rowbytes
once before lookup; static review corrected (.98, v1 miss acknowledged).
Actual bg140 `A4-ply-reference-codec-probe-v2.parent.json` full3currenttarget
decoder SHA/EOF true, caches only3×4096 first/middle/last records per target.
**No observed wall supplied; none invented.** Root7,333,286,865/free15,058,468,864B,
1MiBmetadata+8MiBreceipt reserve; measured component peak working511,430,656B/
commit1,078,534,144/private971,403,264B, **not whole-machine peaks**.
Smoke XYZ16 nearest-ordinal byte-plane **XZ6/CRC64** sizes
**595396/582244/577316B** versus gzip6 **651817/630412/625286B**.
Closest-reference changes8/5/9 negligible/non-dominant; do not pursue nearest-
neighbor/spatial heuristic smoke. Canonical nearestplaneXZ607556/590824/587432B;
baseline nearest672392/736784/647352 versus closest662644/660224/647652B,
material closest-reference benefit concentrated in middle sample; **no full-archive savings extrapolation**.
First full smoke nearestordinal byteplane+stdlibXZ6 candidate **prepared/unrun**;
whole currentDC16 ZIP and originalopaqueZIP/fullnativePLY/real consumers must
restore before separate C6 authorization/retirement. C5 intentionally rejects new
format; future typedC6 private routes under static review, transitive restore/
atomic-retirement-publication-recovery defects repaired pending closure.
**No C6 AUTH/candidate execution/cutover/source GPU work.**
C2 Gate0/1 private record+patch **176,397B** (4files EngineDiagnostic.h/cpp,
EngineDataManager.cpp/G1harness), **not applied/compiled/executed**.
Source all-owned B1 COMMON freeze/one C2 update; no independent-rendererGate2
claim. Initial worst-negative4,013,365,879B impossible current cap; minimal
lossless negative-fixture literal-patch retention/full independent restoration/
consumer before prune is a **prepared, unqualified successor**, avoiding six
redundant124MB copies, **not reduced scientific coverage**. SourceCPP9/binaries
unchanged; PPISP source step/replays/price/A5–A6 unrun/Q FAILED/native default.
**Actual C6 full smoke transition, still no source capture/admission**:
`A4-ppisp-c6-actual-transition-parent.receipt.json` binds seven read-only
transition receipts and unchanged source9/binaries. bg1470/**120.85s** full XZ6
smoke **140,088,194B**/
`68f7b9e77049965a5ce6b951891eb329ea20c47f5567083fed0096b1f6409a43`
versus old currentDC16ZIP191,648,577B/4600...; gain **51,560,383 logicalB**,
**70million margin target NOT met**, no physical/capture admission claim.
bg1490/**60.85s** full nativePLY244,723,715B/9c3c...+current191M ZIP+original
opaque208,123,194B/139c... exact isolated restoration/real CPU62float consumer
all986,783rows/source986,920, olddecoder/fullZIP/prior seals unchanged; all owned
workspace absent. bg150 C6AUTH0/**3.94s**, bg151 exact authorized current ZIP
retirement/release0/**22.93s** only matching owned identity; retired exact path
absent, historical originals unchanged/exact reconstructable.
bg152 typed all3phase C6 graph certificate0/**51.65s**,506,928B/
`0c37cff1a435ac6f61e1a62c640ab285401c0c9e15f47131d59ce02e0c665fa5`.
bg153 v6 readiness **timed out120.13s/deadline120**, **no exit observed** and
**no resource/scientific refusal verdict observed**; six readiness paths absent.
One post-completion native-process query0/stdout''/stderr'' found helper gone.
Fresh logical root7,285,382,648/free14,838,755,328B; fullnegative1,299,400,843+
16MiB total8,601,560,707 exceeds8GiB cap **11,626,115B before parent5110B receipt**.
System commit separately32,730,624,000 available; **not physical/machine peak**.
Do not label timeout the old line63 guard, or infer actual historical-route restores
from graph certificate/hash/static review. One further whole **CANONICAL XZ6**
candidate authorized to measure, C7 private fixed new case **preparing/unrun**;
actual positive ≥approximately20MB margin+full dual proof+C7AUTH before **any**
retire, no projection/coverage/cap waiver. No source proof/replay/price/speed/Q.

C2 private **v3** Gate0/1 complete review correct.97/no findings, prior v1/v2
ineligible preserved. Patch55,676B/
`494e3ff218f792bb6917cef9b64f3aeb82b7ed5558053b02dcfa3f33c1b21dc7`,
implementation166,520B/
`980c42fcee62adb017a68df4821b58198a984e67faca3436be83400939b5260c`.
Fixed shell-injected CPP predicates; actual1195train/0validation+separate132heldout
disjoint/no configchange; restores actual8MBaccum_weight via existing
NeverRasterBwdAccumWeight before stats. **Full native batch packet/Gate0/1
screen import before once-only optimizer**, not independent GPU renderer/full C2.
Worst phase **3,517,362,683B still NOT ADMITTED**; no apply/compile/runtime/
exception fixture executed. A4 active/native default/Q1e-4 FAILED/A5–A6 unrun.
**Actual C7 canonical transition/readiness, capture precomponent still timed out**:
readonly `A4-ppisp-c7-actual-transition-parent.receipt.json` binds nine receipts.
bg157 full candidate0/**146.80s**,143,045,926B/
`cbb40257aa1927339b23c08852c15879286d6560654b45369a266cf5fb0c940a`;
gain **26,782,449 logicalB** versus exact oldXYZ169,828,375B,
**20M target met**, not physical. bg1580/**83.82s** complete current169M+
originalopaque208,114,077B+nativePLY244,716,027B (986,752rows/source986,920)
real62float/olddecoder/allSHA/EOF/priorseals unchanged, full workspace release.
bg159AUTH0/**83.96s**, bg160 exact old canonical namespace retired/released0/
**310.56s**; logical root7,258,959,235/free14,854,090,752B at release.
bg161 actual C7 all3 certificate0/**389.64s**273,814B/
`9f8e3689397da8ec7abfb26f2559924c2f1a76b6616a1cc2e6bdc4e4e18299dc`.
bg162 preparation0/**2902.63s**, whole negative1,299,400,843+16MiB admission
passed, six read-only READY inputs/commands created, **not native source proof**.
Parser rejection selfcheckbg1630/.17s is pure CPU, not source science.
bg164 capture precomponent **TIMEOUT3600.12s/deadline3600**, no observed exit/
resource/numerical verdict; exact capture gate/seal/template output directory absent,
single native-process lookup0/no exact helper match. **No capture/replay/price in that C7 attempt**.
bg165 isolated unchanged3GiB hardware query0/.26s (probe elapsed.07964509999146685),
both supported AMD R9700 UUIDs/heap1 budget33,250,242,560B each/usage0;
**no kernel/train, no device peak, no historical-stop attribution**.
Actual static metadata534READYbindings/2651percomponent/3historical per component;
precomponent repeated loop12historical+headroom3+graph1 gives16 whole certified-
graph invocations. Mechanism established by source/metadata, **historical timeout
attribution [INFERENCE]**, not runtime profile. The following C8 preparation
snapshot is superseded operationally by the actual outcomes below: unchanged
C7 full graph once per phase, immutable physical inputs read-share pinned,
mutable/unknown checks fresh, same root mutex/source9/1416/binary/unit/header/
shape/archive/negative/resource guards; frozen C6/C7 seals/sourceCPP unchanged.

**Actual C8 operational capture completed; independent CPU contract gate failed**
(2026-10-03). Parent-clock wrapper durations/exits: guard smoke bg167 **0/14.95 s**
(internal **14.8039576 s**, stdout
`ACTUAL_C8_GUARD_REFUSALS_AND_SCOPED_REFERENCE_REUSE_PASS_NOT_SCIENCE`),
prepare bg168 **0/22.81 s**, capture precomponent bg169 **0/56.84 s**,
seal bg170 **0/61.53 s**, monitored capture bg171 **0/45.52 s**.
These are wrapper/guard durations, **not training acceleration or pricing**.
`A4-ppisp-runtime-null-v8.capture.sealed.json` **2,034,791 B**, SHA-256
`5cd0010ec9db9b14acb425e4d2497e2530c09f06db2bae6659a2e3eb7ee3c98c`,
binds the genuine ordinary all-owned source step10,000/horizon30,000 capture on
first AMD `uuid:00000000010000000000000000000000`, original frozen v2 proof
binary SHA-256 `e1e73e3cfc9e908a0ce8e0d13b7a60ca7a8bf30b5dbb40ab7510b6cf9abca588`.
`A4-ppisp-runtime-null-v8-capture.run.json` **2,906 B**, SHA-256
`76ca8eeba6411ca243ccea9e53eb62c01526c96994786c663ae1ab377a41058b`:
native monitored wall **11.631742 s**, exit0/STOP null/resource accepted.
Worker-accounted committed/reservation peaks both **2,214,788,880 B** within
unchanged24GiB; Scope3 sampled RSS **2,189,873,152 B**, free minimum
**14,540,980,224 B**, root unique-inode logical maximum **7,571,503,087 B**,
available commit minimum **28,391,497,728 B**. Sampled values are not true/physical
peaks; accounted application device backing is not physical VRAM peak.

CPU bg172 **exit1/29.41 s wrapper**, `ValueError: all slot phases` at
`A4-ppisp-runtime-null-verify-v7.py:114` through the C8 batch route.
Actual `A4-ppisp-runtime-null-v8-capture/ppisp-proof.jsonl` has28before/29after
source slots: solely after-only `eng.v_losses`, `<f4`, shape[14],56B, SHA-256
`36629f67edc533320e4c5ea8c810469d718db6f461d54cbccb7df9450f6a1f01`;
no before-only slot. This historical C8 slot-phase failure is distinct from the
subsequent C9 CPU failure: C9 static review correct/.9/no findings, lifecycle
selfcheck exit0/**0.08 s**; actual complete CPU bg173 **exit1/30.15 s wrapper**
passed the lifecycle gate, then original verifier line153
`strictload(originalsource/config.json)` rejected nonfinite JSON while checking
`raster.accum_weight` shape. The actual original config contains only positive
`Infinity` at `outlier_threshold` line46 and `max_world_size` line95.
The subsequent C10 successor preserved exact bound native legacy-config semantics:
no source/config edit or global JSON constant allowance. Historical C8/C9 failures
are helper metadata-contract bugs, not numerical negatives; superseded by the
following complete CPU receipt, not erased.
**Actual C10 CPU component green and full opaque restore, not qualification**:
static review correct/.95/no findings; exact lifecycle+positive-Infinity decoder
selfchecks exit0/**0.14 s**. bg174 actual same-capture full CPU **0/30.16 s wrapper**;
`A4-ppisp-runtime-null-v10-capture.cpu.json` **5,093 B**, SHA-256
`ff7fa3925f65b371ba54f0945d4e8ca59c5fabe2fe28b97f20ed82143278ab9d`,
classification `CPU_COMPLETE_COMPONENT_GREEN_NOT_QUALIFICATION`, accepted and
native_resource_accepted true. Source28/after29/baseline29 with exact
`eng.v_losses` scratch lifecycle; original config protected, only its two bound
Infinity values decoded; canonical43/frozen18/all1M clocks/original28/packet/
RGB/input/resources checked, no numerical negative.
bg175 retain **0/25.67 s wrapper**:
`A4-ppisp-runtime-null-v10-capture.opaque.restore.json` **1,529,216 B**, SHA-256
`8f9d69ad496b1342543ead85d579d20c9cac3b57a468bff46a6543bf6f823b77`,
classification `OPAQUE_PACKET_FULL_ARCHIVE_AND_ISOLATED_RESTORE_CPU_PASSED_NOT_NATIVE_REPLAY`,
full_isolated_byte_match true. Raw/restored packet **299,678,937 B**, SHA-256
`9dc007abd7aa13f379b509983be214bdce4828abe7064e8e16b4bfff714efa99`;
opaque ZIP **299,679,071 B**, SHA-256
`d7978ee346ae50f29be46b6c6d5be45e916bffd88ea2ba5e74fdc0e471e64962`.
Postphase root logical/free **8,170,958,301 / 13,937,745,920 B**, not physical
peaks or fresh admission. That C11 preparing snapshot is superseded below.
**Actual C11 same-packet two-AMD replay/CPU closure and accepted-only prune**:
parent review correct/.93, all6 binding hashes checked, actual read-only review
2,029B; selfcheck0/**0.11s**. bg176 accepted coalesce0/**27.06s** preserved the
raw name through full299MB coalescing, not merely4KiB.
Parent-clock wrapper outcomes: a admission bg1770/**56.91s**, seal bg1780/
**63.03s**, native bg1790/**37.10s**, CPU bg1800/**31.41s**; b admission
bg1810/**56.99s**, seal bg1820/**63.35s**, native bg1830/**38.10s**,
CPU bg1840/**31.74s**. Native monitored walls **2.3408269 / 2.4053818s**,
both exact supported AMDs/same isolated packet299,678,937B/
`9dc007abd7aa13f379b509983be214bdce4828abe7064e8e16b4bfff714efa99`,
STOP null/resource accepted/full CPU component green. Source-field/RGB bit-exact
complete gates exercised, **not quality/recovery qualification or pricing**.
Worker committed/reservation peaks both516,647,744B/failures0; sampled Scope3
RSS a/b1,153,929,216/1,281,597,440B, free minima14,227,869,696/14,221,381,632B,
root logical maxima7,878,425,057/7,883,978,047B; not true/physical peaks.
bg185 both-accepted-only prune0/**26.25s**:
`A4-ppisp-runtime-null-v10-capture.opaque.restore.json.prune.json`,
classification `RAW_PACKET_ALIAS_PRUNED_AFTER_FULL_ISOLATED_AND_BOTH_NATIVE_REPLAYS`.
Parent observed old owned raw name absent; restored packet and opaque ZIP remain
retained unchanged. The then-unrun producer custody/build/price snapshot is
superseded by the current milestone below; no historical failure is relabeled.
No caps/envelope/Q changes; A4 active/A5–A6 blocked/unrun/native default/Q FAILED.

### Current PPISP source microprice — 2026-10-03

`A4-ppisp-source-price-parent.receipt.json` (9,029 B, SHA-256
`4287fdb4e87cc793e499e93646b2a0dc2b395a41cba6ff33c058f0601bf2bcb9`)
records **both supported AMD native and independent full CPU acceptance**.
This is two single-packet **kernel-level microprices**, not the historical
six-arm resident-owned exchange prices, end-to-end training speed, statistical
significance, quality, numerical recovery, DCE verification or family exhaustion.
Native remains default; unchanged decoded `1e-4` Q recovery remains **FAILED**;
A4 remains **ACTIVE**, conditional families open, A5–A6 **UNRUN**.

The same genuine source-step10,000/horizon30,000 packet binds 1,000,000 rows,
1,195 training views, input590, B=1 and 4080×3060 pixels. Retained packet
299,678,937 B/SHA-256
`9dc007abd7aa13f379b509983be214bdce4828abe7064e8e16b4bfff714efa99`
and whole device inputs remained unchanged. Each device's 62 records cover
eight warm pairs and 32 alternating measured pairs (16 joint→RGB,16 RGB→joint).
Producer2 uses the same three-axis module/common pipeline and uniform runtime
null parameter-gradient selection; no new specialization or verified DCE.
Both joint/RGB full witnesses are **149,817,600 B each**, bit-exact to captured
SHA-256 `144f8599e7c228cd3872490e89d4e2d65120d45bdb2e542567d309ca380e2928`,
with **zero differing/nonfinite elements** on each device.

| Scope (seconds unless bytes) | AMD A | AMD B |
|---|---:|---:|
| Joint launcher+GPU-sync median | 0.1566842 | 0.15648645 |
| RGB-only launcher+GPU-sync median | 0.0017043 | 0.0017213 |
| Median of 32 paired joint/RGB ratios | 84.36302674146376 | 89.81663639290174 |
| Native monitored process wall | 12.8916501 | 13.0469134 |
| Monitor wrapper wall | 29.83 | 29.91 |
| Separate independent full CPU consumer wall | 17.20 | 17.44 |
| Sampled Scope3 RSS peak, B | 1,286,836,224 | 1,287,266,304 |
| Sampled free minimum, B | 11,857,498,112 | 11,857,678,336 |
| Sampled root logical maximum, B | 8,017,186,905 | 8,017,236,963 |

The ratio is **median of paired ratios, not ratio of separate medians**.
The clock is steady-clock launcher plus GPU synchronization, **not GPU timestamps**.
Parsing/upload/readback validation, first-warm cold pipeline, process startup,
monitor overhead and independent CPU work are separate scopes, not additive
kernel time or a full-wall speed ratio. Worker committed/reserved backing peaks
are **666,465,344 B each**, allocation failures0 against unchanged24GiB limits.
Sampled host12GiB/root8GiB/free10GiB gates passed; none establishes true physical
GPU, disk or machine peaks.

Full qualifying producer custody preceded the price-only source cutover:
`A4-ppisp-qualified-producer-custody-parent.receipt.json` binds ZIP
**105,032,130 B**, SHA-256
`be7a89c85e3ee32006e05ec1396448f7d1f5b1269d77dd16d6805dc9a3eaf7bf`,
all1,738 members/full isolated SHA comparisons and four actual restored CPU
consumers0. Readonly attributes19/24 were corrected only after full pre/post
archive SHA equality. Original whole owned-Job monitor **FAILED43** on a prune
FileNotFound race after the record/consumers; original acceptance remains false,
normal prune receipt absent. Distinct accepted recovery0/**20.387s** removed
421 surviving owned paths and reconciled1,409 already absent paths; Jobactive0.
Source archive, opaque ZIP and restored packet remain retained.

`A4-ppisp-source-price-postbuild.receipt.json` binds final supported dev Vulkan
build/guard0 (**29.174s**, Jobactive0,5,520 products, actual automated16 jobs),
headless **17/17/13.21s** and FNV/SHA packet contracts0 (**13.45s** combined).
Earlier native-build0/failed undersampled guard and later guard race43 remain
preserved negatives. Actual owned-Job/ephemeral-child regression gates passed;
no unsafe PID kill. Applied price-only C++ SHA-256
`3ce4c3f60df2f1a694a7eb0fb055ec6dab6b2a58ad128b2381eb547c05ec2d41`;
final proof binary SHA-256
`33103cee96d17e83b57be4691814017e2544a2b13c2ea8037a11208accab7352`.
Experimental command/scope: [testing](../testing.md#qualified-source-ppisp-microprice-experimental).

NTFS compression preserved full source-packet/opaque-ZIP SHA with allocation
248,348,672 B each (about51MB physical recovery each); root logical bytes did
not fall. Later root8,017,270,078 B leaves **572,664,514 B** under8GiB.
Unapplied **C2v3 full worst-negative3,517,362,683 B remains NOT ADMITTED**:
arithmetic gap2,944,698,169 B, not fresh admission or proved liveness savings.

### Current source-before representation proof — 2026-10-03

`A4-source-canonical-reference-v3.parent.receipt.json` (5,547 B/SHA-256
`288bb29f86fed7c4dbf2d92e510e761ad01f149133979909210cef565168b970`)
records guarded selfcheck0/**1.9978316s** and full CPU verify0/**2.3274019s**
wrapper walls (native Job verify wall **0.8698554s**). All43 source-before fields/
**383,983,064 canonical B** match the old43-field oracle `94ef2be4…`: direct
protected TAR `1986eada…`/original config `0ac7b6a4…`,28NPY members,
13 empty fields, eight zero step arrays and1M uint32LE stable IDs. CPU only,
raw duplicate0/retired0/**no C2 reservation credit**: no after-forward/backward/
update shapes, never-8MB claim, point-gradient proof, native GPU checkpoint
readback or image coverage. Full C2v3 **3,517,362,683B remains unapplied/unadmitted**.
V2 selfcheck0 but actual CPU2/fields0 **opened file ID/volume mismatch** remains
a negative identity-admission failure, not a science-field mismatch. Direct
Python3.14 full64-bit volume versus legacy Win32 DWORD32 caused the mismatch;
v3 mandatory FileIdInfo64-volume/128-fileID pin/collision selfchecks passed.
Initial v3 static three-key metadata schema mismatch was fixed before any seal;
there was no actual failed v3 run. Verify Job2/active0, native private commit
peak38,678,528B and separate conservative native working-set peak sum59,162,624B
passed128MiB excluding observer; sampled Scope3RSS449,646,592B/root8,017,920,612B/
free11,825,164,288B are not true physical peaks or future admission.

Next work is **private preparation only**: current price C++/binary full1,738-
closure custody before any source patch, plus an all1,195 explicit training-view
native `flatten_ids`(screen ID)→`gaussian_ids`(world ID)→installed B1 256-row
owner0/1 mask **tile-intersection candidate footprint** probe. Candidates are
a superset, not exact pixel contribution; old M0 per-row proxy is ineligible.
No footprint helper/GPU/source patch has run or landed. Even observed zero
would prove only zero Gaussian raster VJP, not permit skipping statistics,
regularizers, Adam/clocks or closing a schedule/family. A4 ACTIVE/A5–A6 UNRUN,
native default and Q decoded1e-4 FAILED remain unchanged.



Current top20 namespace metadata (before canonical retirement):1732paths/
1721inodes7,231,934,082B versus stable total7,285,453,368B; unidentified53,519,286B
below20th91,124,218B, so top20 known **not atomic/largehash/retirement authority/
physical peak**. Largest A3/A2/control/source archives are required custody,
not junk; exact sourceTAR/threePLYbases/compactbaseline/owner aliases native seals
still constrain retirement. Earlier approximately2.212GB/~2.8GB C2 gaps and
unapplied private PPISP price patch status are historical snapshots, superseded
by the current source microprice milestone above. C2v3 remains unchanged,
unapplied and unadmitted at its full3,517,362,683B negative bound.
Current A2/A3 done/A4 active/A5–A6 blocked phase; native default/Q FAILED/unsettled.
Separate literal application739 custody now closes its earlier dependency-only
gap: `A4-ordinary-app-producer-custody.record.json`, ZIP **31,503,187 B**,
SHA-256 `b872605e9bf1e57901d86bb14bf028982367b1218f866efe551e97ad75a4a1c5`;
whole100,905,984B executable/full isolated SHA restore and real restored main/train
help consumers0 **0.0994556 / 0.0380843 s**. First57 ZIP did **not** archive739;
old95 source reference remains read-only historical ZIP, not patched repository.
Nine System32 runtime DLLs external/not redistributable; no upcoming-build parity.
Conditional source scouts are not measurements: homogeneous C2 internal batch
seam exists, split/heterogeneous FPBO rejects, no external cotangent install API.
Packed screen+IDs+AABB minimum **56MB/view**, C2 **112MB before other fields**,
not80MB economic rejection; changed batch needs own loss/source/clock proof.
Actual B1 owners rank block finite-mean-Y medians/stable blockID, lower/upper half,
not random/alternating/per-row M0 proxy counts. Zero owned projection may support
zero Gaussian VJP conservatively only with all raster footprints captured;
stat/shared/native-zero regularization/Adam still required, skip unmeasured.

Earlier r2 file exited 0; complete/measured **54.3585746 / 23.940768 s** for three
pairs/six measured exposures, monitor **54.410333 s**. All43 native fields
exact versus r1 file, both native TARs exact first-file members; stable IDs,
clocks/source-shared and native readback exact. Sampled RSS/new-live/free
**5,517,271,040 / 8,459,993,000 / 15,310,594,048 B**, not true peaks.
`.cross-arm-isolated-proof.json`/`.science-retention.json`/`.archival-cutover.json`
prove complete serial packet/base/native-TAR union and all19 scientific hashes
before retiring raw target TARs/packet and temporary first-file packet.
Packet archive **381,702 B**, SHA-256
`797c713e200c61a449f31e99317191d34e5b09afabff84fb761c8127d07a0fe5`;
PLYs retain exact raw read-only hardlinks. Restore/install original source →
first-file packet → target packet or first-file native TARs → explicit r2 targets.
Post-cutover root/free **6,435,112,163 / 17,335,463,936 B** is point-in-time.
All subsequent frozen labels now completed; current decision is the diagnosis above.
Remaining r2 ordered/r3 ordered/file/native/mapped records each bind actual run,
verification, scientific retention and archival cutover; the manifest binds all12
and actual isolated proof references. Ordered r2/r3 TAR/PLY match first ordered;
file r3 matches first file; aliases require exact bytes, not a tolerance.
Mapped r3's cross-arm alias correctly refused differing fields without partial
output (`.hardlink-retention.json`); exact within-arm aliases were valid.
It therefore retains its own opaque packet/TAR archives and PLY archive
**116,709,385 B**, SHA-256
`35dc6541cd7ff88ce58c5515c60b8b9a06352854f2d752d6a6f8adaf77cd9fc4`,
after complete isolated packet, serial both-TAR and PLY proofs. Restore/install
source → first-file packet → r3 mapped packet → own native TARs; raw first-file
PLY → own r3 mapped owner0 PLY → exact owner1 alias. Native r3 similarly uses
its own packet/TAR/PLY archives, not numerical aliases: packet **6,631,075 B**,
SHA-256 `941c43828f44b9dd48c55ea815ccc77577cc08f48f79d6dfa056306158043b0f`,
PLY **7,741,094 B**, SHA-256
`dd15ae24ffb3f34594ded872c77430e5079264d3d39ceec499a792240739c97a`.
Restore/install source → r1 native packet → r3 native packet → own TAR;
raw first-file PLY → ordered PLY → r1 native PLY → r3 native PLY.
Current post-cutover root/free **6,691,798,111 / 17,076,609,024 B** is
point-in-time before doc/manifest edits, not fresh admission or true peaks.

`A4-finite-r1-file.archival-cutover.json` governs lossless retention:
packet **166,392,828 B**, SHA-256
`5aaa7f010a59abc0b4bcde28780bca826897a30ffe3dacdfbd0bd9e6fb8679a6`;
native **750,608 B**, SHA-256
`c2fc60aebc48bb5343378fd6aae1f90983383dbb3de2d86dac165b5d06cef29e`.
Packet **391,983,520 B** and both TARs **760,026,112 B** restored exactly:
two serial complete prefixes jointly cover the native archive; neither prefix
alone asserts all-files verification. Raw packet/TARs pruned; PLY/config/logs
retained read-only. **Restore/install packet before native archive decode.**
Ordered `.archival-cutover.json` now proves full isolated restores: native
**376,127 B**, SHA-256 `a9ed7a40227283e126bfbde1032bc8000c1a9661d334362b1cdddf6b9c99e20a`,
packet **175,042,754 B**, SHA-256
`e49b78963fd411c4cd64126ee724fca2421cb0410840bcdcf22520a33d337c4b`.
Raw ordered TAR/packet pruned, raw PLY retained; packet restore/install first.
Mapped archival now **complete**: `.archival-cutover.json`/`.science-retention.json`/
`.cross-arm-isolated-proof.json` bind packet **381,707 B**, SHA-256
`18a7aaedc5119d4f0978f0a1b2ac09c116af44514214424435260eb6c8c09bdf`,
first-file whole-packet XOR and explicit identical-native-member target aliases.
Serial complete packet/native-prefix union and all 19 science SHA bindings exact,
not simultaneous restoration. Raw mapped TARs/packet and temporary first-file
packet pruned; PLY paths retain exact read-only first-file hardlinks.
**Acyclic restore: original source → restore/install first-file packet → mapped
packet or first-file native members → explicit mapped targets.**
Unordered diagnostic-control full isolated restores passed: native **376,122 B**, SHA-256
`3a657309ebafbe7e3fa3a90f6c8d72bb5a67246afa1ccd45f3c7086215ff418d`,
packet **175,043,296 B**, SHA-256
`0bff412f0cf5f3c07f863a6fb5593e98a5092ffee553fa2e691a8bbc073713d0`.
Native `.science-retention.json` current `physical_cutover` now also binds PLY
archive **8,085,978 B**, SHA-256
`e012f59f803f9e7b405229b7c492c030097699fac159e0b5815b2eb08841041b`:
all **244,688,003 B** restored exact before native TAR/packet/PLY raw pruning.
Ordered PLY archival now also verified: **159,858,175 B**, SHA-256
`01d87c72ca59ea19279c9d4b93beac5b25f60d871bb043938f0a24a31783d5e1`;
all **244,688,003 B** isolated restoration exact before raw ordered PLY and
temporary canonical-packet pruning. **PLY restore DAG: raw first-file PLY →
restore/install ordered PLY → native PLY restore.** Source/first-file PLY remain
raw immutable references; A2 owner-base PLYs now use the proven archive below.
R2 mapped `.archival-cutover.json`/`.science-retention.json` bind packet
**381,708 B**, SHA-256
`74592c4880ae99b684af18d52b0ceff73e3fa983c38226b9e220159954553a5d`:
serial first-file/target packet and native-prefix union **760,026,112 B** plus
all19 science hashes exact. Raw r2 TARs/packet/temporary first-file packet pruned;
PLYs retain exact read-only aliases; same acyclic packet-first restore contract.
R2 native `.archival-cutover.json`/`.science-retention.json` bind full isolated
packet/native TAR/PLY restores, respectively **6,258,594 / 376,121 / 7,415,942 B**
archives, SHA-256 `9c360a01f8db478d194108adffb4ae6cde7aba57f8a5a4d22451ce7dd20f868c` /
`62f519274962a9e7a2c5fa5e2593d38a2fb7a444b20e0facdcbf566313f48bd1` /
`9f79ab7fb0d3cdd9e97c4ba70a3e8463524eb7ec67951e9a3123c5bd1f57b2e2`.
Raw target packet/TAR/PLY and restored r1 packet/ordered/native PLY dependencies
were pruned only after proof. **Packet DAG: source → restore/install r1 native
packet → restore/install r2 native packet → r2 TAR. PLY DAG: raw first-file
PLY → restore/install ordered PLY → restore/install r1 native PLY → r2 PLY.**

`A4-A2-peer-separated.science-successor.json` and `.physical-cutover.json`
now govern A2 partial-install custody; original compact/aligned migration/native
consumer records remain historical unchanged. All **71 main members /
1,253,628,038 B**, then both complete peers **74,016,345 / 74,064,993 B**,
restored serially to isolated disk with full SHA equality: exact **73-file /
1,401,709,376 B** science union, not simultaneous restoration or science edits.
Peer references are opaque exact owner-native byte segments, not a training codec.
Main/owner0/owner1 wrappers are **614,545,396 / 132,670 / 132,562 B**, SHA-256
`8a74c0950ebe7dd0ac4305fbdb5a1ebe8543ea3cf8fec413650989d8946744c7` /
`1148f20491550c406eedd224e65abb93169c0869129ebf2b4ea085a2fc37ef6a` /
`3649e347a38dcdb2bef4dcdc12bbc01d7df6cc90e25fcc69bf220c3348e983c6`.
The superseded **753,289,368 B** wrapper and raw baseowner PLYs were retired
only after complete proof. **Source + merged remain raw → restore/install
baseowner PLYs from `A4-base-ply-alias.compact.zip` (239,977 B, SHA-256
`68a1f02fabaf8e2b83567621fa06bb766f6cf6df4db8249d7266a61e0f04b63c`)
→ main decode → restore/install corresponding main native TAR → peer decode.**
Failed owner0 metadata `codec=deflate` versus actual gzip-XOR caused reconstructed
SHA failure; failed archive/declaration/proof remain retained. The helper now
declares explicit `gzip-xor`; complete both-peer proofs pass. Continuation manifest
binds successor, failure, helper and all member/union proofs by paths and hashes.
Peer-cutover root/free **6,434,410,120 / 17,338,482,688 B** was point-in-time only.
R2 file subsequently received conservative phase-based output admission and
completed as recorded above; frozen sequence and executed reservations unchanged.
No new quality, numerical
recovery, throughput qualification or release approval; A4–A6 remain incomplete.

`A4-finite-output-reservation-amendment.json` bounds pending output only using
exact 28-NPY inventory: controls **1.125 GiB**, file initially **2.5 GiB**.
`A4-finite-output-exact-sums-amendment.json` supersedes only r2/r3 mapped
upward rounding with the existing exact **1,754,968,480 B** output sum.
Initial/pre-exact-sums declarations and all four then-executed specs preserved;
all12 argv/env/source/bindings/order/science/acceptance/caps unchanged except
two pending projection fields. Output only, not host/VRAM/pagefile admission;
no retroactive admission or cap relaxation.
`A4-finite-file-phase-reservation-amendment.json` later changes only then-pending
r2/r3 file reservations **2,684,354,560 → 2,146,952,000 B** using source-bound
phase liveness: peak two checkpoint pairs + two canonical replies +100MiB logs;
op1/op2 upper **1,383,752,608 B**, final pairs/completed packet **1,754,968,480 B**.
Parent consumes both replies before completed-packet creation. Pre-file-phase
declaration snapshot is retained; executed reservations, frozen order, argv/env,
bindings, science acceptance and caps unchanged. Output-only bound, no cap waiver.
`A4-partial-state-schema-discriminator.json` is source-schema **inference only**,
not an implemented protocol or measured savings.
`A4-smoke-mapped.native-alias-retention.json` binds the current lossless
`A4-smoke-mapped.native-alias.compact.zip` (**1,226,795 B**, SHA-256
`7ed147e169d3ba679fb4638857f67e74cf0a9a6d0a7eeeeee180f77ac3650c3e`).
Historical decoder full verification then isolated restoration proved all
**four files / 1,249,473,542 B** byte-exact before both raw TAR aliases
were pruned, reclaiming **378,786,261 B net unique storage**.
`A4-smoke-mapped.packet-source-retention.json` supersedes raw-packet retention:
packet archive **161,139,747 B**, SHA-256
`922d4c74246a005e146eca798ff36c20dd4cd331dfd00b5fbaa0fed99a35d945`;
exact **391,983,520 B** packet/all-four native restore, fresh missing-path install
and four-file native verification passed before raw-packet pruning
(**230,843,773 B net savings**). **Restore/verify/install the packet at its
original path before native-alias decoding.** Keep the bound original source and
both raw native PLY paths (**244,723,715 B**, same inode) immutable.
Earlier hardlink/raw-packet records are historical physical state; prior native
consumption remains byte-bound, not a new GPU consumer or recovery qualification.
All-owned 64-update profiling exited the
expected 42. Full-view profiling completed both workers' 64 kernel counts,
then checkpoint coexistence breached the **8 GiB new-live cap
(8,988,232,817 B)**: monitored STOP 42, incomplete group, **resource-invalid**,
not publication/state or timing qualification. Initial/final checkpoints were
underforecast; reserve approximately **2.7 GiB/group** before fresh admission.
Hash-checked recovery discarded only eight new disposable profile state/PLY
expansions (2,252,873,676 B), retaining metadata/logs/config/sidecars.
These bytes are **not archived/restorable or qualification states**; protected
source/historical evidence and required A2/A3 dependencies remain untouched.
Both A3 archives reconstructed all 31/133 files byte-exact and passed isolated
native final owner-pair merge/body readback. Retain both compact archives and
bound dependencies, including models/state/all 102 generation records; restore
before future group/model use. Redundant expansions were removed only after
consumer proofs; protected historical evidence is untouched.
Current aligned successors at unchanged paths are `A3-full-control.compact.zip`
(**568,564,181 B**, down **6,174,029 B**; SHA-256
`7ed54e94746a741899d5e013f7e3a2bc690ab60d38b893139ba9d7dfd3e8ef48`)
and `A2-partial_transfer.compact.zip` (**753,288,471 B**, down **95,860,435 B**;
SHA-256 `e8770e123c99a7476a001d90133641d76001167e4bebe8bee11f361046fa0bdd`).
Their `.aligned-migration.json` records prove full isolated restoration of all
**31 / 1,099,960,870 B** and **71 / 1,401,709,414 B** scientific files,
respectively, byte-exact. Old wrapper bytes are not retained; old compact records
are historical. Required references and prior native-consumer bindings remain;
no new GPU consumer or qualification follows. `A2-owner1-stop.compact.zip`
also now occupies **753,287,344 B** (down **95,859,123 B**), SHA-256
`921f44c9da424ecd52c8c80416b094693821f2fff988cd2828fb057607265dfc`;
its `.aligned-migration.json` proves all **70 / 1,401,710,267 B** scientific files
byte-exact through full isolated restore. `A3-worst-gt-hardlink-retention.json`
retains all six exact GT paths on two read-only inodes (**63,428,232 logical B saved**).
**No A4 family is closed/exhausted; A5–A6 remain unexecuted.** Native remains default;
original Q decoded `1e-4` long recovery remains **FAILED** unchanged.
See the [continuation handoff](multi-gpu-training-handoff-20261002.md#resumed-continuation-evidence--2026-10-02)
for current evidence, resource samples and hardlink preservation rules, with
the original paused-handoff observations retained separately.
The [active continuation contract](#autonomous-execution-and-completion)
remains the authority.

B2-0–B2-5, Q0–Q4, D0–D5 and V0–V5 remain completed historical experiments.
B2 won the bounded cold/reuse throughput comparison, but **no comparable-quality,
long-recovery-qualified split-GPU path is settled yet**. Native remains the
practical default; short refresh smokes do not waive the historical failures.

**Continuation executed: [Q0–Q4 longer-run quality and recovery qualification](multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results).**
The control completed 20,000 updates and both B2 arms completed 10,000 per
owner. Later STOP/killed-owner safety and committed exposure replay passed,
but both owners failed the unchanged decoded recovery screen. Matched 132-view
B2 quality regressed: mean raw/color-corrected PSNR −1.602366 / −2.534380 dB
versus control. Those Q runs lacked worker-local allocation-enforcement proof.
**Withhold longer-history recovery qualification and release approval.**
The bounded throughput milestone remains closed; it does not demonstrate
comparable-quality acceleration. Native training is the practical default
while B2 remains experimental and these qualification failures are unresolved.

**[D0–D5 causal investigation completed](multi-gpu-training-b2-causal-20261001.md#execution-evidence) on 2026-10-02.**
Worker backing is now enforced before allocation and exercised on supported AMD
devices. Both late live-owner immediate save/load roundtrips were exact; no-save
and save/load continuations failed the unchanged screen by 64 updates.
Matching per-row update counts did not remove B2 quality loss: mean raw/cc
PSNR was −1.674363 / −2.357914 dB against fresh native step 20,000.
Native training remains the practical default; B2 remains experimental.
Original Q longer recovery remains FAILED, with no unique numerical cause or
repair qualification claimed. This finite investigation is closed.

**[V0–V5 view-policy ablation completed](multi-gpu-training-b2-view-policy-20261002.md#v5--completed-disposition-and-next-decision) on 2026-10-02.**
Immutable full-view policy and accepted fresh P/F passed native checks. On 132
matched held-out views, F improves raw/cc mean PSNR over P by **1.048877 /
1.287893 dB**, but remains **0.596867 / 1.050402 dB** below native N.
Native visual examples confirm improvement with residual errors. This one pair
reduces the observed gap, not comparable-quality acceleration or qualification.
Native remains default, B2 experimental; Q/D conclusions unchanged. Remaining
context/trajectory and cheaper coverage-preserving work now proceed under A0–A6,
not another user-approval gate and not a continuation that rewrites V0–V5.

The [fresh integrated comparison](multi-gpu-training-comparison-20260929.md#2026-10-01-integrated-b2--selected-cold-and-reuse)
records B2 cold/reuse medians **357.438379 / 250.754732 s** versus
**431.199014 s** native control: **17.105938% / 41.847100% less complete
wall**, with nonoverlapping ranges. Historical B1 measurements remain separate.
This selects approximate fixed-work training, not equal optimizer trajectories,
quality acceptance, capacity gain or standalone multi-GPU viewer rendering.

**O1 is parked and the tested G1 schedules remain closed.** Their
[O1 screen](multi-gpu-training-o1-screen-20260930.md) and
[G1 cost evidence](multi-gpu-training-g1-implementation-20260929.md) are not
architecture-wide impossibility results. A4 authorizes a materially different,
source-grounded candidate when its admission gate passes; a new plan for that
candidate is an agent deliverable, not a request for further authorization.
Do not replay rejected schedules or switch architectures to evade a fixable bug.
Historical evidence and exclusions remain in section 11; the
[timing ledger](multi-gpu-training-timing-20260929.md) and
[M0 protocol](multi-gpu-training-micro-01.md) retain provenance and measurement
conventions, not a requirement to repeat already closed screens.

## 1. Direction and decision rules

Use the user-selected replacement dataset as the performance and native-relative
comparison workload under the [new-dataset entry gates](#replacement-dataset-plan--2026-10-03).
Kitchen checkpoints and measurements remain historical; no cross-dataset
checkpoint resume, timing ratio or quality envelope is valid. No absolute PSNR
floor, demonstrated capacity overflow or failed single-GPU baseline is required
to begin an admitted benchmark. Earlier X4 results remain separate history.

The objective is **a usable split-GPU training path on the selected workload**,
starting with two supported devices and a freshly bound native
fixed-topology/appearance source. Historical throughput informs hypotheses only;
settlement now requires the native-relative quality, complete-cost and recovery
checks below. A faster result with unresolved quality loss is experimental, not
the default recommendation.

Choose the best **demonstrated complete-work path within this scope**. Rank cold
and reusable preparation separately. Native is the fallback, not permission to
stop after the first negative split experiment. Exact synchronous training is an
authorized alternative when measured costs justify it, not a requirement imposed
on every approximate candidate. Larger-scene capacity remains a distinct claim.

```text
Closed B2/Q/D/V evidence
  -> bind inputs/resources and native-relative decision rule
  -> full-view context-refresh mechanism, native proof and matched experiment
  -> measured-cause repair / cheaper coverage / materially different contender
  -> fresh quality + complete-wall + long-recovery qualification
  -> select an implemented split path, or close all justified candidates
```

Complete the comparison, not every possible implementation. Every candidate gets
a measured result, a supported bound explaining exclusion, or an explicit
unverified row with the smallest missing experiment. Missing evidence on one
candidate does not stop independent benchmark rows. A fitting single-GPU control
is the baseline, not a stop condition; input bottlenecks are measured costs, not
a reason to suspend all split-GPU measurements. If no split wins, retain that
result and plan only the specific bottleneck experiment justified by the data.

Replicated-scene independent jobs remain a diagnostic hardware-contention
baseline, not cooperative training. Image/tile partitioning is reserved for
measured per-image memory or imbalance limits, not a default renderer design.

## 2. Repository foundations

| Current seam | Consequence |
|---|---|
| `TrainerSession` and `build_step_config()` in `src/app/TrainerCore.cpp` | Keep the one native training driver, configuration, evaluation and checkpoint flow. |
| `engine()` / `engine_reset()` and process-global `EngineState` | One GPU and one training session per worker process remain the starting model. |
| `_engine_step_fwd_bwd_only()` and accumulated split-batch updates in `src/engine/EngineTrainStep.cpp` | There is an existing numerical seam between rendering/backward and one optimizer/topology update. Refactor only the chosen path through it. |
| `_resolve_split_vs_fpbo()` in `src/engine/EngineDataManager.cpp` | Accumulation and FPBO have different effective layouts and update paths. Audit both semantics and memory when selecting the reference. |
| `POOL_SLOT_TABLE`, `POOL_ALIAS_TABLE`, native quantized codecs | Use actual format/capacity/lifetime accounting. Do not invent a generic bytes-per-Gaussian estimate or a second codec. |
| `JobScheduler`, `WorkerRequest`, `OutputLease`, existing process launcher | Reuse explicit device identities, immutable attempts, process lifetime, cancellation, device and output admission. |
| `AgentTrainingJob`, native checkpoint verification and transfer manifests | Reuse whole-job input/output staging where applicable. Existing remote-worker work is evolving; inspect its current contracts before extending them. |
| Vulkan runtime and shared Slang math | Measure real host staging and keep one numerical implementation. A CUDA-only paper is not a Vulkan transport implementation. |

The accepted [device scheduling work](device-job-scheduling-plan.md) schedules
independent workloads; it does not already implement cooperative training.
Blocks extend that model naturally. Synchronous ownership requires a new
group/step contract at the existing scheduler boundary, not a second scheduler.

## 3. Four responsibilities, introduced only when needed

These are ownership boundaries to preserve during an experiment, not four new
frameworks or a requirement to refactor the engine before M0.

| Responsibility | Owns | Invariant |
|---|---|---|
| Authoritative scene state | Parameters, optimizer moments/counters, densification state, topology identity, camera/global appearance state | Exactly one authority for each mutable row/state item. Its lifetime is independent of the next render batch. |
| Render working set | Snapshot rows needed by assigned views, context, primitive-specific intermediates and temporary gradients | A coherent version; bounded device residency. Read-only context cannot acquire optimizer/topology authority. |
| Work and update plan | Image/face assignment, loss weights, effective global batch, update schedule and topology epoch | Every exposure/contribution is accounted for, with one defined update boundary. |
| Transport and publication | Bounded payload movement, completion, worker attempts and checkpoint publication | Payload identity/version is explicit; partial or stale attempts cannot become a successful model. |

Use the current local path as the implementation until a chosen experiment needs
one of these separations. Independent blocks can have a small owned/context
distinction; they do not require a general all-to-all transport API.

### Cross-mode numerical contract

- Reuse the native forward, loss, backward, optimizer and densification kernels.
  Host orchestration and storage ownership may change; the equations do not get
  a parallel CPU or model-specific distributed implementation.
- Define ownership of background, color-space transforms, bilagrid/PPISP,
  image-specific parameters and their optimizer state, as well as Gaussian rows.
  Independent private copies of a supposedly shared trainable variable change
  the objective.
- Preserve effective loss weighting, gradient scaling, optimizer bias correction,
  learning-rate/SH schedules, regularizers and densification reductions. A max,
  quantile or globally normalized score is not automatically an additive reduction.
- Gradients must reach the correct authoritative row across all views. Packed
  positions are not persistent IDs; distributed paths need stable identity plus
  topology/version mapping. Do not sum packed quantized bytes as if they were
  floating-point gradients.
- Match a resident reference with the same effective numerical path for parity.
  Separately compare speed against the best applicable existing resident path;
  losing FPBO or compact moments is a real capacity/performance cost.
- `3dgs`, `mip` and `3dgut` retain their native semantics. In particular, `3dgut`
  needs its world-space raster inputs and corresponding backward data; one
  ordinary projected screen packet cannot stand in for every primitive.

## 4. B1 — smallest block-training proof

**Hypothesis:** independent owned/context blocks improve fixed-work throughput
or capacity. Benchmark this separately from eventual merged-scene quality.

1. Start on a fitting reference scene and one block. Establish one shared scene
   coordinate frame, initialization and image/appearance calibration. A coarse
   global prepass must itself fit; charge its time and memory to the result.
2. Build an owned/context manifest from native visibility plus conservative
   spatial candidates. Associate views by contribution, not camera position
   alone. Specify loss coverage/weights rather than silently discarding pixels
   whose occluder or target lies outside the block.
3. Feed both sets to the native renderer, but allow **only owned rows** to change.
   Exclusion covers Adam/moments, momentum decay, noise, opacity resets,
   regularization, densification statistics, cloning, pruning and cap accounting.
   Zeroing a final gradient alone is insufficient to freeze context.
4. Compare frozen full-scene and owned+context renders on retained boundary and
   distant-occluder views. Then train one block, followed by two independent blocks,
   using the ordinary trainer. Begin with frozen context from a named snapshot;
   this is explicitly an approximate objective.
5. Give each Gaussian one owner; children inherit that owner. Movement across a
   geometric boundary does not silently duplicate or drop a row. Start with fixed
   ownership; any later migration or context refresh needs an explicit boundary
   and version policy. Divide one total output-cap budget among owners; context
   copies do not count as additional trainable/output Gaussians.
6. Export/merge only owned rows in the shared frame. Context copies are never
   additional output Gaussians. Freeze shared appearance from the common prepass
   initially, or define one reconciliation experiment; independent per-block
   appearance drift cannot be hidden by concatenation.

**Benchmark acceptance:** unchanged context parameters/state, complete unique
owned output (including explicitly excluded rows), valid native readback and
bounded measured memory. Verify native resume only when a benchmark uses it;
do not treat an untested checkpoint format as a reusable control. Report
throughput and complete fixed-work wall time with approximate update semantics.
Frozen-render comparisons diagnose missing work or broken rendering; no PSNR
floor or visual-quality envelope gates these performance measurements.

The original B1 benchmark did not qualify quality or context refresh. A0–A6 now
authorizes measuring context refresh and any necessary native reconciliation to
resolve quality/cost, without upgrading that old result. Nearly global context
limits capacity claims but does not itself disqualify throughput. Any required
global finetune must be declared as a separate candidate intervention, retain
native state/optimizer semantics, and have its complete cost charged.

## 5. B2 — active bounded implementation

**Historical B2-0 through B2-5 completed; selected for bounded cold/reuse work.**
Keep the technical micro-plan below for reproduction, not as unexecuted work or
the current finish line. A0–A6 supersedes its extension-authorization limits;
unchanged frozen-context B2 retains the contract below.

| Executed gate | Bounded evidence / outcome |
|---|---|
| B2-0 | Unchanged full native source/config and exact 1,195-view identities; final native build/toolchain/two AMD devices bound. Fresh device budget 33,250,242,560 B each; caps 24 GiB accounted/device, 12 GiB aggregate sampled host, 8 GiB new live outputs, 10 GiB minimum free scratch admitted. |
| B2-1 | Native source/config/horizon/view/owner/shared-state contracts, marker/sidecar refusal, complete unique owned+excluded coverage and 64-row tail enforced; merged PLY rejected as optimizer checkpoint. |
| B2-2 | Actual normal completion, unequal STOP/resume, killed-owner/sibling-safe restart, stale/duplicate/incomplete/current-prior-writer and foreign-completion-hardlink refusals; no-replace completion and restart without retraining exercised. Power-loss durability remains unproved. |
| B2-3 | Four-update native decoded `1e-4` resume contract and exact exposure replay; exact unowned packed state/bounds/shared state/all clocks. Owned raw bytes not identical; diagnostic outliers retained. Actual merged render/eval and mesh-input consumers; final dev Vulkan build, 16/16 headless, AMD `fpbo_split_parity`, 1/1 `cli_training_smoke` passed. |
| B2-4 | Three fresh unprofiled controls and three native groups plus three fresh visibility/build preparations; full checkpoints/contracts/merge/readback/publication included. Actual exposures/pixels, simultaneous aggregate RSS samples and separate bounded profiler recorded. |
| B2-5 | **Select B2 cold 357.438379 s and valid reuse 250.754732 s**, versus native control 431.199014 s; ranges 1.281349 / 0.109001 / 0.275434 s respectively, nonoverlapping. Native fallback retained for other requirements. |

See the [integrated scorecard, complete costs and release limits](multi-gpu-training-comparison-20260929.md#2026-10-01-integrated-b2--selected-cold-and-reuse)
and [runnable native invocation](multi-gpu-training-b1-execution.md#historical-bounded-native-b2-path).
That bounded selection left no admitted contender unfinished at the time.
It did not establish arbitrary long owner recovery, application quality or
production approval; the later Q/D/V results motivate the active continuation.

### Contract and exclusions

- Reuse the one native trainer and existing block ownership, worker scheduling,
  checkpoint and merge paths. One GPU/training session per worker process;
  explicit supported non-NVIDIA device identities and separate output leases.
- Start with the measured two-owner, fixed-topology native 3DGS/FPBO source
  configuration. Own complete 256-row codec blocks; preserve original IDs,
  partial tails and explicit excluded-row coverage. Freeze unowned parameters,
  moments, clocks and shared appearance from the identified common snapshot.
- Keep full-scene render context and independent per-owner update clocks.
  Do not claim equality to a globally coupled optimizer or silently enable
  topology/shared-appearance updates. Unsupported configurations fail explicitly.
- Deliver a headless coordinated run, validated merged renderable output and
  resumable per-owner checkpoints plus the bound group contract. A merged PLY
  is not a globally resumable native checkpoint; reject that interpretation.
- GUI integration, topology growth, context migration, oversized-scene discovery,
  heterogeneous balancing, remote workers and globally resumable merged optimizer
  state were outside B2. A0–A6 authorizes versioned context refresh and necessary
  state reconciliation for its candidates, not those other extensions by default.
  Do not add transport, trainer or allocator frameworks.

### B2-0 — bind the source and admit the complete run

Read the current B1 source and [execution record](multi-gpu-training-b1-execution.md),
then resolve retained inputs from local manifests. Rehash the source checkpoint,
configuration and view/ownership inputs; record code/build/toolchain/driver and
both devices. Historical source repeats must not be pooled with regenerated
inputs. Missing inputs block their comparisons, not unrelated implementation;
never silently substitute a PLY or regenerate a checkpoint.

Declare the exact supported flags, effective optimizer configuration, frozen
state, ownership/view schedule, native checkpoint format and output consumers.
Specify aggregate exposures, per-owner updates and original learning-rate horizon.
Use existing scheduler admission to exclude conflicting device/output jobs.
Bind numeric per-device, aggregate host, scratch and live-output budgets from
fresh availability and actual checkpoint/staging sizes, with explicit headroom
and stop conditions. Include both full-scene replicas and merge/readback.
The rejected O1 7 GiB target is **not** a B2 limit or permission to change O1.

**Exit:** a local run manifest and a complete source-backed change/check list.
Do not repeat B1 performance discovery as a substitute for closing known gaps.

| Existing implementation | B2 responsibility |
|---|---|
| `src/app/TrainerCore.cpp`, `src/app/cli/main.cpp` | Native source/config/view setup, worker invocation and merged-PLY evaluation with bound shared appearance. |
| `src/engine/EngineB1.cpp`, `src/engine/EngineOptim.cpp`, `src/backend/vulkan/kernels/B1Ownership.h` | Restore stable codec-block ownership and preserve native update/context invariants; do not replace the numerical path. |
| `src/engine/EngineCheckpoint.cpp`, `src/checkpoint/Resume.h` | Persist and validate the block contract in native full checkpoints, including a marker that survives missing sidecars and refuses re-ranking. |
| `src/backend/tests/engine/b1_merge.cpp` | Reuse the existing native merge/readback implementation; make it available to the coordinated path rather than writing another merger. |
| `src/app/JobScheduler.cpp`, `src/app/OutputLease.cpp` | Extend existing attempts, process lifecycle and leases with the required group completion checks, not a second scheduler. |

Existing checks include `checkpoint_resolution_test`, `scheduler_test`,
`subprocess_test`, GPU `cli_training_smoke`, native `fpbo_split_parity` and
`b1_merge <left.ply> <right.ply> <merged.ply>`. Baseline checks alone did not
prove B2 group recovery or missing-sidecar safety; the current native workflow
and focused behavioral refusals now supply that bounded evidence. Preserve these
checks when extending exported interfaces and inspect their current references.

### B2-1 — bind and enforce one training contract

Extend the existing block metadata and validation rather than adding a parallel
manifest format. Bind source identity, effective configuration, owner IDs,
context/shared-appearance identity, view assignments, format/version and completed
step to worker checkpoints and merge inputs. Define which fields must agree
between owners and which are legitimately owner-specific.

Require complete unique source-row coverage across included and excluded rows;
context copies never become output rows. Wrong, absent, stale, duplicate,
truncated or mismatched metadata must fail closed before training or publication.
An owner checkpoint that lost its sidecars must not become a fresh source that
re-ranks moved means. Preserve ordinary single-GPU checkpoint behavior.
Bind the frozen appearance used by render/eval consumers to the merged source.

**Exit:** real native accepted-input and rejection checks, including context
immutability and the latest source-bound checkpoint format. No benchmark with
unchecked private conventions can satisfy this gate.

### B2-2 — coordinate, resume and publish through existing facilities

Drive the existing native workers with immutable attempts and exclusive output
leases. Restore a block's ownership/context/view contract and optimizer clocks
from its latest full native checkpoint; do not infer them from current positions.
Resume a stopped group only from a coherent declared completed schedule position.
If owners progress unequally, retain their valid checkpoints without falsely
publishing a completed group or silently skipping/duplicating exposures.

Use the existing cancellation and worker lifecycle. Demonstrate an interrupted
worker, failed owner, stale result, duplicate attempt and incomplete pair.
Permit safe restart from validated checkpoints; do not introduce automatic retry
policy merely to make the happy path succeed. Protect source and prior valid
outputs. Publish completion only after all expected owned outputs and their
contract validate, merge/readback succeeds, and no-replace publication succeeds.
Retain incomplete attempts as incomplete; never label a partial preview success.

**Exit:** native normal completion, interruption/restart and failed-publication
scenarios, including refusal of an unrelated concurrent output writer. Establish
the actual durability guarantee; atomic naming alone is not proof of power-loss
durability. Unproved durability remains an explicit release limit.

### B2-3 — prove numerical ownership and native consumer behavior

Run one-block/all-owned equivalence through the same numerical path, then the
actual complementary two-owner source case. Require unchanged unowned native
parameters, packed moments/bounds and clocks; verify owned/excluded coverage,
tail lanes, shared state and latest-format native resume/readback. Compare
resumed execution with the same owner schedule uninterrupted, using the existing
native numerical contract. Do not demand global single-GPU trajectory equality
from the explicitly approximate two-owner algorithm.

Open the merged output through the existing native render/eval and mesh-input
consumers; verify the shared-appearance binding and reject incompatible resume.
Use retained boundary/distant-occluder views for frozen-render correctness;
missing work is a correctness failure, not an acceptable approximation.
Report trained quality observations separately without inventing a PSNR floor,
new dataset requirement or quality gate for performance benchmarking.

Build through the dev Vulkan script and run the relevant headless, worker and
native supported-device checks after integration. Launch the real coordinated
headless workflow and inspect its artifacts; unit tests alone are insufficient.
Record exact commands, exit codes and artifact identities.

**Exit:** bounded B2 correctness/recovery/consumer evidence. Fix causal defects
and repeat affected checks before timing; never weaken ownership or input checks.

### B2-4 — fresh integrated performance comparison

Use three fresh independent unprofiled matched runs for B2 and its all-owned
resident control, also recording the best applicable native speed baseline
where different. Start from the same source with the same effective frozen
configuration and original horizon. Reuse the retained 1,200 aggregate measured
exposure convention and document warmup/final work and unequal optimizer counts;
record actual view/face/pixel manifests and disclose non-replayed sampling.

Report complete cold wall and valid reusable-preparation wall separately.
Charge discovery/partitioning, contract validation, staging, worker startup,
required full checkpoints, collection, merge and final readback. Do not repeat
the disposable non-full-checkpoint shortcut if B2 needs resumable outputs.
Common source preparation is not B2-only cost. Evaluation, if performed, is
separate or charged consistently to both arms.

Record medians/spread, work counts, per-worker and aggregate host resources,
device allocation evidence and limits on true peaks. Separate profiler runs
from accepted timings. Historical 21.045%/45.498% savings motivate this gate;
they do not pass it. Do not impose the obsolete 20% threshold.

**Exit:** an updated scorecard with complete costs and fresh comparisons, followed
immediately by B2-5 rather than another open-ended benchmarking phase.

### B2-5 — select, repair or close

Use the autonomous decision table in section 10. A correct, complete B2 result
with lower wall time outside observed variability selects it only for the
demonstrated cold/reuse workload and approximate fixed-topology contract.
If only reuse wins, state that explicitly with preparation amortization.
If variability could reverse the choice, extend matched work under the same
contract; do not select from a favorable individual run.

If integrated overhead removes the advantage, profile the actual bottleneck
separately. Repair an identified avoidable cost only when its measured magnitude
could change the decision; then rerun affected correctness checks and the fresh
comparison. Otherwise record no split-GPU winner and retain the native baseline.
Never weaken full-checkpoint, validation or publication requirements to recover
the old benchmark number. Finish with a selection record and explicit release
limits, not a promise of an unmeasured universal winner.

### Separate production extensions

Production still needs an application-specific approximation/quality decision
and the required recovery/durability guarantees. Beyond this bounded mode,
topology changes need inherited ownership and versioned context; oversized
training needs discovery that does not first materialize the full scene on one
GPU; heterogeneous scheduling needs measured work/memory balance. Native
viewer/render/mesh capacity and any globally resumable merged state need their
own demonstrated contracts. UI integration follows a passing headless path and
uses existing i18n/configuration. None of these claims is granted by B2 selection,
and none silently becomes a prerequisite of its throughput comparison.

The [Q0–Q4 qualification](multi-gpu-training-b2-qualification-20261001.md#q0q4-execution-results)
is executed with valid matched quality findings and a failed longer-history
decoded recovery screen. Retained checkpoint lineage and source-dependent
evidence remain intact. The
[completed D0–D5 investigation](multi-gpu-training-b2-causal-20261001.md#execution-evidence)
implemented worker enforcement, valid late-state diagnostics, attributable fixes
and the shorter quality control. V0–V5 subsequently reduced the quality gap
without closing it. A0–A6 authorizes further work; none of these results is
retroactively relabeled as a pass.

## 6. O1 — early CPU-residency alternative

**Hypothesis:** CPU-authoritative parameters/state plus a bounded GPU working set
meets the capacity target with tolerable measured staging cost.

Start with one GPU, a fitting scene and fixed topology. Reuse native candidate
projection, rendering and optimizer kernels; the CPU stores and schedules state.
Do not implement another optimizer in C++ on the CPU. Compare eviction/reload
against the resident path, including codec bounds, moments and bias counters.

The working-set policy must conservatively find needed rows without first
materializing the entire model on the GPU. Measure candidate-query cost, false
positives, reuse, dirty writeback, host RAM and pinned-buffer bounds. Visibility
alone does not authorize skipped optimizer/regularizer updates: if the current
path updates invisible rows or their moments, preserve that behavior with bounded
chunk passes and include their traffic. Claiming an exact method while freezing
those rows would invalidate the comparison.

After fixed-state parity, add actual densification, codec-block changes,
allocation peaks, checkpoint/resume and a workload exceeding device capacity.
Reuse the same shaders and backend launchers; any required chunking must retain
their global reduction/update semantics. Test eviction at awkward row/codec
boundaries, not just aligned toy chunks.

**Benchmark acceptance:** native numerical agreement on fitting cases and bounded
GPU/host memory, with candidate discovery, staging and writeback in fixed-work
wall time. An oversized case is required only to claim oversized capacity, not
to compare throughput on kitchen. Compare O1 with B1's approximate mode and the
resident control, explicitly labelling their different semantics. Add multiple
GPU consumers only if measurements justify their synchronization cost.

## 7. G1 — globally synchronous whole-image proof

**Entry gate:** measured traffic/residency leaves a plausible competitive lane,
or a bounded proof is needed to resolve the comparison. Neither B1 quality
failure nor a predeclared global-quality requirement is necessary. Begin with
two supported GPU processes, fixed topology and a fitting resident reference.

One step has a single immutable model version:

1. A coordinator fixes the global image/face batch, weights and step schedule.
   Owners expose the required row snapshot; conservative candidate discovery is
   charged to the step and cannot require a full-model replica on each device.
2. Whole-image workers obtain all required owners' render inputs, then perform
   native global depth/tile ordering and render/loss/backward for their images.
   Combining arbitrary shard RGBA outputs does not reproduce interleaved alpha
   compositing or its gradient. Render inputs must preserve the primitive contract.
3. Return gradients and densification statistics to the authoritative owners.
   Accumulate all required contributions with the resident reference's weighting
   before updating. Shared/image-specific trainable state has an explicit owner
   or reduction rule as well.
4. Apply one optimizer update and one defined topology boundary. Publish the
   next version only when all required work completes. No stale gradients,
   independent optimizer clocks or asynchronous approximation in this proof.

A whole-image worker may need almost all Gaussian render parameters even when
optimizer state is sharded. Measure that peak, not `N / GPU_count`. The initial
packet may carry world parameters and recompute local projection if that is the
smallest correct native integration; choose projected versus world-space routing
from actual primitive semantics and measured bytes/compute.

Use bounded host-staged transport first, with explicit attempt, step, topology,
owner and payload layout/version identity. Do not send tensor data through a
generic JSON control-message loop. A faster P2P or overlapped route is optional
only after supported-device measurements justify it.

**Acceptance:** compare renders, loss, gradients, parameters, moments and state
after one and multiple matched updates to the resident path using existing
numerical tolerances. Test empty-owner visibility, heavily overlapping owners,
occlusion-order ties, uneven image sizes and each required primitive/format.
Then measure capacity and critical-path timing; two-device arithmetic parity
alone proves neither speedup nor oversized-scene feasibility.

## 8. G2 — topology, recovery and heterogeneous execution

After G1 passes:

- Allocate persistent global row identities and versioned owner/local-index maps.
  Clone, prune, reset and migrate only at coordinated boundaries. Transfer all
  associated parameters, moments/counters and densification state atomically with
  ownership; do not recreate moments at the destination.
- Preserve global cap, sampling and densification score/reduction semantics.
  Include coordinator/host storage and transient old+new allocations in budgets.
- Add a synchronous job-group admission contract at the existing scheduler.
  Reserve the participating devices without partial-acquisition deadlock, retain
  leases until workers exit/reap, and stop the group if a required worker fails.
  Do not reinterpret an absent contribution as a zero gradient.
- Publish a checkpoint manifest only after every shard and shared-state component
  from the same committed step validates. On interruption, recover the last
  complete version; never assemble independently newest shards. Reuse existing
  native checkpoint readers and staged publication.
- Balance both ownership residency and image compute from measurements. A small
  or slow GPU can increase capacity while reducing throughput; report both.
  Begin with static assignments, adding migration only for observed imbalance.
- Demonstrate an actual oversized workload without a hidden full-model GPU
  replica during initialization, routing, checkpointing or finalization. Retain
  current local index/address bounds even when total distributed state is larger.

Validate on the available supported device combinations, including heterogeneous
hardware when available. Record unsupported/unavailable combinations explicitly;
an NVIDIA run is not a substitute for an unverified supported lane.

## 9. G3 — image/tile partitioning only on measured need

Enter when whole-image rendering has a proven memory or load-balance limit that
existing split-batch behavior cannot solve. First establish frozen-render and
gradient parity, then training parity and performance.

Partition pixels without losing cross-partition splat coverage, global depth
ordering or gradient contributions. Account for large projected/world-space
footprints and duplicated boundary work. Losses introduce another boundary:
SSIM/multiscale windows, depth/normal stencils and global normalization/quantile
statistics need correct halos or reductions. Rendering tiles and computing an
independent loss on each crop is not generally the same objective.

Reuse native full-image loss assembly if it fits and keeps the implementation
small. If the image itself cannot fit, specify and validate the necessary loss
partitioning through the same numerical implementation before claiming capacity.
Adopt tiles only when the measured benefit survives exchange, halo duplication,
stitching, gradient aggregation and additional memory.

## 10. Benchmark selection and separate release gates

### Performance benchmark contract

The user-selected replacement data is the workload, subject to the fresh A0
readiness/binding gates. Record source/build/device identity,
resolved configuration, checkpoint identity, active/reserved rows, resolution,
view split, effective batch/face exposure, original schedule horizon, timing
boundaries and available RAM/VRAM/disk. Keep private paths local. Unknown topology
or absent quality annotations are disclosed fields, not dataset rejection.
Do not alter images, poses or the training objective to manufacture a speedup.

| Benchmark requirement | Evidence |
|---|---|
| Comparable work | Fixed-work throughput comparisons require the same source snapshot, resolution, formats and total image/face/pixel exposure; report optimizer updates separately. A1/A5 quality/cost settlement instead compares predeclared schedules at eligible endpoints, charging any unequal work in full. B1 uses a frozen-topology/appearance all-owned control as well as the best applicable native baseline. |
| Repeatability | Three independent unprofiled windows per compared state/device configuration; retain samples and report interval medians/spread. Preserve schedules. Collect per-step p95 only with actual per-step samples. |
| Correct execution | Context/ownership and output-coverage invariants for B1; native numerical parity and coherent updates for exact modes; received-content validation for transport. Failed correctness invalidates that row, not the dataset. |
| Resource safety | Check available disk before checkpoint-producing runs; bound host/device/staging allocations. Preserve source data and retained controls. Label high-water accounting separately from measured simultaneous peaks. |
| Honest scope | Measured, derived, extrapolated and unverified rows stay distinct. Fixed-state throughput does not establish growth capacity, quality or globally coupled scaling. |

Use retained early/middle/late checkpoints **when available** rather than
requiring a new full train. When a source must be regenerated after external
cleanup, record its common preparation cost and new identity; never merge
its medians with old-source repeats or substitute a PLY for optimizer state.
Missing growth/phase observations limit capacity claims, not other timing
rows. Start with the M0 warmup/window protocol, then use one longer
**fixed-work** comparison: predeclare total exposures and stopping point, repeat
both arms three times from the same source, and keep the original learning-rate
horizon. Count unequal image sizes through pixel totals; disclose non-replayed
sampling and extend only if variability could reverse the ranking.

For that longer comparison, measure elapsed time from input preparation through
validated final model output: partition construction, staging, worker startup,
training, checkpoint writes, collection and merge. Report a cold preparation
total and an amortized reuse total separately where inputs are reusable. Report
evaluation time separately and, if run, an additional total including evaluation
in both arms. Quality evaluation was optional for the historical fixed-work
screen; A0–A6 requires it for settlement, not as permission to benchmark.
Never silently exclude required setup or finalization. Missing costs leave
the total incomplete.

Do not label the unequal-exposure A1/A5 comparison as equal-work throughput.
Report its complete-wall ratio only alongside the declared schedules, actual
exposure/pixel totals and quality outcome; a faster ineligible endpoint is not
a comparable-quality win.

### Comparison sequence and decision

The historical integrated comparison selected B2 for bounded cold/reuse work.
Q/D/V subsequently established unresolved quality/recovery limitations. Preserve
those tables, exact failures and experiment identities. The active work below
must settle the broader practical decision, not repeat a favorable old window.

### Autonomous execution and completion

**Current precedence:** the
[2026-10-07 raw selection](#selected-raw-dataset-for-all-future-evaluations--2026-10-07)
controls every future dataset-backed evaluation, including full qualification.
Work remains plan/cleanup only under the finite
[testing/promotion envelope](#autonomous-testing-and-production-promotion).
Standing language below does not admit raw recordings, preview products or
old source/reference bindings, authorize a new SDK/successor/evaluation, or
override the193 ceiling. Fresh preparation/admission and explicit execution/
budget approval remain required.

**On explicit authorization to execute/resume this replacement plan:** execute
A0–A6 end to end under standing authorization. Until then, this plan-only
revision authorizes no runs or automatic restart. Execution includes investigation,
candidate contracts, native implementation, focused tests, real supported-device
runs, evaluation, failure diagnosis, attributable repairs, resource-safe evidence
handling and final documentation. Do not stop to ask whether to implement an
admitted mechanism, run the next arm, investigate a failed check, benchmark a
viable variant or qualify the winner. Writing a subplan is not completion.

This section supersedes the earlier per-experiment “separate authorization”
clauses for work serving this decision. Completed B2/Q/D/V and G1/O1 records
remain historical evidence, not pending tasks. Record each new experiment's
contract before execution; the agent chooses ordinary implementation details,
cadence, run order and bounded comparisons from source and measurements.

The scope is two supported non-NVIDIA Vulkan devices, the user-selected
replacement dataset with a freshly bound native source, the native numerical
implementation and a usable headless training workflow. Context refresh,
coverage-preserving scheduling, measured transport improvements, native
reconciliation and justified synchronous alternatives are authorized.
The explicit dataset change does not authorize changing loss, optimizer,
precision, resolution, source, horizon or tolerance after the new contract is
frozen to obtain a pass. Additional datasets, larger-scene capacity, topology
growth, GUI/remote integration, representation changes and heterogeneous fleets
are not prerequisites.

#### A0 — bind the retained evidence and fresh resources

Follow the selected full-resolution-original readiness and source-generation
gates above. Bind exact `.insv` identities/SHA and raw-to-frame lineage,
decode/extraction/projection/camera/track/timestamp settings, finalized encoded
inputs/masks/native reconstruction, effective config and fresh source checkpoint.
Freeze newly derived global/train/held-out ID/name/post-face mappings, source/
reference steps/horizon, installed owner masks, row/tail/coverage counts and
current source/binary/evaluator/native references. Kitchen and the previous
Source v5 replacement remain separately bound history, never new inputs/passes.
Rehash required dependencies before use and verify input stability afterward;
metric fingerprints are not input RGB digests. Do not substitute missing
optimizer state. Keep private paths, device UUIDs and commands in local records.

Apply the staged admission order above: freeze A1's protocol before source
generation, then bind the generated source and derived masks/counts. Generate
and bind native reference outputs under that frozen protocol before judging
candidate quality. No phase may use its own not-yet-produced output as an entry
requirement or treat a planned output as verified evidence.

Refresh the complete phase/coexistence ledger before launches: **24 GiB actual
worker backing per device**, **12 GiB aggregate scope-3 sampled host**,
**16 GiB shared retained outputs**, **8 GiB private native Job**,
**10 GiB minimum free scratch**, plus worker-local **1 GiB heap headroom**,
the unchanged **4 GiB baseline reservation / 9 GiB commit reserve** and any
stricter inherited limit. The explicit user selection changed only shared
retained output; count all historical ROOT/campaign namespaces, not just new
live growth. Include old/new context, exchange staging, coordinator storage,
save temporaries, build/smoke/eval artifacts, archives and their dependencies.
Enforce backing before allocation; external Vulkan usage samples do not measure
worker memory. Label sampled RSS and accounting peaks honestly; neither is a
true physical-memory peak. Fresh admission remains mandatory for every phase.

Use existing scheduler/device/output leases and attempt-owned processes. Direct
workers enable `SS_WORKER_CONTROL=1` with writable stdin. Monitor the full group;
on a limit, STOP safely and mark the arm incomplete. Preserve sources and required
evidence; classify and retire other artifacts under the prospective
[three-class cleanup policy](#aggressive-dependency-aware-cleanup).
Never assume compression, raise a cap without explicit user authorization,
kill unrelated work or clean shared Temp to admit an experiment.

**Exit:** verified inputs, source-backed seam inventory, fresh numeric admission,
protected-artifact list and candidate queue. A resource block pauses dependent
runs only; complete independent implementation/checks and identify exact missing
capacity. A temporary external blocker is not a negative architecture result.

#### A1 — freeze the comparison and settlement rule

Use native training as the quality/cost reference, not only partitioned B2.
The replacement campaign plans a freshly generated source at step 10,000 and
10,000 continuation updates per owned row to step 20,000 under horizon 30,000;
freeze and admit that contract before source generation. Full-view two-owner
work therefore has 20,000 aggregate exposures versus native's 10,000; record
actual input/face/dimension/pixel events rather than nominal iteration equality.
Old D3/V/kitchen models and previous-dataset Source v5/readback cannot supply
new original-recording source or native reference baselines.
For final settlement, price the best applicable ordinary native path as well as
any same-sampler/numerical-path control needed for causal or parity checks. A win
only over a deliberately slower control does not select the split path.

Evaluate every named held-out view in the new immutable split with one current
native evaluator. Report
mean/median raw and color-corrected PSNR, SSIM and L1, all paired per-view deltas,
regression counts and worst-view native renders. Hold shared appearance and
input ordering fixed. Selection on this dataset is workload-specific evidence,
not population or generalization proof.

Do not invent an absolute PSNR floor or accept an arbitrary quality loss. The
default final quality rule is native-relative: take three fresh native repeats
and three fresh repeats of the frozen candidate. For every one of the six
metrics, the candidate's median-of-run means and median-of-run medians must be
no worse than the least favorable corresponding native repeat; for each named
view, its median candidate metric must also be no worse than that view's least
favorable native repeat. Higher PSNR/SSIM and lower L1 are better. Report every
failure; one metric cannot compensate for another. This observed-repeat envelope
is a conservative decision rule, not a confidence interval, numerical tolerance
or claim that native variability is harmless. Freeze the formula before new
candidate results and calculate it from contemporary controls, never old P/F
differences. Do not widen it after failure.

Quality does not gate initial profiling or fixed-work benchmarking. It gates
calling a path a practical comparable-quality selection. A candidate needing
more updates or a native reconciliation stage is a different declared work
schedule: set finite evaluation endpoints and required state assembly before
running it, stay within the original horizon, and charge all extra work. Never
resume a merged PLY as optimizer state. Time-to-quality is measured elapsed time
to an actually evaluated eligible endpoint, not interpolation or extrapolation.
The replacement campaign's planned native quality target is **step 20,000 from
its own source step 10,000**, with its own complete-wall baseline; do not switch
to step 30,000 when metrics move nonmonotonically. Declare any different
reference endpoint as a separate comparison before its candidate results exist.
Diagnostic endpoint probes may inform development, but freeze **one common
candidate endpoint and reconciliation
schedule before A5**. All final repeats and all six metrics use that endpoint.
No per-run, per-view or per-metric favorable endpoint selection is permitted.

**Exit:** predeclared source/config/schedule, causal arms, endpoints, quality rule,
timing boundaries and falsifiable decision for the next candidate.

#### A2 — implement and prove one coherent context-refresh mechanism

Treat **full-view B2 with periodic cross-owner render-context refresh** as the
default first discriminator only while fresh A0/A1 coverage/cost evidence makes
it decision-relevant. Reuse the implemented mechanism; do not repeat historical
development by default. If fresh evidence excludes this discriminator, record
the bound/reason in A4 and select the next justified candidate; historical
failure alone does not exhaust the family. When running the refresh experiment,
keep view schedule, ownership, topology, shared appearance and native raster/
optimizer unchanged so refresh is the single initial intervention. Stale context
remains a hypothesis, not an established cause of V's residual loss.

At authorization, `src/engine/EngineB1.cpp` hashed unowned state and had no
refresh interface; `src/app/B1Group.cpp`, `src/checkpoint/B1Contract.cpp` and
`src/checkpoint/B1Merge.cpp` bound only frozen-context owner checkpoints.
The [current handoff](multi-gpu-training-handoff-20261002.md) records the
implemented live-refresh extension and its remaining verification. Extend the
existing seams, one `TrainerCore` and native checkpoint/worker path as necessary.
Do not disable frozen-context verification or disguise refreshed state as the
original source. Existing G1 exchange/state helpers may be reused where their
actual contract fits; a new generic transport layer is not the deliverable.

At a common completed update boundary, derive one context generation from the
authoritative rows of both owners. Each receiver replaces only peer-owned render
parameters and their required codec bounds. Its owned parameters, moments,
optimizer bounds, clocks and accumulator state must remain byte-exact across
installation. Non-render unowned state retains its declared invariant; it does
not gain optimizer authority. Cover every original row/whole codec block and
any partial final block, using the admitted source's actual valid-row count,
including inactive/excluded rows. Do not reconstruct this state from
opacity-filtered, decoded export PLYs.

Bind source, policy, owner masks, common boundary, generation, parent lineage and
actual payload identities in the existing versioned group/checkpoint contracts.
Historical schemas keep their semantics; new candidates fail closed on missing,
stale, mixed or partially installed generations. Both workers acknowledge the
generation before either advances. Checkpoints/resume must recover one committed
pair, not independently newest owners across a refresh barrier. Keep ordinary
frozen-context B2 behavior and owned-row-only no-replace merge intact.

Measure actual payload/transfer/install costs and choose one fixed step cadence
before the primary run, with a complete traffic/time/resource ledger. A missing
native export/install/barrier seam is implementation work, not grounds for
stopping with a design. Prefer persistent workers; checkpoint/restart cycling
cannot stand in for live refresh while long numerical recovery is unqualified.

Exercise the real path: owned-state identity across refresh, exact peer payload
installation, unchanged full-view events, codec tail/inactive rows, shared state,
STOP on either side of the barrier, killed owner, partial transfer/install,
stale/mixed generation, resume, merge and refusal without replacing prior output.
Use the dev Vulkan build and relevant headless/worker/native AMD checks after
integration. Unit tests alone do not prove the workflow. Fix demonstrated defects
and repeat affected checks before a primary run.

**Exit:** implemented, admitted native refresh workflow and invariant/failure
evidence, or a measured intrinsic rejection with the missing mechanism resolved
far enough to justify it. An unimplemented plausible seam is not such evidence.

#### A3 — run the matched context experiment and follow the result

Run a fresh unchanged full-view control and fresh refresh treatment on the same
build/source/device assignment, each uninterrupted through the declared target.
Bind/re-evaluate N as needed. Never substitute a shorter run, a resumed primary,
an earlier partitioned arm or a favorable historical sample. One pair answers a
bounded diagnostic question, not final speed or statistical qualification.

| Observation | Autonomous next action |
|---|---|
| Source/state/coverage/safety failure | Preserve evidence, diagnose the shared cause, repair and rerun affected gates/arms; do not route around a bug. |
| Refresh reduces quality loss and leaves plausible complete-wall headroom | Proceed to A5 if the candidate is ready, otherwise admit a measured-cause refinement through A4. |
| Quality improves but exchange/duplicated compute removes the benefit | Profile native critical-path costs, then admit a cheaper coverage-preserving or transport variant through A4. |
| Substantial quality loss remains | Inspect actual refreshed payloads, exposure/state transitions and native variability; test one justified trajectory/reconciliation variant or a materially different synchronous candidate through A4. |
| Mixed/inconclusive effect | Retain the uncertainty; use one predeclared matched extension if it can change the decision, otherwise close this variant and triage A4. |
| Intrinsic resource/cost bound rejects refresh | Record the complete bound and triage A4; do not repeat the same mechanism under a new label. |

**Exit:** full per-view/cost report and an executed next decision, not another
request for permission. A scientifically negative arm closes that arm only.

#### A4 — resolve every decision-relevant alternative

Maintain one candidate table here/in the scorecard: mechanism, evidence prompting
it, source seams, invariant changes, measured dominant cost, conservative remaining
cost/resources, decisive experiment, result and next action. Assess every row
below; “not relevant” needs source/measurement evidence, not an unsupported guess.
Implement admitted candidates; a missing helper or protocol is not a rejection.

| Candidate family | Entry and bounded discriminator |
|---|---|
| Refresh cadence/trajectory or native reconciliation | A3 exposes a particular staleness/trajectory limit. Test one justified refinement, separately from a view change; preserve authoritative optimizer state and charge reconciliation. |
| Cheaper coverage-preserving owner schedule | Duplicated view work dominates a quality-promising route. Declare support, frequency/weighting and order explicitly, exclude held-out views, prove complete intended coverage and compare actual work/quality; do not claim it is still the identical full native sequence. |
| Materially different synchronous/transport path | Measured transfer/serialization/compute decomposition identifies an affordable alternative to the rejected G1 file-staged schedules. Price actual native payloads and required validation, then prove source-native render/gradient/update parity before long timing. Compare against both its same-numerical-path control and the best applicable native baseline. |
| Offload, residency or image/tile work | A measured capacity/traffic/imbalance bottleneck makes it relevant to this throughput decision. Require conservative discovery, native state/loss/gradient semantics and full peak accounting. O1's rejected 7 GiB schedules and G1 histories remain rejected; do not silently relax their caps or reinterpret unrun work as a pass. |

**Prospective queue — new dataset unqualified, plan update only.**
The [replacement-dataset entry order](#replacement-dataset-plan--2026-10-03)
takes priority. Establish new A0/A1 bindings and producer custody, then rebind
the prepared coverage helpers and run freshly admitted complete-view probes.
The rows below retain kitchen evidence as hypotheses/history, not new-dataset
costs or acceptance. Only one GPU candidate runs at a time; sampled resources
do not establish physical headroom, a speed win or family exhaustion.

| Variant / status | Prompt and source seam | Invariants / source-cause result | Measured cost / resources | Decisive next gate |
|---|---|---|---|---|
| Cadence-1,000 refresh — **closed diagnostic**, not family exhaustion | A3 live peer-context barriers in `EngineB1`/`B1Group`; ten committed generations | Exact accepted events, pair state and frozen/unowned/clocks; raw/cc residual −0.5733015/−0.9063144 dB versus N, mixed six-metric response | Maximum-owner barrier sum 227.6657683 s; one differently scoped timing pair, no speed qualification | Preserve report/archives; no blind cadence grid or A5 admission from this result |
| Same-input native-mask discriminator — **completed bounded check** | `EngineB1` native internal FPBO all-owned repeats versus marked owners | Exact reset/first forward/cotangents and state invariants; no demonstrated first mask-semantic bug; event-8 peer-trajectory signal is descriptive, not Q cause | 32 updates/four arms; sampled RSS 3.67 GB versus 12 GiB cap, about 2.14 GB application worker backing versus 24 GiB cap; not physical peaks/headroom | Use source-grounded trajectory/synchronous discriminator; no decoded tolerance waiver |
| Full-state mapped G1 — **finite12 complete; original-r3 cause unresolved** | Frozen matrix versus newer oracle/stage diagnostic | Later native five-update oracle/stage probes exact first-file43/TAR/PLY; raw op2 FNV attempt-specific, normalized native fingerprints match; no cause fix | Mapped finite41.1911613 s versus ordered12.406858/unordered13.0982101 **source-capture/internal-FPBO** controls, not actual CLI | Preserve scope/archives; new resident-owned candidate below, not family exhaustion/Q/A5 approval |
| Resident-owned cached-worker exchange + optional CNG digest — **AMD oracles/science/custody and six prices complete; not selected/A5** | Worker-local cached state, ownership-projected exchange; three alternating repeats/hash mode isolate FNV→CNG SHA256-64 only | Both five-update oracles and all6 benchmark independent science/full lossless custody passed; all43/native TAR+PLY exact; all6 gradients/full D2H/unowned/cache restore retained, not zero-copy/pure residency or old-r3 fix | Complete medians FNV **30.58593 s** / SHA **26.241224 s**, measured **9.5604241 / 7.4451034 s**; actual ordinary CLI **11.8680671 s** is one unmatched-view/pixel arm, **no speed ratio** | Preserve all caps/custody and old-r3 uncertainty; ordinary native archival before next proof; no Q waiver/quality/A5 from hash-only savings |
| Once-native versus primed discriminator — **bounded native/consumer complete; equivalence not established** | Roles2/0/1 endpoints1/8 fivearms135updates54primes54captures; old32update mask preserved/optional capture observes only | Source28NPY/42runtime+CPUimplicit43 reset exact; ordinary-repeat/capture-equivalence/all decoded flags **false**; backward differs after exactforward beforeauxcapture/no unique cause | Native849.9833316s/consumer0; scope3RSS4,947,775,488B/backing2,119,824,608B; oldmask0/six invariant summaries; boundary22cases0 | Candidate-specific contemporaneous repeatability/capture gates; preserve first-full-pair/Q1e-4, no serializer/quality/recovery/A5/family closure |
| Trajectory/native-checkpoint reconciliation — **conditional**, not closed | Mixed A3 response plus frozen-peer event-8 divergence | Preserve authoritative optimizer/ownership state; isolate reconciliation from view-policy changes | No qualified refinement cost/headroom yet | Admit one falsifiable measured-cause refinement only if it can change the decision; charge reconciliation |
| Cheaper coverage-preserving owner schedule — **conditional**, not closed | Both full-view owners duplicate 10,000 native events each versus N's 10,000 aggregate | Must explicitly declare support/weights/order and exclude held-out views; cannot claim the unchanged full native sequence | No admitted schedule or accepted comparable-quality cost yet | Source/coverage proof and bounded native work/quality discriminator, not blind tuning |
| Offload/residency/image work; PPISP producer2/C2v3 — **both-AMD source microprice native/fullCPU accepted; A4 paused/incomplete** | Same qualified step10,000/horizon30,000 packet, B1/4080×3060/1Mrows/1195views; producer2 uniform runtime-null selection, no DCE proof; C2v3 unapplied/unadmitted3,517,362,683B | Full149,817,600B joint/RGB witnesses exact captured SHA, zero differing/nonfinite; input/packet unchanged; original custody monitor43 and build-guard negatives preserved with distinct accepted recovery/build | Joint/RGB launcher+sync medians A0.1566842/0.0017043s, B0.15648645/0.0017213s; paired-ratio medians84.3630/89.8166, not full-wall speed or historical six resident prices; both backing666,465,344B/failures0 | Follow the [coverage-first restart sequence](#preserve-the-current-producer-then-probe-full-coverage): fix prepared custody, prove producer preservation, then admit footprints v2/full1195 probes. Historical572,664,514B metadata headroom/gap2,944,698,169B is not admission; no Q/cap/envelope waiver/quality/recovery/release/family closure |

Full-view profiling's checkpoint-coexistence breach and disposable-output
recovery remain resource-invalid as recorded above; they are not qualifying
candidate states. Correct the approximately 2.7 GiB/group forecast and freshly
admit any rerun. No conditional family is closed by mapped/once/PPISP bounded results.

Mapped smoke's source/packet fields, complete million-row IDs/events and two
native checkpoint arrays were independently checked; byte-identical owner TAR/PLY
paths were hardlinked without changing bytes. Hash/copy phase counters are
overlapping subsets, not additive wall. Read-only packet-cost analysis finds
one producer FNV pass and one actual receiver pass per packet, not duplicated
validation to delete. Reducing full state/moment/old-gradient capture/transfer
requires a distinct partial-state protocol: conditional and **not implemented**.

**Historical intermediate custody:** A2 partial-install's stable-ID-aligned
v1 migration restored all73/1,401,709,376 B exact and passed isolated native pair
merge/body equality; intermediate wrapper753,289,368 B replaced849,147,066 B.
Those compact/aligned migration/native-consumer records remain unchanged history.
**Current custody:** `A4-A2-peer-separated.science-successor.json` and
`.physical-cutover.json` supersede that retired wrapper after complete isolated
71-main + both-peer full SHA proofs, exact73 science union. Source/merged stay
raw; archive-restored baseowner PLYs must be installed before main decoding,
then corresponding main native TAR installed before peer decoding. Failed-codec
archive/declaration/proof and corrected helper are retained and bound above.
Current partial-transfer/owner1-STOP/A3-control successors remain bound above.
No scientific edits or new training/numerical-recovery qualification.

Bound each experiment before launching it; develop one primary mechanism or
measured-cause refinement at a time. Further refinements require new observed
evidence, a distinct falsifiable cause and a decisive finite check, not another
cadence guess. Correctness repairs remain mandatory. Do not close a promising
family merely because an arbitrary experiment count was reached.
Choose exact cadence/schedule/transport from native evidence before each trial.
No blind parameter grid, indefinite partition tuning, new objective or speculative
overlap claim. A bounded native payload experiment may establish admission;
generic bandwidth tests or extrapolated savings alone cannot pass it.

Work on one GPU candidate at a time under fresh admission; independent source/
implementation/review work may run concurrently. After a family is exhausted,
record why and advance. Before settling a winner, disposition every admitted
contender that could reverse the choice; do not implement merely hypothetical
architectures in search of a universal optimum.

**Exit:** candidate queue exhausted by evidence, or viable candidates routed to
A5 with all relevant exclusions recorded. Failed A5 candidates return here.

#### A5 — qualify the actual end-to-end path

Freeze the replacement-dataset candidate configuration and common endpoint
after diagnostic development. Start with exactly three fresh independent
unprofiled candidate/native repetitions on that same dataset with predeclared
order and identical source/evaluator identities. Use the A1 quality rule and
report work differences, not just update counts. Cold and valid reusable
preparation get separate complete-wall totals: discovery, staging, startup,
training, all refresh/reconciliation, required checkpoints, collection, merge,
readback and publication. Evaluation is separate or included identically.

A speed selection requires lower median wall and nonoverlapping observed timing
ranges for the selected mode. If spread can reverse the ranking, run one further
predeclared batch of three matched repetitions and use all six accepted timings.
Recompute candidate run-aggregate and per-view medians over **all accepted
candidate repeats** against the frozen original three-native-repeat envelope.
Report additional native quality results as a stability check, without widening
that envelope. Added candidate regressions can therefore revoke an earlier pass.
No favorable-run selection or widening the quality rule. If timing remains
unresolved, there is no demonstrated speed win. Profile separately and return
only a measured, decision-changing cause to A4. Never shorten primary work.

Qualify long recovery separately from uninterrupted primary timing: original
source through the full candidate trajectory, late STOP/resume and an
attempt-owned killed-worker event against its uninterrupted counterpart. For
refresh variants include failures immediately before/after a generation commit.
Require exact accepted exposure lineage, owner coverage, frozen/shared state,
generation lineage and clocks, plus the unchanged decoded `1e-4` state screen
where applicable. Run source-native same-path controls for any new exact variant.
Short structural smokes, exact immediate save/load or similar aggregate PSNR do
not waive Q's long failure. Diagnose/fix native or orchestration causes and rerun
the affected full check; do not relax the comparator or drop resume to win.

Verify real supported CLI invocation, checkpoint readback, native render/eval
and mesh-input consumers, mixed/stale/partial refusal and no-replace completion.
Update affected callsites, schema consumers, tests and existing docs. Ordinary
native and historical B2 contracts must remain correct. Power-loss durability,
full meshing or unsupported hardware are not claimed by these checks.

**Exit:** complete comparable-quality timing and candidate-specific long recovery
evidence. A failed required gate disqualifies selection and returns to A4, not to
a user-approval pause. Old Q remains FAILED even if a new variant qualifies.

#### A6 — settle, document and stop

Completion requires all of:

1. A runnable implemented split-GPU path passing A1/A5 quality, complete-cost,
   safety, recovery and consumer gates, with cold/reuse scope named; **or** an
   evidence-backed **no qualifying split-GPU winner within this campaign** after
   every justified family is dispositioned. A negative result retains native,
   not a claim that a split path was selected or is universally impossible.
2. No unfinished admitted decision-relevant candidate, attributable defect or
   necessary native verification. Completing one ablation or writing a follow-on
   plan does not meet this condition.
3. Current master/scorecard/usage/testing records, exact runnable commands,
   configuration and schema contracts, original failures, all accepted/excluded
   attempts, artifact hashes/dependencies and resource/quality/cost limitations.
   Preserve required evidence and enforce the [three-class cleanup exit gate](#aggressive-dependency-aware-cleanup).
4. A final recommendation naming what to run and why. Retain experimental
   throughput-only options separately; never hide a quality/recovery failure
   behind the historical B2 win or silently adopt an unapproved quality tradeoff.

If an external prerequisite is genuinely unreachable, finish all independent
work and report **BLOCKED, NOT SETTLED**, the exact missing prerequisite, attempts
to resolve it and the next executable action. After explicit replacement-plan
execution authorization, resume automatically when a prerequisite becomes
available within that same authorization. Resource limits, supported-device
availability and protected evidence are real stop conditions; experiment/phase
boundaries and ordinary design choices are not.

Ask only for an actual out-of-scope user decision, credentials/permissions,
unreachable external inputs or a limit change after exhausting safe alternatives.
No push/PR, unrelated deletion, credential provisioning or CUDA/NVIDIA work is
authorized. Broad release/GUI/capacity work does not start automatically after
settlement; the evidence-qualified headless decision is the finish line.

### Production release gates (not benchmark entry gates)

Production still requires an application-specific quality/approximation decision,
boundary and held-out validation where relevant, topology correctness and the
required recovery/durability guarantees. Current native B2 sidecar/config/view
binding, latest-format owner resume, failure/publication and render/eval/mesh-input
consumer behavior are proved for the exercised bounded scenarios, superseding
those old B1 gaps without upgrading historical runs. Power-loss durability,
arbitrary long exact owner resume, full meshing, oversized/heterogeneous capacity,
GUI/remote operation and globally resumable merged optimizer state remain
unproved. These are separate release/extension work, not prerequisites for this
workload comparison or automatic approval from a throughput win.

This policy supersedes the historical X4 24 dB/three-hour and 19 dB screens,
boundary-regression thresholds and B1's provisional 20% stop rule. Preserve the
recorded measurements and historical decisions; do not relabel them as production
passes. Historical performance tables measure fixed-work time, not time-to-quality.
A1/A5 adds native-relative quality and observed complete cost for settlement;
it does not rewrite those historical benchmark definitions.

Build and test according to [testing](../testing.md) and the M0 instructions.
Use `headless` for CPU/control checks, `worker` when scheduler/protocol code
changes, and the relevant native engine/kernel checks on supported hardware for
numerical changes. Follow [codegen](../codegen.md) for Slang/generated outputs.
New numerical comparisons use a resident control on the active Vulkan backend;
the legacy CUDA-reference workflow is not an acceptance dependency.
Keep all new production paths in native C++; no Python runtime/build dependency,
alternate numerical trainer, or CUDA/NVIDIA implementation/validation requirement.

## 11. Research evidence and current disposition

The design brief's [Grendel](https://arxiv.org/abs/2406.18533) and
[CLM](https://cs.nyu.edu/~apanda/assets/papers/asplos26-cvm.pdf) references motivate
ownership/routing and early CPU-offload experiments respectively. Report their
batch, hardware, model state and timing boundaries when making comparisons;
converged-model throughput and changed global batches do not establish equal
time-to-quality. Their reported performance is not Spirula validation, nor proof
of heterogeneous Vulkan/host-staged efficiency.

ADMM/consensus, stale updates, aggressive compression and representation-changing
LOD remain separately justified experiments if simpler modes fail. They are not
hidden prerequisites of the first implementation.

Historical milestone: **B2-0 through B2-5 complete, bounded cold and valid reuse
selection made** in the [2026-10-01 integrated scorecard](multi-gpu-training-comparison-20260929.md#2026-10-01-integrated-b2--selected-cold-and-reuse).
Three fresh native groups and controls, each with 1,200 aggregate measured
exposures, passed mandatory source/view/coverage/clocks/merge contracts;
preparation, full checkpoints, final publication, resources and release limits
are recorded separately from historical B1. The
[runnable invocation and owner recovery contract](multi-gpu-training-b1-execution.md#historical-bounded-native-b2-path)
support reproduction. Historical visibility/nonfused/synthetic/pattern and
G1/O1 diagnostics below remain separate evidence, not pending B2 work.
The native FPBO external-float-gradient seam passes
actual-source and edge owner-update parity, partial tails, frozen context
and failed-publication/stop/readback gates. The bounded persistent-worker
resource smoke and three fresh G1/control repeats completed. This staged
G1 schedule is 7.036× slower in complete wall than its matched resident
control; the measured decision declines it as a speedup on this source.
The [G1 implementation record](multi-gpu-training-g1-implementation-20260929.md)
retains the executed gates and exclusions. Its C0–C4 staging/context
continuation is also closed: C3 correctness passed, C4 performance admission
failed against fresh native-resident controls; no current G1 cost candidate
or new long repeats are open. The [O1 screen](multi-gpu-training-o1-screen-20260930.md)
completed six source-native diagnostic updates and same-input exact oracle
replays/readback, observing every codec-pair union dirty across all 3,907
blocks and dense million-row clocks. O2 was not admitted: conservative host
render discovery plus native block-range update and bounded partial-gradient/state
servicing remain unestablished. Its first O3 branch is one precise
missing-mechanism record, not an eviction/reload micro-plan or architecture
impossibility result. No new resident speed reference or repeats ran;
diagnostic wall and retained pool accounting do not prove speed/capacity.
The [M0–M4 mechanism continuation](multi-gpu-training-o1-screen-20260930.md#completed-m0m4-continuation--m0-resource-admission-rejected)
is now **CLOSED through M0, NOT ADMITTED**. Fresh hashes match the original
source/config; native binary and 14 source bindings are unchanged. AMD R9700
`vulkaninfo` summary/full exited 0; fresh budget/usage was
33,250,242,560 / 0 B and free scratch 18,821,275,648 B.
Its predeclared retained-allocation cap was **7 GiB = 7,516,192,768 B**,
below the 8,019,788,176 B resident accounting, not device exhaustion.
Accounted host and sampled aggregate RSS caps were each 8 GiB, pinned 16 MiB,
live outputs 10 GiB and minimum scratch 10 GiB. The schedule specified sequential
128/256 whole 256-row optimizer blocks, at most 850,000 render rows / 2,000,000
tile intersections, retained full-image loss/shared/native ordered-raster buffers and no
full Gaussian device copy; some full-row projection bookkeeping remained.
Even optimistically removing all 560,441,660 B full Gaussian allocations and
15,628 B unrelated B1 ownership leaves **7,459,330,888 B** native non-splat
retention. The required 822,825 candidate rows add **121,823,600 B**
(46,078,200 non-SH + 75,719,680 native padded SH + 25,720 bounds).
The **7,581,154,488 B** floor exceeds the cap by **64,961,720 B** before
optimizer chunks, mapping, additional staging and unknown overhead.
This specific capacity-ledger rejection is not a measured simultaneous peak
or an architecture-impossibility result. **M1–M4 are NOT RUN**; no transfer
pricing, repeats or eviction implementation was admitted. Their original plan
remains a reproduction/conditional contract. Local `mechanism.manifest.json`,
`schedule.json` and `admission-ledger.json` under
`o1-mechanism-20260930T175846Z` bind commands, identities and arithmetic;
private paths/UUIDs stay local. No new dev build, headless run or native
six-update repeat ran: relevant code/inputs are unchanged.
There is **no admitted next implementation for the rejected 7 GiB schedule**.
Another concrete cap/retention schedule requires a separate gate, not silently
raising this cap. O0–O3 remains closed; O1 stays unverified, G1 closed, and
the native resident baseline / historical B1 lead remain retained.

The separate [R0–R3 retention candidate](multi-gpu-training-o1-screen-20260930.md#completed-r0r3-execution--ordered-scratch-only-candidate-rejected)
is also **CLOSED, NOT ADMITTED**, with unchanged inputs/native/relevant source
hashes and fresh AMD Vulkan/scratch snapshots. The source-bound lifetime
inventory preserves transform aliases, caller-visible renders and densification
statistics. One independent ordered-scratch-only release/reacquire schedule
requires 6,144,000,000 B partials + 32,000,000 B keys/permutations at the
declared 2M intersections; native padded 850,000-row values add 125,842,760 B.
The backward retained-capacity lower bound **7,801,278,064 B** exceeds the
unchanged cap by **285,085,296 B** before unresolved map/discovery/servicing/
alignment/nonpool/host/pinned/serialization obligations. Complete peak/margin
is unknown; post-reduction releases are not global savings. The local
`o1-retention-20260930T183046Z` manifest binds references, source owners,
commands, lifetimes, cycle, lower bounds and an arithmetic verifier.
No fresh M0, offload code, build, native replay or timing ran; histories and
the native baseline/historical B1 lead are unchanged.

The [M0 execution record](multi-gpu-training-m0-execution.md),
[B1 execution record](multi-gpu-training-b1-execution.md) and
[timing ledger](multi-gpu-training-timing-20260929.md) preserve historical
observations, including then-limited scratch. O1 dirty codec-block coverage
is now observed; candidate-required writeback/staging costs remain unknown.
Mutable G1 shared-state/statistic reductions, true driver/host/pinned peaks,
oversized capacity and production recovery remain unverified. Frozen G1
stat/appearance payloads and their transfers are measured, not zero.
B2 retains **selection for the demonstrated cold and valid reused-preparation
throughput benchmark**, with B2-0–B2-5 complete. Q0–Q4 then failed longer-history
recovery qualification and found material quality regression. D0–D5 completed
with exact immediate save/load, no-save variability and persistent equal-row
quality loss. V0–V5 view-policy ablation completed with partial gap reduction,
remaining material per-view loss and no acceleration proof. Native remains default.
O1's tested schedules remain parked/rejected and G1's tested schedules closed.
The prospective A0–A6 campaign defines the next bounded decisions and materially
different candidates admitted by A4, subject to the current execution/budget
approval gates; it grants no execution permission now. Historical throughput
selection is not its finish line. No additional dataset or absolute quality
floor is a benchmark entry gate.
