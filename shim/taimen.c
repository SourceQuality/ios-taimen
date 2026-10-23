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
#ifndef WDT_SECS
#define WDT_SECS		120
#endif
#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem-restart_reason; 0x77665500 = fastboot */

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
#ifdef RESET_TO_FASTBOOT
	wr32(IMEM_RESTART_REASON, 0x77665500);
#endif
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
	con_flush();	/* autorefresh is off: one CTL0 START per line */
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t v = rd32(a);
	con_hex2(label, name, (uint32_t)a, "=", v);
	con_flush();
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
	con_flush();
#ifdef DIAG_MIN
	con_str("MIN", "no wdt/mdp access; reset 10s");
	delay_ms(10000);
	psci_reset();
#endif
	con_skip();
	/* 0a. Which ramoops zone survives a reset AND an Android boot in between?
	 * Main console zone 0xb0080000 is Linux's own console (overwritten every
	 * Android boot); the alt region 0xb0e00000 may be left alone. Log both,
	 * dump up to 2 KB of a record we wrote earlier, then plant a new one in the
	 * alt zone. If it survives, the kernel's early log can live there and be
	 * rescued here on the next boot. */
	for (int z = 0; z < 2; z++) {
		uintptr_t base = z ? 0xb0e80000UL : 0xb0080000UL;
		volatile uint32_t *prz = (volatile uint32_t *)base;
		volatile uint8_t *txt = (volatile uint8_t *)(base + 12);
		char snip[49];
		for (int i = 0; i < 48; i++) {
			uint8_t c = txt[i];
			snip[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		snip[48] = 0;
		log_printf("prz @%lx: sig=%08x start=%u size=%u text=\"%s\"\n", (unsigned long)base, prz[0], prz[1], prz[2], snip);
		con_hex2(z ? "PRZalt" : "PRZ", "sig=", prz[0], "size=", prz[2]);
		if (z && prz[0] == 0x43474244u && prz[2] <= 0x100000u - 12u) {
			log_printf("--- alt zone record (%u bytes, first 2 KB):\n", prz[2]);
			for (uint32_t i = 0; i < prz[2] && i < 2048; i++) {
				uint8_t c = txt[i];
				log_printf("%c", (c >= 0x20 && c < 0x7f) || c == '\n' ? (char)c : '.');
			}
			log_printf("\n--- end of alt zone record\n");
		}
	}
	{
		volatile uint32_t *prz = (volatile uint32_t *)0xb0e80000UL;
		volatile uint8_t *txt = (volatile uint8_t *)0xb0e8000cUL;
		static const char msg[] = "TAIMEN SHIM was here (alt zone)\n";
		for (unsigned i = 0; msg[i]; i++)
			txt[i] = (uint8_t)msg[i];
		prz[1] = sizeof(msg) - 1;
		prz[2] = sizeof(msg) - 1;
		prz[0] = 0x43474244u;
		__asm__ volatile("dsb sy" ::: "memory");
	}
	/* 0b. Framebuffer readback: do the previous run's bands (rows 2000+) survive ABL? */
	{
		volatile uint32_t *fb = (volatile uint32_t *)0x9d400000UL;
		unsigned runs = 0, y = 1990;
		while (y < 2880 && runs < 12) {
			uint32_t px = fb[(y * 5760u) / 4u + 720u];
			if ((px & 0xffffffu) != 0) {
				unsigned y0 = y;
				while (y < 2880 && (fb[(y * 5760u) / 4u + 720u] & 0xffffffu) != 0)
					y++;
				log_printf("fb band rows %u-%u color %08x\n", y0, y - 1, px);
				runs++;
			} else {
				y++;
			}
		}
		log_printf("fb readback: %u non-black runs in rows 1990-2879\n", runs);
	}
	/* 0. Does DDR survive a reset through this bootloader? Read markers a previous
	 * run left, then plant new ones: in the ramoops region, in low RAM, and in
	 * the RAM above 4 GB. Compared on the next boot. */
	{
		static const uintptr_t marks[3] = { 0xb0000000UL, 0x8a800000UL, 0x100000000UL };
		for (int i = 0; i < 3; i++) {
			uint64_t v = *(volatile uint64_t *)marks[i];
			log_printf("rammark @%lx = %lx\n", (unsigned long)marks[i], v);
			con_hex2("MARK", "@", (uint32_t)marks[i], "=", (uint32_t)v);
			*(volatile uint64_t *)marks[i] = 0x5441494d454e0000ULL | (uint32_t)(rsr("cntpct_el0") & 0xffff);
		}
		__asm__ volatile("dsb sy" ::: "memory");
	}
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
	con_flush();
	ufs_ok = !ufs_init();
	con_hex2("UFS", "ok=", (uint32_t)ufs_ok, 0, 0);
	save("after probes");
	/* 6. A frame kick (command-mode panel). */
	con_str("KICK", "ctl0 start ..");
	con_flush();
	con_str("KICK", "done");
	log_printf("kick done\n");
	save("after kick");
#ifdef STAGE2
	/*
	 * Stage 2: act as iBoot. The SWEETXNU payload is the boot image's ramdisk,
	 * which ABL loads at /chosen linux,initrd-start (0xa0000000). Arm the APSS
	 * watchdog first so a kernel hang ends in a reset (bite WDT_SECS later)
	 * instead of needing the power button.
	 */
	con_skip();
	con_str("WDT", "arming for xnu");
	wr32(APSS_WDT + WDT_BARK, WDT_SECS * 32768u);
	wr32(APSS_WDT + WDT_BITE, WDT_SECS * 32768u);
	wr32(APSS_WDT + WDT_RST, 1);
	wr32(APSS_WDT + WDT_EN, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	log_printf("wdt armed: %u s\n", (unsigned)WDT_SECS);
	save("before loader");
	con_str("LOADER", "payload from initrd ..");
	con_flush();
	const char *why = loader_boot(dtb);
	log_printf("LOADER: %s\n", why);
	con_str("LOADER", why);
	con_flush();
	save("loader returned");
	con_str("RESET", "in 15s");
	con_flush();
	delay_ms(15000);
	psci_reset();
#else
	con_skip();
	con_str("RESET", "in 15s");
	con_flush();
	delay_ms(15000);
	psci_reset();
#endif
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
	con_flush();
	save("exception");
	delay_ms(15000);
	psci_reset();
}
