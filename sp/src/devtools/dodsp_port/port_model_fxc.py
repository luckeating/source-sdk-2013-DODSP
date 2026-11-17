"""Generates DOD-SP's model shader sources (stage 4) from Mapbase's, once. Kept by hand afterwards."""
import re, sys, os
D = sys.argv[1]

HDR = '''//===================== DOD-SP copy of Mapbase's {src} (lighting stage 4) =====================//
// Shader model 3.0 only. Changes from Mapbase: combos for features DOD-SP's model shaders leave out are
// pinned to 0 (materials using them are handed back to Mapbase's SDK_ shader by SHADER_FALLBACK, see
// dodsp_vertexlitgeneric.cpp), and the sun drawn per pixel (search "DOD-SP"; dodsp_modelsun_*_fxc.h).
//=====================================================================================================//
'''

def rebuild(src, dst, combos, edits):
    s = open(os.path.join(D, src), encoding='latin-1').read()
    lines = s.split('\n')
    # header block: from the first combo/skip comment line to the line before the first #include
    first = next(i for i, l in enumerate(lines) if re.match(r'\s*//\s*(STATIC|DYNAMIC|SKIP)\s*:', l))
    inc = next(i for i, l in enumerate(lines) if l.startswith('#include'))
    assert first < inc, src
    body = '\n'.join(lines[:first]) + '\n' + combos.strip('\n') + '\n\n' + '\n'.join(lines[inc:])
    for old, new, n in edits:
        if isinstance(old, re.Pattern):
            body, k = old.subn(new, body)
        else:
            k = body.count(old)
            body = body.replace(old, new)
        assert k == n, (src, getattr(old, 'pattern', old)[:60], k, n)
    left = re.findall(r'^\s*//\s*(?:STATIC|DYNAMIC|SKIP)\s*:.*$', body[len(combos)+200:], re.M)
    assert not left, (src, left)
    open(os.path.join(D, dst), 'w', encoding='latin-1', newline='\n').write(HDR.format(src=src) + body)
    print('wrote', dst)

VS_OUT = '\tfloat4 dodspSun\t\t\t\t\t\t\t\t: TEXCOORD9;\t// DOD-SP: light-space position for the per-pixel sun\n'

def struct_add(name, field):
    """regex edit: add a field before the closing brace of struct <name>"""
    return (re.compile(r'(struct ' + name + r'\s*\{.*?)(\n\};)', re.S), lambda m: m.group(1) + '\n' + field.rstrip('\n') + m.group(2), 1)

# ---------------------------------------------------------------- plain (vertex lit) VS
rebuild('SDK_vertexlit_and_unlit_generic_vs20.fxc', 'dodsp_vertexlit_and_unlit_generic_vs30.fxc', '''
//  STATIC: "VERTEXCOLOR"				"0..1"
//  STATIC: "CUBEMAP"					"0..1"
//  STATIC: "HALFLAMBERT"				"0..1"
//  STATIC: "FLASHLIGHT"				"0..1"
//  STATIC: "SEAMLESS_BASE"				"0..0"
//  STATIC: "SEAMLESS_DETAIL"			"0..0"
//  STATIC: "SEPARATE_DETAIL_UVS"		"0..1"
//  STATIC: "DECAL"						"0..1"
//  STATIC: "SM30_VERTEXID"				"0..1"
//  STATIC: "DONT_GAMMA_CONVERT_VERTEX_COLOR" "0..1"
//  STATIC: "TREESWAY"					"0..0"
//  DYNAMIC: "COMPRESSED_VERTS"			"0..1"
//  DYNAMIC: "DYNAMIC_LIGHT"			"0..1"
//  DYNAMIC: "STATIC_LIGHT"				"0..1"
//  DYNAMIC: "DOWATERFOG"				"0..1"
//  DYNAMIC: "SKINNING"					"0..1"
//  DYNAMIC: "LIGHTING_PREVIEW"			"0..0"
//  DYNAMIC: "MORPHING"					"0..1"

// SKIP: ($DONT_GAMMA_CONVERT_VERTEX_COLOR && ( ! $VERTEXCOLOR ) )
''', [
    ('#include "common_vs_fxc.h"\n', '#include "common_vs_fxc.h"\n#include "dodsp_modelsun_vs_fxc.h"\t\t// DOD-SP: the sun (stage 4)\n', 1),
    struct_add('VS_OUTPUT', VS_OUT),
    ('\to.worldSpaceNormal = worldNormal;\n', '\to.worldSpaceNormal = worldNormal;\n\to.dodspSun = DoDSP_ModelSunCoord( worldPos, worldNormal, bStaticLight );\t// DOD-SP\n', 1),
    (re.compile(r'\bDoLightingUnrolled\('), 'DoDSP_DoLightingUnrolled(', 1),
    (re.compile(r'(?<![\w])DoLighting\('), 'DoDSP_DoLighting(', 1),
])

# ---------------------------------------------------------------- bump VS
rebuild('SDK_vertexlit_and_unlit_generic_bump_vs20.fxc', 'dodsp_vertexlit_and_unlit_generic_bump_vs30.fxc', '''
//  STATIC: "HALFLAMBERT"				"0..1"
//  STATIC: "USE_WITH_2B"				"0..1"
//  STATIC: "DECAL"						"0..1"
//  STATIC: "SM30_VERTEXID"				"0..1"
//  DYNAMIC: "COMPRESSED_VERTS"			"0..1"
//  DYNAMIC: "DOWATERFOG"				"0..1"
//  DYNAMIC: "SKINNING"					"0..1"
//  DYNAMIC: "MORPHING"					"0..1"
''', [
    ('#include "common_vs_fxc.h"\n', '#include "common_vs_fxc.h"\n#include "dodsp_modelsun_vs_fxc.h"\t\t// DOD-SP: the sun (stage 4)\n', 1),
    struct_add('VS_OUTPUT', VS_OUT),
    ('\to.vWorldNormal.xyz = worldNormal.xyz;\n', '\to.vWorldNormal.xyz = worldNormal.xyz;\n\to.dodspSun = DoDSP_ModelSunCoord( worldPos, worldNormal, false );\t// DOD-SP\n', 1),
    (re.compile(r'(?<![\w])GetVertexAttenForLight\('), 'DoDSP_GetVertexAttenForLight(', 8),
])

# ---------------------------------------------------------------- skin VS
rebuild('SDK_skin_vs20.fxc', 'dodsp_skin_vs30.fxc', '''
//  STATIC: "DECAL"						"0..1"
//  DYNAMIC: "COMPRESSED_VERTS"			"0..1"
//  DYNAMIC: "DOWATERFOG"				"0..1"
//  DYNAMIC: "SKINNING"					"0..1"
//  DYNAMIC: "LIGHTING_PREVIEW"			"0..0"
//  DYNAMIC: "MORPHING"					"0..1"
''', [
    ('#include "common_vs_fxc.h"\n', '#include "common_vs_fxc.h"\n#include "dodsp_modelsun_vs_fxc.h"\t\t// DOD-SP: the sun (stage 4)\n', 1),
    struct_add('VS_OUTPUT', VS_OUT),
    ('\to.tangentSpaceTranspose[2] = float3( worldTangentS.z, worldTangentT.z, worldNormal.z );\n',
     '\to.tangentSpaceTranspose[2] = float3( worldTangentS.z, worldTangentT.z, worldNormal.z );\n\to.dodspSun = DoDSP_ModelSunCoord( worldPos, worldNormal, false );\t// DOD-SP\n', 1),
    (re.compile(r'(?<![\w])GetVertexAttenForLight\('), 'DoDSP_GetVertexAttenForLight(', 8),
])

PS_IN = '\tfloat4 dodspSun\t\t\t\t\t\t\t\t: TEXCOORD9;\t// DOD-SP: from the vertex shader (dodsp_modelsun_vs_fxc.h)\n'

def sun_consts(r0, why):
    return ''.join('const float4 g_DoDSP_MSun%d\t\t\t\t: register( c%d );\t// DOD-SP sun%s\n' % (k, r0 + k, why if k == 0 else '') for k in range(4)) + \
           '#include "dodsp_modelsun_ps_fxc.h"\n'

# ---------------------------------------------------------------- plain PS
rebuild('SDK_vertexlit_and_unlit_generic_ps20b.fxc', 'dodsp_vertexlit_and_unlit_generic_ps30.fxc', '''
// STATIC: "DETAILTEXTURE"				"0..1"
// STATIC: "CUBEMAP"					"0..1"
// STATIC: "DIFFUSELIGHTING"			"0..1"
// STATIC: "ENVMAPMASK"					"0..1"
// STATIC: "BASEALPHAENVMAPMASK"		"0..1"
// STATIC: "SELFILLUM"					"0..1"
// STATIC: "VERTEXCOLOR"				"0..1"
// STATIC: "FLASHLIGHT"					"0..1"
// STATIC: "SELFILLUM_ENVMAPMASK_ALPHA" "0..1"
// STATIC: "DETAIL_BLEND_MODE"          "0..1"
// STATIC: "SEAMLESS_BASE"              "0..0"
// STATIC: "SEAMLESS_DETAIL"            "0..0"
// STATIC: "DISTANCEALPHA"              "0..0"
// STATIC: "DISTANCEALPHAFROMDETAIL"    "0..0"
// STATIC: "SOFT_MASK"                  "0..0"
// STATIC: "OUTLINE"                    "0..0"
// STATIC: "OUTER_GLOW"                 "0..0"
// STATIC: "FLASHLIGHTDEPTHFILTERMODE"	"0..2"
// STATIC: "DEPTHBLEND"					"0..0"
// STATIC: "BLENDTINTBYBASEALPHA"       "0..1"
// STATIC: "ENVMAPFRESNEL"				"0..1"
// STATIC: "DODSP_SUNFILTER"			"0..1"

// DYNAMIC: "LIGHTING_PREVIEW"          "0..0"
// DYNAMIC: "FLASHLIGHTSHADOWS"			"0..1"

// SKIP: ($DETAILTEXTURE == 0 ) && ( $DETAIL_BLEND_MODE != 0 )
// SKIP: $BASEALPHAENVMAPMASK && $ENVMAPMASK
// SKIP: $BASEALPHAENVMAPMASK && $SELFILLUM
// SKIP: $SELFILLUM && $SELFILLUM_ENVMAPMASK_ALPHA
// SKIP: $SELFILLUM_ENVMAPMASK_ALPHA && (! $ENVMAPMASK)
// SKIP: $ENVMAPMASK && ($FLASHLIGHT || $FLASHLIGHTSHADOWS)
// SKIP: ( $CUBEMAP == 0 ) && ( ( $ENVMAPFRESNEL == 1 ) || ( $BASEALPHAENVMAPMASK == 1 ) )
// SKIP: ( $CUBEMAP == 0 ) && ( $ENVMAPMASK == 1 ) && ( $SELFILLUM_ENVMAPMASK_ALPHA == 0 )
// SKIP: ( $FLASHLIGHT == 0 ) && ( $FLASHLIGHTSHADOWS == 1 )
// SKIP: ( $FLASHLIGHT == 0 ) && ( $FLASHLIGHTDEPTHFILTERMODE != 0 )
// SKIP: ($BLENDTINTBYBASEALPHA) && ($SELFILLUM || $BASEALPHAENVMAPMASK)
// SKIP: $FLASHLIGHT && $CUBEMAP
// SKIP: $FLASHLIGHT && $DODSP_SUNFILTER
''', [
    ('#include "common_vertexlitgeneric_dx9.h"\n',
     '#include "common_vertexlitgeneric_dx9.h"\n\n#if !FLASHLIGHT\n' + sun_consts(15, ' (c15..c18 are unused in this shader)') + '#endif\n', 1),
    struct_add('PS_INPUT', PS_IN),
    ('''	if( bDiffuseLighting || bVertexColor && !( bVertexColor && bDiffuseLighting ) )
	{
		diffuseLighting = i.color.rgb;
	}
''', '''	if( bDiffuseLighting || bVertexColor && !( bVertexColor && bDiffuseLighting ) )
	{
		diffuseLighting = i.color.rgb;
	}

#if !FLASHLIGHT
	// DOD-SP: the sun per pixel (stage 4). Lit models only; vertex-coloured lit models (lighting
	// multiplied into the vertex colour) keep the engine's sun.
	if ( bDiffuseLighting && !bVertexColor )
		diffuseLighting += DoDSP_ModelSunDiffuse( i.dodspSun, i.worldSpaceNormal, DoDSP_ModelSunShadow( i.dodspSun.xyz ) );
#endif
''', 1),
])

# ---------------------------------------------------------------- bump PS
BUMP_FL = '''const float4 g_FlashlightAttenuationFactors	: register( c22 );
const float3 g_FlashlightPos				: register( c23 );
const float4x4 g_FlashlightWorldToTexture	: register( c24 ); // through c27
'''
rebuild('SDK_vertexlit_and_unlit_generic_bump_ps20b.fxc', 'dodsp_vertexlit_and_unlit_generic_bump_ps30.fxc', '''
// STATIC: "CUBEMAP"					"0..1"
// STATIC: "DIFFUSELIGHTING"			"0..1"
// STATIC: "LIGHTWARPTEXTURE"			"0..0"
// STATIC: "SELFILLUM"					"0..1"
// STATIC: "SELFILLUMFRESNEL"			"0..0"
// STATIC: "NORMALMAPALPHAENVMAPMASK"	"0..1"
// STATIC: "HALFLAMBERT"				"0..1"
// STATIC: "FLASHLIGHT"					"0..1"
// STATIC: "DETAILTEXTURE"				"0..1"
// STATIC: "DETAIL_BLEND_MODE"      	"0..1"
// STATIC: "FLASHLIGHTDEPTHFILTERMODE"	"0..2"
// STATIC: "BLENDTINTBYBASEALPHA"		"0..1"
// STATIC: "ENVMAPMASK"					"0..1"
// STATIC: "DODSP_SUNFILTER"			"0..1"

// DYNAMIC: "NUM_LIGHTS"				"0..4"
// DYNAMIC: "AMBIENT_LIGHT"				"0..1"
// DYNAMIC: "FLASHLIGHTSHADOWS"			"0..1"

// SKIP: ( $FLASHLIGHT != 0 ) && ( $NUM_LIGHTS > 0 )
// SKIP: ( $FLASHLIGHT == 0 ) && ( $FLASHLIGHTSHADOWS == 1 )
// SKIP: ( $FLASHLIGHT == 0 ) && ( $FLASHLIGHTDEPTHFILTERMODE != 0 )
// SKIP: (! $DETAILTEXTURE) && ( $DETAIL_BLEND_MODE != 0 )
// SKIP: ( $FLASHLIGHT == 1 ) && ( $SELFILLUM == 1 )
// SKIP: ($BLENDTINTBYBASEALPHA) && ($SELFILLUM)
// SKIP: $FLASHLIGHT && $CUBEMAP
// SKIP: $NORMALMAPALPHAENVMAPMASK && !$CUBEMAP
// SKIP: $NORMALMAPALPHAENVMAPMASK && $ENVMAPMASK
// SKIP: $ENVMAPMASK && !$CUBEMAP
// SKIP: $FLASHLIGHT && $DODSP_SUNFILTER
''', [
    (BUMP_FL, '''#if FLASHLIGHT
''' + BUMP_FL + '''#else
// DOD-SP: the base pass never draws the flashlight, so c22..c25 hold the sun instead. The flashlight
// code below sits in branches that are dead here but must still compile, so its names become constants.
#define g_FlashlightAttenuationFactors	float4( 0.0f, 0.0f, 0.0f, 0.0f )
#define g_FlashlightPos					float3( 0.0f, 0.0f, 0.0f )
#define g_FlashlightWorldToTexture		( (float4x4)0 )
''' + sun_consts(22, ' (the flashlight\'s registers, unused in the base pass)') + '''#endif
''', 1),
    struct_add('PS_INPUT', PS_IN),
    ('''				false, 1.0f, bDoDiffuseWarp, DiffuseWarpSampler );
	}
''', '''				false, 1.0f, bDoDiffuseWarp, DiffuseWarpSampler );
#if !FLASHLIGHT
		// DOD-SP: the sun per pixel (stage 4) - its N.L on the bumped normal
		diffuseLighting += DoDSP_ModelSunDiffuse( i.dodspSun, worldSpaceNormal, DoDSP_ModelSunShadow( i.dodspSun.xyz ) );
#endif
	}
''', 1),
])

# ---------------------------------------------------------------- skin PS
SKIN_FL = 'const float4x4 g_FlashlightWorldToTexture			: register( PSREG_FLASHLIGHT_TO_WORLD_TEXTURE );\n'
rebuild('SDK_skin_ps20b.fxc', 'dodsp_skin_ps30.fxc', '''
// STATIC: "CONVERT_TO_SRGB"			"0..0"
// STATIC: "CUBEMAP"					"0..1"
// STATIC: "SELFILLUM"					"0..1"
// STATIC: "SELFILLUMFRESNEL"			"0..0"
// STATIC: "FLASHLIGHT"					"0..1"
// STATIC: "LIGHTWARPTEXTURE"			"0..0"
// STATIC: "PHONGWARPTEXTURE"			"0..0"
// STATIC: "WRINKLEMAP"					"0..0"
// STATIC: "DETAIL_BLEND_MODE"          "0..1"
// STATIC: "DETAILTEXTURE"				"0..1"
// STATIC: "RIMLIGHT"					"0..1"
// STATIC: "FLASHLIGHTDEPTHFILTERMODE"	"0..2"
// STATIC: "FASTPATH_NOBUMP"            "0..1"
// STATIC: "BLENDTINTBYBASEALPHA"       "0..1"
// STATIC: "PHONG_HALFLAMBERT"          "0..1"
// STATIC: "ENVMAPMASK"					"0..1"
// STATIC: "DODSP_SUNFILTER"			"0..1"

// DYNAMIC: "WRITEWATERFOGTODESTALPHA"  "0..1"
// DYNAMIC: "PIXELFOGTYPE"				"0..1"
// DYNAMIC: "NUM_LIGHTS"				"0..4"
// DYNAMIC: "WRITE_DEPTH_TO_DESTALPHA"	"0..1"
// DYNAMIC: "FLASHLIGHTSHADOWS"			"0..1"

// SKIP: ($PIXELFOGTYPE == 0) && ($WRITEWATERFOGTODESTALPHA != 0)
// SKIP: (! $DETAILTEXTURE) && ( $DETAIL_BLEND_MODE != 0 )
// SKIP: ( $FLASHLIGHT == 0 ) && ( $FLASHLIGHTSHADOWS == 1 )
// SKIP: ( $FLASHLIGHT == 0 ) && ( $FLASHLIGHTDEPTHFILTERMODE != 0 )
// SKIP: ( $FLASHLIGHT == 1 ) && ( $SELFILLUM == 1 )
// SKIP: ( $BLENDTINTBYBASEALPHA ) && ( $SELFILLUM )
// SKIP: $ENVMAPMASK && !$CUBEMAP
// SKIP: $FASTPATH_NOBUMP && ( $RIMLIGHT || $DETAILTEXTURE || $SELFILLUM || $BLENDTINTBYBASEALPHA )
// SKIP: $FLASHLIGHT && $DODSP_SUNFILTER
''', [
    (SKIN_FL, '''#if FLASHLIGHT
''' + SKIN_FL + '''#else
// DOD-SP: the base pass never draws the flashlight, so c15..c18 (its matrix) hold the sun instead.
// The flashlight code sits in branches that are dead here but must still compile.
#define g_FlashlightWorldToTexture		( (float4x4)0 )
''' + sun_consts(15, ' (the flashlight matrix\'s registers, unused in the base pass)') + '''#endif
''', 1),
    struct_add('PS_INPUT', PS_IN),
    ('''			// These parameters aren't passed by generic shaders:
			false, 1.0f,
			bDoDiffuseWarp, DiffuseWarpSampler );
''', '''			// These parameters aren't passed by generic shaders:
			false, 1.0f,
			bDoDiffuseWarp, DiffuseWarpSampler );
#if !FLASHLIGHT
		// DOD-SP: the sun per pixel (stage 4)
		diffuseLighting += DoDSP_ModelSunDiffuse( i.dodspSun, worldSpaceNormal, flDoDSPSunShadow );
#endif
''', 1),
    ('''	float3 diffuseLighting = float3( 1.0f, 1.0f, 1.0f );
	float3 envMapColor = float3( 0.0f, 0.0f, 0.0f );
''', '''	float3 diffuseLighting = float3( 1.0f, 1.0f, 1.0f );
	float3 envMapColor = float3( 0.0f, 0.0f, 0.0f );
#if !FLASHLIGHT
	float flDoDSPSunShadow = DoDSP_ModelSunShadow( i.dodspSun.xyz ) * i.dodspSun.w;	// DOD-SP: once, for diffuse and specular
#endif
''', 1),
    ('''			// Outputs
			specularLighting, rimLighting );
	}
''', '''			// Outputs
			specularLighting, rimLighting );
#if !FLASHLIGHT
		// DOD-SP: the sun's phong highlight and rim (the engine's sun is dropped in the vertex shader)
		float3 vDoDSPSunSpec, vDoDSPSunRim;
		PixelShaderDoSpecularLight( vWorldPos, worldSpaceNormal, fSpecExp, vEyeDir,
			flDoDSPSunShadow, g_DoDSP_MSun3.rgb, DoDSP_ModelToSun(),
			false, 1.0f, bDoSpecularWarp, SpecularWarpSampler, fFresnelRanges,
			bDoRimLighting, g_RimExponent,
			vDoDSPSunSpec, vDoDSPSunRim );
		specularLighting += vDoDSPSunSpec;
		rimLighting += vDoDSPSunRim;
#endif
	}
''', 1),
])
