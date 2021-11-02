#include <inc/types.h>
#include <inc/assert.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/stdio.h>
#include <inc/x86.h>
#include <inc/uefi.h>
#include <kern/timer.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/trap.h>

#define kilo      (1000ULL)
#define Mega      (kilo * kilo)
#define Giga      (kilo * Mega)
#define Tera      (kilo * Giga)
#define Peta      (kilo * Tera)
#define ULONG_MAX ~0UL

#if LAB <= 6
/* Early variant of memory mapping that does 1:1 aligned area mapping
 * in 2MB pages. You will need to reimplement this code with proper
 * virtual memory mapping in the future. */
void *
mmio_map_region(physaddr_t pa, size_t size) {
    void map_addr_early_boot(uintptr_t addr, uintptr_t addr_phys, size_t sz);
    const physaddr_t base_2mb = 0x200000;
    uintptr_t org = pa;
    size += pa & (base_2mb - 1);
    size += (base_2mb - 1);
    pa &= ~(base_2mb - 1);
    size &= ~(base_2mb - 1);
    map_addr_early_boot(pa, pa, size);
    return (void *)org;
}

void *
mmio_remap_last_region(physaddr_t pa, void *addr, size_t oldsz, size_t newsz) {
    return mmio_map_region(pa, newsz);
}
#endif

struct Timer timertab[MAX_TIMERS];
struct Timer *timer_for_schedule;

struct Timer timer_hpet0 = {
        .timer_name = "hpet0",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim0,
        .handle_interrupts = hpet_handle_interrupts_tim0,
};

struct Timer timer_hpet1 = {
        .timer_name = "hpet1",
        .timer_init = hpet_init,
        .get_cpu_freq = hpet_cpu_frequency,
        .enable_interrupts = hpet_enable_interrupts_tim1,
        .handle_interrupts = hpet_handle_interrupts_tim1,
};

struct Timer timer_acpipm = {
        .timer_name = "pm",
        .timer_init = acpi_enable,
        .get_cpu_freq = pmtimer_cpu_frequency,
};

void
acpi_enable(void) {
    FADT *fadt = get_fadt();
    outb(fadt->SMI_CommandPort, fadt->AcpiEnable);
    while ((inw(fadt->PM1aControlBlock) & 1) == 0) /* nothing */
        ;
}

static void *
acpi_find_table(const char *sign) {
    /*
     * This function performs lookup of ACPI table by its signature
     * and returns valid pointer to the table mapped somewhere.
     *
     * It is a good idea to checksum tables before using them.
     *
     * HINT: Use mmio_map_region/mmio_remap_last_region
     * before accessing table addresses
     * (Why mmio_remap_last_region is requrired?)
     * HINT: RSDP address is stored in uefi_lp->ACPIRoot
     * HINT: You may want to distunguish RSDT/XSDT
     */

    // LAB 5: Your code here
    //169-170 page in ACPI Specification
   
    RSDT *krsdt; 
    size_t krsdt_len, krsdt_size = 4; // 4*n An array of 32-bit physical addresses that point to other DESCRIPTION_HEADERs.

    //Root System Description Table (RSDT)
    RSDP *krsdp = mmio_map_region(uefi_lp->ACPIRoot, sizeof(RSDP));
    uint64_t rsdt_pa = krsdp->RsdtAddress;

    //Extended System Description Table (XSDT)
    if (krsdp->Revision) { // An ACPI-compatible OS must use the XSDT if present. 
        rsdt_pa = krsdp->XsdtAddress;
        krsdt_size = 8; //8*n An array of 64-bit physical addresses that point to other DESCRIPTION_HEADERs.
    }

    krsdt = mmio_map_region(rsdt_pa, sizeof(RSDT));
    //Can define size only after read header
    krsdt = mmio_map_region(rsdt_pa, krsdt->h.Length);
    krsdt_len = (krsdt->h.Length - sizeof(RSDT)) / 4;
    if (krsdp->Revision) { 
        krsdt_len /= 2; 
    }

    ACPISDTHeader *head = NULL;
    for (size_t i = 0; i < krsdt_len; i++) {
        //The Fixed ACPI Description Table (FADT)
        uint64_t pa_fadt = 0;
        memcpy(&pa_fadt, (uint8_t *)krsdt->PointerToOtherSDT + i * krsdt_size, krsdt_size);
        head = mmio_map_region(pa_fadt, sizeof(ACPISDTHeader));
        //Can define size only after read header
        head = mmio_map_region(pa_fadt, head->Length);

        //sign - input value
        if (!strncmp(head->Signature, sign, 4)) { 
            return head; 
        }
    }

    return NULL;
}

/* Obtain and map FADT ACPI table address. */
FADT *
get_fadt(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    // HINT: ACPI table signatures are
    //       not always as their names
    static FADT *kfadt;
    kfadt = acpi_find_table("FACP");
    return kfadt;
}

/* Obtain and map RSDP ACPI table address. */
HPET *
get_hpet(void) {
    // LAB 5: Your code here
    // (use acpi_find_table)
    static HPET *khpet;
    khpet = acpi_find_table("HPET");
    return khpet;
}

/* Getting physical HPET timer address from its table. */
HPETRegister *
hpet_register(void) {
    HPET *hpet_timer = get_hpet();
    if (!hpet_timer->address.address) panic("hpet is unavailable\n");

    uintptr_t paddr = hpet_timer->address.address;
    return mmio_map_region(paddr, sizeof(HPETRegister));
}

/* Debug HPET timer state. */
void
hpet_print_struct(void) {
    HPET *hpet = get_hpet();
    cprintf("signature = %s\n", (hpet->h).Signature);
    cprintf("length = %08x\n", (hpet->h).Length);
    cprintf("revision = %08x\n", (hpet->h).Revision);
    cprintf("checksum = %08x\n", (hpet->h).Checksum);

    cprintf("oem_revision = %08x\n", (hpet->h).OEMRevision);
    cprintf("creator_id = %08x\n", (hpet->h).CreatorID);
    cprintf("creator_revision = %08x\n", (hpet->h).CreatorRevision);

    cprintf("hardware_rev_id = %08x\n", hpet->hardware_rev_id);
    cprintf("comparator_count = %08x\n", hpet->comparator_count);
    cprintf("counter_size = %08x\n", hpet->counter_size);
    cprintf("reserved = %08x\n", hpet->reserved);
    cprintf("legacy_replacement = %08x\n", hpet->legacy_replacement);
    cprintf("pci_vendor_id = %08x\n", hpet->pci_vendor_id);
    cprintf("hpet_number = %08x\n", hpet->hpet_number);
    cprintf("minimum_tick = %08x\n", hpet->minimum_tick);

    cprintf("address_structure:\n");
    cprintf("address_space_id = %08x\n", (hpet->address).address_space_id);
    cprintf("register_bit_width = %08x\n", (hpet->address).register_bit_width);
    cprintf("register_bit_offset = %08x\n", (hpet->address).register_bit_offset);
    cprintf("address = %08lx\n", (unsigned long)(hpet->address).address);
}

static volatile HPETRegister *hpetReg;
/* HPET timer period (in femtoseconds) */
static uint64_t hpetFemto = 0;
/* HPET timer frequency */
static uint64_t hpetFreq = 0;

/* HPET timer initialisation */
void
hpet_init() {
    if (hpetReg == NULL) {
        nmi_disable();
        hpetReg = hpet_register();
        uint64_t cap = hpetReg->GCAP_ID;
        hpetFemto = (uintptr_t)(cap >> 32);
        if (!(cap & HPET_LEG_RT_CAP)) panic("HPET has no LegacyReplacement mode");

        /* cprintf("hpetFemto = %llu\n", hpetFemto); */
        hpetFreq = (1 * Peta) / hpetFemto;
        /* cprintf("HPET: Frequency = %d.%03dMHz\n", (uintptr_t)(hpetFreq / Mega), (uintptr_t)(hpetFreq % Mega)); */
        /* Enable ENABLE_CNF bit to enable timer */
        hpetReg->GEN_CONF |= HPET_ENABLE_CNF;
        nmi_enable();
    }
}

/* HPET register contents debugging. */
void
hpet_print_reg(void) {
    cprintf("GCAP_ID = %016lx\n", (unsigned long)hpetReg->GCAP_ID);
    cprintf("GEN_CONF = %016lx\n", (unsigned long)hpetReg->GEN_CONF);
    cprintf("GINTR_STA = %016lx\n", (unsigned long)hpetReg->GINTR_STA);
    cprintf("MAIN_CNT = %016lx\n", (unsigned long)hpetReg->MAIN_CNT);
    cprintf("TIM0_CONF = %016lx\n", (unsigned long)hpetReg->TIM0_CONF);
    cprintf("TIM0_COMP = %016lx\n", (unsigned long)hpetReg->TIM0_COMP);
    cprintf("TIM0_FSB = %016lx\n", (unsigned long)hpetReg->TIM0_FSB);
    cprintf("TIM1_CONF = %016lx\n", (unsigned long)hpetReg->TIM1_CONF);
    cprintf("TIM1_COMP = %016lx\n", (unsigned long)hpetReg->TIM1_COMP);
    cprintf("TIM1_FSB = %016lx\n", (unsigned long)hpetReg->TIM1_FSB);
    cprintf("TIM2_CONF = %016lx\n", (unsigned long)hpetReg->TIM2_CONF);
    cprintf("TIM2_COMP = %016lx\n", (unsigned long)hpetReg->TIM2_COMP);
    cprintf("TIM2_FSB = %016lx\n", (unsigned long)hpetReg->TIM2_FSB);
}

/* HPET main timer counter value. */
uint64_t
hpet_get_main_cnt(void) {
    return hpetReg->MAIN_CNT;
}

/* - Configure HPET timer 0 to trigger every 0.5 seconds on IRQ_TIMER line
 * - Configure HPET timer 1 to trigger every 1.5 seconds on IRQ_CLOCK line
 *
 * HINT To be able to use HPET as PIT replacement consult
 *      LegacyReplacement functionality in HPET spec.
 * HINT Don't forget to unmask interrupt in PIC */


// -----------------LAB 5----------------------//
//Timer N Configuration and Capabilities Register. 16-17 page in IA-PC HPET
//Number of bits
uint64_t TN_INT_TYPE_CNF = 0b1; //1st bit
uint64_t TN_INT_ENB_CNF = 0b10; //2nd bit
uint64_t TN_TYPE_CNF = 0b100; //3rd bit
uint64_t TN_VAL_SET_CNF = 0b100000; //6th bit

void
hpet_enable_interrupts_tim0(void) {
    // LAB 5: Your code here
    hpetReg->GEN_CONF |= TN_INT_TYPE_CNF;
    hpetReg->TIM0_CONF = (IRQ_TIMER << 9) | TN_TYPE_CNF | TN_INT_ENB_CNF | TN_VAL_SET_CNF;
    hpetReg->TIM0_COMP = hpet_get_main_cnt() + Peta / 2 / hpetFemto;
    hpetReg->TIM0_COMP = Peta / 2 / hpetFemto;
}

void
hpet_enable_interrupts_tim1(void) {
    // LAB 5: Your code here
    hpetReg->GEN_CONF |= TN_INT_TYPE_CNF;
    hpetReg->TIM1_CONF = (IRQ_CLOCK << 9) | TN_TYPE_CNF | TN_INT_ENB_CNF | TN_VAL_SET_CNF;
    hpetReg->TIM1_COMP = hpet_get_main_cnt() + Peta * 3/2 / hpetFemto;
    hpetReg->TIM1_COMP = Peta * 3/2 / hpetFemto;
}

void
hpet_handle_interrupts_tim0(void) {
    pic_send_eoi(IRQ_TIMER);
}

void
hpet_handle_interrupts_tim1(void) {
    pic_send_eoi(IRQ_CLOCK);
}

/* Calculate CPU frequency in Hz with the help with HPET timer.
 * HINT Use hpet_get_main_cnt function and do not forget about
 * about pause instruction. */
uint64_t
hpet_cpu_frequency(void) {
    static uint64_t cpu_freq;
    // LAB 5: Your code here
    uint64_t time_res = 100, target = hpetFreq / time_res, delta;

    uint64_t hpet_start = hpet_get_main_cnt();
    uint64_t tsc_start = read_tsc();
    do {
        asm("pause");
        delta = hpet_get_main_cnt() - hpet_start;
    } while (delta < target);

    cpu_freq = (read_tsc() - tsc_start) * time_res;
    return cpu_freq;
}

uint32_t
pmtimer_get_timeval(void) {
    FADT *fadt = get_fadt();
    return inl(fadt->PMTimerBlock);
}

/* Calculate CPU frequency in Hz with the help with ACPI PowerManagement timer.
 * HINT Use pmtimer_get_timeval function and do not forget that ACPI PM timer
 * can be 24-bit or 32-bit. */
uint64_t
pmtimer_cpu_frequency(void) {
    static uint64_t cpu_freq;
    // LAB 5: Your code here
    uint32_t time_res = 100, target = PM_FREQ / time_res, delta;

    uint32_t pm_start = pmtimer_get_timeval();
    uint64_t tsc_start = read_tsc();

    do {
        asm("pause");
        uint32_t pm_curr = pmtimer_get_timeval();
        delta = pmtimer_get_timeval() - pm_start;
        if (-delta <= 0xFFFFFF) { delta += 0xFFFFFF; } 
        else if (pm_start > pm_curr) { delta += 0xFFFFFFFF; }
    } while (delta < target);

    cpu_freq = (read_tsc() - tsc_start) * PM_FREQ / delta;
    return cpu_freq;
}
