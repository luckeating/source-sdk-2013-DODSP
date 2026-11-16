"""Fast pre-compile guard for DOD-SP's pixel shader constants (runs in seconds, before ShaderCompile).

INVARIANTS checked:
  1. Every pixel shader we compile (dodsp_shaderlist.txt, *_ps*) and every header it includes uses
     only c0-c31. The engine keeps exactly 32 pixel constants (hardwareconfig.cpp
     m_NumPixelShaderConstants = 32) even on shader model 3 cards; c32+ is written past its buffer.
  2. No shader helper .cpp sets a literal pixel constant >= 32.
  3. The g_DoDSPSun* register( cN ) in the shader and DODSP_PSREG_SUN_* in the helper agree.
  4. Nothing else in the helper writes a literal pixel constant onto one of the DOD-SP slots.

usage: python lint_shader_registers.py <stdshaders dir>
"""
import os
import re
import sys

MAX_PS_CONST = 32


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def find_file(folder, name):
    want = name.lower()
    for f in os.listdir(folder):
        if f.lower() == want:
            return os.path.join(folder, f)
    return None


def includes_closure(folder, start):
    seen, todo = [], [start]
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.append(path)
        with open(path, encoding="latin-1") as fh:
            for inc in re.findall(r'#\s*include\s*"([^"]+)"', strip_comments(fh.read())):
                p = find_file(folder, os.path.basename(inc))
                if p:
                    todo.append(p)
    return seen


def main(folder):
    errors = []
    listed = []
    with open(os.path.join(folder, "dodsp_shaderlist.txt"), encoding="latin-1") as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith(("#", "//")):
                listed.append(line.split()[0])

    checked = set()
    for fxc in listed:
        if "_ps" not in fxc.lower():
            continue
        path = find_file(folder, fxc)
        if not path:
            errors.append("INVARIANT: %s is in dodsp_shaderlist.txt but missing" % fxc)
            continue
        for f in includes_closure(folder, path):
            if f in checked:
                continue
            checked.add(f)
            with open(f, encoding="latin-1") as fh:
                text = strip_comments(fh.read())
            for lineno, line in enumerate(text.split("\n"), 1):
                for m in re.finditer(r"register\s*\(\s*c(\d+)\s*\)", line):
                    if int(m.group(1)) >= MAX_PS_CONST:
                        errors.append("INVARIANT: %s:%d pixel constant c%s - the engine only keeps c0-c%d"
                                      % (os.path.basename(f), lineno, m.group(1), MAX_PS_CONST - 1))

    # helpers: literal pixel constant numbers
    our_regs = {}
    helper = find_file(folder, "lightmappedgeneric_dx9_helper.cpp")
    with open(helper, encoding="latin-1") as fh:
        htext = strip_comments(fh.read())
    for name, num in re.findall(r"DODSP_PSREG_SUN_(\w+)\s*=\s*(\d+)", htext):
        our_regs[name.upper()] = int(num)
    for f in os.listdir(folder):
        if not f.lower().endswith("_helper.cpp"):
            continue
        with open(os.path.join(folder, f), encoding="latin-1") as fh:
            text = strip_comments(fh.read())
        for lineno, line in enumerate(text.split("\n"), 1):
            for m in re.finditer(r"SetPixelShaderConstant\w*\(\s*(\d+)\s*,", line):
                n = int(m.group(1))
                if n >= MAX_PS_CONST:
                    errors.append("INVARIANT: %s:%d sets pixel constant c%d - the engine only keeps c0-c%d"
                                  % (f, lineno, n, MAX_PS_CONST - 1))
                if f == os.path.basename(helper) and n in our_regs.values():
                    errors.append("INVARIANT: %s:%d writes c%d, which is a DOD-SP sun slot" % (f, lineno, n))

    # shader <-> helper agreement
    ps_header = find_file(folder, "lightmappedgeneric_ps2_3_x.h")
    with open(ps_header, encoding="latin-1") as fh:
        stext = strip_comments(fh.read())
    shader_regs = {name.upper(): int(num) for name, num in
                   re.findall(r"g_DoDSPSun(\w+)\s*:\s*register\s*\(\s*c(\d+)\s*\)", stext)}
    if not shader_regs:
        errors.append("INVARIANT: no g_DoDSPSun* constants found in lightmappedgeneric_ps2_3_x.h")
    if shader_regs != our_regs:
        errors.append("INVARIANT: sun constants disagree - shader %s, helper %s"
                      % (sorted(shader_regs.items()), sorted(our_regs.items())))
    if len(set(shader_regs.values())) != len(shader_regs):
        errors.append("INVARIANT: two sun constants share a register: %s" % sorted(shader_regs.items()))

    print("checked %d shader files; sun constants %s" %
          (len(checked), ", ".join("%s=c%d" % kv for kv in sorted(shader_regs.items(), key=lambda kv: kv[1]))))
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
