---
name: spirula-validation
description: Targeted validation for Spirula native C++, CUDA, Vulkan/Slang, numerical, and performance changes.
---

# Spirula validation

Use this skill for Spirula C++, CUDA, or Slang changes, backend-seam and engine
work, viewer/training changes, or a numerical or performance investigation.
Keep the gate proportional to the changed surface; the repository docs remain
the source of truth.

## Baseline first

Read the relevant sections of [`docs/build.md`](../../../docs/build.md),
[`docs/testing.md`](../../../docs/testing.md), and
[`docs/codegen.md`](../../../docs/codegen.md), plus the README for the touched
subsystem. Record the exact command, source/build tree, backend, compiler and
Slang/CUDA toolchain, device/driver, scene/config, and relevant build options.
If the required hardware or toolchain is unavailable, report the result as
**unverified**. `clangd` or an index-only check is never a substitute for a
real build.

## Native tooling

`.clangd` selects `build_vulkan` by default. Generate a compilation database by
adding `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to the relevant dev build command.
For CUDA indexing, pass `--compile-commands-dir=build_cuda` to clangd.
Source-level debugging requires a build with `-DSS_DEBUG_SYMBOLS=ON`; do not
mistake a debugger attaching to a stripped Release binary for source coverage.

## Build and choose a gate

Use the dev entrypoints; they run the supported code generation and select the
backend build tree. On Linux, use the applicable command:

```bash
bash build_develop.bash -DSS_BACKEND=cuda
bash build_develop.bash -DSS_BACKEND=vulkan
```

On Windows use the batch entrypoint, for example:

```bat
build_develop.bat -DSS_BACKEND=vulkan
```

Add `-DSS_BUILD_GUI=OFF` for a no-window CLI gate. Add
`-DSS_BUILD_BACKEND_TESTS=ON` to the CUDA build when parity tests are needed;
Vulkan builds those tests unconditionally. Use the platform matrix in
`docs/build.md` rather than invoking a second build system or inventing a
custom build tree.

- Shared engine, core, kernel, backend, or Slang math changes are dual-backend
  changes: build CUDA and Vulkan, then run the smallest relevant parity or
  engine check. For dump/compare tests, produce the deterministic reference on
  CUDA and compare it on Vulkan, for example
  `<build-dir>/<test_name> dump ref.bin` followed by
  `<build-dir>/<test_name> compare ref.bin`. Keep reference dumps out of git;
  use a relevant `*_DUMP_GOT` output when a mismatch needs numerical detail.
- Engine or training behavior normally calls for the focused
  `engine_render_parity` and/or `engine_train_parity` gate, not an unrelated
  suite. Include a short training run per backend only when training behavior
  changed. A scripted run that serves the viewer must pass
  `--keep-viewer-alive 0` so it can exit.
- `src/sfm/`, `src/nn/`, `src/sam/`, `src/aliked/`, `src/loma/`,
  `src/metric3d/`, `src/moge/`, and `src/video/` are Vulkan + Slang-only
  inference/SfM subsystems. Use the Vulkan dev build and the focused subsystem
  executable or CLI; do not require CUDA engine parity for code isolated there.
- For config, parser, or UI changes, use the directly affected help, round-trip,
  CLI, GUI, or viewer check named in `docs/testing.md`. Exercise a changed GUI
  or browser surface itself; a headless browser run does not prove real-GPU
  rendering.

Generated declarations, instantiations, backend forwarders, and generated
shader output are not source owners. Edit their C++/CUDA/Slang inputs and run
the documented generator or dev entrypoint; never hand-edit generated sections.
For shared `src/shaders/` changes, follow the extra source-generation step in
`docs/codegen.md` because the dev scripts do not run it automatically.

## Correctness before performance

Only profile when performance is part of the question. First rerun the actual
workload with `SS_PROFILE=1`; its stage breakdown and per-kernel GPU timing are
available on both backends. For unstable training trajectories, use the fixed
workload tools documented in `docs/testing.md`:

```text
<build-dir>/raster_bench [num_splats] [iters] [macro_log2]
<build-dir>/fpbo_bench [num_splats] [iters]
```

Compare correctness/parity as well as timing. Hold the scene/config, model and
splat trajectory, device/driver, build options, warmup, iteration count, and
output state equivalent before attributing a speed or memory change. Use the
existing [VRAM note](../../../docs/notes/vram-splat-x-img.md) and [scheduling
profiling guidance](../../../docs/notes/device-job-scheduling-plan.md) when
those questions are in scope. Reuse existing measurements before introducing
another benchmark or profiling dependency.
