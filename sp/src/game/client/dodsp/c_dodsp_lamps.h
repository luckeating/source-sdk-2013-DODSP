//===================== DOD-SP lamps =====================//
//
// One owner for the lamps (lighting stage 5). See c_dodsp_lamps.cpp.
//
//========================================================//
#ifndef C_DODSP_LAMPS_H
#define C_DODSP_LAMPS_H

class CViewSetup;

// Called by the main view (viewrender.cpp CBaseWorldView::DrawExecute, VIEW_MAIN) around its
// opaque pass: Begin binds the lamps' G-buffer as render targets 1..3 and returns true if it did;
// End (only after a true Begin) unbinds them before anything translucent draws.
bool DoDSP_LampsBeginGBuffer();
void DoDSP_LampsEndGBuffer();

// Called by the main view (viewrender.cpp RenderView) after the 2D debug overlays: draws the
// G-buffer on screen when dodsp_lamps_view is on (stage 5a debug).
void DoDSP_DrawLampsView( const CViewSetup &mainView );

#endif // C_DODSP_LAMPS_H
