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
