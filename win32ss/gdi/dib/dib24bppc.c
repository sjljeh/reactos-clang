/*
 * PROJECT:         Win32 subsystem
 * LICENSE:         See COPYING in the top level directory
 * FILE:            win32ss/gdi/dib/dib24bppc.c
 * PURPOSE:         C language equivalents of asm optimised 24bpp functions
 * PROGRAMMERS:     Jason Filby
 *                  Magnus Olsen
 */

#include <win32k.h>

#define NDEBUG
#include <debug.h>

VOID
DIB_24BPP_HLine(
    SURFOBJ* SurfObj,
    LONG x1,
    LONG x2,
    LONG y,
    ULONG color
    )
{
    PBYTE dst;
    ULONG pixelCount;
    ULONG blockCount;
    ULONG fill0;
    ULONG fill1;
    ULONG fill2;

    if (x1 >= x2)
        return;

    dst = (PBYTE)SurfObj->pvScan0
        + (y * SurfObj->lDelta)
        + (x1 * 3);

    pixelCount = (ULONG)(x2 - x1);
    color &= 0x00FFFFFF;

    /*
     * Write one 24-bit pixel in little-endian byte order.
     */
#define WRITE_PIXEL()            \
    do                           \
    {                            \
        dst[0] = (BYTE)color;    \
        dst[1] = (BYTE)(color >> 8);  \
        dst[2] = (BYTE)(color >> 16); \
        dst += 3;                \
    } while (0)

    if (pixelCount < 8)
    {
        while (pixelCount-- != 0)
            WRITE_PIXEL();

        return;
    }

    /*
     * Align the destination for 32-bit stores.
     * Advancing by three bytes eventually reaches a 4-byte boundary.
     */
    while (((ULONG_PTR)dst & 3) != 0)
    {
        WRITE_PIXEL();
        --pixelCount;
    }

    /*
     * Four 24-bit pixels occupy twelve bytes:
     *
     *   pixel layout:  ABC ABC ABC ABC
     *   DWORD layout: ABCA BCAB CABC
     *
     * The numeric DWORD values below account for little-endian storage.
     */
    fill0 = color | (color << 24);
    fill1 = (color >> 8) | (color << 16);
    fill2 = (color << 8) | (color >> 16);

    blockCount = pixelCount / 4;
    while (blockCount-- != 0)
    {
        ((PULONG)dst)[0] = fill0;
        ((PULONG)dst)[1] = fill1;
        ((PULONG)dst)[2] = fill2;
        dst += 12;
    }

    pixelCount &= 3;
    while (pixelCount-- != 0)
        WRITE_PIXEL();

#undef WRITE_PIXEL
}