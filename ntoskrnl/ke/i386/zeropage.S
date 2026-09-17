/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fast zeroing of pages
 * COPYRIGHT:   Copyright 2021 Jérôme Gardou <jerome.gardou@reactos.org>
 */

#include <asm.inc>

/* FUNCTIONS ****************************************************************/
.code

/*
 * VOID
 * FASTCALL
 * KeZeroPages(void* ptr, ULONG Size)
 */
PUBLIC @KeZeroPages@8
FUNC @KeZeroPages@8
    FPO 0, 0, 0, 0, 0, FRAME_FPO

    push edi
    mov edi, ecx
    mov ecx, edx
    shr ecx, 2
    xor eax, eax
    rep stosd
    pop edi
    ret
ENDFUNC

/*
 * VOID
 * FASTCALL
 * KiZeroPagesNonTemporal(void* ptr, ULONG Size)
 *
 * The caller supplies a cacheable, page-aligned range whose size is a
 * multiple of PAGE_SIZE.
 */
PUBLIC @KiZeroPagesNonTemporal@8
FUNC @KiZeroPagesNonTemporal@8
    FPO 0, 0, 0, 0, 0, FRAME_FPO

    add edx, ecx
    xor eax, eax
    ALIGN 16
ZeroNextCacheLine:
    movnti dword ptr [ecx], eax
    movnti dword ptr [ecx + 4], eax
    movnti dword ptr [ecx + 8], eax
    movnti dword ptr [ecx + 12], eax
    movnti dword ptr [ecx + 16], eax
    movnti dword ptr [ecx + 20], eax
    movnti dword ptr [ecx + 24], eax
    movnti dword ptr [ecx + 28], eax
    add ecx, 32
    cmp ecx, edx
    jne ZeroNextCacheLine
    sfence
    ret
ENDFUNC

END
