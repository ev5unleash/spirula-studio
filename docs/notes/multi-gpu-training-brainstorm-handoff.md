# Multi-GPU training: brainstorming handoff

Updated 2026-10-17. Implementation baseline: **`d1a829af`**.

**Executed follow-on: [short-run improvement plan](multi-gpu-training-short-run-plan.md#execution-result--2026-10-17).**
Physical-role reversal averaged 158.56 ms/update versus 96.88 ms native in the
matched 2+6 screen. One temporary timeline diagnostic rejected a result-tail/
timer-only fix; instrumentation was removed and the native build restored.
The single-model multi-GPU speedup goal remains unmet; native stays default.

## Read first

1. This handoff: current position and decisions worth discussing.
2. [Image-strip investigation](multi-gpu-image-strip-investigation.md): latest
   implementation, measurements, numerical contract, limitations and evidence.
3. [Comparison scorecard](multi-gpu-training-comparison-20260929.md): closed B2
   campaign and subsequent raster/strip outcomes.
4. [Original campaign plan](multi-gpu-training-plan.md): consult its completed
   execution results when needed. Its pilot questions and historical next-step
   instructions are not an unfinished execution queue. Do not read or revive
   its frozen ledger by default.

Private locations and retained run artifacts are indexed by the existing
`PRIVATE-HANDOFF.txt` beside the preserved corpus. Do not copy those paths into
this repository. Logs, output directories and commits are the evidence; the
retired call ledger, admission process, deadlines and source snapshots stay retired.

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
- Native stays default; experimental behavior is opt-in. Local commits only;
  no push or PR without a new explicit request.
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
