# Deferred GPU and desktop validation: debugging handoff

Status: investigation stopped at the user's request. Multi-GPU validation and
further GPU/desktop debugging belong in a dedicated session. No GPU fix was
integrated. Diagnostic environment overrides below are experiments, not supported
workarounds or acceptance results.

The [backend support policy](../../AGENTS.md#backend-support-policy) excludes
NVIDIA implementation and validation, including Vulkan on NVIDIA. Do not restart
those lanes when resuming this work.

## Accepted evidence and remaining boundaries

- Windows GUI-enabled Vulkan headless suite: 19/19 passed, 7.63 s. Two concurrent
  invocations passed independently in 8.65 s and 7.54 s.
- GUI-OFF core suite: 14/14 passed, 6.65 s; GUI-enabled build was restored.
- Empty CTest selection and an intentional runner failure returned nonzero;
  failure artifacts were retained.
- Linux/macOS CI execution was not observed. The fork's Actions API returned no
  runs at the time of inspection; workflow configuration is not execution proof.
- AMD training/checkpoint/resume/stop acceptance remains incomplete. The
  zero-exit diagnostic variants below produced divergent training.
- Desktop first-frame rendering and normal close were observed. Submit/cancel
  through the actual desktop controls remains unverified. The earlier desktop
  render was not an AMD-specific acceptance check.

See [the implementation plan](headless-testing-plan.md) and
[testing instructions](../testing.md) for the implemented checks and their scope.

## Local evidence inventory

These artifacts are local, ignored build output, not committed fixtures. Preserve
or export them before deleting the build tree:

- `build_vulkan/Testing/Artifacts/cli_training_smoke-8bc3ce311392fa310aca/`:
  original failing smoke log and `training-fixture/dataset`.
- `build_vulkan/Testing/Artifacts/amd-validation-debugging-kk5b3xvl/`:
  copied diagnostic archive, 33 files / 3,776,464 bytes at handoff.
  - `stdout.log`, `stderr.log`: Vulkan validation/API-dump run.
  - `predump.log`: API dump with before/after arguments.
  - `bytes-to-float-int8.spvasm`: disassembly of the compiled native-byte shader.
  - `no-int8.log`, `portable.log`, `portable-no64.log`: diagnostic flag variants.
  - `outputs/`: checkpoints and run artifacts from the diagnostic variants.
- The original diagnostic scratch directory was
  `%TEMP%/spirula-amd-diagnostic-kk5b3xvl`; the build-tree copy above avoids relying
  on that temporary directory surviving.
- Earlier desktop scratch: `%TEMP%/spirula-desktop-smoke-il00ybzk`. Its original
  launch environment selected NVIDIA; do not reuse it unchanged.

The interrupted isolated byte-access lane was cancelled before integration or a
successful check report. There is no accepted patch or completed regression test
from that lane. Existing unrelated worktrees and user changes were left alone.

## AMD Vulkan: first failure precedes the apparent crash

Observed environment: Windows 11, AMD Radeon(TM) Graphics integrated in a Ryzen
7 9800X3D, AMD proprietary driver 26.8.1, Vulkan API 1.4.315, driver version
2.0.353. Device indices changed between runs: select by UUID, not index. These
observations concern one AMD device/driver, not all AMD hardware.

### Reproduction shape

The existing `cli_training_smoke` generated six 64 × 64 images and 64 seed points.
A reduced direct-training diagnostic used the same dataset and options below.
Set `SS_TEST_DEVICE` to the current AMD UUID selector and use a fresh, owned
output directory. Keep `SS_NO_AUTO_FETCH=1` and isolate application config/cache
as the CTest runner does.

```bat
set SS_VK_DEVICE=%SS_TEST_DEVICE%
set SS_VK_VALIDATION=1
set SS_NO_AUTO_FETCH=1
build_vulkan\spirula.exe train --data build_vulkan/Testing/Artifacts/cli_training_smoke-8bc3ce311392fa310aca/training-fixture/dataset --data-format nerfstudio --output-dir-prefix build_vulkan/Testing/Artifacts/amd-debug-runs --output-dir-name fresh-run --num-iterations 12 --steps-per-save 5 --save-full-checkpoint 1 --disable-viewer 1 --keep-viewer-alive 0 --sh-degree 0 --cap-max 256 --eval-mode all --refine-start-iter 100000 --device %SS_TEST_DEVICE%
```

For the recorded API dump, `VK_LAYER_LUNARG_api_dump` was enabled through
`VK_INSTANCE_LAYERS`, with `VK_APIDUMP_PRE_DUMP=true`. This requires the installed
Vulkan SDK layer. Do not leave API dumping enabled for ordinary timing runs.

### Observed ordering

1. A submission using `warp.bytes_to_float.int8.spv` converted a 64 × 64 × 3
   image: shader module 2,792 bytes, push constants 40 bytes, dispatch 48 × 1 × 1.
2. Timeline value 11 had been observed before that dispatch. Afterwards,
   `vkGetSemaphoreCounterValue` returned `VK_SUCCESS` with `UINT64_MAX`.
3. The next submission, signalling value 13, returned `VK_ERROR_DEVICE_LOST`.
4. Validation subsequently reported the timeline-value violation
   `VUID-VkSubmitInfo-pSignalSemaphores-03242` and pending-command-buffer reuse
   violations `00049` / `00071`.
5. Only later did `vkCreateComputePipelines` crash inside `amdvlk64.dll`, reading
   address `0x60`, with OS exit 3221225477. That later module was 30,276 bytes;
   seven earlier pipeline creations had succeeded.

The later pipeline-creation crash is not the first failure. The byte-conversion
submission is the first localized suspect; causality inside its shader or driver
has not been established. Validation errors after device loss may be secondary.

### Diagnostic variants: none is an accepted fix

| Overrides, in addition to the baseline | Observed result |
|---|---|
| None | Device loss followed by access violation before the first training step |
| `SS_VK_NATIVE_INT8=0` | Exit 0 after 12 steps, but divergence at step 1 |
| Above plus `SS_VK_NATIVE_ATOMICS=0` | Same nonfinite regularization warning and invalid training metrics |
| Above plus `SS_VK_NATIVE_INT64=0` | Same nonfinite regularization warning and invalid training metrics |

All three completing variants reported `ppisp_reg_exposure_mean = inf` at step 1.
The first variant finished with RGB loss approximately `3.079e4` and PSNR 0. A
successful process exit and a structurally readable checkpoint do not establish
numerically valid training.

The native-byte shader disassembly showed its `gindex` byte helper computing
`uint64(buf) + 4 * (idx >> 2)`, casting to a byte pointer, then adding `idx & 3`,
and loading with alignment 1. Simplifying this to direct byte addressing was an
unverified candidate, not a demonstrated fix. No blanket vendor/capability
exclusion was justified by this evidence.

### Separate numerical discrepancy

The completing no-native-int8 run's step-12 `state.tar` was inspected numerically:

- `eng.ppisp.params.npy`: all 54 floats finite; first 18 were zero.
- `eng.ppisp.accum.npy`: all 54 floats finite; first 18 were zero.
- `world.means.npy` and `world.features_dc.npy`: all 768 floats in each array
  finite; sampled active values remained near the initial scene.

Thus nonfinite PPISP parameters were not demonstrated in that saved checkpoint.
The first-step regularization computation/readback still needs investigation;
finite saved arrays do not explain away the warning or prove healthy updates.

### Bounded next investigation

1. Reproduce byte conversion alone through the production
   `uint8_image_to_float_raw` path, with known bytes including 0, 127, 128, 255
   and a length spanning workgroups. Compare every scaled float against the CPU
   expectation, using both default native bytes and `SS_VK_NATIVE_INT8=0` on
   the explicit AMD UUID, with validation enabled.
2. If that isolates a defect, change only the implicated helper/launcher and
   retain a numerical regression check. Do not begin with the later crash or
   globally disable device capabilities.
3. Investigate the PPISP regularization discrepancy separately, including its
   inputs, reduction, parameter ABI and readback. Disabled native atomics and
   native int64 did not remove it in the recorded experiments.
4. Only after finite training is established, rerun the actual checkpoint,
   scheduler-resume, cooperative-stop and forced-interruption smoke. Require
   numerical evidence as well as exit/result/checkpoint validity.

Relevant source: `src/backend/vulkan/shaders/int8_compat.slang`,
`src/backend/vulkan/shaders/warp.slang`,
`src/backend/vulkan/kernels/Warp.cpp`,
`src/app/tests/cli_training_smoke.cpp`. Vulkan-local tests are discovered from
`src/backend/vulkan/tests/*.cpp` by `cmake/SsBackendVulkan.cmake`. No new shader
appeared necessary for the proposed converter regression.

## Desktop: input delivery not proven

The generated dataset opened in the actual desktop application. The first frame
showed six camera frustums and 64 points; the native title-bar Close action exited
normally with code 0. UI Automation exposed title-bar controls but not the Dear
ImGui canvas widgets.

Background canvas clicks, foreground clicks, and a roughly 250 ms one-pixel drag
failed to activate the intended Home/navigation control. Hover appearance could
change. A desktop screenshot confirmed the intended coordinate location, but
there was no observed navigation or submit/cancel transition. Transport success
is not evidence that ImGui consumed the input.

`src/app/gui/GuiMain.cpp` enables `ImGuiConfigFlags_NavEnableKeyboard` and calls
`ImGui_ImplGlfw_InitForOpenGL(window, true)`. It polls/waits for GLFW events before
starting each ImGui frame. No broken callback, input-routing defect, or reliable
keyboard sequence was established before deferral.

In a dedicated session, first prove one ordinary widget activation by its visible
state change, using a fresh window snapshot and supported device selection.
Keyboard navigation is an available avenue, not yet a verified solution. Then
exercise a real submission and cancellation and observe their terminal states.
Do not repeat identical no-op clicks, infer success from hover, or add a
production automation hook merely to bypass the failing input route.
