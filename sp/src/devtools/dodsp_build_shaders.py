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
  - the sun's registers in dodsp_sun_fxc.h and DODSP_PSREG_* in dodsp_sun_shader.cpp agree, and no
    other literal pixel-constant write in our C++ lands on them (a line that only runs on Xbox 360
    says so with a "dodsp-lint: x360 only" comment)

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

    # The sun's slots: shader side vs C++ side
    with open(os.path.join(STDSHADERS, "dodsp_sun_fxc.h"), encoding="latin-1") as f:
        sun_regs = sorted(int(r) for r in re.findall(r"g_DoDSP_\w+\s*:\s*register\s*\(\s*c(\d+)\s*\)",
                                                     strip_comments(f.read())))
    with open(os.path.join(STDSHADERS, "dodsp", "dodsp_sun_shader.cpp"), encoding="latin-1") as f:
        enum = dict((k, int(v)) for k, v in re.findall(r"\bDODSP_PSREG_(\w+)\s*=\s*(\d+)", strip_comments(f.read())))
    first, count = enum.get("SUNCOLOR"), enum.get("COUNT")
    if first is None or count is None or sun_regs != list(range(first, first + count)):
        fail("sun registers disagree: dodsp_sun_fxc.h uses %s, dodsp_sun_shader.cpp says c%s x %s"
             % (sun_regs, first, count))
    sun_slots = set(sun_regs)

    # Every literal pixel-constant write in our C++
    write_re = re.compile(r"SetPixelShaderConstant\w*\s*\(\s*(\d+)\s*(?:,\s*[^,;]*?(?:,\s*(\d+))?)?\s*\)")
    for cpp in glob.glob(os.path.join(STDSHADERS, "dodsp", "*.cpp")):
        with open(cpp, encoding="latin-1") as f:
            raw = f.read()
        raw_lines = raw.split("\n")
        for n, line in enumerate(strip_comments(raw).split("\n"), 1):
            for m in write_re.finditer(line):
                reg = int(m.group(1))
                cnt = int(m.group(2)) if m.group(2) else 1
                if reg + cnt > MAX_PS_CONST:
                    fail("%s:%d writes pixel constant c%d (x%d) - only c0..c31 exist in this engine"
                         % (os.path.basename(cpp), n, reg, cnt))
                if sun_slots & set(range(reg, reg + cnt)) and "dodsp-lint: x360 only" not in raw_lines[n - 1]:
                    fail("%s:%d writes c%d, one of the sun's slots (dodsp_sun_fxc.h owns c%d..c%d)"
                         % (os.path.basename(cpp), n, reg, min(sun_slots), max(sun_slots)))


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
