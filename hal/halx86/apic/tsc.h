
#ifndef _TSC_H_
#define _TSC_H_

#define NUM_SAMPLES 4
#define MSR_RDTSC 0x10
#define RTC_MODE 6 /* Mode 6 is 1024 Hz */
#define SAMPLE_FREQUENCY ((32768 << 1) >> RTC_MODE)

#ifndef __ASM__

void __cdecl TscCalibrationISR(void);
extern LARGE_INTEGER HalpCpuClockFrequency;
extern ULONG ApicCalibrationArray[NUM_SAMPLES];
VOID NTAPI HalpInitializeTsc(void);

#ifdef _M_AMD64
#define KiGetIdtEntry(Pcr, Vector) &((Pcr)->IdtBase[Vector])
#else
#define KiGetIdtEntry(Pcr, Vector) &((Pcr)->IDT[Vector])
#endif

#endif

#endif /* !_TSC_H_ */
