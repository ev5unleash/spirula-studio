# Camera frustum display size

`src/data/FrustumSize.h` picks the one number every viewer needs before it can
draw a dataset's cameras: how big a frustum is, in world units. The native
viewport and the web viewer get it through `viewer_camera_size_heuristic`
(`src/app/webviewer/RenderWorker.cpp`); the standalone viewer through
`ssv_ds_frustum_size` (`viewer/src/dataset_bridge.cpp`). The user's slider
multiplies it.

## What it optimizes

The question the user is really asking is "how much of my screen do the
cameras take up?", so the size is chosen to make that answer a constant. A
frustum of size `s` seen from distance `D` covers about `(s·F/D)²` pixels, `F`
being the viewport focal length in pixels. Summing over the `N` distinct
cameras and asking for a fraction `p` of a square `W×W` view:

    N · s² · F² · ⟨1/D²⟩ = p · W²        with F = W / (2 tan(fov/2))
    s = 2 tan(fov/2) · sqrt(p / N) / sqrt(⟨1/D²⟩)

`p` is 1% and the fov is 90°, which is where all three viewers open. On a
1900-frame ZipNeRF walk that reproduces the previous default to within 5%; on
a 45-shot object capture it is 70x larger than the previous default was.

## The typical eye

`⟨1/D²⟩` needs an eye distribution, not one eye. A single home position is
brittle, and eyes placed on or near the camera positions themselves are
dominated by the nearest pairs (the same failure as a k-NN spacing). The eye is
taken uniform over a shell of 0.5 to 2 cloud radii around the cloud centre,
which is the zoom range a turntable session covers. That average has a closed
form per camera, so it is one O(N) pass with no sampling:

    ball(ρ, r) = 3/(2ρ²) + 3(ρ² − r²)/(4ρ³r) · ln((ρ + r)/|ρ − r|)
    shell(a, b, r) = (b³·ball(b, r) − a³·ball(a, r)) / (b³ − a³)

with `r` the camera's distance from the centre; `ball(ρ, 0) = 3/ρ²`,
`ball(ρ, ρ) = 3/(2ρ²)`. `frustum_size_test` checks it against quadrature.

Measured effective distance `1/sqrt(⟨1/D²⟩)` in cloud radii, for a 45-shot
object capture and a 1900-frame ZipNeRF walk:

| eyes                                      | object | walk |
|-------------------------------------------|--------|------|
| shell 0.5–2 (used)                        | 1.21   | 1.25 |
| shell 0.7–1.5                             | 0.98   | 1.05 |
| shell 1–3                                 | 2.00   | 2.04 |
| 64 sampled eyes, only cameras in the fov  | 1.30   | 0.91 |
| single home eye                           | 0.85   | 1.00 |
| the camera positions themselves           | 0.47   | 0.25 |

Every region-based choice lands within 2x; the fov-restricted sampled version
is the upgrade path if walkthrough datasets ever need it, and it drops in as a
replacement for `⟨1/D²⟩` alone.

## Distinct cameras

`N` counts positions, not images. Positions within 1% of the cloud radius are
one camera: exposure brackets, rig heads and stalled video frames draw on top
of each other and add no screen area. A 225-image, 5-bracket capture has
brackets 0.05% apart and shots 40% apart, so any tolerance from 0.5% to 2%
finds its 45 shots; the 1982-frame walk loses 7% of its frames at 1% and 37%
at 2%.

This is also what the previous heuristic (0.2 × median distance to the 4th
nearest camera, with exact-match deduplication) got wrong: on the bracketed
capture the 4th neighbour was always a bracket of the same shot, giving a size
of 0.05% of the radius, 100x too small for the slider's 10x range to recover.

## Guards

- `s ≤ 0.15 R`: with two or three cameras the formula wants a frustum as wide
  as the scene.
- No spread (one distinct position, or none) returns 0.2, there being no
  scale to measure.
- Non-finite positions are skipped.
