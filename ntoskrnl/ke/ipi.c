/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/ipi.c
 * PURPOSE:         Inter-Processor Packet Interface
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern KSPIN_LOCK KiReverseStallIpiLock;

/* PRIVATE FUNCTIONS *********************************************************/

#ifndef _M_AMD64

VOID
NTAPI
KiIpiGenericCallTarget(IN PKIPI_CONTEXT PacketContext,
                       IN PVOID BroadcastFunction,
                       IN PVOID Argument,
                       IN PVOID Count)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    volatile ULONG *Barrier = Count;

    /* Report that this processor has reached the entry barrier. */
    InterlockedDecrement((PLONG)Barrier);

    /* Start the calls only after every target and the sender are ready. */
    while (*Barrier != 0)
    {
        YieldProcessor();
        KeMemoryBarrierWithoutFence();
    }

    ((PKIPI_BROADCAST_WORKER)BroadcastFunction)((ULONG_PTR)Argument);
    KiIpiSignalPacketDone(PacketContext);
#else
    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Argument);
    UNREFERENCED_PARAMETER(Count);
#endif
}

VOID
FASTCALL
KiIpiSend(IN KAFFINITY TargetProcessors,
          IN ULONG IpiRequest)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    KAFFINITY ProcessorMask;
    ULONG Processor;

    ASSERT((IpiRequest & ~(IPI_APC | IPI_DPC | IPI_FREEZE)) == 0);

    /* Do not publish requests for processors that are not online. */
    TargetProcessors &= KeActiveProcessors;
    if (TargetProcessors == 0)
        return;

    for (Processor = 0, ProcessorMask = 1;
         Processor < KeNumberProcessors;
         Processor++, ProcessorMask <<= 1)
    {
        if (TargetProcessors & ProcessorMask)
        {
            InterlockedOr((PLONG)&KiProcessorBlock[Processor]->RequestSummary,
                          IpiRequest);
        }
    }

    /* Publish every request before making the interrupt observable. */
    KeMemoryBarrier();
    HalRequestIpi(TargetProcessors);
#else
    UNREFERENCED_PARAMETER(TargetProcessors);
    UNREFERENCED_PARAMETER(IpiRequest);
#endif
}

VOID
NTAPI
KiIpiSendPacket(IN KAFFINITY TargetProcessors,
                IN PKIPI_WORKER WorkerFunction,
                IN PKIPI_BROADCAST_WORKER BroadcastFunction,
                IN ULONG_PTR Context,
                IN PULONG Count)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    KAFFINITY ProcessorMask;
    ULONG Processor;

    /* Packet execution on the sender is handled by the caller. */
    TargetProcessors &= KeActiveProcessors & ~CurrentPrcb->SetMember;
    if (TargetProcessors == 0)
        return;

    ASSERT(CurrentPrcb->TargetSet == 0);

    CurrentPrcb->CurrentPacket[0] = (PVOID)BroadcastFunction;
    CurrentPrcb->CurrentPacket[1] = (PVOID)Context;
    CurrentPrcb->CurrentPacket[2] = Count;
    CurrentPrcb->WorkerRoutine = WorkerFunction;
    InterlockedExchange((PLONG)&CurrentPrcb->TargetSet,
                        (LONG)TargetProcessors);

    for (Processor = 0, ProcessorMask = 1;
         Processor < KeNumberProcessors;
         Processor++, ProcessorMask <<= 1)
    {
        if (!(TargetProcessors & ProcessorMask))
            continue;

        TargetPrcb = KiProcessorBlock[Processor];

        /* A target has one incoming packet slot. Wait until it is free. */
        while (InterlockedCompareExchangePointer(
                   (PVOID volatile *)&TargetPrcb->SignalDone,
                   CurrentPrcb,
                   NULL) != NULL)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }

        InterlockedOr((PLONG)&TargetPrcb->RequestSummary,
                      IPI_PACKET_READY);

        /* Deliver now so a competing sender cannot form a slot-wait cycle. */
        HalRequestIpi(ProcessorMask);
    }
#else
    UNREFERENCED_PARAMETER(TargetProcessors);
    UNREFERENCED_PARAMETER(WorkerFunction);
    UNREFERENCED_PARAMETER(BroadcastFunction);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Count);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDone(IN PKIPI_CONTEXT PacketContext)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    PKPRCB SenderPrcb = PacketContext;
    KAFFINITY SetMember = KeGetCurrentPrcb()->SetMember;

    ASSERT(SenderPrcb != NULL);
    ASSERT(SenderPrcb->TargetSet & SetMember);

    /* Clearing our bit releases a sender waiting for packet completion. */
    InterlockedAnd((PLONG)&SenderPrcb->TargetSet, ~(LONG)SetMember);
#else
    UNREFERENCED_PARAMETER(PacketContext);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(IN PKIPI_CONTEXT PacketContext,
                              IN volatile PULONG ReverseStall)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    ASSERT(ReverseStall != NULL);
    KiIpiSignalPacketDone(PacketContext);

    while (*ReverseStall != 0)
    {
        YieldProcessor();
        KeMemoryBarrierWithoutFence();
    }
#else
    UNREFERENCED_PARAMETER(PacketContext);
    UNREFERENCED_PARAMETER(ReverseStall);
#endif
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
KiIpiServiceRoutine(IN PKTRAP_FRAME TrapFrame,
                    IN PKEXCEPTION_FRAME ExceptionFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);
    UNREFERENCED_PARAMETER(ExceptionFrame);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKPRCB SenderPrcb;
    PKIPI_WORKER WorkerFunction;
    PVOID Parameter1, Parameter2, Parameter3;
    PVOID PreviousSignal;
    ULONG RequestSummary;

    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);

    /* Requests may coalesce while this vector is pending. Drain them all. */
    while ((RequestSummary = (ULONG)InterlockedExchange(
                (PLONG)&Prcb->RequestSummary, 0)) != 0)
    {
        if (RequestSummary & IPI_FREEZE)
        {
            NT_VERIFY(KiProcessorFreezeHandler(TrapFrame, ExceptionFrame));
        }

        if (RequestSummary & IPI_APC)
        {
            HalRequestSoftwareInterrupt(APC_LEVEL);
        }

        if (RequestSummary & IPI_DPC)
        {
            Prcb->DpcInterruptRequested = TRUE;
            HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
        }

        if (RequestSummary & IPI_PACKET_READY)
        {
            SenderPrcb = (PKPRCB)InterlockedCompareExchangePointer(
                (PVOID volatile *)&Prcb->SignalDone, NULL, NULL);
            ASSERT(SenderPrcb != NULL);

            /* Snapshot the sender's packet before invoking arbitrary code. */
            KeMemoryBarrier();
            WorkerFunction = SenderPrcb->WorkerRoutine;
            Parameter1 = (PVOID)SenderPrcb->CurrentPacket[0];
            Parameter2 = (PVOID)SenderPrcb->CurrentPacket[1];
            Parameter3 = (PVOID)SenderPrcb->CurrentPacket[2];

            WorkerFunction((PKIPI_CONTEXT)SenderPrcb,
                           Parameter1,
                           Parameter2,
                           Parameter3);

            /* Make the target packet slot available to another sender. */
            KeMemoryBarrier();
            PreviousSignal = InterlockedExchangePointer(
                (PVOID volatile *)&Prcb->SignalDone, NULL);
            ASSERT(PreviousSignal == SenderPrcb);
        }

        ASSERT((RequestSummary & ~(IPI_APC | IPI_DPC | IPI_FREEZE |
                                   IPI_PACKET_READY)) == 0);
    }
#endif

    return TRUE;
}

/*
 * @implemented
 */
ULONG_PTR
NTAPI
KeIpiGenericCall(IN PKIPI_BROADCAST_WORKER Function,
                 IN ULONG_PTR Argument)
{
    ULONG_PTR Status;
    KIRQL OldIrql, OldIrql2;
#if defined(CONFIG_SMP) && defined(_M_IX86)
    KAFFINITY Affinity;
    ULONG Count;
    PKPRCB Prcb = KeGetCurrentPrcb();
#endif

    /* Raise to DPC level if required */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    /* Get current processor count and affinity */
    Count = KeNumberProcessors;
    Affinity = KeActiveProcessors;

    /* Exclude ourselves */
    Affinity &= ~Prcb->SetMember;
#endif

    /* Serialize generic calls so every sender owns one complete packet. */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    if (Affinity)
    {
        KiIpiSendPacket(Affinity,
                        KiIpiGenericCallTarget,
                        Function,
                        Argument,
                        &Count);

        /* Wait until every remote processor has reached the barrier. */
        while (Count != 1)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }
#endif

    /* Run the broadcast at IPI_LEVEL on the source processor as well. */
    KeRaiseIrql(IPI_LEVEL, &OldIrql2);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    /* Release every target from the entry barrier. */
    InterlockedExchange((PLONG)&Count, 0);
#endif

    Status = Function(Argument);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    if (Affinity)
    {
        ASSERT(Prcb == KeGetCurrentPrcb());

        /* Do not reuse the source packet until every call has returned. */
        while (Prcb->TargetSet != 0)
        {
            YieldProcessor();
            KeMemoryBarrierWithoutFence();
        }
    }
#endif

    /* Return from IPI_LEVEL before releasing the DPC-level lock. */
    KeLowerIrql(OldIrql2);
    KeReleaseSpinLockFromDpcLevel(&KiReverseStallIpiLock);

    /* Lower IRQL back to the caller's level. */
    KeLowerIrql(OldIrql);
    return Status;
}

#endif // !_M_AMD64
