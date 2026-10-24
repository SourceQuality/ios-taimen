# eagl.c QuartzCore addresses on the iPhone X cache (2026-10-23)

## Summary

- `tools/sweetgl/eagl.c` patches QuartzCore at fixed unslid shared-cache addresses:
  - the format hook;
  - 12 research entry hooks;
  - the texture-function table and create_shader's uniform cases.
- These addresses came from the iPhone 8 16.7.16 (20H392) cache, "dsc-8". Taimen runs the iPhone X 16.7.16 (20H392) root, "dsc-x". That root has a different cache, so every address pointed at other code there.
- Both caches carry the **same QuartzCore build**: LC_UUID `9e40b2eb-260c-3af1-ae45-a1a551a8c1b9`.
  - In dsc-x it sits **0x4000 higher**. That holds for `__TEXT`, `__DATA_CONST` and `__DATA`.
  - `__DATA_DIRTY` moves by 0x4240 and `__LINKEDIT` by 0xc0000; eagl.c uses neither.
  - The hooked prologue words are identical, so `sgl_hook_def` and the `SGL_HOOK_STUB`s serve both caches.
- eagl.c now keeps one address set per cache in `sgl_caches` and picks it by the running cache's UUID.
  - `sgl_find_cache` reads the UUID from the cache header (+0x58) at the start that `_dyld_get_shared_cache_range` returns.
  - For an unknown cache it logs `sweetgl: unknown shared cache <uuid>, QuartzCore fixes not installed` and patches nothing.
  - The old guards (prologue words, `tex_fragment_shaders[0x13]`, cases) are still checked before each write.
- On taimen, expect `sweetgl: QuartzCore addresses for the iPhone X (dsc-x) shared cache (slide 0x...)`, followed by the four `texture function ... installed` lines.
- All addresses were found; none are missing.

## The two caches

| | dsc-8 (Pixel 1) | dsc-x (taimen) |
| --- | --- | --- |
| IPSW | `iPhone_4.7_P3_16.7.16_20H392_Restore.ipsw` (iPhone10,1/10,4) | `iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` |
| Cache UUID | `32035564-853b-388b-a1ef-2ea354c196e1` | `8f970452-c9e3-3e88-b114-feb1864bc9e0` |
| Unslid base | 0x180000000 | 0x180000000 |
| QuartzCore mach header | 0x188280000 | 0x188284000 |
| QuartzCore `__DATA_CONST` | 0x1d5b219f0 | 0x1d5b259f0 |

## Address map (unslid)

Argument types are shown without their `CA::OGL::` prefix.

| eagl.c name | QuartzCore symbol | dsc-8 | dsc-x | first 4 words (both) |
| --- | --- | --- | --- | --- |
| format hook | `CA::OGL::GLESContext::image_parameters(unsigned, int, unsigned&, unsigned&, unsigned&, CA::OGL::ImageFunction&, bool&)` | 0x18840433c | 0x18840833c | b90000bf b900009f b900007f 390000df |
| 0 emit_filter | `CA::OGL::emit_filter(Renderer&, Filter const&, Layer const&, float, Surface*, float, bool, CA::Shape const*, float*)` | 0x188424e54 | 0x188428e54 | d103c3ff 6d072beb 6d0823e9 a9096ffc |
| 1 render_vibrant_color_matrix | `CA::OGL::render_vibrant_color_matrix(Context&, CA::Render::Filter const*, Surface*, float)` (static) | 0x188427248 | 0x18842b248 | 6db923e9 a9016ffc a90267fa a9035ff8 |
| 2 render_vibrant_filter | `CA::OGL::render_vibrant_filter(Context&, CA::Render::Filter const*, Surface*, float)` (static) | 0x188427018 | 0x18842b018 | d102c3ff 6d0533ed 6d062beb 6d0723e9 |
| 3 filter_node | `CA::OGL::filter_node(Renderer&, Layer*, Filter*, Gstate const*, ImagingNode*)` | 0x1883900d0 | 0x1883940d0 | a9bc5ff8 a90157f6 a9024ff4 a9037bfd |
| 4 MaskNode::apply | `CA::OGL::MaskNode::apply(float, Surface**, float*)` | 0x188372f74 | 0x188376f74 | d10703ff 6d1523e9 a9166ffc a91767fa |
| 5 prepare_filters | `CA::OGL::prepare_filters(Renderer&, Layer&, CA::Render::TypedArray<CA::Render::Filter>*, bool, SourceRequirement&, bool)` | 0x18834d888 | 0x188351888 | 6db63bef 6d0133ed 6d022beb 6d0323e9 |
| 6 emit_filter_quads_color | `CA::OGL::emit_filter_quads_color(Context&, Surface*, float, ExtendedColor)` (static) | 0x18834f480 | 0x188353480 | d10303ff 6d0623e9 a90767fa a9085ff8 |
| 7 render_background | `CA::OGL::render_background(Renderer&, Layer const*)` | 0x1882d30b8 | 0x1882d70b8 | d10243ff 6d0323e9 a90467fa a9055ff8 |
| 8 render_solid_background | `CA::OGL::render_solid_background(Renderer&, Layer const*, ExtendedColor, CA::Render::Pattern*)` | 0x188311534 | 0x188315534 | 6db923e9 a9016ffc a90267fa a9035ff8 |
| 9 fill_round_rect | `CA::OGL::fill_round_rect(Context&, RectState const&, CA::Vec2<double>, unsigned, unsigned)` | 0x1884d504c | 0x1884d904c | d10483ff 6d0d23e9 a90e6ffc a90f57f6 |
| 10 fill_color_rect | `CA::OGL::fill_color_rect(Context&, CA::Rect const&, unsigned, bool, bool, CA::Render::VelocityBucket)` | 0x1882f1298 | 0x1882f5298 | d10203ff a9045ff8 a90557f6 a9064ff4 |
| 11 create_shader | `CA::OGL::GLContext::create_shader(ContextState const*, GLShader const*, int)` | 0x1884af0d8 | 0x1884b30d8 | a9ba6ffc a90167fa a9025ff8 a90357f6 |
| tex_fragment_shaders | `CA::OGL::tex_fragment_shaders` (static, `__DATA_CONST,__const`, `const char *[90]`) | 0x1d5b44a10 | 0x1d5b48a10 | data |
| uniform_cases | create_shader's switch byte table (`__TEXT,__const`, index function - 0x11) | 0x18855a1e4 | 0x18855e1e4 | data |

- Both caches agree on the table data:
  - `tex_fragment_shaders`: all 90 strings are equal.
  - `[0x13]` is `colorMatrix4x4Bias(s0)`.
  - The NULL slots are 0x14, 0x19, 0x1c, 0x26-0x28, 0x2d and 0x33-0x59.
  - The 0x5a case bytes are equal: case 0x13 is 0, and 0x14, 0x26, 0x27 and 0x2d are 0xde.
- The names are QuartzCore's local symbols from the dsc-x `.symbols` file. They are not redacted for these functions.

## Method

1. **Where the iPhone X cache is.** iOS 16 keeps the cache in the OS cryptex.
   - `tools/mkiosroot.sh` copies the cryptex to `/private/preboot/Cryptexes/OS`. It hardlinks `/System/Library/Caches/com.apple.dyld/*` to the same files.
   - In `research/ipsw-x/ios16-root.img`, 7-Zip lists both paths as 0-byte files. The data is in the hardlink inodes, `iOSRoot/[HFS+ Private Data]/iNode*`.
   - Extracting that directory gives 62 files (3.3 GB). The 44 `dyld_v1` files of the main cache are matched by UUID: the main header's sub-cache array plus `symbolFileUUID`.
   - The DriverKit cache's two files are dropped.
   - 7-Zip reports "Data Error" on 33 inodes. Even so, every page of all 44 files matches the cache's code-signature hashes.
2. **The iPhone 8 cache, to confirm dsc-8 and get its UUID.** The `Cryptex1,SystemOS` DMG of the iPhone 8 IPSW is `087-86580-047.dmg`, per its BuildManifest.
   - Fetch it alone over HTTP ranges (1.1 GB; `tools/remotezip.py` does this). 7-Zip opens it as APFS.
   - Its `.symbols` and two `.dyldlinkedit` files are LZBITMAP-compressed (decmpfs type 14), and 7-Zip leaves only their resource forks. That costs the dsc-8 symbol names, nothing else.
   - The other 41 files hash-verify, and all of eagl.c's old addresses and guards hold there.
3. **Code.** Each 16-byte prologue was searched in dsc-x QuartzCore `__TEXT`.
   - Of the deltas between hits and old addresses, only +0x4000 is shared, by all 13.
   - At each new address, `LC_FUNCTION_STARTS` has a function start, and the local symbol is the function eagl.c names.
4. **Same build.** The LC_UUIDs are equal. Across all of `__text` (0x2c9a08 bytes) at +0x4000:
   - 724595 words are identical;
   - 1414 `adrp` differ, with the same register;
   - 4745 `add`/`ldr` differ only in imm12, the page offsets;
   - nothing else differs.
5. **Tables.**
   - Exactly one `__DATA_CONST` pointer array has `[0x13]` pointing at `colorMatrix4x4Bias(s0)`: 0x1d5b48a10.
   - create_shader is the only code that references it (`adrp`+`add` at 0x1884b3838, 0x1884b3f14 and 0x1884b5bdc) and the case table (0x1884b3e78).
6. `tools/sweetgl/eagl-addrs.py CACHE_DIR` replays all of this against eagl.c's own tables: prologues, table guards, free slots, with symbols. `--verify` hash-checks the cache first.

## Adding another cache

- Extract the cache, as in step 1 or 2, and run `eagl-addrs.py`.
  - If QuartzCore is the dsc-8 build at another place, it derives the entry from QuartzCore's segment deltas, checks it, and prints it for `sgl_caches`.
  - Run with dsc-x left out of eagl.c, it reproduces the dsc-x entry above.
- A different QuartzCore build has to be mapped by hand: look the names up in the local symbols, as in steps 3 and 5.
  - If its prologues differ, `sgl_hook_def`'s words and the `SGL_HOOK_STUB` replays would need to become per-cache.

## Not yet verified

- Nothing has run on a phone yet. taimen's GPU path is still in bring-up.
- eagl.c has not been compiled with the iOS SDK here. The changed region passes `aarch64-linux-gnu-gcc -fsyntax-only -Wall -Wextra` against Mach stubs, with the same warnings as before; the only warnings are gcc ignoring `naked`, which clang honours.
- The sweetgl CI job will be the real compile.
