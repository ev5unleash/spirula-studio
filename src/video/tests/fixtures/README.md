# Matroska timing fixtures

These files freeze the packet-order cases used by `mkv_timing_test`. They are
synthetic and have no runtime FFmpeg dependency. They were generated with
FFmpeg 9.0.1 (`Lavf/Lavc 63.1.101`) and inspected with the matching `ffprobe`.

```powershell
ffmpeg -f lavfi -i "testsrc2=size=64x48:rate=5:duration=1.6" `
  -vf "setpts='if(lt(N,3),N,N+2)/(5*TB)'" -map_metadata -1 `
  -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -g 10 -bf 2 `
  -x264-params "b-adapt=0:keyint=10:min-keyint=10:scenecut=0:threads=1" `
  -fps_mode passthrough bframes_gap.mkv

ffmpeg -f lavfi -i "testsrc2=size=64x48:rate=5:duration=1.2" `
  -vf "setpts='floor(N/2)/(5*TB)'" -map_metadata -1 `
  -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -g 10 -bf 2 `
  -x264-params "b-adapt=0:keyint=10:min-keyint=10:scenecut=0:threads=1" `
  -fps_mode passthrough equal.mkv
ffmpeg -f lavfi -i "smptebars=size=64x48:rate=5:duration=1.2" `
  -map_metadata -1 -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p `
  -g 10 -bf 0 -x264-params "keyint=10:min-keyint=10:scenecut=0:threads=1" `
  control.mkv
ffmpeg -i equal.mkv -i control.mkv -map 0:v:0 -map 1:v:0 `
  -map_metadata -1 -c copy equal_pts_two_tracks.mkv
```

`zero_timestamp_scale.mkv` is `bframes_gap.mkv` with the unique
`2a d7 b1 83 0f 42 40` TimestampScale element changed to
`2a d7 b1 83 00 00 00`.

| file | SHA-256 |
|---|---|
| `bframes_gap.mkv` | `0d8341087fb31530520e4cb7fda70f78c7ea572df7c53f37899908c3a15a87b0` |
| `equal_pts_two_tracks.mkv` | `dc7b90408d23ce1b69d4628704626482d5ea266fe97baff6ff17d844dd870d2d` |
| `zero_timestamp_scale.mkv` | `a40654b153ee2998760e3e2da85c798ed5856a230eb94ef2f98f8cd1cd21b49f` |

The frozen `ffprobe -show_packets` PTS traces, in decode order and milliseconds,
are:

```text
bframes_gap track 0:            0 1000 200 400 1600 1200 1400 1800
equal_pts_two_tracks track 0:   0 200 0 200 400 400
equal_pts_two_tracks track 1:   0 200 400 600 800 1000
```

Both B-frame tracks report `has_b_frames=2`; the control track reports zero.
All tracks are 64x48 H.264 with a 1/1000 `ffprobe` time base and 200 ms packet
duration. The native demuxer represents the same times in integer nanoseconds.

The decoded luma oracle was produced without gap-filling frames:

```powershell
ffmpeg -i bframes_gap.mkv -map 0:v:0 -vf extractplanes=y `
  -fps_mode passthrough -f rawvideo gap_luma.raw
```

Its eight 64x48 planes have these presentation-order SHA-256 values:

```text
ed4400cfdcf3fb8909dd2adc847a8e6b3bdb196b1840b384dc9c8bc4feb924a9
2c485ee4b3fb8d84e0aacac75e9891a23f857682558803f7b6fc9f9880390226
23d75a14738069b6ec27af0ec08b25f5ca861f229e2198fda2de7377d0d2a672
0c907b4c3e219860965f6c654b1f8030d70fbc4bde235a87342c8fe51111ebdd
00b47e8c818033ee29a107e54fc81ca9aee2f4c0078e37f4dca8bbf13474d29b
27a2611e5018cd527893659182e9246c3e4a0ff776699689bf74c110f7b716b2
b9bab2a3e787fdae2e135dd16a1d1b1b6f59327a9a798a1c492faad474f5ecd5
```
