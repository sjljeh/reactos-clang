/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            ntoskrnl/ke/i386/context.c
 * PURPOSE:         Context Switching Related Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
KiSwapProcess(IN PKPROCESS NewProcess,
              IN PKPROCESS OldProcess)
{
    PKIPCR Pcr = (PKIPCR)KeGetPcr();
#ifdef CONFIG_SMP
    LONG SetMember;
#endif

    if (NewProcess == OldProcess)
        return;

#ifdef CONFIG_SMP
    /* Publish the new address-space residency before loading its CR3. */
    SetMember = (LONG)Pcr->SetMember;
    InterlockedOr((PLONG)&NewProcess->ActiveProcessors, SetMember);
#endif

    /* Check for new LDT */
    if (NewProcess->LdtDescriptor.LimitLow != OldProcess->LdtDescriptor.LimitLow)
    {
        if (NewProcess->LdtDescriptor.LimitLow)
        {
            KeSetGdtSelector(KGDT_LDT,
                             ((PULONG)&NewProcess->LdtDescriptor)[0],
                             ((PULONG)&NewProcess->LdtDescriptor)[1]);
            Ke386SetLocalDescriptorTable(KGDT_LDT);
        }
        else
        {
            Ke386SetLocalDescriptorTable(0);
        }
    }

    /* Update CR3 */
    __writecr3(NewProcess->DirectoryTableBase[0]);

#ifdef CONFIG_SMP
    /* The old process can no longer have user translations on this CPU. */
    InterlockedAnd((PLONG)&OldProcess->ActiveProcessors, ~SetMember);
#endif

    /* Clear GS */
    Ke386SetGs(0);

    /* Update IOPM offset */
    Pcr->TSS->IoMapBase = NewProcess->IopmOffset;
}

