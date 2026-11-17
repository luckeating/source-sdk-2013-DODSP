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

#endif // DODSP_LIGHTSTYLES_H
