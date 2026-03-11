#pragma once
#include <stdint.h>

struct gl_shader_program;
struct tgsi_token;

typedef struct gl_shader_program* glsl_program;

enum pipeline_stage
{
	pipeline_stage_vertex,
	pipeline_stage_tess_ctrl,
	pipeline_stage_tess_eval,
	pipeline_stage_geometry,
	pipeline_stage_fragment,
	pipeline_stage_compute,
};

/* Uniform metadata for driver constbuf uniforms (bare uniforms in ES 1.00) */
#define GLSL_UNIFORM_MAX_NAME 128
#define GLSL_UNIFORM_MAX 128

typedef struct {
	char name[GLSL_UNIFORM_MAX_NAME];
	uint32_t offset;          /* Byte offset in driver constbuf */
	uint32_t size_bytes;      /* Total size in bytes */
	uint8_t  base_type;       /* GLSL_TYPE_FLOAT, GLSL_TYPE_INT, GLSL_TYPE_BOOL, GLSL_TYPE_SAMPLER */
	uint8_t  vector_elements; /* 1-4 */
	uint8_t  matrix_columns;  /* 1 for scalars/vectors, 2-4 for matrices */
	uint8_t  is_sampler;      /* 1 if sampler type */
	uint32_t array_elements;  /* 0 for non-array, N for array[N] */
} glsl_uniform_info_t;

void glsl_frontend_init();
void glsl_frontend_exit();

/* Error log capture - call reset before compilation, get_log after */
void glsl_frontend_reset_log();
const char* glsl_frontend_get_log();
void glsl_frontend_log(const char *fmt, ...);

glsl_program glsl_program_create(const char* source, pipeline_stage stage);
const tgsi_token* glsl_program_get_tokens(glsl_program prg, unsigned int& num_tokens);
void* glsl_program_get_constant_buffer(glsl_program prg, unsigned int& out_size);
int8_t const* glsl_program_vertex_get_in_locations(glsl_program prg);
unsigned glsl_program_compute_get_shared_size(glsl_program prg);
void glsl_program_free(glsl_program prg);

/* Query uniform metadata from compiled program (driver constbuf uniforms) */
int glsl_program_get_num_uniforms(glsl_program prg);
const glsl_uniform_info_t* glsl_program_get_uniform_info(glsl_program prg, int index);
uint32_t glsl_program_get_constbuf_size(glsl_program prg);

/* Sampler metadata for ES 1.00 shaders */
#define GLSL_SAMPLER_MAX 16

typedef struct {
	char name[GLSL_UNIFORM_MAX_NAME];
	int binding;     /* Texture descriptor binding (auto-assigned 0, 1, 2...) */
	uint8_t type;    /* 0=sampler2D, 1=samplerCube */
} glsl_sampler_info_t;

int glsl_program_get_num_samplers(glsl_program prg);
const glsl_sampler_info_t* glsl_program_get_sampler_info(glsl_program prg, int index);
