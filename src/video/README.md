# Video decoding (`src/video/`)

Video decoding without ffmpeg, on top of `src/nn/`'s Vulkan runtime: the pixels
come out of `VK_KHR_video_decode_*` and everything the driver will not do for
itself — containers, bitstream parsing, reference picture management — is here.

**Compiled only with `-DSS_ENABLE_PATENTED=ON`, which is OFF by default.**
The H.264 / H.265 / AV1 bitstream parsers are the one piece of this repository
carrying third-party patent exposure, and this is a GPLv3 tree. With the option
off, `src/video/` is neither compiled nor linked and every consumer falls back
to an external ffmpeg: the GUI says why in the dataset panel, and `spirula sam
video` / `extract` say so and exit. See `cmake/SsOptions.cmake` and
`docs/build.md`.

Turning it on buys roughly 15x faster frame extraction (a 127-second 1080p30
clip in ten seconds), masking that rides along on the same device pass, and no
ffmpeg to install.

```
 file ──► Demuxer ──► CodecDecoder ──► vkCmdDecodeVideoKHR ──► YCbCr image
          mp4/mkv     H264/H265/AV1      (video queue)             │
                                                                   ▼
                                            luma plane ──► sharpness (compute)
                                            all planes ──► RGB (compute)
```

| | |
|---|---|
| containers | ISO-BMFF (`mp4`, `mov`, `m4v`, `insv`) and Matroska (`mkv`, `webm`), picked by content rather than extension. Multiple video tracks are enumerated, not merged — an Insta360 `.insv` is two fisheye streams. |
| codecs | H.264 (Baseline/Main/High), H.265 (Main/Main10/RExt), AV1 (Main/High/Professional) — whatever the *device* also advertises. |
| not supported | fragmented MP4 (`moof`), laced Matroska blocks, field-coded (interlaced) H.264/H.265, slice groups (FMO). Each is reported by name. |

## What a `CodecDecoder` is for

A hardware decoder does entropy decoding, motion compensation and filtering. It
does **not** track reference-picture state, and Vulkan makes that the
application's job. So per coded frame the codec layer produces:

* the bitstream the driver should see — Annex-B for H.264/H.265 (3-byte start
  codes; NVIDIA's H.265 slice-header parser assumes that prefix length at the
  offsets it is handed), the frame's OBUs for AV1;
* the `StdVideo*` picture-info structure;
* which DPB slot the picture activates and which slots it references.

The three implementations differ in where that state lives, which is why they
share an interface rather than code:

- **H.264** mutates its DPB in place: sliding-window or MMCO marking, with POC
  types 0/1/2 all in play. `H264Decoder.cpp`
- **H.265** rebuilds the reference picture set from scratch for every picture,
  so most of that file is `st_ref_pic_set()` and the POC bookkeeping around it.
  `H265Decoder.cpp`
- **AV1** has no parameter-set NAL units at all: quantization, segmentation,
  loop filter, CDEF, loop restoration, tiling and global motion are re-sent in
  every frame header, and each has a StdVideo struct to fill. That file is a
  complete section-5.9 frame-header parser. `Av1Decoder.cpp`

AV1 also puts several coded frames in one temporal unit — one shown frame plus
the hidden alternate references it was predicted from — so a packet can yield
several pictures (`PictureInfo::more_in_packet`), and `show_existing_frame`
re-outputs a picture decoded earlier, which is why pool images are pinned while
a DPB slot still names them.

## Two decode output layouts

`VkVideoDecodeCapabilitiesKHR` reports one of two arrangements and both are
implemented:

- **DISTINCT** — the driver writes a standalone output image alongside the
  reference it sets up. Nothing more to do.
- **COINCIDE** — the decode target *is* the DPB slot, which is what NVIDIA
  reports for H.264 and H.265. The picture then has to be copied out before that
  slot is recycled, so `recordDecode` appends a `vkCmdCopyImage` (one region per
  plane; multi-planar images cannot be copied with a COLOR aspect) after
  `vkCmdEndVideoCodingKHR`. About 30 µs at 1080p.

## Queues

Decoding runs on the video-decode queue family with its own command pool and
timeline semaphore; the conversion runs on the compute queue through the normal
`vk::Stream`. Output images are created `VK_SHARING_MODE_CONCURRENT` across the
two families so no ownership transfer is needed, and `Stream::waitOn` attaches
the decode's timeline value to the next compute submission.

Two things bear repeating:

1. **`vkCmdBeginVideoCodingKHR` binds resources; the decode operation activates
   slots.** The picture being reconstructed is therefore listed with
   `slotIndex = -1`. Passing its real slot index is a validation error until
   some earlier decode has activated it — which is not the case the first time
   around.
2. Layout transitions for the pool images happen on whichever queue is about to
   use them, with `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` on both sides. A video
   queue supports very few pipeline stages; ALL_COMMANDS is always legal.

## The entry-point table lives one device, not one process

`video_api()` resolves `VK_KHR_video_*` through `vkGetDeviceProcAddr`, and the
table it hands out is valid **only for the device it was resolved from**.
`nn::shutdown()` is not a process-exit hook — the GUI calls it after every
dataset job to hand the 2 GB of segmentation weights back — so a second video
open runs on a second device, and a table cached for the life of the process
then points at loader trampolines that were unmapped with the first one. The
symptom is a segfault in an unnamed frame under `createSession`, one open too
late to look like a lifetime bug.

So the table is keyed on `vk::Context::generation()` and re-resolved when that
changes. Anything else that caches device-derived state across a `shutdown()`
owes the same check; `vk::Stream` and `vk::Pipelines` instead drop their whole
`Impl` and re-initialise lazily, which is the other valid answer.

## Why frames are handles

`VideoPipeline::next()` returns a `FrameHandle`, not an image. Decoding and
*using* a frame are separate because the caller that motivates this — picking
the sharpest frame of every window — should not pay to convert and download the
frames it discards. Pixels are touched only when `queueSharpness()` or
`toImage()` ask for it, and the handle keeps the picture alive until released.
Presentation order needs a reorder queue anyway, so the pool and the explicit
release are not extra machinery.

The pool is sized `max_reorder + lookahead + max_dpb_slots + 4` pictures. At 4K
that is a few hundred megabytes, which is the price of holding a blur-selection
window in decoded form rather than re-decoding it.

## Frame timing and identity

A frame identity is scoped to one source file, one selected container stream,
and one discontinuity segment. `TrackInfo::stream_id` is the stable container
stream identity; `FrameTiming::stream_index` points to that stream in the
source's recorded stream table. Ordinals start at zero independently for each
stream and segment:

- `decode_ordinal` is the selected stream's coded-packet position. It follows
  container/decode order and is assigned before decoding.
- `presentation_ordinal` is that packet's rank in presentation order. It is
  assigned from container timing before decoding, never by counting pictures
  emitted by `VideoPipeline`.

The rank includes packets that later yield no visible picture. A packet with no
visible output, or a picture deliberately dropped after successful decode,
therefore leaves a gap instead of renaming every later frame. Hidden units inside
a packet do not consume additional ordinals. A decode failure aborts the stream.
Reopening the same source and selecting the same stream gives the same ordinals;
draining a short final reorder queue does not change them.

### Matroska ordering contract

Matroska stores referenced frames in coding order and permits their timestamps
to regress in that order (RFC 9559, sections 10 and 11). The demuxer must keep
that packet order for the codec while assigning presentation identity from the
selected track's block timestamps:

1. Compute each unlaced selected-track block's exact timestamp as the signed
   block timestamp plus its cluster timestamp, scaled by `TimestampScale`.
2. Rank all such blocks by ascending PTS, breaking equal-PTS ties by
   `decode_ordinal`. This is the same stable PTS/source-order convention used by
   the ISO-BMFF sample table. Equal timestamps remain distinct and unchanged.
3. Deliver visible handles in ascending `presentation_ordinal`. Codec completion
   plus `max_reorder` determines when output is eligible; the ordinal chooses
   the next eligible output. Codec POC remains display metadata, not the source
   identity or tie-break. Equal PTS has no earlier/later distinction to preserve,
   so a tie that crosses codec POC order is emitted in stable block order.

A PTS regression in decode order is normal B-frame reordering. Neither it nor a
timestamp gap starts a discontinuity segment. Native Matroska files currently
have one segment (`discontinuity_segment == 0`): the demuxer has no supported
container signal for a later segment and must not infer one from timestamp
shape. Other selected tracks do not consume either ordinal and never affect the
rank. Current native extraction and seek naming are valid only for segment zero;
supporting another segment requires composite segment/ordinal output identity
and a segment-aware seek contract before such frames can be published.

Matroska PTS is represented exactly in integer nanoseconds while the scaled
value fits `int64_t`; negative values are valid. The native outcomes are:

| Input | `TimingKind` and availability |
|---|---|
| Valid block timestamp and scale | `SourceExact`, known decode/presentation ordinals, `has_pts`, time base `1/1000000000` |
| Valid `DefaultDuration` | Adds exact source duration; the kind remains `SourceExact` |
| No container DTS | `has_dts == false`; DTS is not inferred |
| Missing, malformed, or unrepresentable block timing | Demux error before packet publication; no ordinal or `FrameHandle` |

The native Matroska path does not emit `SourceDerived` or `Missing` packets.
`SourceDerived` is reserved for a value derived losslessly from other source
metadata; `ExportDerived` is the external-decoder fallback and carries no source
ordinals or source PTS. Nominal FPS, packet count, and decode output order are
not timing fallbacks.

The timing tuple belongs to a container packet. H.264/H.265's supported unlaced
blocks contain one coded picture. An AV1 temporal unit may contain hidden coded
pictures followed by one presentation: hidden/no-show pictures produce no
`FrameHandle`, while the packet's sole visible output inherits its timing.
`show_existing_frame` is a new presentation of stored pixels and inherits the
current packet's timing, not the packet that originally decoded those pixels.
The ready queue must therefore store immutable timing per presentation instead
of attaching mutable timing to a shared picture-pool slot. More than one visible
output from one packet is outside the exact-provenance contract; reject it rather
than assigning duplicate or output-counted identity. All units in a packet must
be consumed and this count validated before any presentation from that packet is
returned. Laced blocks remain unsupported.

`VideoPipeline::next()` returns visible pictures in the order above and retains
the complete packet tuple: timing kind, stream index, discontinuity segment,
both ordinals, available PTS/DTS/duration fields, and their time base.
`PrepFrameSource` persists that tuple together with the source ID, output name,
and output digest. The provenance schema must also serialize and validate the
capture's `PrepStream` table so `stream_index` resolves to the recorded stable
`stream_id` after reload; exact native provenance is incomplete without it.
Extraction must fail if the selected picture has no proven presentation ordinal;
filenames and `export_ordinal` must not repair or replace missing source identity.

At end of stream, the pipeline consumes every `more_in_packet` picture in the
last packet and drains every ready visible picture exactly once before reporting
EOF. There is no separate codec drain API. Truncated packets and incomplete
pictures are errors, not a successful short stream. SimpleBlock and the Block
inside BlockGroup use the same timestamp/ordinal rules; current BlockGroup
keyframe approximation, Cues, seeking, lacing, and live unknown-size clusters
are separate support boundaries.

### Seeking by presentation ordinal

`VideoPipeline::seek()` jumps to the sync sample at or before a presentation
ordinal — the reorder queue, the DPB pins and the codec's reference state all
go, and decoding resumes from the keyframe. It is what makes the GUI's frame
slider usable on a long capture: the last frame of a fifteen-minute GoPro clip
is 35 s of decoding without it and 0.09 s with, and the frames it hands back are
bit-identical (154 probes over the local corpus, one keyframe interval apart
either side of the boundaries). Matroska has no seek here — no cue parsing —
and `seekSync()` returns false without changing the current position. Reopen the
source to read it again from the start.

## Colour

`color_matrix()` folds bit depth, studio/full range and the BT.601/709/2020
coefficients into one 3×4 matrix, so the shader has a single code path. Chroma
is box-averaged over the same source footprint as luma rather than bilinearly
interpolated; against ffmpeg's `swscale` that is worth a maximum error of about
3/255 on a 1080p frame, all of it at chroma edges.

Film grain synthesis is parsed but not requested (`apply_grain = 0`). It is a
cosmetic post-process, the frames feed a segmentation model, and asking for it
would force the distinct-output path on drivers that would rather not.
