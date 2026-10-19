# Same-view image-strip multi-GPU investigation

Status: **experimental implementation complete; native recommended.** The
two-device fixed-input training interval was about **75.4% slower** than the mean
of its two native controls. Checked operator/source gradients and lite lifecycle
checks passed; full-update equivalence, quality and recovery are not certified.

For the next design discussion, start with the
[overall-plan brainstorming handoff](multi-gpu-training-brainstorm-handoff.md).
The subsequent [short-run execution](multi-gpu-training-short-run-plan.md#execution-result--2026-10-17)
also rejected reversed physical roles (158.56 versus 96.88 ms/update). One
common-clock diagnostic found no narrow result-tail/timer fix capable of
closing that gap; no performance patch or default change was retained.

## Decision and numerical contract

The user requested lightweight option tests, not a full suite, then explicitly
selected implementation of a full two-GPU strip path rather than stopping at a
recommendation and optimized native baseline.

For this investigation the user chose **same training semantics, checked
numerics**, not byte-identical native optimizer state: retain the logical batch,
objective, active appearance/topology and one synchronous authoritative update;
require exact pointwise results where applicable and measure gradient/update
errors. Long-run quality and recovery remain unqualified. This does not waive or
relabel any failed check in the closed B2, G1 or raster-only investigations.

## Options and measured screening

| Option | Communication / dependencies | Decision |
|---|---|---|
| Raster backward only | Authority sends image caches/cotangents and receives screen/statistic partials | Prior [Gate 0](multi-gpu-training-synchronous-plan.md#gate-0-execution-result-2026-10-16) closed; not reopened unchanged |
| PPISP parameter-tail overlap | Keep the RGB VJP on A; send two float RGB images to B; return parameter gradients before the one optimizer | Reject against the efficient native control below |
| Whole-image delegation | A projects, B runs the pixel pipeline, A updates | Useful operator reference, not parallel acceleration of a dependent logical step |
| Full image-space strips | Same-view raster, appearance and loss work on both devices; combine global loss populations and data gradients | Implemented experimentally; measured fixed-input training slower than native, no speedup recommendation |
| Replicated synchronous Gaussian ownership | Duplicate substantial view work and exchange model updates; shared appearance/topology still need coordination | No measured advantage established by existing owner schedules; do not substitute freezing |
| Larger-batch data parallelism | Different views in one enlarged update and world-gradient reduction | Changes the current logical batch; outside the selected contract |

### Dominant PPISP control

A current-source borrowed-input probe ran one ordinary managed step per device,
with one warm replay and three measured joint / nullable-parameter RGB / staged
reduction replays. All arms used the same immutable inputs within that process.
Independent device sessions sampled different training frames, so these are not
a same-input cross-device equivalence comparison.

Both captured calls were 2880×2880, one camera, 975 parameter rows, nine PPISP
parameters, `no_crf_no_vig`, with 99,532,800 bytes per RGB image. The staged
scratch was 4,665,600 bytes. Timings are warmed launcher-plus-GPU-completion
wall, excluding input resets, allocation, readback and validation.

| Device UUID | Native joint median, ms | RGB-only median, ms | Staged median, ms |
|---|---:|---:|---:|
| `00000000020000000000000000000000` | 116.0596 | 1.1464 | 2.4568 |
| `00000000010000000000000000000000` | 82.4527 | 0.8211 | 2.5946 |

Joint/null/staged RGB cotangents were byte-identical and finite. Inputs remained
unchanged; the enclosing ordinary step completed one native update to 10,001.
The staged operator retains the native per-pixel VJP, contribution-level finite
filtering and 64-lane local tree, but writes unique group partials and reduces
them in a second dispatch. Its global addition order differs from native atomics.

| Device | Worst staged/native gradient maxabs | Worst staged/native relRMS | Native-repeat maxabs | Native-repeat relRMS |
|---|---:|---:|---:|---:|
| `0200…` | 1.210719347e-8 | 7.813066633e-7 | 1.396983862e-8 | 9.014823399e-7 |
| `0100…` | 5.876063369e-8 | 1.656049707e-4 | 1.708394848e-8 | 4.814467523e-5 |

All gradients were finite; up to nine active components differed. These are
operator diagnostics, not a long-run appearance or quality qualification.
PPISP-only offload would transfer 199,065,600 bytes for these inputs, or
353,894,400 bytes at 3840², to avoid roughly 2.5 ms of efficient local work.
It is not the option to implement against that baseline.

The staged reduction is a strong **standalone single-device optimization** at
this operator boundary: roughly 2.5 ms versus 82–116 ms for joint native PPISP.
It is independently opt-in, not evidence that a second GPU accelerates training.

Sources: `EnginePpisp.cpp`'s borrowed launch hook; `PpispImage.cpp`;
`ppisp_image.slang`; `g1_native_proof.cpp`'s `ppisp-source-price` mode. The hot
native reduction performs eight workgroup barriers per parameter and up to one
atomic add per parameter/group into the selected camera row.

### Transfer and topology probes

`vk_runtime_smoke --transfer-bench` ran separately on both selected devices:
32/256 MiB, pageable and pinned host routes, one warmup plus three repeats,
full deterministic round-trip byte validation. Allocations and validation were
outside synchronized copy timing. The 256 MiB medians were:

| Device UUID | Pageable H2D / D2H, ms | Pinned H2D / D2H, ms |
|---|---:|---:|
| `00000000020000000000000000000000` | 55.5425 / 53.2473 | 42.5157 / 40.5222 |
| `00000000010000000000000000000000` | 23.1812 / 22.8197 | 10.3445 / 9.9827 |

These are individual device/host legs, not measured end-to-end IPC throughput.
A staged GPU-A→GPU-B payload pays both legs, CPU mapping copies and protocol
completion. The asymmetric measured routes must not be replaced by nominal PCIe
bandwidth or a one-leg estimate.

Fresh `vulkaninfo` found two separate single-device groups. Both cards advertise
external Win32 memory/semaphore and external-host-memory extensions, but that is
not evidence of direct peer access. Vulkan requires imported Vulkan-created
external memory to use the original physical-device list; opaque Win32 GPU-memory
sharing is not a cross-card shortcut here. A separately allocated shared host
import would require its own support, synchronization and performance proof.

Primary references:
- [Vulkan memory allocation/import validity](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/memory.adoc)
- [External host memory restrictions](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/appendices/VK_EXT_external_memory_host.adoc)

## Selected operator seam

The authority projects and bins once. A persistent child receives prepared
screen/bin state, current GT/camera/appearance inputs and a globally aligned
horizontal region. Both devices use the existing pixel operators. Halos provide
SSIM and globally phased pyramid neighborhoods; raw sufficient statistics count
owned centers and combine before native normalization/VJP. Returned screen,
density and appearance data gradients combine before the authority's existing
projection/FPBO, appearance updates and densification.

The worker is not a trainer, model owner or sampler. No independently normalized
crop losses, stale weights, duplicated shared optimizer, frozen appearance or
batch enlargement is permitted. Appearance and topology remain active; only the
authority owns Gaussian/appearance optimizers and applies the one native logical
update. Default execution remains native when the experimental selector is absent.

## Opt-in and explicit limits

On Windows Vulkan, select authority A with ordinary `train --device uuid:A`
and set `SS_IMAGE_STRIP_WORKER_DEVICE=uuid:B` for a persistent worker on a
distinct supported non-NVIDIA device. Unset the variable for native training.
`SS_PPISP_REDUCTION=staged` separately selects the efficient two-dispatch PPISP
reduction; unset or `native` retains native reduction. Neither is a new default.

The admitted lane is packed `3dgs` with native FPBO, one equal-size camera/pass,
RGB GT with optional mask, fixed black background, optional PPISP and optional
native `ppisp` RGB bilagrid. Quantized grid camera slices must align to 256-value
blocks. The executor refuses BVH, B1/B2/debug modes, multi-camera/multi-pass
batches, depth/normal GT or grids, learned/nonblack backgrounds,
color-shift/overexposure regularizers, edge-aware loss maps and scheduled or
resolved distortion/depth-normal lanes. Invalid frame/index/transport extents,
stale or incomplete results and nonfinite merged gradients fail rather than
being applied. Partial or poisoned sessions cannot publish a checkpoint.

## Completed targeted evidence

### Build and operator checks

The dev Vulkan build used MSVC 19.44.35229.0 Release and Slang 2026.12.0.1 on two
AMD Radeon AI PRO R9700 devices, driver 32.0.31036.15 / 26.Q3. Build 8's full
native dev build passed. Build 9 corrected only fixture inputs and diagnostic
run-root config publication; production math/runtime were unchanged. The final
build 10 log also recorded native Vulkan **Build Complete**. Its protocol v5
addition is proof-only optional RGB/last-ID audit regions; inactive production
auditing adds zero payload bytes. Numerical math and the measured training
path were unchanged.

Build 9 ran `vk_runtime_smoke --strip-finite`, `raster_bwd_parity strip`,
`ppisp_parity strip` and `msloss_parity strip` on **both devices: 8/8 passed,
under 6 s total**. This was the requested lite selection, **not a full suite**.
No CUDA/NVIDIA work or long-run training/quality campaign was performed.

### Actual-source single-update proof

Final `source-proof-6/image-strip-source-proof.jsonl` completed exit 0 in
26.49844 s and passed **one native logical update, 10,000→10,001**, not 20
updates. Input 847 was 2880², from the full
1M-capacity source with horizon 30,000, 15 SH coefficients, native FPBO,
16-bit non-SH state, 8-bit SH optimizer/16-bit SH values, active nine-parameter
PPISP and a 16-bit RGB grid with 8-bit AdaGrad.

Two full native snapshots/replays were compared with actual A+B owned pixels
and merged loss/gradients. **Both roles' owned RGB and last IDs matched
byte/bit-exactly**. B covered 4,055,040 pixels: 48,660,480 RGB bytes plus
16,220,160 last-ID bytes, with zero differing bytes/elements. Exactly one
`image_strip_audit_pixels` worker callback ran before the authority update.
Native-repeat and dual-gradient gates passed, and all active appearance state
was finite. Proof 5's RGB evidence is retained; proof 6 closes the last-ID audit.
The gates were not relaxed: appearance/loss maxabs ≤1e-6 and relRMS ≤1e-3;
raster maxabs ≤1e-4 + 1e-4 times reference L2 and relRMS ≤5e-4, alongside
pointwise RGB equality and exact-zero reference-array checks. Gradient gates
are not canonical optimizer-byte or full-update equivalence.

An explicit pre-fetch `DataDecodeError` left serialized state and sampler
unchanged, with zero forks/joins. A full source-step checkpoint was saved,
then same-session retry completed one fork/join/update. This used a
**quiescent diagnostic sampler**, not ordinary asynchronous-sampler coverage.

Final build 10 also completed an **audit-free two-update normal source-run**:
exit 0, full checkpoint 10,002, and one final run-root config published by the
existing native writer. Input payloads were 83,514,944 / 84,160,064 B; outputs
were 34,522,816 / 35,056,448 B, with no RGB/last-ID audit payload. Full path was
14.2850 s, dominated by setup/save; this is a normal-path publication check,
**not a new timing comparison**. It does not expand diagnostic-sampler coverage.

### Matched fixed-input continuations: no speedup

Three actual `TrainerSession::train` continuations used the identical source,
config, input 847 and horizon 30,000, staged PPISP in both methods, periodic
save 0, and one final full native checkpoint at 10,020. Proof/audit were disabled.

| Arm | Training loop, s | Sum of step times, s | Train call + checkpoint, s | Full path, s |
|---|---:|---:|---:|---:|
| Native A (`native-20-1`) | 2.0089653 | 1.9804306 | 6.0868525 | 17.0740027 |
| Strip (`dual-20-1`) | 3.5310030 | 3.5079747 | 7.4779381 | 15.1915642 |
| Native B (`native-20-2`) | 2.0181925 | 1.9868428 | 6.1197606 | 11.6678778 |

Dual training was about **75.4% slower** than the mean native training loop.
Full-path setup/cache/I/O noise and run ordering preclude a performance-qualified
wall comparison; the middle full-path value is not a speedup claim. Fixed-input
diagnosis is not ordinary sampling or quality evidence.

Warm 19-step medians were 84,135,296 B input, 34,983,104 B output and
1,105,928 B collective traffic. Timing medians in ms:

| Snapshot | Map | Worker H2D | Worker D2H | Worker pixel | Authority pixel | Authority collective | Worker collective | Join wait | Join |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 13.829 | 4.174 | 4.990 | 1.332 | 62.145 | 43.213 | 20.312 | 2.101 | 31.902 | 57.777 |

These components overlap and **must not be summed**. The old logger's
`whole_step_ms` median of 163.432 ms is the fork-to-join pixel interval,
**not the whole trainer step**.

### Decoded native endpoint comparison: descriptive, not a pass

Final `update-compare-3/native-checkpoint-updates.jsonl` completed on CPU,
exit 0 after 16.12995 s, schema `native_checkpoint_updates_v1`: 24 decoded fields,
28 persistent arrays, 44 field/cohort rows and 18 raw-storage rows. Coverage
includes Gaussian parameters, Adam g1/g2, native codecs/bounds, row clocks,
PPISP, grid/AdaGrad and radii/accumulation. Clocks matched **exactly across
all 1M slots** for the three endpoints.

Topology remained active: source inactive NaN-mean rows were 17,438; endpoint
counts were native A 3,761 / strip 3,687 / native B 3,773. Each endpoint had
39,079 clock-reset rows. The 951,161-row no-observed-reset-or-mean-change cohort
is **not stable-identity proof**. Final comparison 3 separately classifies
legitimate newly inactive all-three-NaN world means as
`endpoint_inactive_mean_nan`: native A 11,112 / strip 10,851 / native B 11,166
**components**, not numerical failures. Unexpected nonfinite counts for the
source/native A/strip/native B were **all zero**, with no ±Inf means.
Comparison 2 is retained with its known `unexpected` labeling defect; the
reviewed report-only correction leaves no residual classification issue.
The same 20-update endpoints were reused, with no new training, tolerance or
acceptance change; all other pair/transition counts and metrics were unchanged.

| Cohort field | Strip update-normalized error | Native-repeat error |
|---|---:|---:|
| Means | 1.29448 | 1.15877 |
| Quaternions | 0.102905 | 0.099744 |
| Scales | 0.042446 | 0.040477 |
| Opacity | 0.061178 | 0.057566 |
| DC | 0.076909 | 0.075968 |
| SH | 0.093579 | 0.093928 |
| PPISP parameters | 0.004993 | 0.010561 |
| Grid parameters | 0.020170 | 0.026583 |
| Grid AdaGrad | 0.027107 | 0.012658 |

The comparator declares **no pass tolerance or qualification**. Repeated
endpoints/topology materially diverged, including native-to-native; these
measurements do not certify full-update equivalence, quality or recovery.
The accepted source proof is a gradient gate only.

### Real CLI lifecycle: lite safety only

These runs used the **ordinary sampler**, not fixed-input replay:

| Case | Root / worker exit | Full checkpoint |
|---|---|---|
| STOP | 42 / 0 | 10,002 |
| Resume that actual STOP checkpoint, then STOP | 42 / 0 | 10,004 |
| Controller EOF | 1 / 1 | None |
| Kill exact worker with exit 93 | 1 / 93 | None |
| Kill parent | 1 / 43 | None |

No case required forced cleanup. STOP/resume publication and abnormal-loss
refusal are targeted safety observations, **not full recovery qualification**.

### Preserved failures and corrections

- Source proof 1 failed SSIM scalar abs 8.34465e-6; native and strip now use
  the tiled pairwise reduction.
- Proof 2 exposed a one-ULP weighted-loss difference, 1.90735e-6.
  `msloss` attempt 3 exposed raw `DepthSupXX` drift 0.046875 above 0.03.
  Globally aligned per-row raw statistics are now reduced after the collective;
  all native/strip raw rows were exact in the final fixtures.
- A new perfect-image scalar regression hit native PSNR's zero-MSE AD
  singularity. The fixture now uses near-identical reference +1e-5, keeping
  expected SSIM 1±1e-6; **production PSNR math was not changed**.
- CPU comparison 1 lacked run-root config because the diagnostic source-run
  writer was disabled with proof. The existing canonical writer is now enabled.
  The three timed artifact roots were repaired by copying authentic embedded
  configs only: no state, checkpoint or timing was modified. The repair receipt
  is preserved.

No numerical gate was relaxed; earlier failures remain part of the record.

## Disposition and evidence

Implementation is complete and experimental, with native still the default
and recommendation. The staged PPISP operator is the strong standalone
optimization; this two-GPU transport/pixel path did **not** demonstrate speedup.
There is no full-update, long-run quality, capacity, durability or recovery
certification, and no full-suite claim. Closed B2/G1/raster-only outcomes remain
closed and are not relabeled by these results.

Private durable evidence is identified here by artifact basenames only:
`gpu-options-lite-build.log`, `gpu-options-vulkaninfo.log`,
`gpu-options-transfer-{0,1}.log`, `gpu-options-ppisp-price-{0,1}.log`,
`gpu-options-price-summary.json`, `ppisp-source-price.jsonl`, the build 9 and
final build 10 logs, `build9-fixtures.json`,
`source-proof-{5,6}/image-strip-source-proof.jsonl`,
`{native-20-1,dual-20-1,native-20-2}/image-strip-source-run.jsonl`,
`update-compare-{2,3}/native-checkpoint-updates.jsonl`,
`gpu-strip-final-normal-2.log` and its `image-strip-source-run.jsonl`,
`life-{stop,resume,controller,worker,parent}-1.{log,json}`, and
`gpu-strip-diagnostic-config-repair.json`. The build 9 operator checks and
five protocol-v4 CLI lifecycle receipts are retained; the final protocol-v5
audit-free publication check does not claim those lifecycle cases were rerun.
The source experiment and its failures
are preserved; the full native step-10,000 source and protected dataset remain
unchanged.
