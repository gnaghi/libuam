# ES 1.00 Direct Compilation via Mesa — Implementation Details

## Overview

This document describes the implementation of GLSL ES 1.00 direct compilation
in uam using the already-imported Mesa GLSL compiler, bypassing the custom
SwitchGLES transpiler for ES 1.00 shaders.

## Problem Statement

Previously, ES 1.00 shaders were rejected by uam because:
1. Bare uniforms (without UBO blocks) were explicitly rejected in `glsl_frontend.cpp`
2. The driver constbuf (c[0]) where bare uniforms live maps to hardware slot 1,
   which has no public deko3d API for dynamic updates

The SwitchGLES transpiler converted ES 1.00 → GLSL 4.60 with UBOs, but it couldn't
handle all GLSL constructs (causing ~1,109 dEQP shader test failures).

## Solution Architecture

### Constbuf Remapping (c[0] → c[1])

**Hardware slot mapping in deko3d:**
```
NV50_IR c[N] → hardware slot = (N + 1) % 18
c[0] → slot 1 (driver constbuf, NOT API-accessible)
c[1] → slot 2 (UBO 0, bindable via dkCmdBufBindUniformBuffer(stage, 0, ...))
c[2] → slot 3 (UBO 1, bindable via id=1)
```

We remap all `c[0]` references to `c[1]` in the NV50_IR codegen, so bare uniforms
end up in hardware slot 2 (UBO 0), which is dynamically bindable.

### Files Modified

#### 1. `mesa-imported/codegen/nv50_ir_driver.h`
- Added `bool remapDriverCBToUBO0;` to `nv50_ir_prog_info.io` struct

#### 2. `mesa-imported/codegen/nv50_ir_from_tgsi.cpp`
- In `Converter::makeSym()`: when `remapDriverCBToUBO0` is set and
  `tgsiFile == TGSI_FILE_CONSTANT && fileIdx == 0`, remap `fileIdx` to 1

#### 3. `source/compiler_iface.cpp`
- Set `m_info.io.remapDriverCBToUBO0 = (m_numUniforms > 0)` before
  `nv50_ir_generate_code()` — auto-enables remap when bare uniforms exist

#### 4. `source/compiler_iface.h`
- Added `IsConstbufRemapped()`, `GetConstbufData()`, `GetConstbufDataSize()` accessors

#### 5. `source/glsl_frontend.cpp`
- **Removed** the driver constbuf rejection code (`goto _fail`)
- **Added** uniform metadata collection: iterates `Parameters` list, collects
  name, offset, type, size for each non-builtin, non-hidden uniform
- **Added** sampler auto-binding: before `link_shaders()`, walks the IR and
  assigns sequential binding numbers (0, 1, 2...) to unbound sampler uniforms.
  This enables ES 1.00 shaders with `uniform sampler2D` to compile without
  explicit `layout(binding=N)` annotations.
- **Added** query functions: `glsl_program_get_num_uniforms()`,
  `glsl_program_get_uniform_info()`, `glsl_program_get_constbuf_size()`

#### 6. `source/glsl_frontend.h`
- Added `glsl_uniform_info_t` struct and query function declarations
- Added `GLSL_UNIFORM_MAX` (128) and `GLSL_UNIFORM_MAX_NAME` (128) constants

#### 7. `source/uam.h` / `source/uam.cpp`
- Added `uam_uniform_info_t` struct (C API mirror of internal type)
- Added `uam_get_num_uniforms()`, `uam_get_uniform_info()`,
  `uam_get_constbuf_size()` — uniform metadata query
- Added `uam_is_constbuf_remapped()` — tells caller if c[0]→c[1] remap is active
- Added `uam_get_constbuf_initial_data()` — returns initial constbuf data pointer

## Uniform Metadata Format

```c
typedef struct {
    char name[128];       // Uniform name (e.g., "u_mvp")
    uint32_t offset;      // Byte offset in constbuf
    uint32_t size_bytes;  // Total size in bytes
    uint8_t  base_type;   // 0=uint, 1=int, 2=float, 6=bool, 7=sampler
    uint8_t  vector_elements; // 1-4
    uint8_t  matrix_columns;  // 1 for scalars/vectors, 2-4 for matrices
    uint8_t  is_sampler;      // 1 if sampler type
    uint32_t array_elements;  // 0 for non-array, N for array[N]
} glsl_uniform_info_t;
```

Layout follows std140 rules:
- Scalars: 4 bytes
- Vectors: 4 × N bytes (N = vector_elements)
- Matrices: 16 × columns bytes (each column padded to vec4)
- Arrays: element_size × array_elements

## SwitchGLES Integration (DONE)

### Shader Compilation Path (`gl_shader.c`)

`glCompileShader` tries Mesa direct compilation first for ES 1.00 shaders:

1. **`sgl_compile_es100_mesa()`**: Creates uam compiler, compiles ES 1.00 source,
   captures uniform+sampler metadata into heap-allocated `sgl_mesa_metadata_t`,
   loads DKSH binary, stores to shader cache. Sets `compiled_via_mesa = true`.
2. **Fallback**: If Mesa fails, falls back to transpiler validation + `needs_transpile = true`.

### Program Linking Path (`gl_shader.c`)

In `glLinkProgram`, Mesa-compiled shaders skip the transpiler and use a dedicated
metadata handler that:

1. Populates `program_uniforms[]` from shader `mesa_meta` (using `SGL_LOC_PACKED_FLAG`)
2. Populates `samplers[]` with auto-assigned bindings
3. Sets up `packed_ubo_sizes[]` for constbuf binding
4. Initializes packed UBO shadow buffers
5. Creates dual-stage mirrors for uniforms present in both VS and FS
6. Populates `active_uniforms[]` (for `glGetActiveUniform`)

### Data Types (`sgl_gl_types.h`)

```c
typedef struct sgl_mesa_metadata {
    int num_uniforms;
    sgl_mesa_uniform_t uniforms[SGL_MESA_MAX_UNIFORMS];
    uint32_t constbuf_size;
    int num_samplers;
    sgl_mesa_sampler_t samplers[SGL_MESA_MAX_SAMPLERS];
} sgl_mesa_metadata_t;
```

Stored per-shader as `sgl_shader_t.mesa_meta` (heap-allocated, freed in `sgl_res_mgr_free_shader`).

### Sampler API (`uam.h`)

```c
typedef struct {
    const char *name;
    int binding;
    uint8_t type;  // 0=sampler2D, 1=samplerCube
} uam_sampler_info_t;
int uam_get_num_samplers(const uam_compiler *compiler);
bool uam_get_sampler_info(const uam_compiler *compiler, int index, uam_sampler_info_t *info);
```

### glslang Conditional Compilation

`glslang_frontend.cpp` is guarded with `#ifdef HAVE_GLSLANG`. When glslang is not
available (Switch cross-build: `-Dglslang_root=`), stub inline functions are used
(no-ops for init/exit, returns false for compile). The define is set in `meson.build`
via `add_project_arguments('-DHAVE_GLSLANG', ...)` when `glslang_root != ''`.

## Test Results

All existing GLSL 4.60 tests continue to pass (no regressions).
New ES 1.00 tests (PC uam.exe validation):

| Test | Result | Uniforms |
|------|--------|----------|
| shader_es100_simple.vert | PASS | u_mvp: mat4 (64 bytes) |
| shader_es100_simple.frag | PASS | u_color: vec4 (16 bytes) |
| shader_es100_sampler.frag | PASS | u_tintColor: vec4 + sampler(binding=0) |
| shader_es100_deqp_style.vert | PASS | u_float + u_vec3 + u_mat3 (80 bytes) |
| shader_es100_deqp_style.frag | PASS | u_ref: float + u_flag: bool (20 bytes) |
| shader_es100_multi_sampler.frag | PASS | u_brightness: float + 2 samplers(0,1) |
| shader_es100_array.vert | PASS | u_bones: mat4[4] (256B) + u_weights: float[4] (16B) |
| shader_es100_cubemap.frag | PASS | u_color: vec4 + samplerCube(binding=0) |

### Build Validation

| Build | Status |
|-------|--------|
| Switch libuam.a (no glslang) | PASS — 193/193 objects |
| PC uam.exe (with glslang) | PASS — 232/232 objects |
| libSwitchGLES.a (bundled libuam) | PASS — 11.3MB |
| dEQP-GLES2 NRO | PASS — 17.4MB |

## Risk Assessment

- **Constbuf slot conflict**: No risk. ES 1.00 shaders have no explicit UBOs,
  so c[1] (UBO 0) is free. GLSL 4.60 shaders have no bare uniforms, so the
  remap flag is never set.
- **Initial constbuf data**: The DKSH still contains constbuf1 data at slot 1
  (harmless dead data). The shader reads from slot 2. SwitchGLES must bind a
  UBO at slot 2 or the shader reads uninitialized memory.
- **Sampler auto-binding**: Sequential assignment (0, 1, 2...) matches GLES2
  default behavior. SwitchGLES can override via `glUniform1i()`.
- **glslang stub safety**: When `HAVE_GLSLANG` is not defined, `CompileGlslViaGlslang()`
  calls inline stub that returns false — clean failure, no crash.
