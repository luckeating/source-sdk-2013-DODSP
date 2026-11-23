# German soldier: animation inventory

Written 2026-11-12. This lists every animation the soldier has, where it comes from, what the code uses today, and which
roadmap stage will use the rest. Keep it current when a pack or a schedule changes.

## Where animations come from

| Source | Model / file | Sequences | Notes |
|---|---|---|---|
| DoD:S player set | `dod_player_shared` (via `$includemodel`) | full set | Locomotion, idles, aim, reloads, prone, zoom, deploy, plant/defuse, hand signal. Mapped through the `"activities"` block in `scripts/npc_german_soldier.txt`. |
| CoD2 pack | `german/npc_ww2_anims.mdl`, built by `build_ww2_pack.py` | 138, prefix `ww2_` | Built from the CoD2 American Ranger SMDs (`Ranger.zip`, 338 files). List in `german_npc/ww2_anims/SEQUENCES.txt`. |
| CoD1 pack | `german/npc_ww2_anims_cod1.mdl`, built by `build_cod1_pack.py` | 281, prefix `c1_` | Built from the CoD1 `Assault.zip` (640 SMDs). List in `german_npc/ww2_anims_cod1/SEQUENCES_COD1.txt`. |

`npc_german_rifleman.qc` includes all three. **Animation events:** the CoD1 pack has only body-drop sounds on deaths.
Neither pack has a melee-hit, grenade-release or muzzle event. Melee and grenades need those added in the pack builders,
or a release frame timed in code.

## Used in code today

- **Locomotion, idle, aim, fire, reload** (stand, crouch, walk, run, sprint, crouch-walk): the DoD set through the
  script map. That's 49 Combine activities mapped to `ACT_DOD_*`.
- **Low and mid cover**: `ACT_WW2_COVER_LOW_HIDE/UP/DOWN/AUTO/SEMI/RELOAD` and `ACT_WW2_COVER_MID_UP/DOWN/AUTO/SEMI` (CoD2).
- **Corners**: CoD1 `ACT_WW2_CN_<weapon><stance>_<side>_<act>`.
  - Weapon is B (bolt rifle) or A (automatic). Stance is S or C. Side is L or R.
  - Uses ALERT, OUT, IN, ENTER, SEMI, AUTO and RELOAD, plus `CN_DEATH_<stance>_<side>`.
  - Aim angles are calibrated from the muzzle at load.
- **Pains**: `ACT_WW2_UPPER_PAIN_HEAD/GUT/LARM/RARM/LSHOULDER/RSHOULDER/HUNCH` and `UPPER_CROUCH_PAIN` as gestures.
  `ACT_WW2_CROUCH_PAIN` and `PAIN_FALLTOHAND` play as full-body pains, along with the stock `ACT_FLINCH_*`.
- **Running (A4a)**: CoD2 combat runs `ACT_WW2_RUN` (three variants, random), the hunched `ACT_WW2_CROUCH_RUN`
  for short hops between cover, and the helmet-in-hand `ACT_WW2_RUN_HELMET` under fire. All blend direction on
  `move_yaw`. DoD's run stays for patrols and as the fallback.
- **Pinned (A4a)**: `ACT_WW2_COVER_LOW_TWITCH` (flinching behind a low wall), CoD1 `GRENADEHIDE_LEFT/RIGHT` with
  `_IN`/`_OUT` (hunkered in the open, arms over his head), CoD1 `ACT_WW2_CROUCH_TWITCH` (two takes, a flinch
  between bursts). `ACT_WW2_IDLE_TWITCH` is built but not used yet.
- **Deaths**: `ACT_WW2_DEATH_RUN_*` (front, back, left, right, crumple, stumble, onfront, onleft),
  `DEATH_CROUCH_*` (headshot, hands, flip, fetal), `CROUCH_DEATH(_HEAD)` and `DEATH_CROUCHRUN`.
  The stock `ACT_DIE*` set (CoD2 standing deaths) also plays.

## Built, not used yet, by roadmap stage

### A4: suppression, grenades, wounded
- **Grenades**
  - Throws: `ACT_WW2_GRENADE` (CoD2 stand throw), CoD1 `GRENADE_THROW` and `CROUCH_GRENADE_THROW`.
  - Corner throws: `CN_*_GRENADE`, 12 sets.
  - Answering a grenade: `GRENADE_RETURN`, `CROUCH_GRENADE_RETURN`, `GRENADE_KICK`, and the dive-and-cover set
    `GRENADEHIDE_LEFT/RIGHT` with `_IN` and `_OUT`.
  - The header comment in `npc_german_soldier.cpp` that says grenades are off "until those animations exist" is out of
    date. They exist; only the release event is missing.
- **Prone (pinned)**
  - Going down and up: `CROUCH2PRONE`, `CROUCHRUN2PRONE`, `PRONE2CROUCH`.
  - Fighting prone: `PRONE_AIM`, `PRONE_SHOOT`, `PRONE_SHOOT_AUTO`, `PRONE_RELOAD`, `PRONE_RECHAMBER`.
  - Hit or killed prone: `PRONE_PAIN`, `DEATH_PRONE`.
  - Crawling is the DoD `ACT_DOD_PRONEWALK_*`, with DoD prone aim, reload and attack.
- **Hiding and suppression**
  - `HIDE_IDLE`, `HIDE_LOOK`, `HIDE_TWITCH`, `HIDE_RECHAMBER`, `HIDEA_IDLE`, `HIDEB_IDLE`, `HIDEB_TWITCH`.
  - Transitions: `STAND2HIDE`, `CROUCH2HIDE`, `AIM2HIDE`, `HIDE2AIM`, `HIDE2STAND`, `HIDEA2CROUCH`.
  - CoD2 `COVER_LOW_TWITCH`.
- **Reacting to fire**: `DODGE_LEFT`, `DODGE_RIGHT`, `STUMBLE`.
- **Wounded and rescue**
  - Wounded: `WOUNDED_DOWN`, `WOUNDED_HOLD`, `WOUNDED_UP`.
  - Downed: `DOWNED_FALL`, `DOWNED_IDLE`, `DOWNED_GETUP`, `DOWNED_DEATH`.
  - Fallen: `FALLEN_IDLE`, `FALLEN_GETUP`, `FALLEN_PICKUP`, `FALLEN_DEATH`.
  - Hurt movement: `RUN_HURT` (with `_LEFT` and `_RIGHT`) and `WALK_HURT`.
  - Carry rescue: `RESCUE_PICKUP/WALK/PUTDOWN` for the carrier, `RESCUED_PICKUP/WALK/PUTDOWN` for the man carried,
    and `RESCUE_DEATH` / `CARRIED_FALL` if it goes wrong.
  - Drag rescue (CoD2): `DRAG_ER_START/CYCLE/END` for the dragger, `DRAG_ED_START/IDLE/CYCLE/END/ENDIDLE` for the dragged.
  - Medic: `MEDIC_KNEEL`, `MEDIC_HEAL`, `MEDIC_HIDE`, `MEDIC_STAND`, `DEATH_MEDIC`.
- **Explosion deaths**
  - CoD1 `DEATH_EXPLODE_FORWARD/BACK/LEFT/RIGHT/UP`, and the CoD2 explosion deaths that come in as stock `ACT_DIE*`.
  - `DEATH_LEGHIT` and the CoD2 `DEATH_CORNER_*` are also unused (the code uses the CoD1 corner deaths).

### Movement stage: obstacle course
- Before any of these is wired: the CoD2 ones carry their motion in the animation (jump_down_56 travels 76 forward and
  57 down), the CoD1 ones are made in place - either way code moves him, so each goes through CDoDSPNpcMotion
  (dodsp_npc_motion.cpp) with an in-place animation + a path table (like the ride get-outs), and through
  dodsp_tools/motion_sim.py before a build. Their QCs mix fps 24 and 30: the code reads SequenceFps, never assumes.
- **Vaulting**
  - CoD2: `JUMP_LOWWALL`.
  - CoD1: `MANTLE_LOW`, `MANTLE_HIGH`, `TRAV_WALL`.
- **Climbing**
  - CoD2: `JUMP_FENCE`, `JUMP_WINDOW`.
  - CoD1: `TRAV_FENCE`, `TRAV_WINDOW`, `TRAV_CLIMB`.
- **Steps**: `TRAV_STEP_UP`, `TRAV_STEP_DOWN`.
- **Jumping down**
  - CoD2: `JUMP_DOWN40`, `JUMP_DOWN56`, `JUMP_DOWN96`.
  - CoD1: `JUMP_DOWN_LOW`, `JUMP_DOWN_HIGH`.
- **Crawl**: DoD prone-walk (see A4).
- **Turning on the spot**: `ACT_WW2_TURN_LEFT` and `ACT_WW2_TURN_RIGHT` (60° aim turns). These aren't wired, so he
  turns by yaw on the idle.

### Vehicles (V4c - used)
- CoD1 `TANK_CLIMB_OUT` (out of the hatch, made in place) and `TANK_JUMP_OFF` (off the side; the code carries him
  down): the tank-crew bail-out, `SCHED_GERMAN_BAILOUT` in npc_german_soldier.cpp.

### Squad leader and orders
- Hand signals (CoD1): `WAVE_COMEON`, `WAVE_GO`, `WAVE_GOGO`, `WAVE_FLANK`, `WAVE_WHAT`, and the DoD `ACT_DOD_HS_*`.
  - Today `ACT_SIGNAL_*` in the script map points at `STAND_AIM` as a placeholder.
  - Leader orders (assault, flank, stack, clear) should play these as gestures.

### Melee (stage 2e)
- CoD2: `ACT_WW2_MELEE`, `MELEEB`, `MELEEC` (rifle-butt swings).
- CoD1: `MELEE`, `MELEE_HIT`, `MELEE_CHARGE_HIT`, `CROUCH_MELEE`.
- `MeleeAttack1Conditions` returns nothing because no swing has a hit event. Add the event in the pack builder and this
  opens up.

### Corners (polish)
- CoD1 corners not wired: `LOOK` (peek without stepping out), `AIM`, `RECHAMBER` (bolt between shots), and the
  pistol `P` sets.
- Blind fire: `CN_RAMBO_READY/OUT/AUTO/SEMI/IN_L/R`. Good for suppression in A4.
- The CoD2 corner set (`ACT_WW2_CORNR_*` / `CORNL_*`, with angle variants A/B/C) is unused. The CoD1 sets replaced it.
- **Known issue**: in some corner sets the hands don't line up with the bonemerged rifle. Left for the
  animation-polish pass.

## Death animations and walls (done - 2026-11-14)

A death animation only plays if it has room. Before it starts, every sequence the death activity can pick
is posed at five points of the fall (up to the ragdoll hand-over) and a line is traced from the chest to
the head, chest, pelvis, hands and feet. Anything solid in the way (world, props, doors, windows; not
other soldiers) and he ragdolls straight away instead. While a death animation plays, a check every 0.1 s
traces pelvis to head and pelvis to chest; if that is blocked (a door swung shut, a prop moved), he
ragdolls on the spot. One owner: `DeathAnimFits` / `DeathSequenceFits` / `DeathBodyBlocked`. With
`npc_german_debug 1` the console says which body part would have hit what.

Slopes: the animations are made for flat ground. `DeathGroundFit` traces the ground under the body's
final pose (head, chest, pelvis, hands, feet), fits a plane through his feet and tilts the whole body
toward it as he falls (`UpdateDeathTilt`: upright at the start, lying along the ground at the hand-over),
so the ragdoll starts on the ground instead of half inside or above it. Too steep (over 30 degrees), too
lumpy (a body part more than 12 units off the plane) or a drop under the body -> plain ragdoll.

## Animation sweep (to do - logged 2026-11-13)

Gameplay with the A4a set is right; these are visual and wait for a full animation pass:

- **K98 swings during transitions** (most visible going into and out of the helmet run, cover to run, pains
  fading). Cause: DoD's skeleton hangs the K98 off the LEFT hand (`weapon_bone_left`); cross-fades blend each
  bone in its parent's space, so when the left hand is somewhere else relative to the rifle in the two poses
  (on the fore-end vs on the helmet / on the ground), the rifle travels through the air. The MP40 hangs off the
  right hand and is fine. A4a-4's client-side hold (rifle kept in the right hand during blends, rate-limited
  catch-up) helps but doesn't cure it. Options for the sweep:
  - give the K98 man a world model that bonemerges to `weapon_bone` (right hand) and generate matching
    BOLT sequences with `weapon_bone` animated, so every pose holds it from the right hand;
  - or do the cross-fade of the rifle in world space on the client using each blended sequence's own pose.
- **Upper-body pains over CoD runs**: switched off for now (kick + helmet run instead). The pains are rebased
  onto DoD's hips; CoD-run versions would need rebasing per base pose.
- **Corner sets**: hands / rifle not lining up in some sets (see above).
- **Big pose changes cross-fade in 0.2 s** (crouched cover straight into a sprint): can look rubbery; per-pair
  transition times or transition animations.

### MG team (with A6)
- CoD2 MG42: `MG42_CARRY_RUN`, `MG42_SETUP_PRONE`, `MG42_SETUP_WALL`, `MG42_AIM`, `MG42_FIRE`, `MG42PRONE_AIM`,
  `MG42PRONE_FIRE`.
- CoD2 .30 cal: `30CAL_HIP_IDLE`, `30CAL_HIP_FIRE` (with `L` and `R`), `30CAL_AIM`, `30CAL_FIRE`.
- Not built yet: the full aim/fire grids in `Ranger.zip`.
  - `standmg42gunner_*`: 5 yaws by 3 pitches.
  - `pronemg42gunner_*`: 5 by 3.
  - `stand30calgunner_*`: 5 by 3.
  - These would make a proper aim blend for an emplaced MG.

### Other built-but-unused
- `ACT_WW2_SURRENDER` (CoD2 loop): Germans giving up when broken (morale, A4).
- Bomb planting: `ACT_WW2_PLANT` and `PLANT_START` (CoD2), `PLANT_BOMB` (CoD1). Also the DoD `ACT_DOD_PLANT*` and
  `DEFUSE*`.
- `ACT_WW2_ROCKET_FIRE` and `ROCKET_DROP`: Panzerschreck and Panzerfaust.

## Source SMDs not built into any pack

**From `Ranger.zip` (CoD2)**
- MG aim/fire grids (see MG team above).
- Mortar crew: `mortar_aimguy_*` and `mortar_loadguy_*` (fire, pickup, wait idle).
- Flak 88 crew: aimer, ejecter, leader, loader and passer, each with idle, load-and-fire and twitch. Also the
  Flakpanzer gunner and its loaders.
- **Vehicle dismounts**: halftrack, German truck (8 seats), tank ride (jump into a combat run), Bren carrier,
  Kübelwagen, wagon, jeep. Use these for reinforcements arriving by truck.
- Shoot-while-moving
  - `stand_shoot_walk_back/left/right`, `stand_shoot_run_forward/back/left/right`.
  - `crouchwalk_loop`, `stand_walk_combat_loop_01`. (The combat runs and crouch runs are built now - A4a.)
  - DoD's walk-aim and run-aim cover most of this.
- Gun hold note (A4a): `gun_follow.py` now treats a gun carried one-handed (close to the right hand and moving
  rigidly with it) as held by the right hand. Before, those frames counted as "free" and the gun floated off the
  hand; this changed 87 CoD1 animations (mostly corner sets, hide, downed) and ~20 CoD2 ones.
- Idles and reloads: `stand_alert_1/2/3`, `crouch_alert_a/b`, `reload_stand/crouch_rifle`, `pistol_stand_reload`.
- Car-chase seats: drive and get-out.

**From `Assault.zip` (CoD1)**
- LMG gunner: `lmg_fire`, `lmg_idle`, `lmg_prone`, `lmg_prone_idle`, `lmg_run`.
- Emplaced MG blends: `mg42p`, `mg42s`, `b30s`.
- Rocket: aim, cover, fire, reload. Also flamethrower.
- Pistol: the `p_*` set.
- **Unarmed**: the `ua_*` set, including cower and dazed. Use for prisoners and civilians.
- Tank crew: climb out, jump off. Paratroopers: chute.
- Ceremony: attention, present arms, an officer checking papers, binoculars. Use for officer and NCO scenes and
  scripted sequences.
- Extra deaths: more crouch deaths, `death_dropinplace`, `death_neck_thrash`.
- Facial: `face_*` animations.
- Locomotion and idles: DoD covers these, so they aren't needed.

## To build a missing set

1. Add the SMDs and their activity names to the right builder: `build_ww2_pack.py` (CoD2) or `build_cod1_pack.py` /
   `cod1_map.py` (CoD1).
2. Add any events the code needs, such as melee hit, grenade release or MG muzzle.
3. Rebuild the pack, recompile `npc_german_rifleman.qc`, and regenerate the matching `SEQUENCES*.txt`.


## The two main packs - THE commands (recovered 2026-11-22, N1)

German reference = the rifleman's compiled bones (the original reference SMD is gone):

    python3 -I rifleman_reference.py <npc_german_rifleman.mdl> <dod_player_shared_anim_a_reference.smd> <ref.smd>
    python3 -I build_ww2_pack.py <Ranger decompile> american_ranger_normandy__2_2.qc <ref.smd>
        <dod_player_shared_anim_@StandAim_BOLT.smd> <dod_player_shared_anim_@StandAim_MP40.smd> <ww2 out>
    python3 -I build_cod1_pack.py <Assault decompile> <dod_player_shared dir> <ref.smd> <ww2 out>/ww2_skeleton.smd <cod1 out>

Before N1 these reproduced the delivered packs (CoD1: every SMD exactly but the unused L_Knee; CoD2: within 2e-5).
Since N1 both builders solve the left arm onto DoD's K98 grip (support_grip.py) and stop over its bar; each writes
SUPPORT_GRIP.txt / SUPPORT_GRIP_COD1.txt (per animation: frames held two-handed, worst miss before, after).

## The ride pack (npc_ww2_anims_ride.mdl) - THE command (recovered 2026-11-20: it reproduces the delivered pack byte for byte)

Source: the owner's Crowbar decompile of the CoD2 officer port (`german_normandy_officer.qc` + SMDs), the German rifleman
reference and DoD's StandAim_BOLT / StandAim_MP40. The `--want` list names the officer QC's cod2_* sequences exactly
(V3's 39):

    python3 build_ww2_pack.py <officer decompile dir> german_normandy_officer.qc <german_rifleman_reference.smd>
        <dod_player_shared_anim_@StandAim_BOLT.smd> <dod_player_shared_anim_@StandAim_MP40.smd> <out dir>
        --want '^(cod2_ride1_idle|cod2_ride2_idle|cod2_ride3_idle|cod2_ride4_idle|cod2_ride6_idle|cod2_jump_ride1|cod2_jump_ride2|cod2_jump_ride3|cod2_jump_ride4|cod2_jump_ride6|cod2_vi_germantruck_driver_sitidle_loop|cod2_jump_germantruck_driver_climbout|cod2_jump_germantruck_guy1_jumpout|cod2_jump_germantruck_guy2_jumpout|cod2_jump_germantruck_guy3_jumpout|cod2_jump_germantruck_guy4_jumpout|cod2_jump_germantruck_guy5_jumpout|cod2_jump_germantruck_guy6_jumpout|cod2_jump_germantruck_guy7_jumpout|cod2_jump_germantruck_guy8_jumpout|cod2_vi_halftrack_driver_idle|cod2_jump_halftrack_guy02_jumpout|cod2_vi_halftrack_guy03_idle|cod2_jump_halftrack_guy03_jumpout|cod2_vi_halftrack_guy05_idle|cod2_jump_halftrack_guy05_jumpout|cod2_vi_halftrack_guy06_idle|cod2_vi_halftrack_guy04_idle|cod2_jump_halftrack_guy02b_jumpout|cod2_vi_halftrack_guy01_idle|cod2_vi_halftrack_guy02_idle|cod2_vi_bren_carrier_driver_driveidle|cod2_jump_bren_carrier_driver_dismount|cod2_vi_bren_carrier_passenger_rideidle|cod2_jump_bren_carrier_passenger_dismount|cod2_vi_bren_carrier_back01_rideidle|cod2_jump_bren_carrier_back01_combatdismount|cod2_vi_bren_carrier_back02_rideidle|cod2_jump_bren_carrier_back02_ridedismount)$'
        --model 'german\npc_ww2_anims_ride.mdl' --gunref german_normandy_officer_anim_a_stand_pain_front_lshoulder.smd
        --ride-paths <out>/dodsp_ride_paths.txt

New sequences are ADDED to that list (never replace it) and the rebuild is checked: every existing SMD byte-identical,
dodsp_ride_paths.txt unchanged. Animations the port doesn't have come from CoD2's raw xanims
(vehicle_preview/cod2_xanim.py + xanim_to_smd.py convert -> SMDs in the port's own format, appended to a copy of the
officer QC as `$animation "a_<name>"` + `$sequence "cod2_vi_<name>"`).

H2 (2026-11-21): after the ride pack command, the halftrack driver's steering:

    python3 vehicle_preview/steer_compose.py <pack out dir> <pack out dir> <pack out dir>/npc_ww2_anims_ride.qc

writes the two one-frame steering poses and adds (once) $poseparameter "steer" + $sequence "ww2_halftrack_driver"
(steer_right / halftrack_driver_idle / steer_left on steer -1..1). The pack's 9 driver animations come from the
owner's CoD2 xanims (vehicle_preview/xanim_to_smd.py convert) as recorded above.

H2b (2026-11-21): the halftrack commander's seat + seated deaths. CoD2 xanims converted (xanim_to_smd.py convert):
halftrack_passenger_idle, halftrack_guy_death1/2, wagon_passenger_death, eldaba_jeep_passenger_death; appended to the
officer QC as cod2_vi_<name>; the pack rebuilt into a SCRATCH folder with the --want list + the 9 H2 names + these 5,
German reference = in_DoD/dod_player_shared_anim_a_reference.smd (reproduces the delivered pack within 1e-5 units); only
the chosen new SMDs (ww2_a_halftrack_passenger_idle, ww2_a_halftrack_guy_death2) and their QC blocks were added to the
delivered pack, so every existing SMD stays byte-identical. Previewed with driver_preview.py at tag_passenger 3.4 -14.5 36.4.

H3 (2026-11-21): seated deaths, ANCHORED (xanim_to_smd.py --anchor / --start-at - see its header):

    xanim_to_smd.py convert <map.npz> <cod2 pack> <officer decompile> halftrack_guy_death1 <decompile>/german_normandy_officer_anim_a_halftrack_bay_death1.smd --anchor halftrack_guy01_idle ... halftrack_guy06_idle
    (same for halftrack_guy_death2 -> ..._bay_death2)
    xanim_to_smd.py convert ... halftrack_driver_death <decompile>/..._halftrack_driver_death.smd --anchor halftrack_driver_idle
    xanim_to_smd.py convert ... halftrack_guy_death2 <decompile>/..._halftrack_commander_death.smd --start-at <decompile>/..._halftrack_passenger_idle.smd

appended to the officer QC as cod2_vi_halftrack_bay_death1/2 and cod2_vi_halftrack_commander_death; the pack rebuilt with
the --want list + those three (every existing SMD within 1e-4 except driver_death, re-anchored; ride paths unchanged).
Checked with vehicle_preview/seat_death_preview.py (jump <= 3 units at every seat).

V2b (2026-11-21): the halftrack's MG42 gunner. CoD2 xanims converted (xanim_to_smd.py convert, into the SCRATCH decompile):
halftrack_mggunner_idle (no anchor) -> cod2_vi_halftrack_mggunner_idle, death_stand_dropinplace --start-at that idle ->
cod2_vi_halftrack_mg_deathdrop; a scratch pack built with the --want list + cod2_mg42_aim (the ground grid, a cross-check
only) + those two. Then

    python3 vehicle_preview/mg_gunner_compose.py <scratch pack> <CoD2 pack> generated/cod2_vehicle_tags.txt <delivered pack dir> <delivered pack dir>/npc_ww2_anims_ride.qc

writes the 52 aim poses + 13 death columns straight into the delivered pack and adds (once) $poseparameter mg_yaw /
mg_pitch, ww2_halftrack_mg and ww2_halftrack_mg_death (every existing file byte-identical). Previewed with
vehicle_preview/mg_preview.py; the gun is vehicle_preview/mg42_model.py (dodsp_mg42_pintle, the owner compiles it too).
V2b's delivered pack: dodsp_npc_ww2_anims_ride_v2b.zip.

After delivering a ride pack: regenerate dodsp_tools/generated/ride_pack_sequences.txt from its .qc (every $sequence name;
the lint checks the vehicle script's seats against it). Latest delivered pack: dodsp_npc_ww2_anims_ride_v2b.zip (H2b's was _h2b2).
