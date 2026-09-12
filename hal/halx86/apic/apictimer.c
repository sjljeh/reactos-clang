/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/apictimer.c
 * PURPOSE:         System Profiling
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include "apicp.h"
#include "tsc.h"
#define NDEBUG
#include <debug.h>

#define AP_RUNTIME_CLOCK_HZ       64
#define HUNDRED_NS_PER_SECOND     10000000ULL

/* HAL profiling variables */
BOOLEAN HalIsProfiling = FALSE;
ULONGLONG HalCurProfileInterval = 10000000;
ULONGLONG HalMinProfileInterval = 1000;
ULONGLONG HalMaxProfileInterval = 10000000;
volatile KAFFINITY HalpProfilingProcessorMask;
static volatile LONG HalpProfilingProcessorCount;
static ULONG HalpApicTimerFrequency;
ULONG ApicCalibrationArray[NUM_SAMPLES];

/* TIMER FUNCTIONS ************************************************************/

static
ULONG
ApicTimerCountFromInterval(
    _In_ ULONGLONG Interval)
{
    ULONGLONG TimerCount;
    ULONG TimerFrequency;

    TimerFrequency = HalpApicTimerFrequency;
    if (!TimerFrequency)
        return 1;

    TimerCount = ((ULONGLONG)TimerFrequency * Interval +
                  HUNDRED_NS_PER_SECOND / 2) / HUNDRED_NS_PER_SECOND;
    if (!TimerCount)
        return 1;

    return (TimerCount > ~0UL) ? ~0UL : (ULONG)TimerCount;
}

static
VOID
ApicProgramTimer(
    _In_ UCHAR Vector,
    _In_ ULONG TimerCount,
    _In_ BOOLEAN Masked)
{
    LVT_REGISTER LvtEntry;

    /* Program the divider and LVT before the initial count starts the timer. */
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);

    LvtEntry.Long = 0;
    LvtEntry.TimerMode = 1;
    LvtEntry.Vector = Vector;
    LvtEntry.Mask = Masked;
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_TICR, Masked ? 0 : TimerCount);
}

VOID
NTAPI
ApicPrepareTimerCalibration(VOID)
{
    LVT_REGISTER LvtEntry;

    NT_ASSERT(KeGetCurrentPrcb()->Number == 0);

    /* Run the local timer masked and one-shot during RTC/TSC calibration. */
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);
    LvtEntry.Long = 0;
    LvtEntry.Vector = CLOCK_IPI_VECTOR;
    LvtEntry.Mask = 1;
    LvtEntry.TimerMode = 0;
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_TICR, ~0UL);
}

VOID
NTAPI
ApicCalibrateTimer(VOID)
{
    ULONGLONG TimerFrequency;
    ULONG ElapsedCount;

    NT_ASSERT(KeGetCurrentPrcb()->Number == 0);

    /* Stop the calibration timer after the RTC-edge samples are complete. */
    ApicWrite(APIC_TICR, 0);

    /* The samples span NUM_SAMPLES - 1 periods of SAMPLE_FREQUENCY. */
    ElapsedCount = ApicCalibrationArray[0] -
                   ApicCalibrationArray[NUM_SAMPLES - 1];
    if (!ElapsedCount)
        KeBugCheck(HAL_INITIALIZATION_FAILED);

    TimerFrequency = ((ULONGLONG)ElapsedCount * SAMPLE_FREQUENCY +
                      (NUM_SAMPLES - 1) / 2) /
                     (NUM_SAMPLES - 1);
    if (!TimerFrequency || TimerFrequency > ~0UL)
        KeBugCheck(HAL_INITIALIZATION_FAILED);

    HalpApicTimerFrequency = (ULONG)TimerFrequency;
    KeGetPcr()->HalReserved[HAL_PROFILING_INTERVAL] =
        ApicTimerCountFromInterval(HalCurProfileInterval);
}

static
VOID
ApicStartRuntimeTimer(VOID)
{
    ULONG TimerFrequency, TimerCount;

    TimerFrequency = HalpApicTimerFrequency;
    if (!TimerFrequency)
        KeBugCheck(HAL_INITIALIZATION_FAILED);
    TimerCount = (ULONG)(((ULONGLONG)TimerFrequency +
                          AP_RUNTIME_CLOCK_HZ / 2) /
                         AP_RUNTIME_CLOCK_HZ);
    ApicProgramTimer(CLOCK_IPI_VECTOR, max(1, TimerCount), FALSE);
}

VOID
NTAPI
ApicInitializeTimer(ULONG Cpu)
{
    /* The BSP keeps the RTC as its clock source. */
    NT_ASSERT(Cpu != 0);
    NT_ASSERT(HalpApicTimerFrequency != 0);
    KeGetPcr()->HalReserved[HAL_PROFILING_INTERVAL] =
        ApicTimerCountFromInterval(HalCurProfileInterval);
    ApicStartRuntimeTimer();
}

VOID
FASTCALL
HalpProfileInterruptHandler(_In_ PKTRAP_FRAME TrapFrame)
{
    KIRQL Irql;

    KiEnterInterruptTrap(TrapFrame);
#ifdef _M_AMD64
    TrapFrame->ErrorCode = 0x50726f66;
#endif

    if (!HalBeginSystemInterrupt(APIC_PROFILE_LEVEL,
                                 APIC_PROFILE_VECTOR,
                                 &Irql))
    {
#ifdef _M_IX86
        KiEoiHelper(TrapFrame);
#endif
        return;
    }

    KeProfileInterruptWithSource(TrapFrame, ProfileTime);
    KiEndInterrupt(Irql, TrapFrame);
}


/* PUBLIC FUNCTIONS ***********************************************************/

VOID
NTAPI
HalInitializeProfiling(VOID)
{
    KeGetPcr()->HalReserved[HAL_PROFILING_INTERVAL] =
        ApicTimerCountFromInterval(HalCurProfileInterval);
    KeGetPcr()->HalReserved[HAL_PROFILING_MULTIPLIER] = 1;
    KeGetPcr()->HalReserved[HAL_PROFILING_ACTIVE] = FALSE;
}

VOID
NTAPI
HalStartProfileInterrupt(IN KPROFILE_SOURCE ProfileSource)
{
    PKPRCB Prcb;

    if (ProfileSource != ProfileTime ||
        KeGetPcr()->HalReserved[HAL_PROFILING_ACTIVE])
    {
        return;
    }

    Prcb = KeGetCurrentPrcb();

    /*
     * An AP's local timer normally supplies its runtime clock.  Publish the
     * AP before switching that timer to profiling so the BSP supplies runtime
     * ticks for the duration of profiling.
     */
    if (Prcb->Number != 0)
    {
        InterlockedOrAffinity((volatile LONG_PTR *)&HalpProfilingProcessorMask,
                              (LONG_PTR)Prcb->SetMember);
    }

    KeGetPcr()->HalReserved[HAL_PROFILING_ACTIVE] = TRUE;
    HalIsProfiling = (InterlockedIncrement(&HalpProfilingProcessorCount) != 0);
    ApicProgramTimer(APIC_PROFILE_VECTOR,
                     KeGetPcr()->HalReserved[HAL_PROFILING_INTERVAL],
                     FALSE);
}

VOID
NTAPI
HalStopProfileInterrupt(IN KPROFILE_SOURCE ProfileSource)
{
    PKPRCB Prcb;
    LONG ProfilingProcessorCount;

    if (ProfileSource != ProfileTime ||
        !KeGetPcr()->HalReserved[HAL_PROFILING_ACTIVE])
    {
        return;
    }

    Prcb = KeGetCurrentPrcb();
    if (Prcb->Number == 0)
    {
        ApicProgramTimer(APIC_PROFILE_VECTOR, 0, TRUE);
    }
    else
    {
        /* Restore the AP's processor-local runtime clock before dropping it. */
        ApicStartRuntimeTimer();
        InterlockedAndAffinity((volatile LONG_PTR *)&HalpProfilingProcessorMask,
                               (LONG_PTR)~Prcb->SetMember);
    }

    KeGetPcr()->HalReserved[HAL_PROFILING_ACTIVE] = FALSE;
    ProfilingProcessorCount = InterlockedDecrement(&HalpProfilingProcessorCount);
    NT_ASSERT(ProfilingProcessorCount >= 0);
    HalIsProfiling = (ProfilingProcessorCount != 0);
}

ULONG_PTR
NTAPI
HalSetProfileInterval(IN ULONG_PTR Interval)
{
    ULONGLONG FixedInterval;
    ULONG TimerCount;

    FixedInterval = (ULONGLONG)Interval;

    if (FixedInterval < HalMinProfileInterval)
        FixedInterval = HalMinProfileInterval;
    else if (FixedInterval > HalMaxProfileInterval)
        FixedInterval = HalMaxProfileInterval;

    HalCurProfileInterval = FixedInterval;
    TimerCount = ApicTimerCountFromInterval(FixedInterval);
    KeGetPcr()->HalReserved[HAL_PROFILING_INTERVAL] = TimerCount;

    if (KeGetPcr()->HalReserved[HAL_PROFILING_ACTIVE])
        ApicProgramTimer(APIC_PROFILE_VECTOR, TimerCount, FALSE);

    return (ULONG_PTR)FixedInterval;
}
