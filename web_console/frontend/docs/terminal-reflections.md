# CRT liner illumination

The bezel is illuminated by the terminal as an area emitter. It no longer copies
and flips strips from the four screen edges.

`terminalReflectionTransport.ts` constructs the four recessed liner surfaces from
the same projected aperture and setback used by the visible bezel. A centered
perspective camera defines the outgoing direction. Surface normals determine the
reflected directions; intersections outside the emitting aperture and paths
crossing adjacent walls contribute zero light. The depth is modeled as 2.5 times
the projected setback; the camera distance is 1.8 times the longer viewport edge.
These are scene dimensions, not measurements of a particular monitor.

“显示调校 → 屏幕嵌入深度” controls the projected setback in CSS pixels, from
12 to 72 px. The default is now 42 px (previously fixed at 28 px). It is saved as
`recess` in `terminal-appearance-v1`, shared across panes, and restored with the
other appearance settings. Old preferences without this field adopt 42 px;
invalid values fall back to the default and finite numeric values are clamped.
Small panes cap the effective inset to 22% of their shorter side, preserving an
open screen without overwriting the saved setting.

The same effective inset drives the terminal layout, bezel polygons and material
gradients, mitres, reflection geometry/cache keys and contact shadow. xterm refits
to the remaining aperture while retaining its session; pointer mapping follows
the resized glass. Range input is coalesced for 180 ms while dragging, with an
immediate commit on release/blur, to avoid rebuilding light paths for every input
event. Contact-shadow extent grows with depth while the displacement primitive
keeps its strict source clip, preserving sharp screen text.

The emitting screen is now a convex elliptic paraboloid:
`z = -depth + bulge * (1 - (x/rx)^2 - (y/ry)^2)`.
Curvature controls its bulge, bounded by the aperture size for tiny split panes;
zero curvature gives the previous plane exactly.
Ray intersections solve the quadratic with a cancellation-resistant root formula;
the closest positive, front-facing intersection supplies screen UVs. This also
handles grazing rays that hit the rising screen without decreasing z. The CRT
texture warp still defines the projected aperture and source coordinates, so the
reflection and the displayed terminal retain the same silhouette. Liner depth
follows the physical screen edge; adjacent walls share the same boundary-depth
function to keep their mitres joined. Occlusion tests use that curved-edge depth.

The default **rough plastic** combines visible-normal GGX single-scattering reflection (alpha 0.32,
IOR 1.5, exact unpolarized dielectric Fresnel, Smith masking) and a cosine-sampled
Lambertian substrate (albedo 0.28 with interface transmission). Each point uses 24
fixed samples per lobe. Misses are not renormalized. Radiance is accumulated in
linear light. Distance affects the emitter's solid angle and ray intersections;
there is no arbitrary inverse-square multiplier on specular radiance.

The selectable **smooth coating** is the ideal zero-roughness dielectric limit.
It traces the exact reflected direction `2 dot(n,v) n - v` at four fixed subpixel
wall positions per output pixel (2×2 pixel coverage, not a roughness lobe) and
multiplies incident radiance by the same unpolarized Fresnel term (IOR 1.5).
Transmitted energy is absorbed by the substrate; no diffuse lobe is added. Misses
and occluded paths still carry zero energy. It is not an axis-flipped image.
The sharper reflection uses up to 768 pixels on its longer edge. Subpixel samples
are averaged in linear radiance without a spatial Gaussian reconstruction pass.
Samples outside the current wall contribute zero rather than leaking across a
mitre. Four samples remain substantially fewer than the rough material's 48.

“显示调校 → 边框材质” selects either material. The choice is shared by the terminal
panes and saved with the other appearance preferences; old or unknown values
resolve to rough plastic. Changing material preserves the shell sessions.

The incident image includes terminal colors, glyph halos, cursor, signal changes,
brightness, and screen attenuation before integration. It is incorrect to reflect
an opacity mask independently and apply it after many incident rays are combined.
Selection backgrounds are captured from the DOM renderer's committed rectangles
and active/inactive colors, including selected blank cells. They enter the same
row light buffers as ANSI backgrounds, so moving or clearing selection leaves a
decaying trail and updates reflection without websocket output. Selection and
focus events invalidate those buffers; there is no extra per-frame DOM polling.
Glyph foregrounds come from the committed DOM styles on changed rows, so xterm's
contrast adjustment, inverse colors and dim opacity are preserved exactly rather
than reconstructed from ANSI attributes a second time.
The terminal screen declares `color-scheme: only light` to opt out of browser
automatic darkening while keeping its explicitly authored CRT palette. Browser
auto-darkening can turn a dark DOM glyph white and lighten SVG bezel colors at
paint time without changing `getComputedStyle()` or Canvas pixels; sampling the
computed foreground alone cannot correct that mismatch. The declaration is
scoped to the terminal screen and does not change the rest of the console.
See [CSS Color Adjustment, automatic color adjustments](https://drafts.csswg.org/css-color-adjust/#color-scheme-prop).

When xterm clears or replaces text, its render callback transfers the previous
emission to persistence before the next browser paint. Reflection remains
coalesced at 20 Hz. Every fading frame integrates the composed source through the
same light path; fading an already sRGB-encoded reflection used to give a different
decay curve and a brightness jump when a signal event forced fresh integration.
The exponential trail is an artistic persistence model, not a calibrated phosphor.
Normal removal of excitation starts a decay from the existing luminance, as
illustrated in [Tektronix, Cathode-Ray Tubes, decay time](https://w140.com/tekwiki/images/2/2b/068-0313-00.pdf).
Before enlargement, the rough material uses a single 5-by-5 reconstruction (sigma
0.75 output pixels) to suppress sampling artifacts in linear radiance. Its longer
edge is now up to 384 pixels rather than 256. The optical spread comes from the
GGX/diffuse lobes; the narrower filter only reconstructs the sampled field.
Wall IDs prevent mixing across a corner, and the narrow
joints are cut at output resolution rather than punching coarse holes into the
light field. This suppresses quadrature aliasing from dense terminal glyphs and
colored selection bars. It leaves the main terminal image untouched.
The reflection slider scales linear reflected radiance. Final composition adds
this radiance to the decoded base material, then encodes the result to sRGB.
`terminalBezelMaterial.ts` supplies the same material gradient to the SVG and the
compositor. The final overlay encodes only the required display-color difference:
source-over composition recreates that sum without washing the material out with
a translucent white layer. Decorative grain and joint shading remain approximate.
Neither material has an additional CSS blur. Both intermediate reflection
canvases retain their transport resolution through material composition, with
only the final CSS enlargement. The smooth GPU path skips the spatial-filter draw
entirely. The source image retains bilinear texture sampling for subpixel lookup.
The glass boundary uses a cached 2.2-pixel alpha feather before curvature, leaving
the interior text unblurred. Only the shadow filter extends beyond the source
rectangle: the displacement primitive stays clipped to prevent duplicate strips
from sampling outside its coordinate map. Neutral lip highlights and a soft
contact shadow join the glass to the liner.

This is a bounded, single-bounce model with a curved emitting surface mapped
through the CRT's existing texture warp. It does not simulate glass refraction,
multiple scattering, spectral transport, or head tracking. The viewpoint is fixed
at the center; pointer movement does not pretend to track a viewer's head.

## Cost and lifecycle

- Trace geometry only when dimensions, curvature or material change; keep at most four maps.
- Reconstruct a light field at up to 384 pixels (rough) or 768 pixels (smooth) on its longer edge, below the
  full-resolution terminal text. Use sparse paths for the visible liner only.
- Upload cached UV/throughput tables once. WebGL subsequently gathers screen light
  with a point draw, a rough-material reconstruction pass, and a material-composition pass, without per-frame geometry
  tracing or GPU pixel readback.
- The Canvas fallback evaluates the same paths and caches bilinear footprints.
- Preserve existing source/settled-jitter caches, update coalescing, hidden-page
  suspension, and continuous visible CRT motion. Reflection disabled creates no
  WebGL context.

## Validation

`test_terminal_transport.py` checks reflection angles, Fresnel limits, visibility,
perspective mapping, and bounded energy for both materials, including every smooth subpixel ray/Fresnel weight.
It also checks curved-surface residuals, aperture alignment, liner-to-glass joins,
flat limits, grazing intersections, back-face rejection and rough-lobe spread
independent of reconstruction. `test_terminal_transport_rendering.py`
compares GPU and fallback output, verifies dark input stays dark, and detects GPU
fallback or unexpected pixel readback. Browser display tests cover the four walls,
material joints, brightness/attenuation changes, trails, split panes, and motion.
The final compositor is checked against an analytic linear-light sum on both
renderers, including zero strength and the software renderer's own output canvas.

References: [PBRT reflection law and Fresnel](https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission),
[PBRT microfacet theory](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory).

The rendering regression also supplies a deliberately alternating bright/dark
sample grid: reconstruction must remove its checkerboard without changing average
radiance or leaking color between adjacent walls. A separate sharp-edge fixture
verifies that smooth pixel coverage keeps the first black pixel dark and the last
white pixel at full reflected radiance on both GPU and CPU renderers.

`test_terminal_materials.py` covers live switching, persistence, invalid preference
fallback, split-pane synchronization, CRT disabling, and session preservation.
`test_terminal_recess.py` verifies depth bounds, legacy preferences, narrow-pane
geometry, cache separation, default/adjusted inset alignment, shadow changes,
mouse selection, session preservation, persistence, split-pane synchronization,
and resetting a pending adjustment.

`test_terminal_browser_colors.py` compares real rendered screenshots with browser
auto-darkening enabled and disabled for both materials. Removing the opt-out is
the negative control: pixels must change while the reported CSS foreground stays
the same. This test requires the modern CDP Auto Dark Theme emulation API; older
system browsers skip it explicitly.
