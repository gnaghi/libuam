# Mesa ES 1.00 Support Analysis for UAM

## Date: 2026-03-10
## Status: Analysis complete, implementation ready to start

---

## Executive Summary

**uam already imports the full Mesa GLSL compiler (142 files, ~208K LOC) which natively supports
GLSL ES 1.00.** The `#version 100` directive is recognized and correctly sets `es_shader=true`.
The `ARB_ES2_compatibility` extension is already enabled. The ONLY blocker preventing ES 1.00
compilation is that `glsl_frontend.cpp` **explicitly rejects bare uniforms** in the driver
constbuf (lines 532-557).

### Key Insight
Mesa's preprocessor (glcpp) already has **correct ES undefined macro handling** with short-circuit
logic. This alone would fix 26 dEQP shader tests that our custom transpiler cannot handle.

### Estimated Impact
- **~26 dEQP tests fixed immediately** (undefined_identifiers + tokens_after_elif)
- **~18 dEQP tests fixed** (light_amount — struct array uniforms work natively)
- **Eliminates transpiler bugs** for all ES 1.00 shaders
- **Potential to fix hundreds more** shader compilation/image comparison failures

---

## Current Architecture (Transpiler-Based)

```
GLSL ES 1.00 source
     │
     ▼
┌──────────────────────┐
│  glsl_transpiler.c   │  ← ~2400 lines, bugs: preprocessor, struct arrays,
│  (custom, line-based)│     pragma, invariant, undefined identifiers...
│                      │
│  Transformations:    │
│  • attribute → in    │
│  • varying → out     │
│  • texture2D→texture │
│  • gl_FragColor→out  │
│  • bare uniforms →   │
│    UBO blocks        │
│  • z=(z+w)/2 inject  │
│  • precision strip   │
│  • #version 100→460  │
└──────────────────────┘
     │
     ▼
GLSL 4.60 with UBOs
     │
     ▼
┌──────────────────────┐
│  Mesa GLSL compiler  │  ← Already handles ES 1.00 natively!
│  (in uam)            │     But we feed it 4.60 instead...
└──────────────────────┘
     │
     ▼
TGSI → NV50_IR → DKSH
```

**Problem:** The transpiler re-implements (badly) what Mesa already does perfectly.

---

## Proposed Architecture (Direct ES 1.00)

```
GLSL ES 1.00 source
     │
     ▼
┌──────────────────────┐
│  Mesa GLSL compiler  │  ← Handles ALL ES 1.00 syntax natively:
│  (already in uam)    │     attribute/varying, texture2D, gl_FragColor,
│                      │     precision qualifiers, preprocessor, built-ins,
│                      │     undefined macro rejection in #if, etc.
│                      │
│  Bare uniforms →     │
│  driver constbuf     │  ← Currently REJECTED by uam — need to ACCEPT
│  c[0x1][offset]      │
└──────────────────────┘
     │
     ▼
TGSI → NV50_IR → DKSH
     +
Uniform metadata (name → offset mapping)
     │
     ▼
SwitchGLES binds uniform data to constbuf slot
```

---

## What Mesa Already Handles (No Code Changes Needed)

### 1. ES 1.00 Version Detection
- `glsl_parser_extras.cpp:418-425`: `#version 100` sets `es_shader = true`
- `standalone_scaffolding.cpp:221`: `ARB_ES2_compatibility = GL_TRUE` (already enabled)
- `glsl_parser_extras.cpp:233-234`: version 100 added to supported versions list

### 2. ES Syntax
- `attribute` → `ir_var_shader_in` (vertex)
- `varying` → `ir_var_shader_out` (vertex) / `ir_var_shader_in` (fragment)
- `gl_FragColor` → built-in output (color attachment 0)
- `gl_FragData[N]` → built-in output array
- `precision lowp/mediump/highp` → handled by `lower_precision.cpp`
- `invariant` → tracked in IR, emitted in headers

### 3. ES Built-in Functions
- `texture2D()`, `texture2DProj()`, `textureCube()` → all available
- `texture2DLod()` (vertex only) → available via extension
- Math functions: `radians`, `degrees`, `sin`, `cos`, `pow`, etc.
- `dFdx()`, `dFdy()`, `fwidth()` → fragment only, via `OES_standard_derivatives`

### 4. Preprocessor (glcpp)
- `#define`, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- `#pragma` → passed through (Mesa parser ignores unknown pragmas)
- `#version 100` → sets `is_gles = true` (`glcpp-parse.y:2346`)
- **Undefined macro rejection in ES mode** (`glcpp-parse.y:218-224`):
  ```
  if (parser->is_gles && $2.undefined_macro)
      glcpp_error("undefined macro %s in expression (illegal in GLES)");
  ```
- **Short-circuit logic** for `||` and `&&` operators (`glcpp-parse.y:498-543`)
- `GL_ES` macro defined automatically (`glcpp-parse.y:2352-2353`)
- `GL_FRAGMENT_PRECISION_HIGH` defined (`glcpp-parse.y:2364-2365`)
- `DEKO3D` macro defined (fincs edit, `glcpp-parse.y:2343`)
- `##` token pasting rejected in ES (`glcpp-lex.l:529-530`)

### 5. ES Built-in Variables
- `gl_Position`, `gl_PointSize` (vertex output)
- `gl_FragCoord`, `gl_FrontFacing`, `gl_PointCoord` (fragment input)
- `gl_MaxVertexAttribs`, `gl_MaxTextureImageUnits`, etc. (constants)
- `gl_DepthRange` (uniform struct: near, far, diff)

### 6. ES Type System
- No `uint`, `uvec*` (integer types require ES 3.00)
- No `double`, `dvec*`, `dmat*` (desktop only)
- `bool`, `int`, `float`, `vec2-4`, `mat2-4`, `sampler2D`, `samplerCube`
- Struct declarations, arrays

### 7. ES Validation
- No implicit conversions (int→float explicit in ES)
- Array indexing restrictions
- `const` variable must have initializer
- Function overloading rules
- No recursion

---

## What Needs to Change in UAM

### Change 1: Accept Driver Constbuf Uniforms (CRITICAL)
**File:** `source/glsl_frontend.cpp` lines 532-557
**Current:** Rejects any uniform in driver constbuf with error
**Change:** Collect uniform metadata instead of rejecting

```cpp
// BEFORE (current code — rejects):
gl_program_parameter_list *pl = linked_shader->Program->Parameters;
for (unsigned i = 0; i < pl->NumParameters; i++) {
    // ...
    glsl_frontend_log("error: uniform '%s' in driver constbuf not supported\n", ...);
    has_uniforms_in_driver_cbuf = true;
}
if (has_uniforms_in_driver_cbuf)
    goto _fail;

// AFTER (collect metadata):
gl_program_parameter_list *pl = linked_shader->Program->Parameters;
// Store uniform metadata for API query (don't reject)
// The driver constbuf data is already in pl->ParameterValues
```

### Change 2: Add Uniform Metadata API
**File:** `source/uam.h`, `source/uam.cpp`, `source/glsl_frontend.cpp/h`

New API functions:
```c
// Query number of active user uniforms
int uam_get_num_uniforms(const uam_compiler *compiler);

// Get uniform info by index
typedef struct {
    const char *name;       // Uniform name (e.g., "u_color")
    uint32_t offset;        // Byte offset in driver constbuf
    uint32_t type;          // GL type (GL_FLOAT, GL_FLOAT_VEC4, GL_FLOAT_MAT4...)
    uint32_t array_size;    // 0 for non-array, N for array[N]
    uint32_t size_bytes;    // Total size in bytes
} uam_uniform_info_t;

bool uam_get_uniform_info(const uam_compiler *compiler, int index, uam_uniform_info_t *info);

// Get driver constbuf initial data (ParameterValues)
const void *uam_get_constbuf_data(const uam_compiler *compiler, size_t *size);
```

### Change 3: Expose Constbuf Slot Information
**File:** `source/compiler_iface.cpp`

The driver constbuf maps to hardware slot via:
```cpp
// m_info.io.auxCBSlot = 17;
// transform: final_id = (raw_id + 1) % 18
// raw c[1] (driver constbuf) → final slot 2
```

SwitchGLES needs to know which hardware constbuf slot to bind uniform data to.

### Change 4: Handle Depth NDC (Two Options)

**Option A (recommended):** Use `DkDeviceFlags_DepthMinusOneToOne`
- Set the deko3d flag to handle [-1,1]→[0,1] in hardware
- Remove the manual `z=(z+w)/2` injection from transpiler
- Applies to ALL shaders (both ES 1.00 and GLSL 4.60)

**Option B:** Add Mesa IR lowering pass
- Insert `gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5` in vertex shader IR
- Only needed if we can't use the device flag
- Done after linking, before TGSI conversion

---

## What Needs to Change in SwitchGLES

### Change 1: New Compilation Path in gl_shader.c
```c
// Detect ES 1.00 source
if (source contains "#version 100") {
    // Skip transpiler — feed directly to uam
    bool ok = uam_compile_dksh(compiler, source);
    // Extract uniform metadata from compiled shader
    // Store in program structure
} else {
    // Existing path: transpile ES→460, then compile
}
```

### Change 2: Uniform Binding for Driver Constbuf
Currently SwitchGLES uploads uniforms to UBO bindings (slots 3+).
For driver constbuf uniforms, upload to slot 2 instead.

```c
// In sgl_prepare_draw():
if (program->uses_driver_constbuf) {
    // Bind uniform shadow buffer to constbuf slot 2
    dkCmdBufBindUniformBuffer(cmdbuf, stage, 2, addr, size);
}
```

### Change 3: glUniform* Mapping
Map `glGetUniformLocation()` names to driver constbuf offsets (from uam metadata).
`glUniform*` writes to a shadow buffer at those offsets.

---

## Constbuf Slot Mapping (deko3d/uam)

```
Raw ID  │ Transform: (id+1)%18  │ Hardware Slot  │ Purpose
────────┼───────────────────────┼────────────────┼─────────────────
c[17]   │ (17+1)%18 = 0         │ Slot 0         │ Aux constbuf (draw info)
c[0]    │ (0+1)%18 = 1          │ Slot 1         │ (unused in our case)
c[1]    │ (1+1)%18 = 2          │ Slot 2         │ Driver constbuf (bare uniforms) ← NEW
c[2]    │ (2+1)%18 = 3          │ Slot 3         │ UBO binding 0 (current path)
c[3]    │ (3+1)%18 = 4          │ Slot 4         │ UBO binding 1
...     │                       │                │
```

---

## Files to Modify

### UAM (D:\projets\programmation\switch\DekoGL\uam\)
| File | Change | Effort |
|------|--------|--------|
| `source/glsl_frontend.cpp` | Accept driver constbuf, collect uniform metadata | Medium |
| `source/glsl_frontend.h` | Add uniform metadata query API | Small |
| `source/compiler_iface.cpp` | Store uniform metadata in DekoCompiler | Small |
| `source/compiler_iface.h` | Add uniform metadata storage + getters | Small |
| `source/uam.cpp` | Add C API wrappers for uniform queries | Small |
| `source/uam.h` | Declare new API functions | Small |

### SwitchGLES (source/)
| File | Change | Effort |
|------|--------|--------|
| `gl/gl_shader.c` | Add ES 1.00 direct compilation path | Medium |
| `transpiler/glsl_transpiler.c` | Keep as fallback for GLSL 4.60 | None |
| `backend/deko3d/dk_backend.c` | Bind driver constbuf slot | Small |
| `gl/gl_uniforms.c` | Map uniforms to driver constbuf offsets | Medium |

---

## dEQP Tests Expected to Pass with This Change

### Immediately Fixed (26 tests)
- `shaders.preprocessor.undefined_identifiers.*` (24 tests)
  → Mesa glcpp rejects undefined macros in ES `#if` expressions
- `shaders.preprocessor.invalid_conditionals.tokens_after_elif_*` (2 tests)
  → Same mechanism

### Fixed by Native ES Compilation (~18 tests)
- `functional.light_amount.*` (18 tests)
  → Struct array uniforms work natively in Mesa (no flattening needed)

### Potentially Fixed (many more)
- All shader compilation errors from transpiler bugs
- Image comparison failures from incorrect transpilation
- Precision-related failures (Mesa handles precision correctly)
- Built-in function/variable failures

---

## Risk Assessment

### Low Risk
- Mesa's ES 1.00 support is battle-tested (used in production GPU drivers)
- The glcpp preprocessor is the reference implementation
- uam already imports all necessary files

### Medium Risk
- Driver constbuf handling requires careful SwitchGLES integration
- Need to ensure constbuf slot 2 doesn't conflict with existing bindings
- Depth NDC conversion needs verification

### Mitigations
- Keep transpiler as fallback (can switch per-shader if needed)
- Incremental approach: start with validation-only, then full compilation
- Extensive dEQP testing at each step

---

## Implementation Order

### Phase 1: UAM Changes (foundation)
1. Modify `glsl_frontend.cpp` to accept driver constbuf uniforms
2. Add uniform metadata collection + API
3. Test with simple ES 1.00 shaders on PC (uam CLI)

### Phase 2: SwitchGLES Integration
4. Add ES 1.00 detection in `gl_shader.c`
5. Add driver constbuf binding in draw path
6. Map `glUniform*` to constbuf offsets
7. Handle depth NDC

### Phase 3: Testing & Validation
8. Build + deploy to Switch
9. Run dEQP shader tests
10. Compare results (should see large improvement)
11. Fix any regressions

---

## Comparison of Full Mesa Source vs Mesa-Imported

The full Mesa source at `D:\projets\programmation\switch\mesa\` contains:
- Complete driver implementations (i965, radeonsi, nouveau, etc.)
- NIR infrastructure (not used by uam — uam uses TGSI→NV50_IR)
- Window system integration (EGL, GLX, etc.)
- Test suites
- Build system for all platforms

The `mesa-imported/` in uam contains exactly what's needed:
- Full GLSL frontend (lexer, parser, AST, IR, builtins, linker, optimizer, lowering)
- TGSI generation (`st_glsl_to_tgsi.cpp`)
- NV50_IR codegen (Maxwell target)
- Utility functions

**No additional files from full Mesa are needed** for ES 1.00 support.
The imported Mesa code already has all ES 1.00 handling built in.

---

## Other GLSL Versions Supported by Mesa-Imported

The following versions are already supported (can be useful for future work):

| Version | Profile | Status in uam |
|---------|---------|---------------|
| GLSL 1.10 | Desktop | Supported (may need testing) |
| GLSL 1.20 | Desktop | Supported |
| GLSL 1.30 | Desktop | Supported |
| GLSL 1.40 | Desktop | Supported |
| GLSL 1.50 | Desktop | Supported |
| GLSL 3.30 | Desktop | Supported |
| GLSL 4.00-4.60 | Core | Supported (current default) |
| GLSL ES 1.00 | ES | Supported (blocked by constbuf rejection) |
| GLSL ES 3.00 | ES | Supported (with ARB_ES3_compatibility) |
| GLSL ES 3.10 | ES | Supported (with ARB_ES3_1_compatibility) |
| GLSL ES 3.20 | ES | Supported (with ARB_ES3_2_compatibility) |

This means GLES 3.0 shaders would also work with the same changes — useful for
future GLES 3.0 support in SwitchGLES.

---

## Conclusion

The path from "current state" to "ES 1.00 support via Mesa" is surprisingly short:
1. **One code change in uam** (accept driver constbuf instead of rejecting)
2. **One new API** (uniform metadata export)
3. **SwitchGLES integration** (detect ES 1.00, bind constbuf, map uniforms)

No files need to be added from the full Mesa source. No major refactoring needed.
The Mesa code is already there, already compiles, already handles ES 1.00 correctly.
We just need to stop rejecting its output and wire up the uniform data path.
