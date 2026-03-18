#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__has_include) && __has_include("deko3d.h")
#   include "deko3d.h"
#else
typedef enum DkStage
{
	DkStage_Vertex   = 0,
	DkStage_TessCtrl = 1,
	DkStage_TessEval = 2,
	DkStage_Geometry = 3,
	DkStage_Fragment = 4,
	DkStage_Compute  = 5,

	DkStage_MaxGraphics = 5,
} DkStage;
#endif

typedef void *uam_compiler;

void uam_get_version(int *major, int *minor, int *micro);
int uam_get_version_nb(void);

// Creates/destroys compiler
// Returns NULL on failure
uam_compiler *uam_create_compiler(DkStage stage);
uam_compiler *uam_create_compiler_ex(DkStage stage, int opt_level);
void uam_free_compiler(uam_compiler *compiler);

// Set attribute binding before compilation (for glBindAttribLocation support).
// Must be called BEFORE uam_compile_dksh().
void uam_set_attrib_binding(uam_compiler *compiler, const char *name, int location);

// Compiles GLSL using Mesa frontend
// Returns true on success, false otherwise
bool uam_compile_dksh(uam_compiler *compiler, const char *glsl);

// Compiles a SPIR-V binary to DKSH
// spirv_data: pointer to SPIR-V binary (must start with SPIR-V magic)
// spirv_size: size in bytes (must be a multiple of 4)
// Returns true on success, false otherwise
bool uam_compile_spirv(uam_compiler *compiler, const void *spirv_data, size_t spirv_size);

// Gets the size of the previously compiled shader, as DKSH file
size_t uam_get_code_size(const uam_compiler *compiler);

// Write the compiled shader as DKSH to the specified location
void uam_write_code(const uam_compiler *compiler, void *memory);

// Gets the error/warning log from the last compilation
// Returns empty string if no errors
const char *uam_get_error_log(const uam_compiler *compiler);

// Gets the number of GPU registers used by the compiled shader
int uam_get_num_gprs(const uam_compiler *compiler);

// Gets the raw Maxwell bytecode size (without DKSH container overhead)
unsigned int uam_get_raw_code_size(const uam_compiler *compiler);

// Uniform metadata (for driver constbuf / bare uniforms in ES 1.00 shaders)
typedef struct {
    const char *name;         // Uniform name
    uint32_t offset;          // Byte offset in driver constbuf
    uint32_t size_bytes;      // Total size in bytes
    uint8_t  base_type;       // Mesa glsl_base_type: 0=uint, 1=int, 2=float, 11=bool, 12=sampler
    uint8_t  vector_elements; // 1-4
    uint8_t  matrix_columns;  // 1 for scalars/vectors, 2-4 for matrices
    uint8_t  is_sampler;      // 1 if sampler type
    uint32_t array_elements;  // 0 for non-array, N for array[N]
} uam_uniform_info_t;

// Gets the number of driver constbuf uniforms in the compiled shader
int uam_get_num_uniforms(const uam_compiler *compiler);

// Gets uniform info by index (0..num_uniforms-1)
// Returns false if index out of range
bool uam_get_uniform_info(const uam_compiler *compiler, int index, uam_uniform_info_t *info);

// Gets the total size of the driver constbuf in bytes
uint32_t uam_get_constbuf_size(const uam_compiler *compiler);

// Sampler metadata (for ES 1.00 auto-bound samplers)
typedef struct {
    const char *name;         // Sampler uniform name
    int binding;              // Texture descriptor binding (0, 1, 2...)
    uint8_t type;             // 0=sampler2D, 1=samplerCube
} uam_sampler_info_t;

// Gets the number of sampler uniforms in the compiled shader
int uam_get_num_samplers(const uam_compiler *compiler);

// Gets sampler info by index (0..num_samplers-1)
// Returns false if index out of range
bool uam_get_sampler_info(const uam_compiler *compiler, int index, uam_sampler_info_t *info);

// Vertex input (attribute) metadata (for vertex shaders)
typedef struct {
    const char *name;         // Attribute name
    int location;             // Generic attribute location (0-based)
    uint8_t base_type;        // Mesa glsl_base_type: 0=uint, 1=int, 2=float, 11=bool
    uint8_t vector_elements;  // 1-4
    uint8_t matrix_columns;   // 1 for scalars/vectors, 2-4 for matrices
    uint8_t pad;
} uam_input_info_t;

// Gets the number of vertex inputs (attributes) in the compiled shader
int uam_get_num_inputs(const uam_compiler *compiler);

// Gets vertex input info by index (0..num_inputs-1)
// Returns false if index out of range
bool uam_get_input_info(const uam_compiler *compiler, int index, uam_input_info_t *info);

// Returns true if driver constbuf was remapped from c[0] to UBO 0 (c[1]).
// When true, SwitchGLES must bind a UBO at id=0 with uniform data.
bool uam_is_constbuf_remapped(const uam_compiler *compiler);

// Gets the initial constant buffer data (for literals/state embedded by Mesa).
// Returns NULL if no data. *size is set to the data size in bytes.
// The returned pointer is valid until the compiler is freed.
const void *uam_get_constbuf_initial_data(const uam_compiler *compiler, uint32_t *size);

// Gets the byte offset of gl_DepthRange in the driver constbuf.
// Returns -1 if the shader doesn't use gl_DepthRange.
// The depth range occupies a vec4 at this offset: [near, far, diff, unused].
int uam_get_depth_range_offset(const uam_compiler *compiler);

#ifdef __cplusplus
}
#endif // __cplusplus
