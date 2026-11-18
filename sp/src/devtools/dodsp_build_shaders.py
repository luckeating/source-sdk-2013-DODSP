"""DOD-SP shader build (lighting stage 3). Runs in CI on Windows; nobody needs to run it by hand.

Compiles every .fxc in materialsystem/stdshaders/dodsp_shaders_dx9_30.txt with Valve's
ShaderCompile2 (devtools/bin, from the 2025 Source SDK; based on SCell555's ShaderCompile) and
copies the .vcs files to the output folder given on the command line (a DOD-SP-shaped tree:
<out>/shaders/fxc/dodsp_*.vcs).

Fails the build (exit 1) when an INVARIANT breaks:
  - every shader source in the list and every .vcs produced is named dodsp_*
    (so nothing can ever replace a Valve or Mapbase shader file)
  - every shader the DLL registers (BEGIN_VS_SHADER / BEGIN_SHADER in stdshaders/dodsp) is DODSP_*
  - every shader file the DLL's C++ asks for (DECLARE_/SET_*_SHADER) is a dodsp_* one, so DOD-SP
    never depends on the layout of Mapbase's or Valve's compiled shaders
  - the combo headers dodsp/<name>.inc are exactly what dodsp_shader_inc.py generates from the .fxc,
    and their numbering equals the numbering ShaderCompile2 compiled the .vcs with
  - every listed .fxc produced its .vcs
  - pixel shader constants stay inside c0..c31: the 2013 engine keeps exactly 32 pixel constants
    even on SM3 cards, and a write above that overflows its storage (found 2026-11-15, lost in the
    revert to stock, re-found 2026-11-17 as the buildcubemaps crash). Checked for every listed pixel
    shader and every header it includes, and for every literal pixel-constant write in our C++
  - the brush sun's registers (dodsp_sun_fxc.h vs DODSP_PSREG_*) and the model sun's (each model
    pixel shader's g_DoDSP_MSun0..3 and dodsp_modelsun_vs_fxc.h vs DODSP_MSUN_* in dodsp_sun_shader.h)
    agree, and no other pixel-constant write in the helper that draws them (literal or PSREG_ name)
    lands on them - unless the line says "dodsp-lint: x360 only" or "dodsp-lint: flashlight pass only"

usage: python dodsp_build_shaders.py <out_dir>
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dodsp_shader_inc  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
STDSHADERS = os.path.join(SRC, "materialsystem", "stdshaders")
LIST = os.path.join(STDSHADERS, "dodsp_shaders_dx9_30.txt")
COMPILER = os.path.join(HERE, "bin", "ShaderCompile2.exe")

errors = []


def fail(msg):
    print("INVARIANT: " + msg)
    errors.append(msg)


def read_list(path):
    names = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            names.append(line)
    return names


def combo_names(fxc_path):
    names = []
    with open(fxc_path, encoding="latin-1") as f:
        for line in f:
            m = re.match(r'^\s*//\s*(STATIC|DYNAMIC)\s*:\s*"([A-Za-z0-9_]+)"', line)
            if m:
                names.append(m.group(2))
    return names


MAX_PS_CONST = 32       # hardwareconfig m_NumPixelShaderConstants in the 2013 engine


def strip_comments(text):
    """Comments out, line count kept (so reported line numbers stay right)."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def find_header(name):
    want = os.path.basename(name).lower()
    for f in os.listdir(STDSHADERS):
        if f.lower() == want:
            return os.path.join(STDSHADERS, f)
    return None


def include_closure(path):
    seen, todo = [], [path]
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.append(p)
        with open(p, encoding="latin-1") as f:
            for inc in re.findall(r'#\s*include\s*"([^"]+)"', strip_comments(f.read())):
                h = find_header(inc)
                if h:
                    todo.append(h)
    return seen


def check_constants(sources):
    """INVARIANT: pixel shader constants c0..c31 only; the sun's slots have one owner."""
    reg_re = re.compile(r"register\s*\(\s*c(\d+)\s*\)")
    for src in sources:
        if "_ps" not in os.path.basename(src).lower():
            continue                                    # vertex shaders really have 256 constants
        for path in include_closure(os.path.join(STDSHADERS, src)):
            with open(path, encoding="latin-1") as f:
                for n, line in enumerate(strip_comments(f.read()).split("\n"), 1):
                    for m in reg_re.finditer(line):
                        if int(m.group(1)) >= MAX_PS_CONST:
                            fail("%s:%d (in pixel shader %s) uses c%s - only c0..c31 exist in this engine"
                                 % (os.path.basename(path), n, src, m.group(1)))

    # Old fxc syntax today's compiler rejects (X3089) - Mapbase ships its shaders precompiled, so its
    # sources still carry it. Write register( s0 ).
    for src in sources:
        for path in include_closure(os.path.join(STDSHADERS, src)):
            with open(path, encoding="latin-1") as f:
                for n, line in enumerate(strip_comments(f.read()).split("\n"), 1):
                    if "D3DVERTEXTEXTURESAMPLER" in line:
                        fail("%s:%d uses register( D3DVERTEXTEXTURESAMPLERn, sN ) - today's compiler rejects it (X3089); write register( sN )"
                             % (os.path.basename(path), n))

    # The brush sun's slots: shader side vs C++ side
    with open(os.path.join(STDSHADERS, "dodsp_sun_fxc.h"), encoding="latin-1") as f:
        sun_regs = sorted(int(r) for r in re.findall(r"g_DoDSP_\w+\s*:\s*register\s*\(\s*c(\d+)\s*\)",
                                                     strip_comments(f.read())))
    with open(os.path.join(STDSHADERS, "dodsp", "dodsp_sun_shader.cpp"), encoding="latin-1") as f:
        enum = dict((k, int(v)) for k, v in re.findall(r"\bDODSP_PSREG_(\w+)\s*=\s*(\d+)", strip_comments(f.read())))
    first, count = enum.get("SUNCOLOR"), enum.get("COUNT")
    if first is None or count is None or sun_regs != list(range(first, first + count)):
        fail("sun registers disagree: dodsp_sun_fxc.h uses %s, dodsp_sun_shader.cpp says c%s x %s"
             % (sun_regs, first, count))
    brush_slots = set(sun_regs)

    # The model sun's slots (stage 4): each model pixel shader declares g_DoDSP_MSun0..3 itself
    with open(os.path.join(STDSHADERS, "dodsp", "dodsp_sun_shader.h"), encoding="latin-1") as f:
        menum = dict((k, int(v)) for k, v in re.findall(r"\bDODSP_MSUN_(\w+)\s*=\s*(\d+)", strip_comments(f.read())))
    model_ps = {"dodsp_vertexlit_and_unlit_generic_ps30.fxc": "PSREG_PLAIN",
                "dodsp_vertexlit_and_unlit_generic_bump_ps30.fxc": "PSREG_BUMP",
                "dodsp_skin_ps30.fxc": "PSREG_SKIN"}
    model_slots = {}
    for fxc, key in model_ps.items():
        if fxc not in sources:
            fail("model shader %s is not in dodsp_shaders_dx9_30.txt" % fxc)
            continue
        with open(os.path.join(STDSHADERS, fxc), encoding="latin-1") as f:
            regs = sorted(int(r) for r in re.findall(r"g_DoDSP_MSun\d\s*:\s*register\s*\(\s*c(\d+)\s*\)", strip_comments(f.read())))
        want = list(range(menum.get(key, -99), menum.get(key, -99) + menum.get("PSREG_COUNT", 0)))
        if regs != want:
            fail("model sun registers disagree: %s declares %s, dodsp_sun_shader.h DODSP_MSUN_%s says %s" % (fxc, regs, key, want))
        model_slots[key] = set(want)
        # the cascade samplers: s13.. unless the shader picks others (skin: DODSP_MSUN_SAMPLER_SKIN)
        with open(os.path.join(STDSHADERS, fxc), encoding="latin-1") as f:
            m = re.search(r"#define\s+DODSP_SAMPLER_NEAR\s+s(\d+)", strip_comments(f.read()))
        got = int(m.group(1)) if m else 13
        want_s = menum.get("SAMPLER_SKIN" if key == "PSREG_SKIN" else "SAMPLER", -1)
        if got != want_s:
            fail("model sun samplers disagree: %s puts the cascades on s%d, dodsp_sun_shader.h says s%d" % (fxc, got, want_s))
    with open(os.path.join(STDSHADERS, "dodsp_modelsun_vs_fxc.h"), encoding="latin-1") as f:
        vregs = sorted(int(r) for r in re.findall(r"register\s*\(\s*c(\d+)\s*\)", strip_comments(f.read())))
    vwant = list(range(menum.get("VSREG", -99), menum.get("VSREG", -99) + menum.get("VSREG_COUNT", 0)))
    if vregs != vwant:
        fail("model sun vertex registers disagree: dodsp_modelsun_vs_fxc.h uses %s, dodsp_sun_shader.h says %s" % (vregs, vwant))
    if any(r <= 220 or r > 255 for r in vregs):
        fail("model sun vertex registers %s must sit in c221..c255 (bones end at c216, c217..c220 are the engine's)" % vregs)

    # Which C++ file may touch which slots (a write onto them is allowed only on lines tagged
    # "dodsp-lint: x360 only" or "dodsp-lint: flashlight pass only")
    protected = {
        "dodsp_lightmappedgeneric_helper.cpp": ("the brush sun (dodsp_sun_fxc.h)", brush_slots),
        "dodsp_vertexlitgeneric_helper.cpp": ("the model sun (plain c15.., bump c22..)",
                                              model_slots.get("PSREG_PLAIN", set()) | model_slots.get("PSREG_BUMP", set())),
        "dodsp_skin_helper.cpp": ("the model sun (skin)", model_slots.get("PSREG_SKIN", set())),
    }

    # PSREG_* names used by Mapbase's helpers -> numbers
    psreg = {}
    for hdr in ("cpp_shader_constant_register_map.h", "shader_constant_register_map.h"):
        path = os.path.join(STDSHADERS, hdr)
        if os.path.exists(path):
            with open(path, encoding="latin-1") as f:
                for name, val in re.findall(r"^\s*#define\s+(PSREG_\w+)\s+(\w+)", strip_comments(f.read()), re.M):
                    psreg[name] = val
    def resolve(tok):
        for _ in range(4):
            if tok.isdigit():
                return int(tok)
            if re.match(r"^c\d+$", tok):                   # the HLSL side of the same map: "c15"
                return int(tok[1:])
            tok = psreg.get(tok, "")
        return None

    # Every pixel-constant write in our C++ (literal numbers and PSREG_ names)
    write_re = re.compile(r"SetPixelShaderConstant\w*\s*\(\s*(\w+)\s*(?:,\s*[^,;]*?(?:,\s*(\d+))?)?\s*\)")
    for cpp in glob.glob(os.path.join(STDSHADERS, "dodsp", "*.cpp")):
        base = os.path.basename(cpp)
        with open(cpp, encoding="latin-1") as f:
            raw = f.read()
        raw_lines = raw.split("\n")
        owner, slots = protected.get(base, ("", set()))
        for n, line in enumerate(strip_comments(raw).split("\n"), 1):
            for m in write_re.finditer(line):
                reg = resolve(m.group(1))
                if reg is None:
                    continue                                    # a variable (e.g. the sun's own register enum)
                cnt = int(m.group(2)) if m.group(2) else 1
                if reg + cnt > MAX_PS_CONST:
                    fail("%s:%d writes pixel constant c%d (x%d) - only c0..c31 exist in this engine" % (base, n, reg, cnt))
                tagged = "dodsp-lint: x360 only" in raw_lines[n - 1] or "dodsp-lint: flashlight pass only" in raw_lines[n - 1]
                if slots & set(range(reg, reg + cnt)) and not tagged:
                    fail("%s:%d writes c%d, one of %s's slots - tag the line if it only runs on Xbox 360 or in the flashlight pass"
                         % (base, n, reg, owner))


def check_raw_depth():
    """Stage 4b. INVARIANT: the sun cascades are raw depth maps read with tex2Dlod and compared in the
    shader. A hardware depth compare (tex2Dproj) or SetShadowDepthFiltering on a cascade sampler hands
    the soft shadow's blocker search compare results instead of depths - garbage shadows."""
    for name in ("dodsp_sun_common_fxc.h", "dodsp_sun_fxc.h", "dodsp_modelsun_ps_fxc.h"):
        path = os.path.join(STDSHADERS, name)
        code = strip_comments(open(path, encoding="latin-1").read())
        if re.search(r"\btex2Dproj\s*\(", code):
            fail("%s: tex2Dproj (hardware depth compare) - the cascades are raw depth, read with tex2Dlod" % name)
        if re.search(r"\btex2D\s*\(", code):
            fail("%s: tex2D on the cascades - use tex2Dlod (no gradients: the reads sit in loops and branches)" % name)
    cascade = {"SHADER_SAMPLER13", "SHADER_SAMPLER14", "SHADER_SAMPLER15",   # brush, plain and bump models
               "SHADER_SAMPLER9", "SHADER_SAMPLER10", "SHADER_SAMPLER11"}    # skin (its wrinkle samplers)
    for cpp in glob.glob(os.path.join(STDSHADERS, "dodsp", "*.cpp")):
        base = os.path.basename(cpp)
        for n, line in enumerate(open(cpp, encoding="latin-1"), 1):
            code = line.split("//")[0]
            m = re.search(r"SetShadowDepthFiltering\s*\(\s*(\w+)", code)
            if not m:
                continue
            if base == "dodsp_sun_shader.cpp":
                fail("%s:%d: SetShadowDepthFiltering in the sun's own code - the cascades are raw depth" % (base, n))
            elif m.group(1) in cascade and "dodsp-lint: x360 only" not in line and \
                    not (base == "dodsp_skin_helper.cpp" and m.group(1) not in {"SHADER_SAMPLER9", "SHADER_SAMPLER10", "SHADER_SAMPLER11"}) and \
                    not (base != "dodsp_skin_helper.cpp" and m.group(1) not in {"SHADER_SAMPLER13", "SHADER_SAMPLER14", "SHADER_SAMPLER15"}):
                fail("%s:%d: SetShadowDepthFiltering on %s, a sun cascade sampler - the cascades are raw depth "
                     "(tag the line 'dodsp-lint: x360 only' if it only runs on the Xbox 360)" % (base, n, m.group(1)))


# Stage 5a: shaders that draw what the lamps light. The debug views draw panels, not the scene.
GBUFFER_EXEMPT = {"dodsp_suncascadedebug_ps30.fxc", "dodsp_gbufferdebug_ps30.fxc"}


def check_gbuffer(sources):
    """Stage 5a. INVARIANT: every DOD-SP pixel shader that draws the scene writes the lamps' G-buffer
    (render targets 1..3, dodsp_gbuffer_fxc.h): main returns DoDSP_PS_OUTPUT and every return in it
    goes through DoDSP_GBuffer (lit pixels) or DoDSP_GBufferNone (additive passes). A pixel shader
    that writes only COLOR0 while targets 1..3 are bound leaves them undefined - garbage lamp light."""
    for src in sources:
        base = os.path.basename(src)
        if not base.endswith("_ps30.fxc") or base in GBUFFER_EXEMPT:
            continue
        text = open(os.path.join(STDSHADERS, src), encoding="latin-1").read()
        # The G-buffer header must be included in EVERY variant: 5a's first build had it inside the model
        # shaders' '#if !FLASHLIGHT' block, so the flashlight variants failed (X3000) 15 minutes in.
        depth, seen = 0, False
        for line in strip_comments(text).splitlines():
            d = line.strip()
            if re.match(r"#\s*if", d):
                depth += 1
            elif re.match(r"#\s*endif", d):
                depth -= 1
            elif re.match(r'#\s*include\s+"dodsp_gbuffer_fxc\.h"', d):
                seen = True
                if depth != 0:
                    fail("%s: dodsp_gbuffer_fxc.h is included inside an #if - some variants won't know DoDSP_PS_OUTPUT" % base)
        if not seen and not re.search(r'#include\s+"dodsp_lightmappedgeneric_ps2_3_x\.h"', text):
            fail("%s: does not include dodsp_gbuffer_fxc.h" % base)
        for inc in re.findall(r'#include\s+"(dodsp_\w+\.h)"', text):   # main may live in one of our headers
            path = os.path.join(STDSHADERS, inc)
            if os.path.exists(path):
                text += "\n" + open(path, encoding="latin-1").read()
        code = strip_comments(text)
        code = re.sub(r"#if\s+LIGHTING_PREVIEW.*?#else", "", code, flags=re.S)   # pinned 0..0, never compiled
        m = re.search(r"\bDoDSP_PS_OUTPUT\s+main\s*\(", code)
        if not m:
            fail("%s: main does not return DoDSP_PS_OUTPUT - the lamps' G-buffer (dodsp_gbuffer_fxc.h) is not written" % base)
            continue
        start = code.index("{", m.end())
        depth, end = 0, start
        for end in range(start, len(code)):
            depth += {"{": 1, "}": -1}.get(code[end], 0)
            if depth == 0:
                break
        for r in re.finditer(r"\breturn\b\s*([^;]*);", code[start:end]):
            if not re.match(r"DoDSP_GBuffer(None)?\s*\(", r.group(1)):
                fail("%s: 'return %s' in main skips the lamps' G-buffer - wrap it in DoDSP_GBuffer / DoDSP_GBufferNone"
                     % (base, r.group(1).strip()[:60]))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out_dir = os.path.abspath(sys.argv[1])
    fxc_out = os.path.join(out_dir, "shaders", "fxc")
    os.makedirs(fxc_out, exist_ok=True)

    sources = read_list(LIST)
    if not sources:
        fail("dodsp_shaders_dx9_30.txt lists no shaders")

    # Names: shader sources and the shaders the DLL registers
    for src in sources:
        if not os.path.basename(src).lower().startswith("dodsp_"):
            fail("shader source %s is not named dodsp_*" % src)
    for cpp in glob.glob(os.path.join(STDSHADERS, "dodsp", "*.cpp")):
        with open(cpp, encoding="latin-1") as f:
            text = f.read()
        for m in re.finditer(r'BEGIN_(?:VS_)?SHADER(?:_FLAGS)?\s*\(\s*([A-Za-z0-9_]+)', text):
            if not m.group(1).startswith("DODSP_"):
                fail("%s registers shader %s, which is not named DODSP_*" % (os.path.basename(cpp), m.group(1)))

    # Shader files the C++ uses: dodsp_* only (comments ignored)
    use_re = re.compile(r'\b(?:DECLARE|SET)_(?:STATIC|DYNAMIC)_(?:VERTEX|PIXEL)_SHADER(?:_CMD)?\s*\(\s*(?:\w+\s*,\s*)?(\w+)\s*\)')
    for cpp in glob.glob(os.path.join(STDSHADERS, "dodsp", "*.cpp")):
        with open(cpp, encoding="latin-1") as f:
            code = re.sub(r"//[^\n]*", "", f.read())
        code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
        for name in set(use_re.findall(code)):
            if not name.lower().startswith("dodsp_"):
                fail("%s uses shader file %s, which is not a dodsp_* one" % (os.path.basename(cpp), name))

    # Console variables: DOD-SP's DLL never registers one under another shader DLL's name (the engine
    # links same-named ConVars across DLLs, and ours loads first). mat_fullbright is the engine's own.
    cvar_re = re.compile(r'^\s*(?:static\s+)?ConVar\s+\w+\s*\(\s*("([^"]*)"|SHADER_DLL_CVAR)', re.M)
    shared_ok = {"mat_fullbright"}
    for cpp in glob.glob(os.path.join(STDSHADERS, "dodsp", "*.cpp")) + [os.path.join(STDSHADERS, "BaseVSShader.cpp")]:
        with open(cpp, encoding="latin-1") as f:
            text = f.read()
        guarded = re.sub(r"#ifndef DODSP_SHADER_DLL.*?#endif", "", text, flags=re.S)   # not compiled into our DLL
        for m in cvar_re.finditer(guarded):
            name = m.group(2)
            if m.group(1) == "SHADER_DLL_CVAR" or name is None:
                continue
            if not name.startswith("dodsp_") and name not in shared_ok:
                fail("%s registers ConVar %s in DOD-SP's DLL - give it a dodsp_ name (or SHADER_DLL_CVAR)" % (os.path.basename(cpp), name))

    check_constants(sources)
    check_raw_depth()
    check_gbuffer(sources)

    # Combo headers: generated, up to date
    for src in sources:
        try:
            text = dodsp_shader_inc.generate(src)
        except ValueError as e:
            fail(str(e))
            continue
        path = dodsp_shader_inc.inc_path(src)
        old = open(path, encoding="latin-1").read() if os.path.exists(path) else None
        if old != text:
            fail("%s is stale or hand-edited - regenerate with devtools/dodsp_shader_inc.py" % os.path.basename(path))

    # Fast checks first: a broken invariant fails the build here, in seconds, instead of after
    # half an hour of compiling (stage 4's first build lost 40 minutes to a known compiler trap)
    if errors:
        print("DOD-SP shader build FAILED before compiling: %d problem(s)" % len(errors))
        return 1

    # Compile
    for src in sources:
        cmd = [COMPILER, "-ver", "30", "-shaderpath", STDSHADERS, src]
        print("> " + " ".join(cmd), flush=True)
        t0 = time.time()
        r = subprocess.run(cmd, cwd=STDSHADERS)
        print("  %s: %.0f s" % (src, time.time() - t0), flush=True)
        if r.returncode != 0:
            fail("ShaderCompile2 failed on %s (exit %d)" % (src, r.returncode))

    # Numbering: our headers vs the include files ShaderCompile2 wrote while compiling
    def terms(text):
        out = []
        for ret in re.findall(r"return\s+(.*?);", text, flags=re.S):
            out.append(sorted((n, int(m)) for m, n in re.findall(r"\(\s*(\d+)\s*\*\s*m_n(\w+)\s*\)", ret)))
        return out
    for src in sources:
        base = os.path.splitext(os.path.basename(src))[0]
        theirs = [p for p in glob.glob(os.path.join(STDSHADERS, "**", base + ".inc"), recursive=True)
                  if os.path.normcase(os.path.dirname(p)) != os.path.normcase(os.path.join(STDSHADERS, "dodsp"))]
        if not theirs:
            print("note: ShaderCompile2 wrote no %s.inc to compare numbering with" % base)
            continue
        mine_t = terms(open(dodsp_shader_inc.inc_path(src), encoding="latin-1").read())
        theirs_t = terms(open(theirs[0], encoding="latin-1").read())
        if sorted(mine_t) != sorted(theirs_t):
            fail("%s: combo numbering differs from ShaderCompile2's (%s)\n  ours:   %s\n  theirs: %s"
                 % (src, os.path.relpath(theirs[0], STDSHADERS), mine_t, theirs_t))
        else:
            print("numbering matches ShaderCompile2: %s" % base)

    # Collect: every listed source must have produced its .vcs; only dodsp_* files ship
    produced = {}
    for vcs in glob.glob(os.path.join(STDSHADERS, "**", "*.vcs"), recursive=True):
        produced[os.path.basename(vcs).lower()] = vcs
    for src in sources:
        name = os.path.splitext(os.path.basename(src))[0].lower() + ".vcs"
        if name not in produced:
            fail("%s produced no %s" % (src, name))
            continue
        shutil.copy2(produced[name], os.path.join(fxc_out, name))
        print("shipped shaders/fxc/%s (%d bytes)" % (name, os.path.getsize(produced[name])))
    for name in produced:
        if not name.startswith("dodsp_"):
            fail("the compiler produced %s, which is not named dodsp_* (not shipped)" % name)

    if errors:
        print("DOD-SP shader build FAILED: %d problem(s)" % len(errors))
        return 1
    print("DOD-SP shader build OK: %d shader file(s)" % len(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())
