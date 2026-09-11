/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Architecture specific source file to hold multiprocessor functions
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Victor Perevertkin <victor.perevertkin@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY Gdt[128];
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
} APINFO, *PAPINFO;

typedef struct _AP_SETUP_STACK
{
    PVOID ReturnAddr;
    PVOID KxLoaderBlock;
} AP_SETUP_STACK, *PAP_SETUP_STACK; // Note: expected layout only for 32-bit x86

/* FUNCTIONS *****************************************************************/

static
CODE_SEG("INIT")
VOID
KiInitializeExceptionTss(
    _Out_ PKTSS Tss,
    _Inout_ PKGDTENTRY TssEntry,
    _In_ ULONG_PTR ExceptionStack,
    _In_ PVOID ExceptionHandler)
{
    KiInitializeTSS(Tss);
    Tss->CR3 = __readcr3();
    Tss->Esp0 = ExceptionStack;
    Tss->Esp = ExceptionStack;
    Tss->Eip = PtrToUlong(ExceptionHandler);
    Tss->Cs = KGDT_R0_CODE;
    Tss->Fs = KGDT_R0_PCR;
    Tss->Ss = KGDT_R0_DATA;
    Tss->Es = KGDT_R3_DATA | RPL_MASK;
    Tss->Ds = KGDT_R3_DATA | RPL_MASK;

    KiSetGdtDescriptorBase(TssEntry, (ULONG_PTR)Tss);
    TssEntry->LimitLow = KTSS_IO_MAPS;
    TssEntry->HighWord.Bits.LimitHi = 0;
    TssEntry->HighWord.Bits.Type = I386_TSS;
    TssEntry->HighWord.Bits.Pres = 1;
    TssEntry->HighWord.Bits.Dpl = 0;
}

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack = NULL, DPCStack = NULL;
    PAPINFO APInfo = NULL;
    PKGDTENTRY TssEntry;
    ULONG_PTR ExceptionStack;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;

    /* NOTE: NT6+ HAL exports HalEnumerateProcessors() and
     * HalQueryMaximumProcessorCount() that help determining
     * the number of detected processors on the system. */
    MaximumProcessors = KeMaximumProcessors;

    /* Limit the number of processors we can start at run-time */
    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    /* Limit also the number of processors we can start during boot-time */
    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    // TODO: Support processor nodes

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DPCStack = NULL;

        // Allocate structures for a new CPU.
        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DPCStack = MmCreateKernelStack(FALSE, 0);
        if (!DPCStack)
            break;

        // Initalize a new PCR for the specific AP
        KiInitializePcr(ProcessorCount,
                        &APInfo->Pcr,
                        &APInfo->Idt[0],
                        &APInfo->Gdt[0],
                        &APInfo->Tss,
                        (PKTHREAD)&APInfo->Thread,
                        DPCStack);

        // Prepare descriptor tables
        KDESCRIPTOR bspGdt, bspIdt;
        __sgdt(&bspGdt.Limit);
        __sidt(&bspIdt.Limit);
        RtlCopyMemory(&APInfo->Gdt, (PVOID)bspGdt.Base, bspGdt.Limit + 1);
        RtlCopyMemory(&APInfo->Idt, (PVOID)bspIdt.Base, bspIdt.Limit + 1);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_R0_PCR),
                               (ULONG_PTR)&APInfo->Pcr);

        /* Initialize the normal TSS before the trampoline loads TR. */
        TssEntry = KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS);
        KiSetGdtDescriptorBase(TssEntry, (ULONG_PTR)&APInfo->Tss);
        TssEntry->HighWord.Bits.Type = I386_TSS;
        TssEntry->HighWord.Bits.Pres = 1;
        TssEntry->HighWord.Bits.Dpl = 0;
        KiInitializeTSS2(&APInfo->Tss, TssEntry);
        KiInitializeTSS(&APInfo->Tss);

        /* Give the task-gate handlers valid per-processor TSS state. */
        ExceptionStack = (ULONG_PTR)&APInfo->NMIStackData[
            RTL_NUMBER_OF(APInfo->NMIStackData)];
        KiInitializeExceptionTss(&APInfo->TssDoubleFault,
                                 KiGetGdtEntry(&APInfo->Gdt, KGDT_DF_TSS),
                                 ExceptionStack,
                                 KiTrap08);
        KiInitializeExceptionTss(&APInfo->TssNMI,
                                 KiGetGdtEntry(&APInfo->Gdt, KGDT_NMI_TSS),
                                 ExceptionStack,
                                 KiTrap02);

        // Fill the processor state
        PKPROCESSOR_STATE ProcessorState = &APInfo->Pcr.Prcb->ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        ProcessorState->ContextFrame.SegCs = KGDT_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegEs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegSs = KGDT_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT_R0_PCR;

        ProcessorState->SpecialRegisters.Gdtr.Base = (ULONG_PTR)APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = (ULONG_PTR)APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = sizeof(APInfo->Idt) - 1;

        ProcessorState->SpecialRegisters.Tr = KGDT_TSS;

        ProcessorState->ContextFrame.Esp = (ULONG_PTR)KernelStack;
        ProcessorState->ContextFrame.Eip = (ULONG_PTR)KiSystemStartup;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        ProcessorState->ContextFrame.Esp = (ULONG)((ULONG_PTR)ProcessorState->ContextFrame.Esp - sizeof(AP_SETUP_STACK));
        PAP_SETUP_STACK ApStack = (PAP_SETUP_STACK)ProcessorState->ContextFrame.Esp;
        ApStack->KxLoaderBlock = KeLoaderBlock;
        ApStack->ReturnAddr = NULL;

        // Update the LOADER_PARAMETER_BLOCK structure for the new processor
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;

        // Start the CPU
        DPRINT("Attempting to Start a CPU with number: %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            break;
        }

        // And wait for it to start
        while (KeLoaderBlock->Prcb != 0)
        {
            //TODO: Add a time out so we don't wait forever
            KeMemoryBarrier();
            YieldProcessor();
        }

        /* The new processor now owns its PCR, idle thread and stacks. */
        APInfo = NULL;
        KernelStack = NULL;
        DPCStack = NULL;
    }

    /* Clean up only an unsuccessful final startup attempt. */
    if (APInfo)
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
    if (KernelStack)
        MmDeleteKernelStack(KernelStack, FALSE);
    if (DPCStack)
        MmDeleteKernelStack(DPCStack, FALSE);

    DPRINT1("KeStartAllProcessors: Successful AP startup count is %lu\n", ProcessorCount - 1);
}
