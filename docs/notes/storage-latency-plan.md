# Storage latency: validated opportunities and ranking

Status: fork-only plan on branch `plan/storage-latency`, 2026-11-16. It is a review record and roadmap, and it authorizes no execution. Line numbers refer to `upstream/master` at `8949092e`, whose `src/` is identical to the fork's 2026-11-16 upstream merge. The implementation plan for rank 1 is [mask-publish-pipeline-plan.md](mask-publish-pipeline-plan.md). The multi-GPU geometry plan, a separate lane, owns `src/app/cli/geometry_main.cpp`.

## Requests (owner's words)

1. 2026-11-16, paraphrased: evaluate every process for storage-latency bottlenecks, and for opportunities to queue future reads or cache writes so the CPU and GPU do not wait.
2. "Review the proposed opportunities. Validate each one and rank which one should be done in order by the greatest impact."
3. "Create a master plan that contains the list of rankings and any details needed to refer to this later. Then, create a plan for #1 and have fable review."
4. "Push the plan to our repo and give me a handoff in which another dev box can pull the plan, execute in a branch for a upstream PR submission later."

## Why the ranking is about network shares

The first evaluation ranked items from a benchmark campaign whose workspaces were on local NVMe. In production, datasets, workspaces and training outputs live on an **SMB share**, where per-file latency dominates.

- **Share, measured read-only from the reference workstation:**
  - first open plus a 4 KiB read: 7.2–7.5 ms per file;
  - repeat open of the same file: ~0.2 ms;
  - stat after enumeration: 0.26 ms;
  - sequential read: 157.8–159.5 MiB/s;
  - Spirula's own writes: 73–114 MB/s (checkpoint file timestamps).
- **GPU busy:**
  - The local benchmark keeps both GPUs 88–96% busy during masks and normals.
  - The production run on the share kept them ≈31% busy during masks: ≈0.48 s of GPU work per frame (`encode_image` + `segmentConcept`) at ~1.3 frames/s on 2 GPUs.
  - Normals' steady state was ≈20% busy (DERIVED).
- **Reference workstation:** 2× Radeon AI PRO R9700 (32 GB each), 31.2 GiB RAM, Windows. Release 2026.11.12 (667e353), whose video extraction used the built-in decoder (patent-gated code present). Local builds default to patented OFF.

## Evidence

The raw evidence stays outside the repository because it names private captures and machine paths; the numbers below are copied from it. Tags:
- **MEASURED:** timed on the reference workstation.
- **DERIVED:** arithmetic on measured values.
- **INFERENCE:** not measured.

| Source | What it gave |
|---|---|
| Production dataset prep, 2026-11-15 (`logs/prep_*.log` in the dataset) | Two source videos of 13.7 GB each (27.46 GB total): one at 5760×3240, one dual-fisheye at 3840² per lens. 3,332 frames, 13.5 GiB. Wall time ≈13,650 s: frames 2,095.8 s, masks 2,833.9 s, SfM 3,736 s (extraction 6:30, matching 10:55, mapping 44:50), normals 4,530 s |
| Same dataset's outputs | `masks` 0.525 GiB; `feature_masks` 0.495 GiB; `normals` 6,640 files, 3.27 GiB; `features` 5.927 GiB; `images_skipped` 313 files, 1.35 GiB; `sparse` 0.466 GiB; `matches.bin` 285,842,189 B (35,492,917 correspondences) |
| Live training run on the share, 2026-11-15 | 60,000 iterations; `steps_per_save` 2000; `cap_max` 2M; `cache_images` disk; 3,320 cameras; ≈1.15 s/step; GPU 94–104% busy while reading 17–26 MB/s. One checkpoint: `splat.ply` 495,964,580 B in 6.32 s plus `state.tar` 870,706,688 B in 8.21 s |
| Local benchmark reference (4,412-image full-360 capture, NVMe workspace) | Frames 138.2 s, masks 1,235.2 s, SfM 1,472.9 s, normals 1,163.6 s; total 4,009.8 s |

## Ranking by expected impact on the production path

Expected saving is per dataset-prep run (≈13,650 s wall) unless stated.

| Rank | IDs | Opportunity | Verdict | Expected saving (ceiling) | Evidence | Fix | Owner |
|---|---|---|---|---|---|---|---|
| 1 | PERF-020/021 | Mask results are accepted inside the dispatch mutex on the worker's own thread, and mask scratch lives in the workspace on the share | Mechanism confirmed; cause split unmeasured | Masks 800–1,200 s, optimistic until measured (ceiling ≈1,750 s; GPU-bound floor ≈800 s) [INFERENCE] | MEASURED 3,332 masks in 2,834 s against ≈0.48 s GPU work per frame. Admission fits one task per worker but not pipelining (DERIVED) | M | [mask-publish-pipeline-plan.md](mask-publish-pipeline-plan.md); the normals share belongs to the geometry plan |
| 2 | PERF-022/023 + mask pass barrier | Serial image hash and scan passes before the GPUs start; masks restart per input | Confirmed | ≈450 s (windows total ≈803 s) | MEASURED windows: 74.8 s and 162.4 s mask pre-hash; 188.6 s pass boundary; 231 s parent and 146 s worker geometry scans | M | Mask part: this lane, after rank 1. Geometry part: geometry plan |
| 3 | PERF-010 | Full SHA-256 of both 13.7 GB sources at SfM-runner start and again after extraction | Confirmed | 150–250 s of 358 s | MEASURED unlogged windows of 204 s and 154.3 s; code order verified | S–M | Unplanned |
| 4 | PERF-060/061 | Checkpoint written under `engine_mutex`; world buffers read back twice | Confirmed | ~6–7 min per 60k-step training run (~0.6%) | MEASURED 14.5 s of writes per save at 2M splats; DERIVED ×~29 saves. D2H and pack time not timed | M–L | Unplanned |
| 5 | NEW-S1, PERF-030/032/031 | SfM on the share: serial header probe and mask index; descriptor first touch via mmap; serial EXIF sweep; match and graph spills in the workspace | Immaterial on local disk; confirmed on the share | ~90–105 s, plus ~135 s of unexplained windows | MEASURED 36.5 s probes and 20.8 s EXIF sweep. DERIVED 34–45 s descriptors; 5–8 s spills | S–M each | Unplanned |
| 6 | PERF-011 | The video mutex serializes the two extractions (built-in decoder + `skip_blurry`) | Adjusted: not taken in a patented-OFF build | Ceiling ~500 s; expected value unclear | MEASURED 518 s then 498 s, back to back. The mutex guards a shared Vulkan stream; not a storage item | M–L | Unplanned |
| 7 | PERF-052 | Serial header probes at training start | Confirmed | ~45 s per training start (≈51 → ~4 s) | DERIVED from the measured first open | S | Unplanned |
| 8 | PERF-062 | Full SHA-256 of the model on every id-based CLI cache hit, then the loader re-reads it | Confirmed | 9–15 s per geometry stage | DERIVED | S | Unplanned |
| 9 | PERF-014 | Blur-rejected frames written to `images_skipped` on the share | Adjusted: real with `skip_blurry` | ~10 s (≤40 s) | MEASURED 313 files, 1.45 GB | M | Unplanned |
| 10 | PERF-082a, 012, 080, batch polls | Interactive stalls (see the verdict table) | Confirmed | Photo listing ~8–15 s per 1,000 photos; PERF-012 ~0.6 s per UI frame per 1,000 photos (photo inputs only); 60–200 ms navigation; 20–45 ms hitch every 2 s | DERIVED | S each | Unplanned |

**Do regardless of rank (correctness or waste, small fixes):**
- **Mask resume regenerates everything.** `DatasetPrep.cpp:3717` sets `d.write = isolated || …`, so a resumed isolated mask run redoes every mask: up to a full stage (2,834 s on the share). S.
- **PERF-050:** in DISK mode with `validation_fraction > 0`, val batches are staged and never consumed, so publishers block forever after ~20 batches (`DataManager.cpp:2388-2411, 2505-2514`; `next_val_batch` has no caller). The option is expert tier and defaults to 0 (`TrainConfig.h:166`). Confirmed statically; not reproduced. S.
- **PERF-064:** `read_into_device` ignores failed reads (`CheckpointIO.h:222-234`), and `adapt_checkpoint` does not check the stream after `tar_finish` (`Adapt.cpp:803-811`). S.
- **Non-atomic writes:** `save_batch_list` and `write_preset_file`. Low severity.

**Found while validating (not storage), owned by the geometry plan:**
- With per-image intrinsics, the parent plans 3,320 per-camera warps (250 s), and each worker repeats it (~398 s).
- Large-frame pairing starves the second GPU: its second worker started 13:22 late.
- Together with the scans, the GPUs sat idle ~1,050 s between SfM and the first normal (MEASURED).

**Unexplained windows (need per-call timing on the share):**
- SfM: 34.8 s rig-mates → compaction log; 32.9 s compaction → "Reconstructing cameras"; 68 s "Placed cameras" → "Wrote model".
- 47.4 s of silence after the last mask. This is consistent with deleting the ~3,332-folder scratch tree from the share [INFERENCE]; the rank-1 plan's per-frame cleanup removes the end-of-stage bulk delete.

## Verdict for every catalog item

| ID | Anchors | Verdict | Impact on the production path |
|---|---|---|---|
| 010 | `DatasetPrep.cpp:1265-1308`; `SfmRunner.cpp:840`; `ColmapRunner.cpp:514`; recheck `DatasetPrep.cpp:1812-1821` | Confirmed | 358 s MEASURED. Remedy: overlap the first pass with the motion scan and the recheck with the mask start; hash inputs concurrently; keep the recheck |
| 011 | `FrameExtract.cpp:128-135, 596`; wave `DatasetPrep.cpp:1632-1653` | Adjusted | The release build serializes; ceiling ~500 s; not storage |
| 012 | `GuiApp.cpp:4753` (call sites 9116, 5536, 7239); `DatasetPrep.cpp:1260-1287` | Confirmed | Photo inputs only; ~0.6 s per UI frame per 1,000 photos. Memoize within the 1 s refresh |
| 013 | `DatasetPrep.cpp:3501-3560` | Refuted for production | No packed-lens video; ≤10 s locally |
| 014 | `FrameExtract.cpp:520-525`; `DatasetPrep.cpp:2241-2245, 1755-1762` | Adjusted | ~10 s |
| 015 | `Mp4Demuxer.cpp:423-424` | Unverifiable | ≤14% of decode time [INFERENCE] |
| 016 | `Subprocess.cpp:226-262, 285-325` | Negligible | <1 s |
| 017 | `DatasetPrep.cpp:3411-3414`; `FrameSelect.cpp:226-241` | Refuted for defaults | — |
| 018 | Batch rows run serially | Low, by design | ≤0.1% |
| 019 | `SourcePreview.cpp:677` | Confirmed, low | ~20 ms per selected photo |
| 020 | `InferenceWorker.cpp:126-160`; masks `DatasetPrep.cpp:3742, 3833-3945`; geometry `geometry_main.cpp:1376, 1458-1490` | Confirmed | Rank 1 |
| 021 | `DatasetPrep.cpp:3782`; `MaskWorker.cpp:174, 186, 205` | Adjusted | Folded into 020 |
| 022 | `DatasetPrep.cpp:3770-3777`; per input `:1910` | Confirmed | Rank 2 |
| 023 | `geometry_main.cpp:1040-1105` (hash 1071, completeness 1087-1089) | Confirmed | Rank 2; geometry plan |
| 024 | Marigold streaming | Refuted | Model unused |
| 025 | `FrameMask.cpp:1141-1142, 1219` | Refuted for defaults | Needs a stencil |
| 3717 note | `DatasetPrep.cpp:3717` | Confirmed | Do regardless |
| 030 | `Features.h:281`; `Matcher.h:52, 507, 537`; `PairSelection.h:73, 187, 219` | Confirmed on the share | 34–45 s (ceiling ~60) |
| 031 | `Pipeline.cpp:2769, 2822, 2848`; `Spill.h:53` (256 MiB) | Refuted locally; confirmed on the share | 35.49M correspondences spill into the workspace; 5–8 s |
| 032 | `Pipeline.cpp:484, 1281`; `Exif.h:291` | Confirmed on the share | 20.8 s MEASURED → ~1.5 s in parallel |
| 033 | `Features.h:206-239`; `Pipeline.cpp:1632, 2737, 2745` | Immaterial unless reuse hits | ~25 s on the share when reuse runs |
| 034 | Extraction SerialWorker, journal, progress | Low | <1 s; feature writes of 56–87 s on the share may or may not block |
| 035 | `Mapper.h:3383-3391` | Refuted locally; untested on the share | The graph spill triggers on the production capture |
| NEW-S1 | Header probe and mask index before extraction (`Pipeline.cpp:~1680`) | Confirmed on the share | 36.5 s → ~3 s |
| 040–043 | ExternalSort, prediction cache, tail hashes, tile flush | Mechanisms confirmed, small | 1–5 s each per dense run. Production presets disable dense |
| Lower (dense, LiDAR) | MarkerScan, SparseEdit, E57Reader, LidarDataset, E57 PNG | Small or refuted | MarkerScan up to ~35 s per 4,412-image scan (rare) |
| 050 | `DataManager.cpp:2388-2411, 2505-2514`; `TrainerCore.cpp:1087-1088` | Confirmed (static) | Opt-in liveness bug |
| 051 | `TrainerCore.cpp:2575-2590` | Adjusted | Eval is off by default (`TrainerCore.cpp:2355`); <5 s |
| 052 | `DataManager.cpp:1344-1382`; `ImageProbe.cpp:60-75` | Confirmed | Rank 7 |
| 053 | `DataManager.cpp:1442-1458`; `DownscaleCache.cpp` | Hidden | Prefetch hides it; live GPU ~100% busy |
| 060/061 | `TrainerCore.cpp:2075-2083`; `EngineCheckpoint.cpp:195-437`; `CheckpointIO.h:100-124` | Confirmed | Rank 4 |
| 062 | `Fetch.cpp:105-109`; `Sha256.h:109-120`; `Onnx.cpp:226` | Confirmed | Rank 8. Remedy: hash the bytes the loader reads; no size/mtime stamps |
| 063 | Marigold install progress | Negligible | — |
| 064 | `CheckpointIO.h:222-234`; `Adapt.cpp:803-811` | Confirmed | Do regardless |
| 065 | `TrainerCore.cpp:1833-1846` | Refuted as impact | — |
| 066 | Model loaders | Negligible | MoGe load 1.3–1.5 s MEASURED |
| 067 | `Fetch.cpp:150`; `ModelCache.cpp:424` | Robustness | A failed download re-fetches 1–2 GB |
| 068 | `SplatMerge.cpp:72`; `MaskPrompt.cpp:307-337` | Refuted | — |
| 069 | Background SH export in the save lock | Off by default | — |
| 070 | `encode_main.cpp:140-150`; `VideoEncoder.cpp:1071-1078` | Confirmed, patent-gated | ~5–10 s per long export [INFERENCE] |
| 071 | `FrameSink.cpp:377` (`+faststart`) | Not worth doing | — |
| 072 | `MeshingHost.cpp:1952, 2022` | Overstated | ≤1 s |
| 073 | `MeshImport.cpp:52-60` | Confirmed, small | ~0.3–0.5 s |
| 074 | Demuxer seeks | Adjusted; don't do | ≤0.7 s |
| 080 | `MaskSession.cpp ~507, 978-996`; `MaskLayer.cpp ~275-340` | Confirmed | 60–200 ms per saved navigation |
| 081 | `MaskLayer.cpp:381, 436-455` | Low | — |
| 082a/b | `SourcePreview.cpp:342, ~375`; `Picture.cpp:174` | Confirmed | Listing ~8–15 s per 1,000 photos on the share (DERIVED); thumbnails CPU-bound |
| 083 | `SplatViewer.cpp:342`; `CompareView.cpp:507` | Small | ~1 s |
| Lower (GUI) | Batch polls `GuiApp.cpp:1590`; `license_accepted` `LicenseConsent.cpp:129-144`; `save_settings`; preset writes | Polls confirmed; `license_accepted` refuted; the rest trivial or low | 20–45 ms hitch every 2 s while the Batch screen is visible |
| 090 | `WriterPool.h:54-69` | Refuted as a RAM risk | Queue ~1.5 GB at 4K; measured peak commit 11.75 GiB; the queue never fills |
| 091 | `RunReport.cpp:124-146` | Trivial | — |
| 092 | `TrainPerfLog.h:71` | Fine | — |

## Decisions and rejected remedies

- Rank by the production share path, not the local benchmark. The local numbers remain the regression baseline.
- Keep every integrity recheck: overlap or parallelize hashes, and never replace content hashes with size or mtime stamps.
- No hardlinked compatibility copies (dense). WriterPool is not a checkpoint writer. No bare 16 MiB demux buffers.
- One owner per file across plans:
  - the geometry plan owns `geometry_main.cpp`;
  - rank 1 owns `src/app/InferenceWorker.{h,cpp}`, `src/app/tests/inference_worker_test.cpp` and the isolated-mask section of `DatasetPrep.cpp`.

  Rank 1's dispatcher change applies only to callers that pass a `prepare` step, so geometry dispatch is unchanged. Geometry timing runs should record the dispatcher commit, and rank 1 must not land between the geometry plan's A and B arms.
- Temporary files never go under `masks/` or `feature_masks/`: `sfm::MaskIndex` (`src/sfm/core/Mask.h:298-325`) claims every regular file there.

## Measurements still needed (idle machine)

1. **Rank 1:** accept time, worker round trip, and the share against a local workspace (rank-1 plan, P0).
2. **SfM on the share:** per-call timing for the unexplained windows and for the 47.4 s window after the last mask.
3. **Training:** `SS_TRAIN_PERF=1` data-wait and save seconds on a share-hosted training run.
4. **`sha256_file` rate:** release build against a current build (buffered file I/O landed after the release).
5. **Photo listing:** cold and warm, on the share.
6. **Dense:** one run with `--log-performance`, only if dense comes into use.

## Ledger

- 2026-11-16: Review complete. Every catalog item has a verdict with evidence, and the ranking is published. Only read-only probes ran on the share, beside a live training run; no builds, benchmarks or app runs.
- 2026-11-16: Rank-1 plan revision 1 failed an expert review (Claude Fable) with eight findings. All were checked against the code and accepted; revision 2 is in [mask-publish-pipeline-plan.md](mask-publish-pipeline-plan.md). Not re-reviewed.
- 2026-11-16: Plans pushed to the fork on branch `plan/storage-latency` for execution from another machine. Private evidence stays off the repository.
