// from winevdm by otya128, GPL2 licensed

#include "haxmvm.h"

void haxmvm_panic(const char *fmt, ...)
{
	va_list arg;
	va_start(arg, fmt);
	vprintf(fmt, arg);
	va_end(arg);
	ExitProcess(1);
}

#define REG8(x) state.x
#define AL _al
#define AH _ah
#define BL _bl
#define BH _bh
#define CL _cl
#define CH _ch
#define DL _dl
#define DH _dh
#define REG16(x) state.x
#define AX _ax
#define BX _bx
#define CX _cx
#define DX _dx
#define SP _sp
#define BP _bp
#define SI _si
#define DI _di
#define REG32(x) state.x
#define EAX _eax
#define EBX _ebx
#define ECX _ecx
#define EDX _edx
#define ESP _esp
#define EBP _ebp
#define ESI _esi
#define EDI _edi
#define SREG_BASE(x) state.x.base
#define SREG(x) state.x.selector
#define DS _ds
#define ES _es
#define FS _fs
#define GS _gs
#define CS _cs
#define SS _ss

#define i386_load_segment_descriptor(x) load_segdesc(state.x)
#define i386_sreg_load(x, y, z) state.y.selector = x; load_segdesc(state.y)
#define i386_get_flags() state._eflags
#define i386_set_flags(x) state._eflags = x
#define i386_push16 PUSH16
#define i386_pop16 POP16
#define vtlb_free(x) {}
#define I386_SREG segment_desc_t

#define m_eip state._eip
#define m_pc (state._eip + state._cs.base)
#define CR(x) state._cr ##x
#define DR(x) state._dr ##x
#define m_gdtr state._gdt
#define m_idtr state._idt
#define m_ldtr state._ldt
#define m_task state._tr

#define HAXMVM_STR2(s) #s
#define HAXMVM_STR(s) HAXMVM_STR2(s)
#define HAXMVM_ERR fprintf(stderr, "%s ("  HAXMVM_STR(__LINE__)  ") HAXM err.\n", __FUNCTION__)
#define HAXMVM_ERRF(fmt, ...) fprintf(stderr, "%s ("  HAXMVM_STR(__LINE__)  ") " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

#define PROTECTED_MODE (state._cr0 & 1)
#define V8086_MODE (state._eflags & 0x20000)

#define I386OP(x) i386_ ##x
#define HOLD_LINE 1
#define CLEAR_LINE 0
#define INPUT_LINE_IRQ 1

static HANDLE hSystem;
static HANDLE hVM;
static HANDLE hVCPU;
static struct hax_tunnel *tunnel;
static struct vcpu_state_t state;
static char *iobuf;
static UINT8 m_CF, m_SF, m_ZF, m_IF, m_IOP1, m_IOP2, m_VM, m_NT;
static UINT32 m_a20_mask = 0xffffffff;
static UINT8 cpu_type = 6; // ppro
static UINT8 cpu_step = 0x0f; // whatever
static UINT8 m_CPL = 0; // always check at cpl 0
static UINT32 m_int6h_skip_eip = 0xffff0; // TODO: ???
static UINT8 m_ext;
static UINT32 m_prev_eip;
static int saved_vector = -1;

static int instr_emu(int cnt);

static void CALLBACK cpu_int_cb(LPVOID arg, DWORD low, DWORD high)
{
/*	DWORD bytes;
	int irq = 0xf8;
	if (tunnel->ready_for_interrupt_injection)
		DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_INTERRUPT, &irq, sizeof(irq), NULL, 0, &bytes, NULL);
	else
		tunnel->request_interrupt_window = 1;*/
}

static DWORD CALLBACK cpu_int_th(LPVOID arg)
{
	LARGE_INTEGER when;
	HANDLE timer;

	if (!(timer = CreateWaitableTimerA( NULL, FALSE, NULL ))) return 0;

	when.u.LowPart = when.u.HighPart = 0;
	SetWaitableTimer(timer, &when, 10, cpu_int_cb, arg, FALSE);
	for (;;) SleepEx(INFINITE, TRUE);
}

static void vm_exit()
{
	CloseHandle(hVCPU);
	CloseHandle(hVM);
	CloseHandle(hSystem);
}

static BOOL cpu_init_haxm()
{
	hSystem = CreateFileW(L"\\\\.\\HAX", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hSystem == INVALID_HANDLE_VALUE)
	{
		HAXMVM_ERRF("HAXM is not installed.");
		return FALSE;
	}
	struct hax_module_version ver;
	DWORD bytes;
	if (!DeviceIoControl(hSystem, HAX_IOCTL_VERSION, NULL, 0, &ver, sizeof(ver), &bytes, NULL))
	{
		HAXMVM_ERRF("VERSION");
		return FALSE;
	}
	struct hax_capabilityinfo cap;
	if (!DeviceIoControl(hSystem, HAX_IOCTL_CAPABILITY, NULL, 0, &cap, sizeof(cap), &bytes, NULL))
	{
		HAXMVM_ERRF("CAPABILITY");
		return FALSE;
	}
	if ((cap.wstatus & HAX_CAP_WORKSTATUS_MASK) == HAX_CAP_STATUS_NOTWORKING)
	{
		HAXMVM_ERRF("Hax is disabled\n");
		return FALSE;
	}
	if (!(cap.winfo & HAX_CAP_UG))
	{
		HAXMVM_ERRF("CPU unrestricted guest support required");
		return FALSE;
	}
	
	uint32_t vm_id;
	if (!DeviceIoControl(hSystem, HAX_IOCTL_CREATE_VM, NULL, 0, &vm_id, sizeof(vm_id), &bytes, NULL))
	{
		HAXMVM_ERRF("CREATE_VM");
		return FALSE;
	}
	WCHAR buf[1000];
	swprintf_s(buf, RTL_NUMBER_OF(buf), L"\\\\.\\hax_vm%02d", vm_id);
	hVM = CreateFileW(buf, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hVM == INVALID_HANDLE_VALUE)
	{
		HAXMVM_ERRF("Could not create vm.");
		return FALSE;
	}
	uint32_t vcpu_id;
	struct hax_qemu_version verq;
	/* 3~ enable fast mmio */
	verq.cur_version = 1;
	verq.least_version = 0;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_NOTIFY_QEMU_VERSION, &verq, sizeof(verq), NULL, 0, &bytes, NULL))
	{
	}
	vcpu_id = 1;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_VCPU_CREATE, &vcpu_id, sizeof(vcpu_id), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("could not create vcpu.");
		return FALSE;
	}
	swprintf_s(buf, RTL_NUMBER_OF(buf), L"\\\\.\\hax_vm%02d_vcpu%02d", vm_id, vcpu_id);
	hVCPU = CreateFileW(buf, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	struct hax_tunnel_info tunnel_info;
	if (!DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_SETUP_TUNNEL, NULL, 0, &tunnel_info, sizeof(tunnel_info), &bytes, NULL))
	{
		HAXMVM_ERRF("SETUP_TUNNEL");
		return FALSE;
	}
	/* memory mapping */
	struct hax_alloc_ram_info alloc_ram = { 0 };
	struct hax_set_ram_info ram = { 0 };
	alloc_ram.size = MAX_MEM; 
	alloc_ram.va = (uint64_t)mem;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_ALLOC_RAM, &alloc_ram, sizeof(alloc_ram), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("ALLOC_RAM");
		return FALSE;
	}
	ram.pa_start = 0;
	ram.size = MEMORY_END;
	ram.va = (uint64_t)mem;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("SET_RAM");
		return FALSE;
	}
	ram.pa_start = UMB_TOP;
	ram.size = 0x100000 - UMB_TOP;
	ram.va = (uint64_t)mem + UMB_TOP;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("SET_RAM");
		return FALSE;
	}
	ram.pa_start = 0x100000;
	ram.size = 0x100000;
	ram.va = (uint64_t)mem;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("SET_RAM");
		return FALSE;
	}
	ram.pa_start = 0x200000;
	ram.size = MAX_MEM - 0x200000;
	ram.va = (uint64_t)mem + 0x200000;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("SET_RAM");
		return FALSE;
	}
/*	ram.pa_start = MEMORY_END;
	ram.size = UMB_TOP - MEMORY_END;
	ram.va = 0;
	ram.flags = HAX_RAM_INFO_INVALID;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
	{
		HAXMVM_ERRF("SET_RAM");
		return FALSE;
	}*/
	tunnel = (struct hax_tunnel*)tunnel_info.va;
	iobuf = (char *)tunnel_info.io_va;
	DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
	state._idt.limit = 0x400;

	HANDLE thread = CreateThread(NULL, 0, cpu_int_th, NULL, 0, NULL);
	SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
	CloseHandle(thread);
	atexit(vm_exit);
	return TRUE;
}

static void cpu_reset_haxm()
{
}

const int TRANSLATE_READ            = 0;        // translate for read
const int TRANSLATE_WRITE           = 1;        // translate for write
const int TRANSLATE_FETCH           = 2;        // translate for instruction fetch

// TODO: mark pages dirty if necessary, check for page faults
// dos programs likly never used three level page tables
static int translate_address(int pl, int type, UINT32 *address, UINT32 *error)
{
	if(!(state._cr0 & 0x80000000))
		return TRUE;

	UINT32 *pdbr = (UINT32 *)(mem + (state._cr3 * 0xfffff000));
	UINT32 a = *address;
	UINT32 dir = (a >> 22) & 0x3ff;
	UINT32 table = (a >> 12) & 0x3ff;
	UINT32 page_dir = pdbr[dir];
	if(page_dir & 1)
	{
		if((page_dir & 0x80) && (state._cr4 & 0x10))
		{
			*address = (page_dir & 0xffc00000) | (a & 0x003fffff);
			return TRUE;
		}
	}
	else
	{
		UINT32 page_entry = *(UINT32 *)(mem + (page_dir & 0xfffff000) + (table * 4));
		if(!(page_entry & 1))
			return FALSE;
		else
		{
			*address = (page_entry & 0xfffff000) | (a & 0xfff);
			return TRUE;
		}
	}
	return FALSE;
}

static BOOL i386_load_protected_mode_segment(I386_SREG *seg, UINT32 *desc)
{
	UINT32 v1,v2;
	UINT32 base, limit;
	int entry;

	if(!seg->selector)
	{
		seg->flags = 0;
		seg->base = 0;
		seg->limit = 0;
		return 0;
	}

	if ( seg->selector & 0x4 )
	{
		base = state._ldt.base;
		limit = state._ldt.limit;
	}
	else
	{
		base = state._gdt.base;
		limit = state._gdt.limit;
	}

	entry = seg->selector & ~0x7;
	if (limit == 0 || entry + 7 > limit)
		return 0;

	UINT32 address = base + entry;
	translate_address(0, TRANSLATE_READ, &address, NULL); 
	v1 = *(UINT32 *)(mem + address);
	v2 = *(UINT32 *)(mem + address + 4);

	seg->flags = (v2 >> 8) & 0xf0ff;
	seg->base = (v2 & 0xff000000) | ((v2 & 0xff) << 16) | ((v1 >> 16) & 0xffff);
	seg->limit = (v2 & 0xf0000) | (v1 & 0xffff);
	if (seg->flags & 0x8000)
		seg->limit = (seg->limit << 12) | 0xfff;

	return 1;
}

static void i386_set_descriptor_accessed(UINT16 selector)
{
	// assume the selector is valid, we don't need to check it again
	UINT32 base, addr;
	if(!(selector & ~3))
		return;

	if ( selector & 0x4 )
		base = state._ldt.base;
	else
		base = state._gdt.base;

	addr = base + (selector & ~7) + 5;
	translate_address(0, TRANSLATE_READ, &addr, NULL); 
	mem[addr] |= 1;
}

static void load_segdesc(segment_desc_t &seg)
{
	if (PROTECTED_MODE)
	{
		if (!V8086_MODE)
		{
			i386_load_protected_mode_segment((I386_SREG *)&seg, NULL);
			if(seg.selector)
				i386_set_descriptor_accessed(seg.selector);
		}
		else
		{
			seg.base = seg.selector << 4;
			seg.limit = 0xffff;
			seg.ar = (&seg == &state._cs) ? 0xfb : 0xf3;
		}
	}
	else
	{
		seg.base = seg.selector << 4;
		if(&seg == &state._cs)
			seg.ar = 0x93;
	}
}

// TODO: check ss limit
static UINT32 i386_read_stack(bool dword = false)
{
	UINT32 addr = state._ss.base;
	if(state._ss.operand_size)
		addr += REG32(ESP);
	else
		addr += REG16(SP) & 0xffff;
	translate_address(0, TRANSLATE_READ, &addr, NULL);
	return dword ? read_dword(addr) : read_word(addr);
}

static void i386_write_stack(UINT32 value, bool dword = false)
{
	UINT32 addr = state._ss.base;
	if(state._ss.operand_size)
		addr += REG32(ESP);
	else
		addr += REG16(SP);
	translate_address(0, TRANSLATE_WRITE, &addr, NULL);
	dword ? write_dword(addr, value) : write_word(addr, value);
}

static void PUSH16(UINT16 val)
{
	if(state._ss.operand_size)
		REG32(ESP) -= 2;
	else
		REG16(SP) = (REG16(SP) - 2) & 0xffff;
	i386_write_stack(val);
}

static UINT16 POP16()
{
	UINT16 val = i386_read_stack();
	if(state._ss.operand_size)
		REG32(ESP) += 2;
	else
		REG16(SP) = (REG16(SP) + 2) & 0xffff;
	return val;
}

// pmode far calls/jmps/rets are potentially extremely complex (call gates, task switches, privilege changes)
// so bail and hope the issue never comes up
// if the destination isn't mapped, we're in trouble
static void i386_call_far(UINT16 selector, UINT32 address)
{
	if (PROTECTED_MODE)
	{
		if (!V8086_MODE)
			haxmvm_panic("i386_call_far in protected mode and !v86mode not supported");
		else
		{
			if((state._cr0 & 0x80000000) && (selector == DUMMY_TOP))  // check that this is mapped
			{
				UINT32 addr = DUMMY_TOP + address;
				translate_address(0, TRANSLATE_READ, &addr, NULL);
				if (address != (DUMMY_TOP + address))
					haxmvm_panic("i386_call_far to dummy segment with page unmapped");
			}
		}
	}
	PUSH16(state._cs.selector);
	PUSH16(state._eip);
	state._cs.selector = selector;
	load_segdesc(state._cs);
	state._eip = address;
}

static void i386_jmp_far(UINT16 selector, UINT32 address)
{
	if (PROTECTED_MODE && !V8086_MODE)
		haxmvm_panic("i386_jmp_far in protected mode and !v86mode not supported");
	state._cs.selector = selector;
	load_segdesc(state._cs);
	state._eip = address;
}

static void i386_pushf()
{
	PUSH16(state._eflags);
}

static void i386_retf16()
{
	if (PROTECTED_MODE && !V8086_MODE)
		haxmvm_panic("i386_retf16 in protected mode and !v86mode not supported");
	state._eip = POP16();
	state._cs.selector = POP16();
	load_segdesc(state._cs);

}

static void i386_iret16()
{
	if (PROTECTED_MODE && !V8086_MODE)
		haxmvm_panic("i386_iret16 in protected mode and !v86mode not supported");
	state._eip = POP16();
	state._cs.selector = POP16();
	load_segdesc(state._cs);
	state._eflags = (state._eflags & 0xffff0002) | POP16();
	m_CF = state._eflags & 1;
	m_ZF = (state._eflags & 0x40) ? 1 : 0;
	m_SF = (state._eflags & 0x80) ? 1 : 0;
	m_IF = (state._eflags & 0x200) ? 1 : 0;
	m_IOP1 = (state._eflags & 0x1000) ? 1 : 0;
	m_IOP2 = (state._eflags & 0x2000) ? 1 : 0;
	m_NT = (state._eflags & 0x4000) ? 1 : 0;
	m_VM = (state._eflags & 0x20000) ? 1 : 0;
}

static void i386_set_a20_line(int state)
{
	DWORD bytes;
	struct hax_set_ram_info ram = { 0 };
	ram.pa_start = 0x100000;
	ram.size = 0x100000;
	if(state)
		ram.va = (uint64_t)mem + 0x100000;
	else
		ram.va = (uint64_t)mem;
	if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
		HAXMVM_ERRF("SET_RAM");
}

static void i386_trap(int irq, int irq_gate, int trap_level)
{
	DWORD bytes;
	if (tunnel->ready_for_interrupt_injection)
		DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_INTERRUPT, &irq, sizeof(irq), NULL, 0, &bytes, NULL);
	else
	{
		saved_vector = irq;
		tunnel->request_interrupt_window = 1;
	}
}

static void i386_set_irq_line(int irqline, int state)
{
	if (state)
	{
		if (tunnel->ready_for_interrupt_injection)
		{
			DWORD bytes, irq = pic_ack();
			DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_INTERRUPT, &irq, sizeof(irq), NULL, 0, &bytes, NULL);
		}
		else
		{
			saved_vector = -2;
			tunnel->request_interrupt_window = 1;
		}
	}
}

static void cpu_execute_haxm()
{
	DWORD bytes;
	state._eflags = (state._eflags & ~0x272c1) | (m_VM << 17) | (m_NT << 14) | (m_IOP2 << 13) | (m_IOP1 << 12)
		| (m_IF << 9) | (m_SF << 7) | (m_ZF << 6) | m_CF;
	if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL))
		HAXMVM_ERRF("SET_REGS");
	while (TRUE)
	{
		if (!DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_RUN, NULL, 0, NULL, 0, &bytes, NULL))
			return;

		switch(tunnel->_exit_status)
		{
			case HAX_EXIT_IO:
				if(tunnel->io._port == 0xf7)
				{
					DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
					m_SF = (state._eflags & 0x80) ? 1 : 0;
					write_io_byte(0xf7, 0);
					state._eflags = (state._eflags & ~0x80) | (m_SF << 7);
					DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL);
					continue;
				}
				if(!(tunnel->io._flags & 1))
				{
					char* addr = iobuf;
					switch(tunnel->io._size)
					{
						case 1:
							if(tunnel->io._direction == HAX_IO_IN)
								*(UINT8 *)addr = read_io_byte(tunnel->io._port);
							else
								write_io_byte(tunnel->io._port, *(UINT8 *)addr);
							break;
						case 2:
							if(tunnel->io._direction == HAX_IO_IN)
								*(UINT16 *)addr = read_io_word(tunnel->io._port);
							else
								write_io_word(tunnel->io._port, *(UINT16 *)addr);
							break;
						case 4:
							if(tunnel->io._direction == HAX_IO_IN)
								*(UINT32 *)addr = read_io_dword(tunnel->io._port);
							else
								write_io_dword(tunnel->io._port, *(UINT32 *)addr);
							break;
					}
				}
				else
				{
					char* addr = iobuf;
					addr += tunnel->io._df ? tunnel->io._count * tunnel->io._size : 0;
					for(int i = 0; i < tunnel->io._count; i++)
					{
						addr = tunnel->io._df ? addr - tunnel->io._size : addr + tunnel->io._size;
						switch(tunnel->io._size)
						{
							case 1:
								if(tunnel->io._direction == HAX_IO_OUT)
									write_io_byte(tunnel->io._port, *(UINT8 *)addr);
								else
									*(UINT8 *)addr = read_io_byte(tunnel->io._port);
								break;
							case 2:
								if(tunnel->io._direction == HAX_IO_OUT)
									write_io_word(tunnel->io._port, *(UINT16 *)addr);
								else
									*(UINT16 *)addr = read_io_word(tunnel->io._port);
								break;
							case 4:
								if(tunnel->io._direction == HAX_IO_OUT)
									write_io_dword(tunnel->io._port, *(UINT32 *)addr);
								else
									*(UINT32 *)addr = read_io_dword(tunnel->io._port);
								break;
						}
					}
				}
#ifdef EXPORT_DEBUG_TO_FILE
				fflush(fp_debug_log);
#endif
				continue;
			case HAX_EXIT_FAST_MMIO:
			{
				// this doesn't work due to the way haxm emulates instructions
				// without it though, programs that use vga are unusably slow
				//DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
				//instr_emu(0);
				//DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL);
				//continue;
				
				struct hax_fastmmio *hft = (struct hax_fastmmio *)iobuf;
				UINT32 val = (hft->direction == 1) ? hft->value : 0;
				UINT32 gpaw = (hft->direction == 2) ? hft->gpa2 : hft->gpa;
				if(hft->direction != 1)
				{
					switch(hft->size)
					{
						case 1:
							val = read_byte(hft->gpa);
							break;
						case 2:
							val = read_word(hft->gpa);
							break;
						case 4:
							val = read_dword(hft->gpa);
							break;
					}
				}
				if(hft->direction != 0)
				{
					switch(hft->size)
					{
						case 1:
							write_byte(gpaw, val);
							break;
						case 2:
							write_word(gpaw, val);
							break;
						case 4:
							write_dword(gpaw, val);
							break;
					}
				}
				else if(hft->direction == 0)
					hft->value = val;
				continue;
			}
			case HAX_EXIT_HLT:
			{
				DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
				offs_t hltaddr = state._cs.base + state._eip - 1;
				translate_address(0, TRANSLATE_READ, &hltaddr, NULL);
				if((hltaddr >= IRET_TOP) && (hltaddr < (IRET_TOP + IRET_SIZE)))
				{
					int syscall = hltaddr - IRET_TOP;
					i386_iret16();
					if(syscall == 0xf8)
						hardware_update();
					else
						msdos_syscall(syscall);
#ifdef EXPORT_DEBUG_TO_FILE
					fflush(fp_debug_log);
#endif
					state._eflags = (state._eflags & ~0x272c1) | (m_VM << 17) | (m_NT << 14) |
						(m_IOP2 << 13) | (m_IOP1 << 12) | (m_IF << 9) | (m_SF << 7) | (m_ZF << 6) | m_CF;
					if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL))
						HAXMVM_ERRF("SET_REGS");
					continue;
				}
				else if(hltaddr == 0xffff0)
				{
					m_exit = 1;
					return;
				}
				else if((hltaddr >= DUMMY_TOP) && (hltaddr < 0xffff0))
					return;
				else 
					haxmvm_panic("handle hlt");
			}
			case HAX_EXIT_STATECHANGE:
				haxmvm_panic("hypervisor is panicked!!!");
				return;
			case HAX_EXIT_INTERRUPT:
				tunnel->request_interrupt_window = 0;
				if(saved_vector == -1)
				{
					hardware_update();
					continue;
				}
				if(saved_vector == -2)
					saved_vector = pic_ack();
				DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_INTERRUPT, &saved_vector, sizeof(saved_vector), NULL, 0, &bytes, NULL);
				saved_vector = -1;
				continue;
			default:
				HAXMVM_ERRF("exit status: %d %04x:%04x", tunnel->_exit_status, state._cs.selector, state._eip);
				return;
		}
	}
}

// simple x86 emulation for vga mmio
// does no privilege checks or complex instructions

/*
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * some configurable options
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Debug level for the Graphics Controller.
 * 0 - normal / 1 - useful / 2 - too much
 */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#define COUNT  150	/* bail out when this #instructions were simulated
			   after a VGA r/w access */

#define R_WORD(a) (*((unsigned short *) &(a)))
#define R_DWORD(a) (*((unsigned *) &(a)))
#define OP_JCC(cond) eip += (cond) ? 2 + *(signed char *)MEM_BASE32(cs + eip + 1) : 2; break;

#define CF  (1 <<  0)
#define PF  (1 <<  2)
#define AF  (1 <<  4)
#define ZF  (1 <<  6)
#define SF  (1 <<  7)
#define DF  (1 << 10)
#define OF  (1 << 11)

static unsigned char *MEM_BASE32(uint32 address)
{
	translate_address(0, TRANSLATE_READ, &address, NULL); // TODO: check for page fault
	return mem + address;
}

typedef uint32 dosaddr_t;
#define DOSADDR_REL(x) (x - mem)

/* assembly macros to speed up x86 on x86 emulation: the cpu helps us in setting
   the flags */

#define OPandFLAG0(eflags, insn, op1, istype) __asm__ __volatile__("\n\
		"#insn"	%0\n\
		pushf; pop	%1\n \
		" : #istype (op1), "=g" (eflags) : "0" (op1));

#define OPandFLAG1(eflags, insn, op1, istype) __asm__ __volatile__("\n\
		"#insn"	%0, %0\n\
		pushf; pop	%1\n \
		" : #istype (op1), "=g" (eflags) : "0" (op1));

#define OPandFLAG(eflags, insn, op1, op2, istype, type) __asm__ __volatile__("\n\
		"#insn"	%3, %0\n\
		pushf; pop	%1\n \
		" : #istype (op1), "=g" (eflags) : "0" (op1), #type (op2));

#define OPandFLAGC(eflags, insn, op1, op2, istype, type) __asm__ __volatile__("\n\
		shr     $1, %0\n\
		"#insn" %4, %1\n\
		pushf; pop     %0\n \
		" : "=r" (eflags), #istype (op1)  : "0" (eflags), "1" (op1), #type (op2));


#if !defined True
#define False 0
#define True 1
#endif

#ifdef ENABLE_DEBUG_LOG
#define instr_deb(x...) fprintf(fp_debug_log, "instremu: " x)
#ifdef ENABLE_DEBUG_TRACE
#define ARRAY_LENGTH(x)     (sizeof(x) / sizeof(x[0]))
const UINT32 DASMFLAG_SUPPORTED     = 0x80000000;   // are disassembly flags supported?
const UINT32 DASMFLAG_STEP_OUT      = 0x40000000;   // this instruction should be the end of a step out sequence
const UINT32 DASMFLAG_STEP_OVER     = 0x20000000;   // this instruction should be stepped over by setting a breakpoint afterwards
const UINT32 DASMFLAG_OVERINSTMASK  = 0x18000000;   // number of extra instructions to skip when stepping over
const UINT32 DASMFLAG_OVERINSTSHIFT = 27;           // bits to shift after masking to get the value
const UINT32 DASMFLAG_LENGTHMASK    = 0x0000ffff;   // the low 16-bits contain the actual length
#include "mame/emu/cpu/i386/i386dasm.c"
#define instr_deb2(x...) fprintf(fp_debug_log, "instremu: " x)
#else
#define instr_deb2(x...)
#endif
#else
#define instr_deb(x...)
#define instr_deb2(x...)
#endif

enum {REPNZ = 0, REPZ = 1, REP_NONE = 2};

typedef struct x86_emustate {
	unsigned seg_base, seg_ss_base;
	unsigned address_size; /* in bytes so either 4 or 2 */
	unsigned operand_size;
	unsigned prefixes, rep;
	unsigned (*instr_binary)(unsigned op, unsigned op1,
			unsigned op2, uint32 *eflags);
	unsigned (*instr_read)(const unsigned char *addr);
	void (*instr_write)(unsigned char *addr, unsigned u);
	unsigned char *(*modrm)(unsigned char *cp, x86_emustate *x86, int *inst_len);
} x86_emustate;

#ifdef ENABLE_DEBUG_LOG
static char *seg_txt[7] = { "", "es: ", "cs: ", "ss: ", "ds: ", "fs: ", "gs: " };
static char *rep_txt[3] = { "", "repnz ", "repz " };
static char *lock_txt[2] = { "", "lock " };
#endif

static unsigned wordmask[5] = {0,0xff,0xffff,0xffffff,0xffffffff};

static unsigned char it[0x100] = {
	7, 7, 7, 7, 2, 3, 1, 1,    7, 7, 7, 7, 2, 3, 1, 0,
	7, 7, 7, 7, 2, 3, 1, 1,    7, 7, 7, 7, 2, 3, 1, 1,
	7, 7, 7, 7, 2, 3, 0, 1,    7, 7, 7, 7, 2, 3, 0, 1,
	7, 7, 7, 7, 2, 3, 0, 1,    7, 7, 7, 7, 2, 3, 0, 1,

	1, 1, 1, 1, 1, 1, 1, 1,    1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,    1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 7, 7, 0, 0, 0, 0,    3, 9, 2, 8, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2,    2, 2, 2, 2, 2, 2, 2, 2,

	8, 9, 8, 8, 7, 7, 7, 7,    7, 7, 7, 7, 7, 7, 7, 7,
	1, 1, 1, 1, 1, 1, 1, 1,    1, 1, 6, 1, 1, 1, 1, 1,
	4, 4, 4, 4, 1, 1, 1, 1,    2, 3, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 2,    3, 3, 3, 3, 3, 3, 3, 3,

	8, 8, 3, 1, 7, 7, 8, 9,    5, 1, 3, 1, 1, 2, 1, 1,
	7, 7, 7, 7, 2, 2, 1, 1,    0, 0, 0, 0, 0, 0, 0, 0,
	2, 2, 2, 2, 2, 2, 2, 2,    4, 4, 6, 2, 1, 1, 1, 1,
	0, 1, 0, 0, 1, 1, 7, 7,    1, 1, 1, 1, 1, 1, 7, 7
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
static unsigned seg, lock, rep;
static int count;
#define vga_base VGA_VRAM_TOP
#define vga_end (vga_base + EMS_TOP)

static unsigned arg_len(unsigned char *, int);

static unsigned char instr_read_byte(const unsigned char *addr);
static unsigned instr_read_word(const unsigned char *addr);
static unsigned instr_read_dword(const unsigned char *addr);
static void instr_write_byte(unsigned char *addr, unsigned char u);
static void instr_write_word(unsigned char *addr, unsigned u);
static void instr_write_dword(unsigned char *addr, unsigned u);
static void instr_flags(unsigned val, unsigned smask, uint32 *eflags);
static unsigned instr_shift(unsigned op, int op1, unsigned op2, unsigned size, uint32 *eflags);
static unsigned char *sib(unsigned char *cp, x86_emustate *x86, int *inst_len);
static unsigned char *modrm32(unsigned char *cp, x86_emustate *x86, int *inst_len);
static unsigned char *modrm16(unsigned char *cp, x86_emustate *x86, int *inst_len);

static void dump_x86_regs()
{
	instr_deb(
			"eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
			state._eax, state._ebx, state._ecx, state._edx, state._esi, state._edi, state._ebp, state._esp
		 );
	instr_deb(
			"eip=%08x cs=%04x/%08x ds=%04x/%08x es=%04x/%08x d=%u c=%u p=%u a=%u z=%u s=%u o=%u\n",
			state._eip, state._cs.selector, (uint32)state._cs.base, state._ds.selector,
			(uint32)state._ds.base, state._es.selector, (uint32)state._es.base,
			(state._eflags&DF)>>10,
			state._eflags&CF,(state._eflags&PF)>>2,(state._eflags&AF)>>4,
			(state._eflags&ZF)>>6,(state._eflags&SF)>>7,(state._eflags&OF)>>11
		 );
}

static int instr_len(unsigned char *p, int is_32)
{
	unsigned u, osp, asp;
	unsigned char *p0 = p;
#ifdef ENABLE_DEBUG_LOG
	unsigned char *p1 = p;
#endif

	seg = lock = rep = 0;
	osp = asp = is_32;

	for(u = 1; u && p - p0 < 17;) switch(*p++) {		/* get prefixes */
		case 0x26:	/* es: */
			seg = 1; break;
		case 0x2e:	/* cs: */
			seg = 2; break;
		case 0x36:	/* ss: */
			seg = 3; break;
		case 0x3e:	/* ds: */
			seg = 4; break;
		case 0x64:	/* fs: */
			seg = 5; break;
		case 0x65:	/* gs: */
			seg = 6; break;
		case 0x66:	/* operand size */
			osp ^= 1; break;
		case 0x67:	/* address size */
			asp ^= 1; break;
		case 0xf0:	/* lock */
			lock = 1; break;
		case 0xf2:	/* repnz */
			rep = 2; break;
		case 0xf3:	/* rep(z) */
			rep = 1; break;
		default:	/* no prefix */
			u = 0;
	}
	p--;

#ifdef ENABLE_DEBUG_LOG
	p1 = p;
#endif

	if(p - p0 >= 16) return 0;

	if(*p == 0x0f) {
		p++;
		switch (*p) {
			case 0xba:
				p += 4;
				return p - p0;
			default:
				/* not yet */
				instr_deb("unsupported instr_len %x %x\n", p[0], p[1]);
				return 0;
		}
	}

	switch(it[*p]) {
		case 1:	/* op-code */
			p += 1; break;

		case 2:	/* op-code + byte */
			p += 2; break;

		case 3:	/* op-code + word/dword */
			p += osp ? 5 : 3; break;

		case 4:	/* op-code + [word/dword] */
			p += asp ? 5 : 3; break;

		case 5:	/* op-code + word/dword + byte */
			p += osp ? 6 : 4; break;

		case 6:	/* op-code + [word/dword] + word */
			p += asp ? 7 : 5; break;

		case 7:	/* op-code + mod + ... */
			p++;
			p += (u = arg_len(p, asp));
			if(!u) p = p0;
			break;

		case 8:	/* op-code + mod + ... + byte */
			p++;
			p += (u = arg_len(p, asp)) + 1;
			if(!u) p = p0;
			break;

		case 9:	/* op-code + mod + ... + word/dword */
			p++;
			p += (u = arg_len(p, asp)) + (osp ? 4 : 2);
			if(!u) p = p0;
			break;

		default:
			p = p0;
	}

#ifdef ENABLE_DEBUG_LOG
	if(p >= p0) {
		instr_deb("instr_len: instr = ");
		fprintf(fp_debug_log, "%s%s%s%s%s",
				osp ? "osp " : "", asp ? "asp " : "",
				lock_txt[lock], rep_txt[rep], seg_txt[seg]
			);
		if(p > p1) for(u = 0; u < p - p1; u++) {
			fprintf(fp_debug_log, "%02x ", p1[u]);
		}
		fprintf(fp_debug_log, "\n");
	}
#endif

	return p - p0;
}


static unsigned arg_len(unsigned char *p, int asp)
{
	unsigned u = 0, m, s = 0;

	m = *p & 0xc7;
	if(asp) {
		if(m == 5) {
			u = 5;
		}
		else {
			if((m >> 6) < 3 && (m & 7) == 4) s = 1;
			switch(m >> 6) {
				case 1:
					u = 2; break;
				case 2:
					u = 5; break;
				default:
					u = 1;
			}
			u += s;
		}
	}
	else {
		if(m == 6)
			u = 3;
		else
			switch(m >> 6) {
				case 1:
					u = 2; break;
				case 2:
					u = 3; break;
				default:
					u = 1;
			}
	}

	instr_deb2("arg_len: %02x %02x %02x %02x: %u bytes\n", p[0], p[1], p[2], p[3], u);

	return u;
}

/*
 * Some functions to make using the vga emulation easier.
 *
 *
 */

static unsigned char instr_read_byte(const unsigned char *address)
{
	unsigned char u;
	dosaddr_t addr = DOSADDR_REL(address);

	if(addr >= vga_base && addr < vga_end) {
		count = COUNT;
		u = vga_read(addr, 1);
	}
	else {
		u = *address;
	}
#ifdef ENABLE_DEBUG_TRACE
	instr_deb2("Read byte 0x%x", u);
	if (addr<0x8000000) fprintf(fp_debug_log, " from address %x\n", addr); else fprintf(fp_debug_log, "\n");
#endif

	return u;
}

static unsigned instr_read_word(const unsigned char *address)
{
	unsigned u;

	/*
	 * segment wrap-arounds within a data word are not allowed since
	 * at least i286, so no problems here
	 */
	dosaddr_t addr = DOSADDR_REL(address);
	if(addr >= vga_base && addr < vga_end) {
		count = COUNT;
		u = vga_read(addr, 2);
	} else
		u = *(unsigned short *)address;

#ifdef ENABLE_DEBUG_TRACE
	instr_deb2("Read word 0x%x", u);
	if (addr<0x8000000) fprintf(fp_debug_log, " from address %x\n", addr); else fprintf(fp_debug_log, "\n");
#endif
	return u;
}

static unsigned instr_read_dword(const unsigned char *address)
{
	unsigned u;

	/*
	 * segment wrap-arounds within a data word are not allowed since
	 * at least i286, so no problems here
	 */
	dosaddr_t addr = DOSADDR_REL(address);
	if(addr >= vga_base && addr < vga_end) {
		count = COUNT;
		u = vga_read(addr, 4);
	} else
		u = *(unsigned *)address;

#ifdef ENABLE_DEBUG_TRACE
	instr_deb2("Read word 0x%x", u);
	if (addr<0x8000000) fprintf(fp_debug_log, " from address %x\n", addr); else fprintf(fp_debug_log, "\n");
#endif
	return u;
}

static void instr_write_byte(unsigned char *address, unsigned char u)
{
	dosaddr_t addr = DOSADDR_REL(address);

	if(addr >= vga_base && addr < vga_end) {
		count = COUNT;
		vga_write(addr, u, 1);
	}
	else {
		*address = u;
	}
#ifdef ENABLE_DEBUG_TRACE
	instr_deb2("Write byte 0x%x", u);
	if (addr<0x8000000) fprintf(fp_debug_log, " at address %x\n", addr); else fprintf(fp_debug_log, "\n");
#endif
}

static void instr_write_word(unsigned char *address, unsigned u)
{
	dosaddr_t dst = DOSADDR_REL(address);
	/*
	 * segment wrap-arounds within a data word are not allowed since
	 * at least i286, so no problems here.
	 * we assume application do not try to mix here
	 */

	if(dst >= vga_base && dst < vga_end) {
		count = COUNT;
		vga_write(dst, u, 2);
	}
	else
		*(unsigned short *)address = u;

#ifdef ENABLE_DEBUG_TRACE
	instr_deb2("Write word 0x%x", u);
	if (dst<0x8000000) fprintf(fp_debug_log, " at address %x\n", dst); else fprintf(fp_debug_log, "\n");
#endif
}

static void instr_write_dword(unsigned char *address, unsigned u)
{
	dosaddr_t dst = DOSADDR_REL(address);

	/*
	 * segment wrap-arounds within a data word are not allowed since
	 * at least i286, so no problems here.
	 * we assume application do not try to mix here
	 */

	if(dst >= vga_base && dst < vga_end) {
		count = COUNT;
		vga_write(dst, u, 4);
	}
	else
		*(unsigned *)address = u;

#ifdef ENABLE_DEBUG_TRACE
	instr_deb2("Write word 0x%x", u);
	if (dst<0x8000000) fprintf(fp_debug_log, " at address %x\n", dst); else fprintf(fp_debug_log, "\n");
#endif
}

/* We use the cpu itself to set the flags, which is easy since we are
   emulating x86 on x86. */
static void instr_flags(unsigned val, unsigned smask, uint32 *eflags)
{
	uintptr_t flags;

	*eflags &= ~(OF|ZF|SF|PF|CF);
	if (val & smask)
		*eflags |= SF;
	OPandFLAG1(flags, orl, val, =r);
	*eflags |= flags & (ZF|PF);
}

/* 6 logical and arithmetic "RISC" core functions
   follow
   */
static unsigned char instr_binary_byte(unsigned char op, unsigned char op1, unsigned char op2, uint32 *eflags)
{
	uintptr_t flags;

	switch (op&0x7){
		case 1: /* or */
			OPandFLAG(flags, orb, op1, op2, =q, q);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return op1;
		case 4: /* and */
			OPandFLAG(flags, andb, op1, op2, =q, q);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return op1;
		case 6: /* xor */
			OPandFLAG(flags, xorb, op1, op2, =q, q);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return op1;
		case 0: /* add */
			*eflags &= ~CF; /* Fall through */
		case 2: /* adc */
			flags = *eflags;
			OPandFLAGC(flags, adcb, op1, op2, =q, q);
			*eflags = (*eflags & ~(OF|ZF|SF|AF|PF|CF)) | (flags & (OF|ZF|AF|SF|PF|CF));
			return op1;
		case 5: /* sub */
		case 7: /* cmp */
			*eflags &= ~CF; /* Fall through */
		case 3: /* sbb */
			flags = *eflags;
			OPandFLAGC(flags, sbbb, op1, op2, =q, q);
			*eflags = (*eflags & ~(OF|ZF|SF|AF|PF|CF)) | (flags & (OF|ZF|AF|SF|PF|CF));
			return op1;
	}
	return 0;
}

static unsigned instr_binary_word(unsigned op, unsigned op1, unsigned op2, uint32 *eflags)
{
	uintptr_t flags;
	unsigned short opw1 = op1;
	unsigned short opw2 = op2;

	switch (op&0x7){
		case 1: /* or */
			OPandFLAG(flags, orw, opw1, opw2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return opw1;
		case 4: /* and */
			OPandFLAG(flags, andw, opw1, opw2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return opw1;
		case 6: /* xor */
			OPandFLAG(flags, xorw, opw1, opw2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return opw1;
		case 0: /* add */
			*eflags &= ~CF; /* Fall through */
		case 2: /* adc */
			flags = *eflags;
			OPandFLAGC(flags, adcw, opw1, opw2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|AF|PF|CF)) | (flags & (OF|ZF|AF|SF|PF|CF));
			return opw1;
		case 5: /* sub */
		case 7: /* cmp */
			*eflags &= ~CF; /* Fall through */
		case 3: /* sbb */
			flags = *eflags;
			OPandFLAGC(flags, sbbw, opw1, opw2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|AF|PF|CF)) | (flags & (OF|ZF|AF|SF|PF|CF));
			return opw1;
	}
	return 0;
}

static unsigned instr_binary_dword(unsigned op, unsigned op1, unsigned op2, uint32 *eflags)
{
	uintptr_t flags;

	switch (op&0x7){
		case 1: /* or */
			OPandFLAG(flags, orl, op1, op2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return op1;
		case 4: /* and */
			OPandFLAG(flags, andl, op1, op2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return op1;
		case 6: /* xor */
			OPandFLAG(flags, xorl, op1, op2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|PF|CF)) | (flags & (OF|ZF|SF|PF|CF));
			return op1;
		case 0: /* add */
			*eflags &= ~CF; /* Fall through */
		case 2: /* adc */
			flags = *eflags;
			OPandFLAGC(flags, adcl, op1, op2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|AF|PF|CF)) | (flags & (OF|ZF|AF|SF|PF|CF));
			return op1;
		case 5: /* sub */
		case 7: /* cmp */
			*eflags &= ~CF; /* Fall through */
		case 3: /* sbb */
			flags = *eflags;
			OPandFLAGC(flags, sbbl, op1, op2, =r, r);
			*eflags = (*eflags & ~(OF|ZF|SF|AF|PF|CF)) | (flags & (OF|ZF|AF|SF|PF|CF));
			return op1;
	}
	return 0;
}

static unsigned instr_shift(unsigned op, int op1, unsigned op2, unsigned size, uint32 *eflags)
{
	unsigned result, carry;
	unsigned width = size * 8;
	unsigned mask = wordmask[size];
	unsigned smask = (mask >> 1) + 1;
	op2 &= 31;

	switch (op&0x7){
		case 0: /* rol */
			op2 &= width-1;
			result = (((op1 << op2) | ((op1&mask) >> (width-op2)))) & mask;
			*eflags &= ~(CF|OF);
			*eflags |= (result & CF) | ((((result >> (width-1)) ^ result) << 11) & OF);
			return result;
		case 1:/* ror */
			op2 &= width-1;
			result = ((((op1&mask) >> op2) | (op1 << (width-op2)))) & mask;
			*eflags &= ~(CF|OF);
			carry = (result >> (width-1)) & CF;
			*eflags |=  carry |
				(((carry ^ (result >> (width-2))) << 11) & OF);
			return result;
		case 2: /* rcl */
			op2 %= width+1;
			carry = (op1>>(width-op2))&CF;
			result = (((op1 << op2) | ((op1&mask) >> (width+1-op2))) | ((*eflags&CF) << (op2-1))) & mask;
			*eflags &= ~(CF|OF);
			*eflags |= carry | ((((result >> (width-1)) ^ carry) << 11) & OF);
			return result;
		case 3:/* rcr */
			op2 %= width+1;
			carry = (op1>>(op2-1))&CF;
			result = ((((op1&mask) >> op2) | (op1 << (width+1-op2))) | ((*eflags&CF) << (width-op2))) & mask;
			*eflags &= ~(CF|OF);
			*eflags |= carry | ((((result >> (width-1)) ^ (result >> (width-2))) << 11) & OF);
			return result;
		case 4: /* shl */
			result = (op1 << op2) & mask;
			instr_flags(result, smask, eflags);
			*eflags &= ~(CF|OF);
			*eflags |= ((op1 >> (width-op2))&CF) |
				((((op1 >> (width-1)) ^ (op1 >> (width-2))) << 11) & OF);
			return result;
		case 5: /* shr */
			result = ((unsigned)(op1&mask) >> op2);
			instr_flags(result, smask, eflags);
			*eflags &= ~(CF|OF);
			*eflags |= ((op1 >> (op2-1)) & CF) | (((op1 >> (width-1)) << 11) & OF);
			return result;
		case 7: /* sar */
			result = op1 >> op2;
			instr_flags(result, smask, eflags);
			*eflags &= ~(CF|OF);
			*eflags |= (op1 >> (op2-1)) & CF;
			return result;
	}
	return 0;
}

static inline void push(unsigned val, x86_emustate *x86)
{
	if (state._ss.operand_size)
		state._esp -= x86->operand_size;
	else
		state._sp -= x86->operand_size;
	i386_write_stack(val, x86->operand_size == 4);
}

static inline void pop(unsigned *val, x86_emustate *x86)
{
	*val = i386_read_stack(x86->operand_size == 4);
	if (state._ss.operand_size)
		state._esp += x86->operand_size;
	else
		state._sp += x86->operand_size;
}

/* helper functions/macros reg8/reg/sreg/sib/modrm16/32 for instr_sim
   for address and register decoding */
enum { es_INDEX, cs_INDEX, ss_INDEX, ds_INDEX, fs_INDEX, gs_INDEX };

#define reg8(reg) ((uint8 *)&state._regs[reg & 3] + ((reg & 4) ? 1 : 0))
#define reg(reg) ((uint32 *)&state._regs[reg & 7])
#define sreg_idx(reg) (es_INDEX+((reg)&0x7))

static uint16 *sreg(int reg)
{
	segment_desc_t *sreg;
	switch(reg & 7)
	{
		case es_INDEX:
			sreg = &state._es;
			break;
		case cs_INDEX:
			sreg = &state._cs;
			break;
		case ss_INDEX:
			sreg = &state._ss;
			break;
		case ds_INDEX:
			sreg = &state._ds;
			break;
		case fs_INDEX:
			sreg = &state._fs;
			break;
		case gs_INDEX:
			sreg = &state._gs;
			break;
		default:
			return 0;
	}
	return &sreg->selector;
}

static unsigned char *sib(unsigned char *cp, x86_emustate *x86, int *inst_len)
{
	unsigned addr = 0;

	switch(cp[1] & 0xc0) { /* decode modifier */
		case 0x40:
			addr = (int)(signed char)cp[3];
			break;
		case 0x80:
			addr = R_DWORD(cp[3]);
			break;
	}

	if ((cp[2] & 0x38) != 0x20) /* index cannot be esp */
		addr += *reg(cp[2]>>3) << (cp[2] >> 6);

	switch(cp[2] & 0x07) { /* decode address */
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x06:
		case 0x07:
			return MEM_BASE32(addr + *reg(cp[2]) + x86->seg_base);
		case 0x04: /* esp */
			return MEM_BASE32(addr + state._esp + x86->seg_ss_base);
		case 0x05:
			if (cp[1] >= 0x40)
				return MEM_BASE32(addr + state._ebp + x86->seg_ss_base);
			else {
				*inst_len += 4;
				return MEM_BASE32(addr + R_DWORD(cp[3]) + x86->seg_base);
			}
	}
	return 0; /* keep gcc happy */
}

static unsigned char *modrm16(unsigned char *cp, x86_emustate *x86, int *inst_len)
{
	unsigned addr = 0;
	*inst_len = 0;

	switch(cp[1] & 0xc0) { /* decode modifier */
		case 0x40:
			addr = (short)(signed char)cp[2];
			*inst_len = 1;
			break;
		case 0x80:
			addr = R_WORD(cp[2]);
			*inst_len = 2;
			break;
		case 0xc0:
			if (cp[0]&1) /*(d)word*/
				return (unsigned char *)reg(cp[1]);
			else
				return reg8(cp[1]);
	}


	switch(cp[1] & 0x07) { /* decode address */
		case 0x00:
			return MEM_BASE32(((addr + state._ebx + state._esi) & 0xffff) + x86->seg_base);
		case 0x01:
			return MEM_BASE32(((addr + state._ebx + state._edi) & 0xffff) + x86->seg_base);
		case 0x02:
			return MEM_BASE32(((addr + state._ebp + state._esi) & 0xffff) + x86->seg_ss_base);
		case 0x03:
			return MEM_BASE32(((addr + state._ebp + state._edi) & 0xffff) + x86->seg_ss_base);
		case 0x04:
			return MEM_BASE32(((addr + state._esi) & 0xffff) + x86->seg_base);
		case 0x05:
			return MEM_BASE32(((addr + state._edi) & 0xffff) + x86->seg_base);
		case 0x06:
			if (cp[1] >= 0x40)
				return MEM_BASE32(((addr + state._ebp) & 0xffff) + x86->seg_ss_base);
			else {
				*inst_len += 2;
				return MEM_BASE32(R_WORD(cp[2]) + x86->seg_base);
			}
		case 0x07:
			return MEM_BASE32(((addr + state._ebx) & 0xffff) + x86->seg_base);
	}
	return 0; /* keep gcc happy */
}

static unsigned char *modrm32(unsigned char *cp, x86_emustate *x86, int *inst_len)
{
	unsigned addr = 0;
	*inst_len = 0;

	switch(cp[1] & 0xc0) { /* decode modifier */
		case 0x40:
			addr = (int)(signed char)cp[2];
			*inst_len = 1;
			break;
		case 0x80:
			addr = R_DWORD(cp[2]);
			*inst_len = 4;
			break;
		case 0xc0:
			if (cp[0]&1) /*(d)word*/
				return ((unsigned char *)reg(cp[1]));
			else
				return reg8(cp[1]);
	}
	switch(cp[1] & 0x07) { /* decode address */
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x06:
		case 0x07:
			return MEM_BASE32(addr + *reg(cp[1]) + x86->seg_base);
		case 0x04: /* sib byte follows */
			*inst_len += 1;
			return sib(cp, x86, inst_len);
		case 0x05:
			if (cp[1] >= 0x40)
				return MEM_BASE32(addr + state._ebp + x86->seg_ss_base);
			else {
				*inst_len += 4;
				return MEM_BASE32(R_DWORD(cp[2]) + x86->seg_base);
			}
	}
	return 0; /* keep gcc happy */
}

static int handle_prefixes(x86_emustate *x86)
{
	unsigned eip = state._eip;
	int prefix = 0;

	for (;; eip++) {
		switch(*(unsigned char *)MEM_BASE32(state._cs.base + eip)) {
			/* handle (some) prefixes */
			case 0x26:
				prefix++;
				x86->seg_base = x86->seg_ss_base = state._es.base;
				break;
			case 0x2e:
				prefix++;
				x86->seg_base = x86->seg_ss_base = state._cs.base;
				break;
			case 0x36:
				prefix++;
				x86->seg_base = x86->seg_ss_base = state._ss.base;
				break;
			case 0x3e:
				prefix++;
				x86->seg_base = x86->seg_ss_base = state._ds.base;
				break;
			case 0x64:
				prefix++;
				x86->seg_base = x86->seg_ss_base = state._fs.base;
				break;
			case 0x65:
				prefix++;
				x86->seg_base = x86->seg_ss_base = state._gs.base;
				break;
			case 0x66:
				prefix++;
				x86->operand_size = 6 - x86->operand_size;
				if (x86->operand_size == 4) {
					x86->instr_binary = instr_binary_dword;
					x86->instr_read = instr_read_dword;
					x86->instr_write = instr_write_dword;
				} else {
					x86->instr_binary = instr_binary_word;
					x86->instr_read = instr_read_word;
					x86->instr_write = instr_write_word;
				}
				break;
			case 0x67:
				prefix++;
				x86->address_size = 6 - x86->address_size;
				x86->modrm = (x86->address_size == 4 ? modrm32 : modrm16);
				break;
			case 0xf2:
				prefix++;
				x86->rep = REPNZ;
				break;
			case 0xf3:
				prefix++;
				x86->rep = REPZ;
				break;
			default:
				return prefix;
		}
	}
	return prefix;
}

static void prepare_x86(x86_emustate *x86)
{
	x86->seg_base = state._ds.base;
	x86->seg_ss_base = state._ss.base;
	x86->address_size = x86->operand_size = (state._cs.operand_size + 1) * 2;

	x86->modrm = (x86->address_size == 4 ? modrm32 : modrm16);
	x86->rep = REP_NONE;

	if (x86->operand_size == 4) {
		x86->instr_binary = instr_binary_dword;
		x86->instr_read = instr_read_dword;
		x86->instr_write = instr_write_dword;
	} else {
		x86->instr_binary = instr_binary_word;
		x86->instr_read = instr_read_word;
		x86->instr_write = instr_write_word;
	}
}

/* return value: 1 => instruction known; 0 => instruction not known */
static inline int instr_sim(x86_emustate *x86, int pmode)
{
	unsigned char *reg_8;
	unsigned char uc;
	unsigned short uns;
	unsigned *dstreg;
	unsigned und, und2, repcount;
	unsigned char *ptr;
	uintptr_t flags;
	int i, i2, inst_len;
	int loop_inc = (state._eflags&DF) ? -1 : 1;		// make it a char ?
	unsigned eip = state._eip;
	unsigned cs = state._cs.base;

#ifdef ENABLE_DEBUG_TRACE
	{
		int refseg;
		char frmtbuf[256];
		const UINT8 *oprom = mem + cs + eip;
		refseg = state._cs.selector;
		dump_x86_regs();
		i386_dasm_one(frmtbuf, eip, oprom, state._cs.operand_size ? 32 : 16);
		instr_deb("%s, %d\n", frmtbuf, count);
	}
#endif

	if (x86->prefixes) {
		prepare_x86(x86);
	}

	x86->prefixes = handle_prefixes(x86);
	eip += x86->prefixes;

	if (x86->rep != REP_NONE) {
		/* TODO: All these rep instruction can still be heavily optimized */
		i2 = 0;
		if (x86->address_size == 4) {
			repcount = state._ecx;
			switch(*(unsigned char *)MEM_BASE32(cs + eip)) {
				case 0xa4:         /* rep movsb */
#ifdef ENABLE_DEBUG_LOG
					if (state._es.base >= 0xa0000 && state._es.base < 0xb0000 &&
							x86->seg_base >= 0xa0000 && x86->seg_base < 0xb0000)
						instr_deb("VGAEMU: Video to video memcpy, ecx=%x\n", state._ecx);
					/* TODO: accelerate this using memcpy */
#endif
					for (i = 0, und = 0; und < repcount;
							i += loop_inc, und++)
						instr_write_byte(MEM_BASE32(state._es.base + state._edi+i),
								instr_read_byte(MEM_BASE32(x86->seg_base + state._esi+i)));
					state._edi += i;
					state._esi += i;
					break;

				case 0xa5:         /* rep movsw/d */
					/* TODO: accelerate this using memcpy */
					for (i = 0, und = 0; und < repcount;
							i += loop_inc*x86->operand_size, und++)
						x86->instr_write(MEM_BASE32(state._es.base + state._edi+i),
								x86->instr_read(MEM_BASE32(x86->seg_base + state._esi+i)));
					state._edi += i;
					state._esi += i;
					break;

				case 0xa6:         /* rep cmpsb */
					for (i = 0, und = 0; und < repcount;) {
						instr_binary_byte(7, instr_read_byte(MEM_BASE32(x86->seg_base + state._esi+i)),
								instr_read_byte(MEM_BASE32(state._es.base + state._edi+i)), &state._eflags);
						i += loop_inc;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0xf2 repnz 0xf3 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._edi += i;
					state._esi += i;
					break;

				case 0xa7:         /* rep cmpsw/d */
					for (i = 0, und = 0; und < repcount;) {
						x86->instr_binary(7, instr_read_byte(MEM_BASE32(x86->seg_base + state._esi+i)),
								x86->instr_read(MEM_BASE32(state._es.base + state._edi+i)), &state._eflags);
						i += loop_inc*x86->operand_size;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0xf2 repnz 0xf3 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._edi += i;
					state._esi += i;
					break;

				case 0xaa: /* rep stosb */
					/* TODO: accelerate this using memset */
					for (und2 = state._edi, und = 0; und < repcount;
							und2 += loop_inc, und++)
						instr_write_byte(MEM_BASE32(state._es.base + und2), state._al);
					state._edi = und2;
					break;

				case 0xab: /* rep stosw */
					/* TODO: accelerate this using memset */
					for (und2 = state._edi, und = 0; und < repcount;
							und2 += loop_inc*x86->operand_size, und++)
						x86->instr_write(MEM_BASE32(state._es.base + und2), state._eax);
					state._edi = und2;
					break;

				case 0xae: /* rep scasb */
					for (und2 = state._edi, und = 0; und < repcount;) {
						instr_binary_byte(7, state._al, instr_read_byte(MEM_BASE32(state._es.base + und2)), &state._eflags);
						und2 += loop_inc;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0x0 repnz 0x1 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._edi = und2;
					break;

				case 0xaf: /* rep scasw */
					for (und2 = state._edi, und = 0; und < repcount;) {
						x86->instr_binary(7, state._eax, x86->instr_read(MEM_BASE32(state._es.base + und2)), &state._eflags);
						und2 += loop_inc*x86->operand_size;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0x0 repnz 0x1 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._edi = und2;
					break;

				default:
					return 0;
			}

			state._ecx -= und;
			if (state._ecx > 0 && i2 == 0) return 1;

		} else {
			repcount = state._cx;
			switch(*(unsigned char *)MEM_BASE32(cs + eip)) {
				case 0xa4:         /* rep movsb */
#ifdef ENABLE_DEBUG_LOG
					if (state._es.base >= 0xa0000 && state._es.base < 0xb0000 &&
							x86->seg_base >= 0xa0000 && x86->seg_base < 0xb0000)
						instr_deb("VGAEMU: Video to video memcpy, cx=%x\n", state._cx);
					/* TODO: accelerate this using memcpy */
#endif
					for (i = 0, und = 0; und < repcount;
							i += loop_inc, und++)
						instr_write_byte(MEM_BASE32(state._es.base + ((state._edi+i) & 0xffff)),
								instr_read_byte(MEM_BASE32(x86->seg_base + ((state._esi+i) & 0xffff))));
					state._di += i;
					state._si += i;
					break;

				case 0xa5:         /* rep movsw/d */
					/* TODO: accelerate this using memcpy */
					for (i = 0, und = 0; und < repcount;
							i += loop_inc*x86->operand_size, und++)
						x86->instr_write(MEM_BASE32(state._es.base + ((state._edi+i) & 0xffff)),
								x86->instr_read(MEM_BASE32(x86->seg_base + ((state._esi+i) & 0xffff))));
					state._di += i;
					state._si += i;
					break;

				case 0xa6: /* rep?z cmpsb */
					for (i = 0, und = 0; und < repcount;) {
						instr_binary_byte(7, instr_read_byte(MEM_BASE32(x86->seg_base + ((state._esi+i) & 0xffff))),
								instr_read_byte(MEM_BASE32(state._es.base + ((state._edi+i) & 0xffff))), &state._eflags);
						i += loop_inc;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0x0 repnz 0x1 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._di += i;
					state._si += i;
					break;

				case 0xa7: /* rep?z cmpsw/d */
					for (i = 0, und = 0; und < repcount;) {
						x86->instr_binary(7, x86->instr_read(MEM_BASE32(x86->seg_base + ((state._esi+i) & 0xffff))),
								x86->instr_read(MEM_BASE32(state._es.base + ((state._edi+i) & 0xffff))), &state._eflags);
						i += loop_inc * x86->operand_size;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0x0 repnz 0x1 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._di += i;
					state._si += i;
					break;

				case 0xaa: /* rep stosb */
					/* TODO: accelerate this using memset */
					for (uns = state._di, und = 0; und < repcount;
							uns += loop_inc, und++)
						instr_write_byte(MEM_BASE32(state._es.base + uns), state._al);
					state._di = uns;
					break;

				case 0xab: /* rep stosw/d */
					/* TODO: accelerate this using memset */
					for (uns = state._di, und = 0; und < repcount;
							uns += loop_inc*x86->operand_size, und++)
						x86->instr_write(MEM_BASE32(state._es.base + uns), (x86->operand_size == 4 ? state._eax : state._ax));
					state._di = uns;
					break;

				case 0xae: /* rep scasb */
					for (uns = state._di, und = 0; und < repcount;) {
						instr_binary_byte(7, state._al, instr_read_byte(MEM_BASE32(state._es.base + uns)), &state._eflags);
						uns += loop_inc;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0x0 repnz 0x1 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._di = uns;
					break;

				case 0xaf: /* rep scasw/d */
					for (uns = state._di, und = 0; und < repcount;) {
						x86->instr_binary(7, state._ax, instr_read_word(MEM_BASE32(state._es.base + uns)), &state._eflags);
						uns += loop_inc*x86->operand_size;
						und++;
						if (((state._eflags & ZF) >> 6) != x86->rep) /* 0x0 repnz 0x1 repz */ {
							i2 = 1; /* we're fine now! */
							break;
						}
					}
					state._di = uns;
					break;

				default:
					return 0;
			}
			state._cx -= und;
			if (state._cx > 0 && i2 == 0) return 1;
		}
		eip++;
	}
	else switch(*(unsigned char *)MEM_BASE32(cs + eip)) {
		case 0x00:		/* add r/m8,reg8 */
		case 0x08:		/* or r/m8,reg8 */
		case 0x10:		/* adc r/m8,reg8 */
		case 0x18:		/* sbb r/m8,reg8 */
		case 0x20:		/* and r/m8,reg8 */
		case 0x28:		/* sub r/m8,reg8 */
		case 0x30:		/* xor r/m8,reg8 */
		case 0x38:		/* cmp r/m8,reg8 */
			ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			uc = instr_binary_byte((*(unsigned char *)MEM_BASE32(cs + eip))>>3,
					instr_read_byte(ptr), *reg8((*(unsigned char *)MEM_BASE32(cs + eip + 1))>>3), &state._eflags);
			if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38)
				instr_write_byte(ptr, uc);
			eip += 2 + inst_len; break;

		case 0x01:		/* add r/m16,reg16 */
		case 0x09:		/* or r/m16,reg16 */
		case 0x11:		/* adc r/m16,reg16 */
		case 0x19:		/* sbb r/m16,reg16 */
		case 0x21:		/* and r/m16,reg16 */
		case 0x29:		/* sub r/m16,reg16 */
		case 0x31:		/* xor r/m16,reg16 */
		case 0x39:		/* cmp r/m16,reg16 */
			ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			und = x86->instr_binary(*(unsigned char *)MEM_BASE32(cs + eip)>>3, x86->instr_read(ptr), *reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3), &state._eflags);
			if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38)
				x86->instr_write(ptr, und);
			eip += 2 + inst_len; break;

		case 0x02:		/* add reg8,r/m8 */
		case 0x0a:		/* or reg8,r/m8 */
		case 0x12:		/* adc reg8,r/m8 */
		case 0x1a:		/* sbb reg8,r/m8 */
		case 0x22:		/* and reg8,r/m8 */
		case 0x2a:		/* sub reg8,r/m8 */
		case 0x32:		/* xor reg8,r/m8 */
		case 0x3a:		/* cmp reg8,r/m8 */
			reg_8 = reg8(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3);
			uc = instr_binary_byte(*(unsigned char *)MEM_BASE32(cs + eip)>>3,
					*reg_8, instr_read_byte(x86->modrm(MEM_BASE32(cs + eip),
							x86, &inst_len)), &state._eflags);
			if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38) *reg_8 = uc;
			eip += 2 + inst_len; break;

		case 0x03:		/* add reg,r/m16 */
		case 0x0b:		/* or reg,r/m16 */
		case 0x13:		/* adc reg,r/m16 */
		case 0x1b:		/* sbb reg,r/m16 */
		case 0x23:		/* and reg,r/m16 */
		case 0x2b:		/* sub reg,r/m16 */
		case 0x33:		/* xor reg,r/m16 */
		case 0x3b:		/* cmp reg,r/m16 */
			dstreg = reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3);
			und = x86->instr_binary(*(unsigned char *)MEM_BASE32(cs + eip)>>3,
					*dstreg, x86->instr_read(x86->modrm(MEM_BASE32(cs + eip), x86,
							&inst_len)), &state._eflags);
			if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38) {
				if (x86->operand_size == 2)
					R_WORD(*dstreg) = und;
				else
					*dstreg = und;
			}
			eip += 2 + inst_len; break;

		case 0x04:		/* add al,imm8 */
		case 0x0c:		/* or al,imm8 */
		case 0x14:		/* adc al,imm8 */
		case 0x1c:		/* sbb al,imm8 */
		case 0x24:		/* and al,imm8 */
		case 0x2c:		/* sub al,imm8 */
		case 0x34:		/* xor al,imm8 */
		case 0x3c:		/* cmp al,imm8 */
			uc = instr_binary_byte(*(unsigned char *)MEM_BASE32(cs + eip)>>3, state._al,
					*(unsigned char *)MEM_BASE32(cs + eip + 1), &state._eflags);
			if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38) state._al = uc;
			eip += 2; break;

		case 0x05:		/* add ax,imm16 */
		case 0x0d:		/* or ax,imm16 */
		case 0x15:		/* adc ax,imm16 */
		case 0x1d:		/* sbb ax,imm16 */
		case 0x25:		/* and ax,imm16 */
		case 0x2d:		/* sub ax,imm16 */
		case 0x35:		/* xor ax,imm16 */
		case 0x3d:		/* cmp ax,imm16 */
			und = x86->instr_binary(*(unsigned char *)MEM_BASE32(cs + eip)>>3,
					state._eax, R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)), &state._eflags);
			if (x86->operand_size == 2) {
				if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38) state._ax = und;
				eip += 3;
			} else {
				if (*(unsigned char *)MEM_BASE32(cs + eip)<0x38) state._eax = und;
				eip += 5;
			}
			break;

		case 0x06:  /* push sreg */
		case 0x0e:
		case 0x16:
		case 0x1e:
			push(*sreg(*(unsigned char *)MEM_BASE32(cs + eip)>>3), x86);
			eip++; break;

		case 0x07:		/* pop es */
			if (pmode || x86->operand_size == 4)
				return 0;
			else {
				unsigned int seg = state._es.selector;
				pop(&seg, x86);
				state._es.selector = seg;
				state._es.base = seg << 4;
				eip++;
			}
			break;

			/* don't do 0x0f (extended instructions) for now */
			/* 0x17 pop ss is a bit dangerous and rarely used */

		case 0x1f:		/* pop ds */
			if (pmode || x86->operand_size == 4)
				return 0;
			else {
				unsigned int seg = state._ds.selector;
				pop(&seg, x86);
				state._ds.selector = seg;
				state._ds.base = seg << 4;
				eip++;
			}
			break;

		case 0x27:  /* daa */
			if (((state._al & 0xf) > 9) || (state._eflags&AF)) {
				state._al += 6;
				state._eflags |= AF;
			} else
				state._eflags &= ~AF;
			if ((state._al > 0x9f) || (state._eflags&CF)) {
				state._al += 0x60;
				instr_flags(state._al, 0x80, &state._eflags);
				state._eflags |= CF;
			} else
				instr_flags(state._al, 0x80, &state._eflags);
			eip++; break;

		case 0x2f:  /* das */
			if (((state._al & 0xf) > 9) || (state._eflags&AF)) {
				state._al -= 6;
				state._eflags |= AF;
			} else
				state._eflags &= ~AF;
			if ((state._al > 0x9f) || (state._eflags&CF)) {
				state._al -= 0x60;
				instr_flags(state._al, 0x80, &state._eflags);
				state._eflags |= CF;
			} else
				instr_flags(state._al, 0x80, &state._eflags);
			eip++; break;

		case 0x37:  /* aaa */
			if (((state._al & 0xf) > 9) || (state._eflags&AF)) {
				state._al = (state._eax+6) & 0xf;
				state._ah++;
				state._eflags |= (CF|AF);
			} else
				state._eflags &= ~(CF|AF);
			eip++; break;

		case 0x3f:  /* aas */
			if (((state._al & 0xf) > 9) || (state._eflags&AF)) {
				state._al = (state._eax-6) & 0xf;
				state._ah--;
				state._eflags |= (CF|AF);
			} else
				state._eflags &= ~(CF|AF);
			eip++; break;

		case 0x40:
		case 0x41:
		case 0x42:
		case 0x43:
		case 0x44:
		case 0x45:
		case 0x46:
		case 0x47: /* inc reg */
			state._eflags &= ~(OF|ZF|SF|PF|AF);
			dstreg = reg(*(unsigned char *)MEM_BASE32(cs + eip));
			if (x86->operand_size == 2) {
				OPandFLAG0(flags, incw, R_WORD(*dstreg), =r);
			} else {
				OPandFLAG0(flags, incl, *dstreg, =r);
			}
			state._eflags |= flags & (OF|ZF|SF|PF|AF);
			eip++; break;

		case 0x48:
		case 0x49:
		case 0x4a:
		case 0x4b:
		case 0x4c:
		case 0x4d:
		case 0x4e:
		case 0x4f: /* dec reg */
			state._eflags &= ~(OF|ZF|SF|PF|AF);
			dstreg = reg(*(unsigned char *)MEM_BASE32(cs + eip));
			if (x86->operand_size == 2) {
				OPandFLAG0(flags, decw, R_WORD(*dstreg), =r);
			} else {
				OPandFLAG0(flags, decl, *dstreg, =r);
			}
			state._eflags |= flags & (OF|ZF|SF|PF|AF);
			eip++; break;

		case 0x50:
		case 0x51:
		case 0x52:
		case 0x53:
		case 0x54:
		case 0x55:
		case 0x56:
		case 0x57: /* push reg */
			push(*reg(*(unsigned char *)MEM_BASE32(cs + eip)), x86);
			eip++; break;

		case 0x58:
		case 0x59:
		case 0x5a:
		case 0x5b:
		case 0x5c:
		case 0x5d:
		case 0x5e:
		case 0x5f: /* pop reg */
			pop(&und, x86);
			if (x86->operand_size == 2)
				R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip))) = und;
			else
				*reg(*(unsigned char *)MEM_BASE32(cs + eip)) = und;
			eip++; break;

			/* 0x60 */
		case 0x68: /* push imm16 */
			push(R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)), x86);
			eip += x86->operand_size + 1; break;

		case 0x6a: /* push imm8 */
			push((int)*(signed char *)MEM_BASE32(cs + eip + 1), x86);
			eip += 2; break;

		case 0x70: OP_JCC(state._eflags & OF);         /*jo*/
		case 0x71: OP_JCC(!(state._eflags & OF));      /*jno*/
		case 0x72: OP_JCC(state._eflags & CF);         /*jc*/
		case 0x73: OP_JCC(!(state._eflags & CF));      /*jnc*/
		case 0x74: OP_JCC(state._eflags & ZF);         /*jz*/
		case 0x75: OP_JCC(!(state._eflags & ZF));      /*jnz*/
		case 0x76: OP_JCC(state._eflags & (ZF|CF));    /*jbe*/
		case 0x77: OP_JCC(!(state._eflags & (ZF|CF))); /*ja*/
		case 0x78: OP_JCC(state._eflags & SF);         /*js*/
		case 0x79: OP_JCC(!(state._eflags & SF));      /*jns*/
		case 0x7a: OP_JCC(state._eflags & PF);         /*jp*/
		case 0x7b: OP_JCC(!(state._eflags & PF));      /*jnp*/
		case 0x7c: OP_JCC((state._eflags & SF)^((state._eflags & OF)>>4))         /*jl*/
		case 0x7d: OP_JCC(!((state._eflags & SF)^((state._eflags & OF)>>4)))      /*jnl*/
		case 0x7e: OP_JCC((state._eflags & (SF|ZF))^((state._eflags & OF)>>4))    /*jle*/
		case 0x7f: OP_JCC(!((state._eflags & (SF|ZF))^((state._eflags & OF)>>4))) /*jg*/

		case 0x80:		/* logical r/m8,imm8 */
		case 0x82:
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   uc = instr_binary_byte(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3,
					   instr_read_byte(ptr), *(unsigned char *)MEM_BASE32(cs + eip + 2 + inst_len), &state._eflags);
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) < 0x38)
				   instr_write_byte(ptr, uc);
			   eip += 3 + inst_len; break;

		case 0x81:		/* logical r/m,imm */
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   und = x86->instr_binary(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3,
					   x86->instr_read(ptr), R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 2 + inst_len)), &state._eflags);
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) < 0x38) x86->instr_write(ptr, und);
			   eip += x86->operand_size + 2 + inst_len;
			   break;

		case 0x83:		/* logical r/m,imm8 */
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   und = x86->instr_binary(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3,
					   x86->instr_read(ptr), (int)*(signed char *)MEM_BASE32(cs + eip + 2 + inst_len),
					   &state._eflags);
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) < 0x38)
				   x86->instr_write(ptr, und);
			   eip += inst_len + 3; break;

		case 0x84: /* test r/m8, reg8 */
			   instr_flags(instr_read_byte(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len)) &
					   *reg8(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3),
					   0x80, &state._eflags);
			   eip += inst_len + 2; break;

		case 0x85: /* test r/m16, reg */
			   if (x86->operand_size == 2)
				   instr_flags(instr_read_word(x86->modrm(MEM_BASE32(cs + eip), x86,
								   &inst_len)) & R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3)),
						   0x8000, &state._eflags);
			   else
				   instr_flags(instr_read_dword(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len)) &
						   *reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3),
						   0x80000000, &state._eflags);
			   eip += inst_len + 2; break;

		case 0x86:		/* xchg r/m8,reg8 */
			   reg_8 = reg8(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3);
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   uc = *reg_8;
			   *reg_8 = instr_read_byte(ptr);
			   instr_write_byte(ptr, uc);
			   eip += inst_len + 2; break;

		case 0x87:		/* xchg r/m16,reg */
			   dstreg = reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3);
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   und = *dstreg;
			   if (x86->operand_size == 2)
				   R_WORD(*dstreg) = instr_read_word(ptr);
			   else
				   *dstreg = instr_read_dword(ptr);
			   x86->instr_write(ptr, und);
			   eip += inst_len + 2; break;

		case 0x88:		/* mov r/m8,reg8 */
			   instr_write_byte(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len),
					   *reg8(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3));
			   eip += inst_len + 2; break;

		case 0x89:		/* mov r/m16,reg */
			   x86->instr_write(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len),
					   *reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3));
			   eip += inst_len + 2; break;

		case 0x8a:		/* mov reg8,r/m8 */
			   *reg8(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3) =
				   instr_read_byte(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len));
			   eip += inst_len + 2; break;

		case 0x8b:		/* mov reg,r/m16 */
			   if (x86->operand_size == 2)
				   R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3)) =
					   instr_read_word(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len));
			   else
				   *reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3) =
					   instr_read_dword(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len));
			   eip += inst_len + 2; break;

		case 0x8c: /* mov r/m16,segreg */
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1) & 0xc0) == 0xc0) /* compensate for mov r,segreg */
				   ptr = (unsigned char *)reg(*(unsigned char *)MEM_BASE32(cs + eip + 1));
			   instr_write_word(ptr, *sreg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3));
			   eip += inst_len + 2; break;

		case 0x8d: /* lea */
			   {
				   unsigned ptr = x86->seg_ss_base;
				   x86->seg_ss_base = x86->seg_base;
				   if (x86->operand_size == 2)
					   R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3)) =
						   x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len) - (unsigned char *)MEM_BASE32(x86->seg_base);
				   else
					   *reg(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3) =
						   x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len) - (unsigned char *)MEM_BASE32(x86->seg_base);
				   x86->seg_ss_base = ptr;
				   eip += inst_len + 2; break;
			   }

		case 0x8e:		/* mov segreg,r/m16 */
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else switch (*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) {
				   case 0:
					   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
					   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1) & 0xc0) == 0xc0)  /* compensate for mov r,segreg */
						   ptr = (unsigned char *)reg(*(unsigned char *)MEM_BASE32(cs + eip + 1));
					   state._es.selector = instr_read_word(ptr);
					   state._es.base = state._es.selector << 4;
					   eip += inst_len + 2; break;
				   case 0x18:
					   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
					   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1) & 0xc0) == 0xc0) /* compensate for mov es,reg */
						   ptr = (unsigned char *)reg(*(unsigned char *)MEM_BASE32(cs + eip + 1));
					   state._ds.selector = instr_read_word(ptr);
					   state._ds.base = state._ds.selector << 4;
					   x86->seg_base = state._ds.base;
					   eip += inst_len + 2; break;
				   default:
					   return 0;
			   }
			   break;

		case 0x8f: /*pop*/
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) == 0){
				   pop(&und, x86);
				   x86->instr_write(x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len), und);
				   eip += inst_len + 2;
			   } else
				   return 0;
			   break;

		case 0x90: /* nop */
			   eip++; break;
		case 0x91:
		case 0x92:
		case 0x93:
		case 0x94:
		case 0x95:
		case 0x96:
		case 0x97: /* xchg reg, ax */
			   dstreg = reg(*(unsigned char *)MEM_BASE32(cs + eip));
			   und = state._eax;
			   if (x86->operand_size == 2) {
				   state._ax = *dstreg;
				   R_WORD(*dstreg) = und;
			   } else {
				   state._eax = *dstreg;
				   *dstreg = und;
			   }
			   eip++; break;

		case 0x98:
			   if (x86->operand_size == 2) /* cbw */
				   state._ax = (short)(signed char)state._al;
			   else /* cwde */
				   state._eax = (int)(short)state._ax;
			   eip++; break;

		case 0x99:
			   if (x86->operand_size == 2) /* cwd */
				   state._dx = (state._ax > 0x7fff ? 0xffff : 0);
			   else /* cdq */
				   state._edx = (state._eax > 0x7fffffff ? 0xffffffff : 0);
			   eip++; break;

		case 0x9a: /*call far*/
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else {
				   unsigned sel = state._cs.selector;
				   push(sel, x86);
				   push(eip + 5, x86);
				   state._cs.selector = R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 3));
				   state._cs.base = state._cs.selector << 4;
				   eip = R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
				   cs = state._cs.base;
			   }
			   break;
			   /* NO: 0x9b wait 0x9c pushf 0x9d popf*/

		case 0x9e: /* sahf */
			   state._eflags = (state._eflags & ~0xd5) | (state._ah & 0xd5);
			   eip++; break;

		case 0x9f: /* lahf */
			   state._ah = state._eflags & 0xff; 
			   eip++; break;

		case 0xa0:		/* mov al,moff16 */
			   state._al = instr_read_byte(MEM_BASE32((R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)) &
							   wordmask[x86->address_size])+x86->seg_base));
			   eip += 1 + x86->address_size; break;

		case 0xa1:		/* mov ax,moff16 */
			   if (x86->operand_size == 2)
				   state._ax = instr_read_word(MEM_BASE32((R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)) &
								   wordmask[x86->address_size])+x86->seg_base));
			   else
				   state._eax = instr_read_dword(MEM_BASE32((R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)) &
								   wordmask[x86->address_size])+x86->seg_base));
			   eip += 1 + x86->address_size; break;

		case 0xa2:		/* mov moff16,al */
			   instr_write_byte(MEM_BASE32((R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)) &
							   wordmask[x86->address_size])+x86->seg_base), state._al);
			   eip += 1 + x86->address_size; break;

		case 0xa3:		/* mov moff16,ax */
			   x86->instr_write(MEM_BASE32((R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)) &
							   wordmask[x86->address_size])+x86->seg_base), state._eax);
			   eip += 1 + x86->address_size; break;

		case 0xa4:		/* movsb */
			   if (x86->address_size == 4) {
				   instr_write_byte(MEM_BASE32(state._es.base + state._edi),
						   instr_read_byte(MEM_BASE32(x86->seg_base + state._esi)));
				   state._edi += loop_inc;
				   state._esi += loop_inc;
			   } else {
				   instr_write_byte(MEM_BASE32(state._es.base + state._di),
						   instr_read_byte(MEM_BASE32(x86->seg_base + state._si)));
				   state._di += loop_inc;
				   state._si += loop_inc;
			   }
			   eip++; break;

		case 0xa5:		/* movsw */
			   if (x86->address_size == 4) {
				   x86->instr_write(MEM_BASE32(state._es.base + state._edi),
						   x86->instr_read(MEM_BASE32(x86->seg_base + state._esi)));
				   state._edi += loop_inc * x86->operand_size;
				   state._esi += loop_inc * x86->operand_size;
			   }
			   else {
				   x86->instr_write(MEM_BASE32(state._es.base + state._di),
						   x86->instr_read(MEM_BASE32(x86->seg_base + state._si)));
				   state._di += loop_inc * x86->operand_size;
				   state._si += loop_inc * x86->operand_size;
			   }
			   eip++; break;

		case 0xa6: /*cmpsb */
			   if (x86->address_size == 4) {
				   instr_binary_byte(7, instr_read_byte(MEM_BASE32(x86->seg_base + state._esi)),
						   instr_read_byte(MEM_BASE32(state._es.base + state._edi)), &state._eflags);
				   state._edi += loop_inc;
				   state._esi += loop_inc;
			   } else {
				   instr_binary_byte(7, instr_read_byte(MEM_BASE32(x86->seg_base + state._si)),
						   instr_read_byte(MEM_BASE32(state._es.base + state._di)), &state._eflags);
				   state._di += loop_inc;
				   state._si += loop_inc;
			   }
			   eip++; break;

		case 0xa7: /* cmpsw */
			   if (x86->address_size == 4) {
				   x86->instr_binary(7, x86->instr_read(MEM_BASE32(x86->seg_base + state._esi)),
						   x86->instr_read(MEM_BASE32(state._es.base + state._edi)), &state._eflags);
				   state._edi += loop_inc * x86->operand_size;
				   state._esi += loop_inc * x86->operand_size;
			   } else {
				   x86->instr_binary(7, x86->instr_read(MEM_BASE32(x86->seg_base + state._si)),
						   x86->instr_read(MEM_BASE32(state._es.base + state._di)), &state._eflags);
				   state._di += loop_inc * x86->operand_size;
				   state._si += loop_inc * x86->operand_size;
			   }
			   eip++; break;

		case 0xa8: /* test al, imm */
			   instr_flags(state._al & *(unsigned char *)MEM_BASE32(cs + eip + 1), 0x80, &state._eflags);
			   eip += 2; break;

		case 0xa9: /* test ax, imm */
			   if (x86->operand_size == 2) {
				   instr_flags(state._ax & R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)), 0x8000, &state._eflags);
				   eip += 3; break;
			   } else {
				   instr_flags(state._eax & R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)), 0x80000000, &state._eflags);
				   eip += 5; break;
			   }

		case 0xaa:		/* stosb */
			   if (x86->address_size == 4) {
				   instr_write_byte(MEM_BASE32(state._es.base + state._edi), state._al);
				   state._edi += loop_inc;
			   } else {
				   instr_write_byte(MEM_BASE32(state._es.base + state._di), state._al);
				   state._di += loop_inc;
			   }
			   eip++; break;

		case 0xab:		/* stosw */
			   if (x86->address_size == 4) {
				   x86->instr_write(MEM_BASE32(state._es.base + state._edi), state._eax);
				   state._edi += loop_inc * x86->operand_size;
			   } else {
				   x86->instr_write(MEM_BASE32(state._es.base + state._di), state._eax);
				   state._di += loop_inc * x86->operand_size;
			   }
			   eip++; break;

		case 0xac:		/* lodsb */
			   if (x86->address_size == 4) {
				   state._al = instr_read_byte(MEM_BASE32(x86->seg_base + state._esi));
				   state._esi += loop_inc;
			   } else {
				   state._al = instr_read_byte(MEM_BASE32(x86->seg_base + state._si));
				   state._si += loop_inc;
			   }
			   eip++; break;

		case 0xad: /* lodsw */
			   if (x86->address_size == 4) {
				   und = x86->instr_read(MEM_BASE32(x86->seg_base + state._esi));
				   state._esi += loop_inc * x86->operand_size;
			   } else {
				   und = x86->instr_read(MEM_BASE32(x86->seg_base + state._si));
				   state._si += loop_inc * x86->operand_size;
			   }
			   if (x86->operand_size == 2)
				   state._ax = und;
			   else
				   state._eax = und;
			   eip++; break;

		case 0xae: /* scasb */
			   if (x86->address_size == 4) {
				   instr_binary_byte(7, state._al, instr_read_byte(MEM_BASE32(state._es.base + state._edi)), &state._eflags);
				   state._edi += loop_inc;
			   } else {
				   instr_binary_byte(7, state._al, instr_read_byte(MEM_BASE32(state._es.base + state._di)), &state._eflags);
				   state._di += loop_inc;
			   }
			   eip++; break;

		case 0xaf: /* scasw */
			   if (x86->address_size == 4) {
				   x86->instr_binary(7, state._eax, x86->instr_read(MEM_BASE32(state._es.base + state._edi)), &state._eflags);
				   state._edi += loop_inc * x86->operand_size;
			   } else {
				   x86->instr_binary(7, state._eax, x86->instr_read(MEM_BASE32(state._es.base + state._di)), &state._eflags);
				   state._di += loop_inc * x86->operand_size;
			   }
			   eip++; break;

		case 0xb0:
		case 0xb1:
		case 0xb2:
		case 0xb3:
		case 0xb4:
		case 0xb5:
		case 0xb6:
		case 0xb7:
			   *reg8(*(unsigned char *)MEM_BASE32(cs + eip)) = *(unsigned char *)MEM_BASE32(cs + eip + 1);
			   eip += 2; break;

		case 0xb8:
		case 0xb9:
		case 0xba:
		case 0xbb:
		case 0xbc:
		case 0xbd:
		case 0xbe:
		case 0xbf:
			   if (x86->operand_size == 2) {
				   R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip))) =
					   R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
				   eip += 3; break;
			   } else {
				   *reg(*(unsigned char *)MEM_BASE32(cs + eip)) =
					   R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
				   eip += 5; break;
			   }

		case 0xc0: /* shift byte, imm8 */
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38)==0x30) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   instr_write_byte(ptr,instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, (signed char) instr_read_byte(ptr),
						   *(unsigned char *)MEM_BASE32(cs + eip + 2+inst_len), 1, &state._eflags));
			   eip += inst_len + 3; break;

		case 0xc1: /* shift word, imm8 */
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38)==0x30) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   if (x86->operand_size == 2)
				   instr_write_word(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, (short)instr_read_word(ptr),
							   *(unsigned char *)MEM_BASE32(cs + eip + 2+inst_len), 2, &state._eflags));
			   else
				   instr_write_dword(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, instr_read_dword(ptr),
							   *(unsigned char *)MEM_BASE32(cs + eip + 2+inst_len), 4, &state._eflags));
			   eip += inst_len + 3; break;

		case 0xc2:		/* ret imm16*/
			   pop(&und, x86);
			   if (state._cs.operand_size)
				   state._esp += R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
			   else
				   state._sp += R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
			   eip = und;
			   break;

		case 0xc3:		/* ret */
			   pop(&eip, x86);
			   break;

		case 0xc4:		/* les */
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else {
				   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
				   state._es.selector = instr_read_word(ptr+2);
				   state._es.base = state._es.selector << 4;
				   R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip + 1) >> 3)) = instr_read_word(ptr);
				   eip += inst_len + 2; break;
			   }

		case 0xc5:		/* lds */
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else {
				   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
				   state._ds.selector = instr_read_word(ptr+2);
				   state._ds.base = x86->seg_base = state._es.selector << 4;
				   R_WORD(*reg(*(unsigned char *)MEM_BASE32(cs + eip + 1) >> 3)) = instr_read_word(ptr);
				   eip += inst_len + 2; break;
			   }

		case 0xc6:		/* mov r/m8,imm8 */
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   instr_write_byte(ptr, *(unsigned char *)MEM_BASE32(cs + eip + 2 + inst_len));
			   eip += inst_len + 3; break;

		case 0xc7:		/* mov r/m,imm */
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   x86->instr_write(ptr, R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 2 + inst_len)));
			   eip += x86->operand_size + inst_len + 2;
			   break;
			   /* 0xc8 enter */

		case 0xc9: /*leave*/
			   if (state._cs.operand_size)
				   state._esp = state._ebp;
			   else
				   state._sp = state._bp;
			   pop(&state._ebp, x86);
			   eip++; break;

		case 0xca: /*retf imm 16*/
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else {
				   unsigned int sel;
				   pop(&und, x86);
				   pop(&sel, x86);
				   state._cs.selector = sel;
				   state._cs.base = state._cs.selector << 4;
				   state._sp += R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
				   cs = state._cs.base;
				   eip = und;
			   }
			   break;

		case 0xcb: /*retf*/
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else {
				   unsigned int sel;
				   pop(&eip, x86);
				   pop(&sel, x86);
				   state._cs.selector = sel;
				   state._cs.base = state._cs.selector << 4;
				   cs = state._cs.base;
			   }
			   break;

			   /* 0xcc int3 0xcd int 0xce into 0xcf iret */

		case 0xd0: /* shift r/m8, 1 */
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38)==0x30) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   instr_write_byte(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3,
						   (signed char) instr_read_byte(ptr),
						   1, 1, &state._eflags));
			   eip += inst_len + 2; break;

		case 0xd1: /* shift r/m16, 1 */
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38)==0x30) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   if (x86->operand_size == 2)
				   instr_write_word(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, (short) instr_read_word(ptr),
							   1, 2, &state._eflags));
			   else
				   instr_write_dword(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, instr_read_dword(ptr), 1, 4, &state._eflags));
			   eip += inst_len + 2; break;

		case 0xd2: /* shift r/m8, cl */
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38)==0x30) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   instr_write_byte(ptr,instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, (signed char) instr_read_byte(ptr),
						   state._cl, 1, &state._eflags));
			   eip += inst_len + 2; break;

		case 0xd3: /* shift r/m16, cl */
			   if ((*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38)==0x30) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   if (x86->operand_size == 2)
				   instr_write_word(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, (short) instr_read_word(ptr),
							   state._cl, 2, &state._eflags));
			   else
				   instr_write_dword(ptr, instr_shift(*(unsigned char *)MEM_BASE32(cs + eip + 1)>>3, instr_read_dword(ptr),
							   state._cl, 4, &state._eflags));
			   eip += inst_len + 2; break;

		case 0xd4:  /* aam byte */
			   state._ah = state._al / *(unsigned char *)MEM_BASE32(cs + eip + 1);
			   state._al = state._al % *(unsigned char *)MEM_BASE32(cs + eip + 1);
			   instr_flags(state._al, 0x80, &state._eflags);
			   eip += 2; break;

		case 0xd5:  /* aad byte */
			   state._al = state._ah * *(unsigned char *)MEM_BASE32(cs + eip + 1) + state._al;
			   state._ah = 0;
			   instr_flags(state._al, 0x80, &state._eflags);
			   eip += 2; break;

		case 0xd6: /* salc */
			   state._al = state._eflags & CF ? 0xff : 0;
			   eip++; break;

		case 0xd7: /* xlat */
			   state._al =  instr_read_byte(MEM_BASE32(x86->seg_base+(state._ebx & wordmask[x86->address_size])+state._al));
			   eip++; break;
			   /* 0xd8 - 0xdf copro */

		case 0xe0: /* loopnz */
			   eip += ( (x86->address_size == 4 ? --state._ecx : --state._cx) && !(state._eflags & ZF) ?
					   2 + *(signed char *)MEM_BASE32(cs + eip + 1) : 2); break;

		case 0xe1: /* loopz */
			   eip += ( (x86->address_size == 4 ? --state._ecx : --state._cx) && (state._eflags & ZF) ?
					   2 + *(signed char *)MEM_BASE32(cs + eip + 1) : 2); break;

		case 0xe2: /* loop */
			   eip += ( (x86->address_size == 4 ? --state._ecx : --state._cx) ?
					   2 + *(signed char *)MEM_BASE32(cs + eip + 1) : 2); break;

		case 0xe3:  /* jcxz */
			   eip += ((x86->address_size == 4 ? state._ecx : state._cx) ? 2 :
					   2 + *(signed char *)MEM_BASE32(cs + eip + 1));
			   break;

			   /* 0xe4 in ib 0xe5 in iw 0xe6 out ib 0xe7 out iw */

		case 0xe8: /* call near */
			   push(eip + 1 + x86->operand_size, x86);
			   /* fall through */

		case 0xe9: /* jmp near */
			   eip += x86->operand_size + 1 + (R_DWORD(*(unsigned char *)MEM_BASE32(cs + eip + 1)) & wordmask[x86->operand_size]);
			   break;

		case 0xea: /*jmp far*/
			   if (pmode || x86->operand_size == 4)
				   return 0;
			   else {
				   state._cs.selector = R_WORD(*(unsigned char *)MEM_BASE32(cs + eip+3));
				   state._cs.base = state._cs.selector << 4;
				   eip = R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 1));
				   cs = state._cs.base;
			   }
			   break;

		case 0xeb: /* jmp short */
			   eip += 2 + *(signed char *)MEM_BASE32(cs + eip + 1); break;

		case 0xec: /* in al, dx */
			   /* Note that we short circuit if we can */
			   if ((state._dx >= 0x3b0) && (state._dx < 0x3e0)) {
				   state._al = read_io_byte(state._dx);
				   eip++; break;
			   }
			   else
				   return 0;
			   /* 0xed in ax,dx */

		case 0xee: /* out dx, al */
			   /* Note that we short circuit if we can */
			   if ((state._dx >= 0x3b0) && (state._dx < 0x3e0)) {
				   write_io_byte(state._dx, state._al);
				   eip++;
			   }
			   else
				   return 0;
			   break;

		case 0xef: /* out dx, ax */
			   if ((x86->operand_size == 2) &&
					   (state._dx >= 0x3b0) && (state._dx < 0x3e0)) {
				   write_io_word(state._dx, state._ax);
				   eip++;
			   }
			   else
				   return 0;
			   break;

			   /* 0xf0 lock 0xf1 int1 */

			   /* 0xf2 repnz 0xf3 repz handled above */
			   /* 0xf4 hlt */

		case 0xf5: /* cmc */
			   state._eflags ^= CF;
			   eip++; break;

		case 0xf6:
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   switch (*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) {
				   case 0x00: /* test ptr byte, imm */
					   instr_flags(instr_read_byte(ptr) & *(unsigned char *)MEM_BASE32(cs + eip + 2+inst_len), 0x80, &state._eflags);
					   eip += inst_len + 3; break;
				   case 0x08: return 0;
				   case 0x10: /*not byte*/
					      instr_write_byte(ptr, ~instr_read_byte(ptr));
					      eip += inst_len + 2; break;
				   case 0x18: /*neg byte*/
					      instr_write_byte(ptr, instr_binary_byte(7, 0, instr_read_byte(ptr), &state._eflags));
					      eip += inst_len + 2; break;
				   case 0x20: /*mul byte*/
					      state._ax = state._al * instr_read_byte(ptr);
					      state._eflags &= ~(CF|OF);
					      if (state._ah)
						      state._eflags |= (CF|OF);
					      eip += inst_len + 2; break;
				   case 0x28: /*imul byte*/
					      state._ax = (signed char)state._al * (signed char)instr_read_byte(ptr);
					      state._eflags &= ~(CF|OF);
					      if (state._ah)
						      state._eflags |= (CF|OF);
					      eip += inst_len + 2; break;
				   case 0x30: /*div byte*/
					      und = state._ax;
					      uc = instr_read_byte(ptr);
					      if (uc == 0) return 0;
					      und2 = und / uc;
					      if (und2 & 0xffffff00) return 0;
					      state._al = und2 & 0xff;
					      state._ah = und % uc;
					      eip += inst_len + 2; break;
				   case 0x38: /*idiv byte*/
					      i = (short)state._ax;
					      uc = instr_read_byte(ptr);
					      if (uc == 0) return 0;
					      i2 = i / (signed char)uc;
					      if (i2<-128 || i2>127) return 0;
					      state._al = i2 & 0xff;
					      state._ah = i % (signed char)uc;
					      eip += inst_len + 2; break;
			   }
			   break;

		case 0xf7:
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   switch (*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) {
				   case 0x00: /* test ptr word, imm */
					   if (x86->operand_size == 4) return 0;
					   instr_flags(instr_read_word(ptr) & R_WORD(*(unsigned char *)MEM_BASE32(cs + eip + 2+inst_len)), 0x8000, &state._eflags);
					   eip += inst_len + 4; break;
				   case 0x08: return 0;
				   case 0x10: /*not word*/
					      x86->instr_write(ptr, ~x86->instr_read(ptr));
					      eip += inst_len + 2; break;
				   case 0x18: /*neg word*/
					      x86->instr_write(ptr, x86->instr_binary(7, 0, x86->instr_read(ptr), &state._eflags));
					      eip += inst_len + 2; break;
				   case 0x20: /*mul word*/
					      if (x86->operand_size == 4) return 0;
					      und = state._ax * instr_read_word(ptr);
					      state._ax = und & 0xffff;
					      state._dx = und >> 16;
					      state._eflags &= ~(CF|OF);
					      if (state._dx)
						      state._eflags |= (CF|OF);
					      eip += inst_len + 2; break;
				   case 0x28: /*imul word*/
					      if (x86->operand_size == 4) return 0;
					      i = (short)state._ax * (short)instr_read_word(ptr);
					      state._ax = i & 0xffff;
					      state._dx = i >> 16;
					      state._eflags &= ~(CF|OF);
					      if (state._dx)
						      state._eflags |= (CF|OF);
					      eip += inst_len + 2; break;
				   case 0x30: /*div word*/
					      if (x86->operand_size == 4) return 0;
					      und = (state._dx<<16) + state._ax;
					      uns = instr_read_word(ptr);
					      if (uns == 0) return 0;
					      und2 = und / uns;
					      if (und2 & 0xffff0000) return 0;
					      state._ax = und2 & 0xffff;
					      state._dx = und % uns;
					      eip += inst_len + 2; break;
				   case 0x38: /*idiv word*/
					      if (x86->operand_size == 4) return 0;
					      i = ((short)state._dx<<16) + state._ax;
					      uns = instr_read_word(ptr);
					      if (uns == 0) return 0;
					      i2 = i / (short)uns;
					      if (i2<-32768 || i2>32767) return 0;
					      state._ax = i2 & 0xffff;
					      state._dx = i % (short)uns;
					      eip += inst_len + 2; break;
			   }
			   break;

		case 0xf8: /* clc */
			   state._eflags &= ~CF;
			   eip++; break;;

		case 0xf9: /* stc */
			   state._eflags |= CF;
			   eip++; break;;

			   /* 0xfa cli 0xfb sti */

		case 0xfc: /* cld */
			   state._eflags &= ~DF;
			   loop_inc = 1;
			   eip++; break;;

		case 0xfd: /* std */
			   state._eflags |= DF;
			   loop_inc = -1;
			   eip++; break;;

		case 0xfe: /* inc/dec ptr */
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   uc = instr_read_byte(ptr);
			   switch (*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) {
				   case 0x00:
					   state._eflags &= ~(OF|ZF|SF|PF|AF);
					   OPandFLAG0(flags, incb, uc, =q);
					   state._eflags |= flags & (OF|ZF|SF|PF|AF);
					   instr_write_byte(ptr, uc);
					   eip += inst_len + 2; break;
				   case 0x08:
					   state._eflags &= ~(OF|ZF|SF|PF|AF);
					   OPandFLAG0(flags, decb, uc, =q);
					   state._eflags |= flags & (OF|ZF|SF|PF|AF);
					   instr_write_byte(ptr, uc);
					   eip += inst_len + 2; break;
				   default:
					   return 0;
			   }
			   break;

		case 0xff:
			   if (x86->operand_size == 4) return 0;
			   ptr = x86->modrm(MEM_BASE32(cs + eip), x86, &inst_len);
			   uns = instr_read_word(ptr);
			   switch (*(unsigned char *)MEM_BASE32(cs + eip + 1)&0x38) {
				   case 0x00: /* inc */
					   state._eflags &= ~(OF|ZF|SF|PF|AF);
					   OPandFLAG0(flags, incw, uns, =r);
					   state._eflags |= flags & (OF|ZF|SF|PF|AF);
					   instr_write_word(ptr, uns);
					   eip += inst_len + 2; break;
				   case 0x08: /* dec */
					   state._eflags &= ~(OF|ZF|SF|PF|AF);
					   OPandFLAG0(flags, decw, uns, =r);
					   state._eflags |= flags & (OF|ZF|SF|PF|AF);
					   instr_write_word(ptr, uns);
					   eip += inst_len + 2; break;;
				   case 0x10: /*call near*/
					   push(eip + inst_len + 2, x86);
					   eip = uns;
					   break;

				   case 0x18: /*call far*/
					   if (pmode || x86->operand_size == 4)
						   return 0;
					   else {
						   push(state._cs.selector, x86);
						   state._cs.selector = instr_read_word(ptr+2);
						   push(eip + inst_len + 2, x86);
						   state._cs.base = state._cs.selector << 4;
						   eip = uns;
						   cs = state._cs.base;
					   }
					   break;

				   case 0x20: /*jmp near*/
					   eip = uns;
					   break;

				   case 0x28: /*jmp far*/
					   if (pmode || x86->operand_size == 4)
						   return 0;
					   else {
						   state._cs.selector = instr_read_word(ptr+2);
						   state._cs.base = state._cs.selector << 4;
						   eip = uns;
						   cs = state._cs.base;
					   }
					   break;

				   case 0x30: /*push*/
					   push(uns, x86);
					   eip += inst_len + 2; break;
				   default:
					   return 0;
			   }
			   break;

		default:		/* First byte doesn't match anything */
			   return 0;
	}	/* switch (cs[eip]) */

	eip &= wordmask[(state._cs.operand_size + 1) * 2];
	state._eip = eip;

#ifdef ENABLE_DEBUG_TRACE
	dump_x86_regs();
#endif

	return 1;
}

static int instr_emu(int cnt)
{
#ifdef ENABLE_DEBUG_LOG
	unsigned int ref;
	int refseg, rc;
	unsigned char frmtbuf[256];
	instr_deb("vga_emu: entry %04x:%08x\n", state._cs.selector, state._eip);
	dump_x86_regs();
#endif
	int i = 0;
	x86_emustate x86;
	count = cnt ? : COUNT + 1;
	x86.prefixes = 1;

	do {
		if (!instr_sim(&x86, !(state._eflags & (1 << 17)) && (state._cr0 & 1))) {
#ifdef ENABLE_DEBUG_LOG
			uint32 cp = state._cs.base + state._eip;
			unsigned int ref;
			char frmtbuf[256];
			instr_deb("vga_emu: %u bytes not simulated %d: fault addr=%08x\n",
					instr_len(MEM_BASE32(cp), state._cs.operand_size), count, state._cr2);
			dump_x86_regs();
#endif
			break;
		}
		i++;
		//if (!cnt && signal_pending())
		//	break;
	} while (--count > 0);

#ifdef ENABLE_DEBUG_LOG
	instr_deb("simulated %i, left %i\n", i, count);
#endif
	if (i == 0) /* really an unknown instruction from the beginning */
		return False;

	return True;
}

