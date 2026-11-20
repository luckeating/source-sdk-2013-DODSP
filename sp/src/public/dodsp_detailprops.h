//===================== DOD-SP detail prop limit =====================//
//
// One owner for how many detail props (grass, sprites, detail models) a map may hold. VBSP (which
// places them) and the client (which draws them) both include this.
//
// Stock Source stops at 65535. The BSP format itself has no 16-bit limit (the detail lump's count is an
// int; each prop stores only its LEAF as 16 bits, and maps have at most 65536 leaves). The 64K came
// from the client: CClientLeafSystem kept each leaf's first detail prop / count as unsigned short, and
// the detail light-style map (CDetailModel::gm_LightStylesMap) had CUtlMap's default unsigned short
// index - every prop lit by a light style (with -dodsp_sunstyle that is every sunlit prop) takes one
// entry. Both are int in DOD-SP (COMPILE_TIME_ASSERTs next to them).
//
// INVARIANT: every detail prop index fits the client's per-leaf index and light-style map. A map
// over 65535 props draws wrong (props from the wrong leaf, or a crash) on a stock client: it needs
// DOD-SP's client.dll.
//
// Cost: each prop is ~60 bytes in the .bsp and in memory. Drawing cost depends on how many are in
// view within cl_detaildist, not on the map's total.
//
//====================================================================//
#ifndef DODSP_DETAILPROPS_H
#define DODSP_DETAILPROPS_H

#define DODSP_MAX_DETAIL_PROPS		( 512 * 1024 )		// 8x stock
#define DODSP_STOCK_DETAIL_PROPS	65535

#endif // DODSP_DETAILPROPS_H
