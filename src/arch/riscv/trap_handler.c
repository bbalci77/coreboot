/*
 * Early initialization code for riscv
 *
 * Copyright 2015 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of
 * the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 */

#include <arch/exception.h>
#include <arch/sbi.h>
#include <console/console.h>
#include <mcall.h>
#include <string.h>
#include <vm.h>
#include <commonlib/configstring.h>

static uint64_t *time;
static uint64_t *timecmp;

void handle_supervisor_call(trapframe *tf) {
	uintptr_t call = tf->gpr[17]; /* a7 */
	uintptr_t arg0 = tf->gpr[10]; /* a0 */
	uintptr_t arg1 = tf->gpr[11]; /* a1 */
	uintptr_t returnValue;
	switch(call) {
		case SBI_ECALL_HART_ID:
			printk(BIOS_DEBUG, "Getting hart id...\n");
			returnValue = read_csr(0xf14);//mhartid);
			break;
		case SBI_ECALL_NUM_HARTS:
			/* TODO: parse the hardware-supplied config string and
			   return the correct value */
			returnValue = 1;
			break;
		case SBI_ECALL_CONSOLE_PUT:
			returnValue = mcall_console_putchar(arg0);
			break;
		case SBI_ECALL_SEND_DEVICE_REQUEST:
			printk(BIOS_DEBUG, "Sending device request...\n");
			returnValue = mcall_dev_req((sbi_device_message*) arg0);
			break;
		case SBI_ECALL_RECEIVE_DEVICE_RESPONSE:
			printk(BIOS_DEBUG, "Getting device response...\n");
			returnValue = mcall_dev_resp();
			break;
		case SBI_ECALL_SEND_IPI:
			printk(BIOS_DEBUG, "Sending IPI...\n");
			returnValue = mcall_send_ipi(arg0);
			break;
		case SBI_ECALL_CLEAR_IPI:
			printk(BIOS_DEBUG, "Clearing IPI...\n");
			returnValue = mcall_clear_ipi();
			break;
		case SBI_ECALL_SHUTDOWN:
			printk(BIOS_DEBUG, "Shutting down...\n");
			returnValue = mcall_shutdown();
			break;
		case SBI_ECALL_SET_TIMER:
			printk(BIOS_DEBUG,
			       "Setting timer to %p (current time is %p)...\n",
			       (void *)arg0, (void *)rdtime());
			returnValue = mcall_set_timer(arg0);
			break;
		case SBI_ECALL_QUERY_MEMORY:
			printk(BIOS_DEBUG, "Querying memory, CPU #%lld...\n", arg0);
			returnValue = mcall_query_memory(arg0, (memory_block_info*) arg1);
			break;
		default:
			printk(BIOS_DEBUG, "ERROR! Unrecognized SBI call\n");
			returnValue = 0;
			break; // note: system call we do not know how to handle
	}
	tf->gpr[10] = returnValue;
	write_csr(mepc, read_csr(mepc) + 4);
	asm volatile("j supervisor_call_return");
}

static const char *const exception_names[] = {
	"Instruction address misaligned",
	"Instruction access fault",
	"Illegal instruction",
	"Breakpoint",
	"Load address misaligned",
	"Load access fault",
	"Store address misaligned",
	"Store access fault",
	"Environment call from U-mode",
	"Environment call from S-mode",
	"Environment call from H-mode",
	"Environment call from M-mode"
};

static const char *mstatus_to_previous_mode(uintptr_t ms)
{
	switch (ms & MSTATUS_MPP) {
		case 0x00000000: return "user";
		case 0x00000800: return "supervisor";
		case 0x00001000: return "hypervisor";
		case 0x00001800: return "machine";
	}

	return "unknown";
}

static void print_trap_information(const trapframe *tf)
{
	const char *previous_mode;
	bool mprv = !!(tf->status & MSTATUS_MPRV);

	/* Leave some space around the trap message */
	printk(BIOS_DEBUG, "\n");

	if (tf->cause < ARRAY_SIZE(exception_names))
		printk(BIOS_DEBUG, "Exception:          %s\n",
				exception_names[tf->cause]);
	else
		printk(BIOS_DEBUG, "Trap:               Unknown cause %p\n",
				(void *)tf->cause);

	previous_mode = mstatus_to_previous_mode(read_csr(mstatus));
	printk(BIOS_DEBUG, "Previous mode:      %s%s\n",
			previous_mode, mprv? " (MPRV)":"");
	printk(BIOS_DEBUG, "Bad instruction pc: %p\n", (void *)tf->epc);
	printk(BIOS_DEBUG, "Bad address:        %p\n", (void *)tf->badvaddr);
	printk(BIOS_DEBUG, "Stored ra:          %p\n", (void*) tf->gpr[1]);
	printk(BIOS_DEBUG, "Stored sp:          %p\n", (void*) tf->gpr[2]);
}

static void gettimer(void)
{
	query_result res;
	const char *config;

	config = configstring();
	query_rtc(config, (uintptr_t *)&time);
	if (!time)
		die("Got timer interrupt but found no timer.");
	res = query_config_string(config, "core{0{0{timecmp");
	timecmp = (void *)get_uint(res);
	if (!timecmp)
		die("Got a timer interrupt but found no timecmp.");
}

static void interrupt_handler(trapframe *tf)
{
	uint64_t cause = tf->cause & ~0x8000000000000000ULL;
	uint32_t ssip, ssie;

	switch (cause) {
	case IRQ_M_TIMER:
		// The only way to reset the timer interrupt is to
		// write mtimecmp. But we also have to ensure the
		// comparison fails, for a long time, to let
		// supervisor interrupt handler compute a new value
		// and set it. Finally, it fires if mtimecmp is <=
		// mtime, not =, so setting mtimecmp to 0 won't work
		// to clear the interrupt and disable a new one. We
		// have to set the mtimecmp far into the future.
		// Akward!
		//
		// Further, maybe the platform doesn't have the
		// hardware or the payload never uses it. We hold off
		// querying some things until we are sure we need
		// them. What to do if we can not find them? There are
		// no good options.

		// This hart may have disabled timer interrupts.  If
		// so, just return. Kernels should only enable timer
		// interrupts on one hart, and that should be hart 0
		// at present, as we only search for
		// "core{0{0{timecmp" above.
		ssie = read_csr(sie);
		if (!(ssie & SIE_STIE))
			break;

		if (!timecmp)
			gettimer();
		*timecmp = (uint64_t) -1;
		ssip = read_csr(sip);
		ssip |= SIP_STIP;
		write_csr(sip, ssip);
		break;
	default:
		printk(BIOS_EMERG, "======================================\n");
		printk(BIOS_EMERG, "Coreboot: Unknown machine interrupt: 0x%llx\n",
		       cause);
		printk(BIOS_EMERG, "======================================\n");
		print_trap_information(tf);
		break;
	}
}
void trap_handler(trapframe *tf)
{
	write_csr(mscratch, tf);
	if (tf->cause & 0x8000000000000000ULL) {
		interrupt_handler(tf);
		return;
	}

	switch(tf->cause) {
		case CAUSE_MISALIGNED_FETCH:
		case CAUSE_FAULT_FETCH:
		case CAUSE_ILLEGAL_INSTRUCTION:
		case CAUSE_BREAKPOINT:
		case CAUSE_FAULT_LOAD:
		case CAUSE_FAULT_STORE:
		case CAUSE_USER_ECALL:
		case CAUSE_HYPERVISOR_ECALL:
		case CAUSE_MACHINE_ECALL:
			print_trap_information(tf);
			break;
		case CAUSE_MISALIGNED_LOAD:
			print_trap_information(tf);
			handle_misaligned_load(tf);
			break;
		case CAUSE_MISALIGNED_STORE:
			print_trap_information(tf);
			handle_misaligned_store(tf);
			break;
		case CAUSE_SUPERVISOR_ECALL:
			/* Don't print so we make console putchar calls look
			   the way they should */
			handle_supervisor_call(tf);
			break;
		default:
			printk(BIOS_EMERG, "================================\n");
			printk(BIOS_EMERG, "Coreboot: can not handle a trap:\n");
			printk(BIOS_EMERG, "================================\n");
			print_trap_information(tf);
			break;
	}

	die("Can't recover from trap. Halting.\n");
}

static uint32_t fetch_instruction(uintptr_t vaddr) {
	printk(BIOS_SPEW, "fetching instruction at 0x%016zx\n", (size_t)vaddr);
	return mprv_read_u32((uint32_t *) vaddr);
}

void handle_misaligned_load(trapframe *tf) {
	printk(BIOS_DEBUG, "Trapframe ptr:      %p\n", tf);
	uintptr_t faultingInstructionAddr = tf->epc;
	insn_t faultingInstruction = fetch_instruction(faultingInstructionAddr);
	printk(BIOS_DEBUG, "Faulting instruction: 0x%x\n", faultingInstruction);
	insn_t widthMask = 0x7000;
	insn_t memWidth = (faultingInstruction & widthMask) >> 12;
	insn_t destMask = 0xF80;
	insn_t destRegister = (faultingInstruction & destMask) >> 7;
	printk(BIOS_DEBUG, "Width: 0x%x\n", memWidth);
	if (memWidth == 3) {
		// load double, handle the issue
		void* badAddress = (void*) tf->badvaddr;
		uint64_t value = 0;
		for (int i = 0; i < 8; i++) {
			value <<= 8;
			value += mprv_read_u8(badAddress+i);
		}
		tf->gpr[destRegister] = value;
	} else {
		// panic, this should not have happened
		die("Code should not reach this path, misaligned on a non-64 bit store/load\n");
	}

	// return to where we came from
	write_csr(mepc, read_csr(mepc) + 4);
	asm volatile("j machine_call_return");
}

void handle_misaligned_store(trapframe *tf) {
	printk(BIOS_DEBUG, "Trapframe ptr:      %p\n", tf);
	uintptr_t faultingInstructionAddr = tf->epc;
	insn_t faultingInstruction = fetch_instruction(faultingInstructionAddr);
	printk(BIOS_DEBUG, "Faulting instruction: 0x%x\n", faultingInstruction);
	insn_t widthMask = 0x7000;
	insn_t memWidth = (faultingInstruction & widthMask) >> 12;
	insn_t srcMask = 0x1F00000;
	insn_t srcRegister = (faultingInstruction & srcMask) >> 20;
	printk(BIOS_DEBUG, "Width: 0x%x\n", memWidth);
	if (memWidth == 3) {
		// store double, handle the issue
		void* badAddress = (void*) tf->badvaddr;
		uint64_t value = tf->gpr[srcRegister];
		for (int i = 0; i < 8; i++) {
			mprv_write_u8(badAddress+i, value);
			value >>= 8;
		}
	} else {
		// panic, this should not have happened
		die("Code should not reach this path, misaligned on a non-64 bit store/load\n");
	}

	// return to where we came from
	write_csr(mepc, read_csr(mepc) + 4);
	asm volatile("j machine_call_return");
}
