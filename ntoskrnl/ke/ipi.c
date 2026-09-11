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

#if defined(CONFIG_SMP) && defined(_M_IX86)
typedef struct _KI_IPI_PACKET_MAILBOX
{
    PKIPI_WORKER volatile WorkerRoutine;
    PVOID volatile CurrentPacket[3];
} KI_IPI_PACKET_MAILBOX, *PKI_IPI_PACKET_MAILBOX;

/*
 * The legacy i386 KPRCB has only one incoming SignalDone slot. Keep one
 * mailbox for every sender/target pair instead, so independent processors can
 * issue non-blocking packet requests without waiting on each other's slots.
 */
static KI_IPI_PACKET_MAILBOX
    KiIpiPacketMailboxes[MAXIMUM_PROCESSORS][MAXIMUM_PROCESSORS];
static volatile KAFFINITY KiIpiSenderSummary[MAXIMUM_PROCESSORS];
#endif

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
    volatile LONG *Barrier = Count;

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
                IN PVOID Parameter1,
                IN PVOID Parameter2,
                IN PVOID Parameter3)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;
    PKI_IPI_PACKET_MAILBOX Mailbox;
    KAFFINITY ProcessorMask;
    ULONG Processor, Sender;

    ASSERT(KeGetCurrentIrql() >= SYNCH_LEVEL);
    ASSERT(KeGetCurrentIrql() < IPI_LEVEL);
    ASSERT(WorkerFunction != NULL);

    /* Packet execution on the sender is handled by the caller. */
    TargetProcessors &= KeActiveProcessors & ~CurrentPrcb->SetMember;
    if (TargetProcessors == 0)
        return;

    ASSERT(CurrentPrcb->TargetSet == 0);
    Sender = CurrentPrcb->Number;
    ASSERT(Sender < MAXIMUM_PROCESSORS);

    InterlockedExchange((PLONG)&CurrentPrcb->TargetSet,
                        (LONG)TargetProcessors);

    for (Processor = 0, ProcessorMask = 1;
         Processor < KeNumberProcessors;
         Processor++, ProcessorMask <<= 1)
    {
        if (!(TargetProcessors & ProcessorMask))
            continue;

        TargetPrcb = KiProcessorBlock[Processor];
        Mailbox = &KiIpiPacketMailboxes[Processor][Sender];

        /* This sender cannot reuse a mailbox before its packet completes. */
        ASSERT(!(KiIpiSenderSummary[Processor] & CurrentPrcb->SetMember));

        Mailbox->CurrentPacket[0] = Parameter1;
        Mailbox->CurrentPacket[1] = Parameter2;
        Mailbox->CurrentPacket[2] = Parameter3;
        Mailbox->WorkerRoutine = WorkerFunction;

        /* Publish the packet before exposing the sender and request bits. */
        KeMemoryBarrier();
        InterlockedOr((PLONG)&KiIpiSenderSummary[Processor],
                      (LONG)CurrentPrcb->SetMember);

        InterlockedOr((PLONG)&TargetPrcb->RequestSummary,
                      IPI_PACKET_READY);
    }

    /* Every target can now discover its packet by sender number. */
    KeMemoryBarrier();
    HalRequestIpi(TargetProcessors);
#else
    UNREFERENCED_PARAMETER(TargetProcessors);
    UNREFERENCED_PARAMETER(WorkerFunction);
    UNREFERENCED_PARAMETER(Parameter1);
    UNREFERENCED_PARAMETER(Parameter2);
    UNREFERENCED_PARAMETER(Parameter3);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDone(IN PKIPI_CONTEXT PacketContext)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    PKPRCB SenderPrcb = PacketContext;
    KAFFINITY SetMember = KeGetCurrentPrcb()->SetMember;
    KAFFINITY OldTargetSet;

    ASSERT(SenderPrcb != NULL);

    /* Clearing our bit releases a sender waiting for packet completion. */
    OldTargetSet = (KAFFINITY)InterlockedAnd(
        (PLONG)&SenderPrcb->TargetSet, ~(LONG)SetMember);
    ASSERT(OldTargetSet & SetMember);
#else
    UNREFERENCED_PARAMETER(PacketContext);
#endif
}

VOID
FASTCALL
KiIpiSignalPacketDoneAndStall(IN PKIPI_CONTEXT PacketContext,
                              IN volatile ULONG *ReverseStall)
{
#if defined(CONFIG_SMP) && defined(_M_IX86)
    ULONG EntryPhase;

    ASSERT(ReverseStall != NULL);

    /* Capture the phase before releasing the source packet. */
    EntryPhase = *ReverseStall;
    KeMemoryBarrierWithoutFence();
    KiIpiSignalPacketDone(PacketContext);

    while (*ReverseStall == EntryPhase)
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
    PKI_IPI_PACKET_MAILBOX Mailbox;
    PKIPI_WORKER WorkerFunction;
    PVOID Parameter1, Parameter2, Parameter3;
    KAFFINITY SenderSet, SenderMask;
    ULONG RequestSummary;
    ULONG Sender;

    ASSERT(KeGetCurrentIrql() == IPI_LEVEL);

    /* Requests may coalesce while this vector is pending. Drain them all. */
    while ((RequestSummary = (ULONG)InterlockedExchange(
                (PLONG)&Prcb->RequestSummary, 0)) != 0)
    {
        if (RequestSummary & IPI_FREEZE)
        {
            /*
             * A concurrent debugger entrant can observe TARGET_FREEZE and
             * join the freeze before the corresponding IPI is delivered.
             * Its delayed request is then stale and needs no further action.
             */
            (VOID)KiProcessorFreezeHandler(TrapFrame, ExceptionFrame);
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
            SenderSet = (KAFFINITY)InterlockedExchange(
                (PLONG)&KiIpiSenderSummary[Prcb->Number], 0);

            for (Sender = 0, SenderMask = 1;
                 Sender < KeNumberProcessors;
                 Sender++, SenderMask <<= 1)
            {
                if (!(SenderSet & SenderMask))
                    continue;

                SenderPrcb = KiProcessorBlock[Sender];
                ASSERT(SenderPrcb != NULL);
                Mailbox = &KiIpiPacketMailboxes[Prcb->Number][Sender];

                /* Snapshot the mailbox before invoking arbitrary code. */
                KeMemoryBarrier();
                WorkerFunction = Mailbox->WorkerRoutine;
                Parameter1 = (PVOID)Mailbox->CurrentPacket[0];
                Parameter2 = (PVOID)Mailbox->CurrentPacket[1];
                Parameter3 = (PVOID)Mailbox->CurrentPacket[2];
                ASSERT(WorkerFunction != NULL);

                WorkerFunction((PKIPI_CONTEXT)SenderPrcb,
                               Parameter1,
                               Parameter2,
                               Parameter3);
            }
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
    KAFFINITY RemainingSet;
    volatile LONG Count;
    PKPRCB Prcb;
#endif

    ASSERT(Function != NULL);

    /* Raise high enough to prevent migration and nested packet producers. */
    OldIrql = KeGetCurrentIrql();
    ASSERT(OldIrql < IPI_LEVEL);
    if (OldIrql < SYNCH_LEVEL) KeRaiseIrql(SYNCH_LEVEL, &OldIrql);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    Prcb = KeGetCurrentPrcb();

    /* Get the target affinity after migration is disabled. */
    Affinity = KeActiveProcessors & ~Prcb->SetMember;

    Count = 1;
    for (RemainingSet = Affinity;
         RemainingSet != 0;
         RemainingSet &= RemainingSet - 1)
    {
        Count++;
    }
#endif

    /* Serialize generic calls so every sender owns one complete packet. */
    KeAcquireSpinLockAtDpcLevel(&KiReverseStallIpiLock);

#if defined(CONFIG_SMP) && defined(_M_IX86)
    if (Affinity)
    {
        KiIpiSendPacket(Affinity,
                        KiIpiGenericCallTarget,
                        (PVOID)(ULONG_PTR)Function,
                        (PVOID)Argument,
                        (PVOID)&Count);

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
