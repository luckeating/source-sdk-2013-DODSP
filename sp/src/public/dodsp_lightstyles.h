//===================== DOD-SP light styles with a fixed meaning =====================//
//
// One owner for these numbers: VRAD (the map compiler), the server (which sets each style's
// brightness) and the client (which reports what a map was compiled with) all include this.
//
// Valve's layout: 0-12 the stock patterns (0 = always on), 32-62 handed out by VBSP to named
// (switchable) lights, 63 testing. 13-31 are unused, so DOD-SP takes 31 (sun) and 30 (live lamps).
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

// Live lamps (lighting stage 5): every light_spot the mapper marks "_dodsp_live" "1". Their direct light
// goes in this style, which the server turns off while the game draws the lamps itself (projected
// textures, c_dodsp_lamps.cpp) and on as the fallback (dodsp_lamps_baked / dodsp_lamps_fallback).
// Their bounce stays in style 0. VRAD keeps live lamps OUT of the world-lights lump (the engine
// would light every model with them a second time: it ignores light styles there - the sun's
// worldlight lights models at full strength with style 31 off) and writes them to the map's pak
// file instead (DODSP_LAMPS_FILE_*), one file per lighting pass.
// INVARIANT: only live lamps use this style, and every live lamp is in the pak file.
#define DODSP_LIGHTSTYLE_LAMPS	30
#define DODSP_LAMPS_FILE_LDR	"dodsp/lamps_ldr.txt"
#define DODSP_LAMPS_FILE_HDR	"dodsp/lamps_hdr.txt"

// The one answer to "does this style's light still bounce in style 0?" - the styles DOD-SP takes a
// light's DIRECT light out of, to draw it itself. Their bounce is indirect light and stays baked.
inline bool DoDSP_StyleBounceStays( int nStyle )
{
	return nStyle == DODSP_LIGHTSTYLE_SUN || nStyle == DODSP_LIGHTSTYLE_LAMPS;
}

#endif // DODSP_LIGHTSTYLES_H
