import re,sys
SRC='lightmappedgeneric_dx9_helper.cpp'; DST='dodsp/dodsp_lightmappedgeneric_helper.cpp'
s=open(SRC,encoding='latin-1').read()

def skip_ws_comments(s,i):
    while i<len(s):
        if s[i] in ' \t\r\n': i+=1
        elif s.startswith('//',i): i=s.index('\n',i)+1
        elif s.startswith('/*',i): i=s.index('*/',i)+2
        else: break
    return i

def match_block(s,i):
    """s[i]=='{' -> index after matching '}' (skips comments, strings, char literals)"""
    assert s[i]=='{',s[i:i+40]
    depth=0
    while True:
        c=s[i]
        if s.startswith('//',i): i=s.index('\n',i); continue
        if s.startswith('/*',i): i=s.index('*/',i)+2; continue
        if c=='"' or c=="'":
            q=c; i+=1
            while s[i]!=q:
                i+= 2 if s[i]=='\\' else 1
            i+=1; continue
        if c=='{': depth+=1
        elif c=='}':
            depth-=1
            if depth==0: return i+1
        i+=1

def match_paren(s,i):
    assert s[i]=='('
    d=0
    while True:
        if s[i]=='(': d+=1
        elif s[i]==')':
            d-=1
            if d==0: return i+1
        i+=1

COND='if ( g_pHardwareConfig->SupportsShaderModel_3_0() )'
count=0
while True:
    i=s.find(COND)
    if i<0: break
    j=skip_ws_comments(s,i+len(COND))
    k=match_block(s,j)          # SM3 block
    body=s[j:k]
    # swallow the else-if / else chain
    m=k
    while True:
        n=skip_ws_comments(s,m)
        if s.startswith('else',n):
            n2=skip_ws_comments(s,n+4)
            if s.startswith('if',n2):
                n3=skip_ws_comments(s,n2+2)
                n3=match_paren(s,n3)
                n3=skip_ws_comments(s,n3)
                m=match_block(s,n3)
            else:
                m=match_block(s,n2)
            continue
        break
    s=s[:i]+'// DOD-SP: shader model 3.0 only (other cards get Mapbase\'s SDK_ shader via SHADER_FALLBACK)\n\t\t'+body+s[m:]
    count+=1
print('sm3 branches collapsed:',count)

# includes
old_inc=s[s.index('#include "SDK_lightmappedgeneric_ps20b.inc"'):s.index('#include "tier0/memdbgon.h"')]
s=s.replace(old_inc,'''#include "dodsp_lightmappedgeneric_ps30.inc"			// DOD-SP: our own compiled shaders
#include "dodsp_lightmappedgeneric_vs30.inc"
#include "dodsp_lightmappedgeneric_flashlight_vs30.inc"
#include "dodsp_lightmappedgeneric_flashlight_ps30.inc"
#include "dodsp_sun_shader.h"						// DOD-SP: the sun

''')
for a in ['lightmappedgeneric_vs30','lightmappedgeneric_ps30','lightmappedgeneric_flashlight_vs30','lightmappedgeneric_flashlight_ps30']:
    s=s.replace('sdk_'+a,'dodsp_'+a)
left=re.findall(r'\bsdk_\w+|\bSDK_\w+',s)
print('remaining sdk names:',sorted(set(left)))

# sun: static combo + samplers in the main pass
anchor='SET_STATIC_PIXEL_SHADER( dodsp_lightmappedgeneric_ps30 );'
assert s.count(anchor)==1
s=s.replace(anchor,'''SET_STATIC_PIXEL_SHADER_COMBO( DODSP_SUNFILTER, DoDSP_SunFilterMode() );	// DOD-SP
					DoDSP_SunShadowState( pShaderShadow );									// DOD-SP: cascade samplers
					'''+anchor)
# sun: dynamic state of the main pass
anchor2='''		float eyePos[4];
		pShaderAPI->GetWorldSpaceCameraPosition( eyePos );
		eyePos[3] = pShaderAPI->GetFloatRenderingParameter( FLOAT_RENDERPARM_MINIMUMLIGHTING );'''
assert s.count(anchor2)==1
s=s.replace(anchor2,'''		// DOD-SP: the sun - cascades and constants from the rendering parameters the client sets
		DoDSP_SunDynamicState( pShader, pShaderAPI, pShaderAPI->GetLightMapScaleFactor() );

'''+anchor2)
# header
first=s.index('\n')+1
s=s[:first]+'''//
// DOD-SP copy of Mapbase 8.0's lightmappedgeneric_dx9_helper.cpp, drawing DODSP_LightmappedGeneric
// and DODSP_WorldVertexTransition (lighting stage 3b-2). Generated once by a script and then kept
// by hand; the only changes from Mapbase:
//   - shader model 3.0 paths only (anything else is sent to Mapbase's SDK_ shader by SHADER_FALLBACK)
//   - DOD-SP's own compiled shaders (dodsp_lightmappedgeneric_*), never Mapbase's .vcs files
//   - the sun: DODSP_SUNFILTER combo, cascade samplers, sun constants (search "DOD-SP")
//
'''+s[first:]
open(DST,'w',encoding='latin-1',newline='\n').write(s)
