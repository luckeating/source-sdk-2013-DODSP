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
