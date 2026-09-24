# HEVC decode admission and safe extraction

Status: implemented and validated on local AMD Vulkan; see "Execution results" below.

## Baseline and goal

- Branch: `fix/hevc-decode-capability`.
- Upstream: `183b2c6df72f42ecb9a0500cdad96749847da171` (`v2026.9.24`).
- Local policy commit: `e0fc463e`; preserved `AGENTS.md` blob:
  `27dff3c8f27db4b8b26e713553873053efd216c6`.
- This branch starts directly from upstream, not from the existing distributed-worker
  branch. Its unrelated commits and uncommitted work are not part of this change.

Prevent unsupported native HEVC decoding from silently producing corrupted dataset
images. The reported dual-track capture must extract cleanly without silently
losing its requested adaptive selection or synchronized camera instants.

## Evidence and uncertainty

Previous investigation used the downloaded 2026.9.20 executable, not this branch:

| Observation | Established result |
|---|---|
| Capture | Two HEVC Main 10, 3840 x 3840, 24 fps tracks; declared level 6.0 (`general_level_idc = 180`) |
| Selected devices | Two AMD Radeon AI PRO R9700 GPUs; proprietary 26.Q3 driver, Vulkan driver version 2.0.395 |
| Matching Vulkan profile | Reports HEVC maximum level 5.1; maximum coded extent 8192 x 4352 |
| Native output | Reproducible corruption on both GPUs, including lossless PNG output |
| Tile layout | Four explicit columns, with boundaries at x = 960, 1920 and 2880 |
| Container control | Packet-copy MP4-to-MKV remux retains essentially the same damaged frame 623 |
| Independent decode | FFmpeg software output is clean; D3D11 decoding of the first frame is clean |
| Validation layer | One-frame release run emitted no warnings; that is not pixel-correctness proof |

At frame 623, mean adjacent-column grayscale jumps in native PNG were
30.26 / 52.23 / 46.26 at those boundaries, versus 2.67 / 3.31 / 5.30 in
software-decoded luma. Different color conversion paths mean these are localization
metrics, not an exact native-vs-reference pixel comparison.

**Confirmed source defect:** the upstream pipeline queried HEVC capabilities but
did not compare parsed SPS levels against `maxLevelIdc`. The new admission check
rejects this level 6.0 capture on the selected device's maximum of 5.1.
Full-resolution raw-plane downloads at the old decoder's output showed tile-edge
corruption already in decoded Y and UV before RGB conversion: frame 0 matched
software Y exactly, while frame 623's Y had mean absolute error 18.445/1023
(track 0) and 16.253/1023 (track 1). Frame 624 is an IDR and its tile seams
recover. The stream exceeds the advertised capability, but these observations
do not establish whether the level mismatch, another bitstream/state defect or
the driver produced the corruption. No GPU/driver blame or stream relabeling is
implied.

The [Vulkan HEVC capability specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoDecodeH265CapabilitiesKHR.html)
defines `maxLevelIdc` as the maximum supported HEVC level. Width/height support alone
must not substitute for that check. Do not relabel the stream to a lower level.

## Current upstream integration points

| Source | Responsibility and gap |
|---|---|
| `src/video/H265Decoder.cpp` | `parse_ptl`, `parseSps`, `updateFormat`, `decodeFrame`: retains PTL internally, but does not expose/admit its level; slices select an SPS through their PPS |
| `src/video/CodecDecoder.h` | `StreamFormat` has profile/geometry but no level; `PictureInfo::params_changed` exists |
| `src/video/VideoPipeline.cpp` | `createSession` queries `VkVideoDecodeH265CapabilitiesKHR`; `decodeNext` proceeds from codec parsing to `recordDecode` without level admission or handling `params_changed` |
| `src/app/FrameExtract.cpp` | Native extraction, motion scanning and preview extraction share `VideoPipeline` |
| `src/app/gui/DatasetPrep.cpp` | `run` invokes native `plan_group` before `extract_video`; the latter alone has native-to-FFmpeg fallback |
| `src/app/gui/DatasetPrep.h` | Job-wide `force_external_decode`; `frames_stamp` includes the requested engine and extraction settings |
| `src/app/gui/FrameSelect.cpp` | Existing FFmpeg candidate selection; adaptive selection uses resampled candidate frames |
| `src/app/cli/sam_extract.cpp` | Native-only CLI; propagates extraction errors, not GUI FFmpeg fallback |
| `src/app/gui/tests/` | Existing `dataset_prep_test.cpp`, `frames_stamp_test.cpp` and `preset_roundtrip_test.cpp` |

Upstream moved `DatasetPrep` into `app/gui/`. Do not copy old-tree line numbers,
provenance APIs, video tests or distributed-worker changes into this branch.
The upstream FFmpeg extractor currently logs `sync_needs_builtin` and selects each
track separately. It is **not** a drop-in solution for the reported synchronized
job. Its adaptive sampling also is not identical to native source-frame sampling.

## Execution sequence

### A. Establish a current baseline and isolate the pixels

1. Build this worktree with the documented Windows dev entrypoint and a local-only
   patent opt-in. Record commit, compiler, Slang, device UUID, driver and options.
2. Run a bounded reproduction on the selected local AMD device: both tracks, frame
   0 and frame 623, no selection/rotation/resizing, PNG output. Keep source and
   outputs outside the repository. Use the existing release findings as evidence;
   this run establishes the changed upstream baseline, not a repeat to question it.
3. Use temporary diagnostic instrumentation at `VideoPipeline::copyPlanes` to read
   full-resolution Y and UV before RGB conversion. `toGray` is useful for triage
   but still passes through a shader. Compare aligned display frames against
   FFmpeg raw 10-bit planes, accounting for packing, range and crop.
4. Record per-plane errors, tile-boundary errors, control columns and temporal
   behavior through an IDR boundary. If raw planes are clean, investigate conversion;
   if already corrupt, compare parsed PPS/SPS, slice offsets and reference state
   against an independent parser before attributing the failure to the driver.
5. Use a redistributable or synthetic in-capability HEVC control. A no-tile reencode
   changes several variables; it alone cannot prove a tile or level root cause.

Gate: a reproducible current-baseline result and an explicit raw-plane diagnosis,
or a concrete unavailable prerequisite. Keep the missing level check and pixel
root-cause conclusions separate. No device-specific blacklist or prefix workaround
without a controlled failing-before/passing-after experiment.

### B. Enforce HEVC admission in the shared native path

1. Preserve the parsed HEVC level in existing codec metadata with an explicit
   representation; never compare raw HEVC IDC numbers with Vulkan enum ordinals.
   Reject malformed/unknown encodings instead of treating them as permission.
2. Reuse one capability/admission implementation for preflight and actual session
   opening. Check before creating session parameters or allocating picture pools;
   return required level, supported level and device identity in the error.
   Preflight must not create a second decoder policy or a second parser.
3. Cover all parameter sets admitted to the session, not merely the last SPS in
   `hvcC`. Validate the SPS actually selected by each picture's PPS before recording
   GPU work, including switching to an already-parsed SPS and in-band replacement.
4. Define the parameter-update boundary explicitly. Upstream currently ignores
   `params_changed`; do not submit a new SPS against stale Vulkan parameters.
   Accept identical repeated parameter sets without treating them as format changes.
   For a real unsupported or unapplied sequence change, stop before that picture is
   submitted with a clear error. Full dynamic session reconfiguration is not required
   for this fix; it must not be approximated with stale state.
5. Trace exported API references before editing. All native consumers, including
   motion scan, previews and CLI extraction, must receive the same rejection.

Gate: below/equal level accepted, above level rejected, initial and in-band cases
covered, and no unsupported picture submitted. Add focused native behavioral tests
using parsed fixture data, including multiple SPS ordering and repeated headers;
avoid tests that merely compare copied metadata or exact error wording.

### C. Route dataset preparation before planning, preserving intent

1. After resolving the requested device, preflight every selected video track before
   frame estimates, adaptive scans, resume decisions or any output publication.
   Keep build-level availability separate from stream/device compatibility. Invalid
   device selection and cancellation remain errors, not reasons to switch devices.
2. On a known unsupported stream, choose the existing software FFmpeg route for the
   preparation job before native group planning. Reuse the local job's existing
   route setting and stamp machinery; do not persistently change the user's preset.
   Require FFmpeg availability and log the capability reason once.
3. Resolve the upstream synchronized-fallback gap before calling the reported workflow
   fixed. Extend the existing external candidate/selection path to retain aligned
   track candidates and choose one common instant per sharpness window, using existing
   motion/selection arithmetic. Match frames by presentation time, not independent
   filenames; never fabricate a missing mate. Keep common multi-input rate-group
   budgeting where requested and keep all camera outputs on the same schedule.
4. Preserve adaptive intent through the external path. Document its resampled-candidate
   behavior rather than promise identical selected indices to native decoding. Until
   a requested combination can be honored, fail before writing with an actionable
   message; logging and silently ignoring `sync_tracks` is not acceptable completion.
5. Include the effective route in cache/stamp decisions. Route changes or an in-band
   rejection must not leave native and FFmpeg frames, stale masks or stale plans mixed
   in a successful output. Reuse existing scoped cleanup/rebuild behavior, never delete
   source media or unrelated files. Do not resume an earlier corrupt native result.
6. Late native failure must unwind queued writers/decoder owners before any restart.
   If restarting would invalidate a shared plan, restart the affected planning/output
   unit coherently; otherwise fail explicitly rather than publish a mixed dataset.

Gate: real GUI preparation of the reported two-track adaptive/synchronized case
finishes with clean aligned camera images. Missing FFmpeg, cancellation, route-changing
resume and a late sequence change fail/recover without falsely marking partial work
complete. Extend existing preparation/stamp tests for these observable transitions.

### D. CLI, interface copy and documentation

- Keep `sam extract` native-only for this change. Initial rejection returns nonzero
  without images and explains the capability mismatch and GUI/software alternative.
  A late rejection must not claim the partial extraction completed successfully.
- Put new user-facing diagnostics in the existing i18n catalogs; reuse `Log.h`,
  `Cli.h` and `SamHelp.h` conventions. Add no English sentence matching in callers.
- Update `src/video/README.md`, `docs/datasets.md` and `docs/testing.md` after behavior
  is exercised. Document support admission, synchronized fallback, external adaptive
  differences, CLI limitations and how to regenerate previously corrupted outputs.

## Validation matrix and commands

Execute in the new worktree, never the original dirty source/build tree.
The commands below were exercised in the dedicated worktree with the patent
opt-in; their fixture and outputs remained outside the repository:

```bat
build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_SAM=ON -DSS_ENABLE_PATENTED=ON
build_vulkan\spirula.exe sam extract "%HEVC_FIXTURE%" --track 1 --skip 623 --keep 0 --max-frames 2 --360 off --quality 101 --threads 1 --device "%AMD_DEVICE%" --out "%HEVC_OUTPUT%"
```

The extraction command reproduced the baseline before admission; afterward it
rejected the unsupported stream. Each run used a fresh output location.

| Lane | Required evidence |
|---|---|
| CPU/parser | Level boundaries, malformed level, multiple SPS, in-band activation/replacement and harmless repeated headers |
| Supported AMD native | H.265 Main and Main10 decode Y/UV bit-identically to software across tiled/untiled inter-predicted controls; H.264/AV1 remain unaffected |
| Unsupported AMD native | Declared level 6.0 versus device 5.1 fails before first GPU decode; CLI returns nonzero |
| Numerical diagnostic | Raw Y/UV versus software reference and RGB output, with explicit tolerances justified by representation |
| GUI fallback | Two-track adaptive + sync job uses software route before motion planning; matching presentation instants and clean pixels |
| Recovery | Route/stamp transition, incomplete old outputs, cancellation, missing FFmpeg and late SPS rejection |
| Patent-disabled build | Normal GUI FFmpeg path and existing native-only CLI unavailable message still work |

Register any new tests using the existing CMake native-test conventions and run the
focused targets once after integration. Run real CLI and GUI scenarios as well;
passing parser tests alone is not acceptance. Restore the ordinary local build:

```bat
build_develop.bat -DSS_BACKEND=vulkan -DSS_ENABLE_PATENTED=OFF
```

Keep `SS_ENABLE_PATENTED=OFF` in committed defaults. No CUDA builds, NVIDIA runs,
new decoder library, permanent raw-dump command, driver update, or upstream publishing
is required or authorized. Remove temporary diagnostic instrumentation after proof;
retain only regression fixtures whose redistribution is permitted.

## Ownership and completion

After baseline research, the native admission/test slice (B) and pixel diagnostic
slice (A) can run concurrently once temporary instrumentation ownership is separated.
One integration owner defines the small preflight/error contract and owns shared files.
Fallback work (C) depends on that contract; CLI/catalog/docs integration (D) follows it.
Do not concurrently edit `VideoPipeline.cpp` for diagnostics and admission.

Completion requires all matrix lanes relevant to the implementation, including the
actual synchronized GUI workflow. Record unverified hardware/visual lanes explicitly.
A level guard alone is a safety fix, not proof of a particular decoder defect.
The synchronized GUI workflow was exercised as described below.

## Execution results

- Windows MSVC 19.44.35229.0, pinned Slang 2026.12.0.1, AMD Radeon AI PRO
  R9700 (UUID `00000000010000000000000000000000`, driver 2.0.395 / 26.Q3).
  The patented decoder was enabled only in the local validation build.
- `hevc_admission_test`, `dataset_prep_test`, `frames_stamp_test`, and
  `preset_roundtrip_test` pass. They cover level/DPB boundaries, multiple
  SPS/PPS choices and in-band changes, adaptive candidate planning, matched
  presentation times, cancellation, missing ffmpeg, and route-stamp changes.
  `ctest -L headless --no-tests=error` found no registered tests in this
  Windows build; the focused standalone executables above were run directly.
- Native `spirula sam extract` rejects both 3840² Main10 level 6.0 tracks
  before writing frames, reporting required level 6.0 and supported level
  5.1; an in-capability Main10 synthetic control decodes 25 frames and
  writes the requested two images. An invalid UUID remains a hard error.
- The real GUI's synchronized/adaptive preparation of the dual-track capture
  selected software decoding before planning. It published 46 matching
  filename stems per camera from 216 candidates per track; the effective
  ffmpeg route and selection settings were recorded in `.spirula-frames`.
  The complete GUI dataset creation succeeded with 92/92 images registered.
  At a late selected frame the mean adjacent-pixel grayscale jumps at
  x=960/1920/2880 were 7.90/13.23/5.00 (camera 0) and 3.09/3.93/5.99
  (camera 1), versus 46.97/39.26/45.41 and 30.26/52.23/46.26 at the
  native baseline's damaged frame 623. The frames differ in instant and
  codec, so this is a tile-seam sanity check, not pixel identity.
- Cancelling the GUI while software extraction was active left only a log:
  no images or frame stamp were published. Retrying that same workspace
  produced 46 aligned images per camera and completed reconstruction with
  all 92 images registered.
- The ordinary `SS_ENABLE_PATENTED=OFF` Vulkan build completed. Its native-only
  `spirula sam extract` reports the unavailable decoder and exits nonzero;
  the real GUI used ffmpeg to publish 46 aligned images per camera with the
  same selected frame stems as the enabled build and completed reconstruction
  with all 92 images registered.
- A local validation fixture (not committed) was encoded with Kvazaar 2.3.2
  compiled for 10-bit: 48 frames at 1024x576/24 fps, Main 10 level 4.1,
  2x2 PPS tiles, low-delay inter pictures and IDRs at frames 0, 16 and 32.
  A matching one-tile control used the same source. Packet timestamps were
  assigned at 24 fps during MP4 remux; FFmpeg software decoding of each
  Annex B stream and its MP4 produced byte-identical 48-frame YUV outputs.
- On the R9700, native extraction wrote 48 PNGs for each stream. Temporary
  pre-conversion P010 readback compared every Y, U and V sample against
  FFmpeg software P010. For the tiled stream only 17/48 frames matched exactly;
  frame 18 differed at 107 samples, including luma errors of 840/1023 near
  x=512. The one-tile control matched 5/48 frames, with a maximum luma error
  of 267/1023. IDRs at frames 0, 16 and 32 matched in both streams. This
  establishes an in-capability native decode discrepancy before RGB conversion;
  the one-tile failures prevent attributing it exclusively to tiling. On this
  AMD device, in-capability tiled-Main10 **failed pixel-correctness acceptance
  before the DPB view fix**. No fixture was committed.

## In-capability follow-up: AMD native HEVC safety

The level check does not prevent corruption in supported streams. On the same
R9700, 48-frame, 1024x576/24 fps level-4.1 controls were decoded to raw
pre-conversion planes: Kvazaar 8-bit and Main10, each with 2x2 tiles and one
tile, plus an independently encoded x265 Main10 P-only stream. FFmpeg software
and D3D11VA were byte-identical on all frames of every control. Native Vulkan
matched software on only 42/48 tiled 8-bit frames, 7/48 one-tile 8-bit frames,
17/48 tiled Main10 frames, 5/48 one-tile Main10 frames, and 34/48 x265 frames.
The one-tile 8-bit and Main10 controls first diverged on frame 1 while their
IDRs matched.

The official Khronos Vulkan-Video-Samples v0.5.0 `vk-video-dec-test` decoded
all 48 frames of **all five** controls on the same AMD GPU with raw planar YUV
byte-identical to FFmpeg software. This isolated the failure to Spirula's
native HEVC application path, not to HEVC level, tiles, encoder or color
conversion. `vkCmdControlVideoCodingKHR(RESET)` after
`vkCmdBeginVideoCodingKHR` is required to occur inside the video coding scope;
it was not the defect.

Spirula created a separate 2D image view for each layer of one DPB array and
bound those views with `baseArrayLayer=0`. Switching only the DPB binding to
one full 2D-array view, with each resource selecting its slot through
`baseArrayLayer`, changed the first failing one-tile 8-bit inter-frame from
nine mismatched luma samples to zero. All 48 raw NV12/P010 frames then
matched software byte-for-byte on all five controls, including their IDR
boundaries. This identifies the defective DPB view/layer binding on the AMD
device. Both representations can describe an image layer under Vulkan; the
experiment does not distinguish an implementation-specific driver handling
problem from a missing application-side constraint on those views. Keeping a
single shared array view matches the working Khronos path.

After removing the AMD guard and diagnostic instrumentation, native CLI
extraction produced 48 images for each control. An H.264 Baseline control and
an AV1 Main control each wrote eight frames through the shared DPB path.
Vulkan validation reported no issues on a three-frame HEVC extraction.
The original level-6.0 dual-track capture still reports the AMD device's 5.1
limit before writing output; the GUI must continue using FFmpeg for that
capture. `hevc_admission_test`, `dataset_prep_test`, `frames_stamp_test` and
`preset_roundtrip_test` passed. In the real GUI, the fixed in-capability
8-bit control decoded 48 frames natively, measured 12 and published four
images at 2 fps without calling FFmpeg. Its synthetic scene remained
unreconstructable (0/4 registered); the visible GUI reported the SfM failure.
This verifies native GUI extraction, not successful scene reconstruction.
