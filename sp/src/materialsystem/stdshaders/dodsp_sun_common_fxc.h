//===================== DOD-SP sun: shared shader code =====================//
//
// One owner for how a cascade is read, used by the brush sun (dodsp_sun_fxc.h, stage 3b-2) and the
// model sun (dodsp_modelsun_ps_fxc.h, stage 4):
//   - the three cascade depth maps on samplers s13..s15 (near, mid, far; skin: s9..s11)
//   - the soft shadow (stage 4b: contact-hardening "PCSS") and the border test
//   - picking the finest cascade that holds the point
//
// Stage 4b. INVARIANT: the cascades are RAW depth maps (NVIDIA: INTZ; elsewhere the card's own
// readable depth format), point sampled, and every read here is tex2Dlod + a compare in the shader.
// No hardware compare (tex2Dproj) and no SetShadowDepthFiltering on the cascade samplers - the
// blocker search needs the depth itself. The CI fails the build on either (dodsp_build_shaders.py).
//
// The soft shadow, per pixel, in the finest cascade that holds it:
//   1. blocker search: average depth of whatever lies between the pixel and the sun, within the
//      widest penumbra we allow (DODSP_PCSS_REACH world units of caster distance)
//   2. penumbra radius = (receiver depth - blocker depth) * depth range * tan(sun radius)
//      - sharp where things touch, softer the further the shadow falls. World units, so the
//      edge matches where one cascade hands over to the next.
//   3. that many world units of filter (never less than the minimum radius, about a texel and a
//      half, so no stair steps)
// Taps sit on a golden-angle spiral, turned per texel so the pattern doesn't band; the loops use
// tex2Dlod (no gradients), so they stay real loops inside the dynamic branches, and one loop body
// reads whichever cascade holds the point (shader model 3 has 512 instruction slots per shader -
// three unrolled copies would not fit next to Mapbase's skin shader).
// Receiver-plane bias: each tap compares against the receiver's own depth at that tap (the depth
// slope across the cascade, from ddx/ddy before any branch), so a sloped lit surface never shadows
// itself however wide the filter grows.
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

#define DODSP_PCSS_SEARCH_TAPS	12		// blocker search
#define DODSP_PCSS_FILTER_TAPS	16		// the filter
#define DODSP_PCSS_REACH		512.0f	// caster distance (world units) the blocker search still finds
#define DODSP_PCSS_MAX_SLOPE	6.0f	// steepest receiver slope (tan) the plane bias follows

// Soft-shadow inputs that don't change within a draw
struct DoDSP_Soft_t
{
	float flRange;		// world units of the cascades' depth range (all three share it)
	float flTanSun;		// tan( sun's angular radius ) - the light_environment's SunSpreadAngle
	float flMinRadius;	// smallest filter radius, cascade texture units (about 1.5 texels)
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

// Raw depth of cascade nCascade (0 near, 1 mid, 2 far) at uv. One loop body serves all three
// cascades: the cascade is the same for a whole patch of screen, so the branch costs next to nothing,
// and the shader's instruction slots (512 in shader model 3) don't triple.
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

// Visibility 0..1 of one point in one cascade. c = (u, v, receiver depth minus bias).
float DoDSP_SoftShadow( float flCascade, float3 c, float2 slope, float flScale, DoDSP_Soft_t soft )
{
	// per-texel turn of the tap spiral (stable in the world: it follows the cascade's texels)
	float2 px = c.xy * 4096.0f;
	float flTurn = 6.2831853f * frac( 52.9829189f * frac( 0.06711056f * px.x + 0.00583715f * px.y ) );
	float2 dir0 = float2( cos( flTurn ), sin( flTurn ) );
	const float2 rotA = float2( -0.7373688f, 0.6754903f );	// cos / sin of the golden angle

	// 1. blockers within the widest penumbra we allow
	float flSearch = max( soft.flTanSun * DODSP_PCSS_REACH * flScale, 2.0f * soft.flMinRadius );
	float flSum = 0.0f;
	float flCount = 0.0f;
	float2 dir = dir0;
	[loop] for ( int i = 0; i < DODSP_PCSS_SEARCH_TAPS; i++ )
	{
		float2 off = dir * ( flSearch * sqrt( ( i + 0.5f ) / DODSP_PCSS_SEARCH_TAPS ) );
		float d = DoDSP_ReadCascade( flCascade, c.xy + off );
		float b = ( d < c.z + dot( slope, off ) ) ? 1.0f : 0.0f;
		flSum += d * b;
		flCount += b;
		dir = float2( dir.x * rotA.x - dir.y * rotA.y, dir.x * rotA.y + dir.y * rotA.x );
	}

	// 2. penumbra from the average blocker's distance, in world units, then this cascade's texture units
	float flBlocker = flSum / max( flCount, 1.0f );
	float flPenumbra = ( c.z - flBlocker ) * soft.flRange * soft.flTanSun * flScale;
	float flRadius = clamp( flPenumbra, soft.flMinRadius, max( flSearch, soft.flMinRadius ) );

	// 3. the filter (skipped when the search found nothing between this point and the sun)
	float flLit = DODSP_PCSS_FILTER_TAPS;
	[branch] if ( flCount > 0.5f )
	{
		flLit = 0.0f;
		dir = dir0;
		[loop] for ( int j = 0; j < DODSP_PCSS_FILTER_TAPS; j++ )
		{
			float2 off = dir * ( flRadius * sqrt( ( j + 0.5f ) / DODSP_PCSS_FILTER_TAPS ) );
			float d = DoDSP_ReadCascade( flCascade, c.xy + off );
			flLit += ( c.z + dot( slope, off ) <= d ) ? 1.0f : 0.0f;
			dir = float2( dir.x * rotA.x - dir.y * rotA.y, dir.x * rotA.y + dir.y * rotA.x );
		}
	}
	return flLit / DODSP_PCSS_FILTER_TAPS;
}

// 1 if a cascade texture coordinate is inside the map (kept off the border for the filter)
float DoDSP_Inside( float2 uv )
{
	float2 edge = min( uv, 1.0f - uv );
	return ( min( edge.x, edge.y ) > 0.02f ) ? 1.0f : 0.0f;
}

// Sun visibility 0..1 from the three cascades' coordinates (xy = texture, z = depth, w = inside),
// with each cascade's scale (texture units per world unit): the finest cascade that holds the point
// wins; outside every cascade counts as lit (open sky). flOn = 0: this draw takes no per-pixel sun,
// so the filter is skipped (its result would be multiplied by 0).
float DoDSP_ShadowFromCascades( float4 cn, float4 cm, float4 cf, float3 scales, DoDSP_Soft_t soft, float flOn )
{
	// slopes first: screen derivatives can't be taken inside the branches below
	float2 gn = DoDSP_DepthSlope( cn.xyz, scales.x, soft );
	float2 gm = DoDSP_DepthSlope( cm.xyz, scales.y, soft );
	float2 gf = DoDSP_DepthSlope( cf.xyz, scales.z, soft );

	// pick the cascade without branching, then filter once
	float flCascade = ( cn.w > 0.5f ) ? 0.0f : ( ( cm.w > 0.5f ) ? 1.0f : 2.0f );
	float3 c = ( cn.w > 0.5f ) ? cn.xyz : ( ( cm.w > 0.5f ) ? cm.xyz : cf.xyz );
	float2 g = ( cn.w > 0.5f ) ? gn : ( ( cm.w > 0.5f ) ? gm : gf );
	float flScale = ( cn.w > 0.5f ) ? scales.x : ( ( cm.w > 0.5f ) ? scales.y : scales.z );
	float flInside = max( cn.w, max( cm.w, cf.w ) );

	float flVis = 1.0f;
	[branch] if ( flInside * flOn > 0.5f )
	{
		flVis = DoDSP_SoftShadow( flCascade, c, g, flScale, soft );
	}
	return flVis;
}

#endif // DODSP_SUN_COMMON_FXC_H
