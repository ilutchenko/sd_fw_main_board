/*
 * If a serious error occurs, one of the fault
 * exception vectors in this file will be called.
 *
 * This file attempts to aid the unfortunate debugger
 * to blame someone for the crashing code
 *
 *  Created on: 12.06.2013
 *      Author: uli
 *
 * Released under the CC0 1.0 Universal (public domain)
 */
#include <stdint.h>
#include <ch.h>
#include <string.h>

#include <sys/types.h>
#include <stdlib.h>

// Can be uncommented to see backtrace.
#define DEBUG_BACKTRACE

/**
 * Executes the BKPT instruction that causes the debugger to stop.
 * If no debugger is attached, this will be ignored
 */
#define bkpt() __asm volatile("BKPT #0\n")

void NMI_Handler(void) {
    //TODO
    while(1);
}

//See http://infocenter.arm.com/help/topic/com.arm.doc.dui0552a/BABBGBEC.html
typedef enum  {
    Reset = 1,
    NMI = 2,
    HardFault = 3,
    MemManage = 4,
    BusFault = 5,
    UsageFault = 6,
} FaultType;

/* On hard fault, copy FAULT_PSP to the sp reg so gdb can give a trace */
void **FAULT_PSP;
register void *stack_pointer asm("sp");

void HardFault_Handler(void) {
#ifdef DEBUG_BACKTRACE
    asm("mrs %0, psp" : "=r"(FAULT_PSP) : :);
    stack_pointer = FAULT_PSP;
    // Here will be good to assert Error LED, like
    // GPIOA->ODR |= 1;
    while(1);
#else
    //Copy to local variables (not pointers) to allow GDB "i loc" to directly show the info
    //Get thread context. Contains main registers including PC and LR
    struct port_extctx ctx;

    memcpy(&ctx, (void*)__get_PSP(), sizeof(struct port_extctx));
    //Interrupt status register: Which interrupt have we encountered, e.g. HardFault?
    FaultType hf_faultType = (FaultType)__get_IPSR();
    //(void)hf_faultType;
    //For HardFault/BusFault this is the address that was accessed causing the error
    uint32_t faultAddress = SCB->BFAR;
    //(void)faultAddress;
    //Flags about hardfault / busfault
    //See http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0552a/Cihdjcfc.html for reference
    bool isFaultPrecise = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 1) ? true : false);
    bool isFaultImprecise = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 2) ? true : false);
    bool isFaultOnUnstacking = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 3) ? true : false);
    bool isFaultOnStacking = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 4) ? true : false);
    bool isFaultAddressValid = ((SCB->CFSR >> SCB_CFSR_BUSFAULTSR_Pos) & (1 << 7) ? true : false);

    // This is to prevent variables to be optimized out
    if ((ctx.r0 != ctx.r1) && (ctx.xpsr != ctx.pc) && (faultAddress != 2))
        asm("nop");
    if (hf_faultType != 2)
        asm("nop");
    if (isFaultPrecise)
        asm("nop");
    if (isFaultImprecise)
        asm("nop");
    if (isFaultOnUnstacking)
        asm("nop");
    if (isFaultOnStacking)
        asm("nop");
    if (isFaultAddressValid)
        asm("nop");
    if ( hf_faultType)
        asm("nop");
    //Cause debugger to stop. Ignored if no debugger is attached
   // bkpt();
    NVIC_SystemReset();
#endif
}

void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));

void UsageFault_Handler(void) {
#ifdef DEBUG_BACKTRACE
    asm("mrs %0, psp" : "=r"(FAULT_PSP) : :);
    stack_pointer = FAULT_PSP;
    while(1);
#else
    //Copy to local variables (not pointers) to allow GDB "i loc" to directly show the info
    //Get thread context. Contains main registers including PC and LR
    struct port_extctx ctx;
    memcpy(&ctx, (void*)__get_PSP(), sizeof(struct port_extctx));
    (void)ctx;
    //Interrupt status register: Which interrupt have we encountered, e.g. HardFault?
    FaultType faultType = (FaultType)__get_IPSR();
    (void)faultType;
    //Flags about hardfault / busfault
    //See http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0552a/Cihdjcfc.html for reference
    bool isUndefinedInstructionFault = ((SCB->CFSR >> SCB_CFSR_USGFAULTSR_Pos) & (1 << 0) ? true : false);
    bool isEPSRUsageFault = ((SCB->CFSR >> SCB_CFSR_USGFAULTSR_Pos) & (1 << 1) ? true : false);
    bool isInvalidPCFault = ((SCB->CFSR >> SCB_CFSR_USGFAULTSR_Pos) & (1 << 2) ? true : false);
    bool isNoCoprocessorFault = ((SCB->CFSR >> SCB_CFSR_USGFAULTSR_Pos) & (1 << 3) ? true : false);
    bool isUnalignedAccessFault = ((SCB->CFSR >> SCB_CFSR_USGFAULTSR_Pos) & (1 << 8) ? true : false);
    bool isDivideByZeroFault = ((SCB->CFSR >> SCB_CFSR_USGFAULTSR_Pos) & (1 << 9) ? true : false);
    (void)isUndefinedInstructionFault;
    (void)isEPSRUsageFault;
    (void)isInvalidPCFault;
    (void)isNoCoprocessorFault;
    (void)isUnalignedAccessFault;
    (void)isDivideByZeroFault;
    //bkpt();
    NVIC_SystemReset();
#endif
}

void MemManage_Handler(void) {
#ifdef DEBUG_BACKTRACE
    asm("mrs %0, psp" : "=r"(FAULT_PSP) : :);
    stack_pointer = FAULT_PSP;
    while(1);
#else
   //Copy to local variables (not pointers) to allow GDB "i loc" to directly show the info
    //Get thread context. Contains main registers including PC and LR
    struct port_extctx ctx;
    memcpy(&ctx, (void*)__get_PSP(), sizeof(struct port_extctx));
    (void)ctx;
    //Interrupt status register: Which interrupabortt have we encountered, e.g. HardFault?
    FaultType faultType = (FaultType)__get_IPSR();
    (void)faultType;
    //For HardFault/BusFault this is the address that was accessed causing the error
    uint32_t faultAddress = SCB->MMFAR;
    (void)faultAddress;
    //Flags about hardfault / busfault
    //See http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0552a/Cihdjcfc.html for reference
    bool isInstructionAccessViolation = ((SCB->CFSR >> SCB_CFSR_MEMFAULTSR_Pos) & (1 << 0) ? true : false);
    bool isDataAccessViolation = ((SCB->CFSR >> SCB_CFSR_MEMFAULTSR_Pos) & (1 << 1) ? true : false);
    bool isExceptionUnstackingFault = ((SCB->CFSR >> SCB_CFSR_MEMFAULTSR_Pos) & (1 << 3) ? true : false);
    bool isExceptionStackingFault = ((SCB->CFSR >> SCB_CFSR_MEMFAULTSR_Pos) & (1 << 4) ? true : false);
    bool isFaultAddressValid = ((SCB->CFSR >> SCB_CFSR_MEMFAULTSR_Pos) & (1 << 7) ? true : false);
    (void)isInstructionAccessViolation;
    (void)isDataAccessViolation;
    (void)isExceptionUnstackingFault;
    (void)isExceptionStackingFault;
    (void)isFaultAddressValid;
   // bkpt();
    NVIC_SystemReset();
#endif
}

pid_t _getpid()
{
	return 0;
}

void _exit( int status )
{
	(void)status;
	while( 1 );
}

int _kill( int pid, int sig )
{
	(void)pid; (void)sig;
	return -1;
}

int _open_r(void *reent, const char *file, int flags, int mode)
{
	(void)reent; (void)file; (void)flags; (void)mode;
	return -1;
}

void *__dso_handle = NULL;
