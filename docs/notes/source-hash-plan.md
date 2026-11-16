# Overlap source-content hashing (rank 3, PERF-010)

Status: revision 2, 2026-11-16. Fork-only plan on `plan/storage-latency`; execution is **not authorized or started**. This session changes only this plan and the [storage-latency ranking](storage-latency-plan.md). Structure and measurement lessons follow [mask-publish-pipeline-plan.md](mask-publish-pipeline-plan.md). Code anchors were checked at [`upstream/master`, `8949092edb41bd60fede5a8cb71a7817e2d924e0`](https://github.com/harry7557558/spirula-studio/tree/8949092edb41bd60fede5a8cb71a7817e2d924e0), still current after fetch on 2026-11-16.

## Goal

Reduce the two full source-content hash passes on the preparation critical path without changing a single identity byte, resume decision, or integrity barrier. Keep both the initial snapshot and the post-extraction recheck.

**Numeric Done for a successful implementation:**
- On the supplied two-video, approximately 27.46 GB SMB fixture, the **median paired saving**, `median(A_i - B_i)`, from runner start through verified Frames completion and `Found N images` is **at least 150 s**. B beats A in each of four pairs under P0's matched repeated-read and SMB-byte protocol. Report A/B medians and ranges too; first-touch samples cannot qualify or be pooled. This establishes only the observed cache-influenced fixture conditions, not cold-share production savings. The ranking's 150–250 s remains a forecast.
- Report the two hash-pass times separately and their combined exposed wait. Do not count work merely moved beyond the measurement endpoint as a saving.
- On the same local-disk fixture, B's median complete-window time is no greater than A's observed maximum. Apply the same non-regression rule to a many-small-photo fixture on both locations; adopt photo concurrency only if its SMB hash-window median improves by at least 20%.
- Every unchanged-input A/B identity is byte-identical, including persisted `source_content`; existing resume/reuse decisions, selected frames and frame file bytes match. Preserve existing mutation-observation guarantees, including the `in-dataset` exception: the specified tested replacements/mutations and failed reads must be rejected without Frames completion or downstream publication. Preserve cancellation behavior and join all owned work; do not promise stronger source isolation or faster cancellation.
- New hash scratch is bounded by **128 MiB total**, including stream buffers and queued chunks, with no per-file large allocation churn. Record actual peak process commit; bounded bookkeeping for the existing inventory is accounted separately.
- Relevant checks and one concurrency-focused code review pass. An upstream PR title/body is drafted, but the PR is **not opened** and the work branch is not pushed under current authority.

A smaller fixture can prove correctness and mechanism, not the 150 s gate. If P0 shows the goal infeasible, record a measured no-go rather than weaken Done or implement a speculative pipeline. Asking to change Done is an owner decision.

**Non-goals:** rank 2 mask hashes; mask-resume fixes; PR #357's multi-GPU mask section; geometry; decoder parallelism; extraction/selection/encoding changes; content-hash caching across runs; identity/schema migrations; new crypto dependencies; new settings or UI text; NVIDIA/CUDA; production-data runs; share provisioning, cache flushing or machine tuning. Patented decoding is OFF except for one P4 CI-parity build, not an ON performance campaign.

**File ownership for future execution:**
- One implementation owner: `src/app/gui/DatasetPrep.cpp`, only source identities/snapshot/recheck and any proven read-only overlap outside the mask section; `DatasetPrep.h` only if the chosen helper needs it. Never edit the multi-GPU mask block (pinned lines approximately 3700–3950), `StencilRaster`, `InferenceWorker.*`, or `geometry_main.cpp`.
- The same owner handles necessary call-site adjustments in `SfmRunner.cpp` and `ColmapRunner.cpp`, and identity/resume tests in `src/app/gui/tests/dataset_prep_test.cpp` and `dataset_plan_test.cpp`. Existing public contracts are preferred; no API change merely to run a benchmark.
- `DatasetPlan.cpp`, `FrameExtract*`, `FrameDecode*`, `src/video/`, `core/Sha256.h` and `core/BufferedFile.h` are evidence/reuse dependencies, not initial edit targets. Do not change their shared semantics to make this optimization fit.
- Main alone owns this plan, its execution/review records and the ranking. No parallel writer may edit `DatasetPrep.cpp`; PR #357 stays independent. That PR also changes `StencilRaster` and adds `core/BufferedFile.h` near the includes (`172caa57af:20-22,447-480`), so expect a shared include hunk if it lands. Keep one include and its mask changes; do not blanket-select either side. Recheck moved anchors, never import that branch into an A/B pair.

## Branches

| Branch | Base | Holds | Goes upstream? |
|---|---|---|---|
| `plan/storage-latency` (origin) | Existing fork planning branch | This plan, ranking and sanitized execution record | Never |
| `perf/source-hash-overlap` (create only at execution start) | Current `upstream/master` | Minimal product changes and deterministic tests | Later, only after separate publication authority |

The fork's `AGENTS.md` from `git show origin/main:AGENTS.md` governs. Leave all policy files untouched. Do not merge or cherry-pick the planning branch onto the work branch. Keep plan files, private evidence, benchmark harnesses and fork instructions off the upstream patch.

The planning worktree was pulled first and is current with origin. It trails local `main` by 16 commits; no rebase is needed to write a plan. On another box, fetch origin/upstream and check out `origin/plan/storage-latency` as the planning branch; do not recreate it from upstream.

After execution approval:

```text
git fetch origin
git fetch upstream
git worktree add -b perf/source-hash-overlap ../spirula-source-hash upstream/master
```

Record the full base SHA and relevant blob IDs. Re-verify every anchor below when the base moves. Do not replay a branch whose name already exists without inspecting it. Create a separate baseline worktree at that exact base for A; no shared build tree, settings directory or simultaneous benchmark processes.

## Evidence

### Observations, not new measurements

From the ranking and the owner-supplied production evidence, release 2026.11.12:
- **MEASURED:** two approximately 13.7 GB videos, 27.46 GB total; 204 s from run start to the first motion-scan line, and 154.3 s from extraction end to `Found N images`. These are unlogged windows, not isolated hash timers.
- **DERIVED:** approximately 135 and 178 MB/s if each window were entirely hashing. **DERIVED**, not a fresh benchmark: scalar SHA-256 approximately 325 MB/s on that CPU.
- **MEASURED:** the following motion scan reads those sources for approximately 720 s; share sequential reads approximately 159 MiB/s; first open plus a small read approximately 7.5 ms/file. Release used the built-in decoder; current acceptance uses patented OFF, so the old windows are not its A baseline.
- **DERIVED model, not a saving forecast:** 27.46 GB / 159 MiB/s = 164.7 s of I/O and / 325 MB/s = 84.5 s of CPU per pass. A serialized sum would be 249.2 s/pass, but neither log window measured that sum. Its theoretical 169 s two-pass overlap delta is therefore not evidence that D3 can meet Done. If the historical 358.3 s windows were all hashing at a fixed uncached rate, saving 150 s would require at least 251 MiB/s aggregate for 54.92 GB / 208.3 s; single-stream overlap would leave only roughly 40 s recoverable. These assumptions conflict with the 154.3 s second window and are not a current-build go/no-go rule. P0 measures the actual baseline and rate before pricing a candidate.
- **DERIVED photo scale:** 1,000 first opens cost about 7.5 s before hashing/enumeration; 10,000 cost about 75 s. In-place/dataset photos are an important exception below.

### Code contract at the pinned upstream revision

All abbreviated GUI filenames below are under `src/app/gui/`.

| Evidence | Anchor | Consequence |
|---|---|---|
| Source inventory, per-file hash and final identity | `DatasetPrep.cpp:1228-1290` | Normalize input root; sort photo paths; exclude workspace subtree; include external non-built-in video LUT. Stat size/mtime before reading, hash every byte via 64 KiB loop, check EOF, then re-stat both |
| Identity framing | `DatasetPrep.cpp:1257-1287` | Outer SHA receives original relative generic name, decimal size, inner SHA hex, each followed by NUL. Inventory-only mode uses mtime instead of inner hash. Preserve field and file order exactly |
| In-place photos | `DatasetPrep.cpp:1232-1237` | Return literal `in-dataset`; do not introduce hashing on this fast path |
| Snapshot publication | `DatasetPrep.cpp:1292-1308`; `DatasetPrep.h:261-266,386-388` | Hash every input; publish the complete ordered `source_content` vector only after all succeed. Empty content is preview state, not a usable asynchronous snapshot |
| Runner entry and added inputs | `SfmRunner.cpp:838-860,873-886`; `ColmapRunner.cpp:511-528` | Snapshot precedes authoritative planning, keep refusal and Frames begin. Added scan-photo inputs get their own identities before preparation |
| Resume persistence/comparison | `DatasetPlan.cpp:662-684,1008-1012,1052-1063`; `DatasetPrep.cpp:1466-1487` | Content enters plan/resume records. Empty content deliberately suppresses a comparison in preview. Direct preparation snapshots only if absent; reused video frames require a completed matching record |
| Motion/extraction ordering | `DatasetPrep.cpp:1491-1500,1574-1630,2093-2174` | Motion scans and extraction occur after the initial snapshot/planning barriers; some reuse/nonadaptive paths never scan |
| Final verification | `DatasetPrep.cpp:1812-1840` | Recheck every eligible input before `done(Stage::Frames, job)`, count/log images, refresh masks and downstream work |
| Move photos | `DatasetPrep.cpp:3373-3385,3453-3470` | Read surviving transferred locations but hash original names; reject unexpected inventory. This verification occurs before later sorting. General final recheck skips Move inputs because originals may be gone |
| Incremental SHA / file helper | `src/core/Sha256.h:71-120` | Reuse `Sha256`, not a new algorithm. `sha256_file` has 16 MiB thread-local chunks but lacks this path's post-open read-error and mutation contract; it is not a drop-in replacement |
| Buffered input | `src/core/BufferedFile.h:11,19-74` | Reuse owning `BufferedInputFile` and `kFileBufferBytes` (16 MiB); preserve its lifetime and platform-specific buffer setup |
| ffmpeg reads | `src/app/FrameDecodeFfmpeg.cpp:39-61,151-185,225-249,273-305`; `src/app/FfmpegVideo.cpp:18-26,94-132,158-190` | Child opens the source; parent receives decoded PGM/PPM, not original bytes. Seeking, clipping, selected tracks and early close prevent full-file coverage |
| Built-in reads | `src/app/FrameDecodeVulkan.cpp:29-155,251-305,378-435`; `src/video/Mp4Demuxer.cpp:99-159,376-438`; `MkvDemuxer.cpp:127-181,305-455` | Header skips and packet seeks omit unrelated bytes/tracks/padding; decoding transforms bytes. Neither demux packets nor pixels are a whole-file hash stream |
| Conditional scans | `src/app/FrameExtract.cpp:108-180,536-603`; `src/app/FrameMotion.cpp:565-607` | Motion work can compete for CPU and storage, and is not present for all input modes |

Identity equality means exact existing serialization, not merely an equal leaf SHA. Directory enumeration and transfer mapping retain their current rules. Per-file concurrent workers may return ordered leaf results; they must never feed the outer SHA in completion order, replace it with a hash tree, or publish partial `source_content`.

## Hypotheses and the P0 measurement that decides each

P0 decides engineering choices without asking the owner to select a thread count. Runtime and fixture authorization remain prerequisites. Every row receives measured/rejected/inapplicable status and evidence; no silent omission.

| ID | Hypothesis | Measurement and decision |
|---|---|---|
| H1 | Small reads plus serialized read/hash leave useful overlap | Compare the actual 64 KiB baseline, owning 16 MiB stream buffering with the same 64 KiB application chunk, then 16 MiB application chunks; only then try two-slot read/hash overlap. Measure read wait, hash CPU, wall time, bytes and buffer high-water locally/SMB. Prefer the simplest control; add pipelining only if at least 10% faster than the best synchronous candidate and beneficial to the full window |
| H2 | Two or more file streams beat a single stream on this share | Same total bytes/files and unchanged identity algorithm at 1, 2, then 4 workers (4 only if 2 helps). Measure read-only aggregate rate separately from hash throughput, per-process CPU and network received bytes. Choose the lowest count within 5% of the best median; no CPU-count-sized fan-out |
| H3 | Initial hash can hide behind motion/startup | Record the source-independent startup span and dependency barriers in P0. Motion overlap is rejected because planning needs the snapshot. D4 is also rejected: the pre-snapshot work contains early refusal paths, and joining an uncancellable hash would delay an otherwise immediate failure. The timer documents the ceiling; it does not authorize a new cancellation/provenance contract |
| H4 | Recheck can overlap a useful part of the next stage | Time read-only final image discovery/counting. Record only a timing line initially; do not prototype unless the independent span is at least 5% of recheck wall time. Any candidate joins before Frames done and mask-state refresh/publication. Actual masking is rejected under present ownership/barriers |
| H5 | Existing decoder reads can eliminate a pass | Rejected by pinned source inspection: seeking/track selection omit bytes, and ffmpeg gives the parent decoded pixels rather than original bytes. P0 records this verdict, not new source-offset tracing or an ON runtime. A contention diagnostic is optional only if another unresolved choice needs it |
| H6 | Small-photo latency benefits more from bounded concurrency than large chunks | Same photo inventory at 1/2/4 workers, local and SMB; split enumeration, stat/open, read/hash and assembly time. Include one large folder and several input folders, ordinary Copy plus separate synthetic Move correctness. Select photo concurrency only above the 20% gate; retain `in-dataset` fast path and one outer digest per input |

## Design and priced alternatives

Prices are rough engineering estimates, not spending authority: active implementation/check effort and approximate agent tokens; P0/P3 machine time is separate. Start with the cheapest candidate and stop adding machinery once Done holds. Performance estimates are **INFERENCE** until measured.

| Option | Cost | Decision / ceiling |
|---|---|---|
| D1: reusable buffered synchronous read, same SHA and barriers | 1–2 h; 8–15k tokens | First candidate. Removes small-read overhead; no promise that SMB wire rate improves. Preserve all read-error/mtime/size checks |
| D2: bounded independent input/file hashing | 3–6 h; 20–40k tokens | P0-selectable. Start with two video inputs, flatten photo file work when one folder dominates. A single concurrency budget across inputs/files; never nested pools. Bounded results assembled in inventory order |
| D3: one reader and one hasher with two reusable chunks within one file | 4–8 h; 25–50k tokens | P0-selectable for a single large source or when D2 is inferior. FIFO blocks, backpressure and joined lifetime; saving is bounded by measured non-overlapped read/hash time, not the historical 169 s model. Do not combine with D2 unless separately measured and within memory |
| D4: initial hash beside source-independent startup | 2–4 h; 15–25k tokens | Rejected: existing pre-snapshot work has refusal paths; a joined hash can turn an immediate refusal into a full-pass wait. No new cancel API merely to recover this likely small span. H3 records its duration only |
| D5: recheck beside immutable image count/discovery | 2–4 h; 15–25k tokens | Only H4. Join before Frames done/log and mask refresh; no mask-section edit. Benefit capped by measured independent work |
| First hash concurrent with actual motion scan | 1–2 days; 40–80k tokens plus provenance redesign | Rejected: planning needs completed identities first, and reuse/nonadaptive paths do not scan. A deferred snapshot could observe changed bytes after extraction starts |
| Recheck concurrent with mask execution | 1–2 days; 40–80k tokens plus failure/publication redesign | Rejected: gating Frames done alone does not retract mask files or downstream records. Requires provisional outputs, cancellation and all-stage publication gates in a section owned by #357; outside this plan |
| Recheck each input as soon as its extraction finishes, while another extracts | 0.5–1 day; 25–50k tokens | Rejected under unchanged semantics: the wave (`DatasetPrep.cpp:1632-1660`) could hide a hash, but starts that input's second observation earlier than today's post-all-extraction barrier (`1812-1821`). A mutation during the remaining extraction can escape this run. Keeping the final check restores the extra read; weakening it needs owner approval, not automatic adoption |
| Reuse built-in decoder reads | 2–4 days; 60–120k tokens | Rejected: seeking/partial coverage/order need a whole-file coverage/cache architecture; does not solve the default OFF path or the later fresh observation |
| Tee source bytes into ffmpeg / local source staging | 2–4 days; 60–120k tokens; at least another source-size disk copy if staged | Rejected: pipes change seek behavior; decoded stdout is not source content. Staging changes source provenance and still needs fresh original-source verification; disk/share write authority and space are not implied |
| CPU-accelerated SHA / platform crypto | 1–3 days; 40–100k tokens for portability and validation | Deferred: potentially useful if P0 proves CPU-bound, but buffer/concurrency options reuse existing SHA and avoid a shared crypto change. Revisit only via an explicit scope decision if needed |
| Cached digest, size/mtime shortcut, sampled bytes or one pass only | Cheap but invalid | Rejected unconditionally: misses same-size/restored-time replacements or mid-run changes; changes resume/integrity semantics |

**Implementation invariants for D1–D3:**
- Reuse `spirula::Sha256`. Keep inventory-only calls cheap, exactly as today; retain `in-dataset`, external LUT, added scan-photo and transfer-aware behavior.
- Freeze the job/input inventory for each pass; workers read immutable descriptions and own stream/SHA state. Preserve pre/post size and mtime checks on the file actually read. Require the total bytes hashed to equal the pre-read size, in addition to EOF/error checks; a short read must never yield a valid prefix digest. Successful identities stay unchanged. Do not weaken symlink or naming behavior in this performance patch.
- Use reusable 8–32 MiB buffers, 16 MiB default, through existing buffered-file facilities or correctly sized explicit chunks. Count stream buffers **and** application chunks: two buffered synchronous lanes can cost 64 MiB; adding double buffering can cost 96 MiB. Four photo lanes with both stream and chunk buffers can consume the full 128 MiB cap. Reuse buffers across files; do not materialize whole videos or queue all file contents.
- Bound unfinished file-result storage as well as bytes: dispatch/consume a fixed ordered window, while retaining the existing sorted inventory. Never wait holding a lock needed by the worker producing the next ordered result.
- All workers join before their owner returns; no detached thread, escaping job reference, or unobserved future exception. Existing hash/snapshot APIs have no cancellation channel (`DatasetPrep.h:386-388`); retain that behavior, not a new chunk-level cancel API. An internal read failure can stop further claims, but user cancellation is observed at the existing caller boundaries after joined hashing. Do not turn cancel into an empty identity/read error. A complete hash pass or blocked SMB read may delay cancellation; record that limit. No timeout/retry framework.
- Assemble one complete identity per input, then publish the full vector on the owning thread. Deterministic input-order errors preserve current behavior; no partial plan/Frames record. Post-extraction comparison completes for all eligible inputs before success.
- Move verification stays at its transfer-local point, using original names and surviving locations, before sorting can move the only copy again. Do not put asynchronous reads across rename/delete ownership transfer.

## Phases

### P0: measure and select (2–4 h active; elapsed time priced from pilot)

**Admission, after separate execution approval.** Obtain the private fixture and existing writable SMB test-folder location out of band. No request for production captures or share provisioning is implicit. Verify idle host, supported non-NVIDIA Vulkan device, cached/accepted resources only if the unchanged preparation path needs them, and sufficient local/share space for source copies plus A/B outputs. Derive required bytes from the supplied fixture and recipe before copying; if it does not fit, ask for a scope/capacity choice rather than silently shrink acceptance. Originals stay read-only. No credentials, addresses, private capture names or absolute machine paths enter the repository.

**Fixture and comparability.**
- Video cohort: the supplied two-video fixture, targeting the production byte scale (approximately 27.46 GB), plus a small excerpt only for setup smoke. Same bytes, relative names, track/LUT/clip/adaptive settings, FFmpeg binary and output settings in every A/B pair. Source hashes are full-file even for a clipped extraction.
- Photo cohort: supplied or deterministic redistributable photos, at least 1,000 files, recording size distribution, count and total bytes. Include one-folder and multi-input arrangements without changing their total inventory. Small synthetic fixtures cover Move/Keep, errors and boundaries; do not Move private source fixtures.
- One verified source copy on local disk and one inside the share's **existing test folder**. Put both sources **and the dataset workspace/outputs** on that share for SMB arms; both are local for local arms. Preserve size and mtime when copying, record manifests privately and verify bytes before timing. Local-vs-SMB identity equality is not assumed if root-dependent names differ; compare A/B at the same canonical source/workspace roots.
- Before each arm, restore the same empty or resumed workspace state under the same paths, outside the timed interval. Reuse/Keep gets a separate matched scenario, not a warm accidental B fast path. Record settings and input inventory; do not mix fresh and resume arms.
- Fresh processes do **not** make SMB or OS caches cold. Record first-touch and repeated-read samples separately, without flushing client/server caches or changing services. Measure wire/process bytes where available; cache-influenced rates are labeled as such. The rank-1 repeated-read probes reached 6–8 GiB/s and did not prove wire throughput; do not repeat that interpretation.
- **Acceptance cache protocol:** after workspace reset, perform one untimed sequential read of the full source inventory in fixed order, then start the arm without other fixture access. Prime A and B identically, local and SMB. This defines repeated reads, not guaranteed residency. Pair order is **A,B / B,A / B,A / A,B**; report first/second-slot results separately. Use existing SMB-client/network counters on the idle host to record actual network read bytes for each hash pass, not logical `ReadFile` counts. A share pair qualifies only if each pass's A/B network bytes differ by at most **5% of the larger count** (both zero is matched cache-resident evidence, not wire performance). Missing or contaminated counters leave Done unproven; do not silently waive the gate or add tracing infrastructure. First-touch observations are separate. No eviction corpus, cache flush or shared-service change is required.

**Build/instrument only in P0 execution.** Use the dev wrapper, GUI enabled, patented OFF:

```text
build_develop.bat -DSS_BACKEND=vulkan -DSS_ENABLE_PATENTED=OFF -DSS_BUILD_GUI=ON -DSS_BUILD_SAM=ON
```

Use the bash wrapper on Linux. Store a throwaway `p0-timing.patch` and scratch runner beside private logs, not on the work branch. Instrument the real identity path, not just a library SHA microbenchmark: enumeration/stat/open/read/hash/re-stat/assembly, input and pass IDs, bytes, queue high-water, initial join, recheck join and record/cancel outcomes. Timing output is diagnostic, not new UI text. Build the baseline once; test-only variants can select the candidate in the throwaway harness, never via a shipped setting.

**Method.**
1. Before candidate builds, compare read-only 1/2-stream throughput on the two videos and 1/2/4 on photos; keep total bytes fixed. Distinguish cached effective file rate from wire throughput. Wire probes use an already available tool's client-cache bypass (for example Windows `FILE_FLAG_NO_BUFFERING`, respecting alignment), or previously unread disjoint slices within an approved corpus; otherwise label them cache-influenced and do not infer wire capacity. This does not establish server coldness. Do not add permanent tooling or duplicate source-sized eviction files. The historical 251 MiB/s conditional bound is context, not a universal threshold.
2. Build the pinned OFF baseline and smoke tiny deterministic identity inputs plus one small real-GUI excerpt. Instrument the real identity path to measure current two-pass time, per-pass read wait/hash CPU, and H3/H4 safe spans. One full-size OFF A pilot prices the real complete window and catches path differences before candidate development. After two setup failures, use a lighter check only if acceptance still holds; otherwise escalate, not build new infrastructure.
3. Calculate an optimistic estimate under the **same cache protocol** before candidate builds: for each pass, `floor = max(full_bytes / best_effective_file_read_rate, measured_SHA_CPU_seconds / useful_hash_lanes)`; sum `A_pass_seconds - floor`, plus at most H4's independent span. One sequential SHA has one hash lane; two independent video digests can use two. Use effective full-file rate for matching cache conditions, not uncached wire rate against cached bytes. If even this estimate is below 150 s, record no-go without the candidate matrix. It is a measured planning estimate, not proof no algorithm could improve things; inconsistent rates invalidate the estimate. Do not add overlapping savings.
4. Screen D1 controls and D2/D3 with cheap **scratch identity-runner**, hash-window pairs; benchmark only promising candidates/combinations with four pairs per location. Measure resident-byte SHA CPU time separately. The fixed disjoint drift probe follows the same bypass/fresh-slice caveat as step 1 and never prewarms the fixture. More than 15% probe drift or failure of the 5% per-pass SMB-byte gate invalidates a pair; retain its data. Allow at most one replacement pair per location, within the proposed allowance below, then diagnose or request a budget/scope change. Do not build every alternative first.
5. Full GUI comparisons belong to P3, not a repeated full matrix for every P0 candidate. Use isolated scratch `APPDATA`, unchanged `LOCALAPPDATA`, existing `SS_GUI_AUTOMATION=1` / `tools/guictl.py` or manual interaction, and an owned port for the P0 pilot. Record start/scan/extraction/recheck/Frames/count boundaries identically; cancel later work only after the measured outcome. No unsafe ordering change or child source-offset tracing is needed.
6. Report medians, ranges, paired/slot deltas, read bytes/rates, CPU time, peak commit, worker count and buffer bound. Include single-input/two-input and photo shapes; do not average away regressions. Price remaining elapsed time from the pilot: four pairs × two locations = 16 arms per retained candidate. Hash-only P0 runs avoid paying the full extraction window each time. Record the estimate before committing to P1/P3.

**Proposed execution allowance, not authority granted now:** 24 h cumulative measurement runtime: at most 6 h in P0 and 18 h in P3, including priming/probes and replacement pairs; builds and active engineering are priced separately. Reprice using the full-size OFF pilot before launching the remaining matrix. If it cannot fit, finish the bounded evidence and ask for a revised allowance, not silent fixture reduction or unbounded retries.

**Decision:** choose the smallest D1–D3 candidate or separately measured combination that can meet Done; D5 is optional only above H4's gate, and D4 is rejected. If no option can plausibly save 150 s, close P0 as no-go with the measured estimate. Missing fixture/authority is an exact prerequisite, not permission for a smaller acceptance fixture. Shipping D1 alone or a smaller improvement after no-go changes Done and needs the owner. Record choices and sanitized evidence here.

### P1: implement the selected hash path (roughly 0.5–1 day)

Implement only selected D1–D3 at the existing identity boundary. Run references before any exported signature change; update every caller together. Preserve ordering/identity format and error handling. Extend existing dataset tests for uncovered behavior below, using generated synthetic files rather than committed binary fixtures. Reuse `buffered_file_test`; do not add a parallel SHA library or durable benchmark framework.

### P2: integrate only proven scheduling changes (0–0.5 day)

If selected, add D5 without moving planning, source ownership or publication barriers. Otherwise record it rejected and keep runners unchanged. D4 stays rejected. Exercise SfM and COLMAP entry, direct-run fallback and added scan-photo inputs. Do not edit the mask section to manufacture overlap. Join before callbacks that mutate job/settings and all returns. One owner integrates P1/P2.

### P3: prove correctness and end-to-end benefit (pilot-priced; roughly 10 h video runtime before photos/builds)

- A = exact work-branch base; B = candidate, each built with the same compiler/configuration in separate worktrees. Both patented OFF. No rank-1/geometry/other patch may land between arms. Strip P0 detailed instrumentation; retain identical minimal boundary timestamps outside the permanent patch where no existing log exists.
- Repeat the matched cache protocol and balanced order on the real GUI full-size video fixture and photo cohort, four pairs per location. At the historical 2,095.8 s frames window, 16 video arms alone cost 9.3 h; OFF ffmpeg performance is unmeasured and can raise or lower that, with priming, photos and checks additional. Replace this estimate with the P0 A pilot, not an assumed 4–8 h total. Include all hash work, joins, counting and overlapped work through the **same** endpoint. Apply numeric Done and local/photo non-regression; report slot medians.
- Compare each pair's frame paths/bytes and serialized identities/plan decisions. Compare content fields rather than unrelated timestamps in entire logs/records. An instrumented pilot is not final acceptance; a repeated-read cache result is not a cold-share claim.
- Exercise unchanged resume/Keep and same-size/restored-time mutation, plus during-extraction mutation on synthetic sources. Observe GUI completion/cancellation, not merely process exit. Record missing prerequisites or SKIP explicitly; zero exit with SKIP is not PASS.
- One `reviewer` reviews the product diff, focused on read errors, deterministic aggregation, thread lifetime, mutation barriers and Move safety. Repair findings; review only the fix delta once. No plan critics substitute for the later code review.

### P4: prepare the upstream PR, not opened

1. Fetch upstream; if unchanged, do not gratuitously rebase. If changed, re-verify affected anchors and replay only product/test changes; rerun affected checks and A/B if the measured path changed. Preserve fork policy separately; never import planning commits. History rewrite/publication outside standing authority needs the owner.
2. Run **one patented-ON all-target CI-parity build**, not just `spirula`, and `spirula train --help`, then restore OFF and run the relevant OFF checks. This catches link propagation that an OFF-only build misses. ON is build/smoke parity, not a built-in-decoder performance run. Keep committed defaults OFF. Record Linux/macOS/Windows checks not actually run as limitations, not inferred passes.
3. Read current upstream testing/fixture guidance and PR template. Run the Source lints block below as well as the dev build; the Windows wrapper is not equivalent to CI lint. Exercise synthetic generation/tests from an empty temporary fixture directory. Record commands and versions; no runtime Python build dependency or private binary fixture in the patch.
4. Commit verified product changes locally on `perf/source-hash-overlap`; update this plan/ranking only on `plan/storage-latency`. Run the private-path checker on each branch and directly inspect changed Markdown too (the checker excludes `docs/`). Delete throwaway build patches/runners from the worktree, retaining necessary reproducibility evidence privately.
5. Draft title and body in **this plan's execution record**: problem, minimal fix, measured A/B table, reproducible tests, exact limitations and no private identifiers. State whether the 150 s gate passed. No `gh pr create`, no work-branch push and nothing sent to upstream without new explicit authority. A P0 no-go closes with evidence, not a fictitious PR.

## Risks

- **Attribution/cache:** 358.3 s of log windows is not 358.3 s of scalar SHA. P0 must isolate current OFF behavior and report first-touch/repeated/cache evidence; snapshots may warm subsequent decode, so measure the complete window.
- **Bandwidth/CPU contention:** two streams can share the same saturated SMB link, and motion can consume the same CPU. A faster hash alone can make extraction slower. Do not sum overlapping savings or scale a photo result to video.
- **Snapshot consistency:** size/mtime before/after is the current file-stability check, not a filesystem snapshot or an adversarial mutation proof. Keep content comparison and both observation boundaries; do not advertise stronger guarantees.
- **False completion:** no empty/partial identities, worker-error digests, Frames completion or downstream publication after hash failure. Mask overlap is specifically excluded; existing failed preparation may leave incomplete frame files just as before.
- **Move/data loss:** source locations can disappear legitimately. Preserve transfer mapping and the pre-sort check; test only expendable synthetic Move inputs. Do not promise rollback of originals the baseline already moved.
- **Memory and latency:** cap queues/threads across nested inputs; reuse large buffers. Hashing retains its existing lack of a cancel channel; cancellation may wait for the entire pass or a blocked SMB read. No detached cleanup or unbounded speculative opens.
- **Ownership:** #357 and geometry are independent; keep the multi-GPU mask block untouched. If upstream moves it, identify by function/behavior rather than stale line range.

## Checks

Existing coverage at the pinned base:
- `src/core/tests/buffered_file_test.cpp:35-115`: bytes across multiple buffer boundaries, seek/EOF, stream moves/swaps/reopen and deferred write failure.
- `src/app/gui/tests/dataset_prep_test.cpp:770-839`: unchanged reuse, rejected content replacement with restored size/mtime, failed completion, legacy record, inventory and LUT identity. Its mutation happens before `run()`, so during-extraction coverage is still needed.
- `src/app/gui/tests/dataset_plan_test.cpp:335-360`: content changes invalidate dependent plan stages.
- `dataset_prep_test.cpp:397-452`: Move ownership and skipped originals; its resume assertions skip Move, so add focused transfer identity/completion coverage if the hash path changes.
- `src/app/gui/tests/dataset_prep_streaming_test.cpp:93-177`: serial/parallel frame parity and failed publication; creates FFV1 with ffmpeg and can return SKIP without Vulkan prerequisites.

Extend only gaps for the selected design: golden old-format identities for zero-byte, SHA block and chunk-boundary files; sorted multi-file/LUT order despite out-of-order completion; snapshot all-or-nothing publication; truncated/error reads never accepted as complete; tested during-extraction mutations prevent Frames completion; Copy/Move/in-dataset behavior. On Windows, exercise a held second-handle `LockFileEx` byte-range lock for read failure and assert no prefix identity is published; whether MSVC presents that as EOF is an untested hypothesis, not a confirmed bug. Successful reads must hash the pre-stat byte count. Test cancel during **both** hash passes against A's records/resume decision, after all work joins; the pause-path cancel check at `DatasetPrep.cpp:1735-1739` does not forbid a later valid Frames record. Use deterministic synchronization, not sleeps. Generate golden expectations from the pinned original's fixed synthetic names/bytes, not the new implementation; no duplicate production hashing implementation.

Future execution commands (not run during planning):

```text
build_develop.bat -DSS_BACKEND=vulkan -DSS_ENABLE_PATENTED=OFF -DSS_BUILD_GUI=ON -DSS_BUILD_SAM=ON
build_vulkan\buffered_file_test.exe
build_vulkan\dataset_plan_test.exe
build_vulkan\dataset_prep_test.exe
build_vulkan\dataset_prep_streaming_test.exe
ctest --test-dir build_vulkan -R "^subprocess_lifecycle_test$" --output-on-failure --no-tests=error
bash tools/check_private_paths.sh
```

The dataset executables and `buffered_file_test` are not CTest registrations at this base (`cmake/SsApps.cmake:416-424,565-567,715-784`); run them directly. The subprocess test (`SsApps.cmake:478-489`) covers ffmpeg lifecycle, not hashing cancellation; retain it only if scheduling affects subprocess lifetime. The Windows wrapper runs comment-length/Windows-macro checks (`build_develop.bat:41-42`); CMake additionally checks UI text (`cmake/SsChecks.cmake:1-54`), not all CI lint. P4 runs the following pinned Source lints block (`.github/workflows/ci.yml:25-35`), adjusted only if upstream changes, plus its single ON build/CLI-help smoke and restored OFF checks:

```text
bash tools/check_ss_prefix.sh
bash tools/check_i18n.sh
bash tools/check_file_macro.sh
bash tools/check_comments.sh
bash tools/check_private_paths.sh
python3 tools/check_font_coverage.py
python3 tools/check_winmacro.py
python3 tools/check_ui_text.py --all --quiet
python3 -m unittest discover -s tools/tests -p 'test_*.py'
```

Use the executing host's Python 3 spelling. No new UI text or generated-source edits are planned; follow current upstream generated-file guidance if that changes, without copying CI's history-reset commands into the worktree.

## Review record

Revision 1 verdicts: requested **Opus Max (`OpusMaxing`): FAIL**; **Fable (`expert-reviewer-fable`): FAIL**; paired **Astra (`expert-reviewer-astra`): PASS** with corrections. Main checked findings against the pinned code and revised the plan. Verdicts apply to revision 1; revision 2 was not independently re-reviewed. No builds or timings formed part of review.

| Finding | Adjudication and revision-2 change | Evidence |
|---|---|---|
| A1: acceptance cache population undefined | Accepted. One explicit repeated-read priming protocol, separate first-touch table, matched-byte caveat and balanced pairs; no cold-share claim | Complete reads at `DatasetPrep.cpp:1273-1285,1812-1821`; rank-1 execution's cache-influenced probes |
| A2: absolute mutation guarantee exceeds baseline | Accepted. Preserve tested detection guarantees and `in-dataset` exception rather than claiming snapshot isolation | `DatasetPrep.cpp:1232-1237,1270-1285` |
| A3: H5 tracing cannot change the reuse rejection | Accepted. Delete mandatory child read-coverage counters; source inspection is decisive | ffmpeg owns `-i` at `FrameDecodeFfmpeg.cpp:44-61`; parent receives pixels at `FfmpegVideo.cpp:94-132,158-190` |
| Astra's missing buffering-only control | Accepted. H1 tests 16 MiB owning stream buffer with the existing 64 KiB application chunk before larger chunks/pipelining | `BufferedFile.h:45-62`; `DatasetPrep.cpp:1276-1281` |
| F1: 169 s model is not a demonstrated ceiling/saving; probe streams first | Partly accepted. D3 no longer forecasts 169 s; cheap stream probes come first, followed by current OFF hash attribution and a measured no-go estimate before candidate builds. Rejected a mandatory historical 251 MiB/s cutoff or universal 40 s ceiling: the cited 154.3 s window already contradicts the fixed-rate assumption, and release/cache/decoder conditions differ | `DatasetPrep.cpp:1273-1285,1812-1821`; historical rates and conditional arithmetic in Evidence |
| F2: odd pair count and uncontrolled cache carryover | Accepted balanced four-pair order and fixed priming per arm. Rejected mandatory 32 GiB eviction/first-touch certification: finite eviction does not prove server coldness, requires an unsupplied extra corpus, and is unnecessary for a clearly limited repeated-read comparison | Same full-read anchors; rank-1 record's explicit cache limitation |
| F3: chunk cancellation has no API and can become an identity error | Accepted. Preserve existing cancellation observation instead of adding a signature/error contract; join workers and report possible full-pass delay | `DatasetPrep.h:386-388`; runner snapshot before `DatasetPrep` construction; final hash after `DatasetPrep.cpp:1737` |
| F4: full-window matrix time underpriced | Accepted. P0 uses cheap hash-window screens plus one full A pilot; P3's 16 video arms alone are approximately 9.3 h at the historical window, with actual OFF pilot repricing | Ranking's 2,095.8 s frames window; OFF decoder choice at `FrameExtract.cpp:108-180` |
| F5: safe startup/count overlap likely too small | Partly accepted: timing lines only before a prototype; no unmeasured millisecond/one-second claim. H4 retains its gate; D4 is rejected after O6's refusal-path check | `SfmRunner.cpp:828-840`; `DatasetPrep.cpp:1491-1500,1823-1840` |
| F6 and proposed per-input wave recheck | Not adopted. Add the priced rejection, not extraction-wave ownership: moving verification inside the wave narrows the existing post-all-extraction observation boundary. Retaining the final check removes the proposed eliminated-read saving | `DatasetPrep.cpp:1632-1660,1812-1821` |
| O1: new cancellation changes Frames records and resume | Accepted, independently confirms F3. Test both pass cancellation outcomes against A; no new cancellation point or exported cancel parameter | `DatasetPrep.h:387-388`; `DatasetPrep.cpp:1735-1739,1812-1823`; `SfmRunner.cpp:903-909` |
| O2: cache/order can produce false saving | Accepted 5% actual SMB network-byte matching per pass, balanced slots and counter evidence as a gate. Uncached/fresh-slice probes are distinguished from cached effective rates; no server-cold claim | Full-pass reads at `DatasetPrep.cpp:1273-1285,1812-1821`; rank-1 probe evidence |
| O3: late/overstated ceiling, ambiguous statistic and run budget | Accepted current-build estimate before candidate matrix, median paired differences, four photo-only lanes, scratch P0 harness, pilot-priced runtime and proposed 24 h measurement allowance. Historical 40/109/169 s models use inconsistent cache/rate assumptions; none is a guaranteed ceiling | Evidence arithmetic; two-input wave `DatasetPrep.cpp:1632-1660`; two full hash passes |
| O4: short read may be reported as EOF | Accept byte-count-equals-pre-stat-size as a required new-reader invariant and deterministic Windows read-failure test. The specific MSVC `fread`/EOF mechanism remains an unexecuted hypothesis, not a verified defect | Existing EOF-only termination plus post-stat at `DatasetPrep.cpp:1275-1286` |
| O5: wrapper is not full CI lint | Accepted with correction: Windows wrapper is partial, but CMake does run UI-text checks. P4 now carries the exact separate Source lints block | `build_develop.bat:41-42`; `cmake/SsChecks.cmake:1-54`; `ci.yml:25-35` |
| O6: startup overlap delays immediate refusals | Accepted. D4 rejected; H3 is timing-only, preserving uncancellable hash semantics and error paths | `SfmRunner.cpp:821-840`; unchanged hash signatures |
| O7: #357 also overlaps includes/StencilRaster | Accepted. Record shared BufferedFile include hunk and protect StencilRaster rather than treating only the mask block as owned | PR commit `172caa57af`, `DatasetPrep.cpp:20-22,447-480` |
| O8: workspace placement and future push authority missing | Accepted. SMB arms include workspace/outputs on the share; future sessions require explicit push authority, including origin | Rank-1 P0 fixture method; governing fork `AGENTS.md` Git/upstream policy |
| Opus's per-input recheck / hardware SHA alternatives | Per-input proposal rejected for the same narrowed observation as F6. Hardware SHA may preserve bytes but changes a shared helper; retain the priced deferral, not an integrity objection. A smaller source-only implementation could be cheaper than the portability estimate | Extraction-wave and final-recheck anchors; shared `core/Sha256.h` |

**Disagreement resolved:** Astra favored matched repeated reads; Fable favored first-touch plus eviction. Adopt repeated-read priming, Fable's balanced order and Opus's 5% actual-SMB-byte gate, without claiming cold-cache results. All three questioned the savings/cancellation wording; current-build measurements replace historical ceilings, and cancellation remains unchanged. Main's adjudication closes all plan findings; no independent revision-2 PASS or future performance PASS is claimed. No escalation rung was used; the three reviews were explicitly requested/required, not failure escalation.

## Execution record: Not started

2026-11-16: planning only. Pulled the plan branch, fetched upstream, verified pinned code and reference-plan execution lessons. No product code changes, builds, GPU/app runs or timing measurements. Historical observations and arithmetic above are labeled; all new measurements belong to P0. Current authority permits plan-branch commits and a push to origin only.

## Execution handoff

> **Goal and active phase:** PERF-010, at least 150 s median paired saving on the full-size SMB fixture with identical identities/resume behavior. Active phase: P0, not started; obtain execution authority and private fixture details, then select D1–D3 and optional D5 from measurements. D4 is rejected.
>
> **Acceptance and evidence:** numeric Done, P0 hypothesis decisions and Checks above. Sanitized results, commands, base/candidate SHAs, verdicts and the unopened PR draft go in this file on `plan/storage-latency`; raw logs, manifests, private paths and throwaway harnesses remain outside the repository.
>
> **Authority and exclusions:** this request authorizes planning, commits and pushing `plan/storage-latency` to origin in this conversation only. It authorizes no product edits, builds, app/GPU/timing runs or share writes now. Future execution needs approval for fixture-only work and the proposed runtime allowance; future sessions also need explicit push authority, including to origin. No upstream activity, production-data writes, new settings/UI, NVIDIA, rank-2/geometry work or PR #357 mask-section edits.
>
> **Prerequisites:** supplied two-video full-size fixture and photo cohort, existing writable share test folder and copy/run permission, idle supported host, sufficient measured local/share free space, dev toolchain/ffmpeg, and isolated settings/build/output directories. All private locations come out of band; no extra GPU-count requirement for CPU hashing.
>
> **Stop/escalate:** ask the owner for scope/Done/authority changes or private facts unavailable to tools. After two failed attempts on one problem climb `task` → `expert` → `expert-max` → owner; do not reset counts. P0 disproving the benefit closes as no-go. A moved anchor is a re-verification task, not automatic scope expansion.
>
> **Readiness:** **not ready for execution** until runtime/fixture permissions and private fixture/test-folder details are supplied. Review findings are adjudicated; the plan is ready for that authorization. Buffering and worker count are P0 engineering choices. A sub-150 s delivery, weaker observation boundary, unavailable SMB-byte evidence or larger runtime allowance needs the owner's decision rather than an implicit waiver.
