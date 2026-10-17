# Multi-GPU training: brainstorming handoff

Updated 2026-10-17. Implementation baseline: **`d1a829af`**.

**Next session is a design discussion, not an automatic experiment restart.**
The latest implementation is finished and committed. The broader desire for a
useful two-GPU training speedup is still unmet by a qualified candidate.
Native remains the default and recommendation.

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

## Brainstorm options — hypotheses, not selected work

| Direction | Question to answer first | Smallest useful follow-on, if selected |
|---|---|---|
| Consolidate the single-GPU win | How much does staged PPISP improve representative end-to-end training, not just its operator? | Matched short ordinary-sampler controls; agree quality evidence before changing defaults. |
| Salvage strips by cutting the critical path | Is enough wall actually removable to beat roughly 100 ms/native update, without changing the update semantics? | Use the retained timeline/traffic to identify one avoidable copy or wait; test only that hypothesis. Do not start a transport rewrite from nominal PCIe bandwidth. |
| Resolve numerical qualification first | Which part of native-repeat variation prevents a meaningful optimizer/topology comparison? | Reuse actual source/checkpoints and the decoded report to design a controlled comparison with defensible row/state correspondence. Do not disable active topology in a candidate and call it the same method. |
| Reconsider the product objective | Is the real value single-model latency, larger-scene capacity, or aggregate throughput of independent jobs? | Discuss explicitly. Independent jobs or larger batches are different objectives, not hidden wins under the current contract. |

The current strip implementation does **not** pool model VRAM: the authority
still owns the full model. Larger-scene capacity remains unqualified. Likewise,
a pipeline that computes the next update against stale weights would violate the
current contract, even if it keeps both GPUs busy.

## Suggested opening for the next conversation

> We have a working synchronous same-view split, but it loses to the efficient
> native control. Before changing code, which direction has the strongest case:
> establish the end-to-end single-GPU gain, identify a removable strip critical
> path, resolve update/topology comparison, or explicitly change the product
> objective? Use existing evidence first, separate facts from hypotheses, and
> propose only the smallest distinguishing experiment after we choose.

No new experiment, parameter change, cleanup or implementation is selected by
this handoff. Completed runs and failed gates stay preserved.
