# DOD-SP port scripts (kept for reference)

These generated DOD-SP's shader copies from Mapbase's sources ONCE. The outputs are now kept by hand,
so don't re-run them over the current files - they show exactly what was changed from Mapbase.

- `make_helper.py` - stage 3b-2: `dodsp/dodsp_lightmappedgeneric_helper.cpp` from Mapbase's
  `lightmappedgeneric_dx9_helper.cpp`.
- `port_model_fxc.py` - stage 4: the six `dodsp_vertexlit*` / `dodsp_skin*` .fxc files from Mapbase's
  `SDK_*` ones (combos pinned, the model sun inserted).
- `port_model_helpers.py` - stage 4: `dodsp/dodsp_vertexlitgeneric_helper.cpp` and
  `dodsp/dodsp_skin_helper.cpp` from Mapbase's helpers (SM3 only, DOD-SP shader names, sun hooks,
  `DoDSP_Pinned` on every pinned combo).

Run from `sp/src/materialsystem/stdshaders`: `python port_model_fxc.py .`
