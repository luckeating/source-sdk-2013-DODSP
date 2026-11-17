//===================== DOD-SP sun: shared shader code =====================//
//
// One owner for how a cascade is read, used by the brush sun (dodsp_sun_fxc.h, stage 3b-2) and the
// model sun (dodsp_modelsun_ps_fxc.h, stage 4):
//   - the three cascade depth maps on samplers s13..s15 (near, mid, far; skin: s9..s11)
//   - the soft shadow (stage 4b: contact hardening) and the border test
//   - picking which cascade answers
//
// INVARIANT (4b): the cascades are RAW depth maps (NVIDIA: INTZ; elsewhere the card's own readable
// depth format), point sampled, and every read here is tex2Dlod + a compare in the shader. No
// hardware compare (tex2Dproj) and no SetShadowDepthFiltering on the cascade samplers - the blocker
// search needs the depth itself. The CI fails the build on either (dodsp_build_shaders.py).
//
// The soft shadow, per pixel:
//   1. blocker search: the average depth of whatever lies between the point and the sun, within the
//      widest penumbra we allow (DODSP_PCSS_REACH world units of caster distance)
//   2. penumbra radius (world units) = (receiver depth - blocker depth) * depth range * tan(sun radius):
//      sharp where things touch, softer the further the shadow falls
//   3. the filter over that radius
// 4b-2 (no grain): the owner found anything softer than a 0.6 degree sun grainy - 16 taps spread wide
// with a random turn per texel. Now every tap is a smooth 2x2 bilinear compare, the tap pattern is
// fixed (no noise at all), and each step reads the cascade whose texels make the radius only a few
// texels wide (DODSP_PCSS_FILTER_TEXELS / _SEARCH_TEXELS). A wide penumbra is read from a coarser
// cascade: it is blurry anyway, so the coarser map loses nothing, and the taps stay close enough
// together that they never show as separate copies of the edge. Radii are in world units, so the
// edge looks the same whichever cascade answers.
// The loops use tex2Dlod (no gradients) so they stay real loops inside dynamic branches, and one
// loop body reads whichever cascade was picked: shader model 3 has 512 instruction slots per
// shader, and three unrolled copies would not fit next to Mapbase's skin shader.
// Receiver-plane bias: every read compares against the receiver's own depth at that spot (the
// depth slope across the cascade, from ddx/ddy taken before any branch), so a sloped lit surface
// never shadows itself however wide the filter grows.
//
//=====================================================================//
#ifndef DODSP_SUN_COMMON_FXC_H
#define DODSP_SUN_COMMON_FXC_H

// Samplers s13..s15 unless the including shader picks others (the skin shader uses all 16 and
// puts the cascades on its unused wrinkle samplers - see dodsp_sun_shader.h DODSP_MSUN_SAMPLER_*)
#ifndef DODSP_SAMPLER_NEAR
#define DODSP_SAMPLER_NEAR	s13
#define DODSP_SAMPLER_MID	s14
#define DODSP_SAMPLER_FAR	s15
#endif
sampler DoDSP_CascadeNear			: register( DODSP_SAMPLER_NEAR );
sampler DoDSP_CascadeMid			: register( DODSP_SAMPLER_MID );
sampler DoDSP_CascadeFar			: register( DODSP_SAMPLER_FAR );

#define DODSP_PCSS_SEARCH_TAPS		12		// blocker search (point reads)
#define DODSP_PCSS_FILTER_TAPS		16		// the filter (each a 2x2 bilinear compare)
#define DODSP_PCSS_SEARCH_TEXELS	10.0f	// widest search radius, in texels of the cascade it reads
#define DODSP_PCSS_FILTER_TEXELS	4.0f	// widest filter radius, in texels of the cascade it reads
#define DODSP_PCSS_MIN_TEXELS		1.0f	// sharpest edge (contact): filter radius in the finest cascade's texels
#define DODSP_PCSS_REACH			512.0f	// caster distance (world units) the blocker search still finds
#define DODSP_PCSS_MAX_SLOPE		6.0f	// steepest receiver slope (tan) the plane bias follows

// Soft-shadow inputs that don't change within a draw
struct DoDSP_Soft_t
{
	float flRange;		// world units of the cascades' depth range (all three share it)
	float flTanSun;		// tan( sun's angular radius ) - the light_environment's SunSpreadAngle
	float flTexel;		// one cascade texel, in texture units (all three share the resolution)
};

// The receiver's depth slope across a cascade (depth per texture unit), from screen derivatives.
// Must be called outside any dynamic branch.
float2 DoDSP_DepthSlope( float3 c, float flScale, DoDSP_Soft_t soft )
{
	float3 dx = ddx( c );
	float3 dy = ddy( c );
	float det = dx.x * dy.y - dx.y * dy.x;
	float2 slope = float2( dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z ) / ( abs( det ) > 1e-12f ? det : 1e-12f );
	float flMax = DODSP_PCSS_MAX_SLOPE / max( flScale * soft.flRange, 1e-4f );	// tan limit, in depth per texture unit
	return clamp( slope, -flMax, flMax );
}

// Raw depth of cascade flCascade (0 near, 1 mid, 2 far) at uv. One body serves all three cascades:
// the cascade is the same for a whole patch of screen, so the branch costs next to nothing.
float DoDSP_ReadCascade( float flCascade, float2 uv )
{
	float4 t = float4( uv, 0.0f, 0.0f );
	float d;
	[branch] if ( flCascade < 0.5f )
	{
		d = tex2Dlod( DoDSP_CascadeNear, t ).x;
	}
	else
	{
		[branch] if ( flCascade < 1.5f )
		{
			d = tex2Dlod( DoDSP_CascadeMid, t ).x;
		}
		else
		{
			d = tex2Dlod( DoDSP_CascadeFar, t ).x;
		}
	}
	return d;
}

// Raw depths of the 2x2 texels whose centres are uv00, +x, +y, +xy - one branch for all four reads
float4 DoDSP_ReadCascade4( float flCascade, float2 uv00, float flTexel )
{
	float4 t0 = float4( uv00, 0.0f, 0.0f );
	float4 tx = float4( uv00 + float2( flTexel, 0.0f ), 0.0f, 0.0f );
	float4 ty = float4( uv00 + float2( 0.0f, flTexel ), 0.0f, 0.0f );
	float4 txy = float4( uv00 + float2( flTexel, flTexel ), 0.0f, 0.0f );
	float4 d;
	[branch] if ( flCascade < 0.5f )
	{
		d = float4( tex2Dlod( DoDSP_CascadeNear, t0 ).x, tex2Dlod( DoDSP_CascadeNear, tx ).x,
					tex2Dlod( DoDSP_CascadeNear, ty ).x, tex2Dlod( DoDSP_CascadeNear, txy ).x );
	}
	else
	{
		[branch] if ( flCascade < 1.5f )
		{
			d = float4( tex2Dlod( DoDSP_CascadeMid, t0 ).x, tex2Dlod( DoDSP_CascadeMid, tx ).x,
						tex2Dlod( DoDSP_CascadeMid, ty ).x, tex2Dlod( DoDSP_CascadeMid, txy ).x );
		}
		else
		{
			d = float4( tex2Dlod( DoDSP_CascadeFar, t0 ).x, tex2Dlod( DoDSP_CascadeFar, tx ).x,
						tex2Dlod( DoDSP_CascadeFar, ty ).x, tex2Dlod( DoDSP_CascadeFar, txy ).x );
		}
	}
	return d;
}

// One smooth tap: the compare results of the 2x2 texels around uv, blended by where uv sits between
// their centres (what NVIDIA's hardware compare does, done by hand on raw depth). z = receiver depth
// at uv; each texel centre is compared against the receiver's depth there (plane bias).
float DoDSP_BilinearLit( float flCascade, float2 uv, float z, float2 slope, float flTexel )
{
	float2 t = uv / flTexel - 0.5f;
	float2 f = frac( t );
	float2 uv00 = ( t - f + 0.5f ) * flTexel;	// centre of the lower-left texel of the block
	float4 d = DoDSP_ReadCascade4( flCascade, uv00, flTexel );
	float zc = z + dot( slope, uv00 - uv );		// receiver depth at that centre, then one texel over in x / y
	float4 zr = zc + float4( 0.0f, slope.x, slope.y, slope.x + slope.y ) * flTexel;
	float4 lit = ( zr <= d ) ? 1.0f : 0.0f;
	return lerp( lerp( lit.x, lit.y, f.x ), lerp( lit.z, lit.w, f.x ), f.y );
}

// Which cascade should answer a lookup of radius flWorld (world units): the finest one that holds
// the point and makes the radius at most flMaxTexels texels; if none does, the coarsest that holds it.
// ins = inside flags (near, mid, far), scales = texture units per world unit.
float DoDSP_PickCascade( float3 ins, float3 scales, float flWorld, float flMaxTexels, float flTexel )
{
	float3 fits = ins * ( ( flWorld * scales / flTexel <= flMaxTexels ) ? 1.0f : 0.0f );
	// the coarsest one holding the point = the smallest scale among those inside
	float3 s = lerp( float3( 1e30f, 1e30f, 1e30f ), scales, ins );
	float flCoarse = ( s.z <= s.y && s.z <= s.x ) ? 2.0f : ( ( s.y <= s.x ) ? 1.0f : 0.0f );
	return ( fits.x > 0.5f ) ? 0.0f : ( ( fits.y > 0.5f ) ? 1.0f : ( ( fits.z > 0.5f ) ? 2.0f : flCoarse ) );
}

float DoDSP_Select( float flCascade, float3 v )
{
	return ( flCascade < 0.5f ) ? v.x : ( ( flCascade < 1.5f ) ? v.y : v.z );
}

float3 DoDSP_Select3( float flCascade, float3 a, float3 b, float3 c )
{
	return ( flCascade < 0.5f ) ? a : ( ( flCascade < 1.5f ) ? b : c );
}

float2 DoDSP_Select2( float flCascade, float2 a, float2 b, float2 c )
{
	return ( flCascade < 0.5f ) ? a : ( ( flCascade < 1.5f ) ? b : c );
}

// The fixed tap pattern: a golden-angle spiral, tap k of n at radius sqrt((k + 0.5) / n) (0..1).
// The direction turns by the golden angle each tap (a rotation, no trig per tap); no per-pixel turn,
// so no grain.
#define DODSP_GOLDEN_COS	-0.7373688f
#define DODSP_GOLDEN_SIN	0.6754903f
float2 DoDSP_SpiralTurn( float2 dir )
{
	return float2( dir.x * DODSP_GOLDEN_COS - dir.y * DODSP_GOLDEN_SIN, dir.x * DODSP_GOLDEN_SIN + dir.y * DODSP_GOLDEN_COS );
}

// Blocker search in one cascade: the sum and count of the depths in front of the receiver
void DoDSP_SearchBlockers( float flCascade, float3 c, float2 slope, float flRadiusUV, out float flSum, out float flCount )
{
	flSum = 0.0f;
	flCount = 0.0f;
	float2 dir = float2( 1.0f, 0.0f );
	[loop] for ( int i = 0; i < DODSP_PCSS_SEARCH_TAPS; i++ )
	{
		float2 off = dir * ( flRadiusUV * sqrt( ( i + 0.5f ) / DODSP_PCSS_SEARCH_TAPS ) );
		dir = DoDSP_SpiralTurn( dir );
		float d = DoDSP_ReadCascade( flCascade, c.xy + off );
		float b = ( d < c.z + dot( slope, off ) ) ? 1.0f : 0.0f;
		flSum += d * b;
		flCount += b;
	}
}

// 1 if a cascade texture coordinate is inside the map (kept off the border for the filter)
float DoDSP_Inside( float2 uv )
{
	float2 edge = min( uv, 1.0f - uv );
	return ( min( edge.x, edge.y ) > 0.02f ) ? 1.0f : 0.0f;
}

// Sun visibility 0..1 from the three cascades' coordinates (xy = texture, z = depth, w = inside),
// with each cascade's scale (texture units per world unit). Outside every cascade counts as lit
// (open sky). flOn = 0: this draw takes no per-pixel sun, so the work is skipped.
float DoDSP_ShadowFromCascades( float4 cn, float4 cm, float4 cf, float3 scales, DoDSP_Soft_t soft, float flOn )
{
	// slopes first: screen derivatives can't be taken inside the branches below
	float2 gn = DoDSP_DepthSlope( cn.xyz, scales.x, soft );
	float2 gm = DoDSP_DepthSlope( cm.xyz, scales.y, soft );
	float2 gf = DoDSP_DepthSlope( cf.xyz, scales.z, soft );

	float3 ins = float3( cn.w, cm.w, cf.w );
	float flVis = 1.0f;
	[branch] if ( max( ins.x, max( ins.y, ins.z ) ) * flOn > 0.5f )
	{
		// the finest cascade holding the point sets the sharpest edge
		float flFinest = ( ins.x > 0.5f ) ? 0.0f : ( ( ins.y > 0.5f ) ? 1.0f : 2.0f );
		float flMinWorld = DODSP_PCSS_MIN_TEXELS * soft.flTexel / max( DoDSP_Select( flFinest, scales ), 1e-6f );

		// 1. blockers. Twice: close by in the finest cascade (thin casters right at the point - a chair
		// leg - are thinner than a coarse texel and must never be missed), and over the whole reach in
		// the cascade that makes it a few texels wide (far casters whose wide penumbra covers the point).
		// The fine search wins when it finds something: it measures contact distances best.
		float flSearchWorld = max( soft.flTanSun * DODSP_PCSS_REACH, 2.0f * flMinWorld );
		float3 c1 = DoDSP_Select3( flFinest, cn.xyz, cm.xyz, cf.xyz );
		float2 g1 = DoDSP_Select2( flFinest, gn, gm, gf );
		float flS1UV = min( flSearchWorld * DoDSP_Select( flFinest, scales ), DODSP_PCSS_SEARCH_TEXELS * soft.flTexel );
		float flSum1, flCount1;
		DoDSP_SearchBlockers( flFinest, c1, g1, flS1UV, flSum1, flCount1 );

		float flS2 = DoDSP_PickCascade( ins, scales, flSearchWorld, DODSP_PCSS_SEARCH_TEXELS, soft.flTexel );
		float3 c2 = DoDSP_Select3( flS2, cn.xyz, cm.xyz, cf.xyz );
		float flSum2 = 0.0f;
		float flCount2 = 0.0f;
		[branch] if ( flS2 != flFinest )
		{
			DoDSP_SearchBlockers( flS2, c2, DoDSP_Select2( flS2, gn, gm, gf ),
								  flSearchWorld * DoDSP_Select( flS2, scales ), flSum2, flCount2 );
		}

		[branch] if ( flCount1 + flCount2 > 0.5f )
		{
			// 2. penumbra (world units) from the average blocker's distance - the cascades share depth
			float flBlocker = ( flCount1 > 0.5f ) ? flSum1 / flCount1 : flSum2 / max( flCount2, 1.0f );
			float flRecv = ( flCount1 > 0.5f ) ? c1.z : c2.z;
			float flPenumbra = ( flRecv - flBlocker ) * soft.flRange * soft.flTanSun;
			float flRadiusWorld = clamp( flPenumbra, flMinWorld, flSearchWorld );

			// 3. the filter, in the cascade that makes it a few texels wide, every tap a smooth 2x2
			float flF = DoDSP_PickCascade( ins, scales, flRadiusWorld, DODSP_PCSS_FILTER_TEXELS, soft.flTexel );
			float3 cfil = DoDSP_Select3( flF, cn.xyz, cm.xyz, cf.xyz );
			float2 gfil = DoDSP_Select2( flF, gn, gm, gf );
			float flRadiusUV = flRadiusWorld * DoDSP_Select( flF, scales );
			float flLit = 0.0f;
			float2 dir = float2( 1.0f, 0.0f );
			[loop] for ( int j = 0; j < DODSP_PCSS_FILTER_TAPS; j++ )
			{
				float2 off = dir * ( flRadiusUV * sqrt( ( j + 0.5f ) / DODSP_PCSS_FILTER_TAPS ) );
				dir = DoDSP_SpiralTurn( dir );
				flLit += DoDSP_BilinearLit( flF, cfil.xy + off, cfil.z + dot( gfil, off ), gfil, soft.flTexel );
			}
			flVis = flLit / DODSP_PCSS_FILTER_TAPS;
		}
	}
	return flVis;
}

#endif // DODSP_SUN_COMMON_FXC_H
