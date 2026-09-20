#pragma once

#define ROP4_BLACKNESS    ((((0x00000042) >> 8) & 0xff00) | (((0x00000042) >> 16) & 0x00ff))
#define ROP4_NOTSRCERASE  ((((0x001100A6) >> 8) & 0xff00) | (((0x001100A6) >> 16) & 0x00ff))
#define ROP4_NOTSRCCOPY   ((((0x00330008) >> 8) & 0xff00) | (((0x00330008) >> 16) & 0x00ff))
#define ROP4_SRCERASE     ((((0x00440328) >> 8) & 0xff00) | (((0x00440328) >> 16) & 0x00ff))
#define ROP4_DSTINVERT    ((((0x00550009) >> 8) & 0xff00) | (((0x00550009) >> 16) & 0x00ff))
#define ROP4_PATINVERT    ((((0x005A0049) >> 8) & 0xff00) | (((0x005A0049) >> 16) & 0x00ff))
#define ROP4_SRCINVERT    ((((0x00660046) >> 8) & 0xff00) | (((0x00660046) >> 16) & 0x00ff))
#define ROP4_SRCAND       ((((0x008800C6) >> 8) & 0xff00) | (((0x008800C6) >> 16) & 0x00ff))
#define ROP4_MERGEPAINT   ((((0x00BB0226) >> 8) & 0xff00) | (((0x00BB0226) >> 16) & 0x00ff))
#define ROP4_MERGECOPY    ((((0x00C000CA) >> 8) & 0xff00) | (((0x00C000CA) >> 16) & 0x00ff))
#define ROP4_SRCCOPY      ((((0x00CC0020) >> 8) & 0xff00) | (((0x00CC0020) >> 16) & 0x00ff))
#define ROP4_SRCPAINT     ((((0x00EE0086) >> 8) & 0xff00) | (((0x00EE0086) >> 16) & 0x00ff))
#define ROP4_PATCOPY      ((((0x00F00021) >> 8) & 0xff00) | (((0x00F00021) >> 16) & 0x00ff))
#define ROP4_PATPAINT     ((((0x00FB0A09) >> 8) & 0xff00) | (((0x00FB0A09) >> 16) & 0x00ff))
#define ROP4_WHITENESS    ((((0x00FF0062) >> 8) & 0xff00) | (((0x00FF0062) >> 16) & 0x00ff))


typedef struct _BLTINFO
{
  SURFOBJ *DestSurface;
  SURFOBJ *SourceSurface;
  SURFOBJ *PatternSurface;
  XLATEOBJ *XlateSourceToDest;
  RECTL DestRect;
  POINTL SourcePoint;
  BRUSHOBJ *Brush;
  POINTL BrushOrigin;
  ROP4 Rop4;
} BLTINFO, *PBLTINFO;

typedef VOID (*PFN_DIB_PutPixel)(SURFOBJ*,LONG,LONG,ULONG);
typedef ULONG (*PFN_DIB_GetPixel)(SURFOBJ*,LONG,LONG);
typedef VOID (*PFN_DIB_HLine)(SURFOBJ*,LONG,LONG,LONG,ULONG);
typedef VOID (*PFN_DIB_VLine)(SURFOBJ*,LONG,LONG,LONG,ULONG);
typedef BOOLEAN (*PFN_DIB_BitBlt)(PBLTINFO);
typedef BOOLEAN (*PFN_DIB_StretchBlt)(SURFOBJ*,SURFOBJ*,SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,POINTL*,BRUSHOBJ*,POINTL*,XLATEOBJ*,ROP4);
typedef BOOLEAN (*PFN_DIB_TransparentBlt)(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
typedef BOOLEAN (*PFN_DIB_ColorFill)(SURFOBJ*, RECTL*, ULONG);
typedef BOOLEAN (*PFN_DIB_AlphaBlend)(SURFOBJ*, SURFOBJ*, RECTL*, RECTL*, CLIPOBJ*, XLATEOBJ*, BLENDOBJ*);

typedef struct
{
  PFN_DIB_PutPixel       DIB_PutPixel;
  PFN_DIB_GetPixel       DIB_GetPixel;
  PFN_DIB_HLine          DIB_HLine;
  PFN_DIB_VLine          DIB_VLine;
  PFN_DIB_BitBlt         DIB_BitBlt;
  PFN_DIB_BitBlt         DIB_BitBltSrcCopy;
  PFN_DIB_StretchBlt     DIB_StretchBlt;
  PFN_DIB_TransparentBlt DIB_TransparentBlt;
  PFN_DIB_ColorFill      DIB_ColorFill;
  PFN_DIB_AlphaBlend     DIB_AlphaBlend;
} DIB_FUNCTIONS;

extern DIB_FUNCTIONS DibFunctionsForBitmapFormat[];

VOID Dummy_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG Dummy_GetPixel(SURFOBJ*,LONG,LONG);
VOID Dummy_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID Dummy_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN Dummy_BitBlt(PBLTINFO);
BOOLEAN Dummy_StretchBlt(SURFOBJ*,SURFOBJ*,SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,POINTL*,BRUSHOBJ*,POINTL*,XLATEOBJ*,ROP4);
BOOLEAN Dummy_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN Dummy_ColorFill(SURFOBJ*, RECTL*, ULONG);
BOOLEAN Dummy_AlphaBlend(SURFOBJ*, SURFOBJ*, RECTL*, RECTL*, CLIPOBJ*, XLATEOBJ*, BLENDOBJ*);

VOID DIB_1BPP_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG DIB_1BPP_GetPixel(SURFOBJ*,LONG,LONG);
VOID DIB_1BPP_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID DIB_1BPP_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN DIB_1BPP_BitBlt(PBLTINFO);
BOOLEAN DIB_1BPP_BitBltSrcCopy(PBLTINFO);
BOOLEAN DIB_1BPP_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN DIB_1BPP_ColorFill(SURFOBJ*, RECTL*, ULONG);

VOID DIB_4BPP_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG DIB_4BPP_GetPixel(SURFOBJ*,LONG,LONG);
VOID DIB_4BPP_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID DIB_4BPP_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN DIB_4BPP_BitBlt(PBLTINFO);
BOOLEAN DIB_4BPP_BitBltSrcCopy(PBLTINFO);
BOOLEAN DIB_4BPP_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN DIB_4BPP_ColorFill(SURFOBJ*, RECTL*, ULONG);

VOID DIB_8BPP_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG DIB_8BPP_GetPixel(SURFOBJ*,LONG,LONG);
VOID DIB_8BPP_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID DIB_8BPP_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN DIB_8BPP_BitBlt(PBLTINFO);
BOOLEAN DIB_8BPP_BitBltSrcCopy(PBLTINFO);
BOOLEAN DIB_8BPP_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN DIB_8BPP_ColorFill(SURFOBJ*, RECTL*, ULONG);

VOID DIB_16BPP_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG DIB_16BPP_GetPixel(SURFOBJ*,LONG,LONG);
VOID DIB_16BPP_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID DIB_16BPP_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN DIB_16BPP_BitBlt(PBLTINFO);
BOOLEAN DIB_16BPP_BitBltSrcCopy(PBLTINFO);
BOOLEAN DIB_16BPP_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN DIB_16BPP_ColorFill(SURFOBJ*, RECTL*, ULONG);
BOOLEAN DIB_16BPP_AlphaBlend(SURFOBJ*, SURFOBJ*, RECTL*, RECTL*, CLIPOBJ*, XLATEOBJ*, BLENDOBJ*);

VOID DIB_24BPP_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG DIB_24BPP_GetPixel(SURFOBJ*,LONG,LONG);
VOID DIB_24BPP_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID DIB_24BPP_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN DIB_24BPP_BitBlt(PBLTINFO);
BOOLEAN DIB_24BPP_BitBltSrcCopy(PBLTINFO);
BOOLEAN DIB_24BPP_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN DIB_24BPP_ColorFill(SURFOBJ*, RECTL*, ULONG);
BOOLEAN DIB_24BPP_AlphaBlend(SURFOBJ*, SURFOBJ*, RECTL*, RECTL*, CLIPOBJ*, XLATEOBJ*, BLENDOBJ*);

VOID DIB_32BPP_PutPixel(SURFOBJ*,LONG,LONG,ULONG);
ULONG DIB_32BPP_GetPixel(SURFOBJ*,LONG,LONG);
VOID DIB_32BPP_HLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
VOID DIB_32BPP_VLine(SURFOBJ*,LONG,LONG,LONG,ULONG);
BOOLEAN DIB_32BPP_BitBlt(PBLTINFO);
BOOLEAN DIB_32BPP_BitBltSrcCopy(PBLTINFO);
BOOLEAN DIB_32BPP_TransparentBlt(SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,XLATEOBJ*,ULONG);
BOOLEAN DIB_32BPP_ColorFill(SURFOBJ*, RECTL*, ULONG);
BOOLEAN DIB_32BPP_AlphaBlend(SURFOBJ*, SURFOBJ*, RECTL*, RECTL*, CLIPOBJ*, XLATEOBJ*, BLENDOBJ*);

BOOLEAN DIB_XXBPP_StretchBlt(SURFOBJ*,SURFOBJ*,SURFOBJ*,SURFOBJ*,RECTL*,RECTL*,POINTL*,BRUSHOBJ*,POINTL*,XLATEOBJ*,ROP4);
BOOLEAN DIB_XXBPP_FloodFillSolid(SURFOBJ*, BRUSHOBJ*, RECTL*, POINTL*, ULONG, UINT);
BOOLEAN DIB_XXBPP_AlphaBlend(SURFOBJ*, SURFOBJ*, RECTL*, RECTL*, CLIPOBJ*, XLATEOBJ*, BLENDOBJ*);

extern unsigned char notmask[2];
extern unsigned char altnotmask[2];
#define MASK1BPP(x) (1<<(7-((x)&7)))

/* Single copy of the directional step helper used by all dib*BPP.c files */
#define DEC_OR_INC(var, decTrue, amount) \
    ((var) = (decTrue) ? ((var) - (amount)) : ((var) + (amount)))

/* 4bpp helpers. High nibble holds even pixels, low nibble holds odd pixels. */
FORCEINLINE
BYTE
DIB_4BPP_PreserveMask(
    _In_ LONG x)
{
    return (x & 1) ? 0xF0 : 0x0F;
}

FORCEINLINE
ULONG
DIB_4BPP_GetNibble(
    _In_ BYTE Byte,
    _In_ LONG x)
{
    return (Byte >> ((1 - (x & 1)) << 2)) & 0x0F;
}

FORCEINLINE
BYTE
DIB_4BPP_SetNibble(
    _In_ BYTE Old,
    _In_ LONG x,
    _In_ ULONG Color)
{
    return (Old & DIB_4BPP_PreserveMask(x)) |
        (BYTE)((Color & 0x0F) << ((1 - (x & 1)) << 2));
}

FORCEINLINE
ULONG
DIB_ExpandNibbleToULONG(
    _In_ ULONG Color)
{
    return (Color & 0x0F) * 0x11111111UL;
}

FORCEINLINE
ULONG
DIB_PixelMaskForFormat(
    _In_ ULONG Format)
{
    switch (Format)
    {
    case BMF_1BPP:
        return 0x1;
    case BMF_4BPP:
        return 0xF;
    case BMF_8BPP:
        return 0xFF;
    case BMF_16BPP:
        return 0xFFFF;
    case BMF_24BPP:
        return 0xFFFFFF;
    default:
        return 0xFFFFFFFF;
    }
}

typedef union _NICEPIXEL16_565
{
    USHORT us;
    struct
    {
        USHORT blue : 5;
        USHORT green : 6;
        USHORT red : 5;
    } col;
} NICEPIXEL16_565;

typedef union _NICEPIXEL16_555
{
    USHORT us;
    struct
    {
        USHORT blue : 5;
        USHORT green : 5;
        USHORT red : 5;
        USHORT xxxx : 1;
    } col;
} NICEPIXEL16_555;

ULONG DIB_DoRop(ULONG Rop, ULONG Dest, ULONG Source, ULONG Pattern);

#define DIB_GetSource(SourceSurf,sx,sy,ColorTranslation)    \
  XLATEOBJ_iXlate(ColorTranslation,                         \
    DibFunctionsForBitmapFormat[SourceSurf->iBitmapFormat]. \
    DIB_GetPixel(SourceSurf, sx, sy))

#define DIB_GetSourceIndex(SourceSurf,sx,sy)                \
  DibFunctionsForBitmapFormat[SourceSurf->iBitmapFormat].   \
    DIB_GetPixel(SourceSurf, sx, sy)
