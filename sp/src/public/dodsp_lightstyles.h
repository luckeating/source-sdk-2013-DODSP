//===================== DOD-SP light styles with a fixed meaning =====================//
//
// One owner for these numbers: VRAD (the map compiler), the server (which sets each style's
// brightness) and the client (which reports what a map was compiled with) all include this.
//
// Valve's layout: 0-12 the stock patterns (0 = always on), 32-62 handed out by VBSP to named
// (switchable) lights, 63 testing. 13-31 are unused, so DOD-SP takes 31.
//
//===================================================================================//
#ifndef DODSP_LIGHTSTYLES_H
#define DODSP_LIGHTSTYLES_H

// The sun's direct light (light_environment), when the map is compiled with VRAD -dodsp_sunstyle.
// Its bounce, the sky ambient and every other light stay in style 0. The server turns this style
// on (stock baked look) or off (the game draws the sun itself) - dodsp_sun_baked.
// INVARIANT: only the light_environment's direct light may use this style.
#define DODSP_LIGHTSTYLE_SUN	31

// Static props (lighting stage 4d). With -dodsp_sunstyle AND -StaticPropLighting, VRAD bakes static
// prop vertex lighting WITHOUT the sun's direct light (vertex lighting has no light styles, so it can't
// be switched like the lightmaps), and the game adds the sun per pixel instead. VRAD marks the BSP's
// level flags (LUMP_MAP_FLAGS, next to Valve's LVLFLAGS_BAKED_STATIC_PROP_LIGHTING_*) so the client
// knows; a map without the mark keeps the sun in its props and the game never adds a second one.
// INVARIANT: the mark is set exactly when that pass baked static props without the direct sun.
#define DODSP_LVLFLAGS_PROPS_NOSUN_NONHDR	0x00010000
#define DODSP_LVLFLAGS_PROPS_NOSUN_HDR		0x00020000

#endif // DODSP_LIGHTSTYLES_H
