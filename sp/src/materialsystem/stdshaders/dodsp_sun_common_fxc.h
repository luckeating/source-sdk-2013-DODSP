//===================== DOD-SP sun: shared shader code =====================//
//
// One owner for how a cascade is read, used by the brush sun (dodsp_sun_fxc.h, stage 3b-2) and the
// model sun (dodsp_modelsun_ps_fxc.h, stage 4):
//   - the three cascade depth maps on samplers s13..s15 (near, mid, far)
//   - the depth compare (DODSP_SUNFILTER static combo: 0 = hardware compare, NVIDIA; 1 = raw depth)
//   - the soft-edge filter and the border test
//   - picking the finest cascade that holds the point, without branching (gradient-based texture
//     reads can't sit inside dynamic branches in shader model 3)
//
//=====================================================================//
#ifndef DODSP_SUN_COMMON_FXC_H
#define DODSP_SUN_COMMON_FXC_H

sampler DoDSP_CascadeNear			: register( s13 );
sampler DoDSP_CascadeMid			: register( s14 );
sampler DoDSP_CascadeFar			: register( s15 );

// 1 = lit, 0 = shadowed, for one depth-map texel neighbourhood
float DoDSP_Compare( sampler s, float2 uv, float z )
{
#if DODSP_SUNFILTER == 0
	return tex2Dproj( s, float4( uv, z, 1.0f ) ).x;		// hardware compare, bilinear between 4 texels
#else
	return ( z <= tex2D( s, uv ).x ) ? 1.0f : 0.0f;
#endif
}

// Four taps o apart (o in texture units): with the hardware compare each tap is already a 2x2
// bilinear compare, so the edge is about 3 texels soft.
float DoDSP_Filter( sampler s, float2 uv, float z, float o )
{
	return 0.25f * ( DoDSP_Compare( s, uv + float2( -o, -o ), z ) +
					 DoDSP_Compare( s, uv + float2(  o, -o ), z ) +
					 DoDSP_Compare( s, uv + float2( -o,  o ), z ) +
					 DoDSP_Compare( s, uv + float2(  o,  o ), z ) );
}

// 1 if a cascade texture coordinate is inside the map (kept off the border for the filter)
float DoDSP_Inside( float2 uv )
{
	float2 edge = min( uv, 1.0f - uv );
	return ( min( edge.x, edge.y ) > 0.02f ) ? 1.0f : 0.0f;
}

// Sun visibility 0..1 from the three cascades' coordinates (xy = texture, z = depth, w = inside):
// the finest cascade that holds the point wins; outside every cascade counts as lit (open sky).
float DoDSP_ShadowFromCascades( float4 cn, float4 cm, float4 cf, float o )
{
	float sn = DoDSP_Filter( DoDSP_CascadeNear, cn.xy, cn.z, o );
	float sm = DoDSP_Filter( DoDSP_CascadeMid, cm.xy, cm.z, o );
	float sf = DoDSP_Filter( DoDSP_CascadeFar, cf.xy, cf.z, o );

	float wn = cn.w;
	float wm = ( 1.0f - wn ) * cm.w;
	float wf = ( 1.0f - wn ) * ( 1.0f - cm.w ) * cf.w;
	float wOutside = 1.0f - wn - wm - wf;
	return sn * wn + sm * wm + sf * wf + wOutside;
}

#endif // DODSP_SUN_COMMON_FXC_H
