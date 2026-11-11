# DOD-SP asset tools

Python helpers used to build DOD-SP model assets (not part of the game build).

- `smd.py` – minimal SMD reader/writer.
- `build_ww2_pack.py` – builds the WW2 NPC animation pack (`npc_ww2_anims.qc` + SMDs) from a
  Crowbar decompile of the CoD2 ranger port: DoD bone lengths, DoD weapon grips (both guns anchored
  to the right hand), clean `ww2_*` sequence names, `ACT_WW2_*` / HL2 activities, delta flinch gestures.

  `python3 build_ww2_pack.py <ranger_decompile_dir> <ranger.qc> <german_rifleman_reference.smd>
   <dod_player_shared_anim_@StandAim_BOLT.smd> <dod_player_shared_anim_@StandAim_MP40.smd> <out_dir>`
- `render_pose.py` – forward kinematics + quick pose plots for checking retargets.
