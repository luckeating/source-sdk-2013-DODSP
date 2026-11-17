"""Generates DOD-SP's model shader helpers (stage 4) from Mapbase's, once. Kept by hand afterwards."""
import re, sys, os
D = sys.argv[1]

def skip_ws_comments(s, i):
    while i < len(s):
        if s[i] in ' \t\r\n': i += 1
        elif s.startswith('//', i): i = s.index('\n', i) + 1
        elif s.startswith('/*', i): i = s.index('*/', i) + 2
        else: break
    return i

def match_block(s, i):
    assert s[i] == '{', s[i:i+60]
    depth = 0
    while True:
        c = s[i]
        if s.startswith('//', i): i = s.index('\n', i); continue
        if s.startswith('/*', i): i = s.index('*/', i) + 2; continue
        if c in '"\'':
            q = c; i += 1
            while s[i] != q: i += 2 if s[i] == '\\' else 1
            i += 1; continue
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return i + 1
        i += 1

COND = re.compile(r'#ifndef _X360\n[ \t]*if \( !g_pHardwareConfig->SupportsShaderModel_3_0\(\) \)\n#endif\n')
ELSE = re.compile(r'\s*#ifndef _X360\n[ \t]*else\n')

def collapse_sm3(s):
    n = 0
    while True:
        m = COND.search(s)
        if not m: break
        j = skip_ws_comments(s, m.end())
        k = match_block(s, j)                       # the non-SM3 block: dropped
        e = ELSE.match(s, k)
        assert e, s[k:k+200]
        j2 = skip_ws_comments(s, e.end())
        k2 = match_block(s, j2)                     # the SM3 block: kept
        end = s.index('#endif', k2)
        assert not s[k2:end].strip(), repr(s[k2:end])
        end += len('#endif')
        s = s[:m.start()] + '\t\t// DOD-SP: shader model 3.0 only (other cards get Mapbase\'s SDK_ shader via SHADER_FALLBACK)\n\t\t' + s[j2:k2] + s[end:]
        n += 1
    return s, n

PINNED = {  # combo -> highest value DOD-SP compiled (see the .fxc files)
    'SEAMLESS_BASE': 0, 'SEAMLESS_DETAIL': 0, 'DISTANCEALPHA': 0, 'DISTANCEALPHAFROMDETAIL': 0, 'SOFT_MASK': 0,
    'OUTLINE': 0, 'OUTER_GLOW': 0, 'DEPTHBLEND': 0, 'TREESWAY': 0, 'LIGHTWARPTEXTURE': 0, 'SELFILLUMFRESNEL': 0,
    'PHONGWARPTEXTURE': 0, 'WRINKLEMAP': 0, 'DETAIL_BLEND_MODE': 1, 'LIGHTING_PREVIEW': 0,
}
SET = re.compile(r'(SET_(?:STATIC|DYNAMIC)_(?:VERTEX|PIXEL)_SHADER_COMBO\(\s*)(\w+)(\s*,)(.*?)(\);)', re.S)

def pin(s):
    count = {}
    def rep(m):
        name = m.group(2)
        if name not in PINNED: return m.group(0)
        count[name] = count.get(name, 0) + 1
        return '%s%s%s DoDSP_Pinned( "%s", %s, %d ) %s' % (m.group(1), name, m.group(3), name, m.group(4).strip(), PINNED[name], m.group(5))
    return SET.sub(rep, s), count

def port(src, dst, renames, includes_old_re, includes_new, hooks, header):
    s = open(os.path.join(D, src), encoding='latin-1').read()
    s, n = collapse_sm3(s)
    print(src, 'sm3 branches collapsed:', n)
    s, k = re.subn(includes_old_re, includes_new, s, count=1, flags=re.S)
    assert k == 1, 'includes'
    for a, b in renames:
        s = re.sub(r'\b' + a + r'\b', b, s)
    left = sorted(set(re.findall(r'\bsdk_\w+|\bSDK_\w+', s)))
    print('  remaining sdk names:', left)
    for old, new, cnt in hooks:
        c = s.count(old)
        assert c == cnt, (src, old[:70], c)
        s = s.replace(old, new)
    s, pins = pin(s)
    print('  pinned combo setters:', pins)
    first = s.index('\n') + 1
    s = s[:first] + header + s[first:]
    open(os.path.join(D, dst), 'w', encoding='latin-1', newline='\n').write(s)
    print('wrote', dst)

# ---------------------------------------------------------------- vertexlitgeneric helper
port('vertexlitgeneric_dx9_helper.cpp', 'dodsp/dodsp_vertexlitgeneric_helper.cpp',
     [('sdk_vertexlit_and_unlit_generic_vs30', 'dodsp_vertexlit_and_unlit_generic_vs30'),
      ('sdk_vertexlit_and_unlit_generic_ps30', 'dodsp_vertexlit_and_unlit_generic_ps30'),
      ('sdk_vertexlit_and_unlit_generic_bump_vs30', 'dodsp_vertexlit_and_unlit_generic_bump_vs30'),
      ('sdk_vertexlit_and_unlit_generic_bump_ps30', 'dodsp_vertexlit_and_unlit_generic_bump_ps30')],
     r'#include "SDK_vertexlit_and_unlit_generic_vs20\.inc".*?(?=#include "commandbuilder\.h")',
     '#include "dodsp_vertexlit_and_unlit_generic_vs30.inc"			// DOD-SP: our own compiled shaders\n'
     '#include "dodsp_vertexlit_and_unlit_generic_ps30.inc"\n'
     '#include "dodsp_vertexlit_and_unlit_generic_bump_vs30.inc"\n'
     '#include "dodsp_vertexlit_and_unlit_generic_bump_ps30.inc"\n'
     '#include "dodsp_sun_shader.h"									// DOD-SP: the sun\n\n',
     [
      ('static ConVar r_lightwarpidentity( "r_lightwarpidentity","0", FCVAR_CHEAT );',
       'static ConVar r_lightwarpidentity( "dodsp_r_lightwarpidentity","0", FCVAR_CHEAT );	// DOD-SP: own name (ConVars link across DLLs)', 1),
      ('\t\t\t\t\tSET_STATIC_PIXEL_SHADER( dodsp_vertexlit_and_unlit_generic_bump_ps30 );',
       '\t\t\t\t\tSET_STATIC_PIXEL_SHADER_COMBO( DODSP_SUNFILTER, bHasFlashlight ? 0 : DoDSP_SunFilterMode() );	// DOD-SP\n'
       '\t\t\t\t\tif ( !bHasFlashlight )\n\t\t\t\t\t\tDoDSP_SunShadowState( pShaderShadow );									// DOD-SP: cascade samplers\n'
       '\t\t\t\t\tSET_STATIC_PIXEL_SHADER( dodsp_vertexlit_and_unlit_generic_bump_ps30 );', 1),
      ('\t\t\t\t\tSET_STATIC_PIXEL_SHADER( dodsp_vertexlit_and_unlit_generic_ps30 );',
       '\t\t\t\t\tSET_STATIC_PIXEL_SHADER_COMBO( DODSP_SUNFILTER, bHasFlashlight ? 0 : DoDSP_SunFilterMode() );	// DOD-SP\n'
       '\t\t\t\t\tif ( !bHasFlashlight )\n\t\t\t\t\t\tDoDSP_SunShadowState( pShaderShadow );									// DOD-SP: cascade samplers\n'
       '\t\t\t\t\tSET_STATIC_PIXEL_SHADER( dodsp_vertexlit_and_unlit_generic_ps30 );', 1),
      ('\t\tDynamicCmdsOut.End();\n\t\tpShaderAPI->ExecuteCommandBuffer( DynamicCmdsOut.Base() );',
       '\t\t// DOD-SP: the sun - cascades and constants (the bump shader keeps it in c22..c25, the plain one in c15..c18)\n'
       '\t\tDoDSP_ModelSunDynamicState( pShader, pShaderAPI, ( bHasBump || bHasDiffuseWarp ) ? DODSP_MSUN_PSREG_BUMP : DODSP_MSUN_PSREG_PLAIN, !bHasFlashlight );\n\n'
       '\t\tDynamicCmdsOut.End();\n\t\tpShaderAPI->ExecuteCommandBuffer( DynamicCmdsOut.Base() );', 1),
     ],
     '//\n// DOD-SP copy of Mapbase 8.0\'s vertexlitgeneric_dx9_helper.cpp, drawing DODSP_VertexLitGeneric\n'
     '// (lighting stage 4). Generated once by a script and then kept by hand; the only changes from Mapbase:\n'
     '//   - shader model 3.0 paths only (anything else is sent to Mapbase\'s SDK_ shader by SHADER_FALLBACK)\n'
     '//   - DOD-SP\'s own compiled shaders (dodsp_vertexlit_and_unlit_generic_*), never Mapbase\'s .vcs files\n'
     '//   - the sun: DODSP_SUNFILTER combo, cascade samplers, sun constants (search "DOD-SP")\n'
     '//   - combos DOD-SP compiled only as 0 go through DoDSP_Pinned (INVARIANT watchdog)\n//\n')

# ---------------------------------------------------------------- skin helper
port('skin_dx9_helper.cpp', 'dodsp/dodsp_skin_helper.cpp',
     [('sdk_skin_vs30', 'dodsp_skin_vs30'), ('sdk_skin_ps30', 'dodsp_skin_ps30')],
     r'#include "SDK_skin_vs20\.inc".*?(?=#include "commandbuilder\.h")',
     '#include "dodsp_skin_vs30.inc"			// DOD-SP: our own compiled shaders\n'
     '#include "dodsp_skin_ps30.inc"\n'
     '#include "dodsp_sun_shader.h"			// DOD-SP: the sun\n',
     [
      ('static ConVar r_lightwarpidentity( "r_lightwarpidentity", "0", FCVAR_CHEAT );',
       'static ConVar r_lightwarpidentity( "dodsp_r_lightwarpidentity", "0", FCVAR_CHEAT );	// DOD-SP: own name (ConVars link across DLLs)', 1),
      ('static ConVar r_rimlight( "r_rimlight", "1", FCVAR_CHEAT );',
       'static ConVar r_rimlight( "dodsp_r_rimlight", "1", FCVAR_CHEAT );	// DOD-SP: own name', 1),
      ('\t\t\tSET_STATIC_PIXEL_SHADER( dodsp_skin_ps30 );',
       '\t\t\tSET_STATIC_PIXEL_SHADER_COMBO( DODSP_SUNFILTER, bHasFlashlight ? 0 : DoDSP_SunFilterMode() );	// DOD-SP\n'
       '\t\t\tif ( !bHasFlashlight )\n\t\t\t\tDoDSP_SunShadowState( pShaderShadow );									// DOD-SP: cascade samplers\n'
       '\t\t\tSET_STATIC_PIXEL_SHADER( dodsp_skin_ps30 );', 1),
      ('\t}\n\tpShader->Draw();\n}',
       '\n\t\t// DOD-SP: the sun - cascades and constants (c15..c18 in the base pass; the flashlight\'s matrix otherwise)\n'
       '\t\tDoDSP_ModelSunDynamicState( pShader, pShaderAPI, DODSP_MSUN_PSREG_SKIN, !bHasFlashlight );\n'
       '\t}\n\tpShader->Draw();\n}', 1),
     ],
     '//\n// DOD-SP copy of Mapbase 8.0\'s skin_dx9_helper.cpp (the phong path of DODSP_VertexLitGeneric,\n'
     '// lighting stage 4). Generated once by a script and then kept by hand; changes as in\n'
     '// dodsp_vertexlitgeneric_helper.cpp (search "DOD-SP").\n//\n')
