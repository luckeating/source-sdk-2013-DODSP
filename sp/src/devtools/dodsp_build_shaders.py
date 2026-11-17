"""DOD-SP shader build (lighting stage 3). Runs in CI on Windows; nobody needs to run it by hand.

Compiles every .fxc in materialsystem/stdshaders/dodsp_shaders_dx9_30.txt with Valve's
ShaderCompile2 (devtools/bin, from the 2025 Source SDK; based on SCell555's ShaderCompile) and
copies the .vcs files to the output folder given on the command line (a DOD-SP-shaped tree:
<out>/shaders/fxc/dodsp_*.vcs).

Fails the build (exit 1) when an INVARIANT breaks:
  - every shader source in the list and every .vcs produced is named dodsp_*
    (so nothing can ever replace a Valve or Mapbase shader file)
  - every shader the DLL registers (BEGIN_VS_SHADER / BEGIN_SHADER in stdshaders/dodsp) is DODSP_*
  - each .fxc's combo names (// STATIC / // DYNAMIC) are exactly the Set<NAME>() setters in the
    hand-written dodsp/<name>.inc, so the C++ side and the .vcs agree on combos
  - every listed .fxc produced its .vcs

usage: python dodsp_build_shaders.py <out_dir>
"""
import glob
import os
import re
import shutil
import subprocess
import sys

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

    # Combos: .fxc lines vs the hand-written .inc setters
    for src in sources:
        base = os.path.splitext(os.path.basename(src))[0]
        inc = os.path.join(STDSHADERS, "dodsp", base + ".inc")
        if not os.path.exists(inc):
            fail("%s has no hand-written dodsp/%s.inc" % (src, base))
            continue
        with open(inc, encoding="latin-1") as f:
            setters = set(re.findall(r'void\s+Set([A-Za-z0-9_]+)\s*\(', f.read()))
        combos = set(combo_names(os.path.join(STDSHADERS, src)))
        if setters != combos:
            fail("%s combos %s do not match %s.inc setters %s" % (src, sorted(combos), base, sorted(setters)))

    # Compile
    for src in sources:
        cmd = [COMPILER, "-ver", "30", "-shaderpath", STDSHADERS, src]
        print("> " + " ".join(cmd), flush=True)
        r = subprocess.run(cmd, cwd=STDSHADERS)
        if r.returncode != 0:
            fail("ShaderCompile2 failed on %s (exit %d)" % (src, r.returncode))

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
