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

INVARIANT (German chatter, C1): every line kind german_chatter.cpp can ask for (s_ChatDefs, the extra
categories, order_cover_<every landmark>) has a sound entry for EVERY voice in s_iVoices and every enemy
nation in the generated script (dodsp_tools/generated/game_sounds_german_chatter.txt, the copy the owner
gets), plus the six pain/death cries. Voice 1 was silent for months because the old generator dropped its
files and nothing compared the code with the script.

INVARIANT (vehicles, V2d): a vehicle's sight has ONE test, CDoDSPVehicle::LineOfFire - firing spots are chosen
with it, the turret sees with it, the guns fire only when it passes from the muzzle. V2c: the spot search used a
world-only trace (TRACE_WORLD_ONLY skips static props, i.e. every tree), the turret didn't, and a tank went back
and forth between two spots with no shot. So dodsp_vehicle.cpp has no CTraceFilterWorldOnly, and every other
UTIL_TraceLine in it is tagged 'dodsp-lint: not a sight line (<what it is>)'.
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
    # stage 5 live lamps: the server's fallback has one client writer; the pak file names one owner each side
    "dodsp_lamps_fallback": {"game/server/world.cpp", "game/client/dodsp/c_dodsp_lamps.cpp"},
    "DODSP_LAMPS_FILE_": {"public/dodsp_lightstyles.h", "utils/vrad/lightmap.cpp", "game/client/dodsp/c_dodsp_lamps.cpp"},
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


CHATTER_CPP = os.path.join(ROOT, "game", "server", "dodsp", "german_chatter.cpp")
CHATTER_SCRIPT = os.path.join(ROOT, "..", "..", "dodsp_tools", "generated", "game_sounds_german_chatter.txt")
CHATTER_NATIONS = ["us", "uk", "ru"]


def check_chatter():
    with open(CHATTER_CPP, encoding="utf-8") as f:
        code = f.read()
    with open(CHATTER_SCRIPT, encoding="utf-8") as f:
        entries = set(re.findall(r'^"([^"]+)"', f.read(), re.M))
    defs = re.search(r"s_ChatDefs\[\]\s*=\s*\{(.*?)\};", code, re.S).group(1)
    cats = [c for c in re.findall(r'\{\s*"([a-z_]+)",\s*GROUP_', defs) if c != "order_cover"]
    cats += re.findall(r'"([a-z_]+)"', re.search(r"s_pszExtraCategories\[\]\s*=\s*\{(.*?)\};", code, re.S).group(1))
    landmarks = re.findall(r'"([a-z]+)"', re.search(r"s_pszLandmarks\[\]\s*=\s*\{(.*?)\};", code, re.S).group(1))
    cats += ["order_cover_" + l for l in landmarks]
    voices = re.findall(r"\d+", re.search(r"s_iVoices\[\]\s*=\s*\{(.*?)\};", code, re.S).group(1))
    # C2: every literal line kind anywhere in the code (sentence parts: names, floors, compass, orders)
    stripped = re.sub(r"/\*.*?\*/", "", re.sub(r"//[^\n]*", "", code), flags=re.S)
    kinds = r"(?:name|location|direction|landmark|threat|order|inform|reaction|response|rank|area|taunt)_[a-z_]*[a-z]"
    cats += [c for c in re.findall(r'"(%s)"' % kinds, stripped) if c != "order_cover"]
    cats = sorted(set(cats))
    bad = []
    for v in voices:
        for c in cats:
            for n in CHATTER_NATIONS:
                name = "GermanChatter.v%s.%s.%s" % (v, c, n)
                if name not in entries:
                    bad.append(name)
    for spk in "mno":
        for kind in ("pain", "death"):
            if "GermanVoice.%s.%s" % (spk, kind) not in entries:
                bad.append("GermanVoice.%s.%s" % (spk, kind))
    if len(voices) < 4 or len(cats) < 50:
        bad.append("parsed only %d voices / %d line kinds from german_chatter.cpp - the lint's patterns are out of date"
                   % (len(voices), len(cats)))
    return bad


VEHICLE_SCRIPT = os.path.join(ROOT, "..", "..", "dodsp_tools", "content", "scripts", "dodsp_vehicles.txt")
VEHICLE_SOUNDS = os.path.join(ROOT, "..", "..", "dodsp_tools", "content", "scripts", "game_sounds_dodsp_vehicles.txt")


def check_vehicle_sounds():
    """Every DoDSP_Vehicle.* sound the vehicle types name exists in the vehicle sound script (DoD:S's own
    Weapon_* entries are checked in game at map load: 'DOD-SP vehicle sounds: ...')."""
    with open(VEHICLE_SOUNDS, encoding="utf-8") as f:
        entries = set(re.findall(r'^"([^"]+)"', f.read(), re.M))
    with open(VEHICLE_SCRIPT, encoding="utf-8") as f:
        script = re.sub(r"//[^\n]*", "", f.read())
    used = set(re.findall(r'"(DoDSP_Vehicle\.[A-Za-z0-9_]+)"', script))
    bad = ["%s (named in dodsp_vehicles.txt)" % u for u in sorted(used) if u not in entries]
    # the file's shape: braces balance, ONE top block, every type has a model or a base (a stray header once
    # opened the top block twice - the loader would have read the real types as one type's keys)
    depth, tops = 0, 0
    for ch in script:
        if ch == "{":
            depth += 1
            if depth == 1:
                tops += 1
        elif ch == "}":
            depth -= 1
    if depth != 0 or tops != 1:
        bad.append("dodsp_vehicles.txt: braces unbalanced (%+d) or %d top-level blocks (must be 1)" % (depth, tops))
    for name, body in re.findall(r'^\t"([a-z0-9_]+)"\s*\{(.*?)^\t\}', script, re.M | re.S):
        if '"model"' not in body and '"base"' not in body:
            bad.append("dodsp_vehicles.txt: type %s has neither a model nor a base" % name)
    if len(used) < 5:
        bad.append("found only %d DoDSP_Vehicle sounds in dodsp_vehicles.txt - the lint's pattern is out of date" % len(used))
    return bad


VEHICLE_CPP = os.path.join(ROOT, "game", "server", "dodsp", "dodsp_vehicle.cpp")


def check_vehicle_sight():
    bad = []
    func = ""
    with open(VEHICLE_CPP, encoding="latin-1") as f:
        for n, line in enumerate(f, 1):
            m = re.match(r"^[A-Za-z][^(]*?(\w+)::~?(\w+)\s*\(", line)
            if m:
                func = m.group(2)
            code = line.split("//")[0]
            if "CTraceFilterWorldOnly" in code:
                bad.append("dodsp_vehicle.cpp:%d: CTraceFilterWorldOnly (it skips static props) - use LineOfFire" % n)
            if "UTIL_TraceLine" in code and func != "LineOfFire" and "dodsp-lint: not a sight line" not in line:
                bad.append("dodsp_vehicle.cpp:%d (%s): %s  (a sight test? use LineOfFire; else tag it)" % (n, func, line.strip()))
    return bad


def parse_kv(text):
    """Minimal KeyValues reader: [(key, value-or-list)], comments stripped."""
    text = re.sub(r"//[^\n]*", "", text)
    toks = re.findall(r'"([^"]*)"|(\{)|(\})', text)
    pos = 0

    def block():
        nonlocal pos
        out = []
        while pos < len(toks):
            q, ob, cb = toks[pos]
            if cb:
                pos += 1
                return out
            key = q
            pos += 1
            q2, ob2, cb2 = toks[pos]
            if ob2:
                pos += 1
                out.append((key, block()))
            else:
                pos += 1
                out.append((key, q2))
        return out
    return block()


def kv_get(blk, key):
    for k, v in blk:
        if k.lower() == key.lower():
            return v
    return None


def check_vehicle_types():
    """V4: every vehicle type has armour, a wreck and real ammunition (no V2 flat damage) - the C++ loader prints
    the same as INVARIANTs at map load, this catches them before a build."""
    bad = []
    with open(VEHICLE_SCRIPT, encoding="utf-8") as f:
        top = parse_kv(f.read())
    types = dict(kv_get(top, "DoDSP_Vehicles") or [])
    roles = {"commander", "gunner", "loader", "driver", "radio"}

    def merged(name, depth=0):
        t = types[name]
        base = kv_get(t, "base")
        out = dict(merged(base, depth + 1)) if base and base in types and depth < 5 else {}
        for k, v in t:
            if k != "base":
                out[k] = v
        return out
    for name in types:
        t = merged(name)
        if "dead_model" not in t:
            bad.append("%s: no dead_model (every variant names its own wreck)" % name)
        ar = t.get("armour")
        if not isinstance(ar, list):
            bad.append("%s: no armour block" % name)
        else:
            a = dict(ar)
            for k in ("hull_front", "hull_side", "hull_rear", "hull_top", "structure", "crew"):
                if k not in a:
                    bad.append("%s: armour has no %s" % (name, k))
            for r in a.get("crew", "").split():
                if r not in roles:
                    bad.append("%s: crew role '%s'" % (name, r))
            if "turret_model" in t and "turret_front" not in a:
                bad.append("%s: has a turret but no turret_front armour" % name)
        gun = t.get("gun")
        if isinstance(gun, list):
            g = dict(gun)
            if "damage" in g or "radius" in g:
                bad.append("%s: gun has V2's flat damage/radius - V4 uses the shells' real numbers" % name)
            if "ap" not in g and "he" not in g:
                bad.append("%s: gun has neither ap nor he ammunition" % name)
            if int(g.get("magazine", "1")) > 1 and "cyclic" not in g:
                bad.append("%s: a magazine gun with no cyclic rate" % name)
            if isinstance(g.get("ap"), list):
                ap = dict(g["ap"])
                nums = [float(x) for x in ap.get("pen", "").split()]
                pts = list(zip(nums[0::2], nums[1::2]))
                if not pts or len(nums) % 2:
                    bad.append("%s: ap pen is not 'metres mm' pairs" % name)
                for (r0, p0), (r1, p1) in zip(pts, pts[1:]):
                    if r1 <= r0 or p1 > p0:
                        bad.append("%s: ap pen must go out in range and down in mm (%g m %g mm -> %g m %g mm)" % (name, r0, p0, r1, p1))
                for k in ("calibre", "effect"):
                    if k not in ap:
                        bad.append("%s: ap has no %s" % (name, k))
            if isinstance(g.get("he"), list):
                he = dict(g["he"])
                if float(he.get("filler", "0")) <= 0:
                    bad.append("%s: he has no filler (kg)" % name)
    if len(types) < 5:
        bad.append("found only %d vehicle types - the lint's reader is out of date" % len(types))
    return bad


# INVARIANT (V4): the damage state has one writer per fact - structure in ApplyPenetration/BlastHit, the crew in
# KillCrew/BailOut/Wreck, the state in SetBurning/CheckMorale/BailOut/Wreck (plus the constructor and Spawn).
DAMAGE_WRITERS = {
    "m_flStructure": {"ApplyPenetration", "BlastHit", "Spawn", "CDoDSPVehicle"},
    "m_iCrewAlive": {"KillCrew", "BailOut", "Wreck", "Spawn", "CDoDSPVehicle"},
    "m_eState": {"SetBurning", "CheckMorale", "BailOut", "Wreck", "CDoDSPVehicle"},
}


def check_vehicle_damage():
    bad = []
    func = ""
    with open(VEHICLE_CPP, encoding="latin-1") as f:
        for n, line in enumerate(f, 1):
            m = re.match(r"^[A-Za-z][^(]*?(\w+)::~?(\w+)\s*\(", line)
            if m:
                func = m.group(2)
            code = line.split("//")[0]
            for var, owners in DAMAGE_WRITERS.items():
                if re.search(r"\b%s\s*(?:[-+&|]?=)(?!=)" % var, code) and func not in owners:
                    bad.append("dodsp_vehicle.cpp:%d (%s) writes %s - only %s may" % (n, func, var, ", ".join(sorted(owners))))
    return bad


def main():
    bad = []
    sight_bad = check_vehicle_sight()
    if sight_bad:
        print("dodsp_lint_client: a vehicle sight line around LineOfFire (V2d):")
        for b in sight_bad:
            print("  " + b)
        return 1
    for what, fn in (("a vehicle type is missing V4's armour / wreck / real ammunition", check_vehicle_types),
                     ("the vehicle damage state is written outside its owner (V4)", check_vehicle_damage)):
        vbad = fn()
        if vbad:
            print("dodsp_lint_client: %s:" % what)
            for b in vbad:
                print("  " + b)
            return 1
    vsound_bad = check_vehicle_sounds()
    if vsound_bad:
        print("dodsp_lint_client: a vehicle type names a sound the vehicle sound script doesn't have:")
        for b in vsound_bad:
            print("  " + b)
        return 1
    chatter_bad = check_chatter()
    if chatter_bad:
        print("dodsp_lint_client: German chatter asks for lines the generated sound script doesn't have:")
        for b in chatter_bad[:30]:
            print("  " + b)
        print("  (%d in all) - regenerate with dodsp_tools/gen_chatter.py, or fix the category name in the code." % len(chatter_bad))
        return 1
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
          "prop shadows read through the sun-mode getters; every chatter line exists in every voice; every vehicle sound exists)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
