# Vehicle animation previews (V4g, 2026-11-20)

Draw a crew/rider animation against the vehicle's real mesh, frame by frame, with the SAME timing and
positions the game code uses - before a build. Two blind builds missed on the tank bail-out; one picture
showed the fault (position right, timing wrong).

- `mesh_tris.py` - reads a compiled model's triangles (.mdl + .vvd + .dx90.vtx), in the in-game frame
  (CoD2 meshes face -Y; the root rotation turns them to +X). `heightmap()` = top-down height map, used to
  measure hatches and seats (dodsp_vehicles.txt "hatches").
- `bailout_preview.py <dir of mesh_tris.py> <dodsp_tools> <vehicle model dir> climb.smd jump.smd out.png` -
  the Panzer II commander's bail-out, front view. Its numbers mirror npc_german_soldier.cpp (BAILCLIMB_*,
  BAILJUMP_*) and dodsp_vehicle.cpp (SpawnCrewman's landing point): change them together.

V3 (riders, troop transports): render every seat's get-in / get-out like this and show the owner before building.

## V3 riders (2026-11-20)
- `rider_preview.py <dodsp_tools> <vehicle model dir> <ride pack dir> <out dir> [vehicle ...]` - every seat's ride
  idle and get-out against the real mesh: `<v>_getout.gif` (3/4 view, the code's timeline: seated 1 s, then each
  rider's start time, segments back to back, every sequence change blended over 0.2 s like the engine's fadein),
  `<v>_seats.png` (top + rear), `<v>_getout.png` (8 moments per seat) and `report.txt` (how deep any body point goes
  under the vehicle's top surface, seated and getting out; the landing correction). The seat list lives in the
  script for now - it moves to dodsp_vehicles.txt with the V3 code, and the numbers here must match it.
- `render3d.py` - the tiny depth-buffered renderer the GIFs use (riders inside a bay are hidden by its walls).
- The ride pack itself: `build_ww2_pack.py <officer decompile> german_normandy_officer.qc <rifleman ref> <bolt ref>
  <mp40 ref> <out> --want '<regex>' --model 'german\npc_ww2_anims_ride.mdl' --gunref <smd>` (the default run without
  --want still builds the ranger pack byte-identically).
- Rule from the search: the depth check only sees surfaces from ABOVE (a heightmap), so it flags thin walls a hand
  brushes and can't see a man floating - every seat is also checked for something under him (`top under seat`).

## H3 halftrack troops (2026-11-21)
- `troop_preview.py <dodsp_tools> <rig out dir> <ride pack dir> <main pack dir> <CoD2 pack dir> <out dir> [norender]` -
  the six bay riders, the rear doors on CoD2's curve, the door men's own get-outs, the aisle men standing into the
  guy02/guy02b walk (STAND_IN = the walk's 0.2 s fadein), the run to the rally spots. It SEARCHES the seats' delays
  (pelvis clearance CLEAR, and the lint's landing rule LAND_GAP / LAND_SPACING) - those are the "delay" values in
  dodsp_vehicles.txt. Change it and npc_german_soldier.cpp (RideGetOut's join frame, RidePosition's stand-in) together.
- `seat_death_preview.py` - every seated death at every seat (jump at the switch <= 3 units, depth into the mesh,
  overlap) and the "sitting" check for seats we place ourselves.

## V2b halftrack MG42 + gunner (2026-11-21)
- `mg42_model.py <CoD2 pack> <out>` - CoD2's mg42_with_bipod_test1 at its REAL size (CoD2's is 1.21x; gun_facts = the one owner of its size, butt, grip, muzzle) as `dodsp_mg42_pintle` (origin = the elevation pivot
  tag_aim, bones mg_base / mg_gun, pose parameter mg_pitch +-15, + = up, attachment "muzzle" = tag_flash). The owner
  compiles it; the material is CoD2's own name, models\cod2\mtl_weapon_mg42.
- `mg_gunner_compose.py <ride pack> <CoD2 pack> <cod2_vehicle_tags.txt> <DoD decompile> <out> [pack .qc]` - the gunner's
  aim poses (13 columns of mg_yaw x 4 rows of mg_pitch = 52, under studiomdl's 64 animations per sequence): DoD's own
  shouldered standing MG42 (aimmatrix_deployed_idle_MG_neutral), the butt (CoD2 tag_butt) in his shoulder pocket, his
  body turning with the gun about tag_turret (hips half, spine the rest, a lean out over the wall where his hips would
  go into it), legs IK to planted feet (squat for muzzle up, toes + shrug for muzzle down), hands IK onto the gun. And
  his death per column (CoD2's death_stand_dropinplace starting on his pose). Adds ww2_halftrack_mg / _mg_death to the .qc.
- `mg_preview.py <dodsp_tools> <rig out> <ride pack> <gunner poses> <MG42 model dir> <CoD2 pack> <DoD decompile> <out> [norender]` - the
  gun sweeping +-30 (bay empty) / +-20 (riders), the gunner blended the ENGINE's way (nlerp of local rotations: hands
  off the grips between poses measured), his death; clearances to the seated riders printed.
