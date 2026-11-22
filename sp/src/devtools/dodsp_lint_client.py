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
import math
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
VEHICLE_CPP_PATH = os.path.join(ROOT, "game", "server", "dodsp", "dodsp_vehicle.cpp")


def check_vehicle_sounds():
    """Every DoDSP_Vehicle.* sound the vehicle types name exists in the vehicle sound script (DoD:S's own
    Weapon_* entries are checked in game at map load: 'DOD-SP vehicle sounds: ...')."""
    with open(VEHICLE_SOUNDS, encoding="utf-8") as f:
        entries = set(re.findall(r'^"([^"]+)"', f.read(), re.M))
    with open(VEHICLE_SCRIPT, encoding="utf-8") as f:
        script = re.sub(r"//[^\n]*", "", f.read())
    used = set(re.findall(r'"(DoDSP_Vehicle\.[A-Za-z0-9_]+)"', script))
    bad = ["%s (named in dodsp_vehicles.txt)" % u for u in sorted(used) if u not in entries]
    # V4h: and every one the code plays itself (the hit sounds)
    with open(VEHICLE_CPP_PATH, encoding="latin-1") as f:
        code_used = set(re.findall(r'"(DoDSP_Vehicle\.[A-Za-z0-9_]+)"', f.read()))
    bad += ["%s (named in dodsp_vehicle.cpp)" % u for u in sorted(code_used) if u not in entries]
    if len(code_used) < 3:
        bad.append("found only %d DoDSP_Vehicle sounds in dodsp_vehicle.cpp - the lint's pattern is out of date" % len(code_used))
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


def check_vehicle_fgd():
    """V4j: dodsp_vehicles.fgd's per-type classes are generated from dodsp_vehicles.txt (the server registers the same
    dodsp_vehicle_<type> names from the same script) - the FGD must be what the generator writes, every type needs a
    "display" name."""
    import subprocess
    tools = os.path.join(ROOT, "..", "..", "dodsp_tools")
    bad = []
    r = subprocess.run([sys.executable, os.path.join(tools, "gen_vehicle_fgd.py"), "--check"], capture_output=True, text=True)
    if r.returncode != 0:
        bad.append((r.stdout + r.stderr).strip() or "gen_vehicle_fgd.py --check failed")
    with open(VEHICLE_SCRIPT, encoding="utf-8") as f:
        top = parse_kv(f.read())
    for name, t in (kv_get(top, "DoDSP_Vehicles") or []):
        if isinstance(t, list) and kv_get(t, "display") is None:
            bad.append("%s: no \"display\" name (Hammer)" % name)
    return bad


COD2_TAGS = os.path.join(ROOT, "..", "..", "dodsp_tools", "generated", "cod2_vehicle_tags.txt")
def _cod2_turret_tags():
    out, veh = {}, None
    if os.path.exists(COD2_TAGS):
        for line in open(COD2_TAGS, encoding="utf-8"):
            m = re.match(r"^(\w+) \(CoD2 ", line)
            if m:
                veh = m.group(1)
            m = re.match(r"^\s+tag_turret\s+hull\s+game\s+(\S+)\s+(\S+)\s+(\S+)", line)
            if m and veh:
                out[veh] = [float(v) for v in m.groups()]
    return out
COD2_TURRET_TAGS = _cod2_turret_tags()


def check_vehicle_types():
    """V4: every vehicle type has armour, a wreck and real ammunition (no V2 flat damage) - the C++ loader prints
    the same as INVARIANTs at map load, this catches them before a build."""
    bad = []
    with open(VEHICLE_SCRIPT, encoding="utf-8") as f:
        top = parse_kv(f.read())
    entries = kv_get(top, "DoDSP_Vehicles") or []
    types = dict((k, v) for k, v in entries if isinstance(v, list))
    roles = {"commander", "gunner", "loader", "driver", "radio"}
    # V4e: the script and server.dll come from one build - its script_version equals the C++ define
    with open(VEHICLE_CPP, encoding="latin-1") as f:
        m = re.search(r'#define DODSP_VEHICLE_SCRIPT_VERSION\s+"([^"]+)"', f.read())
    want = m.group(1) if m else None
    have = dict((k, v) for k, v in entries if not isinstance(v, list)).get("script_version")
    if not want:
        bad.append("dodsp_vehicle.cpp has no DODSP_VEHICLE_SCRIPT_VERSION - the lint's pattern is out of date")
    elif have != want:
        bad.append("dodsp_vehicles.txt script_version is %s, dodsp_vehicle.cpp wants %s - bump both together" % (have, want))

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
            # V4e: every crew role gets out through a measured hatch ("turret x y z" / "hull x y z")
            hatches = t.get("hatches")
            h = dict(hatches) if isinstance(hatches, list) else {}
            for r in a.get("crew", "").split():
                if r not in h:
                    bad.append("%s: crew role '%s' has no \"hatches\" entry (he'd get out beside the tank)" % (name, r))
            for r, v in h.items():
                parts = v.split()
                if r not in roles or len(parts) != 4 or parts[0] not in ("turret", "hull"):
                    bad.append("%s: hatch '%s' is '%s' - want a crew role and \"turret x y z\" / \"hull x y z\"" % (name, r, v))
                elif parts[0] == "turret" and "turret_model" not in t:
                    bad.append("%s: hatch '%s' is on a turret it hasn't got" % (name, r))
            # H2: a crew member you can see in his seat IS a crew role of this vehicle; one that steers needs steered wheels
            cs = t.get("crew_seats")
            for r, v in (cs if isinstance(cs, list) else []):
                d = dict(v) if isinstance(v, list) else {}
                if r not in a.get("crew", "").split():
                    bad.append("%s: crew seat '%s' isn't one of its crew (%s)" % (name, r, a.get("crew", "")))
                if len(d.get("origin", "").split()) != 3 or not d.get("idle"):
                    bad.append("%s: crew seat '%s' needs \"origin\" x y z and \"idle\"" % (name, r))
                if d.get("steer") == "1" and t.get("tracked", "0") != "0":
                    bad.append("%s: crew seat '%s' steers, but a tracked vehicle has no steering wheel" % (name, r))
                # V2b: a gunner's seat mans one of its pintle guns, crewed by his role; "weapon" only says "none"
                if "weapon" in d and d["weapon"] != "none":
                    bad.append("%s: crew seat '%s' weapon '%s' - the only choice is \"none\"" % (name, r, d["weapon"]))
                if "mans" in d:
                    mg_ = dict((k_, dict(v_)) for k_, v_ in (t.get("mgs") if isinstance(t.get("mgs"), list) else []) if isinstance(v_, list))
                    g_ = mg_.get(d["mans"])
                    if not g_ or g_.get("mount") != "pintle" or g_.get("crew") != r:
                        bad.append("%s: crew seat '%s' mans '%s' - want one of its pintle machine guns crewed by '%s'" % (name, r, d["mans"], r))
        # V3e: machine guns on their real mounts; no pre-V3e "coax" (it fired from the main gun's muzzle)
        if "coax" in t:
            bad.append("%s: has the old \"coax\" block - V3e machine guns go in \"mgs\" on their mounts" % name)
        mgs = t.get("mgs")
        for mname, mg in (mgs if isinstance(mgs, list) else []):
            m = dict(mg) if isinstance(mg, list) else {}
            mount = m.get("mount")
            if mount not in ("turret", "barrel", "hull", "pintle"):
                bad.append("%s: machine gun '%s' mount '%s' - want turret / barrel / hull / pintle" % (name, mname, mount))
                continue
            if mount in ("turret", "barrel") and "turret_model" not in t:
                bad.append("%s: machine gun '%s' is on a turret it hasn't got" % (name, mname))
            # V2b: a pintle gun's muzzle and elevation are its MODEL's (attachment "muzzle", pose "mg_pitch") - never a script muzzle
            need = (["pintle", "model", "arc", "pitch", "crew", "traverse"] if mount == "pintle" else ["muzzle"]) + \
                   (["pivot"] if mount == "barrel" else []) + (["arc", "pitch", "crew"] if mount == "hull" else [])
            if mount == "pintle":
                if "muzzle" in m:
                    bad.append("%s: machine gun '%s' (pintle) has a \"muzzle\" - a pintle gun's muzzle is its model's attachment" % (name, mname))
                if "pintle" in m and len(m["pintle"].split()) == 3:
                    root_ = name
                    while kv_get(types[root_], "base") in types:
                        root_ = kv_get(types[root_], "base")
                    want_ = COD2_TURRET_TAGS.get(root_)
                    got_ = [float(x) for x in m["pintle"].split()]
                    if want_ and max(abs(a_ - b_) for a_, b_ in zip(got_, want_)) > 0.15:
                        bad.append("%s: machine gun '%s' pintle %s, but CoD2's tag_turret is at %s (cod2_vehicle_tags.txt)"
                                   % (name, mname, m["pintle"], " ".join("%.1f" % v for v in want_)))
                if "arc_riders" in m and "arc" in m and float(m["arc_riders"]) > float(m["arc"]):
                    bad.append("%s: machine gun '%s' arc_riders %s is wider than its arc %s" % (name, mname, m["arc_riders"], m["arc"]))
            for k in need:
                if k not in m:
                    bad.append("%s: machine gun '%s' (%s) has no \"%s\"" % (name, mname, mount, k))
            for k in ("muzzle", "pivot"):
                if k in m and len(m[k].split()) != 3:
                    bad.append("%s: machine gun '%s' %s is '%s' - want x y z" % (name, mname, k, m[k]))
            if "crew" in m and m["crew"] not in roles:
                bad.append("%s: machine gun '%s' crew '%s' isn't a crew role" % (name, mname, m["crew"]))
            if "crew" in m and isinstance(ar, list) and m["crew"] not in dict(ar).get("crew", "").split():
                bad.append("%s: machine gun '%s' is manned by '%s', who isn't in its crew" % (name, mname, m["crew"]))
        # V3e: the turret sits on CoD2's tag_turret x 1.028 (exact - dodsp_tools/generated/cod2_vehicle_tags.txt)
        root = name
        while kv_get(types[root], "base") in types:
            root = kv_get(types[root], "base")
        if "turret_offset" in t and root in COD2_TURRET_TAGS:
            got = [float(x) for x in t["turret_offset"].split()]
            want = COD2_TURRET_TAGS[root]
            if max(abs(a_ - b_) for a_, b_ in zip(got, want)) > 0.15:
                bad.append("%s: turret_offset %s, but CoD2's tag_turret puts the turret model at %s (cod2_vehicle_tags.txt)"
                           % (name, t["turret_offset"], " ".join("%.1f" % v for v in want)))
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
# KillCrew/BailOut/Wreck, the state ONLY in SetState (it stops the vehicle dead - V4 test: a knocked-out tank drove on
# through the ground at its last velocity), plus the constructor.
DAMAGE_WRITERS = {
    "m_flStructure": {"ApplyPenetration", "BlastHit", "Spawn", "CDoDSPVehicle"},
    "m_iCrewAlive": {"KillCrew", "CrewmanShot", "BailOut", "Wreck", "Spawn", "CDoDSPVehicle"},   # H2: CrewmanShot = a seated crewman shot
    "m_eState": {"SetState", "CDoDSPVehicle"},   # SetState halts the vehicle when it leaves the fight (V4b)
}


RIDE_PATHS = os.path.join(ROOT, "..", "..", "dodsp_tools", "content", "scripts", "dodsp_ride_paths.txt")
RIDE_SEQUENCES = os.path.join(ROOT, "..", "..", "dodsp_tools", "generated", "ride_pack_sequences.txt")
SOLDIER_CPP = os.path.join(ROOT, "game", "server", "dodsp", "npc_german_soldier.cpp")
RIDER_LAND_SPACING = 0.4   # H3: seconds between two get-outs ending closer than RIDER_LAND_GAP: the first man's run starts
                           # at his next think (<= 0.1 s) and clears a man's width (26 units, 227 u/s reached in 0.35 s) in 0.28 s
RIDE_FPS = 30.0            # the ride pack's get-outs play at 30 fps (CoD2's, converted as they are; the game reads SequenceFps)
RIDE_STANDIN = 0.2         # H3: = the walks' fadein in the ride pack (RideGetOut's stand-in reads it from the model)
RIDER_LAND_GAP = 30


def check_vehicle_seats():
    """V3: every rider seat names its idle and get-out, every get-out has a path (dodsp_ride_paths.txt, written with the
    ride pack by build_ww2_pack.py --ride-paths), the paths are whole, and RidePosition is the only place that moves a
    man getting off (one owner of where he is - the preview mirrors it)."""
    bad = []
    with open(VEHICLE_SCRIPT, encoding="utf-8") as f:
        types = dict((k, v) for k, v in (kv_get(parse_kv(f.read()), "DoDSP_Vehicles") or []) if isinstance(v, list))
    paths = {}
    if not os.path.exists(RIDE_PATHS):
        return ["dodsp_ride_paths.txt is missing (build_ww2_pack.py --ride-paths writes it)"]
    with open(RIDE_PATHS, encoding="utf-8") as f:
        for seq, blk in (kv_get(parse_kv(f.read()), "RidePaths") or []):
            if not isinstance(blk, list):
                continue
            d = dict(blk)
            n = int(d.get("frames", "0"))
            take, land = int(d.get("take", "-1")), int(d.get("land", "-1"))
            if n < 2 or not (0 <= take <= land < n):
                bad.append("ride path %s: frames %d, take %d, land %d - not a path" % (seq, n, take, land))
            for i in range(n):
                v = d.get(str(i), "").split()
                if len(v) != 4:
                    bad.append("ride path %s: frame %d is '%s' (want x y z s)" % (seq, i, d.get(str(i))))
                    break
            else:
                if n and abs(float(d[str(0)].split()[0])) + abs(float(d[str(0)].split()[1])) + abs(float(d[str(0)].split()[2])) > 0.05:
                    bad.append("ride path %s doesn't start on the seat (frame 0 is %s)" % (seq, d["0"]))
            paths[seq] = n
    pathend, pathxy, pathland = {}, {}, {}
    with open(RIDE_PATHS, encoding="utf-8") as f:
        for seq, blk in (kv_get(parse_kv(f.read()), "RidePaths") or []):
            if isinstance(blk, list):
                d = dict(blk)
                n = int(d.get("frames", "0"))
                last = d.get(str(n - 1), "0 0").split()
                pathend[seq] = (float(last[0]), float(last[1]))
                pathxy[seq] = [tuple(float(v) for v in d.get(str(i), "0 0 0 0").split()[:2]) for i in range(n)]
                pathland[seq] = int(d.get("land", str(n - 1)))
    for name, t in types.items():
        seats = kv_get(t, "seats")
        if not isinstance(seats, list):
            continue
        # where each man lands (his get-out's start - the seat, or H3 its "walk" - + the get-out's end, turned by that
        # yaw): two closer than a man is wide (26) + a margin would land inside each other (V3: the Panzer II's right
        # side had them 16 apart). H3: men who walk out the same way (the halftrack's aisle) share a landing - allowed
        # only one after the other (delays RIDER_LAND_SPACING apart; RideGetOut also waits while anyone stands there)
        lands = []
        for k, (key, blk) in enumerate(seats):
            d = dict(blk) if isinstance(blk, list) else {}
            if "walk" in d and len(d["walk"].split()) != 4:
                bad.append("%s seat %d: walk '%s' is not x y z yaw" % (name, k + 1, d["walk"]))
                continue
            if "rally" in d and len(d["rally"].split()) != 3:
                bad.append("%s seat %d: rally '%s' is not x y z" % (name, k + 1, d["rally"]))
            if d.get("getout") in pathend and len(d.get("origin", "").split()) == 3:
                if "walk" in d:
                    ox, oy, _, yw = (float(v) for v in d["walk"].split())
                else:
                    ox, oy, _ = (float(v) for v in d["origin"].split()); yw = float(d.get("yaw", "0"))
                a = math.radians(yw); ex, ey = pathend[d["getout"]]
                # when he is down and solid again (EndRide, the path's last frame): delay, + the stand-in and the walk
                # from the frame that passes his seat (RideGetOut's join frame, worked out the same way)
                join, standin = 0, 0.0
                if "walk" in d:
                    sx = float(d["origin"].split()[0]); standin = RIDE_STANDIN
                    join = next((f for f, (px, py) in enumerate(pathxy[d["getout"]]) if ox + math.cos(a) * px - math.sin(a) * py <= sx), 0)
                down = float(d.get("delay", "0")) + standin + max(len(pathxy[d["getout"]]) - 1 - join, 0) / RIDE_FPS
                lands.append((k + 1, ox + math.cos(a) * ex - math.sin(a) * ey, oy + math.sin(a) * ex + math.cos(a) * ey, down))
        for i in range(len(lands)):
            for j in range(i + 1, len(lands)):
                gap = math.hypot(lands[i][1] - lands[j][1], lands[i][2] - lands[j][2])
                if gap < RIDER_LAND_GAP and abs(lands[i][3] - lands[j][3]) < RIDER_LAND_SPACING - 1e-3:
                    bad.append("%s: seats %d and %d land %.0f units apart, %.2f s apart - riders are 26 wide: keep their landings %d "
                               "apart, or their ends %.1f s apart (one runs off before the next is down)"
                               % (name, lands[i][0], lands[j][0], gap, abs(lands[i][3] - lands[j][3]), RIDER_LAND_GAP, RIDER_LAND_SPACING))
        # H3: the doors riders get out through - their pose and range are the MODEL's facts (vehicle_rig.py writes
        # $keyvalues "dodsp_doors" with DOOR_OPEN); the script holds only the curve: frame/degree pairs in order, within it
        doors = kv_get(t, "doors")
        if isinstance(doors, list):
            dd = dict(doors)
            rig = open(os.path.join(ROOT, "..", "..", "dodsp_tools", "vehicle_preview", "vehicle_rig.py"), encoding="utf-8").read()
            mo = re.search(r"^DOOR_OPEN\s*=\s*([\d.]+)", rig, re.M)
            top = float(mo.group(1)) if mo else 0.0
            for k in ("pose", "open"):
                if k in dd:
                    bad.append("%s doors: \"%s\" is the model's fact (vehicle_rig.py dodsp_doors) - not the script's" % (name, k))
            c = dd.get("curve", "").split()
            pairs = [(float(c[i]), float(c[i + 1])) for i in range(0, len(c) - 1, 2)]
            if len(c) % 2 or len(pairs) < 2 or len(pairs) > 32 or any(pairs[i + 1][0] <= pairs[i][0] for i in range(len(pairs) - 1)) \
               or any(not (0.0 <= deg <= top) for _, deg in pairs):
                bad.append("%s doors: curve '%s' is not 2..32 frame-degree pairs, in order, within 0..%.0f (vehicle_rig.py DOOR_OPEN)"
                           % (name, dd.get("curve"), top))
        if len(seats) > 8:
            bad.append("%s: %d seats - VEHICLE_MAX_SEATS is 8" % (name, len(seats)))
        for k, (key, blk) in enumerate(seats):
            d = dict(blk) if isinstance(blk, list) else {}
            for need in ("origin", "yaw", "idle", "getout"):
                if need not in d:
                    bad.append("%s seat %d: no \"%s\"" % (name, k + 1, need))
            if len(d.get("origin", "").split()) != 3:
                bad.append("%s seat %d: origin '%s' is not x y z" % (name, k + 1, d.get("origin")))
            if "getout" in d and d["getout"] not in paths:
                bad.append("%s seat %d: get-out %s has no path in dodsp_ride_paths.txt" % (name, k + 1, d["getout"]))
    # H2b: every sequence a seat or crew seat names is in the ride pack the owner compiles (a name the pack hasn't
    # got = an INVARIANT at the first seat and a man who ragdolls / stands in a T-pose instead)
    with open(RIDE_SEQUENCES, encoding="utf-8") as f:
        packseq = set(l.strip() for l in f if l.strip() and not l.startswith("//"))
    for name, t in types.items():
        for block in ("seats", "crew_seats"):
            for key, blk in (kv_get(t, block) if isinstance(kv_get(t, block), list) else []):
                d = dict(blk) if isinstance(blk, list) else {}
                for k in ("idle", "getout", "death", "fidget"):
                    if d.get(k) and d[k] not in packseq:
                        bad.append("%s %s '%s': %s '%s' is not in the ride pack (generated/ride_pack_sequences.txt)" % (name, block, key, k, d[k]))
    func = ""
    with open(SOLDIER_CPP, encoding="latin-1") as f:
        for n, line in enumerate(f, 1):
            m = re.match(r"^[A-Za-z][^(]*?(\w+)::~?(\w+)\s*\(", line)
            if m:
                func = m.group(2)
            if "m_vecRideTag" in line.split("//")[0] and re.search(r"m_vecRideTag\s*=(?!=)", line) and func not in ("RideGetOut", "CNPC_GermanSoldier"):
                bad.append("npc_german_soldier.cpp:%d (%s) sets where a get-out starts - only RideGetOut may" % (n, func))
    return bad


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


def check_npc_motion():
    """V3e: INVARIANT - an NPC's position and pose reach the screen equally late. Code-driven moves (ride get-outs,
    bail-outs, later vaults/climbs/parachutes) go through CDoDSPNpcMotion (dodsp_npc_motion.cpp), which networks the
    position as late as the client draws the pose - like the engine's own walking glide. So:
      - nothing of ours sets "animated/simulated every tick" (that changes the client's windows: V3c did, and moves
        then jumped ahead of the walking soldiers around them and aim poses stepped);
      - the soldier's PhysicsSimulate moves/animates him only through ScriptedMoveTick -> m_Motion.Move;
      - every class that owns a CDoDSPNpcMotion overrides DoDSP_NetworkOrigin AND DoDSP_NetworkAngles and calls
        PostSimulate (else the delay line is never networked or never drains);
      - CBaseEntity's two networked-origin/angle functions still ask the hook first (a Mapbase merge must not drop it);
      - V3f: Begin and PostSimulate keep a think due next tick (KeepSimulating) - a MOVETYPE_NONE entity is otherwise
        simulated only when its AI thinks (0.1-0.2 s), and every 'per-tick' move of V3b/V3c/V3e ran at thinks.
    motion_sim.py (dodsp_tools) is the preview check for any new code-driven animation."""
    bad = []
    srv = os.path.join(ROOT, "game", "server")
    for root, _, files in os.walk(os.path.join(srv, "dodsp")):
        for fn in files:
            if not fn.endswith((".cpp", ".h")):
                continue
            path = os.path.join(root, fn)
            text = open(path, encoding="utf-8", errors="replace").read()
            for n, line in enumerate(text.split("\n"), 1):
                if re.search(r"\bSet(Animated|Simulated)EveryTick\s*\(", line.split("//")[0]):
                    bad.append("%s:%d sets the every-tick flags - code-driven moves go through CDoDSPNpcMotion" % (fn, n))
            if re.search(r"\bCDoDSPNpcMotion\s+m_\w+\s*;", text):
                for need in ("DoDSP_NetworkOrigin", "DoDSP_NetworkAngles", "PostSimulate("):
                    if need not in text:
                        bad.append("%s owns a CDoDSPNpcMotion but has no %s" % (fn, need))
    with open(SOLDIER_CPP, encoding="utf-8") as f:
        lines = f.read().split("\n")
    func = None
    for n, line in enumerate(lines, 1):
        m = re.match(r"^[A-Za-z].*\bCNPC_GermanSoldier::(\w+)\s*\(", line)
        if m:
            func = m.group(1)
        code = line.split("//")[0]
        if func == "PhysicsSimulate" and re.search(r"\b(SetAbsOrigin|SetLocalOrigin|SetAbsAngles|SetLocalAngles|SetCycle|SetSimulationTime)\s*\(", code):
            bad.append("npc_german_soldier.cpp:%d: PhysicsSimulate moves or animates him itself - go through ScriptedMoveTick" % n)
    body = "\n".join(lines)
    m = re.search(r"void CNPC_GermanSoldier::ScriptedMoveTick\([^)]*\)\s*\{(.*?)\n\}", body, re.S)
    if not m or "m_Motion.Move(" not in m.group(1):
        bad.append("npc_german_soldier.cpp: ScriptedMoveTick doesn't move him through m_Motion.Move")
    be = open(os.path.join(srv, "baseentity.cpp"), encoding="utf-8", errors="replace").read()
    for fn_name, hook in (("UseStepSimulationNetworkOrigin", "DoDSP_NetworkOrigin()"), ("UseStepSimulationNetworkAngles", "DoDSP_NetworkAngles()")):
        mm = re.search(r"bool CBaseEntity::%s\([^)]*\)\s*\{(.*?)\n\}" % fn_name, be, re.S)
        if not mm or hook not in mm.group(1):
            bad.append("baseentity.cpp: CBaseEntity::%s no longer asks %s first" % (fn_name, hook))
    mo = open(os.path.join(srv, "dodsp", "dodsp_npc_motion.cpp"), encoding="utf-8", errors="replace").read()
    for fn_name in ("Begin", "PostSimulate"):
        mm = re.search(r"void CDoDSPNpcMotion::%s\([^)]*\)\s*\{(.*?)\n\}" % fn_name, mo, re.S)
        if not mm or "KeepSimulating( pOwner )" not in mm.group(1):
            bad.append("dodsp_npc_motion.cpp: CDoDSPNpcMotion::%s doesn't KeepSimulating - the engine would simulate a "
                       "MOVETYPE_NONE soldier only at his thinks and his moves would reach the screen in steps" % fn_name)
    km = re.search(r"static void KeepSimulating\([^)]*\)\s*\{(.*?)\n\}", mo, re.S)
    if not km or not re.search(r"SetContextThink\(.*gpGlobals->curtime \+ TICK_INTERVAL", km.group(1)):
        bad.append("dodsp_npc_motion.cpp: KeepSimulating no longer schedules a think for the next tick")
    beh = open(os.path.join(srv, "baseentity.h"), encoding="utf-8", errors="replace").read()
    for hook in ("DoDSP_NetworkOrigin", "DoDSP_NetworkAngles"):
        if not re.search(r"virtual\s+const\s+\w+\s*\*\s*%s" % hook, beh):
            bad.append("baseentity.h: the virtual %s hook is gone" % hook)
    return bad


TREADS = os.path.join(ROOT, "..", "..", "dodsp_tools", "content", "scripts", "dodsp_treads.txt")
CLIENT_VEHICLE = os.path.join(ROOT, "game", "client", "dodsp", "c_dodsp_vehicle.cpp")

def check_vehicle_treads():
    """H1: which materials are tracks, their axis and rate have ONE owner - the meshes, measured by
    dodsp_tools/vehicle_preview/tread_scroll.py into scripts/dodsp_treads.txt (the CoD2 conversion's hand-placed proxies
    sat on the Tiger's and Sherman's wheel markings and ran backwards). The table is whole and well formed, the client
    reads that file, and nothing in the client scrolls a track from a .vmt's own numbers."""
    bad = []
    if not os.path.exists(TREADS):
        return ["dodsp_tools/content/scripts/dodsp_treads.txt is missing (tread_scroll.py --write)"]
    txt = open(TREADS, encoding="utf-8").read()
    if "GENERATED by dodsp_tools/vehicle_preview/tread_scroll.py" not in txt:
        bad.append("dodsp_treads.txt isn't tread_scroll.py's output - regenerate it, don't edit it")
    rows = re.findall(r'"(\w+)"\s*\{\s*"axis"\s*"(\w+)"\s*"scale"\s*"([-\d.]+)"\s*\}', txt)
    if len(rows) < 6:
        bad.append("dodsp_treads.txt has %d track materials - expected every tracked vehicle's (9 when measured)" % len(rows))
    for mat, ax, sc in rows:
        if ax not in ("u", "v"):
            bad.append("dodsp_treads.txt: %s axis '%s' (u or v)" % (mat, ax))
        if abs(float(sc)) < 0.001 or abs(float(sc)) > 0.5:
            bad.append("dodsp_treads.txt: %s scale %s isn't a texture rate per unit" % (mat, sc))
    if txt.count("{") != txt.count("}"):
        bad.append("dodsp_treads.txt: unbalanced braces")
    cv = open(CLIENT_VEHICLE, encoding="utf-8").read()
    if '#define DODSP_TREADS_FILE\t"scripts/dodsp_treads.txt"' not in cv:
        bad.append("c_dodsp_vehicle.cpp doesn't read scripts/dodsp_treads.txt (DODSP_TREADS_FILE)")
    if re.search(r'pKeyValues->GetFloat\(\s*"scale"', cv) or re.search(r'pKeyValues->GetString\(\s*"axis"', cv):
        bad.append("c_dodsp_vehicle.cpp reads a track's scale/axis from the .vmt - they come from dodsp_treads.txt only")
    return bad

def check_vehicle_push():
    """H3b: INVARIANT - a vehicle's physics shadow never takes a move back. The engine's VPhysicsUpdatePusher moves a
    SOLID_VPHYSICS pusher back whenever its shadow lags (a player standing on it, riders seated in it): the halftrack
    crawled at ~10 moves a second with its tracks at full speed. CDoDSPVehicle overrides VPhysicsUpdate, and for
    MOVETYPE_PUSH it must neither call the base nor VPhysicsUpdatePusher; DriveThink runs CreepWatchdog."""
    src = open(VEHICLE_CPP, encoding="latin-1").read()
    m = re.search(r"void CDoDSPVehicle::VPhysicsUpdate\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        return ["dodsp_vehicle.cpp: CDoDSPVehicle::VPhysicsUpdate is gone - the engine would take its moves back again"]
    body = "\n".join(l.split("//")[0] for l in m.group(1).split("\n"))
    bad = []
    push = body.split("return;", 1)[1] if "return;" in body else body
    if "VPhysicsUpdatePusher" in push or "BaseClass::VPhysicsUpdate" in push:
        bad.append("dodsp_vehicle.cpp: CDoDSPVehicle::VPhysicsUpdate hands a driving (MOVETYPE_PUSH) vehicle to the engine's pusher check")
    if "MOVETYPE_PUSH" not in body:
        bad.append("dodsp_vehicle.cpp: CDoDSPVehicle::VPhysicsUpdate doesn't single out MOVETYPE_PUSH")
    d = re.search(r"void CDoDSPVehicle::DriveThink\(\s*void\s*\)\s*\{(.*?)\n\}", src, re.S)
    if not d or "CreepWatchdog(" not in d.group(1):
        bad.append("dodsp_vehicle.cpp: DriveThink no longer runs CreepWatchdog")
    return bad


def check_vehicle_eyes():
    """H3b: INVARIANT - every vehicle sees, armed or not (its crew's eyes: UpdateQuarry, the riders' "contact"). The
    unarmed halftrack's WeaponThink returned before the target scan, so it never noticed anyone and its riders never
    got off on contact. WeaponThink must scan (FindTarget) before any return that skips the guns."""
    src = open(VEHICLE_CPP, encoding="latin-1").read()
    m = re.search(r"void CDoDSPVehicle::WeaponThink\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        return ["dodsp_vehicle.cpp: CDoDSPVehicle::WeaponThink not found"]
    body = "\n".join(l.split("//")[0] for l in m.group(1).split("\n"))
    scan, ret = body.find("FindTarget()"), body.find("return;")
    if scan < 0 or (0 <= ret < scan):
        return ["dodsp_vehicle.cpp: WeaponThink can return before its target scan - an unarmed vehicle would be blind"]
    return []


def check_hashint():
    """INVARIANT: tier1's HashInt never reads outside its 256-entry table, for ANY int key. Valve's shifted the signed
    key, so a negative one (ACT_INVALID, asked of the activity hash by TranslateActivity) indexed before the table and
    then anywhere - the server crash under NPC move-and-shoot (2026-11-21). Runs every table index expression of the
    function as it is written, for keys across the whole int range, the way the C code computes them."""
    path = os.path.join(ROOT, "tier1", "generichash.cpp")
    src = open(path, encoding="latin-1").read()
    m = re.search(r"unsigned FASTCALL HashInt\(\s*const int (\w+)\s*\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        return ["generichash.cpp: HashInt not found"]
    key, body = m.group(1), "\n".join(l.split("//")[0] for l in m.group(2).split("\n"))
    if not re.search(r"const unsigned n\s*=\s*\(unsigned\)\s*%s\s*;" % key, body):
        return ["generichash.cpp: HashInt must shift an UNSIGNED copy of its key (const unsigned n = (unsigned)%s;)" % key]
    if re.search(r"\b%s\b" % key, body.replace("(unsigned)" + key, "")):
        return ["generichash.cpp: HashInt uses its signed key '%s' after making the unsigned copy" % key]
    tm = re.search(r"g_nRandomValues\[256\]\s*=\s*\{(.*?)\}", src, re.S)
    table = [int(x) for x in re.findall(r"\d+", tm.group(1))] if tm else []
    if len(table) != 256:
        return ["generichash.cpp: g_nRandomValues table not read (%d entries)" % len(table)]
    steps = re.findall(r"(even|odd)\s*=\s*g_nRandomValues\[(.*?)\];", body)
    if len(steps) != 6 or any(not re.fullmatch(r"[\w\s^&|()<>]*", e) for _, e in steps):
        return ["generichash.cpp: HashInt's table reads changed shape - update check_hashint"]
    bad = []
    for k in (0, 1, 255, 0x7fffffff, -1, -2, -256, -0x80000000, 0x12345678, -0x12345678):
        env = {"n": k & 0xffffffff, "even": 0, "odd": 0}
        for var, expr in steps:
            i = eval(expr, {"__builtins__": {}}, env)
            if not 0 <= i < 256:
                bad.append("generichash.cpp: HashInt(%d) indexes g_nRandomValues[%d] (%s)" % (k, i, expr.strip()))
                break
            env[var] = table[i]
    return bad


def check_vehicle_body():
    """H1: the body's rock has one owner per kind of vehicle - tanks: the server spring (m_flSuspPitch/Vel, written only
    by the spring in DriveThink, BodyKick and HaltMotion; every blow / recoil goes through BodyKick, which networks it
    for a sprung body); sprung bodies (a model with a body_pivot): the client's BodySprings, the only writer of the body
    pitch/roll."""
    bad = []
    for path, var_re, allowed in ((VEHICLE_CPP_PATH, r"\bm_flSuspVel\s*(\+|-)?=(?!=)|\bm_flSuspPitch\s*=(?!=)",
                                   ("BodyKick", "DriveThink", "HaltMotion", "CDoDSPVehicle")),
                                  (CLIENT_VEHICLE, r"\bm_flBody(Pitch|Roll)(Vel)?\s*(\+|-)?=(?!=)", ("BodySprings", "C_DoDSPVehicle")),
                                  # T1: a suspension unit's lift - UpdateSuspension follows the ground, BuildWheelList starts it at 0
                                  (CLIENT_VEHICLE, r"\bflLift\s*(\+|-)?=(?!=)", ("UpdateSuspension", "BuildWheelList"))):
        func = None
        for n, line in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
            m = re.match(r"^(?:[A-Za-z][^(]*?[\s*&])?(?:CDoDSPVehicle|C_DoDSPVehicle)::(~?\w+)\s*\(", line)
            if m:
                func = m.group(1)
            code = line.split("//")[0]
            if re.search(var_re, code) and func not in allowed:
                bad.append("%s:%d (%s) writes the body's rock / a suspension unit's lift - kicks go through BodyKick, the "
                           "springs and UpdateSuspension are their only writers" % (os.path.basename(path), n, func))
    # H2b (owner 2026-11-21): an abandoned vehicle stays intact for a crew to take over - nothing sets it alight on a
    # timer (V4b did: m_flIgniteAt). Only a real fire (SetBurning from a hit) burns a vehicle.
    for n, line in enumerate(open(VEHICLE_CPP_PATH, encoding="utf-8").read().split("\n"), 1):
        m = re.search(r"\bm_flIgniteAt\s*=(?!=)([^;]*);", line.split("//")[0])
        if m and m.group(1).split("=")[-1].strip() not in ("0.0f", "0"):
            bad.append("dodsp_vehicle.cpp:%d sets an abandoned vehicle alight on a timer - abandoned vehicles stay intact (H2b)" % n)
    # T1: how far a wheel travels is the MODEL's (vehicle_rig.py measures it) - never a vehicle-type key
    if re.search(r'"susp_travel"', open(VEHICLE_SCRIPT, encoding="utf-8").read()):
        bad.append("dodsp_vehicles.txt has a susp_travel key - suspension travel is measured into the model (vehicle_rig.py)")
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
                     ("the vehicle damage state is written outside its owner (V4)", check_vehicle_damage),
                     ("the Hammer FGD doesn't match the vehicle types (V4j)", check_vehicle_fgd),
                     ("a rider seat or ride path is broken (V3)", check_vehicle_seats),
                     ("an NPC is moved by code outside CDoDSPNpcMotion (V3e: position and pose drawn equally late)", check_npc_motion),
                     ("the measured track table or its reader is broken (H1)", check_vehicle_treads),
                     ("the vehicle body's rock or suspension is written outside its owners (H1, T1)", check_vehicle_body),
                     ("a vehicle's physics shadow can take its moves back (H3b)", check_vehicle_push),
                     ("a vehicle can be blind (H3b)", check_vehicle_eyes),
                     ("tier1's HashInt can read outside its table (a negative key crashed the server)", check_hashint)):
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
