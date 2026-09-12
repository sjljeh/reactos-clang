
#include <asm.inc>
#include "tsc.h"

.code

#ifdef _M_IX86

EXTERN _TscCalibrationPhase:BYTE
EXTERN _TscCalibrationArray:QWORD
EXTERN _ApicCalibrationArray:DWORD
EXTERN _HalpSendEOI@0:PROC

PUBLIC _TscCalibrationISR
_TscCalibrationISR:
    push eax
    push ecx
    push edx

    /* The first thing we do is read the current TSC value */
    rdtsc

    /* Read the current phase */
    movzx ecx, byte ptr ds:[_TscCalibrationPhase]

    /* Check if we're already done */
    cmp cl, NUM_SAMPLES
    jnb _CalibrationISR_Exit

    /* Store the current value */
    shl ecx, 3
    mov dword ptr _TscCalibrationArray[ecx], eax
    mov dword ptr _TscCalibrationArray[ecx + 4], edx

    /* Record the LAPIC current count on the same RTC edge. */
    shr ecx, 1
    mov eax, dword ptr ds:[HEX(FFFE0390)]
    mov dword ptr _ApicCalibrationArray[ecx], eax

    /* Advance phase */
    inc byte ptr ds:[_TscCalibrationPhase]

_CalibrationISR_Exit:

    /* Read CMOS register C */
    mov al, HEX(0C)
    out HEX(70), al
    jmp $+2
    in al, HEX(71)
    jmp $+2

    /* Send EOI */
    call _HalpSendEOI@0

    pop edx
    pop ecx
    pop eax
    iretd

#else

EXTERN TscCalibrationPhase:BYTE
EXTERN TscCalibrationArray:DWORD
EXTERN ApicCalibrationArray:DWORD
EXTERN HalpSendEOI:PROC

PUBLIC TscCalibrationISR
FUNC TscCalibrationISR
    push rax
    push rbx
    push rcx
    push rdx
    .ENDPROLOG

    /* The first thing we do is read the current TSC value */
    rdtsc

    /* Read the current phase */
    movzx rcx, byte ptr [rip+TscCalibrationPhase]

    /* Check if we're already done */
    cmp cl, NUM_SAMPLES
    jnb CalibrationISR_Exit

    /* Store the current value */
    shl rcx, 3
    lea rbx, [rip+TscCalibrationArray]
    mov dword ptr [rbx + rcx], eax
    mov dword ptr [rbx + rcx + 4], edx

    /* Record the LAPIC current count on the same RTC edge. */
    shr rcx, 1
    mov rbx, HEX(FFFFFFFFFFFE0000)
    mov eax, dword ptr [rbx + HEX(390)]
    lea rbx, [rip+ApicCalibrationArray]
    mov dword ptr [rbx + rcx], eax

    /* Advance phase */
    inc byte ptr [rip+TscCalibrationPhase]

CalibrationISR_Exit:
    /* Read CMOS register C */
    mov al, HEX(0C)
    out HEX(70), al
    jmp $+2
    in al, HEX(71)
    jmp $+2

    /* Send EOI */
    call HalpSendEOI

    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq
ENDFUNC
#endif

END
