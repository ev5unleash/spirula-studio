# Documentation index

Start at [`../AGENTS.md`](../AGENTS.md) for orientation. This directory holds
the detail.

| document | what it covers |
|---|---|
| [architecture.md](architecture.md) | layers, data flow, engine state, where each responsibility lives |
| [build.md](build.md) | the build matrix, every option, per-platform notes |
| [backends.md](backends.md) | the CUDA/Vulkan seam, coverage, pointers to the authoritative backend docs |
| [codegen.md](codegen.md) | the four generators, what they own, and the invariants |
| [datasets.md](datasets.md) | supported dataset layouts and how they are parsed |
| [i18n.md](i18n.md) | shipping a localized build: `SS_DEFAULT_LANG`, `SS_FONT_CJK`, what is translated |
| [testing.md](testing.md) | native parity tests and the CUDA-vs-Vulkan reference-dump workflow |
| [notes/rename-and-i18n-plan.md](notes/rename-and-i18n-plan.md) | the Spirula Studio rename, 13-locale localization, and retiring the Python client |
| [notes/pose-normalization.md](notes/pose-normalization.md) | orientation/centering: what the native parser implements, and the kept reference for what it doesn't |
| [notes/compare-view.md](notes/compare-view.md) | showing several models at once: engine scene slots, the shared navigation frame |
| [notes/vram-splat-x-img.md](notes/vram-splat-x-img.md) | what the largest scratch category costs per element, the bitmask compaction, and the measured dead ends |
| [notes/multi-gpu-training-micro-01.md](notes/multi-gpu-training-micro-01.md) | performance benchmark protocol: native baseline, residency, visibility, transport and candidate comparison on the supplied dataset |
| [notes/multi-gpu-training-plan.md](notes/multi-gpu-training-plan.md) | performance-first comparison of blocks, CPU residency and synchronous ownership; selection and implementation handoff, separate from production release gates |
| [notes/multi-gpu-training-comparison-20260929.md](notes/multi-gpu-training-comparison-20260929.md) | separate historical/new-source B1 benchmarks, strict native G1 parity and resource evidence, with matched fixed-work timing |
| [notes/multi-gpu-training-g1-implementation-20260929.md](notes/multi-gpu-training-g1-implementation-20260929.md) | closed native FPBO proof and C0–C4 continuation: correctness passed, performance admission failed |
| [notes/multi-gpu-training-o1-screen-20260930.md](notes/multi-gpu-training-o1-screen-20260930.md) | completed native residency/dirty-state screen; O2 not admitted, O1 unverified with a precise bounded-discovery/update servicing blocker |
| [notes/color-transfer.md](notes/color-transfer.md) | linear storage vs the output tone curve, the dynamic range a curve buys, and why its clip is straight-through |
| [notes/sfm-in-process-plan.md](notes/sfm-in-process-plan.md) | running SfM inside the GUI: the library seam, the input manifest, and what manual ties need |
| [notes/](notes/) | design notes for individual subsystems |

Authoritative documents that live next to their code rather than here:

- `src/backend/README.md` — the backend seam design.
- `src/backend/vulkan/README.md` — the Vulkan
  backend in depth: device baseline, capability variants, memory model,
  atomics, Slang notes, kernel coverage. **The single most detailed document
  in the repo**; read it before touching Vulkan code.
- `src/app/README.md` — working notes on the
  standalone CLI and the native GUI.
- `src/i18n/README.md` — how a translation is a type, the `ui::` wrapper rule,
  and how to add a language. **Read before adding interface copy.**
- `assets/fonts/README.md` — the embedded UI font, why it is renamed, and the
  CJK faces.
- `viewer/README.md` — the standalone WebGL2/WASM viewer.
- `reference/scripts/README.md` — dataset preprocessing tools.
