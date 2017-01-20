#include "InterruptUtils.h"

#include "ArchSerialInfo.h"
#include "BDManager.h"
#include "ArchMemory.h"
#include "ArchThreads.h"
#include "ArchCommon.h"
#include "kprintf.h"
#include "Scheduler.h"

#include "SerialManager.h"
#include "KeyboardManager.h"
#include "ArchInterrupts.h"
#include "backtrace.h"

#include "Thread.h"
#include "Loader.h"
#include "Syscall.h"
#include "paging-definitions.h"
#include "offsets.h"
#include "PageFaultHandler.h"

#define LO_WORD(x) (((uint32)(x)) & 0x0000FFFF)
#define HI_WORD(x) ((((uint32)(x)) >> 16) & 0x0000FFFF)

#define GATE_SIZE_16_BIT     0 // use 16- bit push
#define GATE_SIZE_32_BIT     1 // use 32- bit push

#define TYPE_TRAP_GATE       7 // trap gate, i.e. IF flag is *not* cleared
#define TYPE_INTERRUPT_GATE  6 // interrupt gate, i.e. IF flag *is* cleared

#define DPL_KERNEL_SPACE     0 // kernelspace's protection level
#define DPL_USER_SPACE       3 // userspaces's protection level

#define SYSCALL_INTERRUPT 0x80 // number of syscall interrupt


// --- Pagefault error flags.
//     PF because/in/caused by/...

#define FLAG_PF_PRESENT     0x01 // =0: pt/page not present
                                 // =1: of protection violation

#define FLAG_PF_RDWR        0x02 // =0: read access
                                 // =1: write access

#define FLAG_PF_USER        0x04 // =0: supervisormode (CPL < 3)
                                 // =1: usermode (CPL == 3)

#define FLAG_PF_RSVD        0x08 // =0: not a reserved bit
                                 // =1: a reserved bit

#define FLAG_PF_INSTR_FETCH 0x10 // =0: not an instruction fetch
                                 // =1: an instruction fetch (need PAE for that)

struct GateDesc
{
  uint16 offset_low;       // low word of handler entry point's address
  uint16 segment_selector; // (code) segment the handler resides in
  uint8 reserved  : 5;     // reserved. set to zero
  uint8 zeros     : 3;     // set to zero
  uint8 type      : 3;     // set to TYPE_TRAP_GATE or TYPE_INTERRUPT_GATE
  uint8 gate_size : 1;     // set to GATE_SIZE_16_BIT or GATE_SIZE_32_BIT
  uint8 unused    : 1;     // unsued - set to zero
  uint8 dpl       : 2;     // descriptor protection level
  uint8 present   : 1;     // present- flag - set to 1
  uint16 offset_high;      // high word of handler entry point's address
}__attribute__((__packed__));

extern "C" void arch_dummyHandler();

void InterruptUtils::initialise()
{
  uint32 num_handlers = 0;
  for (uint32 i = 0; handlers[i].offset != 0; ++i)
    num_handlers = handlers[i].number;
  ++num_handlers;
  // allocate some memory for our handlers
  GateDesc *interrupt_gates = new GateDesc[num_handlers];

  uint32 j = 0;
  for (uint32 i = 0; i < num_handlers; ++i)
  {
    while (handlers[j].number < i && handlers[j].offset != 0)
      ++j;
    interrupt_gates[i].offset_low = LO_WORD((handlers[j].number == i && handlers[j].offset != 0) ? handlers[j].offset : arch_dummyHandler);
    interrupt_gates[i].offset_high = HI_WORD((handlers[j].number == i && handlers[j].offset != 0) ? handlers[j].offset : arch_dummyHandler);
    interrupt_gates[i].gate_size = GATE_SIZE_32_BIT;
    interrupt_gates[i].present = 1;
    interrupt_gates[i].reserved = 0;
    interrupt_gates[i].segment_selector = KERNEL_CS;
    interrupt_gates[i].type = TYPE_INTERRUPT_GATE;
    interrupt_gates[i].unused = 0;
    interrupt_gates[i].zeros = 0;
    interrupt_gates[i].dpl = ((i == SYSCALL_INTERRUPT && handlers[j].number == i) ? DPL_USER_SPACE : DPL_KERNEL_SPACE);
  }

  IDTR idtr;

  idtr.base = (uint32) interrupt_gates;
  idtr.limit = sizeof(GateDesc) * num_handlers - 1;
  lidt(&idtr);
}

void InterruptUtils::lidt(IDTR *idtr)
{
  asm volatile("lidt (%0) ": :"q" (idtr));
}

#define ERROR_HANDLER(x,msg) extern "C" void arch_errorHandler_##x(); \
  extern "C" void errorHandler_##x () \
  {\
    currentThread->switch_to_userspace_ = false;\
    currentThreadRegisters = currentThread->kernel_registers_;\
    ArchInterrupts::enableInterrupts();\
    kprintfd("\nCPU Fault " #msg "\n\n%s", intel_manual);\
    kprintf("\nCPU Fault " #msg "\n\n%s", intel_manual);\
    currentThread->kill();\
  }

extern "C" void arch_dummyHandler();
extern "C" void arch_contextSwitch();
extern "C" void dummyHandler()
{
  uint32 saved_switch_to_userspace = currentThread->switch_to_userspace_;
  currentThread->switch_to_userspace_ = 0;
  currentThreadRegisters = currentThread->kernel_registers_;
  ArchInterrupts::enableInterrupts();
  kprintfd("DUMMY_HANDLER: Spurious INT\n");
  ArchInterrupts::disableInterrupts();
  currentThread->switch_to_userspace_ = saved_switch_to_userspace;
  if (currentThread->switch_to_userspace_)
  {
    currentThreadRegisters = currentThread->user_registers_;
    arch_contextSwitch();
  }
}

extern "C" void arch_irqHandler_0();
extern "C" void arch_contextSwitch();
extern "C" void irqHandler_0()
{
  ArchCommon::drawHeartBeat();

  Scheduler::instance()->incTicks();

  Scheduler::instance()->schedule();
  // kprintfd("irq0: Going to leave irq Handler 0\n");
  ArchInterrupts::EndOfInterrupt(0);
  arch_contextSwitch();
}

extern "C" void arch_irqHandler_65();
extern "C" void irqHandler_65()
{
  Scheduler::instance()->schedule();
  // kprintfd("irq65: Going to leave int Handler 65 to user\n");
  arch_contextSwitch();
}

extern "C" void arch_pageFaultHandler();
extern "C" void pageFaultHandler(uint32 address, uint32 error)
{
  PageFaultHandler::enterPageFault(address, error & FLAG_PF_USER,
                                   error & FLAG_PF_PRESENT,
                                   error & FLAG_PF_RDWR,
                                   error & FLAG_PF_INSTR_FETCH);
  if (currentThread->switch_to_userspace_)
    arch_contextSwitch();
  else
    asm volatile ("movl %%cr3, %%eax; movl %%eax, %%cr3;" ::: "%eax");
}

extern "C" void arch_irqHandler_1();
extern "C" void irqHandler_1()
{
  KeyboardManager::instance()->serviceIRQ();
  ArchInterrupts::EndOfInterrupt(1);
}

extern "C" void arch_irqHandler_3();
extern "C" void irqHandler_3()
{
  kprintfd("IRQ 3 called\n");
  SerialManager::getInstance()->service_irq(3);
  ArchInterrupts::EndOfInterrupt(3);
  kprintfd("IRQ 3 ended\n");
}

extern "C" void arch_irqHandler_4();
extern "C" void irqHandler_4()
{
  kprintfd("IRQ 4 called\n");
  SerialManager::getInstance()->service_irq(4);
  ArchInterrupts::EndOfInterrupt(4);
  kprintfd("IRQ 4 ended\n");
}

extern "C" void arch_irqHandler_6();
extern "C" void irqHandler_6()
{
  kprintfd("IRQ 6 called\n");
  kprintfd("IRQ 6 ended\n");
}

extern "C" void arch_irqHandler_9();
extern "C" void irqHandler_9()
{
  kprintfd("IRQ 9 called\n");
  BDManager::getInstance()->serviceIRQ(9);
  ArchInterrupts::EndOfInterrupt(9);
}

extern "C" void arch_irqHandler_11();
extern "C" void irqHandler_11()
{
  kprintfd("IRQ 11 called\n");
  BDManager::getInstance()->serviceIRQ(11);
  ArchInterrupts::EndOfInterrupt(11);
}

extern "C" void arch_irqHandler_14();
extern "C" void irqHandler_14()
{
  //kprintfd( "IRQ 14 called\n" );
  BDManager::getInstance()->serviceIRQ(14);
  ArchInterrupts::EndOfInterrupt(14);
}

extern "C" void arch_irqHandler_15();
extern "C" void irqHandler_15()
{
  //kprintfd( "IRQ 15 called\n" );
  BDManager::getInstance()->serviceIRQ(15);
  ArchInterrupts::EndOfInterrupt(15);
}

extern "C" void arch_syscallHandler();
extern "C" void syscallHandler()
{
  currentThread->switch_to_userspace_ = 0;
  currentThreadRegisters = currentThread->kernel_registers_;
  ArchInterrupts::enableInterrupts();

  currentThread->user_registers_->eax = Syscall::syscallException(currentThread->user_registers_->eax,
                                                                         currentThread->user_registers_->ebx,
                                                                         currentThread->user_registers_->ecx,
                                                                         currentThread->user_registers_->edx,
                                                                         currentThread->user_registers_->esi,
                                                                         currentThread->user_registers_->edi);

  ArchInterrupts::disableInterrupts();
  currentThread->switch_to_userspace_ = 1;
  currentThreadRegisters = currentThread->user_registers_;
  //ArchThreads::printThreadRegisters(currentThread,false);
  arch_contextSwitch();
}

#include "ErrorHandlers.h" // error handler definitions and irq forwarding definitions

