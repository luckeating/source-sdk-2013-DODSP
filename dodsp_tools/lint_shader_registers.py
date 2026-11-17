"""Fast pre-compile guard for DOD-SP's pixel shader constants (runs in seconds, before ShaderCompile).

INVARIANTS checked:
  1. Every pixel shader we compile (dodsp_shaderlist.txt, *_ps*) and every header it includes uses
     only c0-c31. The engine keeps exactly 32 pixel constants (hardwareconfig.cpp
     m_NumPixelShaderConstants = 32) even on shader model 3 cards; c32+ is written past its buffer.
  2. No shader helper .cpp sets a literal pixel constant >= 32.
  3. The g_DoDSPSun* register( cN ) in the shader and DODSP_PSREG_SUN_* in the helper agree.
  4. Nothing else in the helper writes a literal pixel constant onto one of the DOD-SP slots.
  5. The model sun's vertex flag (g_DoDSPSunVSFlags, common_vs_fxc.h) sits on c51 =
     SHADER_SPECIFIC_CONST_3, matching DODSP_VSREG_SUN_FLAGS in dodsp_modelsun.h, and nothing
     else claims that slot for the model shaders: no listed vertex shader declares it, and the
     model helpers neither write it nor a 2-register transform onto SPECIFIC_CONST_2 (c50-c51)
     in MAPBASE builds. (Other shaders' helpers may use c51 for their own vertex shaders.)
  6. Every listed vertex shader that lights from the engine's light list (GetVertexAttenForLight,
     DoLighting) has a helper that calls DoDSP_SetModelSunState on every draw - otherwise it
     would read whatever sun flag the previous draw left behind.

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

    errors += check_model_sun(folder, listed)

    print("checked %d shader files; sun constants %s" %
          (len(checked), ", ".join("%s=c%d" % kv for kv in sorted(shader_regs.items(), key=lambda kv: kv[1]))))
    for e in errors:
        print(e)
    return 1 if errors else 0


# vertex shader (listed .fxc) -> the helper that drives it
MODEL_SUN_HELPERS = {
    "sdk_vertexlit_and_unlit_generic_vs20.fxc": "vertexlitgeneric_dx9_helper.cpp",
    "sdk_vertexlit_and_unlit_generic_bump_vs20.fxc": "vertexlitgeneric_dx9_helper.cpp",
    "sdk_skin_vs20.fxc": "skin_dx9_helper.cpp",
}


def ifndef_mapbase_lines(text):
    """line numbers (1-based) that sit inside an #ifndef MAPBASE block"""
    inside, stack = set(), []
    for n, line in enumerate(text.split("\n"), 1):
        t = line.strip()
        if re.match(r"#\s*if", t):
            stack.append(bool(re.match(r"#\s*ifndef\s+MAPBASE\b", t)))
        elif re.match(r"#\s*endif", t) and stack:
            stack.pop()
        elif re.match(r"#\s*else", t) and stack:
            stack[-1] = False
        if any(stack):
            inside.add(n)
    return inside


def check_model_sun(folder, listed):
    errors = []
    vs_header = find_file(folder, "common_vs_fxc.h")
    with open(vs_header, encoding="latin-1") as fh:
        vtext = strip_comments(fh.read())
    m = re.search(r"g_DoDSPSunVSFlags\s*:\s*register\s*\(\s*(\w+)\s*\)", vtext)
    dx9_const3 = re.findall(r"#define\s+SHADER_SPECIFIC_CONST_3\s+c(\d+)", vtext)
    msun = find_file(folder, "dodsp_modelsun.h")
    with open(msun, encoding="latin-1") as fh:
        mtext = strip_comments(fh.read())
    mreg = re.search(r"DODSP_VSREG_SUN_FLAGS\s*=\s*(\d+)", mtext)
    if not m or m.group(1) != "SHADER_SPECIFIC_CONST_3":
        errors.append("INVARIANT: g_DoDSPSunVSFlags must be declared on SHADER_SPECIFIC_CONST_3 in common_vs_fxc.h")
    if "51" not in dx9_const3:
        errors.append("INVARIANT: SHADER_SPECIFIC_CONST_3 is no longer c51 for DX9 vertex shaders (%s)" % dx9_const3)
    if not mreg or int(mreg.group(1)) != 51:
        errors.append("INVARIANT: DODSP_VSREG_SUN_FLAGS in dodsp_modelsun.h must be 51 (= SHADER_SPECIFIC_CONST_3)")

    for fxc in listed:
        if "_vs" not in fxc.lower():
            continue
        path = find_file(folder, fxc)
        if not path:
            continue
        lights = False
        for f in includes_closure(folder, path):
            with open(f, encoding="latin-1") as fh:
                text = strip_comments(fh.read())
            base = os.path.basename(f).lower()
            if base != "common_vs_fxc.h":
                if re.search(r"register\s*\(\s*(SHADER_SPECIFIC_CONST_3|c51)\s*\)", text):
                    errors.append("INVARIANT: %s declares SHADER_SPECIFIC_CONST_3 (c51), the DOD-SP sun flag slot" % os.path.basename(f))
                if re.search(r"\b(GetVertexAttenForLight|DoLighting|DoLightingUnrolled)\s*\(", text):
                    lights = True
        if lights:
            helper = MODEL_SUN_HELPERS.get(os.path.basename(path).lower())
            hpath = find_file(folder, helper) if helper else None
            ok = False
            if hpath:
                with open(hpath, encoding="latin-1") as fh:
                    ok = "DoDSP_SetModelSunState(" in strip_comments(fh.read())
            if not ok:
                errors.append("INVARIANT: %s lights from the engine's light list but no helper writes the DOD-SP sun flag "
                              "for it (add it to MODEL_SUN_HELPERS and call DoDSP_SetModelSunState)" % fxc)

    for f in os.listdir(folder):
        if not f.lower().endswith(".cpp"):
            continue
        with open(os.path.join(folder, f), encoding="latin-1") as fh:
            text = strip_comments(fh.read())
        # Other shaders may use c51 for their own vertex shaders - every draw writes its own
        # constants first. Only the model helpers must leave it to DoDSP_SetModelSunState.
        if f.lower() in set(MODEL_SUN_HELPERS.values()):
            for n, line in enumerate(text.split("\n"), 1):
                if "SPECIFIC_CONST_3" in line and "Set" in line:
                    errors.append("INVARIANT: %s:%d writes SPECIFIC_CONST_3, the DOD-SP sun flag slot" % (f, n))
            excluded = ifndef_mapbase_lines(text)
            for n, line in enumerate(text.split("\n"), 1):
                if re.search(r"(TextureTransform|ScaledTransform)\s*\(\s*VERTEX_SHADER_SHADER_SPECIFIC_CONST_2\b", line) and n not in excluded:
                    errors.append("INVARIANT: %s:%d writes a 2-register transform at SPECIFIC_CONST_2, which runs into c51 (the DOD-SP sun flag)" % (f, n))
    return errors


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
