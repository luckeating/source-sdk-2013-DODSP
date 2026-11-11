# DOD-SP asset tools

Python helpers used to build DOD-SP model assets (not part of the game build).

- `smd.py` – minimal SMD reader/writer.
- `build_ww2_pack.py` – builds the WW2 NPC animation pack (`npc_ww2_anims.qc` + SMDs) from a
  Crowbar decompile of the CoD2 ranger port: DoD bone lengths, guns placed per frame through
  whichever hand holds the CoD gun (`gun_follow.py`), clean `ww2_*` sequence names,
  `ACT_WW2_*` / HL2 activities, upper-body pain overlays.

  `python3 build_ww2_pack.py <ranger_decompile_dir> <ranger.qc> <german_rifleman_reference.smd>
   <dod_player_shared_anim_@StandAim_BOLT.smd> <dod_player_shared_anim_@StandAim_MP40.smd> <out_dir>`
- `build_cod1_pack.py` – the CoD1 half (`npc_ww2_anims_cod1.qc`), retargeted from the 3ds Max biped
  with `retarget_cod1.py` / `cod1_setup.py` / `cod1_map.py`.

  `python3 build_cod1_pack.py <assault_decompile_dir> <dod_player_shared_dir> <german_rifleman_reference.smd>
   <ww2_skeleton.smd from the CoD2 pack> <out_dir>`
- `gun_follow.py` – per frame, decides who holds the source gun (right hand, left hand, nobody) and
  places DoD's gun bones through that hand, cross-fading between holders.
- `upper_layer.py` – the `ww2_upper` weightlist, upper-body overlay sequences, the `$ikchain` list
  (must match the German's, same order) and the hand IK "release" rules every CoD animation carries
  (DoD's own sequences lock the left hand to the right with `ikrule "lhand" touch`).
- `render_pose.py`, `render_compare.py` – forward kinematics + quick pose plots for checking retargets.
