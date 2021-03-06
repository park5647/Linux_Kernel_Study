/*
 *  linux/arch/arm/kernel/setup.c
 *
 *  Copyright (C) 1995-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/utsname.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/bootmem.h>
#include <linux/seq_file.h>
#include <linux/screen_info.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/of_fdt.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/sort.h>

#include <asm/unified.h>
#include <asm/cp15.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/elf.h>
#include <asm/procinfo.h>
#include <asm/psci.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/mach-types.h>
#include <asm/cacheflush.h>
#include <asm/cachetype.h>
#include <asm/tlbflush.h>

#include <asm/prom.h>
#include <asm/mach/arch.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>
#include <asm/system_info.h>
#include <asm/system_misc.h>
#include <asm/traps.h>
#include <asm/unwind.h>
#include <asm/memblock.h>
#include <asm/virt.h>

#include "atags.h"


#if defined(CONFIG_FPE_NWFPE) || defined(CONFIG_FPE_FASTFPE)
char fpe_type[8];

static int __init fpe_setup(char *line)
{
	memcpy(fpe_type, line, 8);
	return 1;
}

__setup("fpe=", fpe_setup);
#endif

extern void init_default_cache_policy(unsigned long);
extern void paging_init(const struct machine_desc *desc);
extern void early_paging_init(const struct machine_desc *,
			      struct proc_info_list *);
extern void sanity_check_meminfo(void);
extern enum reboot_mode reboot_mode;
extern void setup_dma_zone(const struct machine_desc *desc);

unsigned int processor_id;
EXPORT_SYMBOL(processor_id);
unsigned int __machine_arch_type __read_mostly;
EXPORT_SYMBOL(__machine_arch_type);
unsigned int cacheid __read_mostly;
EXPORT_SYMBOL(cacheid);

unsigned int __atags_pointer __initdata;

unsigned int system_rev;
EXPORT_SYMBOL(system_rev);

unsigned int system_serial_low;
EXPORT_SYMBOL(system_serial_low);

unsigned int system_serial_high;
EXPORT_SYMBOL(system_serial_high);

unsigned int elf_hwcap __read_mostly;
EXPORT_SYMBOL(elf_hwcap);

unsigned int elf_hwcap2 __read_mostly;
EXPORT_SYMBOL(elf_hwcap2);


#ifdef MULTI_CPU

/* IAMROOT-12A:
 * ------------
 * MULTI_CPU로 정의된 아키텍처들은 아래의 전역변수에 
 * 각종 구조체가 연결되어 있다.
 */

struct processor processor __read_mostly;
#endif
#ifdef MULTI_TLB
struct cpu_tlb_fns cpu_tlb __read_mostly;
#endif
#ifdef MULTI_USER
struct cpu_user_fns cpu_user __read_mostly;
#endif
#ifdef MULTI_CACHE
struct cpu_cache_fns cpu_cache __read_mostly;
#endif
#ifdef CONFIG_OUTER_CACHE

/* IAMROOT-12AB:
 * -------------
 * outer cache가 사용되는 경우 아래 전역 outer_cache를 사용한다.
 */
struct outer_cache_fns outer_cache __read_mostly;
EXPORT_SYMBOL(outer_cache);
#endif

/*
 * Cached cpu_architecture() result for use by assembler code.
 * C code should use the cpu_architecture() function instead of accessing this
 * variable directly.
 */

/* IAMROOT-12AB:
 * -------------
 * rpi2: CPU_ARCH_ARMv7(9)
 */
int __cpu_architecture __read_mostly = CPU_ARCH_UNKNOWN;

/* IAMROOT-12A:
 * ------------
 * 각 CPU 모드용 스택 저장 장소이다.
 * 배열이 3인 이유?
 *	- 각 exception 모드에서 3개의 레지스터를 보관할 때 사용한다.
 *	   (r0, lr, spsr)
 */
struct stack {
	u32 irq[3];
	u32 abt[3];
	u32 und[3];
	u32 fiq[3];
} ____cacheline_aligned;

#ifndef CONFIG_CPU_V7M
static struct stack stacks[NR_CPUS];
#endif

char elf_platform[ELF_PLATFORM_SIZE];
EXPORT_SYMBOL(elf_platform);

static const char *cpu_name;
static const char *machine_name;
static char __initdata cmd_line[COMMAND_LINE_SIZE];
const struct machine_desc *machine_desc __initdata;

static union { char c[4]; unsigned long l; } endian_test __initdata = { { 'l', '?', '?', 'b' } };
#define ENDIANNESS ((char)endian_test.l)

DEFINE_PER_CPU(struct cpuinfo_arm, cpu_data);

/*
 * Standard memory resources
 */
static struct resource mem_res[] = {
	{
		.name = "Video RAM",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_MEM
	}
};

#define video_ram   mem_res[0]
#define kernel_code mem_res[1]
#define kernel_data mem_res[2]

static struct resource io_res[] = {
	{
		.name = "reserved",
		.start = 0x3bc,
		.end = 0x3be,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	},
	{
		.name = "reserved",
		.start = 0x378,
		.end = 0x37f,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	},
	{
		.name = "reserved",
		.start = 0x278,
		.end = 0x27f,
		.flags = IORESOURCE_IO | IORESOURCE_BUSY
	}
};

#define lp0 io_res[0]
#define lp1 io_res[1]
#define lp2 io_res[2]

static const char *proc_arch[] = {
	"undefined/unknown",
	"3",
	"4",
	"4T",
	"5",
	"5T",
	"5TE",
	"5TEJ",
	"6TEJ",
	"7",
	"7M",
	"?(12)",
	"?(13)",
	"?(14)",
	"?(15)",
	"?(16)",
	"?(17)",
};

#ifdef CONFIG_CPU_V7M
static int __get_cpu_architecture(void)
{
	return CPU_ARCH_ARMv7M;
}
#else

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2: cpu_arch = CPU_ARCH_ARMv7(9)
 */

static int __get_cpu_architecture(void)
{
	int cpu_arch;

	if ((read_cpuid_id() & 0x0008f000) == 0) {
		cpu_arch = CPU_ARCH_UNKNOWN;
	} else if ((read_cpuid_id() & 0x0008f000) == 0x00007000) {
		cpu_arch = (read_cpuid_id() & (1 << 23)) ? CPU_ARCH_ARMv4T : CPU_ARCH_ARMv3;
	} else if ((read_cpuid_id() & 0x00080000) == 0x00000000) {
		cpu_arch = (read_cpuid_id() >> 16) & 7;
		if (cpu_arch)
			cpu_arch += CPU_ARCH_ARMv3;
	} else if ((read_cpuid_id() & 0x000f0000) == 0x000f0000) {
		/* Revised CPUID format. Read the Memory Model Feature
		 * Register 0 and check for VMSAv7 or PMSAv7 */
		unsigned int mmfr0 = read_cpuid_ext(CPUID_EXT_MMFR0);
		if ((mmfr0 & 0x0000000f) >= 0x00000003 ||
		    (mmfr0 & 0x000000f0) >= 0x00000030)
			cpu_arch = CPU_ARCH_ARMv7;
		else if ((mmfr0 & 0x0000000f) == 0x00000002 ||
			 (mmfr0 & 0x000000f0) == 0x00000020)
			cpu_arch = CPU_ARCH_ARMv6;
		else
			cpu_arch = CPU_ARCH_UNKNOWN;
	} else
		cpu_arch = CPU_ARCH_UNKNOWN;

	return cpu_arch;
}
#endif

/* IAMROOT-12AB:
 * -------------
 * rpi2: CPU_ARCH_ARMv7(9)
 */
int __pure cpu_architecture(void)
{
	BUG_ON(__cpu_architecture == CPU_ARCH_UNKNOWN);

	return __cpu_architecture;
}

static int cpu_has_aliasing_icache(unsigned int arch)
{
	int aliasing_icache;
	unsigned int id_reg, num_sets, line_size;

/* IAMROOT-12A:
 * ------------
 * aliasing은 VIPT에서만 사용. (page coloring)
 */

	/* PIPT caches never alias. */
	if (icache_is_pipt())
		return 0;

	/* arch specifies the register format */
	switch (arch) {
	case CPU_ARCH_ARMv7:

/* IAMROOT-12A:
 * ------------
 * L1 i-cache를 선택 (CSSELR)
 * 현재 캐시 속성을 id_reg에 담는다. (CCSIDR)
 *
 * 라즈베리파이2:
 *	L1 Cache:	I-32KB(TCM, VIPT, 2 way, 32B cache line), 
 *			D-32KB(PIPT, 4 way, 64B cache line)
 *      CCSIDR:		0x203FE009
 *	line_size =	4 * (2 ^ 3) = 32 bytes
 *	num_sets =	0x200 (512)
 *	aliasing_icache = (32 * 512) > 4K = 1 (true)
 */

		asm("mcr	p15, 2, %0, c0, c0, 0 @ set CSSELR"
		    : /* No output operands */
		    : "r" (1));
		isb();
		asm("mrc	p15, 1, %0, c0, c0, 0 @ read CCSIDR"
		    : "=r" (id_reg));
		line_size = 4 << ((id_reg & 0x7) + 2);
		num_sets = ((id_reg >> 13) & 0x7fff) + 1;

/* IAMROOT-12A:
 * ------------
 * aliasing이 필요한 경우는 캐시 단면의 사이즈가 
 * 한 개의 페이지 사이즈(4K)를 초과하는 경우 aliasing이 필요
 */
		aliasing_icache = (line_size * num_sets) > PAGE_SIZE;
		break;
	case CPU_ARCH_ARMv6:
		aliasing_icache = read_cpuid_cachetype() & (1 << 11);
		break;
	default:
		/* I-cache aliases will be handled by D-cache aliasing code */
		aliasing_icache = 0;
	}

	return aliasing_icache;
}

static void __init cacheid_init(void)
{
	unsigned int arch = cpu_architecture();

	if (arch == CPU_ARCH_ARMv7M) {
		cacheid = 0;
	} else if (arch >= CPU_ARCH_ARMv6) {

/* IAMROOT-12A:
 * ------------
 * cachetype = CTR(Cache Type Register)
 * CTR.Format을 읽어 0x4 인경우 ARMv7이다.
 *	- L1 d-cache를 CACHEID_VIPT_NONALIASING로 설정한다.
 *	  (참고로 ARMv7의 실제 L1 d-cache 타입은 PIPT)
 * CTR.L1IP(L1 instruction cache policy)
 *	값이 2인 경우는 i-cache 관련하여 VIPT
 */

		unsigned int cachetype = read_cpuid_cachetype();
		if ((cachetype & (7 << 29)) == 4 << 29) {
			/* ARMv7 register format */
			arch = CPU_ARCH_ARMv7;
			cacheid = CACHEID_VIPT_NONALIASING;
			switch (cachetype & (3 << 14)) {
			case (1 << 14):
				cacheid |= CACHEID_ASID_TAGGED;
				break;
			case (3 << 14):
				cacheid |= CACHEID_PIPT;
				break;
			}
		} else {
			arch = CPU_ARCH_ARMv6;
			if (cachetype & (1 << 23))
				cacheid = CACHEID_VIPT_ALIASING;
			else
				cacheid = CACHEID_VIPT_NONALIASING;
		}

/* IAMROOT-12A:
 * ------------
 * L1 i-cache에 대하여 aliasing이 필요한지 정한다.
 * 캐시의 단면(1 way, 라인사이즈 * numsets) 사이즈가 페이지 크기(4KB) 보다
 * 큰 경우 aliasing(page coloring)이 필요하다고 판단한다.
 *
 * 라즈베리파이2:
 *	L1 i-cache의 타입은 CACHEID_VIPT_I_ALIASING
 *	L1 cacahe의 타입은 d-cache + i-cache 플래그들의 특성을 담는다.
 *		CACHEID_VIPT_NONALIASING(b1) | CACHEID_VIPT_I_ALIASING(b4)
 *		cacheid = 0x12
 *
 *	 출력메시지:
 *		"CPU: PIPT / VIPT nonaliasing data cache, VIPT aliasing instruction cache"
 */

		if (cpu_has_aliasing_icache(arch))
			cacheid |= CACHEID_VIPT_I_ALIASING;
	} else {
		cacheid = CACHEID_VIVT;
	}

	pr_info("CPU: %s data cache, %s instruction cache\n",
		cache_is_vivt() ? "VIVT" :
		cache_is_vipt_aliasing() ? "VIPT aliasing" :
		cache_is_vipt_nonaliasing() ? "PIPT / VIPT nonaliasing" : "unknown",
		cache_is_vivt() ? "VIVT" :
		icache_is_vivt_asid_tagged() ? "VIVT ASID tagged" :
		icache_is_vipt_aliasing() ? "VIPT aliasing" :
		icache_is_pipt() ? "PIPT" :
		cache_is_vipt_nonaliasing() ? "VIPT nonaliasing" : "unknown");
}

/*
 * These functions re-use the assembly code in head.S, which
 * already provide the required functionality.
 */
extern struct proc_info_list *lookup_processor_type(unsigned int);

void __init early_print(const char *str, ...)
{
	extern void printascii(const char *);
	char buf[256];
	va_list ap;

	va_start(ap, str);
	vsnprintf(buf, sizeof(buf), str, ap);
	va_end(ap);

#ifdef CONFIG_DEBUG_LL
	printascii(buf);
#endif
	printk("%s", buf);
}

static void __init cpuid_init_hwcaps(void)
{
	unsigned int divide_instrs, vmsa;

	if (cpu_architecture() < CPU_ARCH_ARMv7)
		return;

/* IAMROOT-12A:
 * ------------
 * ARMv7 부터 HWCAP에 추가 항목 여부에 따라 HWCAP에 설정한다.
 *
 * CPUID_EXT_ISAR0.Device_instrs:   
 *	0=나눗셈명령지원없음, 
 *	1=Thumb에서 SDIV/UDIV 명령 지원 (IDIVT: Instruction DIV for Thumb)
 *	2=ARM에서 SDIV/UDIV 명령 지원   (IDIVA: Instruction DIV for ARM)
 */

	divide_instrs = (read_cpuid_ext(CPUID_EXT_ISAR0) & 0x0f000000) >> 24;

	switch (divide_instrs) {
	case 2:
		elf_hwcap |= HWCAP_IDIVA;
	case 1:
		elf_hwcap |= HWCAP_IDIVT;
	}

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2는 LPAE를 사용하지는 않지만 칩은 지원함.
 */

	/* LPAE implies atomic ldrd/strd instructions */
	vmsa = (read_cpuid_ext(CPUID_EXT_MMFR0) & 0xf) >> 0;
	if (vmsa >= 5)
		elf_hwcap |= HWCAP_LPAE;
}

static void __init elf_hwcap_fixup(void)
{
	unsigned id = read_cpuid_id();
	unsigned sync_prim;

	/*
	 * HWCAP_TLS is available only on 1136 r1p0 and later,
	 * see also kuser_get_tls_init.
	 */

/* IAMROOT-12A:
 * ------------
 * CPU 아키텍처가 ARM1136 r1p0 이상에서는 TLS 기능이 있다.
 * 아래의 루틴같이 ARM1136 r1p0에서 MIDR.variant = 0인 경우 
 * TLS 기능이 없다고 판단하면 뒤 루틴을 생략하고 빠져나간다. 
 */

	if (read_cpuid_part() == ARM_CPU_PART_ARM1136 &&
	    ((id >> 20) & 3) == 0) {
		elf_hwcap &= ~HWCAP_TLS;
		return;
	}

/* IAMROOT-12A:
 * ------------
 * MIDR.architecture가 0xf인 경우 ARMv7이고 아닌 경우는 그 이하.
 * SWP를 제거할 필요가 있는 경우는 ARMv7 부터이다.
 * ARMv7에서는 SWP를 대체할 수 있는 LDREX/STREX를 사용한다.
 */

	/* Verify if CPUID scheme is implemented */
	if ((id & 0x000f0000) != 0x000f0000)
		return;

	/*
	 * If the CPU supports LDREX/STREX and LDREXB/STREXB,
	 * avoid advertising SWP; it may not be atomic with
	 * multiprocessing cores.
	 */
	sync_prim = ((read_cpuid_ext(CPUID_EXT_ISAR3) >> 8) & 0xf0) |
		    ((read_cpuid_ext(CPUID_EXT_ISAR4) >> 20) & 0x0f);
	if (sync_prim >= 0x13)
		elf_hwcap &= ~HWCAP_SWP;
}

/*
 * cpu_init - initialise one CPU.
 *
 * cpu_init sets up the per-CPU stacks.
 */
void notrace cpu_init(void)
{
#ifndef CONFIG_CPU_V7M
	unsigned int cpu = smp_processor_id();

/* IAMROOT-12A:
 * ------------
 * 4가지 모드에 대한 스택은 CPU 별로 필요하다.
 */
	struct stack *stk = &stacks[cpu];

	if (cpu >= NR_CPUS) {
		pr_crit("CPU%u: bad primary CPU number\n", cpu);
		BUG();
	}

	/*
	 * This only works on resume and secondary cores. For booting on the
	 * boot cpu, smp_prepare_boot_cpu is called after percpu area setup.
	 */

/* IAMROOT-12A:
 * ------------
 * TODO: per_cpu_offset() 함수 분석 필요
 */
	set_my_cpu_offset(per_cpu_offset(cpu));

/* IAMROOT-12A:
 * ------------
 * 특정 CPU 아키텍처에서 필요한 코드를 실행한다.
 *	v7 아케텍처에서는 아무것도 하지 않고 그냥 return 한다.
 *
 * 라즈베리파이2:
 *	MULTI_CPU가 동작 중이어서 processor._proc_init()를 호출하는데 
 *	이 변수에는 cpu_v7_proc_init의 주소가 담김 (../mm/proc-v7.S)
 */
	cpu_proc_init();

	/*
	 * Define the placement constraint for the inline asm directive below.
	 * In Thumb-2, msr with an immediate value is not allowed.
	 */
#ifdef CONFIG_THUMB2_KERNEL
#define PLC	"r"
#else
#define PLC	"I"
#endif

	/*
	 * setup stacks for re-entrant exception handlers
	 */

/* IAMROOT-12A:
 * ------------
 * 1) IRQ_MODE에 대한 스택 설정
 *    irq, fiq 비트 매스크한 상태에서 IRQ_MODE 진입
 *    stack 구조체의 irq[0]를 sp에 설정.
 * 2) 순서대로 ABT_MODE, UND_MODE, FIQ_MODE에 대한 스택 설정.
 * 3) 마지막으로 다시 SVC_MODE로 돌아온다.
 */
	__asm__ (
	"msr	cpsr_c, %1\n\t"
	"add	r14, %0, %2\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %3\n\t"
	"add	r14, %0, %4\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %5\n\t"
	"add	r14, %0, %6\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %7\n\t"
	"add	r14, %0, %8\n\t"
	"mov	sp, r14\n\t"
	"msr	cpsr_c, %9"
	    :
	    : "r" (stk),
	      PLC (PSR_F_BIT | PSR_I_BIT | IRQ_MODE),
	      "I" (offsetof(struct stack, irq[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | ABT_MODE),
	      "I" (offsetof(struct stack, abt[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | UND_MODE),
	      "I" (offsetof(struct stack, und[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | FIQ_MODE),
	      "I" (offsetof(struct stack, fiq[0])),
	      PLC (PSR_F_BIT | PSR_I_BIT | SVC_MODE)
	    : "r14");
#endif
}


/* IAMROOT-12A:
 * ------------
 * Designated Initializers 라고 불리는 배열 초기화 방법.
 * 참고: http://gcc.gnu.org/onlinedocs/gcc/Designated-Inits.html
 *
 * NR_CPUS: 가능한 CPU의 최대 개수이고 configuration 할 때 정해지는 값.
 *          2 ~ 32의 값을 가질 수 있다.
 * MPIDR_INVALID: 하위 세 바이트는 모두 0이고 상위 바이트는 0xFF인 값.
 *
 * 따라서 __cpu_logical_map은 모두 invalid mpidr 값을 가지도록 초기화된다.
 */
u32 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = MPIDR_INVALID };

void __init smp_setup_processor_id(void)
{
	int i;

/* IAMROOT-12A:
 * ------------
 * MPIDR의 하위 3바이트(Aff0, Aff1, Aff2)를 가져오기 위해 비트마스킹을 한다.
 * CPU Affinity는 여러 개의 CPU core 중에서 각각의 cpu가 가지는 고유 번호같은 것.
 * Affinity는 계층적으로 표현된다.
 * MPIDR은 현재 동작하는 cpu의 affinity값을 담고 있으며
 * 자세한 형태는 매뉴얼을 참고할 것.
 *
 * 예시(각각의 affinity level의 의미는 아키텍처 구현마다 다름):
 *   Cluster 0                     Cluster 1                  <- Aff1
 *   -------------------------     -------------------------
 *  |  ___   ___   ___   ___  |   |  ___   ___   ___   ___  |
 *  | | 0 | | 1 | | 2 | | 3 | |   | | 0 | | 1 | | 2 | | 3 | | <- Aff0
 *  |  ---   ---   ---   ---  |   |  ---   ---   ---   ---  |
 *   -------------------------     -------------------------
 */
	u32 mpidr = is_smp() ? read_cpuid_mpidr() & MPIDR_HWID_BITMASK : 0;
	u32 cpu = MPIDR_AFFINITY_LEVEL(mpidr, 0);

/* IAMROOT-12A:
 * ------------
 * cpu_logical_map(0) = cpu <=> __cpu_logical_map[0] = cpu
 *
 * 이 코드가 동작할 시에는 nr_cpu_ids는 NR_CPUS와 값이 같다.
 * SMP가 아닌 경우에는 1이 될 것이다.
 *
 * cpu_logical_map은 커널이 사용할 cpu의 logical 번호를 physical cpu 번호로 매핑한다.
 * 이 때 부팅 cpu를 항상 logical cpu 0번으로 지정한다.
 *
 * 부팅 cpu가 physical cpu 2번이라면 physical cpu 0번은 logical cpu 2번이 된다.
 * 나머지는 physical 번호와 logical 번호가 같은 값을 가진다.
 */
	cpu_logical_map(0) = cpu;
	for (i = 1; i < nr_cpu_ids; ++i)
		cpu_logical_map(i) = i == cpu ? 0 : i;

	/*
	 * clear __my_cpu_offset on boot CPU to avoid hang caused by
	 * using percpu variable early, for example, lockdep will
	 * access percpu variable inside lock_release
	 */

/* IAMROOT-12AB:
 * -------------
 * 이 레지스터는 현재 원래 목적으로 사용되지 않고 per-cpu의 offset 저장용으로
 * 사용되므로 일단 0으로 초기화한다.
 */
	set_my_cpu_offset(0);


/* IAMROOT-12A:
 * ------------
 * printk류의 함수는 일단 분석하지 않고 넘어가기로 함.
 * 무서움...
 */
	pr_info("Booting Linux on physical CPU 0x%x\n", mpidr);
}

struct mpidr_hash mpidr_hash;
#ifdef CONFIG_SMP
/**
 * smp_build_mpidr_hash - Pre-compute shifts required at each affinity
 *			  level in order to build a linear index from an
 *			  MPIDR value. Resulting algorithm is a collision
 *			  free hash carried out through shifting and ORing
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity;
	u32 fs[3], bits[3], ls, mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */

/* IAMROOT-12AB:
 * -------------
 * cpu_logical_map 비트에서 모두 중복된 비트를 제거한 값을 산출한다.
 * 예) rpi2: 0xf00, 0xf01, 0xf02, 0xf03
 *           0b1111_0000_0000	0b1111_0000_0001   0b1111_0000_0010   0b1111_0000_0011
 *   mask -> 0b0000_0000_0000_0000_0000_0000_0000_0011 (0x0000_0003)
 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits 0x%x\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */

/* IAMROOT-12AB:
 * -------------
 *			aff-0	aff-1	aff-2
 * rpi2: affinity 값이	  3,	  0,      0	으로 변환한다.
 *	 ls 값이	  2,      0,      0	으로 변환한다.
 *	 fs[] 값이	  0,      0,      0	으로 설정된다.
 *	 bits[] 값이	  2,      0,      0	으로 설정된다.
 */

	for (i = 0; i < 3; i++) {
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		ls = fls(affinity);
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		bits[i] = ls - fs[i];
	}
	/*
	 * An index can be created from the MPIDR by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 24 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR[7:0] = {0x2, 0x80}.
	 */

/* IAMROOT-12AB:
 * -------------
 * mpidr_hash.shift_aff[]
 *	cpu 번호 -> mpidr 값으로 변환하기 위해 각 affnity 필드로 쉬프트해야할 
 *	비트 수를 미리 계산해놓는다.
 *
 *	예) mask = 0x00000000_00000011_00000011_00000011 (max 64 cpus)
 *	    bits[0]=2, bits[1]=2, bits[2]=2
 *	    shift_aff[0] = 0
 *	    shift_aff[1] = 6
 *	    shift_aff[2] = 12
 */
	mpidr_hash.shift_aff[0] = fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_BITS + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = 2*MPIDR_LEVEL_BITS + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] mask[0x%x] bits[%u]\n",
				mpidr_hash.shift_aff[0],
				mpidr_hash.shift_aff[1],
				mpidr_hash.shift_aff[2],
				mpidr_hash.mask,
				mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");

	sync_cache_w(&mpidr_hash);
}
#endif

static void __init setup_processor(void)
{
	struct proc_info_list *list;

	/*
	 * locate processor in the list of supported processor
	 * types.  The linker builds this table for us from the
	 * entries in arch/arm/mm/proc-*.S
	 */
	list = lookup_processor_type(read_cpuid_id());
	if (!list) {
		pr_err("CPU configuration botched (ID %08x), unable to continue.\n",
		       read_cpuid_id());
		while (1);
	}

/* IAMROOT-12A:
 * ------------
 * 전역 변수 __cpu_architecture에 CPU 아키텍처 번호를 저장한다.
 *
 * 라즈베리파이2: 
 *	__cpu_architecture = CPU_ARCH_ARMv7(9)
 */

	cpu_name = list->cpu_name;

	__cpu_architecture = __get_cpu_architecture();

/* IAMROOT-12A:
 * ------------
 * 전역 변수 processor, cpu_tlb, cpu_user, cpu_cache에 CPU 아키텍처가
 * 제공하는 구조체를 찾아 대입한다.
 *
 * 라즈베리파이2: MULTI_CPU를 사용하므로 아래의 전역변수를 사용한다.
 *	processor	v7_processor_functions	(../mm/proc-v7.S)
 *	cpu_tlb_fns	v7_tlb_process_		(../mm/tlb-v7.S)
 *	cpu_user_fns	v6_user_fns		(../mm/copypage-v6.c)
 *	cpu_cache_fns	v7_cache_fns		(../mm/cache-v7.S)
 *
 * 위의 구조체중 v7_으로 시작되는 구조체들은 ../mm/proc-macros.S에서 정의된다.
 * v6_user_fns만 직접 사용한다.
 */

#ifdef MULTI_CPU
	processor = *list->proc;
#endif
#ifdef MULTI_TLB
	cpu_tlb = *list->tlb;
#endif
#ifdef MULTI_USER
	cpu_user = *list->user;
#endif
#ifdef MULTI_CACHE
	cpu_cache = *list->cache;
#endif

	pr_info("CPU: %s [%08x] revision %d (ARMv%s), cr=%08lx\n",
		cpu_name, read_cpuid_id(), read_cpuid_id() & 15,
		proc_arch[cpu_architecture()], get_cr());

	snprintf(init_utsname()->machine, __NEW_UTS_LEN + 1, "%s%c",
		 list->arch_name, ENDIANNESS);
	snprintf(elf_platform, ELF_PLATFORM_SIZE, "%s%c",
		 list->elf_name, ENDIANNESS);

/* IAMROOT-12A:
 * ------------
 * 전역 변수 elf_hwcap에 하드웨어 지원 캐파(capacity)를 설정한다.
 *
 * 라즈베리파이2: 
 *	- 초기 hwcaps:		HWCAP_SWP | HWCAP_HALF | HWCAP_THUMB | 
 *				HWCAP_FAST_MULT | HWCAP_EDSP | HWCAP_TLS
 *	- cpuid_init_hwcaps():	HWCAP_IDIVA 추가. 
 *				HWCAP_LPAE 추가. 
 *	- THUMB 여부에 따라:	HWCAP_THUMB 및 HWCAP_IDIVT 제거. 
 *	- elf_hwcap_fixup():	HWCAP_TLS 보존
 *				HWCAP_SWP 제거.
 * -----------------------------------------------------------------------
 *	HWCAP_HALF | HWCAP_FAST_MULT | HWCAP_EDSP | HWCAP_TLS | HWCPA_IDIVA
 */
	elf_hwcap = list->elf_hwcap;

	cpuid_init_hwcaps();

#ifndef CONFIG_ARM_THUMB
	elf_hwcap &= ~(HWCAP_THUMB | HWCAP_IDIVT);
#endif
#ifdef CONFIG_MMU

/* IAMROOT-12A:
 * ------------
 * 전역 변수 cachepolicy에 pmd 엔트리에 사용할 캐시 정책을 저장한다.
 *
 * 라즈베리파이2:
 *  __cpu_mm_mmu_flags = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | 
 *		PMD_SECT_AP_READ | PMD_SECT_AF | PMD_FLAGS_SMP 
 *		(PMD_FLAGS_SMP = PMD_SECT_WBWA | PMD_SECT_S)
 */
	init_default_cache_policy(list->__cpu_mm_mmu_flags);
#endif
	erratum_a15_798181_init();

	elf_hwcap_fixup();

/* IAMROOT-12A:
 * ------------
 * 라즈베리파이2:
 *	최종 elf_hwcap:
 *		HWCAP_LPAE | HWCAP_IDIVA | HWCAP_TLS | HWCAP_EDSP
 *		HWCAP_FAST_MULT | HWCAP_HALF 
 */

/* IAMROOT-12A:
 * ------------
 * 전역 변수 cacheid에 L1 cache 타입 설정
 */
	cacheid_init();

/* IAMROOT-12A:
 * ------------
 * TPIDRPRW <- per_cpu_offset???
 * IRQ_MODE, ABT_MODE, UND_MODE, FIQ_MODE에 대한 스택 설정.
 */
	cpu_init();
}

void __init dump_machine_table(void)
{
	const struct machine_desc *p;

	early_print("Available machine support:\n\nID (hex)\tNAME\n");
	for_each_machine_desc(p)
		early_print("%08x\t%s\n", p->nr, p->name);

	early_print("\nPlease check your kernel config and/or bootloader.\n");

	while (true)
		/* can't use cpu_relax() here as it may require MMU setup */;
}


/* IAMROOT-12AB:
 * -------------
 * 이 함수를 호출하는 곳:
 *	parse_tag_mem32()
 *	early_mem()
 */
int __init arm_add_memory(u64 start, u64 size)
{
	u64 aligned_start;

	/*
	 * Ensure that start/size are aligned to a page boundary.
	 * Size is rounded down, start is rounded up.
	 */
	aligned_start = PAGE_ALIGN(start);
	if (aligned_start > start + size)
		size = 0;
	else
		size -= aligned_start - start;

#ifndef CONFIG_ARCH_PHYS_ADDR_T_64BIT
	if (aligned_start > ULONG_MAX) {
		pr_crit("Ignoring memory at 0x%08llx outside 32-bit physical address space\n",
			(long long)start);
		return -EINVAL;
	}

	if (aligned_start + size > ULONG_MAX) {
		pr_crit("Truncating memory at 0x%08llx to fit in 32-bit physical address space\n",
			(long long)start);
		/*
		 * To ensure bank->start + bank->size is representable in
		 * 32 bits, we use ULONG_MAX as the upper limit rather than 4GB.
		 * This means we lose a page after masking.
		 */
		size = ULONG_MAX - aligned_start;
	}
#endif

	if (aligned_start < PHYS_OFFSET) {
		if (aligned_start + size <= PHYS_OFFSET) {
			pr_info("Ignoring memory below PHYS_OFFSET: 0x%08llx-0x%08llx\n",
				aligned_start, aligned_start + size);
			return -EINVAL;
		}

		pr_info("Ignoring memory below PHYS_OFFSET: 0x%08llx-0x%08llx\n",
			aligned_start, (u64)PHYS_OFFSET);

		size -= PHYS_OFFSET - aligned_start;
		aligned_start = PHYS_OFFSET;
	}

	start = aligned_start;
	size = size & ~(phys_addr_t)(PAGE_SIZE - 1);

	/*
	 * Check whether this memory region has non-zero size or
	 * invalid node number.
	 */
	if (size == 0)
		return -EINVAL;


/* IAMROOT-12AB:
 * -------------
 * memory memblock에 메모리 영역을 추가한다.
 * (start는 시작물리주소)
 */
	memblock_add(start, size);
	return 0;
}

/*
 * Pick out the memory size.  We look for mem=size@start,
 * where start and size are "size[KkMm]"
 */

static int __init early_mem(char *p)
{
	static int usermem __initdata = 0;
	u64 size;
	u64 start;
	char *endp;

	/*
	 * If the user specifies memory size, we
	 * blow away any automatically generated
	 * size.
	 */
	if (usermem == 0) {
		usermem = 1;
		memblock_remove(memblock_start_of_DRAM(),
			memblock_end_of_DRAM() - memblock_start_of_DRAM());
	}

	start = PHYS_OFFSET;
	size  = memparse(p, &endp);
	if (*endp == '@')
		start = memparse(endp + 1, NULL);

	arm_add_memory(start, size);

	return 0;
}
early_param("mem", early_mem);

static void __init request_standard_resources(const struct machine_desc *mdesc)
{
	struct memblock_region *region;
	struct resource *res;

/* IAMROOT-12AB:
 * -------------
 * 커널 코드와 데이터 영역의 시작과 끝
 */
	kernel_code.start   = virt_to_phys(_text);
	kernel_code.end     = virt_to_phys(_etext - 1);
	kernel_data.start   = virt_to_phys(_sdata);
	kernel_data.end     = virt_to_phys(_end - 1);

	for_each_memblock(memory, region) {

/* IAMROOT-12AB:
 * -------------
 * System RAM을 iomem_resource에 등록
 */
		res = memblock_virt_alloc(sizeof(*res), 0);
		res->name  = "System RAM";
		res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
		res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;

		request_resource(&iomem_resource, res);


/* IAMROOT-12AB:
 * -------------
 * RAM에 있는 커널 코드와 데이터 부분을 System RAM의 sub resource로 등록 
 * (XIP 커널 코드 부분 제외)
 */

		if (kernel_code.start >= res->start &&
		    kernel_code.end <= res->end)
			request_resource(res, &kernel_code);
		if (kernel_data.start >= res->start &&
		    kernel_data.end <= res->end)
			request_resource(res, &kernel_data);
	}


/* IAMROOT-12AB:
 * -------------
 * Video RAM 영역을 iomem_resource에 추가
 */
	if (mdesc->video_start) {
		video_ram.start = mdesc->video_start;
		video_ram.end   = mdesc->video_end;
		request_resource(&iomem_resource, &video_ram);
	}

	/*
	 * Some machines don't have the possibility of ever
	 * possessing lp0, lp1 or lp2
	 */

/* IAMROOT-12AB:
 * -------------
 * 머신에 등록된 포트가 있는 경우 ioport_resource에 추가한다.
 */
	if (mdesc->reserve_lp0)
		request_resource(&ioport_resource, &lp0);
	if (mdesc->reserve_lp1)
		request_resource(&ioport_resource, &lp1);
	if (mdesc->reserve_lp2)
		request_resource(&ioport_resource, &lp2);
}

#if defined(CONFIG_VGA_CONSOLE) || defined(CONFIG_DUMMY_CONSOLE)
struct screen_info screen_info = {
 .orig_video_lines	= 30,
 .orig_video_cols	= 80,
 .orig_video_mode	= 0,
 .orig_video_ega_bx	= 0,
 .orig_video_isVGA	= 1,
 .orig_video_points	= 8
};
#endif

static int __init customize_machine(void)
{
	/*
	 * customizes platform devices, or adds new ones
	 * On DT based machines, we fall back to populating the
	 * machine from the device tree, if no callback is provided,
	 * otherwise we would always need an init_machine callback.
	 */
	of_iommu_init();
	if (machine_desc->init_machine)
		machine_desc->init_machine();
#ifdef CONFIG_OF
	else
		of_platform_populate(NULL, of_default_bus_match_table,
					NULL, NULL);
#endif
	return 0;
}
arch_initcall(customize_machine);

static int __init init_machine_late(void)
{
	if (machine_desc->init_late)
		machine_desc->init_late();
	return 0;
}
late_initcall(init_machine_late);

#ifdef CONFIG_KEXEC
static inline unsigned long long get_total_mem(void)
{
	unsigned long total;

/* IAMROOT-12AB:
 * -------------
 * lowmem 사이즈를 반환한다.
 */
	total = max_low_pfn - min_low_pfn;
	return total << PAGE_SHIFT;
}

/**
 * reserve_crashkernel() - reserves memory are for crash kernel
 *
 * This function reserves memory area given in "crashkernel=" kernel command
 * line parameter. The memory reserved is used by a dump capture kernel when
 * primary kernel is crashing.
 */
static void __init reserve_crashkernel(void)
{
	unsigned long long crash_size, crash_base;
	unsigned long long total_mem;
	int ret;

/* IAMROOT-12AB:
 * -------------
 * lowmem 사이즈를 반환한다.
 */
	total_mem = get_total_mem();

/* IAMROOT-12AB:
 * -------------
 * panic 발생 시 캡쳐전용커널(라이트한 1core용)을 호출해서 coredump를 수행하도록
 * 사이즈와 주소를 parsing 해온다.
 */
	ret = parse_crashkernel(boot_command_line, total_mem,
				&crash_size, &crash_base);
	if (ret)
		return;

/* IAMROOT-12AB:
 * -------------
 * 구해온 size와 base 주소로 crash kernel용 메모리를 할당한다.
 */
	ret = memblock_reserve(crash_base, crash_size);
	if (ret < 0) {
		pr_warn("crashkernel reservation failed - memory is in use (0x%lx)\n",
			(unsigned long)crash_base);
		return;
	}

	pr_info("Reserving %ldMB of memory at %ldMB for crashkernel (System RAM: %ldMB)\n",
		(unsigned long)(crash_size >> 20),
		(unsigned long)(crash_base >> 20),
		(unsigned long)(total_mem >> 20));

	crashk_res.start = crash_base;
	crashk_res.end = crash_base + crash_size - 1;

/* IAMROOT-12AB:
 * -------------
 * iomem_resource에 크래쉬 커널(Crash kernel) 리소스를 등록한다.
 */
	insert_resource(&iomem_resource, &crashk_res);
}
#else
static inline void reserve_crashkernel(void) {}
#endif /* CONFIG_KEXEC */

void __init hyp_mode_check(void)
{
#ifdef CONFIG_ARM_VIRT_EXT

/* IAMROOT-12AB:
 * -------------
 * 전역 __boot_cpu_mode 변수를 모든 cpu에 대해 inner &outer 캐시 동기화
 */
	sync_boot_mode();

	if (is_hyp_mode_available()) {
		pr_info("CPU: All CPU(s) started in HYP mode.\n");
		pr_info("CPU: Virtualization extensions available.\n");
	} else if (is_hyp_mode_mismatched()) {
		pr_warn("CPU: WARNING: CPU(s) started in wrong/inconsistent modes (primary CPU mode 0x%x)\n",
			__boot_cpu_mode & MODE_MASK);
		pr_warn("CPU: This may indicate a broken bootloader or firmware.\n");
	} else
		pr_info("CPU: All CPU(s) started in SVC mode.\n");
#endif
}
void __init setup_arch(char **cmdline_p)
{
	const struct machine_desc *mdesc;

	setup_processor();

/* IAMROOT-12A:
 * ------------
 * 처음에 DT_START_MACHINE에서 등록한 머신 디스크립터 테이블에서 이름으로 
 * 검색해보고 없으면 START_MACHINE에서 등록한 머신 디스크립터 테이블에서
 * 머신 번호로 검색한다.
 *
 * setup_machine_fdt()
 *   - dtb로부터  boot_cmd_line과 memblock에 메모리 노드의 reg 영역을 추가.
 */
	mdesc = setup_machine_fdt(__atags_pointer);
	if (!mdesc)
		mdesc = setup_machine_tags(__atags_pointer, __machine_arch_type);
	machine_desc = mdesc;
	machine_name = mdesc->name;
	dump_stack_set_arch_desc("%s", mdesc->name);

	if (mdesc->reboot_mode != REBOOT_HARD)
		reboot_mode = mdesc->reboot_mode;

	init_mm.start_code = (unsigned long) _text;
	init_mm.end_code   = (unsigned long) _etext;
	init_mm.end_data   = (unsigned long) _edata;
	init_mm.brk	   = (unsigned long) _end;

	/* populate cmd_line too for later use, preserving boot_command_line */
	strlcpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
	*cmdline_p = cmd_line;

/* IAMROOT-12AB:
 * -------------
 * cmdline에서 입력된 모든 파라메터에 대응하는 early 함수를 찾아 호출한다.
 * 일반 파라메터 함수 등록 매크로: __setup()        -> __setup_param(,,0)
 * early 파라메터 함수 등록 매크로: __early_param() -> __setup_param(,,1)
 * earlycon 파라메터 함수 등록 매크로: EARLYCON_DECLARE() -> __early_param() -> ..
 *
 * rpi2:
 *	- setup_of_earlycon()
 *	- pl011_early_console_setup()
 *	- uart_setup_earlycon()
 *	- uart8250_setup_earlycon()
 */
	parse_early_param();

	early_paging_init(mdesc, lookup_processor_type(read_cpuid_id()));
	setup_dma_zone(mdesc);
	sanity_check_meminfo();
	arm_memblock_init(mdesc);

	paging_init(mdesc);
	request_standard_resources(mdesc);

/* IAMROOT-12AB:
 * -------------
 * 머신이 제공하는 restart 함수를 등록한다.
 * 예) rpi2: bcm2709_restart()
 */
	if (mdesc->restart)
		arm_pm_restart = mdesc->restart;

/* IAMROOT-12AB:
 * -------------
 * dtb를 unflatten한 후 메모리를 할당받아 구조체 형태로 변환한다.
 *
 * 전역 of_aliases, of_chosen, of_stdout 노드를 찾고,
 * 전역 aliases_lookup 리스트에 aliases 노드의 모든 속성값으로 찾은 노드들을 
 *	alias_prop 구조체 + stem[] 형태로 변환하여 추가한다.
 */
	unflatten_device_tree();

/* IAMROOT-12AB:
 * -------------
 * dtb의 /cpus의 child 노드에서 읽은 값을 __cpu_logical_map[]에 저장하고 
 * 각 cpu를 possible 상태로 바꾼다.
 * (MPIDR에서 읽은 값이 한 번도 매치되지 않은 경우 에러)
 */
	arm_dt_init_cpu_maps();

/* IAMROOT-12AB:
 * -------------
 * dtb에서 /psci 노드를 사용하는 경우 psci_ops를 설정한다.
 *	- 두 가지 version 지원
 *	  (psci 버전 0.2의 경우 리셋 및 파워 off 전역 함수도 준비한다.)
 * 모드(hvc, smc)에 따라 전역 invoke_psci_fn에 핸들러 함수 설정 
 *	- hvc(HyperVisor Call)
 *	- smc(Secure Monitor Call)
 */
	psci_init();
#ifdef CONFIG_SMP
	if (is_smp()) {

/* IAMROOT-12AB:
 * -------------
 * vexpress: arch/arm/mach-vexpress/platsmp.c 에서 
 *	 mdesc->smp_init에 vexpress_smp_init_ops()가 등록됨
 * rpi2: mdesc->smp_init이 지정되지 않음
 *	 mdesc->smp에 bcm2709_smp_ops이 대입되어 있다.
 */
		if (!mdesc->smp_init || !mdesc->smp_init()) {

/* IAMROOT-12AB:
 * -------------
 * psci_init()을 통해 psci_ops.cpu_on에 핸들러 함수가 있는 경우 true
 *	- ver 0.1: /psci 노드의 cpu_on 속성이 있는 경우에 핸들러 함수 등록
 *	- ver 0.2: /psci 노드가 있기만 하면 핸들러 함수 등록
 */
/* IAMROOT-12AB:
 * -------------
 * psci_ops.cpu_on에 핸들러 함수가 있는 경우 true
 */
			if (psci_smp_available())
				smp_set_ops(&psci_smp_ops);
			else if (mdesc->smp)
				smp_set_ops(mdesc->smp);
		}

/* IAMROOT-12AB:
 * -------------
 * smp_ops에 등록된 smp cpu 초기화 루틴을 동작시킨다.
 * rpi2: smp_ops.smp_init_cpus = bcm2709_smp_init_cpus()
 */
		smp_init_cpus();
		smp_build_mpidr_hash();
	}
#endif

	if (!is_smp())
		hyp_mode_check();

	reserve_crashkernel();

#ifdef CONFIG_MULTI_IRQ_HANDLER

/* IAMROOT-12AB:
 * -------------
 * 인터럽트 핸들러가 호출할 함수
 */
	handle_arch_irq = mdesc->handle_irq;
#endif

#ifdef CONFIG_VT
#if defined(CONFIG_VGA_CONSOLE)
	conswitchp = &vga_con;
#elif defined(CONFIG_DUMMY_CONSOLE)
	conswitchp = &dummy_con;
#endif
#endif

/* IAMROOT-12AB:
 * -------------
 * 머신에 있는 init_early() 함수를 호출한다.
 * rpi2: bcm2709_init_early()를 호출하면 init_dma_coherent_pool_size()를 
 *       호출하여 사이즈를 4M로 설정한다.
 */

	if (mdesc->init_early)
		mdesc->init_early();
}


static int __init topology_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct cpuinfo_arm *cpuinfo = &per_cpu(cpu_data, cpu);
		cpuinfo->cpu.hotpluggable = 1;
		register_cpu(&cpuinfo->cpu, cpu);
	}

	return 0;
}
subsys_initcall(topology_init);

#ifdef CONFIG_HAVE_PROC_CPU
static int __init proc_cpu_init(void)
{
	struct proc_dir_entry *res;

	res = proc_mkdir("cpu", NULL);
	if (!res)
		return -ENOMEM;
	return 0;
}
fs_initcall(proc_cpu_init);
#endif

static const char *hwcap_str[] = {
	"swp",
	"half",
	"thumb",
	"26bit",
	"fastmult",
	"fpa",
	"vfp",
	"edsp",
	"java",
	"iwmmxt",
	"crunch",
	"thumbee",
	"neon",
	"vfpv3",
	"vfpv3d16",
	"tls",
	"vfpv4",
	"idiva",
	"idivt",
	"vfpd32",
	"lpae",
	"evtstrm",
	NULL
};

static const char *hwcap2_str[] = {
	"aes",
	"pmull",
	"sha1",
	"sha2",
	"crc32",
	NULL
};

static int c_show(struct seq_file *m, void *v)
{
	int i, j;
	u32 cpuid;

	for_each_online_cpu(i) {
		/*
		 * glibc reads /proc/cpuinfo to determine the number of
		 * online processors, looking for lines beginning with
		 * "processor".  Give glibc what it expects.
		 */
		seq_printf(m, "processor\t: %d\n", i);
		cpuid = is_smp() ? per_cpu(cpu_data, i).cpuid : read_cpuid_id();
		seq_printf(m, "model name\t: %s rev %d (%s)\n",
			   cpu_name, cpuid & 15, elf_platform);

#if defined(CONFIG_SMP)
		seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
			   per_cpu(cpu_data, i).loops_per_jiffy / (500000UL/HZ),
			   (per_cpu(cpu_data, i).loops_per_jiffy / (5000UL/HZ)) % 100);
#else
		seq_printf(m, "BogoMIPS\t: %lu.%02lu\n",
			   loops_per_jiffy / (500000/HZ),
			   (loops_per_jiffy / (5000/HZ)) % 100);
#endif
		/* dump out the processor features */
		seq_puts(m, "Features\t: ");

		for (j = 0; hwcap_str[j]; j++)
			if (elf_hwcap & (1 << j))
				seq_printf(m, "%s ", hwcap_str[j]);

		for (j = 0; hwcap2_str[j]; j++)
			if (elf_hwcap2 & (1 << j))
				seq_printf(m, "%s ", hwcap2_str[j]);

		seq_printf(m, "\nCPU implementer\t: 0x%02x\n", cpuid >> 24);
		seq_printf(m, "CPU architecture: %s\n",
			   proc_arch[cpu_architecture()]);

		if ((cpuid & 0x0008f000) == 0x00000000) {
			/* pre-ARM7 */
			seq_printf(m, "CPU part\t: %07x\n", cpuid >> 4);
		} else {
			if ((cpuid & 0x0008f000) == 0x00007000) {
				/* ARM7 */
				seq_printf(m, "CPU variant\t: 0x%02x\n",
					   (cpuid >> 16) & 127);
			} else {
				/* post-ARM7 */
				seq_printf(m, "CPU variant\t: 0x%x\n",
					   (cpuid >> 20) & 15);
			}
			seq_printf(m, "CPU part\t: 0x%03x\n",
				   (cpuid >> 4) & 0xfff);
		}
		seq_printf(m, "CPU revision\t: %d\n\n", cpuid & 15);
	}

	seq_printf(m, "Hardware\t: %s\n", machine_name);
	seq_printf(m, "Revision\t: %04x\n", system_rev);
	seq_printf(m, "Serial\t\t: %08x%08x\n",
		   system_serial_high, system_serial_low);

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show
};
