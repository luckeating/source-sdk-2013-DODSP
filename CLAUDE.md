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
- `MaterialSystem_Config_t` (GetCurrentConfigForVideoCard) needs `materialsystem/materialsystem_config.h` (C2027, 44293835742).
- A name table for an enum (`s_pszNames[]` next to `enum Invariant_t`) gets `COMPILE_TIME_ASSERT( ARRAYSIZE( names )
  == NUM_... )`: the soldier's INVARIANT names were two short, so INV_PINNED_STUCK / INV_MG_MOVING printed whatever
  memory followed (found 2026-11-18, C1).
- `ISoundEmitterSystemBase::GetWaveName` takes a NON-const `CUtlSymbol&`: copy `SoundFile::symbol` into a local first (C2664, 44667501038).
- `IMaterialSystemHardwareConfig` has no GetShadowSlopeScaleDepthBias/GetShadowDepthBias in this SDK (only in env_projectedtexture's
  dead non-ASW branch): read Mapbase's `mat_slopescaledepthbias_shadowmap` / `mat_depthbias_shadowmap` (C2039, 44341665956).
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
- **DOD-SP is played and lit in HDR (`mat_hdr_level 2`), owner's call 2026-11-18 ("far superior").** In LDR the
  engine's model lighting draws props in the shade ~10x too dark (see the prop-darkness log), so LDR is not supported.
  Every test before 2026-11-18 was LDR. Compile with `-both` (an `-ldr`-only map has no HDR lighting and draws in
  LDR; a `-hdr`-only map has no LDR lighting and draws fullbright in LDR, 2026-11-17). One owner for "is this map
  drawn in HDR": `DoDSP_DrawsHDR()` (c_dodsp_sun_cascades.cpp; lint OWNERS `MapHasHDRLighting`); the console says
  `DOD-SP HDR: on` at map load, or warns why not (HDR off / map without HDR lighting) and what to type.
  Owner's VRAD line: `-game $gamedir -both -final -dodsp_sunstyle -StaticPropLighting -StaticPropPolys
  -textureshadows $path\$file` (Hammer's $light_exe is our Mapbase vrad in the SDK 2013 SP bin).
  Still to check in HDR: sun brightness vs the baked sun, fog, cubemaps (build them in HDR), exposure
  (env_tonemap_controller).
- **Detail props: limit 512K (was 64K), 2026-11-19.** One owner: `DODSP_MAX_DETAIL_PROPS` (public/dodsp_detailprops.h),
  used by VBSP and the client. The BSP format never had a 16-bit limit (the lump count is an int, each prop stores only
  its 16-bit LEAF); the 64K was the CLIENT's: `CClientLeafSystem::ClientLeaf_t::m_FirstDetailProp/m_DetailPropCount`
  were unsigned short, and `CDetailModel::gm_LightStylesMap` used CUtlMap's default unsigned short index (one entry per
  prop lit by a light style = every sunlit prop with `-dodsp_sunstyle`). Both are int now, with COMPILE_TIME_ASSERTs.
  Lesson: CUtlMap/CUtlRBTree default to an unsigned short index - give any map that can hold one entry per world
  object an explicit `int`. VBSP now skips (warns) over the limit for sprites too (stock: fatal Error for sprites,
  warning for models) and prints `DOD-SP detail props: N placed (limit ..)`; the client prints `DOD-SP detail props: N`
  at map load, INVARIANT if over the limit. A map over 65535 needs DOD-SP's vbsp AND client.dll (a stock client draws
  the wrong leaves' props). Lump version unchanged (4): VRAD needed no change.
  FIRST TEST 2026-11-19 (build 44642676300, gm_countrysidedrive_d, a ~32K-unit map): client loaded exactly 524288 = the
  map wanted MORE, and stock-style overflow fills faces in order -> the last faces get none (bare patches). VBSP now
  thins EVENLY: the surface pass runs again with every density x (budget / wanted) (placement is seeded per face, so
  it's the same pattern, sparser); hand-placed detail_prop/prop_detail_sprite are kept first and never thinned. Line:
  `DOD-SP detail props: N placed (H hand-placed; the surfaces wanted W; limit ..)`. Density is props per million
  square units (VBSP: area x density x 1e-6), so a huge map x a dense detail.vbsp = millions. Raising the limit
  further costs ~52 bytes/prop in the .bsp, ~55 in client memory (32-bit process) and VRAD time (detail prop lighting
  runs on ONE thread, ComputeDetailPropLighting). OWNER 2026-11-19: 512K is plenty - his own maps won't need more (the
  32K-unit map is a vehicle test bed only). Don't raise it further unless he asks.
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
- Stage 5: `dodsp_lamps_baked 0` (live lamps drawn live), `dodsp_lamps_scale 1`, `dodsp_lamps_shadows 1` reset too.
- Vehicles: `dodsp_vehicle_road_slack 160`, `dodsp_vehicle_debug 0`, `dodsp_vehicle_he_scale 0.5` reset too.
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
- **Stage 5 - live lamps, HAND-PLACED, FORWARD (owner's call 2026-11-18, final).** The mapper marks a `light_spot`
  as a live lamp (key `_dodsp_live 1`, in dodsp.fgd); everything about the lamp (colour, brightness, cone, falloff)
  has ONE owner: that light entity. VRAD bakes only its BOUNCE; its direct light goes to a light style the game
  switches off while the lamp draws live (same idea as the sun's style 31; fallback = baked). The game draws each
  live lamp the engine's forward way - a projected texture (Mapbase's env_projectedtexture path, shadowed, an
  additive pass per lit surface, already in our shaders' flashlight variants). Forward keeps MSAA. Plan: 5-1 VRAD
  style + server/client lamp + FGD key (no shader build); 5-2 soft (PCSS) lamp shadows in the flashlight passes,
  raw-depth lamp shadow maps (shader build); 5-3 cost: shadow maps re-rendered only when something moved, budget
  (Mapbase: 5 shadowed at once, `-numshadowtextures`). Point lights (6 faces) only if a map needs them.
  **REJECTED: deferred lamps (5a, every light live).** Built and probed 2026-11-18 (builds 44287491699, 44296304491):
  the shaders wrote albedo/normal/distance to render targets 1..3 in the main opaque pass. D3D9 MRT needs every
  bound target to share render target 0's bit depth (the owner's HDR is INTEGER: 8-bit screen, not float) AND its
  multisampling - so MSAA had to go (the owner won't take FXAA; SMAA or a second geometry pass were the only ways
  around). Deferred + MSAA properly needs per-sample reads (D3D10.1); averaging G-buffer samples invents surfaces
  -> halos. Lessons kept: integer HDR on PC (`GetHDRType() == HDR_TYPE_INTEGER`), MRT bit-depth/multisample rules,
  a header the whole shader needs goes outside every #if (5a's first build failed X3000 on flashlight variants).
  If SSAO (stage 6) wants normals/depth, it meets the same MSAA rule: plan a separate pass. The 5a client code is
  removed in 5-1 (nothing binds targets 1..3, so the shaders' extra outputs are dropped); the shader outputs and
  check_gbuffer go with 5-2's shader build.
  **5-1 BUILT 2026-11-18.** VRAD (lightmap.cpp): `"_dodsp_live" "1"` on a light_spot (key added by hand, SmartEdit off -
  no FGD shipped yet) -> style `DODSP_LIGHTSTYLE_LAMPS` (30), INVARIANT style 30 <=> live; point lights (180-degree
  cones) stay baked with a warning. Bounce stays in style 0 for every style `DoDSP_StyleBounceStays` (sun, lamps):
  BuildPatchLights, leaf ambient, static-prop indirect. Live lamps are NOT in the world-lights lump (the engine
  ignores styles when lighting models from it - the sun's worldlight lit models at full strength with style 31 off -
  so they'd light every model twice); VRAD writes them per pass to the pak: `dodsp/lamps_hdr.txt` / `_ldr.txt`
  (KeyValues: origin, normal, intensity /255, attn c l q, cone stopdot stopdot2 exponent, radius, fade). Server
  (world.cpp): `dodsp_lamps_baked` (player, default 0) + `dodsp_lamps_fallback` (client-written) -> style 30.
  Client (c_dodsp_lamps.cpp, one owner): one engine flashlight per lamp (CreateFlashlight + UpdateProjectedTexture
  every frame), cookie = VRAD's cone formula per texel (procedural, stored sRGB-encoded because the flashlight
  sampler reads sRGB), attenuation VRAD 1/(c+ld+qd^2) -> shader c'+l'/d+q'/d^2 (exact for one term, approximated
  for mixed - `dodsp_lamps` says which), colour = intensity / dodsp_r_flashlightbrightness (0.25) x
  `dodsp_lamps_scale`. Range = _distance / hard-falloff end / where the light drops below `dodsp_lamps_cutoff`, cap
  `dodsp_lamps_maxrange`. The shader's flashlight fades the light from 60% to 100% of FarZ and its SHADOW with the
  attenuation unless m_flShadowAtten = 0 (set). Known gaps: in fallback (baked) models get no lamp direct light
  (it isn't in the world lights); more shadowed lamps in view than shadow maps (5) -> the rest draw unshadowed
  ("Too many shadow maps this frame!") until 5-3. Files: dodsp_lightstyles.h, vrad.h, lightmap.cpp,
  leaf_ambient_lighting.cpp, vraddetailprops.cpp, world.cpp, c_dodsp_lamps.cpp (c_dodsp_lamps.h removed),
  viewrender.cpp (5a hooks removed), dodsp_lint_client.py. Undo: `dodsp_lamps_baked 1` (the baked lamps; no rebuild),
  or the previous VRAD + recompile (the lamp is an ordinary baked light again).
  FIRST TEST 2026-11-18 (build 44344800913, one live hall lamp, cone 90/90): WORKS - drawn live, the chair casts a
  real shadow. `dodsp_lamps`: `colour 7843 ... distance falloff exact`. Two faults, fixed after: (1) live was 3.07x
  the baked lamp (floor 3.05 / 3.09 unclipped, unlit areas 1.00 = a constant units factor) -> DODSP_LAMP_BAKE_MATCH
  = 1/3.07 (measured, not derived; re-measure on a second lamp); (2) the cone edge drew as stair steps (128-texel
  cookie, hard edge) -> 512 texels, each the average of 4x4 points.
  CONFIRMED 2026-11-18 (build 44348802435): smooth cone edge, live brightness in the baked range (floor 0.060-0.084
  linear vs baked 0.076, camera moved slightly), chair shadow sharp - owner: "holy shit that is crispy". KNOWN-GOOD
  for stage 5-1. Still to confirm the 1/3.07 on a second lamp with another brightness/falloff.
  OWNER'S TEST 2026-11-18: 5 live lamps on one map, two shadows cast on one object, FPS fine - "serviceable for now".
  Lamps parked here (5-2 soft shadows, 5-3 budget) while NPC work goes on.
- Stage 6: extras. **SSAO** (screen-space ambient occlusion: contact darkening from what's on screen),
  and then blob shadows OFF entirely while the per-pixel sun draws (agreed 2026-11-17). Also cubemap
  normalization / specular occlusion (the owner's idea). HDR moved up: it is the standard since 2026-11-18.

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
  - PROBE v3 RESULT 2026-11-18 (build 44262556220): cafe_bar1 (static) counter TOP: the engine's own light there 29
    vs the floor where the owner stood 9 (12x in linear), material colour 128 vs floor 95 - so the top should draw
    ~16x BRIGHTER than that floor; on screen it draws 0.065 vs floor 0.24-0.74 (linear). cafe_cabinet1 (prop_dynamic,
    no envmap): engine ambient 49 vs floor 33, draws near black. ~100x of light lost between the engine's lighting
    data and the pixels, on static AND moving props, on our shaders AND Mapbase's, worst on the darkest (up-facing)
    surfaces, fine in the sun and outdoors (bright light) and in fullbright. Darks crushed, brights fine = the shape
    of an extra gamma/linear conversion on model lighting. Every test so far was LDR (mat_hdr_level 0, -ldr
    compiles). Next test: compile -both, run mat_hdr_level 2 - if props light right in HDR, the LDR model lighting
    path is at fault (engine/shared shader headers), not our sun work.
  - HDR TEST RESULT 2026-11-18 (build 44262556220, VRAD `-both`, `mat_hdr_level 2`, map restarted): props in the
    shade now draw level with the world next to them (linear, from screenshots: counter top 0.0013-0.0020 vs wall
    panels/floor 0.0021-0.0023, corner chair seat 0.0009 vs floor 0.0011, chair seats 0.003-0.004 vs floor 0.001-0.002;
    in LDR the same surfaces were ~10x darker than the floor), cabinet shelves readable. CONFIRMED: the dark props are
    the LDR model-lighting path, not our sun work, not the lighting points, not VRAD's ambient. The room as a whole
    is still dark in HDR (shade floor ~70x darker than the lit corridor): that is the map's sky/bounce light plus
    exposure, the same for brushes and props.
- Confirmed 2026-11-17: `buildcubemaps` no longer crashes with the sun shaders loaded (3b4).

## Vehicles roadmap (owner 2026-11-19: moving tanks that aim, infantry riding; ground armour first)
- Content: the owner's CoD2 conversions (vehicles.zip, artillery.zip, planes.zip; hobby only like the rest). MDL v48,
  checksums good, 1 LOD. EVERY model faces +X IN GAME (forward_yaw 0) - the live models' MESHES (.vvd) face -Y,
  but studiomdl's default 90-degree root rotation lives in their animation (MDL hull box is X-long); *_dead and CoD1
  models have no rotation. LESSON (V1 test): read directions from the MDL hull box, never the raw mesh. V1 shipped
  forward_yaw -90: Panzer II graded "256 units wide", turret offset sideways. Guard: Spawn INVARIANT when a vehicle
  is wider than long along forward_yaw. Hulls carry wheel bones (panzer2 14, sherman 16, tiger 20, crusader 14, armoredcar 4); turrets: bones
  turret+barrel, pose `barrel_pitch` -15..30, attachment `muzzle`, no .phy. Hulls have NO turret attachment: turret
  offsets are ESTIMATES in the script (deck height + turret-ring hole; tiger fitted against its wreck: 0 -11 108).
  Scale ~1.1-1.3x real (owner to check against a player). Textures not in the zips (materials/models/cod2/...).
- Owner's choices: per-vehicle SCRIPT file for seats/mounts (not QC attachments), FREE AI driving (not path_track),
  riders tank 4 / halftrack 8.
- Stages: V1 drive, V2 aim + fire, V3 riders (seats in the script already), V4 damage (dead models).
- **V1 BUILT 2026-11-19.** server dodsp/dodsp_vehicle.cpp (`dodsp_vehicle`, keys vehicletype/goal, inputs DriveTo/
  Stop/SetMaxSpeed, outputs OnArrived/OnStuck/OnNoRoute), client dodsp/c_dodsp_vehicle.cpp (wheel bones turned in
  BuildTransformations, material proxy `DoDSPTrackScroll`; one distance owner for both), content
  dodsp_tools/content/scripts/dodsp_vehicles.txt + dodsp_vehicles.fgd. Size from the model. Navigation: the soldiers'
  AI node graph, each link graded per vehicle half-width by CorridorClear (ground every 32 units, no step > 36,
  normal z >= 0.6, three lines x two heights + a cross line clear) - the ONE fit test, also used to straighten routes;
  load line `DOD-SP vehicles: <type> (<w> units wide) can drive N of M node links`. A* skipping links it got stuck on
  (30 s refusal memory). Movement: MOVETYPE_PUSH + vphysics shadow (the engine pushes people/props, Blocked() when it
  can't); pushers go through the world, so DriveThink sweeps its own collision model ahead (UTIL_TraceEntity) and
  stops for walls. Orientation from four corner ground traces (BuildAngles maps model axes -> heading/ground).
  Tracked: pivots when the turn is > 50 degrees; wheeled: yaw rate <= speed / min_turn_radius. Watchdog INVARIANT:
  wants to move, < 8 units and < 10 degrees in 3 s -> stuck (world / entity / no-progress / no-route by reason code),
  plan again. Console: dodsp_vehicle_goto [all], dodsp_vehicles, dodsp_vehicle_turret x y z, dodsp_vehicle_reload,
  dodsp_vehicle_debug 1 (route lines). Undo: remove the entities (nothing else uses the code).
  FIRST TEST 2026-11-19 (build 44524896164, carentan_south_1a, 4 nodes): tank spawned, turret on, but "(256 units
  wide) can drive 0 of 0 node links" and goto -> no-route. Causes: (1) forward_yaw -90 (see Content) -> width read
  along the length; (2) grading ran in Activate, BEFORE the engine built the node links ("Node Graph out of Date.
  Rebuilding" came after). V1b: forward_yaw 0 + turret/seat positions turned into the in-game frame; grading 1.5 s
  after activation and cached per map + node count + LINK count; INVARIANT when nodes exist but no links. The
  converted tread materials name proxy "COD2TreadScroll": registered as a second name of DoDSPTrackScroll. Wheel
  spinner skips bones below 4 units (CoD's ground tags). The owner's shader DLL was build 5a vs client 5a2.
- **V1b TEST 2026-11-19 (build 44642676300, gm_countrysidedrive_d, forest roads, 672 nodes): WORKS** - `panzer2 (128 units
  wide) can drive 1676 of 1678 node links`, graded in 0.09 s; drove and arrived twice ("moves pretty well"). Fault: a
  long straightened leg stopped with `wall ahead (worldspawn)` + stuck/world on terrain the route was graded clear on.
  V1c: ONE rule for drivable ground, `VehicleDrivable(normal z >= VEHICLE_MIN_GROUND_NZ 0.6)`, used by CorridorClear AND
  the drive sweep: a sweep hit on drivable ground is climbed, not a wall (and the sweep looks again 48 units higher, so
  a wall behind the slope is still found). Guard: when the sweep stops it for a wall where CorridorClear says the next
  stretch is clear, `INVARIANT ... the sweep found a wall (...) where CorridorClear says it can drive` with the hit's
  normal/height/position. Known gap: CorridorClear samples GROUND on the centre line only (the sides are checked by
  lines at 24/64 units up), so a steep bump under one track is found only by the sweep - the INVARIANT shows it.
  V1c TESTED 2026-11-19 (build 44648397962, same map): "working pretty well" - drives the forest road, arrives, sits on
  the ground right. Note: route straightening (CorridorClear) cuts across open ground on the inside of road bends - it
  is drivable, so allowed; a "keep to the road" mode is an option for later, if the owner wants it.
  SECOND V1c RUN (same build): (1) `INVARIANT ... the sweep found a wall (worldspawn, normal z -0.15 ...) where CorridorClear
  says it can drive` - the guard worked: a straightened leg passed beside a TREE (static props come back as worldspawn)
  that the old thin-line test missed (3 lines 64 apart at 24/64 units up; ground on the centre line only). (2) owner:
  "it seems to skip most of the nodes" - straightening cut across country between road nodes.
  V1d: CorridorClear tests the BODY: VEHICLE_BODY_COLUMN (16) boxes side by side across the width, from step height to
  the model's top (m_flBodyHeight), every 32 units, each column on its own ground with the step/slope rule (closes the
  one-track gap). What blocks has ONE owner, `VehicleSweepBlocked` (drivable ground is climbed + re-check 48 higher),
  used by the boxes AND the drive sweep of the real model. Grading is keyed by width AND height. Routes keep to the
  nodes (the mapper's roads): straight to the goal only within VEHICLE_DIRECT_DIST (768) or with no reachable node;
  a shortcut may skip a node only if it passes it within VEHICLE_ROAD_SLACK (48). Hits on static props print
  "static prop". Also fixed: the body sizes weren't saved, so after a save load they were 0 - MeasureBody() runs on
  Spawn AND OnRestore (INVARIANT in DriveThink if a body is ever unmeasured).
  V1d TEST 2026-11-19 (build 44651972586): routes follow the road. Fault: off-road start, the leg onto the road crossed a
  small terrain berm; the drive sweep of the RIGID model hit it (`normal z 0.00, swept 24 units up` x4 -> stuck/world)
  although the plan had passed it. Lesson: hull/model traces against DISPLACEMENTS return flat normals (0.00) at edges,
  so "is this hit climbable ground?" can't be read from a sweep's normal; and a rigid 230-unit body lifted 24 units
  catches a berm its tracks would climb. V1e: ONE test for planning AND driving - every think, CorridorClear runs over
  the stretch the front of the body covers next (half-length + 2 thinks + 16 units, along the heading, reversing too);
  the model sweep and the disagreement INVARIANT are gone (they can't disagree by construction). CorridorClear says
  WHY with reason codes (CorridorFail_t: no ground / too steep / step too high / blocked + what), printed with
  dodsp_vehicle_debug: `can't go on - <reason>, <what> at x y z`. The first sample never fails (the vehicle or node is
  standing there). Known gap: a pivoting tracked vehicle's rear isn't checked (pushers clip through it).
  V1e: owner 2026-11-19 "now the node usage is a little too tight, blend the shortcut with the node usage" (route hooked
  back to a node before the goal). The slack is now `dodsp_vehicle_road_slack` (default 160; 0 = every node), so the
  blend can be tuned live: a shortcut may skip nodes that lie within the slack of it, and only where the body fits.
  Owner, same build: "rotated all the way around to line up with a node, to then turn all the way back" - the route
  always joined the road at the NEAREST node, even one behind the tank (and left it at the node nearest the goal ->
  hooks). V1f: NodeCandidates (one owner for "which nodes join a spot to the road", NearestNode = its first answer)
  gives VEHICLE_JOIN_NODES (6) start and end nodes; the A* starts from all of them (cost = drive there + TurnCost:
  VEHICLE_TURN_COST 512 units for a full about-turn, 0 straight ahead) and ends at whichever end node makes the
  cheapest whole drive (+ the distance on to the goal); it stops once nothing left can beat the best found.
  OWNER 2026-11-19 (build 44663089277): "good enough for now" - V1 (driving) parked as KNOWN-GOOD; slack default 160 kept.
- **V2 BUILT 2026-11-19 (aim + fire).** Owner: targets = "anyone hostile to the faction" (Axis vs Allies - American,
  British, Russian all one side), main gun "super deadly". FACTIONS: one owner, dodsp/dodsp_factions.cpp
  (`DoDSP_FactionOf` maps AI classes -> side: player/ally/rebel = allies, CLASS_COMBINE (npc_german_soldier)/metropolice/
  military = axis, anything else none = never a target; `DoDSP_ClassForFaction` = what a side's entity answers to
  Classify). A nation is NOT a side (chatter us/uk/ru = which words; faction = who gets shot). Vehicles: type key
  "faction", Hammer key "faction" wins; Spawn INVARIANT if Classify doesn't map back to its side. TURRET: measured from
  the model at first use (CalibrateTurret: barrel direction at yaw 0 from the muzzle attachment, which sign of
  `barrel_pitch` raises the muzzle, pose range) - one load line `turret barrel faces N deg, barrel_pitch a..b (+/- = up),
  muzzle found; main gun yes, coax yes`, INVARIANT for no muzzle / no pose / a pose that doesn't move the muzzle.
  Aims in the HULL's frame (works on slopes); turret_rate / pitch_rate per type. Fires only along where the barrel
  REALLY points (within 1.5 deg main / 5 deg coax), never "at" the target. Holds fire for a friend on the line or
  within the shell's radius of the impact. Main gun = trace + ExplosionCreate (damage falls off over radius, silent) +
  our explode sound + screen shake; coax = FireBullets bursts at infantry only. Target: nearest hostile in range and
  sight (players, every AI, every vehicle), kept while valid; notarget respected; inputs SetTarget/ClearTarget/HoldFire/
  OpenFire, outputs OnTargetSeen/OnFire. SOUNDS: owner's tanksounds.zip (15 waves, 44.1 kHz mono, loops have cue points)
  -> DOD-SP\sound\dodsp\vehicles\, entries in dodsp_tools/content/scripts/game_sounds_dodsp_vehicles.txt (loaded by
  the game like the chatter's); engine idle fades into move with speed (CSoundPatch), turret turn loop + stop clunk,
  reload clank 2 s before the gun is ready. Coax uses DoD:S's Weapon_Mg42.Shoot / Weapon_30cal.Shoot (names NOT
  verified - the load line says if they're missing). Guards: map load `DOD-SP vehicle sounds: all N found` or ONE
  INVARIANT naming the first missing entry/wave; lint check_vehicle_sounds (every DoDSP_Vehicle.* name in the types
  exists in the sound script; the type file has balanced braces, ONE top block, every type a model or base - a doubled
  header slipped in while writing it). Script: "base" inheritance (paint variants = only their models; whole blocks
  override). Engine-sound choice per vehicle is a guess (engine1 light/2 medium/3 heavy/truck wheels). NOT in V2:
  halftrack/carrier hull MGs, the SU-152 "hull aims" mode (needs its collision model first), damage to vehicles (V4).
  Files: dodsp_vehicle.cpp, dodsp_factions.cpp/.h (server_hl2.vpc), dodsp_vehicles.txt/.fgd, game_sounds_dodsp_vehicles.txt,
  dodsp_lint_client.py, dodsp_build.yml (content/scripts path). Undo: the V1f server.dll (build 44663089277) - the old
  type file still loads (unknown keys are ignored).
  TESTED 2026-11-19 (build 44669320376, gm_countrysidedrive_d, Panzer II): WORKS - owner: "damn this thing shreds", "all
  the sounds work and actually blend very very well together" (so DoD:S's Weapon_Mg42.Shoot is a real entry). KNOWN-GOOD
  for V2. Next: V2b (halftrack/carrier hull MGs, SU-152 hull-aims), V3 riders, V4 damage.
- **V2c BUILT 2026-11-19 (tactics, reversing, lurch).** Owner: vehicles hunt by DEFAULT; fighting style is a Hammer
  choice ("both - a hammer option to play defensively or offensively"); knowledge = own eyes + its side's soldiers
  (no wallhacks); "allowing it to go backwards". Key `behavior`: offensive (default: closes to 600..1800, shoots on the
  move with 3x spread, patrols random graded nodes 512..3000 away with 4-8 s pauses when no enemy is known),
  defensive (spots 1500..min(gun range x 0.8, 4000) away with a shot, fires only when stopped, keeps its front to the
  enemy = reverses to spots behind it, leash 2000 by default, returns home when no enemy), hold, scripted. Key `leash`.
  TacticsThink (1 Hz) is the ONE owner of where it wants to be: UpdateQuarry (turret's target, else the nearest enemy
  its side's AIs within 3000 are fighting at their GetEnemyLKP; forgotten after 30 s unseen/unreported) ->
  ChooseFirePosition (graded nodes in the band, in the leash, within 4000; best 24 by distance get a world-only line
  test from turret height) -> DriveTo(.., false). Map orders (DriveTo input, console goto, initial goal) set
  m_bScriptedGoal: tactics wait until it arrives. Reason codes (VehicleTactic_t / VehicleTacticWhy_t, name tables
  with COMPILE_TIME_ASSERT) printed on every change with dodsp_vehicle_debug, and in `dodsp_vehicles`. Guards: a new
  spot is kept >= 4 s (VEHICLE_SETTLE_TIME); 6 new spots in 30 s = `INVARIANT ... can't settle`. The turret pre-aims at
  the last known spot. REVERSE: a route point > 110 deg off the nose (sticky to 70) within 768 units - or anywhere
  when m_bPreferReverse (defensive with an enemy) - is driven to backwards at reverse_speed; TurnCost counts it so
  routes may start with a node behind. LURCH: the models have no animation for it (every hull: only 'idle'; turrets
  'aim'), so a damped spring on the hull pitch (driven by acceleration; nose up pulling away, down braking, overshoot)
  plus a recoil kick from the main gun (by turret yaw); type keys susp_accel / susp_freq / susp_damp / recoil.
  Files: dodsp_vehicle.cpp, dodsp_vehicles.fgd/.txt. Undo: `behavior scripted` on the entity (V2 behaviour), or the
  V2 server.dll (build 44669320376).
  TESTED 2026-11-19 (build 44673825456, 2 Panzer II + a Sherman): owner: "the lurching is actually extremely well done".
  Faults: (1) two tanks took the same path and drove into each other; (2) `INVARIANT ... 6 new firing spots in 21 s` -
  both tanks re-picked the spot each stood on every 4 s; (3) five shells into the same static prop; (4) the log named
  every vehicle "dodsp_vehicle". Causes: pushers overlap (MOVETYPE_PUSH doesn't block MOVETYPE_PUSH) and CorridorClear
  sees only the world; the spot search's trace was TRACE_WORLD_ONLY, which SKIPS STATIC PROPS (every tree), while the
  turret's sight hit them, and nothing tested the line from the muzzle.
- **V2d BUILT 2026-11-19 (vehicle space, one sight test).** VehicleInTheWay (one owner): a body of half-width w going
  a->b vs another vehicle's capsule (its length, its half-width) + VEHICLE_SPACE_MARGIN 24; a body already touching may
  always move away. Driving: every think, the stretch it covers before it could stop, toward where it WANTS to go
  (brakes, no turning). Planning (PathClear = CorridorClear + VehicleInTheWay): every leg and A* link avoids PARKED
  vehicles and the one it gives way to (10 s). GiveWay (right of way, VehicleGiveWay_t): parked -> plan round it after
  1 s (or "arrived" if it sits on the goal); head-on -> the higher entity index plans round; one ahead that stalled ->
  after 4 s; one driving on -> follow. No way round: a map order waits, a tactical goal is dropped. SpotTakenBy: a
  spot within both half-lengths + 48 of another vehicle's goal (or where it's parked) is taken - firing spots and patrol
  nodes skip it, DriveTo stops short of it on its own side. Waiting for a vehicle resets the stuck watchdog (it would
  mark road links bad). SIGHT: LineOfFire (CTraceFilterVehicleSight: world, brush entities, static props, vehicles; not
  people) is THE test for spots (from turret height), the turret's sight (aims at the point it sees: middle or eyes) and
  firing (from the MUZZLE: the shell must land within half its blast radius of the target, the coax must reach it;
  else `holding fire - no line from the muzzle ...`). A spot it reached without a shot isn't picked again for 20 s
  (BadSpot), and never the spot it stands on. Log names: `Label()` = targetname#entindex. Lint check_vehicle_sight: no
  CTraceFilterWorldOnly in dodsp_vehicle.cpp, every other UTIL_TraceLine tagged `dodsp-lint: not a sight line (...)`.
  LESSON: TRACE_WORLD_ONLY / CTraceFilterWorldOnly never sees static props - a forest map is all static props.
  Known gaps: a pivoting tracked vehicle's swing isn't checked against vehicles; two head-on on a one-lane road with no
  way round end with the tactical one dropping its goal. Undo: the V2c server.dll (build 44673825456).
  TESTED 2026-11-20 (build 44677367648): owner "v2d worked fine" - KNOWN-GOOD for V2d (vehicle space + one sight test).
  Owner expects more corrections once scenes and map orders get complex; re-test there before building on it.
- **V4 BUILT 2026-11-20 (armour, damage, crews, wrecks - REAL numbers).** Owner's calls: "fully real" armour/guns/fire
  rates, HE at HALF its real size against people (`dodsp_vehicle_he_scale` 0.5), damage to parts + crew with bail-outs,
  infantry AT later (DoD's bazooka/Panzerschreck - "DOD's weapons cover it"). Research (sources in dodsp_vehicles.txt's
  header): Panzer II F 35/30 front, 15 sides; KwK 30 10-round magazines ~280 rpm (sources disagree: 260 / 280 / 600
  cyclic), PzGr 40 40 mm@100 m / 20@500 (30-degree plate); Tiger 100/80/80/25, KwK 36 PzGr 39 162..116 mm flat at
  0.1..2 km; Sherman glacis 51@56 (91 eff.), sides 38, M61 66..51 mm (30 deg) at 457..1829 m, M48 HE 0.68 kg TNT; Crusader
  Mk III (6-pdr - from the barrel length, owner to confirm) 51 mm; Sd.Kfz. 222 14.5/8/5 open turret; Sd.Kfz. 251 14.5/8
  open; Universal Carrier 7-10 open; SU-152 75/60/20, ML-20S BR-540 125..90 mm. Penetration tables are FLAT-plate mm at
  metres (30-degree sources x 1.155). 1 m = 40 units (DODSP_UNITS_PER_METRE, one owner) - so a DoD map (a few thousand
  units) is 50-150 m: at game ranges almost everything gets through sides; fronts decide (Sherman 75 never through a
  Tiger's front, through its side only within 100 m; Panzer II 20 mm through a Sherman's side within 200 m only).
  Files: NEW dodsp/dodsp_vehicle_armour.cpp/.h (server_hl2.vpc) = the numbers and maths, one owner each: DoDSP_ShellPen
  (range interpolation), DoDSP_EffectiveArmour (mm / cos, plates leaned back by their slope), DoDSP_RayBox, HE by
  cube-root law anchored on the 75 mm M48 (0.68 kg: kills 10 m / wounds 25 m real; HE pen 35 mm x (kg/5.9)^(1/3)),
  DoDSP_BulletPen (rifle ball 6 mm, pistol 2), DoDSP_ShellBlast (THE shell explosion: fireball, sound, shake,
  RadiusDamage to people; g_DoDSPBlast tells vehicles in the blast its real charge, saved/restored for chain
  cook-offs). dodsp_vehicle.cpp: ShellHitBox (which plate: turret box cut at its own body - the model box reaches to
  the muzzle; hull box cut halfway into the turret), FindShellHit (shells find vehicles by their ARMOUR BOXES - the
  turrets have no collision model, the trace alone flies through them), TakeArmourHit (effective mm vs pen +-10%,
  ricochet past 70 deg unless calibre >= 3x plate, open tops have no top plate), BlastHit/ExternalBlast (nearest plate
  vs charge at distance; big HE outside breaks tracks/gun/turret, >= 4 kg can knock the crew out), ApplyPenetration
  (structure -= effect +-25%, spall casualties, crit table by part x face (VehicleCrit_t, engine front/rear/middle),
  ammo -> ammo_fire, engine -> immobile + fuel_fire), crew roles (commander/gunner/loader/driver/radio: no turret crew =
  can't aim, no gunner = 2x spread + 0.6x traverse, no loader = 2x reload, no driver AND no radio = immobile),
  CheckMorale -> BailOut (VehicleBail_t: fire / too few crew / immobile + can't fight / immobile + 2 penetrations in
  15 s; German crews spawn as npc_german_soldier with MP40s on the side away from the attacker, squad tankcrew<idx>;
  Allied crews just leave - no Allied NPC yet), states (VehicleState_t fighting / burning / abandoned / wrecked: out of
  the fight = CLASS_NONE, nobody shoots it), SetBurning (env_fire; ammunition: cook-off in 6-15 s; engine: burns out in
  30-60 s, 40% reaches the ammunition), Wreck (dead_model, static physics, still an obstacle in s_All, burns 40-90 s,
  cook-off blast VEHICLE_COOKOFF_KG 2.0). Guns: AP at vehicles / HE at the rest (ShellFor), magazine + cyclic +
  reload + rounds carried ("out of main-gun ammunition"), targets it can't hurt count 3x as far (CanHurt). Bullets go
  through TraceAttack (never the engine's health), blasts through OnTakeDamage. Outputs OnPenetrated/OnImmobilized/
  OnBailOut/OnKnockedOut. Console: `dodsp_vehicle_penchart` (every gun vs every plate, metres), `dodsp_vehicles` adds the
  damage line. Every penetration / immobilised / burning / bail-out / knock-out / wreck prints one line; non-penetrating
  hits only with dodsp_vehicle_debug.
  Guards: loader INVARIANTs (no armour = can't be damaged, bad pen table, gun with neither ap nor he, V2's flat
  damage/radius left in, magazine without cyclic, dead_model file missing); lint check_vehicle_types (armour, crew roles,
  dead_model per variant, pen pairs out in range / down in mm, no flat damage) and check_vehicle_damage (ONE writer
  set for m_flStructure / m_iCrewAlive / m_eState); name tables with COMPILE_TIME_ASSERTs. Estimates are marked "est." in
  the script (fire chances, structure values, some plates, Sherman rounds, 6-pdr HE filler, 88 HE ~0.9 kg).
  Undo: the V2d server.dll (build 44677367648) + the old scripts\dodsp_vehicles.txt (V2d ignores the new keys but the
  new script has no "damage"/"radius", so V2d's guns would fall back to 600/256 - keep the old script with it).
  FIRST V4 RUNS 2026-11-20 (build 44767081287, owner): (1) a tank knocked out while moving drove on at its speed through
  the ground and props forever - the vehicle moves by its pusher VELOCITY (SetAbsVelocity in DriveThink) and the
  out-of-the-fight path returned before DriveThink, so the last velocity stayed (and the fire, not parented, stayed
  behind); (2) the crew just appeared beside the tank - "I didn't even know a tank was downed"; (3) no wreck swap, no
  burning seen: half the knock-outs and every morale bail-out left an intact, abandoned tank (realistic, invisible).
  V4b: SetState is the ONE writer of m_eState (lint) and leaving the fight HALTS the vehicle (HaltMotion: velocity,
  angular velocity, speed, track speed); watchdog `INVARIANT ... but still moving` in DamageStateThink/WreckThink.
  Every way out of the fight ends as a burning wreck: knock-out -> burning (ammo: cook-off 5-10 s; engine: wreck at
  12-20 s, 40% cook off first), an abandoned vehicle catches fire 4-8 s later; the knock-out shows (a burst at the
  hatches + env_fire parented to the vehicle). Crews come out ONE AT A TIME (0.6-1 s apart, m_nBailPending) on the far
  side from the attacker and RUN from the vehicle (SCHED_FORCED_GO_RUN to a point 256 units away, ordered on the next
  think). Owner: CoD2 has crew bail-out animations - if he converts them for the German soldier model, the crew can
  climb out of the hatch instead.
  V4c (2026-11-20): the bail-out animations WERE in our own sources - CoD1 Assault.zip has `tank_climb_out` (41 frames,
  pelvis rises 8 -> 34 units: legs in the hatch -> standing on the roof) and `tank_jump_off` (40 frames, made IN PLACE:
  the code carries him down). LESSON: before saying an animation doesn't exist, grep the decompiles we already
  retargeted (scratchpad in_Assault / in_Ranger, ANIMATIONS.md) - half the German set came from them (owner: "you've done
  these retargets before just fine"). build_cod1_pack.py now takes `tank_` (c1_tank_climb_out / c1_tank_jump_off,
  ACT_WW2_TANK_*); the rebuilt pack differs from the delivered one ONLY by those two (every other SMD byte-identical).
  The owner recompiles npc_ww2_anims_cod1 with Crowbar. Soldier side (one owner, npc_german_soldier.cpp):
  SCHED_GERMAN_BAILOUT / TASK_GERMAN_BAILOUT - BeginBailOut puts him in the hatch (MOVETYPE_NONE, NODRAW until the climb
  starts), climb_out holds him at the hatch, jump_off carries him to the ground (hop + fall), EndBailOut = MOVETYPE_STEP
  on the ground, then SCHED_FORCED_GO_RUN away; killed mid-climb = he falls from there; a save loaded mid-climb puts
  him on the ground. Vehicle side: HatchFor (turret crew: turret roof a little behind its middle; driver/radio: hull roof
  at the front, by a trace onto the hull), one seat at a time from m_iBailMask, the far side from the attacker. A model
  without the sequences: `INVARIANT npc_german_soldier: ... has no c1_tank_climb_out` once per map, crews appear beside
  the tank as in V4b. Riders (V3): the owner's CoD2 officer model (cod2/npc/german_normandy_officer.mdl, 551 seqs) has
  ride1-6_idle, jump_ride1-6, germantruck guy1-8 jumpout + driver climbout, halftrack driver/guy jumpouts, bren carrier
  driver/passenger/back dismounts, kubelwagen, carchase get-outs; the CoD1 wehrmacht/driver2 models have truck_drive /
  truck_driver_out / truck_guy1-4 - all need a Crowbar decompile (SMDs) before the retarget.
- CoD1 set (owner's vehicles_part2.zip, 2026-11-19): REFERENCE ONLY except the **SU-152, which we keep**. One model per
  vehicle, turret/gun as BONES (tag_turret, tag_barrel, tag_flash = muzzle, tag_coaxel), faces +X (forward_yaw 0),
  CoD's rider tags tag_guy01..06 (Sherman, Elefant, SU-152: 6; Opel truck driver/passenger + 4), NO .phy (needs a
  $collisionmodel before it can drive - Claude can generate box collision SMDs + QC lines). Its tag_wheel_* markers sit
  at z 0 and contain "wheel": the client wheel spinner must skip bones at ground height before a CoD1 model drives.
  The Elefant/SU-152 are casemate guns: V2 needs a "hull aims" mode (the owner asked about a StuG).

## Later (agreed, not scheduled)
- **Graphics options in the menu** (owner asked 2026-11-18, "later"). Valve's Video > Advanced page is GameUI.dll (engine
  binary, not in the SDK), so: a "DOD-SP Graphics" main-menu entry (GameMenu.res, `engine <command>`) opening our own
  client.dll VGUI window with cvar-bound checkboxes/sliders. Proposed rows: sun shadows (dodsp_sun_baked), sun
  softness, live lamps (dodsp_lamps_baked), lamp shadows, prop shadows (dodsp_sun_propshadows), shadow resolution
  (r_flashlightdepthres, restart). Those become FCVAR_ARCHIVE (saved) - update "Settings that reset" when it lands;
  debug/calibration cvars stay console-only. Needs the owner's DOD-SP\resource\GameMenu.res to add the entry.
- **NPC soldier types as data** (owner: "hold onto that", 2026-11-18; from CoD2's AI/axis_nrmdy_wehr_reg_kar98.gsc).
  CoD defines each soldier TYPE in a generated file: team, accuracy (x weapon spread, 0.2), health, primary/secondary
  weapon, grenade type + COUNT (riflemen 0 - throwers are a role), and a random body from a list (4 bodies, each
  picking head/gear). Spawner flags: count, FORCESPAWN (delete a far unimportant AI at the cap), UNDELETABLE,
  NOENEMYINFO (spawn without knowing where the player is). For npc_german_soldier (today: model from Hammer or one
  default, no per-type settings): one KeyValues file per type (weapon, sidearm, proficiency/spread, health,
  grenades, body/head/bodygroup/skin variants), picked by a "type" key on the NPC/spawner; random look per spawn.
  Start with types + random looks together. npc_template_maker already covers count.
- **German battle chatter: the voice set vs the code** (review 2026-11-18, the owner's Voices.zip). The set:
  Voices/German/Battlechatter/GE/<0..3>/ge_<v>_<category>[_us|_uk|_ru]_NN.wav - 4050 files, 328 categories, 4 voices
  (~1012 each); Axis/A,N,O = melee attack/charge shouts, Axis/M,N,O gen_<m|n|o> = pain/death (used). The code
  (german_chatter.cpp) can play 793 of the 4050 (20%): voices {0,2,3} only - VOICE 1 (1014 lines) IS NEVER USED;
  16 kinds of line + order_cover_<38 landmarks>. Unused families (files): landmark_near_/behind_<x> 870, name_<surname>
  355 (70 names), location_<floor>_<window|doorway|balcony> 264, direction_relative/inbound/side_<dir> 228, taunt 149,
  area_<x> 104, inform_killconfirm 92 + response_killconfirm 36, inform_attacking_grenade 82, order_action
  grenade/coverme/smoke/boost 120, order_smokescreen 64, inform_incoming mg/mortar/artillery/smkgrenade 100,
  threat_vehicle tank/brencarrier/trooptruck 48, threat_emplace (MGs) 44, threat_infantry many/sniper/panzerfaust/
  panzerschreck 92, rank_<x> 24, inform_suppressed 16, reaction_surprise_ambush 12, response_ack_no/follow,
  order_move_follow/generic, order_cover_tank/antitankgun. CoD builds a callout from PARTS said back to back by one
  speaker ("Infanterie!" + "bei der Scheune!", or name + order) - ours plays one clip per line.
  Silent-chatter suspects: script/waves missing (load warning), strict gates (threat only after 15 s unseen, 30%
  become "Angriff!", threat expires after 1 s if a squadmate talks, orders only for 250+ unit moves with a mate in
  800, a soldier with no squad name never gets/answers orders), and npc_german_debug prints only what was SAID -
  dropped/expired lines leave no trace. Source's info_landmark is the changelevel marker - never reuse it.
  SENTENCES (owner's plan 2026-11-18, HL1 HECU style): NAME + THREAT + LOCATION + ORDER, one speaker, parts back to
  back ("Mueller! Infanterie! Zweiter Stock, Fenster! Niederhalten!"); the named man answers. Measured on the set: every
  clip is trimmed tight (median 0.01-0.02 s silence before speech, 0.03-0.05 s after; speech = 5% of the clip's peak),
  so parts join with no trimming - the code adds the gap (~0.1-0.15 s). Median lengths: name 0.57 s, threat_infantry
  1.87, landmark 1.52, location 1.47, direction 1.30, area 1.09, orders 1.6-1.9, response_ack 0.94, rank 0.67 -> a
  full four-part sentence is ~5.5 s, long in a firefight: full form for first contact / when the enemy moves to a
  new spot, 2-part forms otherwise. Re-check each part before it plays (enemy still there, named man alive, speaker
  not hit - pain already cuts him off).
  **C1 BUILT 2026-11-18 (chatter that works).** Why voice 1 was silent: folder GE/1's files are named `GE_4_...` (CoD's
  speaker 4) and the old generator required the name's number to match the folder, so the whole voice was dropped
  (the owner's game had de.zip = 1216 files, voices 0/2/3). Now: `dodsp_tools/gen_chatter.py Voices.zip out.txt` reads
  the set AS SHIPPED (voice = folder, spellings merged word by word, `_NNb` = one more take, cries from Axis/M,N,O;
  melee shouts not used yet - Axis/O/MeleeCharge_GE_0_10.wav is 48 kHz, convert before using it). Every wave path is
  `dodsp/Battlechatter/...`: the owner copies the set's `Battlechatter` folder into `DOD-SP\sound\dodsp\` unchanged
  (old sound/dodsp/chatter/de and sound/dodsp/voice folders are no longer read). Generated copy:
  `dodsp_tools/generated/game_sounds_german_chatter.txt` (3762 entries, 4097 waves, all 315 kinds in all 4 voices) -
  the owner's `DOD-SP\scripts\` copy must be this one. Guards: `dodsp_lint_client.py` check_chatter (every line the
  code asks for x every voice x us/uk/ru exists in the generated script, plus the 6 cries); map load prints ONE line
  `DOD-SP German chatter: 4 voices, N lines ..., all wave files found` or `INVARIANT German chatter: N of M wave files
  missing (first: ...)` / `... line kinds have no sound entry`. Every request that isn't said is counted by reason
  (ChatDrop_t: off, self-wait, squad-wait, kind-wait, outranked, replaced, stale, no-line, cut); `npc_german_chatter`
  prints the totals + per line kind (soldiers, voices in use, how many have no squad = no orders/answers);
  `npc_german_debug 1` prints each drop live. Gates relaxed: a threat after 4 s unseen (was 15), "Angriff!" no longer
  REPLACES 30% of threat calls - the squad leader follows 40% of sightings with it (1.5 s later); threat validity
  1 -> 2.5 s and informs 1 -> 2 s (a squadmate's line outlasted them); cover orders from 150 units (was 250).
  Cry speakers for voices 0..3 = "n m m o" (pitch-matched; 3-letter "n m o" = the old voices 0/2/3 form).
  Files: german_chatter.cpp/.h, npc_german_soldier.cpp (ChatterThreat, ChatterOrderCoverMove, cry default, the
  INVARIANT name table), dodsp_tools/gen_chatter.py + generated/, devtools/dodsp_lint_client.py, dodsp_build.yml.
  Undo: the previous server.dll + the old scripts\game_sounds_german_chatter.txt (de/ paths).
  FIRST TEST 2026-11-18 (build 44465997298): load line clean (3656/3656 waves for 'us', voices 3/3/2/3, no squadless
  soldiers), owner heard talking and orders answered - but `npc_german_chatter` said `said 0 | not said: none`. Cause:
  the counters reset whenever game time went backwards, which a LOADED SAVE also does. Lesson: "time went back" means
  map restart OR save load - reset only what belongs to the old clock (squad waits), never a session's evidence.
  C1b: counters run since game start; DOD-SP\dodsp_chatter_log.txt (new each game start, flushed per line) logs every
  line SAID (soldier, voice, squad, sound entry, English meaning, cover landmark or why generic, to whom) and NOT SAID
  (reason + seconds), plus `=== map ===` and `--- game time went back ---` markers. The owner pastes the log.
  FIRST LOG 2026-11-18 (build 44470219910, dod_argentan, one squad A1, ~2 min): 31 lines said, 66 not; all 4 voices
  heard (voice 1: 7 lines); orders answered by the man ordered (9 acks). Threat calls mostly squad-wait = one per squad
  per 8 s, as designed; "cut" = the man was shot first. Two faults: (1) the squad LEADER was asked for nearly every
  order (in earshot = always) and dropped 22 on his own 8 s order wait; "Angriff!" asked 8 times, said 0. (2) 6 of 9
  cover orders "no landmark recognised" with no way to see why. C1c: OrderSpeaker - the leader if in earshot AND free
  (CGermanChatter::CanSay), else the nearest squadmate who is free (NearestSquadmate's eCanSay), else the leader/nearest
  anyway so the log shows the reason; "Angriff!" uses it too. GermanChatter_LandmarkAt says what it found (prop +
  model, static prop, brushwork + material, tall brushwork = building, nothing within 160 units) and a generic cover
  order logs it: `take cover! (no landmark recognised: ...)`.
  CONFIRMED 2026-11-19 (build 44472599057, dod_argentan, squad A1, ~3.3 min): 67 lines said (was 31 in 2 min), orders
  from 9 different soldiers (most active 7, was one man with nearly all), "Angriff!" said 3 times (was 0), all 4 voices
  (0:25 3:20 1:11 2:11). What stops lines now is squad-wait (one line of a kind per squad per 6-8 s) - by design.
  Owner: "extremely immersive". KNOWN-GOOD for C1. Cover callouts: rubble 5 named; generic 7 = nothing within 160
  units at hip height 4 (open ground / corner spots - generic is right), static props with no rule: railing01 (not
  cover; no fence line in order_cover), piano (no line), props_exteriors/storefront3 (-> "house" in C2: storefront /
  facade / shop words = a building front).
  **C2 BUILT 2026-11-19 (sentences).** One owner: CGermanChatter::BuildSentence picks the parts (<= CHAT_MAX_PARTS 4),
  PlaySentencePart plays them back to back in ONE voice (gap CHAT_PART_GAP 0.12 s; the squad's one-speaker lock is held
  through the gaps), EndSentence logs ONE line with every part + meaning and has the named man answer. Each part is
  checked again before it plays (PARTCHECK_TARGET: the named man alive; PARTCHECK_ENEMY); a hit/death cuts the rest
  (`[cut after k of n parts: ...]` in the log). Forms: orders = NAME + ORDER (3 in 4 named); threats = THREAT +
  LOCATION, and the full call NAME + THREAT + LOCATION + ORDER (order_attack_window "fire at the window!" when he's at a
  window, else order_attack_infantry) on first contact / enemy 300+ units from the squad's last full call / 20 s since;
  the addressee = nearest squadmate free to answer (NearestSquadmate eCanSay CHAT_ACK_YES); the old separate "Angriff!"
  follow-up is gone (it's the last part now). Names: every soldier gets a surname from the set's 69 (s_pszNames,
  unique in the squad, random start; re-picked after a save load). Location (EnemyLocation, as the SPEAKER sees him):
  1 building - enemy under a roof seen from outside/another level, or high (>96 units) in the open not on terrain:
  floor by height above the speaker (<-64 basement, <80 ground = the set's "firstfloor", <210 second, <340 third, else
  top), window if wall below him toward us (16 units up, 64 out), else doorway; in the open high up = balcony (wall
  72 units behind him) or roof; 2 behind a landmark (GermanChatter_LandmarkAt from his feet toward the speaker); 3
  near one (trigger_german_landmark, static props, prop entities within 200); 4 compass (+Y north). Set words differ
  for two landmarks: tree->trees, kubelwagon->kubelwagen (LandmarkSetWord). Storefront/facade/shopfront -> house.
  Every GermanChatter entry of the enemy nation is precached once per map (sentences can use any kind). Test command:
  `npc_german_callout` = where the nearest soldier would say you are. Lint: EVERY literal line kind in
  german_chatter.cpp (names, floors, compass, orders) must exist in every voice x nation (tested: a fake name fails).
  FIRST C2 LOG 2026-11-19 (build 44479230913, dod_argentan, ~7.5 min with a save load): names work (owner heard them);
  full calls said and answered ("Schneider! enemy infantry spotted! behind the car! attack!" -> "yes, sir!"); locations
  used: behind the car/trees, near the rubble/wall, ground floor doorway, compass northwest/west. Three faults, fixed in
  C2b: (1) "basement!" x3 - the floor was counted from the SPEAKER's height, so soldiers upstairs looking down on the
  player called him in the basement. Now StreetLevel: the lowest ground under open air on the line between them (48..600
  units out); no open air between = same building = no floor words. (2) `SAID ... [cut after 0 of 2 parts]` - a line
  whose first part couldn't play (named man dead) counted as said and started the waits; now it is NOT SAID - cut, and
  said/waits start only when part 1 plays. (3) a cover call that NAMED its landmark still printed "no name rule
  matches": LandmarkAt wrote the note before checking the prop (donner_wall_160 = wall); the note is now written only
  for an unrecognised prop, and the log shows it only on a generic cover call.
  C2c (owner asked 2026-11-19): names and voices ride in the save - CGermanChatter has a SIMPLE datadesc (m_iName,
  m_iVoice), embedded in the soldier's FIRST datadesc (the soldier saved none of its own state before; its tactics
  still restart after a load). OnRestore -> Init(..., bRestored) keeps both and marks the name used in the squad.
  Casualty calls name the fallen: the dead man's name line rides in the request's modifier (he may be removed before
  the witness's turn) -> "Kahn! man down!" (NAME + reaction_casualty_generic; nobody answers).
- **US battle chatter: the voice set** (review 2026-11-18, the owner's US_Voices.zip; nothing plays it yet - DOD-SP has
  no US NPC). US Voices/US/<0..3>/US_<v>_<category>_NN[b|c|d].wav: 2677 clips, 244 categories, all 22050 Hz 16-bit
  mono (no conversion). The letter suffix = another take of the same line (same length/level) - the generator must
  accept `_NNb` (457 files) and treat it as one more variant. Same family layout as the German set, Normandy-flavoured:
  threat_vehicle tiger/panther/panzer/halftrack, threat_emplace mg34/mg42, flak88, hedgerow. 60 names (vs 70 German).
  Spelling differs per nation, so the EVENT is one owner and each nation maps it to its file names: US killfirm = GE
  killconfirm, US secondflr only, US pzerschreck = GE panzerschreck, US smkgrenade = GE smoke. VOICE 1 IS PARTIAL: it
  has no threat_*, taunt, reaction_*, most order_action/order_move, the upper floors - so a sentence may only be built
  from parts its speaker has (pick the speaker per sentence, check every part). Timing like the German set (median
  0.008 s before speech, 0.026 s after; name 0.60 s, threat 1.43, landmark 1.33, location 1.27, order 1.53).
  US/allied/: 115 generic pain/death/exert/charge/melee clips from SEVERAL nations (AS_RU_, JP_RU_, JS_UK_, Ru_fem1 =
  a Russian woman) - the owner picks the US ones by ear before they're used.
- **US Airborne models** (owner's us_Airborne.zip, a GMod pack ripped from Brothers in Arms: RotH30; hobby build only,
  replace before any release - owner knows). MDL v48 (loads in 2013 SP), ValveBiped, plain VertexLitGeneric. Use the
  5 `bia_roth30_us_airborne<N>npc.mdl` (includes HL2's male_shared/male_ss/gestures/postures - HL2 is mounted); the
  `...pm.mdl` ones include GMod's m_anm.mdl (absent -> T-pose). Faces = HL2 citizen facemaps. Bodygroups: helmet 4,
  backpack 3, pouches 6, grenades 4, pistol_holster 3, gear/side_bag/leg_side_bag/knife1/knife2/bandolier 2 each
  -> random looks per spawn (soldier types above). 22 single-bone gear props. The GMod lua is spawn-menu only.
