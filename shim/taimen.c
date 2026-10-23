/*
 * Pixel 2 XL (taimen, MSM8998) shim, stage 1: a staged diagnostic. Each step
 * paints a line on the ABL splash framebuffer BEFORE the next register access,
 * so when something hangs the bus (watchdog reset follows) the last line on
 * the screen names the culprit. Everything also goes to a RAM log (log.c)
 * that is written to UFS at the end (ufs_taimen.c, `align_parti` partition)
 * and read back from rooted Android with tools/taimen/log-pull.sh.
 * Order: things known safe, clock status from GCC and MMSS CC (CBCR bit 31 =
 * CLK_OFF), the MDSS blocks, the APSS watchdog write, UFS init + log save,
 * a CTL0 START kick, then PSCI reset.
 *   -DDIAG_MIN: paint + 10 s + PSCI reset only (known good, 2026-10-22).
 * Derived from pixel.c (Pixel 1). Addresses: docs/taimen-porting-map.md.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"
#include "log.h"
#include "ufs_taimen.h"

#define PSCI_SYSTEM_RESET	0x84000009UL
#define APSS_WDT		0x17817000UL	/* qcom,msm-watchdog@17817000, 32768 Hz */
#define WDT_RST			0x04
#define WDT_EN			0x08
#define WDT_BARK		0x10
#define WDT_BITE		0x14

#define GCC_BASE		0x100000UL	/* qcom,gcc@100000 */
#define MMSS_CC_BASE		0xc8c0000UL	/* qcom,mmsscc@c8c0000 */
#define MDSS_BASE		0xc900000UL	/* qcom,mdss_mdp@c900000 */
#define MDP_INTR_STATUS		(MDSS_BASE + 0x1014)
#define CTL0			(MDSS_BASE + 0x2000)	/* qcom,mdss-ctl-off */
#define VIG0			(MDSS_BASE + 0x5000)	/* qcom,mdss-pipe-vig-off */
#define LM0			(MDSS_BASE + 0x45000)	/* qcom,mdss-mixer-intf-off */
#define INTF1			(MDSS_BASE + 0x6b800)	/* qcom,mdss-intf-off[1] (DSI0) */
#define PP0			(MDSS_BASE + 0x71000)	/* qcom,mdss-pingpong-off */
#define DSC0			(MDSS_BASE + 0x81000)	/* qcom,mdss-dsc-off */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void __attribute__((noreturn)) psci_reset(void)
{
	register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
	__asm__ volatile("dsb sy; smc #0" : "+r"(x0) : : "memory");
	for (;;)
		__asm__ volatile("wfe");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

/* Announce the access, do it, then show the value: a hang leaves "name ..." as the last line. */
static uint32_t probe(const char *label, const char *name, uintptr_t a)
{
	con_hex2(label, name, (uint32_t)a, "..", 0);
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t v = rd32(a);
	con_hex2(label, name, (uint32_t)a, "=", v);
	log_printf("%s %s @%08x = %08x\n", label, name, (uint32_t)a, v);
	return v;
}

static int ufs_ok;

static void save(const char *why)
{
	if (!ufs_ok)
		return;
	int rc = ufs_save_log(why);
	con_hex2("UFS", "save rc=", (uint32_t)rc, 0, 0);
}

extern char _end[];

void shim_main(uint64_t dtb)
{
	log_printf("TAIMEN SHIM: stage 1 staged diag\n");
	log_printf("dtb=%lx magic=%08x _end=%p mmfr0=%lx midr=%lx mpidr=%lx el=%lx sctlr=%lx cntfrq=%lu\n",
		   dtb, rd32(dtb), _end, rsr("id_aa64mmfr0_el1"), rsr("midr_el1"), rsr("mpidr_el1"),
		   rsr("CurrentEL"), rsr("sctlr_el1"), rsr("cntfrq_el0"));

	con_clear(0);
	con_str("SHIM", "taimen stage 1 diag");
	con_hex("DTB", dtb);
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex("MIDR", rsr("midr_el1"));
	con_hex("EL", rsr("CurrentEL"));
	con_hex("SCTLR", rsr("sctlr_el1"));
#ifdef DIAG_MIN
	con_str("MIN", "no wdt/mdp access; reset 10s");
	delay_ms(10000);
	psci_reset();
#endif
	con_skip();
	/* 1. GCC is always clocked. 8998 gcc: mmss_sys_noc_axi 0x9000, mmss_noc_cfg_ahb 0x9004 */
	probe("GCC", "mmss_axi", GCC_BASE + 0x9000);
	probe("GCC", "mmss_ahb", GCC_BASE + 0x9004);
	/* 2. MMSS CC (needs the GCC mmss ahb above). 8998 mmcc: mdss_ahb 0x2308, mdss_axi 0x2310, mdp 0x231c */
	probe("MMSS", "mdss_ahb", MMSS_CC_BASE + 0x2308);
	probe("MMSS", "mdss_axi", MMSS_CC_BASE + 0x2310);
	probe("MMSS", "mdss_mdp", MMSS_CC_BASE + 0x231c);
	/* 3. MDSS blocks (need mdss_ahb; the SSPP/CTL side needs mdp too) */
	probe("MDSS", "hw_ver", MDSS_BASE + 0x0);
	probe("MDP", "hw_ver", MDSS_BASE + 0x1000);
	probe("MDP", "intr_st", MDP_INTR_STATUS);
	probe("VIG0", "src_size", VIG0 + 0x00);
	probe("VIG0", "out_xy", VIG0 + 0x10);
	probe("VIG0", "src0_addr", VIG0 + 0x14);
	probe("VIG0", "ystride0", VIG0 + 0x24);
	probe("VIG0", "src_fmt", VIG0 + 0x30);
	probe("VIG0", "unpack", VIG0 + 0x34);
	probe("VIG1", "src0_addr", MDSS_BASE + 0x7000 + 0x14);
	probe("VIG1", "out_xy", MDSS_BASE + 0x7000 + 0x10);
	probe("DMA0", "src0_addr", MDSS_BASE + 0x25000 + 0x14);
	probe("DMA1", "src0_addr", MDSS_BASE + 0x27000 + 0x14);
	probe("CTL0", "layer0", CTL0 + 0x0);
	probe("CTL0", "layer1", CTL0 + 0x4);
	probe("CTL0", "top", CTL0 + 0x14);
	probe("CTL0", "flush", CTL0 + 0x18);
	probe("CTL1", "layer0", CTL0 + 0x200 + 0x0);
	probe("CTL1", "top", CTL0 + 0x200 + 0x14);
	probe("LM0", "out_size", LM0 + 0x0);
	probe("INTF1", "timing_en", INTF1 + 0x0);
	probe("INTF1", "config", INTF1 + 0x4);
	probe("PP0", "tear_en", PP0 + 0x0);
	probe("PP0", "autorefr", PP0 + 0x30);
	probe("PP1", "autorefr", PP0 + 0x800 + 0x30);
	probe("DSC0", "common", DSC0 + 0x0);
	/* 4. The watchdog: read, then disable. */
	probe("WDT", "en", APSS_WDT + WDT_EN);
	probe("WDT", "bark", APSS_WDT + WDT_BARK);
	probe("WDT", "bite", APSS_WDT + WDT_BITE);
	con_str("WDT", "write en=0 ..");
	wr32(APSS_WDT + WDT_EN, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	probe("WDT", "en", APSS_WDT + WDT_EN);
	/* 5. UFS: bring up the host ABL left running and save the log so far. */
	con_str("UFS", "init ..");
	ufs_ok = !ufs_init();
	con_hex2("UFS", "ok=", (uint32_t)ufs_ok, 0, 0);
	save("after probes");
	/* 6. A frame kick (command-mode panel). */
	con_str("KICK", "ctl0 start ..");
	con_flush();
	con_str("KICK", "done");
	log_printf("kick done\n");
	save("after kick");
	con_skip();
	con_str("RESET", "in 15s");
	delay_ms(15000);
	psci_reset();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	log_printf("EXC slot=%lu esr=%lx far=%lx elr=%lx\n", slot, rsr("esr_el1"), rsr("far_el1"), rsr("elr_el1"));
	con_skip();
	con_hex("EXC", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("ELR", rsr("elr_el1"));
	save("exception");
	delay_ms(15000);
	psci_reset();
}
