"""VTF 7.2 writer (Source 2013 reads 7.0-7.5): DXT1 / DXT5 copied as compressed blocks, or BGR888 / BGRA8888.
Layout (public/vtf/vtf.h, VTFFileHeader_t 7.2): 'VTF\\0', u32 7, u32 2, u32 headerSize 80, u16 width, u16 height,
u32 flags, u16 frames, u16 startFrame, 4 pad, float3 reflectivity, 4 pad, float bumpScale, u32 format, u8 mipCount,
u32 lowResFormat, u8 lowResW, u8 lowResH, u16 depth; padded to 80. Then the low-res image (none here), then the mips
SMALLEST FIRST - the same order as a CoD2 .iwi, so an iwi's DXT mips go in as they are, block for block."""
import struct, numpy as np

FMT = {'BGR888': 3, 'BGRA8888': 12, 'DXT1': 13, 'DXT5': 15}
TEXTUREFLAGS_NORMAL = 0x80
TEXTUREFLAGS_EIGHTBITALPHA = 0x2000
TEXTUREFLAGS_ONEBITALPHA = 0x1000

def write(path, w, h, fmt, mips_small_first, flags=0, reflectivity=(0.5, 0.5, 0.5)):
    hdr = struct.pack('<4sIIIHHIHH4x3f4xfIBIBBH', b'VTF\0', 7, 2, 80, w, h, flags, 1, 0, *reflectivity, 1.0, FMT[fmt],
                      len(mips_small_first), 0xFFFFFFFF, 0, 0, 1)
    hdr += b'\0' * (80 - len(hdr))
    with open(path, 'wb') as f:
        f.write(hdr)
        for m in mips_small_first: f.write(m)

def mip_chain(rgb):
    """box-filtered mips of an h x w x c uint8 image, largest first, down to 1x1"""
    out = [rgb]; a = rgb.astype(np.float32)
    while a.shape[0] > 1 or a.shape[1] > 1:
        h, w = a.shape[:2]
        a = a[:h - h % 2 or 1, :w - w % 2 or 1]
        if a.shape[0] > 1: a = (a[0::2] + a[1::2]) / 2
        if a.shape[1] > 1: a = (a[:, 0::2] + a[:, 1::2]) / 2
        out.append(np.clip(a + 0.5, 0, 255).astype(np.uint8))
    return out

def read_header(path):
    d = open(path, 'rb').read(80)
    sig, vmaj, vmin, hs, w, h, flags, frames, start = struct.unpack_from('<4sIIIHHIHH', d, 0)
    fmt, = struct.unpack_from('<I', d, 52); mips = d[56]
    return dict(sig=sig, version=(vmaj, vmin), header=hs, w=w, h=h, flags=flags, frames=frames, fmt=fmt, mips=mips)
