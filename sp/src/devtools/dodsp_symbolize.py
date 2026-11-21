"""DOD-SP: name the code at crash addresses (CI only - runs on the Windows runner, prints to the job log).

    python dodsp_symbolize.py <dir with the symbolised dlls + pdbs> <dir with the owner's dlls or ""> <address> ...

An address is how dodsp_crash.txt prints it: server.dll+0x585431. The crash log (c_dodsp_crashlog.cpp) has no
symbols - the game's PDBs never leave CI - so this is how a crash gets read: the job (dodsp_crash.yml) either takes the
symbols artifact of the build that crashed, or rebuilds that commit and first checks its code is byte-identical to the
build the owner ran (each .text section compared), so the names are the names of the code that crashed.
Uses cdb (Debugging Tools for Windows, opens a dll as a dump) and prints every frame with its source line."""
import os, re, struct, subprocess, sys

SYMS, ORIG = sys.argv[1], sys.argv[2]
ADDRS = [a for a in sys.argv[3:] if re.match(r"^\w+\.dll\+0x[0-9a-fA-F]+$", a)]


def text_section(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    off = pe + 24 + opt
    for i in range(nsec):
        name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", d, off + i * 40)
        if name.rstrip(b"\0") == b".text":
            return va, d[rawptr:rawptr + min(vsize, rawsize)]
    return None, b""


def compare(mod):
    a, b = os.path.join(SYMS, mod), os.path.join(ORIG, mod)
    if not ORIG or not os.path.exists(b):
        print("%s: symbols are the crashing build's own (no rebuild to compare)" % mod)
        return True
    va1, t1 = text_section(a)
    va2, t2 = text_section(b)
    same = sum(1 for x, y in zip(t1, t2) if x == y)
    n = max(len(t1), len(t2))
    print("%s: rebuilt .text %d bytes at 0x%x, the owner's %d bytes at 0x%x - %.4f%% identical"
          % (mod, len(t1), va1 or 0, len(t2), va2 or 0, 100.0 * same / max(n, 1)))
    if t1 == t2:
        print("  IDENTICAL - the names below are exact")
        return True
    print("  NOT identical - names below may be off (the rebuild isn't the code that crashed)")
    return False


def cdb():
    for p in (r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe",
              r"C:\Program Files\Windows Kits\10\Debuggers\x86\cdb.exe"):
        if os.path.exists(p):
            return p
    return None


mods = sorted(set(a.split("+")[0] for a in ADDRS))
for m in mods:
    compare(m)
exe = cdb()
if not exe:
    sys.exit("cdb.exe (Debugging Tools for Windows) isn't on this runner")
for m in mods:
    base = m[:-4]
    cmds = ".lines -e; " + "; ".join("ln %s+%s" % (base, a.split("+")[1]) for a in ADDRS if a.startswith(m + "+")) + "; q"
    out = subprocess.run([exe, "-z", os.path.join(SYMS, m), "-y", SYMS, "-c", cmds], capture_output=True, text=True).stdout
    print("==== %s ====" % m)
    keep = False
    for line in out.splitlines():
        if line.startswith("0:000>") and "ln " in line:
            keep = True
        if keep and line.strip() and not line.startswith("quit"):
            print(line)
