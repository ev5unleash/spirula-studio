# M0 execution record — 2026-09-27

Status: **M0 timing decision recorded; cooperative scaling unverified.** Earlier
quality-first and 20% stop gates below are historical decisions, superseded
for the requested performance-only comparison on the supplied kitchen dataset.
The current benchmark disposition is at the end of this record; the experiment
specification is [M0](multi-gpu-training-micro-01.md).

## Observed local lane

- Source `8f8c0f5a58c4740824ef5e9d4a335614424e4860`; worktree was dirty
  before execution (the M0 roadmap, M0 plan and `docs/README.md`). No source
  revision or probe was introduced for this run. A comparative run still needs
  a digest of its exact patch.
- Windows 11, two AMD Radeon AI PRO R9700 discrete Vulkan devices reported by
  `vulkaninfo --summary`: UUID selectors
  `uuid:00000000010000000000000000000000` and
  `uuid:00000000020000000000000000000000`; AMD proprietary driver 26.Q3,
  Vulkan API 1.4.349. These are the selector's 32-hex-digit form, not the
  hyphenated display form from `vulkaninfo`. Host physical RAM: 50,260,623,360
  bytes. Usable memory budgets, topology, storage, NUMA and other manifest
  fields have not been measured.
- Built `build_vulkan` via
  `cmd /c .\build_develop.bat -DSS_BACKEND=vulkan -DSS_BUILD_BACKEND_TESTS=ON -DSS_BUILD_GUI=OFF`.
  CMake configured Release with MSVC 14.44.35207; `slangc -version` reported
  2026.8. The build completed at `build_vulkan\spirula.exe`. Enabled device
  capability variants have not been recorded for a comparative manifest.
- `cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error -j 2`:
  14/14 passed. With `SS_TEST_DEVICE` set separately to each UUID selector,
  `cmake -E chdir build_vulkan ctest -R "^cli_training_smoke$" --output-on-failure --no-tests=error`
  passed 1/1 on each device (4.32 s and 1.72 s total CTest times, **not**
  comparable training timings). The first attempt used hyphenated UUID syntax
  and failed selector resolution before training; correcting the selector
  resolved it. The smoke test generates six 64×64 views and 64 seed points,
  trains 12 steps with a 256-row cap, then tests checkpoint resume and worker
  lifecycle. It does not measure target quality, capacity or throughput.

## Historical open M0.1 gate

No representative fitting and target/stress dataset, intended final model
size/resolution, wall-time and RAM/VRAM budgets, global-coupling requirement,
or held-out global/boundary quality envelope was supplied. These must be fixed
before comparing candidates. The generated smoke fixture cannot stand in for
either workload. No early/middle/late growth windows, simultaneous phase
residency, visibility trace, or completed transport measurements were taken;
M0.2–M0.5 are **unverified**, and no B1/O1/G1 branch is selected.

Next bounded action: obtain the two workload identities/configurations and
target/acceptance envelope, then freeze the M0.1 manifest and run a supported
single-device baseline. Add phase and visibility probes only for the rows that
remain needed by that baseline's limiting factor.

## 2026-09-27 continuation: X4 dataset, provisional target

The earlier gate above is a historical observation. A private local manifest
now holds the dataset path, run commands, checkpoint identities, raw tool-output
locations and source patch digest; none of those private paths belongs in this
repository. The source revision remains `8f8c0f5a`. This continuation used the
headless Release Vulkan build with MSVC 14.44.35207 and pinned Slang 2026.12.0.1.
Both selected devices were AMD Radeon AI PRO R9700 on driver 26.Q3, reporting
32,624 MiB apiece. PCIe topology, NUMA and usable memory budgets remain unknown.

The supplied dataset parses as 3,000 views with 501,607 seed points; the input
sample inspected was 1024×1024. The measured held-out run used `eval_mode=fraction`
and `train_split_fraction=0.9`: 2,700 training views, 300 evaluation views.
It retained the original 50,000-step horizon, native 3DGS/SH3, 3M cap,
quantized moments, packed projection, disk image cache and split-batch path.
The managed path resolves requested FPBO=1 to effective FPBO=0 for this
multi-view batch. Full checkpoints were retained every 5,000 steps.

The **provisional**, not user-approved, target was 6M reserved rows on this
dataset at the same image resolution, 50,000 steps within three hours, an
85%-of-reported-VRAM planning bound (27,730 MiB/device), 85% of host RAM,
and held-out PSNR at least 24 dB. The proposed quality comparison would allow
at most 0.5 dB mean and 1.0 dB boundary-view regression; the boundary set
has not been defined. Independent blocks were allowed provisionally only as
an experiment, not accepted as a quality-preserving mode.

| Observation | Result and interpretation |
|---|---|
| Original all-images run | Completed 50,000 steps in 02:46:08.924 with 3M cap; no held-out quality, full-checkpoint or controlled window evidence. Historical, not a comparator. |
| Held-out fitting reference | Completed 50,000 steps in 02:05:27.788, 3M active splats, `metrics.json` reports 300 views, PSNR **19.7718 dB**, SSIM 0.758626, L1 0.068432; color-corrected PSNR 21.8193 dB. Native `engine_vram` reports 1815.63 MiB, not a simultaneous phase peak. The provisional 24 dB raw held-out quality gate failed. |
| Early profile, device 0 | Resumed a full checkpoint at step 500 for 202 instrumented steps; 23.580 s profile wall, 17.676 s device wait, 1.469 s transfers, 1552.29 MiB pool retained high-water and 1553.30 MiB driver process memory. It is not an unprofiled speed measurement. |
| Late profile, device 0 | Resumed step 45,000 for 202 instrumented steps; 42.943 s profile wall, 36.459 s device wait, 1.260 s transfers, 1860.16 MiB pool retained high-water and 1861.95 MiB driver process memory. The profile includes a first resumed step and checkpoint. |
| Provisional 6M-cap stress, device 1 | 602 steps, 526,687 active splats, 6M reserved, 2640.72 MiB pool retained high-water, 2641.61 MiB driver process memory. This proves startup/reservation and early growth fit on that device, **not** 6M active rows or late scratch. |
| 6M held-out control, device 1 | From initialization, 47,602 steps in 03:00:10.294; stopped at the declared three-hour limit, with a full checkpoint. Resumed on the same device and completed 2,398 steps in another 00:08:22.095. Combined reported training times exceed the limit by at least 8:32.389, excluding resume setup/eval. At 6M active splats the native `engine_vram` figure is 3014.71 MiB. On 300 held-out views raw PSNR is **19.8785 dB**, SSIM 0.764591, L1 0.067414; color-corrected PSNR 21.9606 dB. It fails the provisional 24 dB quality gate by 4.1215 dB. The 0.1067 dB raw PSNR difference from the 3M reference is not a paired or repeated estimate: device, stochastic trajectory and stop/resume differ. |

The first 3M diagnostic run failed at its first full checkpoint with
`engine_save_checkpoint: optimizer layout changed`: the checkpoint compared
requested FPBO=1 to effective FPBO=0. A temporary explicit-FPBO=0 run saved
checkpoints at 500/1000/1500/2000/2002 without changing the executed
multi-view path. The source fix now shares the DataManager's effective resolver
with checkpoint save and resume-target layout. With FPBO requested on, an X4
full checkpoint at 500 succeeded, and a 202-step profiled resume from it
succeeded. Vulkan dev build, 14/14 headless tests and 1/1 AMD-device
`cli_training_smoke` passed. A scoped static review found no actionable issue.

**Decision at this gate:** stop M0 without choosing B1, O1 or G1 for these
provisional requirements. The native path fits both tested caps by a wide
reported memory margin; 3M finishes within three hours but misses the
provisional 24 dB quality threshold by 4.2282 dB. A full 6M control also
misses quality by 4.1215 dB and needs more than three hours. Additional
training capacity alone has not solved the target; none of these outcomes
establishes a multi-GPU capacity problem. The one next bounded experiment
is single-GPU held-out-view/input diagnosis using the same 300 views, checking
registration, masks and image/pose alignment against the worst views before
choosing a representation or revising the quality envelope. Its falsifiable
hypothesis is that identifiable input/geometry errors explain the persistent
roughly 4 dB deficit; stop and explicitly revisit the quality requirement
if that diagnosis cannot account for it. This is **not** approval to lower
the quality bar or start a distributed implementation.

M0.2 simultaneous phase peaks and growth allocator events, matched 200-step
windows with three repeats and view IDs, M0.3 visibility/partition traces,
M0.4 completed two-process host staging, transport contention and boundary
quality remain **unverified**. The profiler totals are not transport or phase
residency evidence. The provisional quality/time miss is a stop gate before
spending on those probes; it is not evidence that blocks/offload/ownership
would solve quality. This is a negative M0 gate, not a completed feasibility
acceptance, and no multi-GPU production-mode acceptance is claimed.

## Historical revised screening gate for a short-runtime continuation

The 24 dB/three-hour criteria above describe the completed full-run experiment;
do not relabel either historical result as passing. For the next M0 feasibility
pass, the plan uses a provisional **19 dB raw held-out PSNR floor** on the same
300 views, with no more than 1 dB mean and 2 dB predeclared boundary/worst-view
regression against a matched control. Both completed single-GPU runs clear the
floor, but no candidate has passed the relative or boundary checks. This
screen is deliberately weaker than a production quality acceptance.

The next action is the [short-window protocol](multi-gpu-training-micro-01.md#short-runtime-decision-before-another-full-train):
20 warmup plus 200 measured steps, three independent resumes per state, with
the original schedule horizon. Use the 3M early/middle/late and 6M near-final
snapshots, matching active rows between each candidate and its own control
on an explicit AMD device; do not compare 3M speed directly with 6M speed.
Measure unprofiled step-time distributions separately from phase diagnostics.
Stop a candidate after its roughly 15-minute window budget unless variance
changes the decision. The provisional runtime signal is 20% lower median
steady-state step time at unchanged exposure semantics; no candidate runtime
or quality result has been measured under this revised gate yet. Full-run
time-to-quality and production boundary quality remain open gates.

## 2026-09-27: separate kitchen dataset intake

The supplied kitchen dataset is **not X4**. X4's retained checkpoints, 300-view
evaluation split and provisional 19 dB screen cannot be transferred to it.
The private local manifest records its path and output identities. The dataset
contains two COLMAP models; the native parser chose `sparse/0` (1,327
registered images), yielding 1,195 usable cameras/faces and 488,119 seed
points. An inspected JPEG was 3024×4032. No masks, depths or normals were
present in the supplied root. Its `.spirula_manifest.yaml` points at a
different, absent image directory, but the native COLMAP parser used the
supplied root's `images/`; do not infer that the manifest selected the images.

On AMD Radeon AI PRO R9700 device 0, an experimental **fitting control**, not
the unspecified target workload, used the native `3dgs` defaults with a 1M
cap, disk cache, fraction evaluation split (0.9 training), full checkpoints,
viewer disabled and the original 30,000-step schedule. A cooperative stop
left a complete step-1 checkpoint. A resumed unprofiled run stopped at step
134 after 133 steps in 44.223 s; a second unprofiled resume from that same
checkpoint stopped after 113 steps in 37.276 s. A separate `SS_PROFILE=1`
resume from step 134 stopped after 112 steps in 37.158 s. The three resumed
runs exited with code 42 and retained full checkpoints. These are short,
stochastic, differently sized windows, **not** the plan's three matched
20-warmup/200-step repeats or a profiling-overhead estimate.

The instrumented window reported 1,676.88 MiB pool capacity (952.52 MiB
categorized as image, 350.36 MiB appearance, 307.37 MiB splat, 38.73 MiB
splat×image, alias arena included), 1,677.70 MiB driver process memory,
and 2,612.94/32,624 MiB device in use/total. These are retained high-water
figures, **not simultaneous phase peaks or an available-memory budget**.
Its 112 calls each to `ppisp_image_bwd` and `rasterize_bwd_2d` cost 16.87 s
and 12.79 s of stamped GPU time; transfers were 4.77 GB H2D/0.90 GB D2H
with 0.645 s summed transfer wall time, versus 38.89 s profiled total.
This early snapshot suggests high-resolution per-image work merits a
controlled test; it does not establish input wait as the sole bottleneck or
support any multi-GPU throughput claim.

**Historical gate:** kitchen has no retained middle/late or growth-state checkpoints,
held-out metrics, defined boundary/worst-view IDs, intended target cap,
quality and wall-time envelope, or agreed independent-block tolerance. Thus
M0.1 comparative baseline and M0.2–M0.5 remain unverified for kitchen. No
B1/O1/G1 branch is selected. The short screen below uses a provisional
fitting question, not a production envelope. Do not substitute the
X4 quality floor or this short training PSNR for kitchen held-out quality.

### Short comparative screen on kitchen

To bound the cost before another full train, use a **provisional fitting
question**: native 3DGS/SH3, full input resolution, 1M cap and original
30,000-step schedule on one device, with 0.9 training fraction and disk cache.
For planning only, use a three-hour wall-time ceiling and 85% of the reported
32,624 MiB device total (27,730 MiB); there is no agreed kitchen held-out
quality floor, boundary set or target larger than the fitting cap. These are
not production acceptance criteria. No other training candidate is proposed
until a capacity, quality or time deficit is established.

From the same step-134 full checkpoint, separate unprofiled resumes on each
AMD device logged steps 201, 301 and 401, then stopped cooperatively (exit
42, complete checkpoints). The 201→401 interval covers **200 steps after 67
preceding warmup steps**; all three used 488,119 active rows and the same 30,000
step schedule:

| Device and comparison | Elapsed at step 201 / 301 / 401 | 200-step interval | Mean over interval |
|---|---:|---:|---:|
| R9700 device 0, full resolution | 23.118 / 57.416 / 91.970 s | 68.852 s | 344.26 ms/step |
| R9700 device 1, full resolution | 23.106 / 57.401 / 91.283 s | 68.177 s | 340.89 ms/step |
| R9700 device 0, divisor 2 diagnostic | 5.405 / 13.264 / 20.980 s | 15.575 s | 77.88 ms/step |

The two full-resolution means differ by about 1%; one repeat per device,
100-step timestamp granularity and non-replayed stochastic sampling cannot
establish a median, p95, worst-view cost or a reliable device ranking. Setup
and stop-checkpoint costs are excluded from these intervals. Divisor 2 is
4.42× faster in this early window, but changes pixel count and the objective;
it does **not** solve the full-resolution target or measure a multi-GPU gain.
Combined with the separate profile, it points first to image-resolution
work, not demonstrated persistent-state overflow or input-wait dominance.

**Historical decision:** stop this short screen without B1/O1/G1 or a transport probe.
The only falsifiable next experiment is a full-resolution single-GPU control
through growth and held-out evaluation at this fitting cap, retaining
middle/late checkpoints. If its measured end-to-end quality/time or memory
misses an agreed kitchen target, run matched three-repeat windows and the
specific residency/visibility probe implicated by that miss. Early ETA is
an extrapolation, not a three-hour completion result; neither a 1M fitting
snapshot nor a halved-resolution diagnostic proves target capacity. This
leaves the M0 completion checklist open rather than promoting a preliminary
screen to a feasibility decision.

### Full-horizon fitting control and M0 stop (2026-09-28)

The same 1M/full-resolution/30,000-step native configuration was resumed
from the retained step-134 checkpoint on R9700 device 0, with full checkpoint
save every 5,000 steps and only the latest checkpoint retained to bound disk
use. The run reached 1M active rows near step 2,000, finished step 30,000,
published a complete full checkpoint, and evaluated the 132 held-out views.
Its resumed training segment took **02:40:31.995** for 29,866 steps;
`metrics.json` records 9,631.99 s training time, raw PSNR **20.2990 dB**,
SSIM 0.805369, L1 0.083895 and color-corrected PSNR 25.4256 dB.
`engine_vram` was 1,949.81 MiB. The earlier 134 steps, setup, checkpoint
I/O and evaluation are not in the reported training time; the full
initialization-to-quality wall time was not captured as one interval. The
driver/process residency at peak and phase/growth allocator events remain
unmeasured. Nevertheless, this complete run demonstrates that the tested
1M/full-resolution workload fits and finishes its training segment inside
the provisional three-hour limit with a wide reported memory margin.
The final full checkpoint restored in a separate native process with zero
remaining training steps; its 132-view evaluation reproduced raw PSNR to
within 0.000002 dB and exited successfully.

The 19 dB floor in the X4 plan was defined for **X4's** 300 views, not this
dataset. No kitchen floor was fixed before this run; 20.299 dB is an
observed held-out baseline, **not a retrospectively passed quality gate**.
The worst raw held-out entries in `metrics.json` are roughly 10.42–10.78 dB;
their image identities and any predeclared boundary set are still absent.
The disk-bounded latest-only setting also discarded the middle/growth
checkpoints, so the plan's matched early/middle/late repeats cannot be
claimed. The early two-device 200-step measurements remain a feasibility
screen, not a full-run scaling comparison.

**Historical M0 decision for the provisional fitting workload:** stop before M0.2–M0.4
and do not implement B1, O1 or G1. Persistent-capacity overflow is not the
limiting factor at 1M: a native single-device control completed at full
resolution. No per-step transport can be justified by the observed target,
and the half-resolution diagnostic changes that target. The next *conditional*
micro-plan is an existing-path, single-GPU investigation of the worst held-out
views and high-resolution image work. Its hypothesis is that input/pose or
large-image processing explains the quality/time headroom; stop it if the
error is not attributable, and revisit the quality envelope or representation
explicitly. Start B1/O1/G1 only after a larger capacity target or a coupled
quality requirement is specified and the corresponding M0.2/M0.3/M0.4
evidence is collected. This is a conditional early-stop decision under M0's
section 1, **not** completion of the full M0.2–M0.5 checklist or production
multi-GPU acceptance. The user subsequently requested the comparative GPU
timing lane explicitly, so this stop no longer governs the current M0 run.

### One versus two GPUs: independent-job timing (2026-09-28)

Two supported R9700 devices ran one trainer process each from the same full
kitchen checkpoint and the original 30,000-step schedule. Each window used
the same full-resolution disk-cache configuration, 100+ warmup steps, then
200 steps timed between CLI progress lines. Checkpoint I/O was moved outside
the interval; no profiler was enabled. Middle and late used three separate
restarts each for the one-device control and concurrent two-device case;
early used one of each. Both devices kept 488,119 active rows in the early state
and 1M at middle/late; the latter checkpoints were retained by a second
complete 30,000-step native run (02:40:40.982 for the resumed segment,
132-view raw PSNR 20.2877 dB). The first full run was not a matched
two-device quality or speed result.

| State | One process on device 0: 200-step seconds | Concurrent device 0 / device 1: seconds per 200 steps | Median aggregate independent-job throughput |
|---|---|---|---:|
| Early, step 134 | 66.951 | 66.244 / 66.644 | 2.01× (one sample) |
| Middle, step 10,000 | 65.320, 65.864, 65.492 | 65.597/66.382, 65.687/65.658, 65.525/66.058 | 1.98× |
| Late, step 25,000 | 62.924, 62.269, 61.406 | 63.568/62.722, 61.907/62.825, 62.788/63.209 | 1.97× |

For paired rows, the slower worker's interval bounds each simultaneous
200-step pair; scaling is `2 × median(single interval) / median(pair max)`.
The middle medians are 65.492 s single versus 66.058 s pair maximum;
the late medians are 62.269 versus 63.209 s. This measures **two independent
model updates per wall-time**, not one model receiving the combined views,
and it cannot be called a cooperative 2-GPU training speedup or a
time-to-quality gain. Random sampler/background trajectories are not
bit-replayed; aggregate 100-step timestamps cannot yield per-step p95 or
worst-view IDs. Startup and output publication are separate costs. The
near-2× result does indicate no large host/cache contention for these
independent processes at the tested workload. Only two supported AMD GPUs
are present; a three-device timing row is unavailable.

The phase and transport diagnosis below is separate from these unprofiled
step intervals. No single-model two-GPU speedup has yet been measured.

### Residency, visibility and staged-copy bounds

Instrumented 302-step middle/late resumes from the same checkpoints were
run separately from the throughput windows. Middle retained pool capacity
was 1,782.16 MiB and driver process memory 1,783.89 MiB; late was
1,870.14 and 1,873.70 MiB. Late's image category alone held 952.52 MiB
capacity, versus 313.23 MiB categorized as splat state/gradients and
110.43 MiB splat×image. PPISP image backward took 48.24 s and raster
backward 25.72 s over 302 calls; the profiled wall was 97.09 s. H2D moved
11.80 GB in 1.226 s; D2H 0.193 GB in 0.056 s. GPU stage stamps overlap
other timing rows and cannot be summed as critical-path wall. These are
**retained high-water** allocations and profiled times, not simultaneous
phase residency or normal step timing; topology-growth allocation events,
host RSS and usable device budgets remain unmeasured.

A separate zero-training-step diagnostic on the final 1M-row checkpoint
captured packed projection candidates and tile intersections for all 132
held-out input views. The x/y/z spatial median splits assign exactly 500k
rows per owner, breaking coordinate ties by row ID. Each view projected
rows from **both** owners. Candidate `nnz` was 101,486 median, 645,693 p95
and 802,846 maximum; tile intersections were 479,776 median, 1,292,763
p95 and 1,481,052 maximum. The worst input IDs were 37 and 38, with
roughly 803k projected candidates each. These are conservative projected
sets, **not** contributing or occlusion-safe subsets. No `C×N` matrix was
exported; the capture copied one view's packed IDs while its forward
buffers remained live.

| Equal-row spatial axis | Views assigned to owners | Intersection-load ratio | Unique foreign context rows (owner 0 / 1) | Foreign projected fraction |
|---|---:|---:|---:|---:|
| x | 100 / 32 | 2.41× | 368,554 / 335,529 | 39.6% |
| y | 79 / 53 | 2.19× | 365,414 / 244,262 | 36.1% |
| z | 98 / 34 | 4.95× | 369,320 / 281,435 | 40.7% |

View assignment here goes to the owner of more projected rows. The best
simple y split needs up to **865,414 owned+context rows** on one worker and
744,262 on the other, with 609,676 context copies in total; read-only
context does not need optimizer moments. A greedy y-axis view rebalance
brings intersection load near 50/50 in this trace but increases the summed
foreign-projection fraction to 37.9%; its distinct context union and
training quality were **not** measured. Pixel work, global appearance,
occluders and per-view image loss still need B1 validation. This trace
suggests block capacity is plausible but a naive equal-row split is not a
2× compute schedule.

The diagnostic then swept **all 1,195 training views** at the same frozen
final checkpoint, rendering without training or scoring. Projected `nnz`
was 97,760 median, 563,094 p95 and 803,047 maximum; intersections were
472,898 median, 1,173,299 p95 and 1,508,647 maximum. All axes again own
500k rows per worker:

| Axis | Assigned views | Intersection-load ratio | Foreign context union (owner 0 / 1) | Foreign projected fraction |
|---|---:|---:|---:|---:|
| x | 875 / 320 | 2.17× | 368,581 / 425,675 | 39.2% |
| y | 695 / 500 | 2.04× | 368,635 / 319,676 | 35.8% |
| z | 897 / 298 | 5.24× | 369,328 / 370,531 | 40.8% |

The y split has the least projected foreign work, context duplication and
intersection imbalance of these three **simple** axes. It still has
1,187/1,195 views touching both owners and splits intersection load
420.5M/206.1M. Its context unions imply **868,635/819,676 owned+context
rows** on the two workers, 688,311 duplicated context rows in total. A greedy visibility-aware view
rebalance reaches 50.2% of intersection load on one worker at 37.3%
foreign projected rows, but its distinct context union was not captured.
This training-view sweep supersedes the held-out-only trace for B1 sizing;
both are frozen projection diagnostics, not block-training measurements.

The backend's actual pinned-host copy path was measured on both AMD UUIDs
with reused buffers, four warmup round trips, content validation and
completed stream fences. Representative local single-device times:

| Payload | Device 0 H2D / D2H | Device 1 H2D / D2H |
|---|---:|---:|
| 64 KiB | 0.068 / 0.073 ms | 0.080 / 0.094 ms |
| 1 MiB | 0.110 / 0.175 ms | 0.253 / 0.317 ms |
| 16 MiB | 0.721 / 1.051 ms | 2.721 / 2.858 ms |
| 64 MiB | 2.561 / 3.051 ms | 10.616 / 10.774 ms |

The large-payload device asymmetry repeated in reverse run order. Topology
and PCIe link state are unknown; these local rates do not prove why device
1 is slower. A bounded binary pipe bridge then measured two processes in
both directions, with an ACK after GPU B completed upload and a verifying
readback. Each packet waits for that ACK before the next, so no compute or
transport overlap is credited. The first packet includes process setup and
is excluded from the following steady medians. The private parent pipe bridge
records per-packet read, relay and receiver ACK wall time; the C++ benchmark
prints cumulative local-copy/pipe-stage totals, **not** these medians:

| Payload | A→B completed packet | B→A completed packet |
|---|---:|---:|
| 64 KiB | 0.564 ms | 0.511 ms |
| 1 MiB | 3.840 ms | 3.538 ms |
| 16 MiB | 47.065 ms | 46.460 ms |
| 64 MiB | 187.999 ms | 180.495 ms |

Those **conservative measured protocol times include pipe copying, host
content checks, device poisoning and an extra GPU B D2H validation read**;
they are not a production packet implementation or a pure traffic floor.
For example, the isolated local-copy lower bound for a 64 MiB A→B packet
is 3.051 + 10.616 = 13.667 ms before the process hop, packing and sync;
the measured serialized verified packet is about 188 ms. Simultaneous
opposite-direction 64 MiB streams measured roughly 192 and 186 ms median
packets, versus 188 and 180 ms isolated, with verification traffic included.
One 16 MiB IPC verification attempt failed immediately after a build with
a payload mismatch; a one-packet check and six subsequent eight-packet
repeats passed. Treat transport reliability as an open gate, not a clean
production acceptance. Packet sizes derived from actual block manifests,
whole-job staging/merge costs and contention during training remain open.

### M0.5 decision and next slice

The user explicitly requested a cooperative timing comparison, so the
fitting single-GPU capacity result is not treated as a reason to abandon the
experiment. The measured **one/two-GPU timing** is currently for two
independent native jobs, not for one cooperative model. A single 1M-row job
fits on one device; O1 has no observed capacity need. The equal-row visibility trace makes B1
the smallest conditional cooperative experiment: no per-step cross-process
tensor traffic is required, while a y-axis owned/context split gives a
measurable but imperfect starting schedule. G1 would pay per-step routing
and gradient-return traffic; the measured serialized host path and the
worst nearly global views make it a poorer first implementation experiment.

**Historical selection of B1 as the next falsifiable experiment, not as an accepted mode:**
one-block parity first, then two independent owned/context
blocks on explicit AMD devices with the native renderer/optimizer and one
merged output. Its falsifiable timing hypothesis is at least 20% lower
median steady-state wall per unchanged total input exposure at the same
active-model size, including a separately reported partition/staging/merge
cost; the two-block result must also retain matched held-out and boundary
quality before it can advance. Stop B1 if context or load balancing erases
that gain or if a coherent merged scene fails quality. This is a next
**experiment**, not a measured single-model speedup. Actual block-manifest
packet sizes, whole-job staging, context quality and phase peaks are entry
measurements for B1, not values this report has passed. No agreed kitchen
production-quality envelope or boundary-view set exists, so production
acceptance cannot be claimed. M0.2 phase peaks, B1 context-quality and
multi-worker job staging are still unverified and must be checked in B1.

### Current performance-first disposition (2026-09-29)

The supplied kitchen dataset is sufficient for a **performance-only**
comparison; no new dataset, held-out PSNR floor, boundary-quality envelope,
full convergence or oversized-scene capacity failure is an entry gate.
The historical X4 and kitchen quality/three-hour stops and B1's 20% timing
threshold above do not decide which experiment runs next. The observed
independent-job near-2× aggregate throughput is not cooperative single-model
scaling. B1's later fixed-topology proof is eligible for further performance
benchmarking, **not** accepted B2 or a production result.

Continue with the bounded B1/O1/G1 comparison in the
[roadmap's benchmark selection and separate release gates](multi-gpu-training-plan.md#10-benchmark-selection-and-separate-release-gates):
freeze the kitchen work/exposure, pixels, active-state semantics and explicit
supported AMD device identities; compare repeated windows and a longer
fixed-work, overhead-inclusive run. Record measured versus derived versus
unverified claims and B1's approximate update semantics. Keep owned context
unchanged, updates coherent, provenance and outputs valid, and available disk
space checked before checkpoint-heavy runs. Quality remains an optional
diagnostic for this benchmark; production quality, recovery and capacity
acceptance remain separate.
