# 16K-page catalogue for the sailfish/sweet series and the S8 4K changes

Work list for the TAIMEN (4K-page) kernel. Produced 2026-10-22 from the patch series; "verify" marks inferences.


There is no XNU source tree on this machine. Every claim below about upstream xnu-8796 internals is inferred from the patch context and the S8 notes, and is marked "verify" where it matters. Line numbers are `patch:line` inside the .patch file. For S8, `S8 0085:72` means line 72 of that S8 patch file.

## The three issues that decide the rest

1. **Single 16K switch.** `0004:250 +#define __ARM_16K_PG__ 1` in SWEET.h. It is still present in the context lines at `0080:26` and `0081:256`. The comment at `0004:227` says "16K pages and GICv3 like VMAPPLE". Removing it switches ARM_PGSHIFT, PAGE_SHIFT, ARM_TT_L1/L2 shifts, TCR TG0/TG1 and T0SZ/T1SZ_BOOT to their upstream 4K values automatically.
   - Fix: new board header without `__ARM_16K_PG__`, as S8 did with A5XELTE.h (S8 0085:72-88), or delete line 250.
2. **Layout cascade.** The sweet kernel is an ARM_LARGE_MEMORY kernel. Evidence: `0011:15` says "ARM_LARGE_MEMORY configuration used by SWEET's non-PPL kernel", and `0177:58` uses the floor `0xfffffe0000000000`. The MASTER config is MASTER.arm64.MacOSX-derived (`0006:8`), so `XNU_PLATFORM_MacOSX` holds. With 4K the TTBR1 range is 39 bits and the -2 TiB link address `0xfffffe0007004000` cannot be reached.
   - Fix: port S8 0093 (`-UARM_LARGE_MEMORY`, link at `0xfffffff007004000`), S8 0084 (MACH_VM_MAX_ADDRESS) and S8 0097 (zone VA). All three key on MacOSX/LARGE, so they apply here as written, with the guard renamed.
   - Odd detail: `0112:13`'s comment already says kernel PCs are "0xfffffff007…", which contradicts LARGE_MEMORY. Check which link address the current kernel actually has (`nm`).
3. **RAM-size blocker (open, not solved by S8).** S8's 4K kernels only ran with a 512 MiB ADT RAM window. In DREAMLTE_PROGRESS item 1, 2 GiB gave `kmem_range_init: vm_map_locate_space failing for claim …` even with 0097 applied, and they cut RAM to 512 MiB instead of fixing it. Taimen's candidate window is about 2 GiB (porting-map §3).
   - Open work: size the kmem_range_init claims for a 39-bit kernel VA with at least 2 GiB, or start taimen at 512 MiB.

## (A) start.s / early boot / bootstrap page tables

- **0010:123-138, rewritten by 0081:78-99.** The SWEET block in start.s after the KVA `create_bootstrap_mapping`: `MOV64 x10, SWEET_EARLY_LOG_BLOCK`, ARM_TTE_TYPE_BLOCK, then `and x12, x10, #(ARM_TT_L2_INDEX_MASK)`, `lsr x12, x12, #(ARM_TT_L2_SHIFT)`, `add x13, x25, #(2 * PGBYTES)`, `str x11, [x13, x12, lsl #TTE_SHIFT]`. The comment at 0081:85-89 reads "one 32MB Device block … Both it and RAM use L1 index 0 … SWEET uses 16K pages and the RAM is below 64GB, so the first V=P L2 page is bootstrap_pagetables + 2*PGBYTES".
  - Why 16K: an L1 entry covers 64 GB with 16K but 1 GB with 4K, and an L2 block is 32 MB with 16K but 2 MB with 4K. On taimen the log block (0xb0000000, L1 index 2) and kernel RAM (0x100000000, L1 index 4) sit in different L1 slots. The store would go into RAM's L2 table at index 0x180, giving a wrong VA (0x130000000), and VA 0xb0000000 would stay unmapped.
  - Fix: use S8 0089's pattern. Call `create_bootstrap_mapping` VA=PA for one 2 MB block, then rewrite ATTRINDX to Device (CACHE_ATTRINDX_DISABLE). That costs one more bootstrap L2 page, so check that BOOTSTRAP_TABLE_SIZE still has room.
- **0081:94 / 0081:262-263.** `SWEET_EARLY_LOG_BLOCK 0x82000000`, `SWEET_EARLY_LOG_PRZ 0x83540000`.
  - Why 16K: the block is 32 MB-aligned and PRZ sits 0x1540000 into it, so with a 2 MB block PRZ falls outside.
  - Fix: `LOG_BLOCK = PRZ & ~(ARM_TT_L2_SIZE-1)`. For taimen that is block 0xb0000000 and PRZ 0xb0080000; the 1 MiB console ends at 0x180000, inside one 2 MB block.
  - Not a granule issue but on the same lines: `PRZ_DATA_MAX (0x80000u - 12u)` (0081:139) is sailfish's 512K console; taimen's is 1 MiB. NOTES-taimen.md also says RAM at 0xb0080000 does not survive the PSCI reset on taimen.
- **0081:50-72.** Stage 0 with the MMU off writes to the PRZ. Not granule-dependent; the address is board-specific.
- **0065:252-259.** `extern uint8_t bootstrap_pagetables[BOOTSTRAP_TABLE_SIZE]; CleanPoC…`. Generic: the size macro follows the page size.
- **0065:321-399.** Secondary entry (`sweet_secondary_entry`, SCTLR 0x30d00800) and the dsb/isb before MMU enable. Generic: TCR comes from upstream TCR_EL1_BOOT.
- **0004:124-130.** SCTLR check skipped. Generic.

## (B) sweet_early.c: early console and log mapping

- **0012:9-17, 59-76.** `#define SWEET_EARLY_FB_VA (VM_MIN_KERNEL_ADDRESS + ARM_TT_L2_SIZE)`, `assert(va + ARM_TT_L2_SIZE <= gVirtBase)`, the `cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT]` table assert, and `assert(*slot == ARM_TTE_EMPTY)`. Comment: "otherwise empty 32MB block above low globals".
  - Why 16K: with the LARGE 16K layout, VM_MIN and the kernel share one 64 GB L1 entry. With 4K and non-LARGE, ARM_TT_L2_SIZE becomes 2 MB, VM_MIN_KERNEL_ADDRESS changes, and VM_MIN may sit in a different 1 GB L1 entry from the kernel.
  - **Open (verify):** check the !ARM_LARGE_MEMORY VM_MIN_KERNEL_ADDRESS in vm_param.h. Check that its L1 entry is a table when `sweet_early_map_final` runs, that the VM_MIN+2MB slot is empty, and that it lies below gVirtBase.
  - S8 has no model here: S8 0082:64-71 just stubs all sweet_early functions for A5XELTE.
  - Fix candidate: a 2 MB slot between LowGlobals and the first kernel segment.
- **0081:13, 0081:20-23.** Same alias, now Device memory: `*slot = SWEET_EARLY_LOG_BLOCK | … ATTRINDX(CACHE_ATTRINDX_DISABLE)`. It inherits both problems above.
- **0081:137-145, 0083:17-51.** `PRZ_OFF = PRZ - LOG_BLOCK` must be less than ARM_TT_L2_SIZE with 4K. Add `_Static_assert(PRZ_OFF + 0x100000 <= ARM_TT_L2_SIZE)`.
- **0177:58.** `(uintptr_t)fp >= 0xfffffe0000000000ull` is a LARGE-layout literal. As a lower bound it still passes with the 0xfffffff0… layout, so it is harmless. Use VM_MIN_KERNEL_ADDRESS for clarity.
- **0012:86-109, 0081:… sweet_early_relocate/disable.** Generic.

## (C) arm_vm_init / pmap

- **0011:8-18, 27-28, 36-42.** `ARM64_PHYSMAP_ALIGNMENT_PADDING (PTOV_TABLE_SIZE * ARM_TT_TWIG_SIZE)`, and `physmap_l1_entries = (… >> ARM_TT_L1_SHIFT) + 1` in the LARGE branch.
  - Effect with 4K: the patch comment says the LARGE branch is the one in use, but after `-UARM_LARGE_MEMORY` the `#else` branch at 0011:39-42 runs instead. Both branches carry the padding, and the twig becomes 2 MB, so padding drops to 8 x 2 MB. No change needed beyond updating the comment.
- **0082:70-78.** `flush_mmu_tlb()` after `set_mmu_ttb_alternate`. Generic; keep it.
- **0080:62-78 (same flush), 0010:85-86, 0012:86-117.** Mark calls only. Generic.
- **0180:23-24.** `flush_dcache((vm_offset_t) pp << PAGE_SHIFT, …)` widened for RAM above 4 GB. Generic and correct for 4K too.
- **0008:25-60, 0005.** PPL / iommu stubs. Not page-related.
- **0165:124-131.** `IO_MAP_SIZE (64ul << 20)`; the comment mentions "a 16 KB mapping panicked". Generic; 64 MB of 4K pages is fine.
- Upstream items to add (not in the series): 0084, 0093 and 0097 equivalents (see top), and the RAM-size blocker.
- From S8 0124:1053-1063: `physmap_slide = 0` plus `ROUND_TWIG(physmap_base)` on DREAMLTE, after random hangs before pmap_startup. Cause unknown. Treat it as an unverified generic-4K candidate and adopt it if early hangs appear.

## (D) Loader-facing structures

- **shim/loader.c:16** `#define PAGE_16K 0x4000UL`; **loader.c:120-123** `ba_pa = (adt_pa + adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1); top = ba_pa + PAGE_16K; zero64(ba_pa, PAGE_16K)`.
  - Harmless over-alignment, since 16K is a multiple of 4K. topOfKernelData stays page-aligned.
  - Fix (clarity only): rename to `PAGE_ALIGN` with 0x1000, or keep. S8's Samsung loader uses 4 KiB-aligned boot_args.
- **loader.c:36-50.** The boot_args layout asserts. Granule-independent.
- **tools/mkpayload.py:39** `PAGE = 0x4000`; **:158** kernel_end rounding; **:168** RAMDisk/blob rounding. Harmless: whole 4K pages, and md0 still gets whole pages. Fix: 0x1000, optional.
- **tools/mkpayload.py:154-156** `# virt_base: 32MB (16K-granule L2 block) below the mach header …` / `virt_base = text['vmaddr'] & ~(0x2000000 - 1)`.
  - Why 16K: it aligns to the 16K L2 block. A 32 MB-aligned value is still 2 MB-aligned, so it works, but it wastes up to 30 MB below the kernel.
  - Fix: `& ~(0x200000 - 1)` for 4K. A `--granule` switch would keep sailfish building.
- **tools/mkstub-payload.py:10** `VIRT = 0xfffffff007004000`. By coincidence this already equals the non-LARGE link address; no change.
- **tools/mkadt.py.** No page assumptions found. dram-size 0x7e550000 and the taimen candidate 0x7e4c0000 are 64K-aligned.

## (E) VM / code signing / dyld shared cache / commpage

- The series never touches ubc_subr.c, vm_shared_region.c, vm_param.h or zalloc.c, so all of these are upstream behaviour that needs S8 patches:
  - **cd->pageSize 14 rejected** on a 4K kernel ("disallowing unsupported code signature page shift"). The iOS 16 dyld cache is signed with 16 KiB pages, so launchd dies at libSystem. Fix: S8 0107 (three hunks) plus `cs_enforcement_disable=1`.
    - Caution: 0107:22-26's `cs_validate_page` early return disables all page hashing, not just for the cache.
  - **MACH_VM_MAX_ADDRESS** is the MacOSX value `0x00007FFFFE000000` (47-bit). It exceeds a 4K T0SZ, and the commpage nesting assert in `pmap_insert_commpage_internal` fires. Fix: S8 0084 (use `0x0000000FC0000000`). That also matches the iOS userland layout.
  - **ZONE_MAP_VA_SIZE 128 GiB** (MacOSX) does not fit. Fix: S8 0097 (24 GiB / 8 GiB). It is verified only at 512 MiB RAM.
- **0035:110-116.** Comment: "converting a 4K-page signature to multilevel hashes drops the XML blob". The multilevel conversion only happens on a 16K kernel. On 4K, 4K signatures pass natively. The ordering rationale changes but the code still works; update the comment.
- **0024:26-27.** `/* memdev counts in 4K pages, independent of the kernel's PAGE_SHIFT. */ … << 12`. Already correct (now trivially so).
- **0148:26** `va >= MACH_VM_MAX_ADDRESS`. Generic; follows 0084.
- **0080:29-61.** commpage `casa` replaced by LL/SC. Not 4K-related, but needed on the ARMv8.0 Kryo 280 too (S8 0096 is the Exynos twin).
- **0080:12 `-mcpu=kryo`**, and `tools/xnu-build-local.sh` builds firehose with `-mcpu=kryo`. Not page-related. The Kryo 280 is A73/A53-class ARMv8.0 without LSE, so check that `-mcpu=kryo` (8996) is acceptable or use `cortex-a73`. S8 found LSE leaking from libfirehose.
- Userland side: tools/mkinit.py:26 (16K segments) and sweetgpu-test.c:111 (`posix_memalign 16384`) still work on 4K. sweetgl/eagl.c uses `vm_page_size` (generic).

## (F) Drivers

### GPU (sweet_gpu.c), concrete bug first

- **Ring overflow (real 4K bug).**
  - `0120:114-123` `gpu_page()` allocates `PAGE_SIZE`; `0134:33-37` "One 16 KB page" `gpu_alloc(va, PAGE_SIZE)`.
  - `0126:276` `rb_pa = gpu_page(&ring_va)`.
  - `0120:201-202` `/* BUFSZ log2(16 KB / 8) = 11 */ gw(gpu, R_CP_RB_CNTL, 11u | …)`.
  - `0137:31-32, 39-41` `#define RB_DWORDS 4096u` and wrap `& (RB_DWORDS - 1)`; `0150:332` free count uses RB_DWORDS.
  - On 4K the CP and the CPU both use 16 KB of ring inside a 4 KB allocation.
  - Fix: `rb_pa = gpu_alloc(&ring_va, RB_DWORDS * 4)` and flush 16 KB at 0150:394 and 0126:339.
- **Limits that shrink 4x with PAGE_SIZE:**
  - `0133:166-174` `kernel_page`: `k->size <= PAGE_SIZE` (shader binaries must be ≤ 4 KB; check sizes in sweet_gpu_shaders.h). Fix: gpu_alloc(16 KB).
  - `0131:78` and `0133:197` `bytes > PAGE_SIZE`.
  - `0130:94-95` `mdt_len > PAGE_SIZE` for the zap metadata. The a540_zap.mdt size could not be checked here; firmware is not in research/. Fix: allocate round_page(mdt_len).
  - `0137:208-214` `vm_buffer`: `uint64_t pa[16]; … npages > 16`, a cap of 256 KB on 16K and 64 KB on 4K. Fix: size by bytes.
  - `0136:178` `src = gpu_page(&sva)`: the texture source is one page.
- **Stack risk.** `0144:39-48` `pages[(3072 * 1334 + PAGE_MASK) / PAGE_SIZE]` on the kernel stack is about 2 KB on 16K and about 8 KB on 4K. Fix: kalloc it.
- **Degrade correctly to sub=1 (no change):** `0137:72-104` `pt4k` (`pool_left = PAGE_SIZE / 4096`); `0137:168-171` / `0141:29-30` / `0150:107-111` `sub = PAGE_SIZE / 4096`; `0150:84` `per = (2u << 20) / PAGE_SIZE`.
- **More memory, still fine:** `0150:16, 49` `GVA_PAGES = 8 GB / PAGE_SIZE` and `g_owner` becomes 2M x u16 = 4 MB kalloc_data (was 1 MB). Note the VM_GUARD/u16 index scheme is unchanged.
- **Comments only:** `0137:73, 87, 132` and `0150:8` say "16 KB pages".
- **Generic:** `0138:363, 498-510`, `0150:731-751, 789-800`, `0164:211-224, 246-255`, `0155:45-46`, `0182:301-322` (`pa >> PAGE_SHIFT`, PAGE_MASK checks, getPhysicalSegment per page). User BOs only need 4K alignment now.
- **Not kernel-granule:** `0121:44-45` / `0122:47-48` SMMU "TCR: T0SZ 28, TG0 4K" are the SMMU's own tables. `0130:112` `0xfff` rounding is the PIL 4K convention.

### IOSurface / framebuffer

- `0046:360, 450, 462` round_page allocSize and plane offsets; `0087:142-145` / `0154:17-18` shadow buffer round_page plus PAGE_MASK alignment; `0157:41-42` trunc/round_page of client address. All generic.
- `0079:125-127`, `0099:84-86`, `0164:524-526`, `0168:139-141` `ml_io_map(trunc_page_64(reg), PAGE_SIZE)` + `(reg & PAGE_MASK)`. Generic, as long as the registers used stay within that 4K page. They do: kickoff[-7..-1], INTR +0x14/+0x18, VIG +0x30/+0x34.
- `0046:180-181` `MAX_DIM 16384`, `0138:153-154` / `0149:41-49` `> 16384` are pixel dimensions, not pages.

### UFS / USB

- `0028:98-123` `UFS_BLOCK 4096u`, `DMA_UCD 0x1000`, `DMA_BOUNCE 0x4000`, `DMA_SIZE` are byte offsets inside one `kmem_alloc_contig` (0028:519-528), plus the device block size. Not granule-specific.
- `0034:24`, `0030:80`, `0047:73-92`, `0069` are generic. `0073:95-100, 371-380` `UD_SIZE 0x4000` contig alloc is generic.

### ml_io_map(…, 0x1000) audit (rule for new taimen drivers)

- On 16K every 0x1000 map really mapped 16 KB, so an offset of 0x1000 or more worked silently. On 4K it faults.
- Audited, all offsets below 0x1000, safe: 0089:86, 151, 177 (keys_obs off=0); 0093:97-98 (clk 0x0-0x110, CLK_MUX 0x40); 0094:70, 112; 0096:832-836 (QUP max 0x408, GCC +4/+8/+0x20, TLMM +0/+4); 0100:299; 0102:54, 93 (macc 0xc0); 0114:291; 0115:20; 0117:24-25; 0118:55; 0119:143-145 (+0x10/+0x18); 0145:37-38; 0175:656 (+0xa0..+0xd0); 0176:15.
- Explicitly sized maps are fine: 0119:142 MMCC 0x10000, 0119:219 GPU 0x40000, 0120:159 SMMU 0x10000 (CB +0x8000), 0100:298 msgram.
- Sweet-only: 0060:705 `TOUCH_SE 0x4000`, removed in 0096:829. Wi-Fi (0169 `wl_map` rounds correctly) does not apply to taimen.
- `0098:17` / `0170:31, 64` "L1 0x4000 + 0x100 n" are PMIC register addresses, not pages.

### Touch / keys / perf / battery

- No page math beyond the maps above.

## (G) Shim and tools

- **Pixel 1 build (shim/build-pixel.sh:16):** `start.S pixel.c pstore.c console.c fdt.c loader.c`. Taimen (build-taimen.sh:14) adds `log.c ufs_taimen.c util_taimen.c`, uses taimen.c instead of pixel.c, and its stage 1 does not call the loader yet.
  - Neither build compiles mmu.c, shim.c or ufs.c; those are only in the sweet build (build.sh:9). The Pixel/taimen shims jump with the MMU off.
- **16K items that only matter if mmu.c is reused as a template:** shim/mmu.c:2-5 comment, `:17-21` `PG_SHIFT 14`, `BLK_SHIFT 25` (32MB), `NENT 2048`, `:42-44` `TCR_VAL` (T0SZ=28, TG0=16K `2<<14`, TG1=16K `1<<30`, IPS 36), `:57-58` `pt_pool[4 * NENT]`, `:77-105` 16K/32MB math. A 4K rewrite needs TG0=0, T0SZ=25 or 28 with an L1 start (512 entries) and 2 MB blocks. S8's `shim/a5xelte/build-probe4k.sh` does this.
  - Also mmu.h:4 `FB_PHYS 0x9c000000` and shim.c:417 (tg16 probe printout).
- **Used by Pixel/taimen:** loader.c:16, 120-123 (harmless, see D). start.S:12 and granule-probe.S:14 `flags 0xa` are the Linux Image header page-size hint and do not reflect a kernel assumption.
- **Tools:** mkpayload.py:39, 154-158, 168 (see D); mkadt.py has none.

## (H) What the S8 project changed for 4K

| S8 patch | What it does | Generic-4K or Exynos? |
|---|---|---|
| 0085:72-88 (A5XELTE.h), 0085:47 MakeInc | New board header **without** `__ARM_16K_PG__` (this is the actual 16K removal). Keeps `#define SWEET 1` (0085:74) to reuse donor code. `-march=armv8-a+crypto`. | Generic (the board split pattern). |
| 0082:7-16, 24-25, 35-36, 44-45, 53-54, 64-71 | Entry marker; guards the 32MB early-FB start.s write, `sweet_early_map_final` and `relocate` with `!A5XELTE`; stubs all of sweet_early.c. | Guards are generic-4K (they skip the 16K-only code). The marker address is Exynos. It is a stub, not a fix. |
| 0084:9-10 | `MACH_VM_MAX_ADDRESS_RAW` set to 0x0000000FC0000000 instead of the MacOSX 47-bit value, so the 4K commpage nesting assert holds. | Generic-4K. |
| 0093:7-13 | `-UARM_LARGE_MEMORY`, PLKSEG 0xfffffff004004000, LINK 0xfffffff007004000 (39-bit TTBR1). | Generic-4K. |
| 0097:7-13 | ZONE_MAP_MAX 8 GiB, ZONE_MAP_VA_SIZE 24 GiB. | Generic-4K; verified only with 512 MiB RAM. |
| 0107:9-14 / 22-26 / 39-47 | Accept `cd->pageSize == 14`; `cs_validate_page` returns early; shared region `vm_map_cs_enforcement_set(sub_map, false)`. Plus boot-arg `cs_enforcement_disable=1`. | Generic-4K (lab-grade; disables page hashing). |
| 0089:31-51 (0124:1159-1180 generalises it to `A5_FB_2M_BLOCKS`) | VA=PA framebuffer via an extra `create_bootstrap_mapping` (5 x 2 MB blocks, its own L1 index), then ATTRINDX rewritten to WRITECOMB. | The pattern is generic-4K (use it for the taimen log block); the addresses are Exynos. |
| 0124:1053-1063 | DREAMLTE `physmap_slide = 0` + `ROUND_TWIG`, after random pre-pmap_startup hangs. | Generic-4K candidate, unverified. |
| DREAMLTE_PROGRESS item 1 | 2 GiB RAM triggers the kmem_range_init failure; ADT RAM cut to 512 MiB. | Generic-4K workaround; root cause open. |
| 0094 (`-mcpu=cortex-a53`), 0095 (PAN off), 0096 (commpage LL/SC), firehose rebuild | ARMv8.0 code generation. | Not 4K. Sailfish already has equivalents (0080, xnu-build-local.sh `-mcpu=kryo`). |
| 0086, 0099, 0100, 0112-0115 | GICv2, architected timer without MMIO base, counter epoch, SGI/topology. | Exynos-specific. Taimen is GICv3 with vtimer INTID 19, like sailfish. |
| 0083 | Xcode 27 IORPC.kernelContent. | Toolchain, not 4K. |
| 0103-0106, 0108-0111, 0116-0123 | Ramdisk, eMMC, unsealed root, AMFI bridge, display, logs, touch. | Exynos/board-specific. |

## Work list in order

1. Board header without `__ARM_16K_PG__`.
2. Port 0093, 0084 and 0097.
3. Fix the 0010/0081 start.s log block with the 0089 pattern and the 2 MB LOG_BLOCK.
4. Resolve the 0012 VA (verify VM_MIN_KERNEL_ADDRESS).
5. Port 0107 plus `cs_enforcement_disable=1`.
6. Fix the GPU ring and the PAGE_SIZE limits.
7. Adjust the mkpayload 2 MB alignment (optional).
8. Investigate kmem_range sizing for 2 GiB or more, or boot with 512 MiB first.
9. Keep the 0124 physmap-slide pin ready.
