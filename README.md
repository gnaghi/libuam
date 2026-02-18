# libuam - deko3d shader compiler library

libuam is a static library that compiles GLSL 4.60 shaders into DKSH (deko3d shader) binaries at runtime on the Nintendo Switch. It targets the Nvidia Tegra X1 (Maxwell GM20B) GPU.

Based on [mesa](https://www.mesa3d.org/) 19.0.8's GLSL parser and TGSI infrastructure, and nouveau's nv50_ir code generation backend.

## Building for Nintendo Switch

### Prerequisites

- [devkitPro](https://devkitpro.org/) with devkitA64
- Python 3 with the `mako` module (`pacman -S python-mako`)
- `bison` and `flex` (`pacman -S bison flex`)
- `meson` and `ninja` (`pacman -S meson`)

### Cross-compilation

```bash
# Configure (once)
meson setup builddir --cross-file cross_switch.txt

# Build
meson compile -C builddir

# Install to devkitPro portlibs
DESTDIR=/opt/devkitpro/portlibs/switch meson install -C builddir
```

This produces `libuam.a` (~6 MB) for aarch64.

## C API

```c
#include <libuam/libuam.h>

// Create a compiler for a specific shader stage
uam_compiler *compiler = uam_create_compiler(DkStage_Vertex);
// Or with a custom optimization level (0-3, default is 3):
uam_compiler *compiler = uam_create_compiler_ex(DkStage_Fragment, 2);

// Compile GLSL source to DKSH
const char *glsl = "#version 460\nlayout(location=0) in vec4 pos;\nvoid main() { gl_Position = pos; }";
if (uam_compile_dksh(compiler, glsl)) {
    // Get DKSH size and write to memory
    size_t size = uam_get_code_size(compiler);
    void *dksh = malloc(size);
    uam_write_code(compiler, dksh);

    // Query shader info
    int gprs = uam_get_num_gprs(compiler);
    unsigned int code_size = uam_get_raw_code_size(compiler);
} else {
    // Get error/warning log
    const char *log = uam_get_error_log(compiler);
    fprintf(stderr, "Compilation failed:\n%s\n", log);
}

// Cleanup
uam_free_compiler(compiler);
```

### Function reference

| Function | Description |
|----------|-------------|
| `uam_create_compiler(stage)` | Create compiler for a pipeline stage (opt level 3) |
| `uam_create_compiler_ex(stage, opt_level)` | Create compiler with custom optimization level |
| `uam_free_compiler(compiler)` | Destroy compiler and free resources |
| `uam_compile_dksh(compiler, glsl)` | Compile GLSL source, returns true on success |
| `uam_get_code_size(compiler)` | Get DKSH binary size (with container) |
| `uam_get_raw_code_size(compiler)` | Get raw Maxwell bytecode size |
| `uam_write_code(compiler, memory)` | Write DKSH binary to a memory buffer |
| `uam_get_error_log(compiler)` | Get error/warning log from last compilation |
| `uam_get_num_gprs(compiler)` | Get GPU register count used by compiled shader |
| `uam_get_version(major, minor, micro)` | Get library version |

## GLSL requirements

- Shaders must use **GLSL 4.60** syntax (`#version 460`)
- UBO, SSBO, sampler and image bindings must be **explicit**: `layout(binding = N)`
- Default uniforms outside UBO blocks are **not supported** (will produce an error)
- `layout(location = N)` is required for vertex inputs and varying I/O
- The `DEKO3D` preprocessor symbol is defined (value 100)

## Differences with standard GL and mesa/nouveau

- UBO, SSBO, sampler and image bindings are **required to be explicit** (i.e. `layout (binding = N)`), and they have a one-to-one correspondence with deko3d bindings. Failure to specify explicit bindings will result in an error.
- There is support for 16 UBOs, 16 SSBOs, 32 "samplers" (combined image+sampler handle), and 8 images for each and every shader stage; with binding IDs ranging from zero to the corresponding limit minus one. However note that due to hardware limitations, only compute stage UBO bindings 0-5 are natively supported, while 6-15 are emulated as "SSBOs".
- Default uniforms outside UBO blocks (which end up in the internal driver const buffer) are detected, however they are reported as an error due to lack of support in both DKSH and deko3d for retrieving the location of and setting these uniforms.
- Internal deko3d constbuf layout and numbering schemes are used, as opposed to nouveau's.
- `gl_FragCoord` always uses the Y axis convention specified in the flags during the creation of a deko3d device. `layout (origin_upper_left)` has no effect whatsoever and produces a warning, while `layout (pixel_center_integer)` is not supported at all and produces an error.
- Integer divisions and modulo operations with non-constant divisors decay to floating point division, and generate a warning. Well written shaders should avoid these operations for performance and accuracy reasons. (Also note that unmodified nouveau, in order to comply with the GL standard, emulates integer division/module with a software routine that has been removed in UAM)
- 64-bit floating point divisions and square roots can only be approximated with native hardware instructions. This results in loss of accuracy, and as such these operations should be avoided, and they generate a warning as well. (Also note that likewise, unmodified nouveau uses a software routine that has been removed in UAM)
- Transform feedback is not supported.
- GLSL shader subroutines (`ARB_shader_subroutine`) are not supported.
- There is no concept of shader linking. Separable programs (`ARB_separate_shader_objects`) are always in effect.
- The compiler is based on mesa 19.0.8 sources; however several cherrypicked bugfixes from mesa 19.1 and up have been applied.
- Numerous codegen differences:
	- Added **Maxwell dual issue** scheduling support based on the groundwork laid out by karolherbst's [dual_issue_v3](https://github.com/karolherbst/mesa/commits/dual_issue_v3) branch, and enhanced with new experimental findings.
	- Removed bound checks in SSBO accesses.
	- Removed bound checks in atomic accesses.
	- Removed bound checks in image accesses.
	- Multisampled texture lookups use optimized bitwise logic with hardcoded sample positions instead of requiring helper data in the driver constbuf.
	- Multisampled image operations use `TXQ` instead of requiring helper data in the driver constbuf.
	- Non-bindless image operations are supported natively instead of being emulated with bindless operations.
	- SSBO size calculations use unsigned math instead of signed math, which results in better codegen.
	- `ballotARB()` called with a constant argument now results in optimal codegen using the PT predicate register.
	- **Bugfixes**:
		- Bindless texture queries were broken.
		- `IMAD` instruction encoding with negated operands was broken.
	- Minor changes done to match properties observed in official shader code:
		- `MOV Rd,RZ` is now preferred to `MOV32I Rd,0`.
		- `LDG`/`STG` instructions are used for SSBO accesses instead of `LD`/`ST`.
		- Shader programs are properly padded out to a size that is a multiple of 64 bytes.
