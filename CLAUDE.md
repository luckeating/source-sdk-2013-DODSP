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
- Mapbase ships its shaders precompiled, so their sources may not compile on today's compilers:
  `register( D3DVERTEXTEXTURESAMPLER0, s0 )` is rejected (X3089, write `s0`), and a loop over the
  integer light count fails (X4509, read the light-enabled booleans instead).
- SM3: texture reads that use gradients (tex2D, tex2Dproj) can't sit inside dynamic branches.
  Sample everything and select without branching.
- Shadow compare: on NVIDIA (`GetShadowFilterMode() == 0`) `tex2Dproj` does the depth compare in
  hardware. Elsewhere, read raw depth and compare in the shader (static combo `DODSP_SUNFILTER`).
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
- Hammer expert compile: every `-option` goes BEFORE `$path\$file` in a tool's parameters. An
  option after the map name is read as the map name, the tool prints usage and the compile fails.
- The engine writes no crash logs. Ours (c_dodsp_crashlog.cpp) writes `DOD-SP\dodsp_crash.txt` and
  `.mdmp`. Memory corruption crashes far from the cause, so don't trust the crash location alone.

## Settings that reset every launch (put them in every test step)
- `dodsp_sun_baked` starts at **1** (baked sun, per-pixel sun off). Testing the per-pixel sun
  needs `dodsp_sun_baked 0` after loading. Cost us a false alarm on 3b4.
- `dodsp_sunshader` is read once per game start. Changing it mid-game does nothing until a restart.

## Owner-side pitfalls seen
- Windows hides extensions: a rename to `x.dll` became `x.dll.dll`.
- Shader updates need BOTH `bin\game_shader_dx6.dll` and `shaders\fxc\dodsp_*.vcs` from one build.
- A setting changed after the map loaded invalidated a test once. Restart for start-up settings.

## Open known issues (lighting)
- The door entity doesn't cast into the sun cascades.
- Black front steps = the HL2-materials issue above (fixed in the client build after 44117643276). The half-black CAFÉ sign is an overlay - the
  owner's to fix, not a shader problem. woodstair02 materials reverting.
- Cubemaps (owner: "fix later"): after a compile, before `buildcubemaps`, envmapped props show noise
  (the material points at cubemap images the compile reserved but nothing has filled). Idea: fall
  back to the default cubemap when a map's cubemaps aren't built, and say so at load.
- Pending checks: FPS with `dodsp_sun_baked` 1 vs 0; `dodsp_sun_filter 3` against edge saw-tooth.
- Confirmed 2026-11-17: `buildcubemaps` no longer crashes with the sun shaders loaded (3b4).
