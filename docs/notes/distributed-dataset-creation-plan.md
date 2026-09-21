# Distributed dataset creation

Status: implementation present in the working tree; acceptance is not yet closed.
Sections 1–12 retain the design contracts and acceptance scope; their
implementation phases are not evidence that their gates have passed.
The current P1 provenance status and next dependency are tracked in
[section 14](#14-p1-provenance-closeout).

Prior-session results are recorded separately from repeatable acceptance evidence.
Remote connectivity, a representative reconstructable corpus, real cross-host
recovery, trainer handoff, and performance measurements remain open.

## 1. Goal and scope

Create one trainable dataset using feature-extraction work performed on several machines or GPUs, then one coordinator for matching, mapping, model assembly, and publication.

Implement **feature shards first**, then **overlapping scene chunks using the same artifact protocol**. A chunk is an image-membership and pairing hint, not an independently numbered reconstruction. Workers produce features only. Matching, geometric verification, camera grouping/calibration, mapping, merge validation, bundle adjustment, and final gauge fixing remain centralized.

This is deliberately not distributed bundle adjustment or distributed training. It addresses extraction throughput and the ability to prepare a large capture across available machines without creating a second reconstruction pipeline.

### Decisions

- Reuse native SfM, its SIFT/ALIKED/LoMa extractors, existing feature payloads, and existing mapper/assembly implementations.
- Start with a file-based workflow: export a work plan, run workers, transfer completed artifact directories, import them, reconstruct. Manual transfer, shared storage, or an operator's existing SSH/file-copy tooling are sufficient; the application needs no network service to work across machines.
- A worker is one process on one explicitly selected local device. Device identities are host-local execution bindings, not portable dataset identities.
- Exactly one coordinator owns a mutable reconstruction workspace. Workers never write into its feature directory, match database, resume journal, or sparse output.
- Prepare source frames, names, masks, and capture metadata before freezing the work plan. Do not independently resample the same source video on each worker.
- Support existing extractor families when their modules/models are available. Pin model content, not just model filenames. Use existing model acquisition/licensing policy; do not bundle weights into work packages.
- Default GPU policy remains Vulkan on non-NVIDIA hardware. For this acceptance campaign the user explicitly authorizes **Vulkan on the designated Windows NVIDIA testing nodes**, alongside the local AMD lane. This is not a general restoration of NVIDIA support. CUDA builds, execution, reference dumps, and CUDA-specific work remain out of scope. Keep `SS_ENABLE_PATENTED=OFF`.
- No runtime Python, database service, message broker, generic DAG engine, new mapper, or custom remote process launcher.

## 2. Implementation baseline and ownership

The table describes the starting constraints that motivated the implementation,
not a current list of missing features. The working tree now contains
[`FeatureWork.h`](../../src/sfm/core/FeatureWork.h),
[`FeatureWork.cpp`](../../src/sfm/core/FeatureWork.cpp), the planned CLI commands,
indexed import, and local GUI/scheduler orchestration. Source and runtime
evidence, rather than this ownership map, determine acceptance.

| Area | Existing owner | Consequence for this work |
|---|---|---|
| Extraction | [`Pipeline.cpp`](../../src/sfm/Pipeline.cpp), `extractDirectory`; [`Extractor.h`](../../src/sfm/feature/Extractor.h); [`ImageLoader.h`](../../src/sfm/core/ImageLoader.h) | One reusable per-image computation and bounded decode pool already exist. Directory extraction has no explicit fixed image-list input. Preserve largest-first execution and buffer reuse. |
| Output ownership | `extractDirectory`, `sweepStaleFeatures` | Extraction sweeps feature files not belonging to its current input set. Several workers pointed at one output directory can delete each other's output. |
| Names and indices | `extractDirectory`, `loadFeatureDir`, `imageStemMap` in `Pipeline.cpp` | Feature paths replace image extensions with `.bin`; sorted feature paths define image indices. `foo.jpg` and `foo.png` collide. Shard-local numbering is not a global namespace. |
| Feature payload | [`Features.h`](../../src/sfm/core/Features.h) | The current writer emits custom `VKFT` v6 files with keypoints, descriptors, colors, extraction dimensions, and selected EXIF fields. The payload has no source or extraction-recipe fingerprint. |
| Reader safety | `peekFeatures`, `readFeatures` | The lightweight probe is not a full validator. Version, allocation bounds, and complete version-specific tails must be checked before accepting transferred artifacts. |
| Cache/resume | [`Resume.cpp`](../../src/sfm/core/Resume.cpp); `featuresAreCurrent`, `featureDirDigest`, `run_auto`; [`SfmConfig.cpp`](../../src/sfm/SfmConfig.cpp) | Existing cache validity relies on settings and path/size/mtime information. It is not a cross-machine content-identity contract. |
| Capture configuration | [`Manifest.h`](../../src/sfm/core/Manifest.h), [`Manifest.cpp`](../../src/sfm/core/Manifest.cpp) | Camera prefixes, rigs, captures/telemetry, and color settings exist. This manifest is not a shard job/result format; reuse its resolved configuration rather than inventing another interpretation. |
| Central matching | `matchFeatureDir`; [`Matches.h`](../../src/sfm/core/Matches.h) | Custom `VKMT` matches refer to image-vector indices and feature-row indices. Camera setup travels with the matches. Generate these centrally after import. |
| Mapping/assembly | `runMapper`; [`Atoms.h`](../../src/sfm/map/Atoms.h), [`Bottomup.h`](../../src/sfm/map/Bottomup.h), [`Assemble.h`](../../src/sfm/map/Assemble.h) | Flat and bottom-up mapping already use shared assembly. Atoms demonstrate local-to-global image remapping while retaining global camera IDs. Bottom-up is local concurrency, not remote distribution. |
| Standalone merge | [`sfm_main.cpp`](../../src/app/cli/sfm_main.cpp), `merge`; [`Merge.h`](../../src/sfm/map/Merge.h) | Shared image IDs, names, and feature rows matter. The standalone command performs merging and optional BA, not the full assembly growth/audit/split/reseed schedule. Do not substitute it for `runMapper`'s assembly. |
| Feature compaction | [`FeatureCompaction.h`](../../src/sfm/core/FeatureCompaction.h); `run_auto` | Compaction remaps in-memory features and matches after writing the on-disk match database. Keep this central; independent compaction would invalidate shared keypoint indices. |
| Local jobs | [`JobScheduler.h`](../../src/app/JobScheduler.h), [`WorkerRequest.h`](../../src/app/WorkerRequest.h), [`OutputLease.h`](../../src/app/OutputLease.h), [`Subprocess.h`](../../src/app/Subprocess.h) | Process isolation, attempt identity, cancellation, output leases, and recovery already exist. The current phase list is linear and does not implement shard fan-out/fan-in. |
| Dataset handoff | [`DatasetPrep.h`](../../src/app/DatasetPrep.h), [`SfmRunner.cpp`](../../src/app/gui/SfmRunner.cpp), [`ColmapParser.cpp`](../../src/data/parsers/ColmapParser.cpp) | Preserve image/mask paths, COLMAP sparse output, optional geometry, partial/non-metric status, and normal trainer loading. |

Only the final sparse reconstruction is COLMAP interchange here; the native feature and match files are custom formats. File-based stages alone do not make arbitrary independently produced artifacts interchangeable.

## 3. Target execution flow

```text
Existing dataset preparation
  -> frozen image/mask inventory + resolved capture configuration
  -> immutable extraction plan
       -> worker A -> isolated completed feature artifacts
       -> worker B -> isolated completed feature artifacts
       -> worker C -> isolated completed feature artifacts
  -> coordinator validates/imports the complete feature collection
  -> central pairing + matching + geometric verification
  -> central mapper: incremental or existing bottom-up schedule
  -> existing assembly: merge/grow/refine/audit/split/reseed as configured
  -> final refinement + orientation/metric gauge + normal sparse output
  -> existing dataset validation, optional geometry, trainer handoff
```

The import boundary is a barrier: central matching begins only after a complete, validated feature collection has been sealed. Results may be transferred/imported incrementally, but a partially imported collection is never exposed as a complete dataset.

Use the same orchestration on one machine and across machines. A separate local output directory per worker is required in both cases. A shared filesystem is a transport, not a shared mutable SfM database or a distributed lock service.

### Overlapping chunks

- Every image has exactly one extraction owner, even if it belongs to several chunks.
- Chunk overlap references the same canonical image and feature artifact. It does not cause independent feature extraction or different keypoint ordering for the same image.
- Workers extract their owned images; the coordinator has the union. A worker does not need every halo image locally merely because its chunk names that image.
- Initial chunk definitions support explicit membership lists and ordered capture windows with configurable overlap. Respect capture boundaries and keep rig-frame membership together when constructing windows.
- Do not call lexical adjacency of unrelated photographs a scene partition. For unordered captures, require explicit grouping or use ordinary feature shards; the existing view-graph partitioner becomes available only after central matching.
- Chunk membership can add central pairing candidates, but must not suppress global retrieval, sequential boundary pairs, or loop closures. Deduplicate unordered pairs before matching.
- The first chunk implementation keeps the existing global pairing policy and adds bounded chunk-neighborhood candidates. Replacing that policy with restricted per-chunk matching is a separate measured optimization, not a prerequisite.
- Mapping remains global. The existing bottom-up mapper may form overlapping atoms from the verified graph; input chunks are not automatically valid mapper atoms.

This avoids needing a new distributed submodel format. Merely sharing three image names is not a guarantee of a mergeable overlap: usable correspondences, scene geometry, and the existing alignment/seam gates still decide.

## 4. Portable work and artifact contracts

The native `src/sfm/core/FeatureWork.h/.cpp` module owns the work-plan/index/result records, their validation, and deterministic selection/import rules. It uses the existing JSON implementation and [`core/Sha256.h`](../../src/core/Sha256.h). Do not add a protocol framework, duplicate binary feature reader, or second capture-manifest parser.

### 4.1 Identities

Keep these concepts distinct:

| Identity | Definition and use |
|---|---|
| Logical image key | Normalized dataset-relative image name **including its extension**. Preserve folder/capture identity. Two files containing identical bytes may still represent different observations. |
| Dataset snapshot | Digest of the ordered image inventory and source/mask content bindings. Moving the dataset to a different root does not change it. |
| Extraction recipe | Canonical, fully resolved output-affecting settings, implementation compatibility identifier, and model/shader content digests. |
| Image artifact key | Digest of logical image name, source digest, resolved mask digest or explicit absence, and extraction recipe. |
| Plan identity | Digest of schema version, dataset snapshot, recipe, and frozen work assignments/chunk membership. |
| Global image index | Dense index explicitly frozen in the plan, in normalized feature-path byte order; this is the index used by feature arrays, match endpoints, camera arrays, and rigs. |
| Attempt identity | Identifies one execution/result, independently of dataset or image identity. Used for retries and stale-result rejection. |
| Accepted collection digest | Digest of the ordered accepted image/artifact hashes and counts. This binds central matching and resume to the actual feature rows. |

Do not use worker completion order, absolute filesystem roots, timestamps, GPU ordinals, or a shard-local sorted list as global identity.

Retain the existing stem-based feature file layout initially. The work index explicitly binds a full source name to its existing SfM stem and feature path. Reject ambiguous stem mappings, case collisions, and other unrepresentable path mappings before dispatch rather than silently rename or overwrite images. This fixes the distributed trust boundary without changing the whole repository's feature filename convention.

Use the same collision check in the common extraction-selection path. A future collision-preserving filename scheme would require an explicit migration of every reader; it is not needed to distribute currently unambiguous datasets.

### 4.2 Records

Use versioned JSON records around existing `VKFT` payloads:

- **Plan:** schema/kind, plan and dataset digests, resolved extraction recipe, source-relative image/mask bindings and digests, global order, owner shard, optional chunk memberships, and central capture/configuration references with digests.
- **Extraction request:** coordinator-issued immutable plan/shard/attempt identity, request digest, and optional superseded request digest. It authorizes one assignment without embedding host-local roots or device identities.
- **Worker binding:** extraction-request path, local image/mask/model roots, local device request, execution limits, and output/attempt directory. Absolute paths exist only in this host-local binding.
- **Image receipt:** producing request identity, image artifact key/path/hash/byte count, feature count, descriptor type/dimension, source/extraction dimensions, and producer build/device diagnostics. Published atomically after its complete feature payload.
- **Shard result:** authorized request identity, one outcome and receipt reference for every assigned image, and overall outcome. Adopted receipts retain their original producer identity.
- **Accepted collection index:** exact accepted feature rows in global order, provenance/result identities, collection digest, and completeness. Its sealed publication is the only signal that central stages may consume the collection.

A separate work plan is justified because the existing capture manifest and local `WorkerRequest` describe different things. Do not make either one a loosely typed catch-all. Reuse their existing config/path codecs where applicable.

The coordinator issues initial requests with the plan. A manual retry/export operation creates a new immutable request explicitly superseding the previous one. The coordinator's own records must identify one unambiguous active request per shard; reject branching supersession or stale/cancelled requests, and never accept worker-supplied records as authority to replace a request. A cancellation record prevents acceptance without claiming to stop the remote process. Local jobs use the existing scheduler's current immutable request/attempt as that authority; project its identity into the portable extraction request rather than maintaining a competing current-attempt pointer.

Resolve presets, manifest precedence, per-group overrides, EXR color-space adoption, and frontend defaults **before** computing the recipe. Reuse `SfmConfig`'s option table/finalization, not a hand-maintained duplicate of every setting.

### 4.3 Provenance and compatibility

Include all output-affecting extraction state: frontend and descriptor variant, effective feature limits/thresholds, resize policy, color gamut/linear interpretation, EXIF-orientation behavior, selected mask and polarity, and resolved model content.

Pin the feature implementation compatibility identifier and actual custom shader bytes when `--spv-path` is used. Current stage signatures exclude that path, so they cannot be reused as the entire new provenance contract. Reject an unidentifiable implementation rather than treating an arbitrary dirty build as interchangeable.

Record host/build/device details for diagnosis, but do not demand identical executable hashes across Windows/Linux or identical GPU UUIDs across workers. Supported builds may share a tested extraction compatibility identifier. Unknown schema/implementation combinations fail before work or import; no best-effort acceptance.

CPU thread counts, decode budgets, local roots, progress settings, and device choice are execution settings, not extraction-recipe fields. Do not promise bitwise equality of independently recomputed GPU features; once accepted, the actual artifact hash is authoritative.

Camera grouping, rig calibration, central matcher model/options, telemetry, metric references, and final gauge configuration also need frozen/content-addressed stage inputs. They do not all belong in the per-image extraction key. Keep their invalidation attached to the stage that consumes them.

### 4.4 Input and payload validation

- Validate all requested images and plan membership; exactly one owner per image, valid chunk members, bounded indices, no duplicate rows.
- Normalize relative separators and preserve exact UTF-8 name spelling. Define one bytewise order, not locale-dependent or platform filesystem ordering.
- Reject absolute artifact paths, drive/UNC paths, `..`, embedded NULs, and symlink/reparse-point escapes from an output/import root. Detect filesystem collisions at planning/binding time; do not introduce a homemade Unicode normalization library.
- A worker must verify source/mask content against the snapshot before accepting cached work or producing a successful result. Require immutable source files during the attempt and detect mutation during reading. Staging files on another host must not change identity.
- Resolve masks centrally to explicit per-image bindings, including intentional absence. An expected missing/unreadable mask is an item failure, not an instruction to extract unmasked features. Do not rediscover masks against a shard-specific directory layout.
- Harden the common `readFeatures` path: supported version/endian/scalar contract, overflow-safe size calculations, allocation limits derived from file sizes and configured limits, valid descriptor types/dimensions, complete declared optional sections, and semantic value checks required by consumers. Retain valid older formats where supported; reject truncation masquerading as an older file.
- The current binary representation assumes little-endian hosts and expected scalar widths. State and check that supported platform contract rather than claiming universal binary portability.
- Match descriptor family/type/dimension and feature implementation to the resolved central matcher. Equal dimensions alone do not establish learned-model compatibility.
- Distinguish a valid zero-feature image from a decode/extraction failure. Both must have explicit outcomes; only the former can be part of a complete feature collection.
- Verify payload bytes/hashes and expected image coverage before sealing a result or collection. A valid JSON file or successful process exit alone is insufficient.

Checksums protect against corruption and accidental mismatch, not dishonest workers. The initial trust model is operator-controlled machines and authenticated external transfer. No listening service, credential store, or arbitrary command execution is added.

## 5. Publication, retries, and central resume

### Worker publication

Write only into a worker-owned attempt directory. Persist its immutable request before work. Use unique temporary siblings, close/check files, and publish each whole payload followed by its atomic image receipt; publish the final shard result last. A `.part` file or a payload without a valid receipt is not reusable completed work.

Every worker entry path, including direct CLI invocation, must hold the existing `OutputLease` on its attempt directory for the full attempt lifetime. Reuse the scheduler's existing lease handoff where applicable, not a second lock implementation. A concurrent invocation targeting the same attempt output must fail before writing. Keep active attempt outputs on worker-local storage; transfer completed artifacts to shared storage rather than assuming a local file lock coordinates remote hosts.

Reuse the existing feature writer rather than introducing a parallel implementation. Address its fixed `.part` ownership and Windows replace-existing behavior where the new path requires replacement. Prefer immutable destinations and validated reuse over overwriting completed artifacts. Document/test filesystem guarantees; rename alone is not a claim of power-loss durability.

An interrupted attempt may retain validated per-image work. A fresh authorized retry can adopt prior receipts only after checking the image/recipe keys, payload hashes, and original request provenance; retain the producer identity rather than relabeling old output as newly computed. Its final result accounts for both adopted and newly produced images. A crash between payload and receipt publication may redo that one image, not every successful image. Reassignment creates a new authorized attempt. There is no automatic remote retry/migration service in this milestone.

### Import

Acquire the coordinator's existing output lease, then validate incoming results in coordinator-owned staging. Copy into owned storage by default; do not retain mutable references or hard links to operator-owned incoming files.

Identical repeated delivery is idempotent. Wrong plan/shard/owner, unexpected images, missing assigned images, invalid payloads, stale attempts, or a conflicting artifact for an already accepted image are explicit errors. Never choose between conflicting payloads by last writer or directory enumeration order.

Imported payloads remain byte-for-byte unchanged. The importer orders and validates references; it does not rescale features, reorder keypoints, remask images, or run compaction.

An explicit coordinator-side adoption operation may reuse already owned payloads and receipts in a new plan when logical image names, image artifact keys, payload hashes, and source bindings still validate. Build a new accepted index with the new plan's order/ownership and retained producer provenance; never treat an old shard result as an authorized result for the new plan. This lets resharding/rechunking or unchanged images survive a plan revision without re-extraction. Reuse local immutable files where ownership permits, otherwise stage validated copies; no shared cache service is needed.

Publish the complete collection index at `<workspace>/features/index.json` only after every required image has either an authorized validated result or an explicit coordinator adoption record. Unexpected extra files do not become dataset members. Do not feed `loadFeatureDir` an arbitrary recursive scan of a mixed incoming/results directory.

### State and failures

The persisted plan, coordinator-issued request/supersession/cancellation records, validated image receipts/results, and sealed collection are the durable data facts. The local scheduler remains authoritative for local process attempts; artifact readiness is derived from those files, not a second job-recovery database.

```text
plan -> extracting / awaiting transfers -> complete collection
     -> matching -> mapping / assembling -> published dataset
```

- Stopping a worker leaves no false completion marker; completed validated image artifacts remain reusable.
- Stopping central reconstruction retains the accepted collection and valid match-resume work.
- An absent worker result means "awaiting results", not "reconstruction partial" and not inferred remote process death.
- An operator may explicitly create a new plan excluding failed inputs. Do not silently redefine the dataset under the old identity.
- Input completeness and geometric partial coverage are different: a complete extraction collection can legitimately produce partial or disconnected reconstructions.
- Late results from a cancelled/replaced attempt cannot alter a sealed collection.

### Central integration and invalidation

Extend the existing `AutoInputs`/argument path with an explicit imported-feature input. Normal `auto` still extracts locally. `auto --feature-plan` must require the sealed `<workspace>/features/index.json` produced by `collect` and verify that it matches the supplied plan. Staged `match`/`map` with that flag require the same index under their selected feature directory. A missing, incomplete, malformed, or mismatched index is an error; plan presence alone is not evidence of import completion and must not trigger skip-extraction or a directory-scan fallback.

Imported mode skips extraction without entering `run_auto`'s current invalidation branch that removes `features/`. Populate common image/feature/coverage totals from the sealed index before the minimum-image gate, automatic pair-mode selection, summaries, and result calculation. Current code obtains these from `ExtractStats`; leaving that object empty would fail a valid import. Report imported/reused work honestly, and emit extraction-specific mask diagnostics only when supported by worker receipts, not fabricated coordinator extraction statistics.

Both modes converge into the same matching, mapper, assembly, and finalization implementation. Central overrides that would change extraction must be rejected or require a new feature plan, not silently trigger local extraction. Matching/mapping settings may change with correctly invalidated downstream state.

| Changed input | Required invalidation |
|---|---|
| Image/mask bytes, mask binding/polarity, effective extraction settings, feature implementation/model/shader | Affected feature work and accepted collection; all dependent matching/mapping |
| Actual accepted feature bytes/order/counts | Pair list, match journal/database, mapping/resume tied to old rows |
| Pairing/chunk candidate policy, matcher/model, verification or camera setup | Matching and downstream geometry, not extraction |
| Mapper/assembly/compaction/rig settings | Dependent mapping/model resume; preserve compatible features/matches |
| Final orientation/metric/telemetry inputs | Relevant finalization or mapping stages according to actual dependency; never reuse an incompatible finished model |
| Device, root relocation, worker completion order, thread/decode budget | No semantic invalidation by themselves |

Use the accepted collection digest in match-resume signatures. Before attaching a cached `MatchesDatabase`, compare its image names/counts/order and validate all image/keypoint/camera indices against the collection. Do not trust equal image counts or copy pair arrays into a newly loaded database without those checks.

Publish `matches.bin` and its completion/signature records in an order that cannot advertise a partial database as complete. Keep valid match-journal recovery and reject/recompute corrupt records rather than counting them as completed work.

## 6. Central reconstruction and final dataset contract

The coordinator:

1. Loads the sealed feature collection in its frozen global order.
2. Resolves global camera groups/calibration and rig membership from the full capture, not worker-local EXIF clusters or local camera IDs.
3. Runs the existing pair selector, matcher, and geometric verification, retaining cross-shard and loop-closure candidates.
4. Writes the match database against original on-disk feature rows.
5. Releases descriptors when no longer needed and performs the existing in-memory compaction/remapping once.
6. Calls the existing incremental or bottom-up mapping path and its shared assembly.
7. Runs existing final refinement, image-name restoration, gauge fixing, recoloring, camera-size splitting, and model output.

Keep original images and relevant capture/telemetry/metric inputs available centrally. Before consuming imported features, validate the coordinator's bound image/mask inventory and bytes against the snapshot, and validate the central capture/configuration/telemetry/metric-reference digests against the recorded inputs. Missing, extra, changed, or ambiguous bindings are errors even if the feature collection itself is valid.

Use the accepted source-name mapping for imported-mode name restoration rather than re-inferring names from an unrestricted root scan. Keep bound inputs immutable through finalization; when this cannot be enforced by owned immutable staging, revalidate before publication. This protects EXIF/GPS reads, metric gauge fitting, and the trainer's eventual images from silently referring to different bytes. Feature payloads do not contain all sensor information, and a feature-only workspace is not a portable training dataset.

No unconditional `sfm merge` after `auto` is needed: assembly already happens inside the mapper path. Preserve normal merge refusals and disconnected components; never force a Sim(3) fit or concatenate point clouds just to produce one directory.

Audit name/index-dependent consumers together: `loadFeatureDir`, standalone `map`'s direct feature reads, match-resume loading, `resolveImageNames`, unregistered-image reporting, camera/rig setup, `SfmProgress`'s feature/keypoint readers, and model resume/audit. All must agree with the accepted collection's name-to-row mapping.

Validate final output through the existing dataset parser. Preserve `images/` or explicit image-root binding, masks/polarity, `sparse/0` and other valid components, rigs/gauge sidecars, optional geometry, and existing partial/non-metric result fields. Distinguish a self-contained dataset from an in-place dataset whose image paths remain external.

Write final models into private staging and validate before publication under the workspace lease. For the first implementation, publish into a fresh destination; reuse an already completed identical result, and require a new destination for a changed completed run. Do not assume portable atomic replacement of an existing nonempty directory. A future in-place replacement must use the existing explicit redo workflow with recovery, not remove the old valid dataset first.

A missing/disconnected reconstruction retains existing failure/partial semantics. Feature-import success alone must never trigger the trainer or mark the dataset complete.

## 7. CLI contract

The current executable exposes these commands. Request authority and retry
requirements are in section 5; these examples are not acceptance evidence.

```text
# Coordinator: snapshot prepared inputs and export work assignments.
spirula sfm plan dataset/images -o work/distribution --shards 4 --quality high

# Each worker: bind its issued request to local input/device/output paths.
spirula sfm extract local-images --feature-request work/distribution/request-shard-0000-attempt-0001.json \
    -o results/shard-0000-attempt-0001 --device <local-device-selector>

# Transfer completed result directories by an existing authenticated mechanism.

# Coordinator: validate/import deliveries and seal only a complete collection.
spirula sfm collect work/distribution/plan.json results/shard-0000-attempt-0001 \
    results/shard-0001-attempt-0001 results/shard-0002-attempt-0001 \
    results/shard-0003-attempt-0001 -o work/reconstruction --requests work/distribution

# Coordinator: reuse the normal pipeline from matching onwards.
spirula sfm auto dataset/images -o work/reconstruction \
    --feature-plan work/distribution/plan.json --device <coordinator-device-selector>
```

Provide explicit local bindings for masks/models where necessary. A plan stores their identity and relative mapping, not another machine's absolute paths.

For chunks, planning accepts either an explicit membership file or ordered-window size/overlap options. Report planned image ownership, repeated memberships, unique input count, and estimated work per shard. Overlap is a count/configuration parameter, not a promised geometric merge success rate.

`plan` and `collect` must not initialize a GPU. Worker/central GPU resolution happens only at the actual compute boundary. `collect` may report received/missing/invalid image counts without claiming completion. Machine-readable results remain separate from localized terminal copy.

The normal staged `match`/`map` commands must also consume a sealed indexed feature collection correctly, so failures remain bisectable without a new monolithic command.

## 8. Local scheduler and GUI integration

Deliver the CLI/file workflow before adding UI orchestration. It is already a complete cross-machine workflow, not a placeholder for a remote service.

Then reuse the existing application surfaces:

- Add a narrowly defined `sfm-extract` worker phase with a feature-artifact result validator. Do not send extraction through the existing `sfm` phase and pretend a feature directory satisfies its sparse-model validator.
- A local shard is an independent scheduler job with its own attempt/output lease and device binding. The existing central `sfm` job becomes eligible only after collection sealing.
- Keep the scheduler's fixed workflows. The distributed dataset controller needs one bounded fan-out/fan-in barrier, not arbitrary inter-job DAG dependencies or a second queue.
- Use existing `WorkerRequest`, `Subprocess`, device selection, cancellation, and output ownership. Translate a portable plan into host-local worker requests; never ship the existing absolute-path request unchanged to another host.
- Do not claim the coordinator can stop an independently launched remote worker. It can cancel local supervised processes and stop accepting obsolete remote results; the operator stops remote processes in the initial workflow.
- The coordinator workspace lease must not cover worker outputs on another host or be held as a GPU reservation while waiting for transfers. Keep output claims scoped to the actual writer.
- Integrate with `SfmRunner`, `GuiApp`, existing dataset/batch controls, and preset field tables rather than a separate distributed-dataset editor.
- Display plan/shard status, validated images versus expected images, awaiting-transfer state, failures, selected local device, import readiness, and central-stage progress. Count unique images, not overlapping memberships.
- Reuse `RunContext` events and `.progress` snapshots for running central stages. Progress snapshots remain presentation data, never completion receipts.
- Persist execution/recovery in the existing scheduler and portable artifact records. On restart, reconcile them and never infer success from a stale percentage or leftover sparse directory.
- Preserve partial/non-metric warnings and existing trainer handoff. Optional geometry runs only after validated reconstruction.
- Localize new CLI/GUI copy in the existing catalogs; update all languages and regenerate font subsets when required. Persist only reusable settings in `DatasetPreset`, never source paths, remote machine paths, or capture-specific selections.

## 9. Implementation phases and acceptance contracts

These phases describe the delivered design and the gates still to prove. Do not
reimplement them as part of test execution; fix only defects exposed by the gates.

### Phase A — baseline and finalized contracts

Measure representative ordinary runs using current stage timings, extraction profiling, and `SS_SFM_MAP_PROF`. Include at least an ordered capture and unordered photographs; include masks and a rig/wide-angle example in the correctness corpus.

Record extraction, matching/verification, mapping/assembly, I/O, peak host memory/VRAM, artifact bytes, coverage, components, and reconstruction quality. Fix the dataset/config/build/toolchain/device details for later comparison. This is evidence gathering, not a new profiler subsystem.

Finalize the work schema, exact path/order rules, recipe serialization, supported implementation IDs, partial/error semantics, and proposed CLI parsing before parallel implementation.

**Gate:** a worked example maps every source image to one owner, one existing-format feature path, one immutable artifact key, and one global row; every downstream consumer's namespace is accounted for.

### Phase B — portable plan and strict artifact boundary

Owners: new `FeatureWork.h/.cpp`; existing `Manifest`, `SfmConfig`, `Features`, SHA-256/JSON utilities; CLI planning dispatch.

Implement inventory/snapshot creation, mask bindings, canonical work assignment, explicit model provenance, path/collision checks, strict feature reads, and versioned plan/results. Use deterministic cost balancing based on probed/resized image sizes rather than only equal image counts; allow an explicit shard count without introducing a scheduler policy engine.

Use one compact host regression executable for the genuinely new artifact-contract failure modes; extend existing feature/resume tests where their ownership fits.

**Gate:** relocating an identical snapshot preserves identity/order; changed content with preserved mtime is detected; malformed, truncated, conflicting, or path-escaping artifacts are rejected before allocation/import side effects.

### Phase C — subset extraction through the existing implementation

Owners: `Pipeline.h/.cpp`, `Extractor`, `ImageLoader`, `sfm_main.cpp`.

Factor directory discovery from the existing extraction loop. Both ordinary directory extraction and planned subsets call the same image-list primitive, preserving decode budgeting, largest-first ordering, color/mask/EXIF behavior, frontend selection, cancellation, and feature serialization.

Scope stale-file cleanup to the owning attempt; no worker may sweep another shard or the coordinator. Implement atomic per-image receipts, validated adoption by a fresh authorized attempt, and final shard results only when the complete assignment is accounted for.

**Gate:** real extraction on disjoint subsets covers each assigned image exactly once; repeated execution safely reuses valid work; interrupted/failed/masked images cannot appear as successful unmasked output; current standalone extraction still works.

### Phase D — import and centralized reconstruction

Owners: `FeatureWork`, `Pipeline`, `Resume`, `Matches`, standalone CLI readers, name/progress consumers.

Implement staged import, idempotence/conflict handling, collection sealing, and explicit imported input to `run_auto`. Bind pair/match/model resume to accepted content and ordering. Preserve the common camera/rig, mapper/assembly, and publication path; add no alternate mapper.

Do not expose `collect` success until the collection is complete. Validate final dataset loading and the existing partial/non-metric contract.

**Gate / first complete release:** two physical worker hosts in the authorized Vulkan lanes extract one planned dataset; a coordinator imports their transferred artifacts and produces a dataset the existing trainer can load. The coordinator may also be one worker, provided it sequences central compute after extraction. Delivery order and shard count preserve the dataset snapshot, logical/global image mapping, and per-image artifact keys. With identical payload bytes they also preserve accepted collection order/digest; plan, shard, request, and result identities may change. An interruption resumes without recomputing unaffected completed work.

### Phase E — overlapping scene chunks

Owners: work-plan chunk records; existing `Pairing`/`PairSelection` integration; central mapping configuration.

Add explicit membership and ordered-window planners, single-owner/halo semantics, chunk diagnostics, bounded extra pair candidates, and pair deduplication. Preserve global candidate discovery and verify cross-boundary/loop-closure behavior. Use existing bottom-up mapping when selected; do not dispatch remote mappers.

**Gate:** adjacent chunks and a non-adjacent revisit remain connected when the original central pipeline can connect them; shared images have one artifact/row; disconnected captures remain honestly separate; increasing overlap never multiplies extraction ownership.

### Phase F — scheduler/UI integration and acceptance

Owners: `WorkerRequest`, `worker_main`, `JobScheduler`, `SfmRunner`, `SfmProgress`, `GuiApp`, dataset/batch presets, i18n catalogs, relevant CMake test registration.

Implement the local extraction phase and UI barrier using the existing queue and leases. Exercise cancellation/restart while workers, import, and central reconstruction occupy different states. Verify the actual desktop surface, not only serialized job records.

After successful runtime acceptance, update existing SfM/application/build/testing documentation with the command contract, portability requirements, transfer procedure, recovery behavior, final dataset layout, and measured limitations. Remove any throwaway acceptance artifacts; do not commit datasets, model weights, private paths, or benchmark captures.

**Gate:** both manual cross-machine CLI and managed local multi-device flows work, restart/cancellation cannot publish false success, and the normal single-machine workflow remains intact.

### Implementation ownership

Keep one integration owner for `Pipeline.cpp` and `sfm_main.cpp`. After the schema/API contract is frozen, artifact validation, extraction integration, and application protocol/UI work can progress independently in their owned files. Serialize their shared-file integration; do not let multiple branches each grow a different extraction loop or job-state interpretation. Run integrated validation after those changes land together.

## 10. Verification matrix

Register new host-only artifact checks through [`cmake/SsTests.cmake`](../../cmake/SsTests.cmake) and its existing isolated runner. [`cmake/SsSfm.cmake`](../../cmake/SsSfm.cmake) creates SfM test executables, but the current headless label does not automatically run all of them. Do not mistake an unregistered test or empty CTest selection for a pass.

| ID | Scenario | Observable acceptance |
|---|---|---|
| V01 | Root relocation and randomized delivery order | Identical snapshot/accepted collection order and exact accepted payload bytes; no image or camera identity drift. |
| V02 | Several shard counts over the same frozen payloads | The same global feature collection reaches central matching; all cross-shard candidates permitted by the selected policy remain available. |
| V03 | Content/model/mask change with unchanged path or mtime | Stale work is rejected; changed extraction cannot reuse old match/model indices. |
| V04 | Replanned shard/chunk assignments with unchanged image keys | Explicit adoption reuses validated coordinator-owned payloads with original provenance; old results do not gain new request authority. |
| V05 | Changed coordinator images/masks or telemetry after remote extraction | Reject incompatible central bindings or mutation before publishing a dataset; features alone cannot authorize different training inputs. |
| V06 | Valid plan but absent/incomplete collection index | Imported `auto`/`match`/`map` refuse; no scan of partial files, local re-extraction, or empty-statistics reconstruction. |
| V07 | Same stem, case-sensitive names copied to a case-insensitive host, Unicode paths | Supported names round-trip; ambiguous/unrepresentable names fail before work or overwrite. |
| V08 | Bad result identity, unknown version, overflow, truncated optional section, escaping/symlinked artifact path | Explicit rejection; no allocation explosion, out-of-root write, or partial accepted collection. |
| V09 | Zero features, missing image, corrupt expected mask, wrong descriptor family | Correctly distinguish valid empty results from failures; no silent image loss or unmasking. |
| V10 | Kill worker during payload/result publication | No false complete result; validated completed images can be reused. |
| V11 | Duplicate transfer, conflicting repeated image, old attempt delivered late | Identical delivery is harmless; conflicts/stale deliveries cannot mutate accepted features. |
| V12 | Kill import or central match/publication | No complete marker for partial output; previous valid artifacts remain usable; restart recomputes only invalid/incomplete work. |
| V13 | Cached matches reordered or attached to same-sized different feature collection | Reject before graph/mapping use; keypoint and image endpoints retain their intended observations. |
| V14 | Global camera/rig/EXIF/color handling | Same grouping and units as the baseline; full relative names resolve to the correct sources and masks. |
| V15 | Ordered chunks with boundary overlap and non-adjacent loop closure | Bridge evidence reaches verification/mapping; duplicate memberships do not duplicate image IDs or extraction. |
| V16 | Insufficient or contradictory overlap | Existing merge gates refuse unsafe joins; multiple models/partial status are retained without forced success. |
| V17 | Geometry equivalence | With frozen features and matching policy, coverage, component structure, reprojection, and aligned poses stay within the predeclared baseline repeatability envelope. Do not demand byte-identical BA output across devices. |
| V18 | Real cross-machine pipeline | At least two physical worker hosts in the authorized Vulkan lanes, actual file transfer, central reconstruction, parser load, and a short real training run on the resulting dataset. |
| V19 | Scheduler/GUI | Correct unique-image progress, no conflicting writers/device ownership, cancellation/recovery, localized errors, and trainer handoff on the real desktop. |

Reuse `sfm_resume_test`, `sfm_manifest_test`, `sfm_feature_compaction_test`, `sfm_merge_test`, `sfm_map_test`, and `sfm_rig_test` for their actual contracts; add cases only where the changed behavior needs protection. Reuse `worker_request_test`, `scheduler_test`, `scheduler_result_test`, `command_argv_test`, `preset_roundtrip_test`, and `dataset_parser_test` when their surfaces change.

Build through the dev scripts:

```text
Windows: build_develop.bat -DSS_BACKEND=vulkan
Linux:   bash build_develop.bash -DSS_BACKEND=vulkan

cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error
```

Also build the headless configuration when changing CLI/core boundaries. Run selected SfM GPU binaries and the real distributed CLI scenario explicitly on the authorized Vulkan devices; they are not implied by the headless result. Test SIFT without learned modules and available learned frontends with approved weights. Preserve CPU BA fallback rather than requiring unsupported GPU arithmetic. Use `--keep-viewer-alive 0` for scripted training. Record unavailable hardware/model lanes as unverified; the scoped Windows NVIDIA authorization in section 1 does not waive any non-NVIDIA regression gate.

## 11. Performance expectations and rollout criteria

Measure end-to-end latency, not just the sum of extraction kernel timings:

```text
T_distributed = planning/staging
              + slowest worker extraction
              + unhidden transfer/import cost
              + central matching/verification
              + central mapping/assembly/finalization
```

If extraction is fraction `f` of a local run, an ideal `p`-worker speedup is bounded by `1 / ((1 - f) + f/p)` before transfer/validation costs. This is a planning bound, not a benchmark result. Chunk hints do not change that bound unless they also measurably change central work.

Record source/feature bytes moved, hashing/import time, worker utilization/stragglers, central RAM/VRAM, matching pair counts, mapping time, and final quality over repeated runs. Compare both cold staging and already-local/shared input cases. Separate wall-time improvement from aggregate compute cost.

The coordinator still loads a large feature collection and owns expensive matching/BA. Distribution does not remove its memory limit or make matching linear. Preserve current descriptor release/compaction and identify the measured next bottleneck; do not add out-of-core matching, distributed BA, or a global cache speculatively.

Correctness gates are unconditional. Before claiming a speed benefit, the complete distributed run must improve measured end-to-end latency on the named extraction-heavy workload after overhead, without exceeding the agreed quality envelope. If it does not, report that result; the workflow may still be useful for remote capacity, but it is not an established acceleration.

## 12. Explicitly deferred work

- Remote worker daemon, automatic discovery, credentials, heartbeats, cloud/object-store integration, automatic scheduling/reassignment, and a general dependency graph.
- Distributed pair matching, bundle adjustment, mapper state, or training/optimizer synchronization.
- Independently reconstructed remote submodels. That extension would require global image/camera identity, exact canonical feature provenance and point2D row mappings, compaction reconciliation, track remapping, rig/Sim(3) compatibility, and entry into the central `Assemble` path with the global correspondence graph. Concatenating COLMAP directories or calling standalone `merge` is not sufficient.
- Automatic spatial partitioning of unordered photographs without a verified graph or explicit spatial evidence.
- New raw-video frame extraction/masking distribution, learned-model redistribution, feature payload compression, shared content-addressed caches, and big-endian format support.

None is required for the complete extraction-shard/chunk workflow described above. Revisit only against a measured limitation or a separate explicit requirement.

## 13. Original planning evidence

The original planning pass was grounded in the extraction/resume/index code, central matching/mapping/assembly, application scheduler/worker protocol, dataset handoff, and test registration. The then-existing built executable was invoked successfully with `sfm --help`, `sfm extract --help`, `sfm match --help`, and `sfm merge --help` to inspect its public command surface.

Those help checks establish command exposure only. They do not establish that the installed binary includes every inspected source change, that distributed commands exist, that cross-machine artifacts are compatible, or that any proposed performance gain has been measured. No implementation build, reconstruction benchmark, GPU acceptance run, or distributed test was performed for this planning-only change.

## 14. P1 provenance closeout

This section tracks the input-provenance P1 work from the latest progress report.
It does not renumber phases A–F or close their distributed acceptance gates.
Reported build/test/review results below are accepted as supplied; they were not
rerun for this documentation update.

### Completed: raw-clock stream ownership

- [x] [`ProjectManifest.cpp`](../../src/data/ProjectManifest.cpp), `validate_revision`, rejects a raw clock whose `stream_id` is not in the `streams` of its referenced source. A matching stream on a different source does not satisfy that relationship.
- [x] [`project_manifest_test.cpp`](../../src/core/tests/project_manifest_test.cpp) covers the cross-source stream case.
- [x] Independent review completed with no findings.

| Reported verification | Result |
|---|---|
| Vulkan development build | Succeeded |
| `project_manifest_test` | Passed |
| Headless suite | **23/23 passed** |
| `git diff --check` | Clean |
| Existing `vswhere.exe` warning | Still present and nonfatal; not a new P1 blocker |

The clock-ownership fix is complete. Do not reopen it or rerun these checks merely
to reconfirm the report. The headless result is not native-video acceptance.

### Completed: Matroska presentation ordering

The contract, synthetic fixture set, source correction, supported non-NVIDIA
Vulkan Video run, and real extraction/provenance handoff are complete. The
frozen timing and decoded-luma oracles pass on the selected AMD device.

Keep `SS_ENABLE_PATENTED=OFF` by default. This native-video item is separate from
ordinary ffmpeg-backed preparation and extraction-shard acceptance; it does not
make patented decoding a requirement for those workflows. Any later native run
must be explicitly opted in on supported non-NVIDIA Vulkan hardware. No CUDA or
NVIDIA work is authorized by this status update.

#### Source boundary and ownership

[`Demuxer::next`](../../src/video/Demuxer.h) supplies coded packets in decode
order. [`MkvDemuxer::selectTrack`](../../src/video/MkvDemuxer.cpp) now scans the
selected track's timestamps without loading packet payloads and assigns stable
PTS/decode-order ranks. [`VideoPipeline::next`](../../src/video/VideoPipeline.cpp)
orders eligible presentations by those ranks and stores timing on each ready
entry, so `show_existing_frame` cannot overwrite a shared picture's identity.

[`Mp4Demuxer::buildPresentationOrder`](../../src/video/Mp4Demuxer.cpp), which
stable-sorts sample indices by PTS, is the existing convention to evaluate before
adding another ordering mechanism. It is not proof that copying that mechanism
unchanged handles every Matroska packet/picture relationship.

[`FrameExtract.cpp`](../../src/app/FrameExtract.cpp) consumes presentation
ordinals for selection and exported frame identity.
[`DatasetPrep.cpp`](../../src/app/DatasetPrep.cpp) writes and validates the
resulting provenance. Fix the source identity at the demuxer/picture boundary;
do not repair filenames afterwards or weaken the provenance validator.

#### Dependency-ordered remaining work

The row IDs below are local to this P1 continuation, not additional distributed
implementation phases.

| Row | State | Deliverable and gate |
|---|---|---|
| P1-MKV-SPEC | **Completed; reviewed** | The [video timing documentation](../../src/video/README.md) defines identity scope, stable PTS/decode-order ranking, equal-PTS behavior, timing availability, packet/picture limits, EOF, selected-track isolation, and the persisted consumer tuple. Static review found no unresolved contract requirement or source-boundary contradiction. |
| P1-MKV-FIXTURE | **Completed** | Three committed synthetic fixtures freeze hashes, packet traces, decoded luma hashes, two-track isolation, and malformed zero-scale behavior. The unchanged demuxer failed at `presentation ordinal 1` before the source correction. |
| P1-MKV-FIX | **Completed; GPU checked** | The demuxer pre-ranks selected-track blocks by stable PTS/decode order and rejects invalid timing. The pipeline ready queue carries immutable packet timing and orders by presentation ordinal. One layered DPB view selects reference slots through `baseArrayLayer`; this fixed the B-picture corruption exposed by the luma oracle. |
| P1-MKV-VERIFY | **Completed** | The patented Vulkan fixture passes timing and content checks on the selected AMD device, including a validation-layer run. Native worker preparation exported all eight frames with exact provenance, and a resumed worker run read and accepted that sidecar. |

#### Ordering decisions the specification must settle

- **Identity domain:** define the relationship between source, stream, discontinuity segment, decode ordinal, and presentation ordinal; state ordinal origin, whether gaps are preserved, and what remains stable after reopen, a dropped picture, and end-of-stream draining.
- **B-frames:** keep packets in decode order for the codec. Define how each displayed picture retains its source packet identity while receiving the correct presentation identity. A PTS regression in decode order is normal for B-frames and must not automatically create a clock-discontinuity segment.
- **Equal PTS:** retain distinct frames; specify a deterministic tie-break and its relationship to codec display order. Evaluate the existing stable PTS/sample-order convention, including a tie that crosses a B-frame reorder. Do not silently choose a tie-break during implementation, deduplicate by timestamp, or fabricate different timestamps.
- **Time authority:** preserve container timestamp/time-base precision. Specify negative, missing, invalid, and unrepresentable timestamp behavior and the correct `TimingKind`/availability. Do not manufacture PTS from nominal FPS or substitute packet count for an unavailable presentation identity.
- **Packet versus picture:** define the treatment of hidden/no-show pictures, repeated/show-existing output, and multiple visible pictures in one packet wherever the existing codec path permits them. State exact-provenance limits explicitly; do not assume every coded packet is one displayed frame.
- **Boundaries:** distinguish real discontinuities from reordering; specify short final GOP/EOF flushing and selected-track isolation. Matroska currently has no seek implementation: preserve that boundary rather than adding Cues/seek or unrelated BlockGroup sync changes to this item.
- **Consumer contract:** define the complete timing/identity tuple delivered by `FrameHandle` and persisted in exported provenance. If the source identity cannot be established under the agreed contract, report that limitation explicitly rather than publishing an exact-looking ordinal.

An output counter incremented when pictures happen to emerge is not an accepted
shortcut: the existing timing contract requires source identity to survive
missing pictures without renaming every later frame.

#### Opt-in fixture and oracle

Use synthetic or redistributable, explicitly approved media with recognizable
frame identities and an independently recorded expected trace. Record fixture
content hashes, generation/reference-tool versions where applicable, track IDs,
timestamp scale, codec/reorder assumptions, and expected outcomes. Do not derive
the oracle from the implementation being repaired or commit private captures.
Fixture preparation may use existing external tools; it must not create a new
build/runtime dependency.

| Fixture case | Required observation |
|---|---|
| B-frame GOP with differing packet and display order | Decoded frame content/identity, emitted order, PTS/time base, decode ordinal, and presentation ordinal agree with the frozen trace. Include the ordinary no-reorder control. |
| Equal-PTS frames, including a reorder tie | Both visible frames survive with stable distinct identities and the specified tie order; repeated runs/reopen do not change the mapping. |
| Timestamp gap and B-frame PTS regression | Normal reordering is not classified as a clock reset; gap/ordinal behavior matches the agreed contract. |
| Missing/invalid/unrepresentable time | The specified error or timing-availability result reaches the consumer; no fabricated exact provenance is accepted. Use a bounded malformed-container companion if needed. |
| Short final reorder queue | EOF emits every expected visible picture exactly once with its original timing association. |
| Multiple tracks and supported block/picture forms | Selected-track IDs and ordinals do not bleed across tracks. Any claimed exact support for SimpleBlock/BlockGroup or multi-picture/show-existing output has a matching case; other forms have an explicit boundary, not an assumed pass. |
| Real extraction/provenance handoff | Exported names and `PrepFrameSource` timing describe the intended source pictures and survive the existing provenance writer/reader. Disable sharpness filtering for this identity check; subjective sharpness labels are not a prerequisite. |

Observe the real `Demuxer` → `VideoPipeline` → extraction/provenance path, not just
a mocked timestamp sort or a helper forwarding its inputs. Do not fix unrelated
seek, sync, codec, or GUI behavior unless the scoped fixture exposes a necessary
dependency; document such a dependency before expanding the change.

#### Validation and closeout rule

The dedicated fixture is registered only in the explicit native-video lane:
[`cmake/SsNn.cmake`](../../cmake/SsNn.cmake) owns the patent-gated video library,
and [`cmake/SsTests.cmake`](../../cmake/SsTests.cmake) owns CTest registration.
`mkv_timing_test` is present only with `SS_ENABLE_PATENTED=ON` and carries the
`gpu;native_video` labels plus the shared `training_gpu` resource lock.

After the contract and fixture are ready, use the dev build entrypoint with an
explicit `SS_ENABLE_PATENTED=ON` opt-in and select a supported non-NVIDIA Vulkan
video device. An absent fixture, unavailable decoder/device, skipped test, or
empty test selection is **unverified**, not a pass. Keep a native-fixture result
separate from ordinary headless results. Restore `SS_ENABLE_PATENTED=OFF`
explicitly afterwards; omitting a cached CMake option does not reset it.

After the production change, run the focused native fixture and actual provenance
handoff, then the affected manifest/host tests and headless suite once after
integration, followed by review and diff checking. Record the new commands,
configuration, fixture hashes, device, outcomes, and any remaining blockers.
These are future post-change gates, not instructions to repeat the already
accepted clock-ownership verification now.

#### Observed fixture result

| Check | Observed result |
|---|---|
| `build_develop.bat -DSS_BACKEND=vulkan -DSS_ENABLE_PATENTED=ON` | Passed with MSVC 14.44.35207 and Slang 2026.12.0.1 |
| `build_vulkan\mkv_timing_test.exe src\video\tests\fixtures --demux-only` | Passed all hashes, packet timing/ranks, track isolation, and malformed timing checks |
| `cmake -E chdir build_vulkan ctest -N -R "^mkv_timing_test$"` | Registered exactly one test |
| `$env:SS_TEST_DEVICE='uuid:00000000020000000000000000000000'; cmake -E chdir build_vulkan ctest -R '^mkv_timing_test$' --output-on-failure --no-tests=error` | Passed timing and decoded-luma oracles on AMD Radeon AI PRO R9700, proprietary driver 2.0.395 / 26.Q3 |
| The same focused test with `SS_VK_VALIDATION=1` | Passed with no validation error |
| `spirula worker --request <native prep request>` followed by a resume request | Both attempts succeeded; eight frames were exported and the second attempt accepted `.spirula/prep-provenance.json` with exact presentation/decode ordinals and PTS |
| `cmake -E chdir build_vulkan ctest -L headless --output-on-failure --no-tests=error -j 2` | **23/23 passed** |
| `git diff --check` | Clean |

The Matroska ordering item is closed: semantics, fixture, implementation,
exported provenance, and supported-lane evidence agree. This does not reopen
the separate optional native-video CLI gap or claim closure of the remaining
distributed reconstruction/transfer/training/performance acceptance work.
