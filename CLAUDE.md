# DOD-SP: read this first, every session

DOD-SP is a single-player Day of Defeat: Source mod built on Mapbase (Source SDK 2013 SP). The
owner maps and models; Claude writes the code and builds everything in GitHub Actions. This file
holds what we learned the hard way. **We have already lost a full day re-fixing something we had
fixed before** (the 32-constant limit below), because the knowledge lived only in a chat summary.
If you learn something like this, add it here in the same commit as the fix.

## How we work (the owner's rules)

- Whole-class fixes, never one-off band-aids. Every fix names the INVARIANT it protects, and gets a
  guard (CI check, COMPILE_TIME_ASSERT, or a watchdog that prints an `INVARIANT:` line).
- One owner per fact. Merge duplicates before changing one of them.
- **A revert keeps the guards.** When we went back to stock Mapbase lighting (3f606c6), the
  register lint went with it and we walked straight back into the bug. It also took the runtime
  SDK_ material swap with it, which predated the reverted work (restored in 36624ab). Before a
  revert, list the lints, checks and older features in the reverted range and keep them.
- Every stage states the files it touches and how to undo it, and ends with a known-good comparison.
- Nothing of ours shares a name with Valve's or Mapbase's: files `dodsp_*`, shaders `DODSP_*`,
  ConVars `dodsp_*` (CI enforces this for the shader DLL).
- The owner doesn't run scripts or hunt logs. Tell them exactly what to type and which single
  console line to paste back. Post the build number and the direct artifact link
  (`https://github.com/luckeating/source-sdk-2013-DODSP/actions/runs/<run>/artifacts/<id>`), and
  give an ETA before a build. Long builds are only for big shader work, with a warning first.
- Tell the owner when they're wrong (their monitor settings can make screenshots look off).
- Test steps must list every setting that resets on launch (see "Settings that reset" below).

## Engine facts (Source 2013 SP, engine binaries are 2014 builds)

### Shaders
- **Pixel shader constants: c0..c31 ONLY.** The engine keeps exactly 32 pixel constants
  (`m_NumPixelShaderConstants = 32`) even on shader model 3 cards. Writing c32+ overflows its
  storage and corrupts unrelated memory. That crash shows up somewhere else entirely: 3b-2 used
  c32-c39 and `mat_reloadallmaterials`/`buildcubemaps` crashed deep in the filesystem's VPK code.
  Found 2026-11-15 (b50308d), lost in the revert, re-found 2026-11-17 (3b4). Guard:
  `devtools/dodsp_build_shaders.py` check_constants (pixel shaders + every header they include,
  literal writes in our C++, sun registers vs `DODSP_PSREG_*`). Vertex shaders really do have 256.
- Taken pixel constants in the brush shader: c0, c2-c4, c7, c8, c10-c12; c21 (blend/cubemap pos),
  c22-c25 (parallax); c28 flashlight colour, c29 linear fog colour, c30 light scale, c31 screen
  scale (common_ps_fxc.h, engine-set). **The sun owns c13-c20** (dodsp_sun_fxc.h); those were
  Xbox-360-only flashlight constants, now under `#if defined( _X360 )`. Mapbase's helper still
  writes c13/c14/c19 inside `hasFlashlight && IsX360()`. Those lines carry `dodsp-lint: x360 only`.
- The engine loads exactly `game_shader_dx6.dll` .. `game_shader_dx9.dll` from the mod's bin.
  `game_shader_generic*` is NOT loaded (tested). Ours is `game_shader_dx6.dll`. Mapbase's is
  `game_shader_dx9.dll` and must stay (the user once renamed the wrong one: missing brushes).
- When two DLLs define the same shader, the last-loaded wins. A mod shader reusing a Valve shader
  name is a fatal Error. Hence the `DODSP_` prefix.
- **ConVars with the same name in two DLLs get linked**, and ours loads first, so our default
  wins. Prefix everything `dodsp_` (`SHADER_DLL_CVAR` in BaseVSShader.cpp). `mat_fullbright` is
  the engine's own and is fine to share.
- Compiler: Valve's ShaderCompile2 (2025 SDK, x64, needs D3DCOMPILER_47) in devtools/bin. .inc
  combo numbering: dynamic combos first in declaration order, then static. A `0..0` combo keeps
  its setter but adds no variants. CI checks our generated .inc against ShaderCompile2's.
- **Model shaders (stage 4) have almost no free pixel constants.** The model sun does its cascade
  maths in the vertex shader (light-space position out on TEXCOORD9, `dodsp_modelsun_vs_fxc.h`) and
  needs only 4 pixel constants: plain `c15..c18` (unused there), bump `c22..c25` and skin `c15..c18`
  (the flashlight's registers; in the base pass the flashlight names become `#define`d constants so
  the dead flashlight code still compiles). Vertex constants `c221..c224` (bones end at c216,
  c217..c220 are the engine's clip planes). Numbers live in `DODSP_MSUN_*` (dodsp_sun_shader.h);
  CI checks every .fxc against them and flags any C++ write onto them (literal or PSREG_ name)
  unless the line says `dodsp-lint: flashlight pass only` / `x360 only`.
- **vs_3_0 has 11 output registers** (o0..o10 with POSITION and FOG; 12 only with PSIZE) and Mapbase's
  plain and bump model vertex shaders use all 11 (X5622). The sun took their eye-vector slot; the pixel
  shader rebuilds the eye vector from `g_EyePos` (c20) and the world position (`DoDSP_VertToEye`).
- **The skin pixel shader uses all 16 samplers** (X4500 on s13..s15). Its cascades sit on the wrinkle
  samplers s9..s11 (WRINKLEMAP is pinned to 0): `DODSP_SAMPLER_NEAR` in the .fxc, `DODSP_MSUN_SAMPLER_SKIN`
  in C++ - CI checks they agree.
- The shader build runs every fast check first and stops BEFORE compiling if one fails (seconds, not
  40 minutes). The D3DVERTEXTEXTURESAMPLER0 trap below was in these notes and still cost a 40-minute
  build on stage 4, because nothing checked for it - now the build does. Turn every trap written
  here into a check.
- **One sun on models:** a vertex that takes our per-pixel sun drops every directional light the engine
  hands the shader (`cLightInfo[i].color.w == 1`, the engine's once-per-model sun). Which vertices take
  it: unbaked ones follow `c221.w` (`DODSP_SUNMODE_DYNAMIC`, i.e. `dodsp_sun_baked 0`); ones with VRAD's
  baked vertex lighting (`STATIC_LIGHT`, static props) follow `c224.x` (`DODSP_SUNMODE_STATICPROPS`).
- **Stage 4d: static props get the per-pixel sun.** Vertex lighting has no light styles, so with
  `-dodsp_sunstyle` + `-StaticPropLighting` VRAD bakes static props WITHOUT the direct sun and marks the
  BSP's level flags (`DODSP_LVLFLAGS_PROPS_NOSUN_NONHDR/_HDR`, per pass, `dodsp_lightstyles.h`). The client
  reads the mark at map load and turns `DODSP_SUNMODE_STATICPROPS` on in BOTH sun modes (bits ride in
  `DODSP_RP_VEC_FILTER.z`: all 20 vector render parms are taken). Old maps (no mark) keep the sun in their
  props and get no second one. Watchdog INVARIANT if a marked map can't draw the per-pixel sun (those
  props then have no direct sun at all). The sun's BOUNCE stays in the bake: `ComputeIndirectLightingAtPoint`
  (vraddetailprops.cpp) adds the hit surface's style-31 lightmap - it used to read style 0 only, so
  since `-dodsp_sunstyle` static props had lost the sun bounce off sunlit floors (darker props).
  Guard: `dodsp_lint_client.py` OWNERS (one writer/reader for the modes and the mark).
- **Static props compiled WITHOUT `-StaticPropLighting` get no per-pixel sun either** (tested 2026-11-17,
  `dodsp_sun_baked 0`): sun-facing quoins/shutters drew dark grey next to sunlit plaster. The engine
  lights them its own way at map load, not like moving props. So Stage 4d keeps `-StaticPropLighting`
  and adds the per-pixel sun to the STATIC_LIGHT shader path (VRAD bakes static props minus the direct
  sun). Don't retry the "skip the bake" shortcut.
- **Sleeping physics props bake their lighting once** (`C_PhysicsProp`, `r_PhysPropStaticLighting`): when
  a prop settles, `RecomputeStaticLighting` bakes the engine's lights as they are at that moment and the
  prop then draws with `STUDIO_STATIC_LIGHTING` (the STATIC_LIGHT shader path: no per-pixel sun). With
  `dodsp_sun_baked 0` the bake has no sun, so a bumped prop went dark once it settled (2026-11-17). While
  the sun shaders are in use, sleeping props use live lighting like moving ones. Guard:
  `devtools/dodsp_lint_client.py` (runs before the game build) fails on any client use of static
  model lighting without the `DoDSP_SunShadersInUse()` check and the `dodsp-lint: static lighting guarded`
  tag. Props that never moved were baked at map load, while `dodsp_sun_baked` was still 1.
- DOD-SP shaders pin features they leave out to `"0..0"` combos and hand such materials back to
  Mapbase via SHADER_FALLBACK. The list of features (`DoDSP_BrushUnsupported`,
  `DoDSP_ModelUnsupported`) and the pins must describe the same things; `DoDSP_Pinned` wraps every
  pinned combo setter in the model helpers and prints an INVARIANT if a pinned combo is ever set
  non-zero (that would select the wrong shader variant).
- Mapbase ships its shaders precompiled, so their sources may not compile on today's compilers:
  `register( D3DVERTEXTEXTURESAMPLER0, s0 )` is rejected (X3089, write `s0`), and a loop over the
  integer light count fails (X4509, read the light-enabled booleans instead).
- SM3: texture reads that use gradients (tex2D, tex2Dproj) can't sit inside dynamic branches.
  Sample everything and select without branching.
- **Sun cascades are RAW depth (stage 4b).** On NVIDIA (`GetShadowFilterMode() == 0`) the client makes them
  INTZ, elsewhere the card's own shadow depth format; always point sampled (TEXTUREFLAGS_POINTSAMPLE), read
  with `tex2Dlod` and compared in the shader. No `tex2Dproj`, no `SetShadowDepthFiltering` on a cascade
  sampler (Mapbase's brush helper had one on s14 = our mid cascade; removed). Guard: build script
  `check_raw_depth`. The old `DODSP_SUNFILTER` combo is gone. Watchdog INVARIANT if a cascade didn't come
  out in the readable format (the dynamic sun then stays off).
- **Soft shadows (4b, dodsp_sun_common_fxc.h):** blocker search (12 taps) -> penumbra = (receiver -
  blocker depth) x depth range x tan(sun radius) -> 16-tap filter, golden-angle spiral turned per texel,
  receiver-plane bias from ddx/ddy (taken BEFORE any branch). One loop body reads whichever cascade
  holds the point (`DoDSP_ReadCascade`): three inlined copies would blow the ps_3_0 512-slot limit next
  to Mapbase's skin shader. Constants: brush c15.w = tan, c20.w = min radius; model PS MSun0.w = min
  radius, MSun3.w = tan, to-sun z rebuilt from x/y (watchdog: sun must shine down); model VS c224.y =
  depth range, passed to the PS in the sun coordinate's .w (0 = takes no sun). Render parms: DEPTH.y =
  tan, FILTER.x/.y now in cascade texture units. Sun size: light_environment `SunSpreadAngle` (client
  parses the entity string; VRAD reads the same key), x `dodsp_sun_softness`; none in the map ->
  `dodsp_sun_spread_default` (2 degrees).
- Valve's `BuildOrthoWorldToFlashlightMatrix` gives depth (d + near)/(far - near), but the ortho
  shadow map stores (d - near)/(far - near). If you ever use Valve's ortho matrix, fix the depth
  (5cd6ea4). Our cascades build their own matrix.
- `r_flashlightdepthres` changed mid-game reallocates depth textures and crashes. It's a launch
  setting. 8192 shadow maps looked blurrier than 2048 in testing.
- The engine draws overlay/decal flashlight passes by frustum. Any flashlight state handed to the
  engine lights overlays too, even ones its shadow map never covered (the "phantom window").
- Abandoned: the darken-only sun (subtract the sun where a dynamic map says blocked). Stale maps,
  phantom light, and saw-tooth fringes where two differently filtered maps disagree. The current
  design is VRAD's direct sun in light style 31, switched off (`dodsp_sun_baked 0`) and added back
  per pixel. Nothing is ever subtracted.

### Code / build
- MSVC precompiled headers: anything before `#include "cbase.h"` is skipped. A file that needs
  windows.h/dbghelp.h first is set to "Not Using Precompiled Headers" in its .vpc.
- MSVC treats C4189 (unused local) as an error. Calling a non-const member through a const object
  is C2662.
- `mod_studio` / `mod_brush` need `model_types.h`; cbase.h doesn't bring it in (C2065, build 44250885877).
- `IMaterialVar` is only forward-declared by imaterial.h: include `materialsystem/imaterialvar.h` (C2027, 44255842127).
- GitHub warns "This repository moved" on push. That's harmless. A workflow that fails in 0 s is a
  YAML error: check for duplicate keys left by edits. The workflow file contains a literal tab in
  a sed line that GitHub accepts but PyYAML doesn't.
- Artifact downloads are blocked from Claude's sandbox. Verify from CI logs instead.

### Content / game
- `buildcubemaps` ends with `mat_reloadallmaterials`. Crash tests use that command, which is faster.
- Every map compile throws away the built cubemaps, so envmapped surfaces look wrong until
  `buildcubemaps` runs again. After any recompile (even a test one), run it before judging
  reflections. Build them in the sun mode you'll play in (`dodsp_sun_baked 0`): what's on screen is
  what the cubemaps capture.
- Mapbase's patched HL2 materials use `SDK_LightmappedGeneric` and need Mapbase's shader DLL. The
  client swaps DoD:S world materials onto the SDK_ shaders at runtime (c_dodsp_sdkshaders),
  because the stock 2013 shaders draw envmapped props as noise and floor decals fullbright.
  With the sun shaders in use the swap goes one step further, to the DODSP_ twins: brushes
  (LightmappedGeneric/WorldVertexTransition) and models (VertexLitGeneric), including materials
  whose .vmt already names the SDK_ shader (HL2 content) - only if the loaded DLL has that twin.
- DoD:S content mounts after HL2, so HL2's same-named sound and soundscape manifests hide DoD's.
  We copy missing DoD entries into a generated override (eaa0e7e).
- DoD maps use prop_physics_multiplayer. The SP player didn't collide with it (004173a).
- **HL2 materials drew dark with `dodsp_sun_baked 0`** (2026-11-17). Their .vmt names Mapbase's
  `SDK_LightmappedGeneric` directly, and the runtime swap only moved materials naming Valve's shaders,
  so they lost the baked sun and never got the per-pixel one. NOT the compiler, NOT leftover .vcs
  (dodsp_lightmap_report: all match; DOD-SP\shaders\fxc held only dodsp_ files). Lesson: when a
  surface is dark, first ask which shader draws it and whether `dodsp_sun_baked` is 0. Fixed: the
  swap also moves SDK_ brush materials to the DODSP_ twins; INVARIANT at load lists any lit world
  material still without the per-pixel sun. `dodsp_lightmap_report` / `_here` (compiled lightmap vs
  what the material wants) stay as the check for real compile mismatches.
- `mat_luxels 1` draws garbage (solid blue patches) in this setup. Don't diagnose with it.
- **The owner plays with `mat_hdr_level 0` (LDR), and every lighting test so far was in LDR.** A
  `-hdr`-only VRAD writes no LDR lighting (log: `LDR lightdata 0`) and the map draws fullbright
  (2026-11-17). Give compile lines with `-ldr` (or both passes). HDR with the DOD-SP shaders is untested.
  Owner's VRAD line: `-game $gamedir -ldr -final -dodsp_sunstyle [-StaticPropLighting] -StaticPropPolys
  -textureshadows $path\$file` (Hammer's $light_exe is our Mapbase vrad in the SDK 2013 SP bin).
- Hammer expert compile: every `-option` goes BEFORE `$path\$file` in a tool's parameters. An
  option after the map name is read as the map name, the tool prints usage and the compile fails.
- The engine writes no crash logs. Ours (c_dodsp_crashlog.cpp) writes `DOD-SP\dodsp_crash.txt` and
  `.mdmp`. Memory corruption crashes far from the cause, so don't trust the crash location alone.

## Settings that reset every launch (put them in every test step)
- Since the defaults build (2026-11-17, after 4c) the owner's look is the DEFAULT: `dodsp_sun_baked 0`
  (per-pixel sun), `dodsp_sun_softness 0.3`, `dodsp_sun_propshadows 0` (no prop shadows). Anything else typed in the console
  resets to these on launch. Builds before that started at `dodsp_sun_baked 1` - a false alarm on 3b4.
- `dodsp_sun_baked` is the player's CHOICE only; code never sets it (lint). Where the per-pixel sun can't
  draw (no light_environment, map compiled without `-dodsp_sunstyle`, sun shaders off), the client sets
  the server's `dodsp_sun_fallback 1` and the baked sun shows on that map only. The old watchdog set
  `dodsp_sun_baked 1` itself, which would have stuck for every later map once 0 became the default. Maps
  without a separate sun also never get the per-pixel sun on top (it used to add a second sun there).
  The fallback lives in server.dll: client.dll and server.dll must come from one build.
- `dodsp_sunshader` is read once per game start. Changing it mid-game does nothing until a restart.

## Owner-side pitfalls seen
- Windows hides extensions: a rename to `x.dll` became `x.dll.dll`.
- Shader updates need BOTH `bin\game_shader_dx6.dll` and `shaders\fxc\dodsp_*.vcs` from one build.
- A setting changed after the map loaded invalidated a test once. Restart for start-up settings.

## Stage 4 (models) - files and undo
- Shader side: `dodsp_modelsun_vs_fxc.h`, `dodsp_modelsun_ps_fxc.h`, `dodsp_sun_common_fxc.h` (shared
  with the brushes), `dodsp_vertexlit_and_unlit_generic{,_bump}_{vs,ps}30.fxc`, `dodsp_skin_{vs,ps}30.fxc`
  (generated once from Mapbase's by a script, then kept by hand).
- DLL side: `dodsp/dodsp_vertexlitgeneric.cpp` (DODSP_VertexLitGeneric), `dodsp_vertexlitgeneric_helper.cpp`,
  `dodsp_skin_helper.cpp`, model parts of `dodsp_sun_shader.cpp/.h`. Client: swap table in
  `c_dodsp_sdkshaders.cpp`.
- Undo: put the 3b4 DLL and .vcs files back (or `dodsp_sunshader 0` + restart: everything stays on
  Mapbase's shaders).

## Lighting roadmap (one stage at a time)
- Stages 1-3 done (3b4). Stage 4 (models' per-pixel sun) confirmed 2026-11-17 with the settled-prop fix
  (8a582ff, build 44157620634): props are sunlit from map load, no bumping needed.
- **Stage 4d - static props take the per-pixel sun** (see Engine facts). CONFIRMED in game 2026-11-17
  (build 44168681578): the wall lamp's shadow lands on the quoin props. Built together with
  the 4b INTZ probe (d21896b); needs the new VRAD (maptools artifact) + a recompile WITH `-StaticPropLighting`.
  Files: vrad.cpp, vradstaticprops.cpp, vraddetailprops.cpp, dodsp_lightstyles.h, dodsp_shaders.h,
  dodsp_sun_shader.cpp/.h, dodsp_suncascadedebug.cpp, dodsp_modelsun_vs_fxc.h, c_dodsp_sun_cascades.cpp.
  Undo: the previous VRAD + recompile (props get the baked sun again), previous shader DLL/.vcs +
  client.dll (all are "4d" now - a mismatch prints the dodsp_shaders_version warning).
- **4b INTZ probe** (`dodsp_csm_probe_intz 1`, read at map load, then `dodsp_csm_view 1`): the near
  cascade is also drawn into an INTZ map and shown raw as a 4th (left) panel. Same picture + ring colour
  as the near panel = readable depth works on the 2014 engine. Console line `DOD-SP INTZ probe: ...`.
  Never used by the game's lighting. **RESULT 2026-11-17 (owner's NVIDIA card): PASS** - `made 2048x2048,
  format 32 (INTZ = 32, cascades use 30)`, the raw-depth panel matched the near cascade picture and ring
  colours exactly (sunlit and shadowed aim points). So 4b can move the cascades to INTZ and read raw depth
  on NVIDIA (the blocker search), with the compare done in the shader like the ATI path.
- **Stage 4b - soft sun shadows (contact hardening, PCSS)**, agreed 2026-11-17. BUILT 2026-11-17 (see
  Engine facts "Soft shadows"; the INTZ probe code was removed - the cascades are INTZ now). Files:
  dodsp_sun_common_fxc.h, dodsp_sun_fxc.h, dodsp_modelsun_{vs,ps}_fxc.h, the four sun PS .fxc + .inc
  (combo removed), dodsp_sun_shader.cpp/.h, the three helpers, c_dodsp_sun_cascades.cpp, dodsp_shaders.h
  ("4b"), dodsp_build_shaders.py. Undo: the 4d shader DLL/.vcs + client.dll (build 44168681578).
  TESTED 2026-11-17 (build 44176044660): works; owner's best look is `dodsp_sun_softness 0.3` with the 2-degree
  default (= 0.6 degree sun radius). Softer than that "really degrades quality": 16 taps spread over a wider
  radius with a per-texel random turn = grain. Next: 4b-2 smooth taps (bilinear 2x2 compares) instead of
  more noise, and 0.6 degrees as the default.
  **4b-2 REJECTED 2026-11-17 (build 44179877884), reverted.** It picked the filter's cascade per pixel from the
  penumbra estimate (wide penumbra -> coarser cascade) and searched blockers in two cascades. Neighbouring
  pixels then switched between cascades whose shadows are rasterized differently -> blocky patches and
  saw-tooth fringes at contact points and under the window light. Lesson: never let the filter's cascade
  change with the penumbra estimate; one pixel's shadow comes from ONE cascade (the finest that holds it).
  Known-good = 4b (build 44176044660) with `dodsp_sun_softness 0.3` (0.6-degree sun). Replaces the saved 3x3 PCF
  fix (`dodsp_sun_filter 3` made the saw-tooth worse). Per pixel: blocker search in the cascade ->
  penumbra width = (receiver - blocker depth) x sun angular size -> ~16-tap rotated Poisson PCF.
  Sharp at contact, soft far from the caster, no stair-steps. The sun's size has ONE owner: the
  light_environment's SunSpreadAngle (what VRAD uses for the baked soft sun), plus a `dodsp_` multiplier
  for art direction. Blur width in world units, so cascade seams match. Risk to check FIRST: the
  NVIDIA hardware-compare path can't return raw depth for the blocker search -> cascades need a
  readable depth format. Long shader build (~40 min): warn first. Pixel constants: stay inside c0..c31.
- **Stage 4c - blob shadows become contact shadows** (client only, ~12 min build). The engine's
  render-to-texture prop shadows point along `shadow_control` and look like holes in shade, and darken
  twice under props in sunlit patches. While the per-pixel sun draws, `dodsp_sun_propshadows`: 0 = none (default:
  each shadow switched off in UpdateShadow, none rendered), 1 = contact shadows (straight down,
  `dodsp_contactshadow_dist` (30) units below the object, `dodsp_contactshadow_strength` (0.5) of the map's
  darkness), 2 = the map's shadow_control. Without the per-pixel sun they are always the map's. BUILT 2026-11-17. How: clientshadowmgr.cpp only STORES the map's shadow_control direction/distance/
  colour; the getters (GetShadowDirection x3, GetShadowDistance x2, DoDSP_ShadowColor) apply the sun mode, so
  `dodsp_sun_baked 1` gives the map's values back with nothing to restore. The sun mode's one owner is
  `DoDSP_PerPixelSunDraws()` (c_dodsp_sun_cascades.cpp, = DODSP_SUNMODE_DYNAMIC as PublishSun last published).
  Guards: `dodsp_lint_client.py` (every line touching the stored values is tagged `dodsp-lint: map shadow value`,
  every ColorModulate `dodsp-lint: shadow colour`; the cvars live in one file) + PreRender watchdog
  (`INVARIANT: DOD-SP contact shadows: ... drifted`: material colour, and a real shadow's direction through the
  whole getter chain). History: the 2026-11-14 blob-shadow work (1f345a6 sun-only shadows, e04d691 shadows along
  the sun) was lost in the 3f606c6 revert; 4c is a different design (straight down, follows the sun mode).
  Files: clientshadowmgr.cpp, dodsp/c_dodsp_sun_cascades.cpp/.h, devtools/dodsp_lint_client.py.
  Undo: `dodsp_sun_propshadows 2`, or the 4b client.dll (build 44176044660).
  OWNER'S CALL 2026-11-17 (build 44187011599 tested): prop shadows OFF looks best with the per-pixel sun, so
  they default to off until SSAO (stage 6). SSAO does NOT use them (it works from the depth buffer); when it
  lands, "SSAO on" is one more condition in DoDSP_UpdateContactShadows.
  MISTAKE 2026-11-17 (build 44239160931): the first "off" default was `dodsp_contactshadows 0`, which meant
  "the map's shadow_control" - the stock blob shadows came back. A switch named for one feature can't also
  mean "none": the setting now names every choice (0 none / 1 contact / 2 the map's) and 0 is none.
  CONFIRMED 2026-11-17 (build 44241790052, client.dll + server.dll): the new defaults load on launch (per-pixel sun,
  softness 0.3, no prop shadows) and the sun fallback works. KNOWN-GOOD for everything after stage 4.
- Stage 5: deferred lamps (can reuse the 4b filter).
- Stage 6: extras. **SSAO** (screen-space ambient occlusion: contact darkening from what's on screen),
  and then blob shadows OFF entirely while the per-pixel sun draws (agreed 2026-11-17). Also cubemap
  normalization / specular occlusion (the owner's idea). **HDR** (owner: "later"): test and fix the
  DOD-SP shaders and sun intensity under `mat_hdr_level 2`; compiles then need both `-ldr` and `-hdr` passes.

## Open known issues (lighting)
- The door entity doesn't cast into the sun cascades.
- Black front steps = the HL2-materials issue above (fixed in the client build after 44117643276). The half-black CAFÉ sign is an overlay - the
  owner's to fix, not a shader problem. woodstair02 materials reverting.
- Cubemaps (owner: "fix later"): after a compile, before `buildcubemaps`, envmapped props show noise
  (the material points at cubemap images the compile reserved but nothing has filled). Idea: fall
  back to the default cubemap when a map's cubemaps aren't built, and say so at load.
- Cosmetic: after a `map` reload in the same session the load lines say `0 materials moved` / `0 world
  materials on DOD-SP's sun shaders` (materials were already swapped on the first load). The sun still
  draws (no INVARIANT, per-pixel sun on). The report should count materials already on DODSP_ shaders.
- Pending checks: FPS with `dodsp_sun_baked` 1 vs 0 (and again after 4b's extra taps).
- Props in the shade look darker than the room around them ("black holes", owner 2026-11-17; owner: if props
  take the ambient light around them they won't look pasted on without prop shadows, and SSAO perfects it).
  Already in place: the leaf ambient cubes keep the sun's bounce (stage 1, leaf_ambient_lighting.cpp) and so
  does the static-prop bake (4d). Next step is to measure, not guess: what ambient cube the engine hands a dark
  prop vs the lightmap of the wall/floor next to it. MEASURE: `dodsp_prop_light` (c_dodsp_proplight.cpp, aim at a
  moving/physics prop in the SHADE): one line with the cube facing the viewer + lamps, cube up/down/sides, the
  floor's lightmap under the prop, and prop/floor. ~1 = the engine hands the prop the room's light (look at the
  shader/material next); well below 1 = the baked ambient itself is dark (look at VRAD's ambient samples).
  RESULT 2026-11-17 (build 44253429405, cafe room, shade): chair2 facing 23 vs floor 26 (prop/floor 0.72), table1
  up 31 / down 64 / sides 48 vs floor 49 (0.45 facing the viewer from above - its top faces the dimmer ceiling side,
  its underside sees the sunlit floor). Screenshot pixels agree with those numbers once the materials' own
  darkness is counted (chair seat vs floor: 0.16 of the floor's brightness in the SUN, 0.075 in the shade = 0.16 x
  the cube's 0.56). VERDICT: nothing is lost in the engine or in our model shaders; the sun's bounce is in style 0
  (lightmap.cpp, AddSampleToPatch) as designed. The props look black because the room's shade is ~1/160 of the
  sun patch (floor, linear) and DoD's furniture textures are darker than the floor. Levers: brighter sky light
  through the openings (light_environment `_ambient`), more bounce, or exposure (HDR + auto exposure, stage 6).
  The command prints "?" for the shader: modelinfo->GetModelMaterials gives nothing for studio models - read
  the materials from the studio header when the file is next touched.
  VERDICT WITHDRAWN 2026-11-18. The owner doubled the sky ambient: walls/floor brightened, chair2 stayed near
  black (2.3 vs floor 7.0 on screen), while boxes and an UPTURNED chair in the same room were brighter than the
  floor, and chair2 in the sun draws a normal brown frame and grey seat. So its texture isn't dark and the light
  is lost for some props only. The first probe sampled at the box CENTRE, which is not where the engine samples
  a model's light (the model's $illumposition / illum attachment through its transform). Probe v2 prints the
  light at the engine's own origin (height above the floor, inside-solid flag) and every material's shader and
  path features (bump/phong/halflambert/lightwarp/selfillum/envmap). Lesson: measure at the point the engine
  uses, and check a prop in the SUN before blaming its texture.
  OWNER'S TEST 2026-11-18: sliding chair2 across the door's sun patch, the WHOLE chair (back, legs, parts in
  shade) turns warm and lit the moment its lighting origin (centre of the seat) is in the sun, and black the
  moment it leaves. Screenshots: shaded plates on the floor draw ~15-100x brighter than the shaded floor around
  them (linear). So the engine lights each model by what its ORIGIN sees, the sun included - a second sun on
  models that "one sun on models" was meant to prevent, gated per model by one point. Probe v2 prints ambient
  and lamps at that origin separately so the sun's way in (a light, or folded into the ambient cube) shows.
  PROBE v2 RESULTS 2026-11-18 (build 44256478924):
  - chair2 in the sun outdoors: own point = box centre, ambient 148 + lamps 240. "lamps 240" = the ENGINE's sun,
    still handed to models at full strength although style 31 is off; our shader drops it (one sun on models).
  - chair2 in the shade (the black one): own point fine (21 units up, open), ambient 23 vs floor 20 (1.41). On
    screen: back bar 0.33 linear = floor 0.31, SEAT 0.028 = 10x darker, though in the sun the seat is as bright
    as the floor. One material (furniture_chair01a, DODSP_VertexLitGeneric, envmap). OPEN: something kills the
    seat's ambient only - next check is the same chair on Mapbase's own shaders (+dodsp_sunshader 0).
  - plate models/ww2/ro1-dish.mdl (phong): its own point is INSIDE SOLID ~378 units away, the engine reads
    ambient 169 there vs 43 at its middle -> the glowing plates. FIX (c_dodsp_lightorigin.cpp): every model is
    lit from a point inside its own bounds (+8 units) in open space; else from its middle, lifted out of
    solid. One owner, called from C_BaseAnimating::InternalDrawModel (C_PhysicsProp's OnInternalDrawModel
    never calls the base, so the hook can't live there). A mapper's LightingOrigin wins. Console names each
    corrected model once per map; INVARIANT if no open point exists. Lint: every `->pLightingOrigin =` /
    `.pLightingOrigin =` carries `dodsp-lint: lighting origin`. `dodsp_lightorigin_fix 0` = engine's points.
    CONFIRMED 2026-11-18 (build 44258391671): plate now lit from its middle (ambient 49 vs 166 at its own point,
    floor 55) - "blends into the scene really well" (owner). Its own point was outside the model, ~377 units away.
  - chair2 black in the shade is NOT our shader: same with +dodsp_sunshader 0 (Mapbase's shaders). mat_fullbright 1:
    the seat texture is light grey like the floor. Light received (lit/fullbright, linear): floor 0.003, the RIGHT
    chair's seat 0.0024 (fine), the LEFT/corner chair's seat 0.0001, table top 0.0005. Same model, same texture -
    it depends on where the prop stands (both black chairs stood in corners against the wall), yet
    engine->ComputeLighting at the chair's own point reads normal (23 vs floor 20). So the engine DRAWS it with
    less light than ComputeLighting reports there. OPEN; next test: push a black chair to the middle of the room.
  - 2026-11-18, owner: "nearly every model is like this". What's black are the surfaces FACING UP (chair seats,
    table tops, the cafe_bar1 counter top), on moving props AND static props (cafe_bar1 is a prop_static with VRAD
    vertex lighting - its LightingOrigin key is ignored for that, so moving the info_lighting changed nothing).
    Sides of the same models show colour; up-facing surfaces are fine in the sun. mat_ambient_light_r/g/b don't
    exist in this engine. table1 alone is explained (dark texture: fullbright 35 vs floor 95, top faces the
    dimmer ceiling side). Probe v3: static props (the engine's own light at the aimed spot + the floor where
    you stand, with material colours), and `dodsp_prop_materials` writes every shader param of the prop's
    materials to DOD-SP\dodsp_prop_materials.txt (compare a black prop with a good one, e.g. the boxes).
- Confirmed 2026-11-17: `buildcubemaps` no longer crashes with the sun shaders loaded (3b4).
