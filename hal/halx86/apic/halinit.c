/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Initialize the APIC HAL
 * COPYRIGHT:   Copyright 2011 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

VOID
NTAPI
ApicInitializeLocalApic(ULONG Cpu);

extern PPROCESSOR_IDENTITY HalpProcessorIdentity;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

static
CODE_SEG("INIT")
VOID
HalpPutBootProcessorFirst(VOID)
{
    PROCESSOR_IDENTITY Identity;
    UCHAR BootApicId;
    ULONG Index;

    BootApicId = (UCHAR)(ApicRead(APIC_ID) >> 24);

    for (Index = 0; Index < HalpApicInfoTable.ProcessorCount; Index++)
    {
        if (HalpProcessorIdentity[Index].LapicId == BootApicId)
            break;
    }

    if (Index == HalpApicInfoTable.ProcessorCount)
    {
        /* The MADT is unusable for startup, but the boot CPU can still run. */
        DPRINT1("BSP APIC ID %u is absent from the MADT; disabling AP startup\n",
                BootApicId);
        RtlZeroMemory(HalpProcessorIdentity, sizeof(*HalpProcessorIdentity));
        HalpProcessorIdentity[0].LapicId = BootApicId;
        HalpApicInfoTable.ProcessorCount = 1;
        return;
    }

    if (Index != 0)
    {
        /* NT processor zero is the BSP regardless of firmware table order. */
        Identity = HalpProcessorIdentity[0];
        HalpProcessorIdentity[0] = HalpProcessorIdentity[Index];
        HalpProcessorIdentity[Index] = Identity;
    }
}

/* FUNCTIONS ****************************************************************/

VOID
NTAPI
HalpInitProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    if (ProcessorNumber == 0)
    {
        HalpParseApicTables(LoaderBlock);
        HalpPutBootProcessorFirst();
    }

    HalpSetupProcessorsTable(ProcessorNumber);

    /* Initialize the local APIC for this cpu */
    ApicInitializeLocalApic(ProcessorNumber);

    /* Record and verify the firmware-to-NT processor association. */
    NT_ASSERT(HalpProcessorIdentity[ProcessorNumber].LapicId ==
              (UCHAR)(ApicRead(APIC_ID) >> 24));
    HalpProcessorIdentity[ProcessorNumber].ProcessorStarted = TRUE;
    HalpProcessorIdentity[ProcessorNumber].BSPCheck = (ProcessorNumber == 0);

    /* Initialize profiling data (but don't start it) */
    HalInitializeProfiling();

    /* Initialize the timer */
    //ApicInitializeTimer(ProcessorNumber);
}

VOID
HalpInitPhase0(IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    DPRINT1("Using HAL: APIC %s %s\n",
            (HalpBuildType & PRCB_BUILD_UNIPROCESSOR) ? "UP" : "SMP",
            (HalpBuildType & PRCB_BUILD_DEBUG) ? "DBG" : "REL");

    HalpPrintApicTables();

    /* Enable clock interrupt handler */
    HalpEnableInterruptHandler(IDT_INTERNAL,
                               0,
                               APIC_CLOCK_VECTOR,
                               CLOCK2_LEVEL,
                               HalpClockInterrupt,
                               Latched);

    /* Enable profile interrupt handler */
    HalpEnableInterruptHandler(IDT_DEVICE,
                               0,
                               APIC_PROFILE_VECTOR,
                               APIC_PROFILE_LEVEL,
                               HalpProfileInterrupt,
                               Latched);
}

VOID
HalpInitPhase1(VOID)
{
    /* Initialize DMA. NT does this in Phase 0 */
    HalpInitDma();
}

/* EOF */
