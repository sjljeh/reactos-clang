/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor freeze support for i386
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PKPRCB KiFreezeOwner;

/* FUNCTIONS ******************************************************************/

BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_opt_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE)
        return FALSE;

    /* Capture a complete, debugger-visible state before acknowledging. */
    KiSaveProcessorState(TrapFrame, ExceptionFrame);
    KeMemoryBarrier();
    InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen,
                        IPI_FROZEN_STATE_FROZEN);

    while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
    {
        if (CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE)
        {
            KCONTINUE_STATUS ContinueStatus;

            ContinueStatus = KdReportProcessorChange();

            /* Return to the passive frozen state after debugger ownership. */
            InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen,
                                IPI_FROZEN_STATE_FROZEN);

            if (ContinueStatus == ContinueSuccess)
            {
                InterlockedExchange((PLONG)&KiFreezeOwner->IpiFrozen,
                                    IPI_FROZEN_STATE_THAW);
            }
        }

        YieldProcessor();
        KeMemoryBarrierWithoutFence();
    }

    KiRestoreProcessorState(TrapFrame, ExceptionFrame);
    KeFlushCurrentTb();

    InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen,
                        IPI_FROZEN_STATE_RUNNING);
    return TRUE;
}

VOID
NTAPI
KxFreezeExecution(VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY TargetSet;
    ULONG Processor;

    /* A recursive debugger entry by the owner has nothing more to freeze. */
    if (KiFreezeOwner == CurrentPrcb)
        return;

    while (InterlockedCompareExchangePointer(
               (PVOID volatile *)&KiFreezeOwner,
               CurrentPrcb,
               NULL) != NULL)
    {
        while (KiFreezeOwner != NULL)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }

    InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen,
                        IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE);

    TargetSet = KeActiveProcessors & ~CurrentPrcb->SetMember;
    for (Processor = 0; Processor < KeNumberProcessors; Processor++)
    {
        TargetPrcb = KiProcessorBlock[Processor];
        if (TargetSet & TargetPrcb->SetMember)
        {
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING);
            InterlockedExchange((PLONG)&TargetPrcb->IpiFrozen,
                                IPI_FROZEN_STATE_TARGET_FREEZE);
        }
    }

    KiIpiSend(TargetSet, IPI_FREEZE);

    for (Processor = 0; Processor < KeNumberProcessors; Processor++)
    {
        TargetPrcb = KiProcessorBlock[Processor];
        if (TargetSet & TargetPrcb->SetMember)
        {
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
            {
                YieldProcessor();
                KeMemoryBarrierWithoutFence();
            }
        }
    }
}

VOID
NTAPI
KxThawExecution(VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY TargetSet;
    ULONG Processor;

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    TargetSet = KeActiveProcessors & ~CurrentPrcb->SetMember;
    for (Processor = 0; Processor < KeNumberProcessors; Processor++)
    {
        TargetPrcb = KiProcessorBlock[Processor];
        if (TargetSet & TargetPrcb->SetMember)
        {
            ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);
            InterlockedExchange((PLONG)&TargetPrcb->IpiFrozen,
                                IPI_FROZEN_STATE_THAW);
        }
    }

    for (Processor = 0; Processor < KeNumberProcessors; Processor++)
    {
        TargetPrcb = KiProcessorBlock[Processor];
        if (TargetSet & TargetPrcb->SetMember)
        {
            while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
            {
                YieldProcessor();
                KeMemoryBarrierWithoutFence();
            }
        }
    }

    InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen,
                        IPI_FROZEN_STATE_RUNNING);
    InterlockedExchangePointer((PVOID volatile *)&KiFreezeOwner, NULL);
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    if (ProcessorIndex >= KeNumberProcessors)
        return ContinueError;

    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);
    InterlockedAnd((PLONG)&CurrentPrcb->IpiFrozen,
                   ~IPI_FROZEN_FLAG_ACTIVE);

    TargetPrcb = KiProcessorBlock[ProcessorIndex];
    InterlockedOr((PLONG)&TargetPrcb->IpiFrozen,
                  IPI_FROZEN_FLAG_ACTIVE);

    if (KiFreezeOwner != CurrentPrcb)
        return ContinueNextProcessor;

    while (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_OWNER)
    {
        YieldProcessor();
        KeMemoryBarrierWithoutFence();
    }

    if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
    {
        InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen,
                            IPI_FROZEN_STATE_OWNER |
                            IPI_FROZEN_FLAG_ACTIVE);
        return ContinueSuccess;
    }

    ASSERT(CurrentPrcb->IpiFrozen ==
           (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));
    return ContinueProcessorReselected;
}
