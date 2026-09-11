/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     HAL Processor Routines
 * COPYRIGHT:   Copyright 2010 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>
#define NDEBUG
#include <debug.h>

KAFFINITY HalpActiveProcessors;
KAFFINITY HalpDefaultInterruptAffinity;

#if defined(CONFIG_SMP) && defined(_M_IX86)
extern ULONG HalpStartedProcessorCount;
extern PPROCESSOR_IDENTITY HalpProcessorIdentity;
#endif

/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
HaliHaltSystem(VOID)
{
    /* Disable interrupts and halt the CPU */
    _disable();
    __halt();
}

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalAllProcessorsStarted(VOID)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    ULONG Processor;

    if (HalpStartedProcessorCount != KeNumberProcessors)
        return FALSE;

    for (Processor = 0; Processor < HalpStartedProcessorCount; Processor++)
    {
        if (!HalpProcessorIdentity[Processor].ProcessorStarted)
            return FALSE;
    }
#endif

    return TRUE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalProcessorIdle(VOID)
{
    /* Enable interrupts and halt the processor */
    _enable();
    __halt();
}

/* EOF */
