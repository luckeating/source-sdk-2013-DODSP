"""DOD-SP client lint: fast checks on game code, run before the game build compiles anything.

INVARIANT (lighting): no model's lighting depends on a runtime bake while DOD-SP's sun shaders are in
use. A runtime bake (IVModelRender::RecomputeStaticLighting, drawing with STUDIO_STATIC_LIGHTING)
reads the sun's light style at bake time, so with dodsp_sun_baked 0 the model has no sun, and the
STATIC_LIGHT model shaders don't add the per-pixel sun either (sleeping physics props went dark after
being bumped, 2026-11-17). Every such line in the client must check DoDSP_SunShadersInUse() and carry
the tag 'dodsp-lint: static lighting guarded'.

INVARIANT (one owner per fact): see OWNERS below - the sun modes and the static-prop "baked without the
sun" mark each have one writer and one reader.

INVARIANT (stage 4c, prop shadows follow the sun mode): clientshadowmgr.cpp only STORES the map's
shadow_control direction, distance and colour; every shadow reads them through the getters that turn them
into contact shadows while the per-pixel sun draws. So every line touching the stored values carries
'dodsp-lint: map shadow value', and every ColorModulate (the shadow materials' colour) carries
'dodsp-lint: shadow colour' (only DoDSP_ApplyShadowColor may write it). A new raw read would hand some
shadows the map's long, slanted, dark shadow while the per-pixel sun already draws the real one.
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CLIENT = os.path.join(ROOT, "game", "client")
PATTERN = re.compile(r"\bSTUDIO_STATIC_LIGHTING\b|\bRecomputeStaticLighting\s*\(")
TAG = "dodsp-lint: static lighting guarded"


# One owner per fact: who takes the per-pixel sun (DODSP_SUNMODE_*) is decided only by the client's
# PublishSun and read only by the shader DLL's DoDSP_SunModes; the "static props baked without the
# sun" mark (DODSP_LVLFLAGS_PROPS_NOSUN_*) is written only by VRAD and read only by the client.
OWNERS = {
    "DODSP_SUNMODE_": {"public/dodsp_shaders.h", "game/client/dodsp/c_dodsp_sun_cascades.cpp",
                       "materialsystem/stdshaders/dodsp/dodsp_sun_shader.cpp"},
    "DODSP_LVLFLAGS_PROPS_NOSUN_": {"public/dodsp_lightstyles.h", "utils/vrad/vrad.cpp",
                                    "game/client/dodsp/c_dodsp_sun_cascades.cpp"},
    # stage 4c: the contact shadows are set in one place
    "dodsp_contactshadow": {"game/client/clientshadowmgr.cpp"},
    "dodsp_sun_propshadows": {"game/client/clientshadowmgr.cpp"},
    # "the per-pixel sun can't draw here, show the baked one": the client writes it, the server reads it
    "dodsp_sun_fallback": {"game/server/world.cpp", "game/client/dodsp/c_dodsp_sun_cascades.cpp"},
    # "does this map draw with HDR lighting?" - DoDSP_DrawsHDR (cdll_int.h declares it, viewpostprocess.cpp is Valve's own use)
    "MapHasHDRLighting": {"game/client/dodsp/c_dodsp_sun_cascades.cpp", "game/client/viewpostprocess.cpp",
                          "public/cdll_int.h"},
}

# dodsp_sun_baked is the player's choice (default 0). Code never sets it: the old watchdog's
# "dodsp_sun_baked 1" outlived the sunless map that caused it and switched every later map back to the
# baked sun. Where the per-pixel sun can't draw, the client sets dodsp_sun_fallback instead.
SUN_BAKED_WRITE = re.compile(r'"dodsp_sun_baked\s+\d|\bdodsp_sun_baked\.SetValue\b|s_SunBaked\.SetValue\b')


def check_sun_baked_writes():
    bad = []
    for dirpath, _, files in os.walk(os.path.join(ROOT, "game")):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="latin-1") as f:
                for n, line in enumerate(f, 1):
                    if SUN_BAKED_WRITE.search(line.split("//")[0]):
                        bad.append("%s:%d: %s" % (os.path.relpath(path, ROOT), n, line.strip()))
    return bad

SHADOWMGR = os.path.join(CLIENT, "clientshadowmgr.cpp")
SHADOW_RULES = [
    (re.compile(r"\bm_SimpleShadowDir\b|\bm_flShadowCastDist\b|\bm_AmbientLightColor\b"), "dodsp-lint: map shadow value"),
    (re.compile(r"\bColorModulate\s*\("), "dodsp-lint: shadow colour"),
]


# Every model is lit from the point C_BaseAnimating::DoDSP_LightingOrigin decides (or a mapper's own
# lighting origin); a new write of pLightingOrigin elsewhere would skip that check (lit from inside a wall).
LIGHTORIGIN_WRITE = re.compile(r"(\.|->)\s*pLightingOrigin\s*=(?!=)")


def check_lighting_origin():
    bad = []
    for dirpath, _, files in os.walk(CLIENT):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="latin-1") as f:
                for n, line in enumerate(f, 1):
                    if LIGHTORIGIN_WRITE.search(line.split("//")[0]) and "dodsp-lint: lighting origin" not in line:
                        bad.append("%s:%d: %s" % (os.path.relpath(path, ROOT), n, line.strip()))
    return bad


def check_shadow_values():
    bad = []
    with open(SHADOWMGR, encoding="latin-1") as f:
        for n, line in enumerate(f, 1):
            code = line.split("//")[0]
            for pattern, tag in SHADOW_RULES:
                if pattern.search(code) and tag not in line:
                    bad.append("clientshadowmgr.cpp:%d: %s  (needs '%s')" % (n, line.strip(), tag))
    return bad


def check_owners():
    bad = []
    for dirpath, _, files in os.walk(ROOT):
        for name in files:
            if not name.endswith((".cpp", ".h", ".fxc")):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            with open(path, encoding="latin-1") as f:
                text = f.read()
            text = re.sub(r"/\*.*?\*/", "", re.sub(r"//[^\n]*", "", text), flags=re.S)   # code only
            for prefix, allowed in OWNERS.items():
                if prefix in text and rel not in allowed:
                    bad.append("%s uses %s* - only %s may" % (rel, prefix, ", ".join(sorted(allowed))))
    return bad


def main():
    bad = []
    owner_bad = check_owners()
    if owner_bad:
        print("dodsp_lint_client: a DOD-SP sun fact has a second owner:")
        for b in owner_bad:
            print("  " + b)
        return 1
    baked_bad = check_sun_baked_writes()
    if baked_bad:
        print("dodsp_lint_client: code sets dodsp_sun_baked (the player's choice) - set dodsp_sun_fallback instead:")
        for b in baked_bad:
            print("  " + b)
        return 1
    origin_bad = check_lighting_origin()
    if origin_bad:
        print("dodsp_lint_client: a model's lighting point is set around DoDSP_LightingOrigin:")
        for b in origin_bad:
            print("  " + b)
        return 1
    shadow_bad = check_shadow_values()
    if shadow_bad:
        print("dodsp_lint_client: prop shadow values read around the sun-mode getters (stage 4c):")
        for b in shadow_bad:
            print("  " + b)
        print("Read them through GetShadowDirection/GetShadowDistance/DoDSP_ShadowColor, or tag the line if it is a getter.")
        return 1
    for dirpath, _, files in os.walk(CLIENT):
        for name in files:
            if not name.endswith((".cpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            with open(path, encoding="latin-1") as f:
                for n, line in enumerate(f, 1):
                    code = line.split("//")[0]
                    if PATTERN.search(code) and TAG not in line:
                        bad.append("%s:%d: %s" % (os.path.relpath(path, ROOT), n, line.strip()))
    if bad:
        print("dodsp_lint_client: static model lighting used without the DOD-SP sun check:")
        for b in bad:
            print("  " + b)
        print("Check DoDSP_SunShadersInUse() first (see c_physicsprop.cpp) and tag the line '%s'." % TAG)
        return 1
    print("dodsp_lint_client: OK (no unguarded static model lighting in the client; sun facts have one owner each; "
          "prop shadows read through the sun-mode getters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
