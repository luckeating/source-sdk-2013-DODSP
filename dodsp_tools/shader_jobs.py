"""Plans the DOD-SP shader build: one CI job per (shader, version), run in parallel.

Each job gets a cache key = hash of the .fxc, every header it includes (followed recursively),
its version, the ShaderCompile commit and SCHEMA. A shader whose sources did not change is
restored from cache instead of recompiled, so changing one shader never recompiles the others.

INVARIANT: the key covers every file the compile reads from stdshaders. A header missing from
the include walk would serve a stale .vcs, so an #include that cannot be found fails the plan.

usage: python shader_jobs.py <stdshaders dir> <ShaderCompile commit>
prints the GitHub Actions matrix JSON: {"include": [{"fxc", "ver", "name", "key"}, ...]}
"""
import hashlib
import json
import os
import re
import sys

SCHEMA = "1"   # bump to force every shader to recompile
DEFAULT_VERSIONS = ["20b", "30"]


def find_file(folder, name):
    want = name.lower()
    for f in os.listdir(folder):
        if f.lower() == want:
            return os.path.join(folder, f)
    return None


def include_closure(folder, start):
    seen, todo = [], [start]
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.append(path)
        with open(path, encoding="latin-1") as fh:
            text = fh.read()
        for inc in re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, flags=re.M):
            p = find_file(folder, os.path.basename(inc))
            if p is None:
                # Valve's generated headers live in subfolders the compile does not read
                if inc.lower().startswith(("fxctmp9", "vshtmp9", "tier0", "shaderlib")):
                    continue
                raise SystemExit("INVARIANT: %s includes %s, which is not in %s"
                                 % (os.path.basename(path), inc, folder))
            todo.append(p)
    return sorted(seen, key=lambda p: os.path.basename(p).lower())


def main(folder, compiler_ref):
    jobs = []
    with open(os.path.join(folder, "dodsp_shaderlist.txt"), encoding="latin-1") as fh:
        lines = [l.strip() for l in fh]
    for line in lines:
        if not line or line.startswith(("//", "#")):
            continue
        parts = line.split()
        fxc_path = find_file(folder, parts[0])
        if fxc_path is None:
            raise SystemExit("INVARIANT: %s is in dodsp_shaderlist.txt but missing" % parts[0])
        files = include_closure(folder, fxc_path)
        h = hashlib.sha256()
        h.update(("schema %s compiler %s\n" % (SCHEMA, compiler_ref)).encode())
        for f in files:
            h.update(os.path.basename(f).lower().encode() + b"\n")
            with open(f, "rb") as fh:
                h.update(fh.read())
        for ver in (parts[1:] or DEFAULT_VERSIONS):
            name = "%s-%s" % (os.path.splitext(os.path.basename(fxc_path))[0], ver)
            key = "dodsp-vcs-%s-%s" % (name, h.hexdigest()[:24])
            jobs.append({"fxc": os.path.basename(fxc_path), "ver": ver, "name": name, "key": key})
    if not jobs:
        raise SystemExit("INVARIANT: dodsp_shaderlist.txt lists no shaders")
    names = [j["name"] for j in jobs]
    if len(set(n.lower() for n in names)) != len(names):
        raise SystemExit("INVARIANT: a shader/version is listed twice")
    print(json.dumps({"include": jobs}))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
