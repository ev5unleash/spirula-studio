# Multi-GPU training: continuation handoff

Updated 2026-10-20. Implementation baseline: **`d1a829af`**.
Completed short-run results are committed at **`ef89ab3d`**; the first
cross-machine handoff is **`ebefdde7`**. Publication destination:
**[`ev5unleash/spirula-studio`, branch `dull-baboon`](https://github.com/ev5unleash/spirula-studio/tree/dull-baboon)**.

**Executed follow-on: [short-run improvement plan](multi-gpu-training-short-run-plan.md#execution-result--2026-10-17).**
Physical-role reversal averaged 158.56 ms/update versus 96.88 ms native in the
matched 2+6 screen. One temporary timeline diagnostic rejected a result-tail/
timer-only fix; instrumentation was removed and the native build restored.
The single-model multi-GPU speedup goal remains unmet; native stays default.

**Next action: inspect retained optimized-native profiling evidence, not another
two-GPU run.** The Reviewer-2 consultation below supersedes the proposed
projected-ID byte-count gate. This handoff authorizes no build, profile, training,
packing implementation or restart of a closed campaign.

## Read first

1. This handoff: next decision, transfer prerequisites and settled constraints.
2. [Short-run execution result](multi-gpu-training-short-run-plan.md#execution-result--2026-10-17):
   the latest matched timing and common-clock diagnostic.
3. [Image-strip investigation](multi-gpu-image-strip-investigation.md): implementation,
   numerical contract, checks and evidence limits.
4. [Comparison scorecard](multi-gpu-training-comparison-20260929.md): closed B2
   campaign and subsequent raster/strip outcomes.
5. [Original campaign plan](multi-gpu-training-plan.md): consult completed results
   only when needed. Its old next-step instructions and frozen ledger are not
   an unfinished execution queue.

Private locations and retained run artifacts are indexed by the existing
`PRIVATE-HANDOFF.txt` beside the preserved corpus. Do not copy those paths into
this repository. Logs, output directories and commits are the evidence; the
retired call ledger, admission process, deadlines and source snapshots stay retired.

## Reviewer-2 advice and the next decision

The 2026-10-18 consultation was read-only. No source changes, profile, training
or GPU checks followed. Its corrected recommendation is:

1. **Do not implement packing or run the proposed byte-count gate now.** No
   measured net saving supports it. The missing quantity is critical-path time
   after packing, transfers, remapping and merging, not just removable bytes.
2. **Inspect retained native staged-PPISP per-stage profiles first.** Establish
   source checkpoint/config, input 847 at 2880x2880, authority device, build,
   measured update interval and profiler mode before reusing a record. The old
   raster-only profile with unoptimized PPISP is not the fair control.
3. **If suitable evidence is missing, propose one native eight-update profile
   for approval.** Keep the full step-10000 source and 30000-step horizon,
   staged PPISP and active appearance/topology; no worker. Use the existing
   source-run mode and `SS_PROFILE=1`, not a new harness. Profiling changes
   synchronization, so profiled wall is not a fresh unprofiled speed baseline.
4. **Assess one stated split, not every possible multi-GPU algorithm.** Map
   row-splittable GPU work, serial dependencies, state-dependent traffic and
   synchronization. A balanced zero-cost split is only an optimistic model;
   account for the destination devices and actual timing boundaries. Require
   credible net headroom before a design/implementation decision. Otherwise
   park this direction as unsupported or inconclusive, not physically impossible.

Reviewer-2 withdrew its initial approximately 104.6 ms lower bound and "no
reachable pass outcome" claim. Worker output D2H precedes the post-D2H finite
validation tail ([executor](../../src/app/ImageStripExecutor.cpp)); deleting that
tail and the authority join does not remove this transfer or all possible
compaction savings. Scaling upload time by a byte fraction is an assumption,
not a hard bound. Native range/IQR measures variation, not unavoidable runtime
overhead; neither it nor the current collective timings rules out the whole
image-space family. The recorded 119.13 ms tail-only counterfactual remains valid.

GT/mask is already region-plus-halo limited and remains substantial traffic.
No concrete replacement ownership/transport design or speedup candidate was
selected. A later candidate still needs the unchanged numerical contract and
a fresh matched native control before any win claim.

## Continuing on another development machine

### Fetch the branch and verify its history

The user explicitly authorized publishing this branch to the fork's `origin`.
The destination is **`https://github.com/ev5unleash/spirula-studio.git`**,
branch **`dull-baboon`**. It is not a branch in the upstream
`harry7557558/spirula-studio` repository. No PR, upstream push, force-push or
experiment is authorized by this publication.

The previous missing-history report was valid: these commits were local-only
when the handoff was first supplied. Fetch the actual branch, not just copies
of its Markdown documents. A fresh, non-shallow clone is the simplest route:

```bash
git clone --branch dull-baboon https://github.com/ev5unleash/spirula-studio.git spirula-multi-gpu
```

For an existing checkout, preserve local changes first. If its `origin` points
elsewhere, leave it unchanged and add a distinct remote:

```bash
git remote add multi-gpu https://github.com/ev5unleash/spirula-studio.git
git fetch multi-gpu
git switch --create multi-gpu-continuation --track multi-gpu/dull-baboon
```

If `multi-gpu` already exists, verify its URL rather than replacing it. Use a
fresh local branch name if `multi-gpu-continuation` already exists. If already
on a clean local branch tracking this fork's `dull-baboon`, `git pull --ff-only`
updates it without rewriting local history. Do not reset or force-update a
destination branch to make the checkout succeed.

Inside the receiving checkout, verify the implementation and document history:

```bash
git status --short --branch
git rev-parse HEAD
git cat-file -t d1a829af
git cat-file -t ef89ab3d
git cat-file -t ebefdde7
git merge-base --is-ancestor d1a829af HEAD
git merge-base --is-ancestor ef89ab3d HEAD
git merge-base --is-ancestor ebefdde7 HEAD
git log --oneline -- docs/notes/multi-gpu-training-short-run-plan.md docs/notes/multi-gpu-training-brainstorm-handoff.md
```

Each `cat-file` must print `commit`; each ancestry check must exit 0. The
publication receipt supplies the pushed tip SHA to compare with `HEAD`.
For a shallow checkout, obtain full history from the correct remote before
interpreting missing ancestors. If a baseline or document history still cannot
be resolved, stop and report the exact remote, fetched tip and missing object;
do not treat the Markdown as proof that implementation exists locally.

| Commit | Role |
|---|---|
| `d1a829af` | Opt-in synchronous image-strip implementation; production executor restored to this baseline after the diagnostic. |
| `41e4e614` | Original brainstorming handoff and the checkout used to start the short-run evaluation. |
| `ef89ab3d` | Completed short-run plan/results and associated note updates. |
| `ebefdde7` | Cross-machine handoff with corrected Reviewer-2 advice and private-artifact prerequisites. |

Do not copy only this linked worktree's `.git` file: it points at another local
checkout. Do not copy a configured build tree as a portable build; rebuild for
the new host only after build authorization.

### Transfer private artifacts separately

Git contains source, generated source and public notes, **not the training
inputs or private experimental evidence**:

| Material | Required use |
|---|---|
| `PRIVATE-HANDOFF.txt` and its referenced identity/config/build records | Locate the exact source, dataset, run commands, hashes and retained logs; inspect for a matching native staged-PPISP profile before proposing a run. |
| Full native `native-src/step-000010000.ckpt` and its associated `config.json` | The preserved starting state: 1M capacity, original 30000-step horizon, packed 3DGS/FPBO, active PPISP and quantized RGB grid/AdaGrad. Copy the full checkpoint, not just a PLY or inference-only export. |
| Matching `ds-1104` dataset and all referenced images/masks/model files | Preserve the 1083 reconstructed views, 975 training / 108 held-out split and the identity of input 847 at 2880x2880. Do not reconstruct or replace it. |
| `short-run-reversed-{native,dual,timeline}-8.{log,json}`, `short-run-screen-summary.json` and each run's `image-strip-source-run.jsonl` | Timing, configuration and common-clock evidence; see the [complete artifact index](multi-gpu-training-short-run-plan.md#checks-preservation-and-evidence-limits). |
| Build identity logs, original/temporary executor copies, source-proof and decoded-update/lifecycle reports | Preserve numerical limitations, implementation identity and diagnostic provenance. The temporary protocol-6 executor is evidence, not code to restore. |

Keep the full step-10008 endpoints, failed attempts, earlier campaign evidence,
protected corpus and originals in the preserved archive. They need not all be
duplicated to start a read-only profile inventory, but must remain accessible;
step 10008 is **not** a replacement starting checkpoint. Ensure copied hardlinked
or externally referenced data has its backing files on the receiving host.

**Availability at this handoff:** the historical private inventory location was
recovered from the archived execution session, but the private share could not
be opened by filesystem tools or a native Windows file lookup. Its location is
supplied privately, not committed here. Current artifact presence/checksums and
a matching staged-PPISP profile remain **unverified, not absent**. No dataset,
checkpoint or private log transfer was performed for this handoff. Restore
access to that inventory, or obtain it and the listed files from the owner,
before claiming the receiving machine can reproduce the experiment.

### Resolve relocation without changing the experiment

- Compare source `state.tar`, config and dataset identity against the private
  records before execution; do not invent missing checksums or replace the
  checkpoint with a freshly trained source.
- Copy the checkpoint's `config.json`, or its run-level `config.json` when
  that is the original source. Resume looks in the checkpoint directory first,
  then the run directory ([resume implementation](../../src/checkpoint/Resume.cpp)).
- Check embedded dataset/image/mask paths. The source-run interface has **no
  `--data` override** and loads the saved config through `ordinary_source_setup`
  ([source-run implementation](../../src/backend/tests/engine/g1_native_proof.cpp)).
  Prefer a destination path mapping that preserves the original config. If
  relocation requires path edits, use a separate working copy, retain the
  original, record the exact path-only changes and old/new identities, and
  verify all training settings, dataset ordering and image contents unchanged.
  Do not silently claim the original config hash after editing it.
- Check both build/output storage afresh and retain at least 10 GiB free after
  planned growth. Keep one process per GPU; inspect for other jobs before any
  later authorized execution.
- Rediscover device UUIDs, driver/toolchain versions and topology on the new
  host. Old `0100...`/`0200...` selectors and timings are not portable device
  assignments or fresh native controls. A changed GPU, driver or host transfer
  route requires new matched controls before a performance claim.

### Build and command reference — execution requires approval

The implemented **two-device strip executor is Windows-only**. Moving it to
Linux/macOS would be a separate port, not ordinary setup. Use supported
non-NVIDIA Vulkan hardware; do not substitute a CUDA or NVIDIA Vulkan run.

For Windows, install the Visual Studio C++ tools and Windows SDK, CMake/Ninja
(the dev script can use the VS-bundled copies), and Vulkan SDK. Use the committed
Slang version pin; the earlier accepted build used MSVC 19.44.35229.0 Release and
Slang 2026.12.0.1. CMake can fetch pinned Slang, or an offline setup can supply
the matching compiler through `SS_SLANGC`. Generated C++ sources are committed;
Python, PyTorch and CUDA are not Vulkan build dependencies. See
[dev builds](../build.md#dev-builds) and [backend specifics](../build.md#backend-specifics);
the current repository support policy overrides their legacy CUDA/PyTorch text.

After build authorization, from a Windows command prompt:

```bat
build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_GUI=OFF -DSS_ENABLE_PATENTED=OFF
```

The tools are `build_vulkan\spirula.exe` and
`build_vulkan\g1_native_proof.exe`. Discover UUIDs from the destination CLI's
device listing; do not reuse ordinals or assume the historical UUIDs identify
the same physical cards. Clear inherited worker/B1/B2/debug overrides, including
deprecated `SSPLAT_` aliases, before a native control. The source-run mode refuses
an inherited worker selector; the native command below has no `--worker-device`.

Only if a suitable retained profile is unavailable **and the proposed diagnostic
is explicitly approved**, set `SS_HANDOFF_SOURCE` to the validated checkpoint,
`SS_HANDOFF_AUTHORITY` to the destination UUID's 32 hex digits, and
`SS_HANDOFF_OUTPUT` to a new, nonexistent output directory with an existing parent:

```bat
set "SS_PPISP_REDUCTION=staged"
set "SS_PROFILE=1"
build_vulkan\g1_native_proof.exe image-strip-source-run --resume "%SS_HANDOFF_SOURCE%" --device "uuid:%SS_HANDOFF_AUTHORITY%" --output "%SS_HANDOFF_OUTPUT%" --updates 8 --input-index 847
set "SS_PROFILE="
```

Capture complete stdout/stderr and the actual exit code privately as well as the
generated JSONL; per-stage profiler output is not promised in the JSONL. Preserve
all eight updates and separate the first two from the six timing-screen updates.
Inspect profiler scope: an aggregate including warm-up/setup cannot be relabelled
as updates 3-8. A successful run must end at full step 10008 with the unchanged
30000 horizon, expected source/config identities, input dimensions and native
arm. This is fixed-input diagnostic evidence, not ordinary-sampler timing,
numerical qualification or a speedup claim. Do not add a dual run, 20-update
extension, parameter sweep or full suite automatically.

### Prompt for the receiving session

> Fetch branch `dull-baboon` from `https://github.com/ev5unleash/spirula-studio.git`
> using the commands in `docs/notes/multi-gpu-training-brainstorm-handoff.md`.
> Verify `d1a829af`, `ef89ab3d` and `ebefdde7` are ancestors and both handoff/plan
> documents have history; preserve any unrelated local edits. Read the handoff
> and the short-run plan's execution result. The tested two-GPU executor is
> slower; native remains default. Do not restart B2/G1/O1/raster/strip campaigns,
> implement packing, profile, train or build yet. Verify the private
> `PRIVATE-HANDOFF.txt`, source/dataset identity and artifact access. Inspect
> retained native staged-PPISP profiles for a matching per-stage GPU breakdown.
> Report what is reusable and what is missing; if no suitable profile exists,
> propose one eight-update native diagnostic for approval. Assess causal
> compute/transfer headroom for a stated split without converting byte fractions
> or timing spread into hard impossibility bounds. Preserve the same logical
> objective, active appearance/topology, authoritative native update and existing
> numerical gates. This branch publication does not authorize new experiments,
> further pushes or a PR.

## Settled user decisions and constraints

- **Same training semantics, checked numerics**, not byte-identical optimizer
  state. Preserve the logical batch/objective, active appearance and topology,
  and one synchronous authoritative native update.
- The user explicitly chose implementation of the two-GPU strip path; that
  deliverable is complete. Do not substitute a recommendation for missing code,
  or describe its lack of speedup as an unfinished implementation.
- No independently normalized crop objectives, stale weights, frozen appearance,
  enlarged batches or duplicated shared optimizers under that contract.
- Windows Vulkan on the two supported AMD Radeon AI PRO R9700 devices. No
  NVIDIA/CUDA implementation, debugging or validation.
- Lightweight targeted checks were requested, not a full suite or long campaign.
  Do not launch either merely to prepare this discussion.
- Native stays default; experimental behavior is opt-in. Publishing this handoff
  and its branch history to the fork's `origin/dull-baboon` is explicitly
  authorized. No PR, upstream publication or further push is implied.
- Preserve originals, corpus, source checkpoint and failed evidence. Check
  resources afresh before future execution; retain the existing 10 GiB
  post-growth free-space floor and one process per GPU rule.

## What the experiments actually established

| Work | Observed result | Meaning for the next discussion |
|---|---|---|
| B2 frozen / refresh-1000 | Complete-wall medians 1323.576 / 1624.831 s versus native 2614.295 s; both failed the frozen six-metric/named-view quality rule | Tested schedules are closed, not qualifying winners. Speed alone did not solve the objective. |
| Raster-backward-only split | Ideal half-raster saving 3.498 s was below native wall spread 5.806 s | Closed at its specified Gate 0; not an implemented or measured two-GPU speedup. |
| PPISP staged local reduction | Warm operator medians about 2.46–2.59 ms versus native joint 82–116 ms on the captured calls | Strong standalone single-GPU operator opportunity; use the efficient native control before proposing offload. Not a measured whole-training speedup of that magnitude. |
| Same-view image strips | Matched 20-update training loops: native 2.00897 / 2.01819 s, dual 3.53100 s | Dual was about 75.4% slower on this fixed-input diagnostic. No speedup recommendation. |
| Earlier file-staged G1 / O1 | Earlier G1 workload about 7× slower; O1 rejected at resource screening before timing | Do not rerun those unchanged or generalize their outcomes to every possible transport/ownership design. |

These rows have different workloads and timing boundaries; do not rank their
numbers against one another. The strip full paths were 17.074 / 15.192 / 11.668 s
(native / dual / native repeat), dominated by variable setup/save/cache effects.
The middle value is **not** evidence of a complete-wall speedup.

## Reusable implementation and validation

- `SS_IMAGE_STRIP_WORKER_DEVICE=uuid:B` opts into the persistent strip worker;
  ordinary `train --device uuid:A` selects the authority. A projects/bins once,
  both devices process complementary global rows, and A alone updates state.
- `SS_PPISP_REDUCTION=staged` is independent and also usable without the worker.
  It was selected in **all** matched native/strip arms. Unset or `native`
  retains the original PPISP reduction; the optimization is not default.
- Native pixel operators, globally phased pyramids/halos, globally aligned
  raw-statistic reduction, and pre-update gradient merging are implemented.
  The worker owns neither the model, optimizer nor sampler.
- Supported runtime scope is deliberately explicit: packed 3DGS/FPBO, one
  RGB/mask camera pass, fixed black background, supported PPISP/RGB-grid lanes.
  Unsupported lanes refuse; see the investigation rather than assuming generality.
- Final source proof passed **one** update, 10000→10001, on input 847 at 2880²:
  actual A/B owned RGB and last-intersection IDs were byte-exact against two
  full native replays; loss/raster/appearance gradient gates passed unchanged.
- A pre-fetch decode exception preserved serialized state/sampler, allowed a
  source-step checkpoint, then retried in the same session. This diagnostic
  used a quiescent sampler, not asynchronous-sampling coverage.
- Ordinary-CLI STOP/resume saved full checkpoints at 10002 and 10004. Worker,
  controller and parent loss refused checkpoint publication without orphaned
  workers or forced cleanup in those targeted cases.
- Final Vulkan build passed. Eight focused operator checks passed across both
  devices. No full suite, long quality campaign or full recovery certification.
- [Testing commands](../testing.md#experimental-same-view-image-strips) cover
  same-input proof, fixed-input continuation and CPU decoded checkpoint comparison.

The preserved source is a full native step-10000 checkpoint, 1M capacity,
30000-step horizon, with active PPISP and quantized RGB grid/AdaGrad. The
original-derived subset reconstructed 1083 views; its split is 975 training /
108 held-out. No new reconstruction or replacement source is needed to brainstorm.

## The two unresolved technical questions

### 1. Can this decomposition beat the optimized native critical path?

Warm strip traffic was about **84.1 MB input + 35.0 MB output**, plus 1.11 MB
collectives per update. Warm medians included snapshot 13.829 ms, authority
collective 20.312 ms, join 57.777 ms, worker pixels 62.145 ms and authority pixels
43.213 ms. These intervals overlap: **do not sum them** or treat every component
as removable overhead. The old `whole_step_ms` field measures pixel fork→join,
not the whole native trainer step.

The devices were separate single-device Vulkan groups. Extension availability
is not proof of peer access; opaque Win32 GPU-memory import is not a direct
cross-card shortcut here. Separately imported shared host memory or a
same-process multi-device runtime would need their own ownership, synchronization
and performance argument. No such transport has been demonstrated.

### 2. What numerical evidence is sufficient beyond one-step gradients?

The CPU endpoint report decoded native parameters, moments, bounds, row clocks,
appearance/AdaGrad and auxiliary state. Unexpected nonfinites were zero after
separating legitimate inactive all-three-NaN means. This is not equivalence.

Active topology reset 39079 source-row clocks. The 951161-row conditional cohort
is **not stable identity proof**, even though all endpoint clocks matched across
1M slots. Endpoints diverged materially even native-to-native; for example,
conditional means update-normalized errors were strip 1.29448 versus native
repeat 1.15877. Reduction ordering, quantization, topology and nondeterminism
are possible contributors, not established root causes. No full-update pass
threshold was declared, and none should be invented after seeing these values.

Historical B2 quality failures remain failures. A changed optimized-native
baseline needs fresh matched quality controls before any later qualification;
old metric envelopes and the latest short gradient proof are not interchangeable.

## Short-run follow-on disposition

The [short-run plan](multi-gpu-training-short-run-plan.md#execution-result--2026-10-17)
was executed without a longer campaign. Reversed authority `0100…` / worker
`0200…` was decisively slower than native on authority `0100…`: measured
updates 3–8 averaged 158.5575 / 96.8757 ms. All runs retained input 847,
full resolution, source step 10000 and the 30000-step horizon.

One eight-update common-host-clock diagnostic found worker finite validation
18.19–19.98 ms and the subsequent entire authority post-worker join
21.66–24.64 ms. Even hypothetically deleting both nonoverlapping result-tail
intervals leaves 119.13 ms/update mean, above native. No measured narrow fix
had a route to closing the gap; no cut sweep, longer run or permanent
performance patch followed. This rejects the tested executor-tuning hypothesis,
not every possible image-strip algorithm or transport.

Temporary instrumentation was removed, the Vulkan dev rebuild passed, and
restored finite-result runtime checks passed on both supported AMD devices.
No new numerical/ordinary-sampler/lifecycle qualification was reached, no
defaults changed, and no trainer/proof processes remained. Exact evidence and
limits are in the plan; private artifact locations remain in `PRIVATE-HANDOFF.txt`.

Larger batches, stale updates, independent-job throughput and capacity-only
work are not substitutes for lower latency of one synchronous training job.
The authority still owns the full model; larger-scene capacity is unqualified.
Earlier failed gates and completed runs remain preserved, not pending work.
