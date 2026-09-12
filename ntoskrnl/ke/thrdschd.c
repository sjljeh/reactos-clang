/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/ke/thrdschd.c
 * PURPOSE:         Kernel Thread Scheduler (Affinity, Priority, Scheduling)
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#ifdef _WIN64
# define InterlockedOrSetMember(Destination, SetMember) \
    InterlockedOr64((PLONG64)Destination, SetMember);
# define InterlockedAndSetMember(Destination, SetMember) \
    InterlockedAnd64((PLONG64)Destination, SetMember);
#else
# define InterlockedOrSetMember(Destination, SetMember) \
    InterlockedOr((PLONG)Destination, SetMember);
# define InterlockedAndSetMember(Destination, SetMember) \
    InterlockedAnd((PLONG)Destination, SetMember);
#endif

/* GLOBALS *******************************************************************/

KAFFINITY KiIdleSummary;
KAFFINITY KiIdleSMTSummary;
volatile KAFFINITY KiIdleSpinSummary;
KI_SCHEDULER_CPU_DATA KiSchedulerCpuData[MAXIMUM_PROCESSORS];

/* FUNCTIONS *****************************************************************/

#ifdef CONFIG_SMP
#define KI_MAXIMUM_STEAL_SCAN 32

static
PKTHREAD
KiFindStealableThread(
    _In_ PKPRCB SourcePrcb,
    _In_ ULONG TargetProcessor,
    _In_ KPRIORITY MaximumPriority)
{
    ULONG Summary, Priority, ScanCount;
    PLIST_ENTRY ListHead, ListEntry;
    PKTHREAD Thread;

    ASSERT(SourcePrcb->PrcbLock != 0);
    Summary = SourcePrcb->ReadySummary;
    ScanCount = 0;

    /* Preserve normal priority ordering while looking for legal affinity. */
    while (Summary)
    {
        NT_VERIFY(BitScanReverse(&Priority, Summary) != FALSE);
        ListHead = &SourcePrcb->DispatcherReadyListHead[Priority];

        for (ListEntry = ListHead->Flink;
             ListEntry != ListHead;
             ListEntry = ListEntry->Flink)
        {
            Thread = CONTAINING_RECORD(ListEntry, KTHREAD, WaitListEntry);
            ASSERT(Thread->State == Ready);
            ASSERT(Thread->NextProcessor == SourcePrcb->Number);

            if ((Thread->Priority <= MaximumPriority) &&
                (Thread->Affinity & AFFINITY_MASK(TargetProcessor)))
            {
                KiRemoveReadyQueue(SourcePrcb, Thread);
                return Thread;
            }

            /* Keep idle-side balancing work bounded under pinned load. */
            if (++ScanCount == KI_MAXIMUM_STEAL_SCAN) return NULL;
        }

        Summary &= ~PRIORITY_MASK(Priority);
    }

    return NULL;
}

static
BOOLEAN
KiStealReadyThread(
    _In_ PKPRCB TargetPrcb)
{
    KAFFINITY CandidateSet, ScanSet;
    PKPRCB SourcePrcb;
    PKTHREAD Thread;
    LONG ReadyCount, HighestReadyCount;
    ULONG Processor, SourceProcessor;

    ASSERT(TargetPrcb == KeGetCurrentPrcb());
    ASSERT(KeGetCurrentIrql() >= SYNCH_LEVEL);
    ASSERT(TargetPrcb->CurrentThread == TargetPrcb->IdleThread);

    Thread = NULL;
    CandidateSet = KeActiveProcessors & ~TargetPrcb->SetMember;
    while (CandidateSet)
    {
        /* Pick the queue with the most immediately transferable work. */
        SourceProcessor = MAXULONG;
        HighestReadyCount = 0;
        ScanSet = CandidateSet;
        while (ScanSet)
        {
            NT_VERIFY(BitScanForwardAffinity(&Processor, ScanSet) != FALSE);
            ScanSet &= ~AFFINITY_MASK(Processor);

            ReadyCount =
                KiSchedulerCpuData[Processor].TransferableReadyThreadCount;
            if (ReadyCount > HighestReadyCount)
            {
                HighestReadyCount = ReadyCount;
                SourceProcessor = Processor;
            }
        }

        if (SourceProcessor == MAXULONG) return FALSE;
        CandidateSet &= ~AFFINITY_MASK(SourceProcessor);
        SourcePrcb = KiProcessorBlock[SourceProcessor];

        /*
         * Never make an idle CPU wait on a busy run-queue lock.  Holding the
         * target lock closes the gap while a ready thread changes queues.
         */
        if (!KiTryAcquirePrcbLock(TargetPrcb)) return FALSE;
        if (TargetPrcb->NextThread ||
            TargetPrcb->ReadySummary ||
            (TargetPrcb->CurrentThread != TargetPrcb->IdleThread))
        {
            KiReleasePrcbLock(TargetPrcb);
            return FALSE;
        }

        if (!KiTryAcquirePrcbLock(SourcePrcb))
        {
            KiReleasePrcbLock(TargetPrcb);
            continue;
        }

        Thread = KiFindStealableThread(SourcePrcb,
                                       TargetPrcb->Number,
                                       HIGH_PRIORITY);
        if (Thread)
        {
            Thread->NextProcessor = TargetPrcb->Number;
            Thread->State = Standby;
            TargetPrcb->NextThread = Thread;
            TargetPrcb->IdleSchedule = FALSE;
            InterlockedAndSetMember(&KiIdleSummary, ~TargetPrcb->SetMember);
            KiSchedulerCpuData[TargetPrcb->Number].FindAny++;
        }

        KiReleasePrcbLock(SourcePrcb);
        KiReleasePrcbLock(TargetPrcb);

        if (Thread) return TRUE;
    }

    return FALSE;
}
#endif

PKTHREAD
FASTCALL
KiIdleSchedule(IN PKPRCB Prcb)
{
    PKTHREAD IdleThread, Thread;

    ASSERT(Prcb == KeGetCurrentPrcb());
    ASSERT(KeGetCurrentIrql() >= SYNCH_LEVEL);

    /* Serialize against processors assigning work to this PRCB. */
    KiAcquirePrcbLock(Prcb);
    IdleThread = Prcb->IdleThread;
    ASSERT(Prcb->CurrentThread == IdleThread);

    /* Prefer a thread already selected by a remote processor. */
    Thread = Prcb->NextThread;
    if (Thread)
    {
        Prcb->NextThread = NULL;

        /* A stale self-selection does not require a context switch. */
        if (Thread == IdleThread)
        {
            Thread->State = Running;
            Thread = NULL;
        }
    }

    /* Otherwise, consume work queued on this processor. */
    if (!Thread)
        Thread = KiSelectReadyThread(0, Prcb);

#ifdef CONFIG_SMP
    /* Pull queued work before committing this processor to another idle pass. */
    if (!Thread)
    {
        KiReleasePrcbLock(Prcb);
        KiStealReadyThread(Prcb);
        KiAcquirePrcbLock(Prcb);

        /* A remote ready or the steal may have supplied local work. */
        Thread = Prcb->NextThread;
        if (Thread)
            Prcb->NextThread = NULL;
        else
            Thread = KiSelectReadyThread(0, Prcb);
    }
#endif

    if (Thread == IdleThread)
    {
        Thread->State = Running;
        Thread = NULL;
    }

    if (Thread)
    {
        /* Protect the idle stack until it is resumed on this processor. */
        KiSetThreadSwapBusy(IdleThread);

        /* Commit the selected thread before dropping the PRCB lock. */
        Prcb->CurrentThread = Thread;
        Thread->State = Running;
        InterlockedAndSetMember(&KiIdleSummary, ~Prcb->SetMember);
    }
    else
    {
        /* There is no runnable work; continue to advertise this CPU. */
        InterlockedOrSetMember(&KiIdleSummary, Prcb->SetMember);
    }

    /* One scheduling pass is complete; new work will set NextThread. */
    Prcb->IdleSchedule = FALSE;
    KiReleasePrcbLock(Prcb);
    return Thread;
}

VOID
FASTCALL
KiProcessDeferredReadyList(IN PKPRCB Prcb)
{
    PSINGLE_LIST_ENTRY ListEntry;
    PKTHREAD Thread;

    /* Make sure there is something on the ready list */
    ASSERT(Prcb->DeferredReadyListHead.Next != NULL);

    /* Get the first entry and clear the list */
    ListEntry = Prcb->DeferredReadyListHead.Next;
    Prcb->DeferredReadyListHead.Next = NULL;

    /* Start processing loop */
    do
    {
        /* Get the thread and advance to the next entry */
        Thread = CONTAINING_RECORD(ListEntry, KTHREAD, SwapListEntry);
        ListEntry = ListEntry->Next;

        /* Make the thread ready */
        KiDeferredReadyThread(Thread);
    } while (ListEntry != NULL);

    /* Make sure the ready list is still empty */
    ASSERT(Prcb->DeferredReadyListHead.Next == NULL);
}

VOID
FASTCALL
KiQueueReadyThread(IN PKTHREAD Thread,
                   IN PKPRCB Prcb)
{
    /* Call the macro. We keep the API for compatibility with ASM code */
    KxQueueReadyThread(Thread, Prcb);
}

#ifdef CONFIG_SMP
static
ULONG
KiQueryProcessorLoad(
    _In_ PKPRCB Prcb)
{
    LONG ReadyCount;
    ULONG Load;
    PKTHREAD CurrentThread, NextThread;

    /*
     * The ready count is changed with this PRCB's lock held.  A racy snapshot
     * is sufficient for placement: the result is a hint and is re-evaluated
     * on every ready transition.
     */
    ReadyCount = KiSchedulerCpuData[Prcb->Number].ReadyThreadCount;
    Load = (ReadyCount > 0) ? (ULONG)ReadyCount : 0;

    CurrentThread = Prcb->CurrentThread;
    if (CurrentThread && (CurrentThread != Prcb->IdleThread)) Load++;

    NextThread = Prcb->NextThread;
    if (NextThread && (NextThread != Prcb->IdleThread)) Load++;

    return Load;
}

static
BOOLEAN
KiTryBalanceReadyQueuePair(
    _In_ PKPRCB SourcePrcb,
    _In_ PKPRCB TargetPrcb)
{
    PKPRCB FirstPrcb, SecondPrcb;
    PKTHREAD Thread, CurrentThread, NextThread;
    KPRIORITY MaximumPriority;
    ULONG SourceLoad, TargetLoad;
    BOOLEAN RequestInterrupt, Moved;

    ASSERT(SourcePrcb != TargetPrcb);
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    /* Use a stable order even though both acquisitions are nonblocking. */
    if (SourcePrcb->Number < TargetPrcb->Number)
    {
        FirstPrcb = SourcePrcb;
        SecondPrcb = TargetPrcb;
    }
    else
    {
        FirstPrcb = TargetPrcb;
        SecondPrcb = SourcePrcb;
    }

    if (!KiTryAcquirePrcbLock(FirstPrcb)) return FALSE;
    if (!KiTryAcquirePrcbLock(SecondPrcb))
    {
        KiReleasePrcbLock(FirstPrcb);
        return FALSE;
    }

    Moved = FALSE;
    RequestInterrupt = FALSE;
    SourceLoad = KiQueryProcessorLoad(SourcePrcb);
    TargetLoad = KiQueryProcessorLoad(TargetPrcb);
    /* Moving one runnable thread must reduce, rather than reverse, imbalance. */
    if ((SourceLoad <= TargetLoad) || ((SourceLoad - TargetLoad) <= 1))
        goto Exit;

    /*
     * Do not displace an already selected thread.  A candidate no stronger
     * than that thread can still be queued without delaying a preemption that
     * has already been requested.
     */
    NextThread = TargetPrcb->NextThread;
    MaximumPriority = NextThread ? NextThread->Priority : HIGH_PRIORITY;
    Thread = KiFindStealableThread(SourcePrcb,
                                   TargetPrcb->Number,
                                   MaximumPriority);
    if (!Thread) goto Exit;

    Thread->NextProcessor = TargetPrcb->Number;
    CurrentThread = TargetPrcb->CurrentThread;
    ASSERT(CurrentThread != NULL);

    if (!NextThread && (Thread->Priority > CurrentThread->Priority))
    {
        /* The migrated thread must preempt the target's current thread. */
        if (CurrentThread->State == Running) CurrentThread->Preempted = TRUE;
        Thread->State = Standby;
        TargetPrcb->NextThread = Thread;
        TargetPrcb->IdleSchedule = FALSE;
        InterlockedAndSetMember(&KiIdleSummary, ~TargetPrcb->SetMember);
        RequestInterrupt = TRUE;
        if (TargetPrcb->Number == KeGetCurrentProcessorNumber())
            KiSchedulerCpuData[KeGetCurrentProcessorNumber()].PreemptCurrent++;
        else
            KiSchedulerCpuData[KeGetCurrentProcessorNumber()].PreemptAny++;
    }
    else
    {
        /* No immediate preemption is required; retain normal FIFO ordering. */
        Thread->State = Ready;
        KiInsertReadyQueue(TargetPrcb, Thread, FALSE);
    }

    KiSchedulerCpuData[KeGetCurrentProcessorNumber()].FindAny++;
    Moved = TRUE;

Exit:
    KiReleasePrcbLock(SecondPrcb);
    KiReleasePrcbLock(FirstPrcb);

    if (RequestInterrupt &&
        (TargetPrcb->Number != KeGetCurrentProcessorNumber()))
    {
        KiRequestSchedulerInterrupt(TargetPrcb->Number);
    }

    return Moved;
}

VOID
NTAPI
KiBalanceReadyQueues(VOID)
{
    static ULONG BalanceSeed;
    KAFFINITY SourceSet, TargetSet;
    PKPRCB SourcePrcb, TargetPrcb, Prcb;
    LONG ReadyCount;
    ULONG Start, Offset, Processor;
    ULONG SourceProcessor, TargetProcessor;
    ULONG Load, HighestLoad, LowestLoad;

    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);
    if (KeNumberProcessors < 2) return;

    Start = BalanceSeed + 1;
    if (Start >= KeNumberProcessors) Start = 0;
    BalanceSeed = Start;

    SourceSet = KeActiveProcessors;
    while (SourceSet)
    {
        SourceProcessor = MAXULONG;
        HighestLoad = 0;

        /* Select the busiest CPU which has a queued thread to transfer. */
        for (Offset = 0; Offset < KeNumberProcessors; Offset++)
        {
            Processor = Start + Offset;
            if (Processor >= KeNumberProcessors) Processor -= KeNumberProcessors;
            if (!(SourceSet & AFFINITY_MASK(Processor))) continue;

            ReadyCount =
                KiSchedulerCpuData[Processor].TransferableReadyThreadCount;
            if (ReadyCount <= 0) continue;

            Prcb = KiProcessorBlock[Processor];
            Load = KiQueryProcessorLoad(Prcb);
            if ((SourceProcessor == MAXULONG) || (Load > HighestLoad))
            {
                SourceProcessor = Processor;
                HighestLoad = Load;
            }
        }

        if (SourceProcessor == MAXULONG) return;
        SourceSet &= ~AFFINITY_MASK(SourceProcessor);
        SourcePrcb = KiProcessorBlock[SourceProcessor];

        TargetSet = KeActiveProcessors & ~AFFINITY_MASK(SourceProcessor);
        while (TargetSet)
        {
            TargetProcessor = MAXULONG;
            LowestLoad = MAXULONG;

            /* Select the least-loaded remaining destination. */
            for (Offset = 0; Offset < KeNumberProcessors; Offset++)
            {
                Processor = Start + Offset;
                if (Processor >= KeNumberProcessors) Processor -= KeNumberProcessors;
                if (!(TargetSet & AFFINITY_MASK(Processor))) continue;

                Prcb = KiProcessorBlock[Processor];
                Load = KiQueryProcessorLoad(Prcb);
                if (Load < LowestLoad)
                {
                    TargetProcessor = Processor;
                    LowestLoad = Load;
                }
            }

            if ((TargetProcessor == MAXULONG) ||
                (HighestLoad <= LowestLoad) ||
                ((HighestLoad - LowestLoad) <= 1))
            {
                break;
            }

            TargetSet &= ~AFFINITY_MASK(TargetProcessor);
            TargetPrcb = KiProcessorBlock[TargetProcessor];
            if (KiTryBalanceReadyQueuePair(SourcePrcb, TargetPrcb)) return;
        }
    }
}

ULONG
NTAPI
KiFindIdealProcessor(
    _In_ KAFFINITY ProcessorSet,
    _In_ UCHAR OriginalIdealProcessor)
{
    PKPRCB OriginalIdealPrcb;
    KAFFINITY NodeMask;
    ULONG Processor;

    /* Never select a processor that has not entered the active set. */
    ProcessorSet &= KeActiveProcessors;
    if (!ProcessorSet)
    {
        KeBugCheckEx(INVALID_AFFINITY_SET,
                     OriginalIdealProcessor,
                     KeActiveProcessors,
                     0,
                     0);
    }

    /* Check if we can use the original ideal processor */
    if ((OriginalIdealProcessor < MAXIMUM_PROCESSORS) &&
        (ProcessorSet & AFFINITY_MASK(OriginalIdealProcessor)))
    {
        /* We can, so use it */
        return OriginalIdealProcessor;
    }

    /* Get the original ideal PRCB, if that processor was initialized. */
    OriginalIdealPrcb =
        (OriginalIdealProcessor < MAXIMUM_PROCESSORS) ?
        KiProcessorBlock[OriginalIdealProcessor] : NULL;

    /* Check if we can use the original node */
    if (OriginalIdealPrcb && OriginalIdealPrcb->ParentNode)
    {
        NodeMask = OriginalIdealPrcb->ParentNode->ProcessorMask & ProcessorSet;
        if (NodeMask)
        {
            /* Use the node set instead */
            ProcessorSet = NodeMask;
        }
    }

    /* Calculate the ideal CPU from the affinity set */
    BitScanReverseAffinity(&Processor, ProcessorSet);
    return Processor;
}

static
ULONG
KiSelectNextProcessor(
    _In_ PKTHREAD Thread)
{
    PKI_SCHEDULER_CPU_DATA SchedulerData;
    KAFFINITY PreferredSet, IdleSet, ScanSet;
    ULONG Processor, CurrentProcessor, LastProcessor, IdealProcessor;
    ULONG Load, LowestLoad, LowestProcessor;
    BOOLEAN HasLastProcessor;

    /* Start with the affinity, restricted to online processors. */
    PreferredSet = Thread->Affinity & KeActiveProcessors;
    if (!PreferredSet)
    {
        KeBugCheckEx(INVALID_AFFINITY_SET,
                     (ULONG_PTR)Thread,
                     Thread->Affinity,
                     KeActiveProcessors,
                     0);
    }

    CurrentProcessor = KeGetCurrentProcessorNumber();
    SchedulerData = &KiSchedulerCpuData[CurrentProcessor];
    LastProcessor = Thread->NextProcessor;
    IdealProcessor = Thread->IdealProcessor;
    HasLastProcessor = (Thread->ContextSwitches != 0) &&
                       (LastProcessor < MAXIMUM_PROCESSORS) &&
                       ((PreferredSet & AFFINITY_MASK(LastProcessor)) != 0);

    /* Prefer an idle CPU without discarding established cache affinity. */
    IdleSet = PreferredSet & KiIdleSummary;
    if (IdleSet != 0)
    {
        if (HasLastProcessor &&
            (IdleSet & AFFINITY_MASK(LastProcessor)))
        {
            SchedulerData->IdleLast++;
            return LastProcessor;
        }

        if ((IdealProcessor < MAXIMUM_PROCESSORS) &&
            (IdleSet & AFFINITY_MASK(IdealProcessor)))
        {
            SchedulerData->IdleIdeal++;
            return IdealProcessor;
        }

        if (IdleSet & AFFINITY_MASK(CurrentProcessor))
        {
            SchedulerData->IdleCurrent++;
            return CurrentProcessor;
        }

        NT_VERIFY(BitScanForwardAffinity(&Processor, IdleSet) != FALSE);
        SchedulerData->IdleAny++;
        return Processor;
    }

    /* Find the least loaded allowed CPU. */
    LowestLoad = MAXULONG;
    LowestProcessor = MAXULONG;
    ScanSet = PreferredSet;
    while (ScanSet)
    {
        NT_VERIFY(BitScanForwardAffinity(&Processor, ScanSet) != FALSE);
        ScanSet &= ~AFFINITY_MASK(Processor);

        Load = KiQueryProcessorLoad(KiProcessorBlock[Processor]);
        if (Load < LowestLoad)
        {
            LowestLoad = Load;
            LowestProcessor = Processor;
        }
    }

    ASSERT(LowestProcessor < KeNumberProcessors);

    /*
     * A difference of one runnable thread is deliberately tolerated.  Avoiding
     * a cold migration is normally worth one turn through a local ready queue;
     * larger imbalance is not.  The ideal processor remains a hint, not a
     * binding constraint.
     */
    if (HasLastProcessor &&
        (KiQueryProcessorLoad(KiProcessorBlock[LastProcessor]) <=
         LowestLoad + 1))
    {
        SchedulerData->FindLast++;
        return LastProcessor;
    }

    if ((IdealProcessor < MAXIMUM_PROCESSORS) &&
        (PreferredSet & AFFINITY_MASK(IdealProcessor)) &&
        (KiQueryProcessorLoad(KiProcessorBlock[IdealProcessor]) <=
         LowestLoad + 1))
    {
        SchedulerData->FindIdeal++;
        return IdealProcessor;
    }

    SchedulerData->FindAny++;
    return LowestProcessor;
}
#else
#define KiSelectNextProcessor(Thread) 0
#endif

#ifdef CONFIG_SMP
static
VOID
KiCountPreemption(
    _In_ PKTHREAD Thread,
    _In_ ULONG Processor,
    _In_ ULONG LastProcessor)
{
    PKI_SCHEDULER_CPU_DATA SchedulerData;

    SchedulerData = &KiSchedulerCpuData[KeGetCurrentProcessorNumber()];
    if (Processor == KeGetCurrentProcessorNumber())
    {
        SchedulerData->PreemptCurrent++;
    }
    else if ((Thread->ContextSwitches != 0) &&
             (Processor == LastProcessor))
    {
        SchedulerData->PreemptLast++;
    }
    else
    {
        SchedulerData->PreemptAny++;
    }
}
#endif

VOID
FASTCALL
KiDeferredReadyThread(IN PKTHREAD Thread)
{
    PKPRCB Prcb;
    BOOLEAN Preempted;
    ULONG Processor;
    KPRIORITY OldPriority;
    PKTHREAD NextThread;
#ifdef CONFIG_SMP
    ULONG LastProcessor;
#endif

    /* Sanity checks */
    ASSERT(Thread->State == DeferredReady);
    ASSERT((Thread->Priority >= 0) && (Thread->Priority <= HIGH_PRIORITY));

    /* Check if we have any adjusts to do */
    if (Thread->AdjustReason == AdjustBoost)
    {
        /* Lock the thread */
        KiAcquireThreadLock(Thread);

        /* Check if the priority is low enough to qualify for boosting */
        if ((Thread->Priority <= Thread->AdjustIncrement) &&
            (Thread->Priority < (LOW_REALTIME_PRIORITY - 3)) &&
            !(Thread->DisableBoost))
        {
            /* Calculate the new priority based on the adjust increment */
            OldPriority = min(Thread->AdjustIncrement + 1,
                              LOW_REALTIME_PRIORITY - 3);

            /* Make sure we're not decreasing outside of the priority range */
            ASSERT((Thread->PriorityDecrement >= 0) &&
                   (Thread->PriorityDecrement <= Thread->Priority));

            /* Calculate the new priority decrement based on the boost */
            Thread->PriorityDecrement += ((SCHAR)OldPriority - Thread->Priority);

            /* Again verify that this decrement is valid */
            ASSERT((Thread->PriorityDecrement >= 0) &&
                   (Thread->PriorityDecrement <= OldPriority));

            /* Set the new priority */
            Thread->Priority = (SCHAR)OldPriority;
        }

        /* We need 4 quanta, make sure we have them, then decrease by one */
        if (Thread->Quantum < 4) Thread->Quantum = 4;
        Thread->Quantum--;

        /* Make sure the priority is still valid */
        ASSERT((Thread->Priority >= 0) && (Thread->Priority <= HIGH_PRIORITY));

        /* Release the lock and clear the adjust reason */
        KiReleaseThreadLock(Thread);
        Thread->AdjustReason = AdjustNone;
    }
    else if (Thread->AdjustReason == AdjustUnwait)
    {
        /* Acquire the thread lock and check if this is a real-time thread */
        KiAcquireThreadLock(Thread);
        if (Thread->Priority < LOW_REALTIME_PRIORITY)
        {
            /* It's not real time, but is it time critical? */
            if (Thread->BasePriority >= (LOW_REALTIME_PRIORITY - 2))
            {
                /* It is, so simply reset its quantum */
                Thread->Quantum = Thread->QuantumReset;
            }
            else
            {
                /* Has the priority been adjusted previously? */
                if (!(Thread->PriorityDecrement) && (Thread->AdjustIncrement))
                {
                    /* Yes, reset its quantum */
                    Thread->Quantum = Thread->QuantumReset;
                }

                /* Wait code already handles quantum adjustment during APCs */
                if (Thread->WaitStatus != STATUS_KERNEL_APC)
                {
                    /* Decrease the quantum by one and check if we're out */
                    if (--Thread->Quantum <= 0)
                    {
                        /* We are, reset the quantum and get a new priority */
                        Thread->Quantum = Thread->QuantumReset;
                        Thread->Priority = KiComputeNewPriority(Thread, 1);
                    }
                }
            }

            /* Now check if we have no decrement and boosts are enabled */
            if (!(Thread->PriorityDecrement) && !(Thread->DisableBoost))
            {
                /* Make sure we have an increment */
                ASSERT(Thread->AdjustIncrement >= 0);

                /* Calculate the new priority after the increment */
                OldPriority = Thread->BasePriority + Thread->AdjustIncrement;

                /* Check if this is a foreground process */
                if (CONTAINING_RECORD(Thread->ApcState.Process, EPROCESS, Pcb)->
                    Vm.Flags.MemoryPriority == MEMORY_PRIORITY_FOREGROUND)
                {
                    /* Apply the foreground boost */
                    OldPriority += PsPrioritySeparation;
                }

                /* Check if this new priority is higher */
                if (OldPriority > Thread->Priority)
                {
                    /* Make sure we don't go into the real time range */
                    if (OldPriority >= LOW_REALTIME_PRIORITY)
                    {
                        /* Normalize it back down one notch */
                        OldPriority = LOW_REALTIME_PRIORITY - 1;
                    }

                    /* Check if the priority is higher then the boosted base */
                    if (OldPriority > (Thread->BasePriority +
                                       Thread->AdjustIncrement))
                    {
                        /* Setup a priority decrement to nullify the boost  */
                        Thread->PriorityDecrement = ((SCHAR)OldPriority -
                                                    Thread->BasePriority -
                                                    Thread->AdjustIncrement);
                    }

                    /* Make sure that the priority decrement is valid */
                    ASSERT((Thread->PriorityDecrement >= 0) &&
                           (Thread->PriorityDecrement <= OldPriority));

                    /* Set this new priority */
                    Thread->Priority = (SCHAR)OldPriority;
                }
            }
        }
        else
        {
            /* It's a real-time thread, so just reset its quantum */
            Thread->Quantum = Thread->QuantumReset;
        }

        /* Make sure the priority makes sense */
        ASSERT((Thread->Priority >= 0) && (Thread->Priority <= HIGH_PRIORITY));

        /* Release the thread lock and reset the adjust reason */
        KiReleaseThreadLock(Thread);
        Thread->AdjustReason = AdjustNone;
    }

    /* Clear thread preemption status and save current values */
    Preempted = Thread->Preempted;
    OldPriority = Thread->Priority;
    Thread->Preempted = FALSE;

    /* Select a processor to run on */
#ifdef CONFIG_SMP
    LastProcessor = Thread->NextProcessor;
#endif
    Processor = KiSelectNextProcessor(Thread);
    Thread->NextProcessor = Processor;

    /* Get the PRCB and lock it */
    Prcb = KiProcessorBlock[Processor];
    KiAcquirePrcbLock(Prcb);

#ifndef CONFIG_SMP
    /* Check if we have an idle summary */
    if (KiIdleSummary)
    {
        /* Clear it and set this thread as the next one */
        KiIdleSummary = 0;
        Thread->State = Standby;
        Prcb->NextThread = Thread;

        /* Unlock the PRCB and return */
        KiReleasePrcbLock(Prcb);
        return;
    }
#endif // !CONFIG_SMP

    /* Get the next scheduled thread */
    NextThread = Prcb->NextThread;
    if (NextThread)
    {
        /* Sanity check */
        ASSERT(NextThread->State == Standby);

        /* Check if priority changed */
        if (OldPriority > NextThread->Priority)
        {
#ifdef CONFIG_SMP
            KiCountPreemption(Thread, Processor, LastProcessor);
#endif
            /* Preempt the thread */
            NextThread->Preempted = TRUE;

            /* Put this one as the next one */
            Thread->State = Standby;
            Prcb->NextThread = Thread;
#ifdef CONFIG_SMP
            InterlockedAndSetMember(&KiIdleSummary, ~Prcb->SetMember);
            Prcb->IdleSchedule = FALSE;
#endif

            /* Set it in deferred ready mode */
            NextThread->State = DeferredReady;
            NextThread->DeferredProcessor = Prcb->Number;
            KiReleasePrcbLock(Prcb);
            KiDeferredReadyThread(NextThread);
            return;
        }
    }
    else
    {
        /* Set the next thread as the current thread */
        NextThread = Prcb->CurrentThread;
        if (OldPriority > NextThread->Priority)
        {
#ifdef CONFIG_SMP
            KiCountPreemption(Thread, Processor, LastProcessor);
#endif
            /* Preempt it if it's already running */
            if (NextThread->State == Running) NextThread->Preempted = TRUE;

            /* Set the thread on standby and as the next thread */
            Thread->State = Standby;
            Prcb->NextThread = Thread;
#ifdef CONFIG_SMP
            InterlockedAndSetMember(&KiIdleSummary, ~Prcb->SetMember);
            Prcb->IdleSchedule = FALSE;
#endif

            /* Release the lock */
            KiReleasePrcbLock(Prcb);

            /* Check if we're running on another CPU */
            if (KeGetCurrentProcessorNumber() != Thread->NextProcessor)
            {
                /* We are, send an IPI */
                KiRequestSchedulerInterrupt(Thread->NextProcessor);
            }
            return;
        }
    }

    /* Sanity check */
    ASSERT((OldPriority >= 0) && (OldPriority <= HIGH_PRIORITY));

    /* Set this thread as ready */
    Thread->State = Ready;
    Thread->WaitTime = KeTickCount.LowPart;

    /* Insert this thread in the appropriate order */
    KiInsertReadyQueue(Prcb, Thread, Preempted);

    /* Sanity check */
    ASSERT(OldPriority == Thread->Priority);

    /* Release the lock */
    KiReleasePrcbLock(Prcb);
}

PKTHREAD
FASTCALL
KiSelectNextThread(IN PKPRCB Prcb)
{
    PKTHREAD Thread;

    /* Select a ready thread */
    Thread = KiSelectReadyThread(0, Prcb);
    if (!Thread)
    {
        /* Didn't find any, get the current idle thread */
        Thread = Prcb->IdleThread;

        /* Enable idle scheduling */
        InterlockedOrSetMember(&KiIdleSummary, Prcb->SetMember);
        Prcb->IdleSchedule = TRUE;

        /* FIXME: SMT support */
        //ASSERTMSG("SMP: Not yet implemented\n", FALSE);
    }

    /* Sanity checks and return the thread */
    ASSERT(Thread != NULL);
    //ASSERT((Thread->BasePriority == 0) || (Thread->Priority != 0));
    return Thread;
}

LONG_PTR
FASTCALL
KiSwapThread(IN PKTHREAD CurrentThread,
             IN PKPRCB Prcb)
{
    BOOLEAN ApcState = FALSE;
    KIRQL WaitIrql;
    LONG_PTR WaitStatus;
    PKTHREAD NextThread;
    ASSERT(KeGetCurrentIrql() >= DISPATCH_LEVEL);

    /* Acquire the PRCB lock */
    KiAcquirePrcbLock(Prcb);

    /* Get the next thread */
    NextThread = Prcb->NextThread;
    if (NextThread)
    {
        /* Already got a thread, set it up */
        Prcb->NextThread = NULL;
        Prcb->CurrentThread = NextThread;
        NextThread->State = Running;
    }
    else
    {
        /* Try to find a ready thread */
        NextThread = KiSelectReadyThread(0, Prcb);
        if (NextThread)
        {
            /* Switch to it */
            Prcb->CurrentThread = NextThread;
            NextThread->State = Running;
        }
        else
        {
            /* Set the idle summary */
            InterlockedOrSetMember(&KiIdleSummary, Prcb->SetMember);
#ifdef CONFIG_SMP
            Prcb->IdleSchedule = TRUE;
#endif

            /* Schedule the idle thread */
            NextThread = Prcb->IdleThread;
            Prcb->CurrentThread = NextThread;
            NextThread->State = Running;
            KiSchedulerCpuData[Prcb->Number].SwitchToIdle++;
        }
    }

    /* Sanity check and release the PRCB */
    ASSERT(CurrentThread != Prcb->IdleThread);
    KiReleasePrcbLock(Prcb);

    /* Save the wait IRQL */
    WaitIrql = CurrentThread->WaitIrql;

#ifdef CONFIG_SMP
    /* An unwait can make the selected thread be the current thread. */
    if (NextThread == CurrentThread)
    {
        CurrentThread->SwapBusy = FALSE;

        if (CurrentThread->ApcState.KernelApcPending &&
            !CurrentThread->SpecialApcDisable &&
            (WaitIrql == PASSIVE_LEVEL))
        {
            ApcState = TRUE;
        }
    }
    else
#endif
    {
        /* Swap contexts */
        KiSchedulerCpuData[Prcb->Number].WaitSwitches++;
        ApcState = KiSwapContext(WaitIrql, CurrentThread);
    }

    /* Get the wait status */
    WaitStatus = CurrentThread->WaitStatus;

    /* Check if we need to deliver APCs */
    if (ApcState)
    {
        /* Lower to APC_LEVEL */
        KeLowerIrql(APC_LEVEL);

        /* Deliver APCs */
        KiDeliverApc(KernelMode, NULL, NULL);
        ASSERT(WaitIrql == 0);
    }

    /* Lower IRQL back to what it was and return the wait status */
    KeLowerIrql(WaitIrql);
    return WaitStatus;
}

VOID
NTAPI
KiReadyThread(IN PKTHREAD Thread)
{
    IN PKPROCESS Process = Thread->ApcState.Process;

    /* Check if the process is paged out */
    if (Process->State != ProcessInMemory)
    {
        /* We don't page out processes in ROS */
        ASSERT(FALSE);
    }
    else if (!Thread->KernelStackResident)
    {
        /* Increase the stack count */
        ASSERT(Process->StackCount != MAXULONG_PTR);
        Process->StackCount++;

        /* Set the thread to transition */
        ASSERT(Thread->State != Transition);
        Thread->State = Transition;

        /* The stack is always resident in ROS */
        ASSERT(FALSE);
    }
    else
    {
        /* Insert the thread on the deferred ready list */
        KiInsertDeferredReadyList(Thread);
    }
}

VOID
NTAPI
KiAdjustQuantumThread(IN PKTHREAD Thread)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    PKTHREAD NextThread;

    /* Acquire thread and PRCB lock */
    KiAcquireThreadLock(Thread);
    KiAcquirePrcbLock(Prcb);

    /* Don't adjust for RT threads */
    if ((Thread->Priority < LOW_REALTIME_PRIORITY) &&
        (Thread->BasePriority < (LOW_REALTIME_PRIORITY - 2)))
    {
        /* Decrease Quantum by one and see if we've ran out */
        if (--Thread->Quantum <= 0)
        {
            /* Return quantum */
            Thread->Quantum = Thread->QuantumReset;

            /* Calculate new Priority */
            Thread->Priority = KiComputeNewPriority(Thread, 1);

            /* Check if there's no next thread scheduled */
            if (!Prcb->NextThread)
            {
                /* Select a ready thread and check if we found one */
                NextThread = KiSelectReadyThread(Thread->Priority, Prcb);
                if (NextThread)
                {
                    /* Set it on standby and switch to it */
                    NextThread->State = Standby;
                    Prcb->NextThread = NextThread;
                }
            }
            else
            {
                /* This thread can be preempted again */
                Thread->Preempted = FALSE;
            }
        }
    }

    /* Release locks */
    KiReleasePrcbLock(Prcb);
    KiReleaseThreadLock(Thread);
    KiExitDispatcher(Thread->WaitIrql);
}

VOID
FASTCALL
KiSetPriorityThread(IN PKTHREAD Thread,
                    IN KPRIORITY Priority)
{
    PKPRCB Prcb;
    ULONG Processor;
    BOOLEAN RequestInterrupt = FALSE;
    KPRIORITY OldPriority;
    PKTHREAD NewThread;
    ASSERT((Priority >= 0) && (Priority <= HIGH_PRIORITY));

    /* Check if priority changed */
    if (Thread->Priority != Priority)
    {
        /* Loop priority setting in case we need to start over */
        for (;;)
        {
            /* Choose action based on thread's state */
            if (Thread->State == Ready)
            {
                /* Make sure we're not on the ready queue */
                if (!Thread->ProcessReadyQueue)
                {
                    /* Get the PRCB for the thread and lock it */
                    Processor = Thread->NextProcessor;
                    Prcb = KiProcessorBlock[Processor];
                    KiAcquirePrcbLock(Prcb);

                    /* Make sure the thread is still ready and on this CPU */
                    if ((Thread->State == Ready) &&
                        (Thread->NextProcessor == Prcb->Number))
                    {
                        /* Sanity check */
                        ASSERT((Prcb->ReadySummary &
                                PRIORITY_MASK(Thread->Priority)));

                        /* Remove it from the current queue */
                        KiRemoveReadyQueue(Prcb, Thread);

                        /* Update priority */
                        Thread->Priority = (SCHAR)Priority;

                        /* Re-insert it at its current priority */
                        KiInsertDeferredReadyList(Thread);

                        /* Release the PRCB Lock */
                        KiReleasePrcbLock(Prcb);
                    }
                    else
                    {
                        /* Release the lock and loop again */
                        KiReleasePrcbLock(Prcb);
                        continue;
                    }
                }
                else
                {
                    /* It's already on the ready queue, just update priority */
                    Thread->Priority = (SCHAR)Priority;
                }
            }
            else if (Thread->State == Standby)
            {
                /* Get the PRCB for the thread and lock it */
                Processor = Thread->NextProcessor;
                Prcb = KiProcessorBlock[Processor];
                KiAcquirePrcbLock(Prcb);

                /* Check if we're still the next thread to run */
                if (Thread == Prcb->NextThread)
                {
                    /* Get the old priority and update ours */
                    OldPriority = Thread->Priority;
                    Thread->Priority = (SCHAR)Priority;

                    /* Check if there was a change */
                    if (Priority < OldPriority)
                    {
                        /* Find a new thread */
                        NewThread = KiSelectReadyThread(Priority + 1, Prcb);
                        if (NewThread)
                        {
                            /* Found a new one, set it on standby */
                            NewThread->State = Standby;
                            Prcb->NextThread = NewThread;

                            /* Dispatch our thread */
                            KiInsertDeferredReadyList(Thread);
                        }
                    }

                    /* Release the PRCB lock */
                    KiReleasePrcbLock(Prcb);
                }
                else
                {
                    /* Release the lock and try again */
                    KiReleasePrcbLock(Prcb);
                    continue;
                }
            }
            else if (Thread->State == Running)
            {
                /* Get the PRCB for the thread and lock it */
                Processor = Thread->NextProcessor;
                Prcb = KiProcessorBlock[Processor];
                KiAcquirePrcbLock(Prcb);

                /* Check if we're still the current thread running */
                if (Thread == Prcb->CurrentThread)
                {
                    /* Get the old priority and update ours */
                    OldPriority = Thread->Priority;
                    Thread->Priority = (SCHAR)Priority;

                    /* Check if there was a change and there's no new thread */
                    if ((Priority < OldPriority) && !(Prcb->NextThread))
                    {
                        /* Find a new thread */
                        NewThread = KiSelectReadyThread(Priority + 1, Prcb);
                        if (NewThread)
                        {
                            /* Found a new one, set it on standby */
                            NewThread->State = Standby;
                            Prcb->NextThread = NewThread;

                            /* Request an interrupt */
                            RequestInterrupt = TRUE;
                        }
                    }

                    /* Release the lock and check if we need an interrupt */
                    KiReleasePrcbLock(Prcb);
                    if (RequestInterrupt)
                    {
                        /* Check if we're running on another CPU */
                        if (KeGetCurrentProcessorNumber() != Processor)
                        {
                            /* We are, send an IPI */
                            KiRequestSchedulerInterrupt(Processor);
                        }
                    }
                }
                else
                {
                    /* Thread changed, release lock and restart */
                    KiReleasePrcbLock(Prcb);
                    continue;
                }
            }
            else if (Thread->State == DeferredReady)
            {
                /* FIXME: TODO */
                DPRINT1("Deferred state not yet supported\n");
                ASSERT(FALSE);
            }
            else
            {
                /* Any other state, just change priority */
                Thread->Priority = (SCHAR)Priority;
            }

            /* If we got here, then thread state was consistent, so bail out */
            break;
        }
    }
}

#ifdef CONFIG_SMP
static
VOID
KiUpdateEffectiveAffinityThread(
    _In_ PKTHREAD Thread)
{
    PKPRCB Prcb;
    BOOLEAN WasTransferable, IsTransferable;

    /* Acquire the thread lock */
    KiAcquireThreadLock(Thread);

    /* Get the PRCB that the thread is to be run on and lock it */
    Prcb = KiProcessorBlock[Thread->NextProcessor];
    KiAcquirePrcbLock(Prcb);

    /* Set the thread's affinity and keep ready-queue accounting coherent. */
    WasTransferable = KiIsThreadTransferable(Thread);
    Thread->Affinity = Thread->UserAffinity;
    Thread->IdealProcessor = Thread->UserIdealProcessor;
    IsTransferable = KiIsThreadTransferable(Thread);
    if ((Thread->State == Ready) && (WasTransferable != IsTransferable))
    {
        if (IsTransferable)
        {
            KiSchedulerCpuData[Prcb->Number].TransferableReadyThreadCount++;
        }
        else
        {
            ASSERT(KiSchedulerCpuData[Prcb->Number].TransferableReadyThreadCount > 0);
            KiSchedulerCpuData[Prcb->Number].TransferableReadyThreadCount--;
        }
    }

    /* Check if the affinity doesn't match with the current processor */
    if ((Prcb->SetMember & Thread->Affinity) == 0)
    {
        if (Thread->State == Running)
        {
            /* Check if there is the next thread is selected already */
            if (Prcb->NextThread == NULL)
            {
                /* It is not, select a new thread and set it on standby */
                Prcb->NextThread = KiSelectNextThread(Prcb);
                Prcb->NextThread->State = Standby;
            }

            /* Check if the thread is running on a different processor */
            if (Prcb != KeGetCurrentPrcb())
            {
                /* It is, send an IPI */
                KiRequestSchedulerInterrupt(Thread->NextProcessor);
            }
        }
        else if (Thread->State == Standby)
        {
            /* Select a new thread and set it on standby */
            Prcb->NextThread = KiSelectNextThread(Prcb);
            Prcb->NextThread->State = Standby;

            /* Insert the thread back into the ready list */
            KiInsertDeferredReadyList(Thread);
        }
        else if (Thread->State == Ready)
        {
            /* Remove it from the list */
            KiRemoveReadyQueue(Prcb, Thread);

            /* Insert the thread back into the ready list */
            KiInsertDeferredReadyList(Thread);
        }
    }

    KiReleasePrcbLock(Prcb);
    KiReleaseThreadLock(Thread);
}
#endif // CONFIG_SMP

KAFFINITY
FASTCALL
KiSetAffinityThread(IN PKTHREAD Thread,
                    IN KAFFINITY Affinity)
{
    KAFFINITY OldAffinity;

    /* Get the current affinity */
    OldAffinity = Thread->UserAffinity;

    /* Make sure that the affinity is valid */
    if (((Affinity & Thread->ApcState.Process->Affinity) != (Affinity)) ||
        (!Affinity))
    {
        /* Bugcheck the system */
        KeBugCheck(INVALID_AFFINITY_SET);
    }

    /* Update the new affinity */
    Thread->UserAffinity = Affinity;

#ifdef CONFIG_SMP
    /* Check if system affinity is not active */
    if (!Thread->SystemAffinityActive)
    {
        /* Calculate the new ideal processor from the affinity set */
        Thread->UserIdealProcessor =
            KiFindIdealProcessor(Affinity, Thread->UserIdealProcessor);

        /* Update the effective affinity */
        KiUpdateEffectiveAffinityThread(Thread);
    }
#endif

    /* Return the old affinity */
    return OldAffinity;
}

//
// This macro exists because NtYieldExecution locklessly attempts to read from
// the KPRCB's ready summary, and the usual way of going through KeGetCurrentPrcb
// would require getting fs:1C first (or gs), and then doing another dereference.
// In an attempt to minimize the amount of instructions and potential race/tear
// that could happen, Windows seems to define this as a macro that directly acceses
// the ready summary through a single fs: read by going through the KPCR's PrcbData.
//
// See http://research.microsoft.com/en-us/collaboration/global/asia-pacific/programs/trk_case4_process-thread_management.pdf (DEAD_LINK)
//
// We need this per-arch because sometimes it's Prcb and sometimes PrcbData, and
// because on x86 it's FS, and on x64 it's GS (not sure what it is on ARM/PPC).
//
#ifdef _M_IX86
#define KiGetCurrentReadySummary() __readfsdword(FIELD_OFFSET(KIPCR, PrcbData.ReadySummary))
#elif _M_AMD64
#define KiGetCurrentReadySummary() __readgsdword(FIELD_OFFSET(KIPCR, Prcb.ReadySummary))
#else
#define KiGetCurrentReadySummary() KeGetCurrentPrcb()->ReadySummary
#endif

/*
 * @implemented
 */
NTSTATUS
NTAPI
NtYieldExecution(VOID)
{
    NTSTATUS Status;
    KIRQL OldIrql;
    PKPRCB Prcb;
    PKTHREAD Thread, NextThread;

    /* NB: No instructions (other than entry code) should preceed this line */

    /* Fail if there's no ready summary */
    if (!KiGetCurrentReadySummary()) return STATUS_NO_YIELD_PERFORMED;

    /* Now get the current thread, set the status... */
    Status = STATUS_NO_YIELD_PERFORMED;
    Thread = KeGetCurrentThread();

    /* Raise IRQL to synch and get the KPRCB now */
    OldIrql = KeRaiseIrqlToSynchLevel();
    Prcb = KeGetCurrentPrcb();

    /* Now check if there's still a ready summary */
    if (Prcb->ReadySummary)
    {
        /* Acquire thread and PRCB lock */
        KiAcquireThreadLock(Thread);
        KiAcquirePrcbLock(Prcb);

        /* Find a new thread to run if none was selected */
        if (!Prcb->NextThread) Prcb->NextThread = KiSelectReadyThread(1, Prcb);

        /* Make sure we still have a next thread to schedule */
        NextThread = Prcb->NextThread;
        if (NextThread)
        {
            /* Reset quantum and recalculate priority */
            Thread->Quantum = Thread->QuantumReset;
            Thread->Priority = KiComputeNewPriority(Thread, 1);

            /* Release the thread lock */
            KiReleaseThreadLock(Thread);

            /* Set context swap busy */
            KiSetThreadSwapBusy(Thread);

            /* Set the new thread as running */
            Prcb->NextThread = NULL;
            Prcb->CurrentThread = NextThread;
            NextThread->State = Running;

            /* Setup a yield wait and queue the thread */
            Thread->WaitReason = WrYieldExecution;
            KxQueueReadyThread(Thread, Prcb);

            /* Make it wait at APC_LEVEL */
            Thread->WaitIrql = APC_LEVEL;

            /* Sanity check */
            ASSERT(OldIrql <= DISPATCH_LEVEL);

            /* Swap to new thread */
            KiSchedulerCpuData[Prcb->Number].YieldSwitches++;
            KiSwapContext(APC_LEVEL, Thread);
            Status = STATUS_SUCCESS;
        }
        else
        {
            /* Release the PRCB and thread lock */
            KiReleasePrcbLock(Prcb);
            KiReleaseThreadLock(Thread);
        }
    }

    /* Lower IRQL and return */
    KeLowerIrql(OldIrql);
    return Status;
}
