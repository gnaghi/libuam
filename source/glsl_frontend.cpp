#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "glsl/ast.h"
#include "glsl/glsl_parser_extras.h"
#include "glsl/ir_optimization.h"
#include "glsl/program.h"
#include "glsl/loop_analysis.h"
#include "glsl/standalone_scaffolding.h"
#include "glsl/string_to_uint_map.h"
#include "util/set.h"
#include "glsl/linker.h"
#include "glsl/glsl_parser_extras.h"
#include "glsl/builtin_functions.h"
#include "glsl/opt_add_neg_to_sub.h"
#include "main/mtypes.h"
#include "program/program.h"
#include "state_tracker/st_glsl_to_tgsi.h"
#include "tgsi/tgsi_from_mesa.h"
#include "glsl/ir_uniform.h"
#include "pipe/p_state.h"

extern "C"
{
#include "tgsi/tgsi_parse.h"
}

#include "glsl_frontend.h"

/* Uniform metadata storage — populated during glsl_program_create() */
static glsl_uniform_info_t s_uniforms[GLSL_UNIFORM_MAX];
static int s_num_uniforms = 0;
static uint32_t s_constbuf_size = 0;

/* Sampler metadata storage — populated during glsl_program_create() */
static glsl_sampler_info_t s_samplers[GLSL_SAMPLER_MAX];
static int s_num_samplers = 0;

/* Vertex input (attribute) metadata — populated during glsl_program_create() */
static glsl_input_info_t s_inputs[GLSL_INPUT_MAX];
static int s_num_inputs = 0;

/* gl_DepthRange offset in driver constbuf (-1 = not used by this shader) */
static int s_depth_range_offset = -1;

/* Attribute bindings — set by caller before glsl_program_create() */
static glsl_attrib_binding_t s_attrib_bindings[GLSL_ATTRIB_BINDING_MAX];
static int s_num_attrib_bindings = 0;

void glsl_frontend_set_attrib_bindings(const glsl_attrib_binding_t *bindings, int count)
{
	s_num_attrib_bindings = 0;
	if (!bindings || count <= 0) return;
	if (count > GLSL_ATTRIB_BINDING_MAX) count = GLSL_ATTRIB_BINDING_MAX;
	for (int i = 0; i < count; i++)
		s_attrib_bindings[i] = bindings[i];
	s_num_attrib_bindings = count;
}

class dead_variable_visitor : public ir_hierarchical_visitor {
public:
	dead_variable_visitor()
	{
		variables = _mesa_set_create(NULL,
									_mesa_hash_pointer,
									_mesa_key_pointer_equal);
	}

	virtual ~dead_variable_visitor()
	{
		_mesa_set_destroy(variables, NULL);
	}

	virtual ir_visitor_status visit(ir_variable *ir)
	{
		/* If the variable is auto or temp, add it to the set of variables that
		* are candidates for removal.
		*/
		if (ir->data.mode != ir_var_auto && ir->data.mode != ir_var_temporary)
			return visit_continue;

		_mesa_set_add(variables, ir);

		return visit_continue;
	}

	virtual ir_visitor_status visit(ir_dereference_variable *ir)
	{
		struct set_entry *entry = _mesa_set_search(variables, ir->var);

		/* If a variable is dereferenced at all, remove it from the set of
		* variables that are candidates for removal.
		*/
		if (entry != NULL)
			_mesa_set_remove(variables, entry);

		return visit_continue;
	}

	void remove_dead_variables()
	{
		set_foreach(variables, entry) {
			ir_variable *ir = (ir_variable *) entry->key;

			assert(ir->ir_type == ir_type_variable);
			ir->remove();
		}
	}

private:
	set *variables;
};

struct gl_program_with_tgsi : public gl_program
{
	struct glsl_to_tgsi_visitor *glsl_to_tgsi;
	const tgsi_token *tgsi_tokens;
	unsigned int tgsi_num_tokens;
	int8_t vtx_in_locations[PIPE_MAX_ATTRIBS];

	void cleanup()
	{
		if (glsl_to_tgsi)
		{
			free_glsl_to_tgsi_visitor(glsl_to_tgsi);
			glsl_to_tgsi = NULL;
		}
		if (tgsi_tokens)
		{
			tgsi_free_tokens(tgsi_tokens);
			tgsi_tokens = NULL;
			tgsi_num_tokens = 0;
		}
	}

	static gl_program_with_tgsi* from_ptr(void* p)
	{
		return static_cast<gl_program_with_tgsi*>(p);;
	}
};

static void
destroy_gl_program_with_tgsi(void* p)
{
	gl_program_with_tgsi::from_ptr(p)->cleanup();
}

static void
init_gl_program(struct gl_program *prog, GLenum target, bool is_arb_asm)
{
	prog->RefCount = 1;
	prog->Target = target;
	prog->Format = GL_PROGRAM_FORMAT_ASCII_ARB;
	prog->info.stage = (gl_shader_stage)_mesa_program_enum_to_shader_stage(target);
	prog->is_arb_asm = is_arb_asm;
}

static struct gl_program *
new_program(UNUSED struct gl_context *ctx, GLenum target,
            UNUSED GLuint id, bool is_arb_asm)
{
	switch (target) {
	case GL_VERTEX_PROGRAM_ARB: /* == GL_VERTEX_PROGRAM_NV */
	case GL_GEOMETRY_PROGRAM_NV:
	case GL_TESS_CONTROL_PROGRAM_NV:
	case GL_TESS_EVALUATION_PROGRAM_NV:
	case GL_FRAGMENT_PROGRAM_ARB:
	case GL_COMPUTE_PROGRAM_NV: {
		struct gl_program_with_tgsi *prog = rzalloc(NULL, struct gl_program_with_tgsi);
		ralloc_set_destructor(prog, destroy_gl_program_with_tgsi);
		init_gl_program(prog, target, is_arb_asm);
		return prog;
	}
	default:
		printf("bad target in new_program\n");
		return NULL;
	}
}

void
attach_visitor_to_program(struct gl_program *prog, struct glsl_to_tgsi_visitor *v)
{
	gl_program_with_tgsi* prg = gl_program_with_tgsi::from_ptr(prog);
	prg->cleanup();
	prg->glsl_to_tgsi = v;
}

struct glsl_to_tgsi_visitor*
_glsl_program_get_tgsi_visitor(struct gl_program *prog)
{
	return gl_program_with_tgsi::from_ptr(prog)->glsl_to_tgsi;
}

void
_glsl_program_attach_tgsi_tokens(struct gl_program *prog, const tgsi_token *tokens, unsigned int num)
{
	gl_program_with_tgsi* prg = gl_program_with_tgsi::from_ptr(prog);
	prg->cleanup();
	prg->tgsi_tokens = tokens;
	prg->tgsi_num_tokens = num;
}

static void
initialize_context(struct gl_context *ctx, gl_api api)
{
	initialize_context_to_defaults(ctx, api);

	ctx->Const.MaxPatchVertices = MAX_PATCH_VERTICES;

	// Adapted from st_init_extensions
	ctx->Const.GLSLVersion = 460;
	ctx->Const.NativeIntegers = GL_TRUE;
	ctx->Const.MaxClipPlanes = 8;
	ctx->Const.UniformBooleanTrue = ~0U;
	ctx->Const.MaxSamples = 8;
	ctx->Const.MaxImageSamples = 8;
	ctx->Const.MaxColorTextureSamples = 8;
	ctx->Const.MaxDepthTextureSamples = 8;
	ctx->Const.MaxIntegerSamples = 8;
	ctx->Const.MaxFramebufferSamples = 8;
	ctx->Const.MaxColorFramebufferSamples = 8;
	ctx->Const.MaxColorFramebufferStorageSamples = 8;
	ctx->Const.MaxDepthStencilFramebufferSamples = 8;
	ctx->Const.MinMapBufferAlignment = 64;
	ctx->Const.MaxTextureBufferSize = 128 * 1024 * 1024;
	ctx->Const.TextureBufferOffsetAlignment = 16;
	ctx->Const.MaxViewports = 16;
	ctx->Const.ViewportBounds.Min = -32768.0;
	ctx->Const.ViewportBounds.Max = 32767.0;
	ctx->Const.MaxComputeWorkGroupInvocations = 1024;
	ctx->Const.MaxComputeSharedMemorySize = 64 << 10;
	ctx->Const.MaxComputeWorkGroupCount[0] = 0x7fffffff;
	ctx->Const.MaxComputeWorkGroupCount[1] = 65535;
	ctx->Const.MaxComputeWorkGroupCount[2] = 65535;
	ctx->Const.MaxComputeWorkGroupSize[0] = 1024;
	ctx->Const.MaxComputeWorkGroupSize[1] = 1024;
	ctx->Const.MaxComputeWorkGroupSize[2] = 64;
	ctx->Const.MaxComputeVariableGroupSize[0] = 1024;
	ctx->Const.MaxComputeVariableGroupSize[1] = 1024;
	ctx->Const.MaxComputeVariableGroupSize[2] = 64;
	ctx->Const.MaxComputeVariableGroupInvocations = 1024;
	ctx->Const.NoPrimitiveBoundingBoxOutput = true;

	// Adapted from st_init_limits
	ctx->Const.MaxTextureLevels = 15;
	ctx->Const.Max3DTextureLevels = 12;
	ctx->Const.MaxCubeTextureLevels = 15;
	ctx->Const.MaxTextureRectSize = 16384;
	ctx->Const.MaxArrayTextureLayers = 2048;
	ctx->Const.MaxViewportWidth =
	ctx->Const.MaxViewportHeight =
	ctx->Const.MaxRenderbufferSize = ctx->Const.MaxTextureRectSize;
	ctx->Const.SubPixelBits = 8;
	ctx->Const.ViewportSubpixelBits = 8;
	ctx->Const.MaxDrawBuffers = ctx->Const.MaxColorAttachments = 1;
	ctx->Const.MaxDualSourceDrawBuffers = 1;
	ctx->Const.MaxLineWidth = 10.0f;
	ctx->Const.MaxLineWidthAA = 10.0f;
	ctx->Const.MaxPointSize = 63.0f;
	ctx->Const.MaxPointSizeAA = 63.375f;
	ctx->Const.MinPointSize = 1.0f;
	ctx->Const.MinPointSizeAA = 0.0f;
	ctx->Const.MaxTextureMaxAnisotropy = 16.0f;
	ctx->Const.MaxTextureLodBias = 15.0f;
	ctx->Const.QuadsFollowProvokingVertexConvention = GL_TRUE;
	ctx->Const.MaxUniformBlockSize = 65536;
	for (unsigned sh = 0; sh < PIPE_SHADER_TYPES; ++sh)
	{
		gl_program_constants *pc = &ctx->Const.Program[sh];
		gl_shader_compiler_options *options = &ctx->Const.ShaderCompilerOptions[tgsi_processor_to_shader_stage(sh)];
		pc->MaxTextureImageUnits = 16;
		pc->MaxInstructions = pc->MaxNativeInstructions = 16384;
		pc->MaxAluInstructions = pc->MaxNativeAluInstructions = 16384;
		pc->MaxTexInstructions = pc->MaxNativeTexInstructions = 16384;
		pc->MaxTexIndirections = pc->MaxNativeTexIndirections = 16384;
		pc->MaxNativeAttribs = sh == PIPE_SHADER_VERTEX ? 16 : (sh == PIPE_SHADER_FRAGMENT ? (0x1f0 / 16) : (0x200 / 16));
		/* MaxAttribs=32 for vertex shaders: allows GLES2 aliased-inactive
		 * attribute shaders (2×GL_MAX_VERTEX_ATTRIBS).  Hardware limit stays
		 * 16 via MaxNativeAttribs.  Do NOT raise beyond 32: NV50_IR will
		 * generate shaders with >16 native inputs, crashing the GPU. */
		pc->MaxAttribs = sh == PIPE_SHADER_VERTEX ? 32 : pc->MaxNativeAttribs;
		pc->MaxTemps = pc->MaxNativeTemps = 128;
		pc->MaxAddressRegs = pc->MaxNativeAddressRegs = sh == PIPE_SHADER_VERTEX ? 1 : 0;
		pc->MaxUniformComponents = 256*4;
		pc->MaxParameters = pc->MaxNativeParameters = pc->MaxUniformComponents / 4;
		pc->MaxInputComponents = pc->MaxAttribs*4;
		pc->MaxOutputComponents = 32*4;
		pc->MaxUniformBlocks = 16; // fincs-note: this is custom - also this doesn't count driver ubos or blockless uniforms
		pc->MaxCombinedUniformComponents = pc->MaxUniformComponents + uint64_t(ctx->Const.MaxUniformBlockSize) / 4 * pc->MaxUniformBlocks;
		pc->MaxShaderStorageBlocks = 16; // fincs-note: this is also custom
		pc->MaxAtomicCounters = 0; // fincs-note: we don't support atomic counters
		pc->MaxAtomicBuffers = 0; // same here
		pc->MaxImageUniforms = 8;
		pc->MaxLocalParams = 4096;
		pc->MaxEnvParams = 4096;
		pc->LowInt.RangeMin = 31;
		pc->LowInt.RangeMax = 30;
		pc->LowInt.Precision = 0;
		pc->MediumInt = pc->HighInt = pc->LowInt;
		options->MaxIfDepth = 16;
		options->EmitNoIndirectOutput = sh == PIPE_SHADER_FRAGMENT ? GL_TRUE : GL_FALSE;
		options->MaxUnrollIterations = 16384;
		options->LowerCombinedClipCullDistance = GL_TRUE;
		options->LowerBufferInterfaceBlocks = GL_TRUE;
	}

	ctx->Const.MaxUserAssignableUniformLocations =
		ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformComponents +
		ctx->Const.Program[MESA_SHADER_TESS_CTRL].MaxUniformComponents +
		ctx->Const.Program[MESA_SHADER_TESS_EVAL].MaxUniformComponents +
		ctx->Const.Program[MESA_SHADER_GEOMETRY].MaxUniformComponents +
		ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxUniformComponents;

	ctx->Const.LowerTessLevel = GL_TRUE;
	ctx->Const.LowerCsDerivedVariables = GL_TRUE;
	ctx->Const.PrimitiveRestartForPatches = GL_TRUE;

	ctx->Const.MaxCombinedTextureImageUnits = 16;

	ctx->Const.MaxVarying = 15;
	ctx->Const.MaxGeometryOutputVertices = 1024;
	ctx->Const.MaxGeometryTotalOutputComponents = 1024;
	ctx->Const.MaxGeometryShaderInvocations = 32;
	ctx->Const.MaxTessPatchComponents = 30*4;
	ctx->Const.MinProgramTexelOffset = -8;
	ctx->Const.MaxProgramTexelOffset = 7;
	ctx->Const.MaxProgramTextureGatherComponents = 4;
	ctx->Const.MinProgramTextureGatherOffset = -32;
	ctx->Const.MaxProgramTextureGatherOffset = 31;
	ctx->Const.MaxTransformFeedbackBuffers = 4;
	ctx->Const.MaxTransformFeedbackSeparateComponents = 128;
	ctx->Const.MaxTransformFeedbackInterleavedComponents = 128;
	ctx->Const.MaxVertexStreams = 4;
	ctx->Const.MaxVertexAttribStride = 2048;
	ctx->Const.UniformBufferOffsetAlignment = 256;
	ctx->Const.MaxCombinedUniformBlocks = ctx->Const.MaxUniformBufferBindings =
		ctx->Const.Program[MESA_SHADER_VERTEX].MaxUniformBlocks; // fincs-note: this is custom (only 1)
	ctx->Const.GLSLFrontFacingIsSysVal = GL_TRUE;
	ctx->Const.MaxCombinedShaderOutputResources = ctx->Const.MaxDrawBuffers;
	ctx->Const.ShaderStorageBufferOffsetAlignment = 16;
	ctx->Const.MaxCombinedShaderStorageBlocks = ctx->Const.MaxShaderStorageBufferBindings =
		ctx->Const.Program[MESA_SHADER_VERTEX].MaxShaderStorageBlocks; // fincs-note: this is custom (only 1)
	ctx->Const.MaxCombinedShaderOutputResources += ctx->Const.MaxCombinedShaderStorageBlocks;
	ctx->Const.MaxShaderStorageBlockSize = 1 << 27;
	ctx->Const.MaxCombinedImageUniforms =
		ctx->Const.Program[MESA_SHADER_VERTEX].MaxImageUniforms; // fincs-note: this is custom (only 1)
	ctx->Const.MaxCombinedShaderOutputResources += ctx->Const.MaxCombinedImageUniforms;
	ctx->Const.MaxImageUnits = ctx->Const.Program[MESA_SHADER_VERTEX].MaxImageUniforms; // fincs-note: this is also custom
	ctx->Const.MaxFramebufferWidth = ctx->Const.MaxViewportWidth;
	ctx->Const.MaxFramebufferHeight = ctx->Const.MaxViewportHeight;
	ctx->Const.MaxFramebufferLayers = 2048;
	ctx->Const.MaxWindowRectangles = 8;
	ctx->Const.AllowMappedBuffersDuringExecution = GL_TRUE;
	ctx->Const.MaxSubpixelPrecisionBiasBits = 8;
	ctx->Const.ConservativeRasterDilateRange[0] = 0.0f;
	ctx->Const.ConservativeRasterDilateRange[1] = 0.75f;
	ctx->Const.ConservativeRasterDilateGranularity = 0.25f;
	// end

	ctx->Driver.NewProgram = new_program;
}

static struct gl_context gl_ctx;
static int glsl_frontend_refcount = 0;

/* Error log capture */
static char s_log_buffer[8192];
static size_t s_log_offset = 0;

void glsl_frontend_reset_log()
{
	s_log_offset = 0;
	s_log_buffer[0] = '\0';
}

const char* glsl_frontend_get_log()
{
	return s_log_buffer;
}

void glsl_frontend_log(const char *fmt, ...)
{
	va_list args;

	/* Write to stderr */
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	/* Also capture to buffer */
	if (s_log_offset < sizeof(s_log_buffer) - 1)
	{
		va_start(args, fmt);
		int n = vsnprintf(s_log_buffer + s_log_offset,
		                  sizeof(s_log_buffer) - s_log_offset, fmt, args);
		if (n > 0)
			s_log_offset += (size_t)n;
		va_end(args);
	}
}

void glsl_frontend_init()
{
	if (glsl_frontend_refcount++ == 0)
		initialize_context(&gl_ctx, API_OPENGL_CORE);
}

void glsl_frontend_exit()
{
	if (--glsl_frontend_refcount == 0)
	{
		_mesa_glsl_release_types();
		_mesa_glsl_release_builtin_functions();
	}
}

// Prototypes for translation functions
bool tgsi_translate_vertex(struct gl_context *ctx, struct gl_program *prog, int8_t *out_inlocations);
bool tgsi_translate_tessctrl(struct gl_context *ctx, struct gl_program *prog);
bool tgsi_translate_tesseval(struct gl_context *ctx, struct gl_program *prog);
bool tgsi_translate_geometry(struct gl_context *ctx, struct gl_program *prog);
bool tgsi_translate_fragment(struct gl_context *ctx, struct gl_program *prog);
bool tgsi_translate_compute(struct gl_context *ctx, struct gl_program *prog);

glsl_program glsl_program_create(const char* source, pipeline_stage stage)
{
	struct gl_shader_program *prg;

	prg = rzalloc (NULL, struct gl_shader_program);
	assert(prg != NULL);
	prg->data = rzalloc(prg, struct gl_shader_program_data);
	assert(prg->data != NULL);
	prg->data->InfoLog = ralloc_strdup(prg->data, "");
	prg->SeparateShader = true;
	exec_list_make_empty(&prg->EmptyUniformLocations);

	/* Created just to avoid segmentation faults */
	prg->AttributeBindings = new string_to_uint_map;
	prg->FragDataBindings = new string_to_uint_map;
	prg->FragDataIndexBindings = new string_to_uint_map;

	// Allocate a shader list
	prg->Shaders = reralloc(prg, prg->Shaders, struct gl_shader *, 1);

	// Allocate a shader and add it to the list
	struct gl_shader *shader = rzalloc(prg, gl_shader);
	prg->Shaders[prg->NumShaders] = shader;
	prg->NumShaders++;

	switch (stage)
	{
		case pipeline_stage_vertex:
			shader->Type = GL_VERTEX_SHADER;
			break;
		case pipeline_stage_tess_ctrl:
			shader->Type = GL_TESS_CONTROL_SHADER;
			break;
		case pipeline_stage_tess_eval:
			shader->Type = GL_TESS_EVALUATION_SHADER;
			break;
		case pipeline_stage_geometry:
			shader->Type = GL_GEOMETRY_SHADER;
			break;
		case pipeline_stage_fragment:
			shader->Type = GL_FRAGMENT_SHADER;
			break;
		case pipeline_stage_compute:
			shader->Type = GL_COMPUTE_SHADER;
			break;
		default:
			goto _fail;
	}
	shader->Stage = _mesa_shader_enum_to_shader_stage(shader->Type);

	/* GLES2 spec: shaders without #version are implicitly ES 1.00.
	 * Mesa with API_OPENGL_CORE requires an explicit #version directive
	 * to recognize ES keywords (attribute, varying, etc.), so prepend
	 * "#version 100\n" when the source lacks a #version directive.
	 * "#line 1" resets line numbering so __LINE__ matches the original source. */
	{
		const char *p = source;
		/* Skip whitespace AND comments before #version (GLSL spec allows them) */
		for (;;) {
			while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
			if (p[0] == '/' && p[1] == '/') { /* line comment */
				while (*p && *p != '\n') p++;
				continue;
			}
			if (p[0] == '/' && p[1] == '*') { /* block comment */
				p += 2;
				while (*p && !(p[0] == '*' && p[1] == '/')) p++;
				if (*p) p += 2;
				continue;
			}
			break;
		}
		if (strncmp(p, "#version", 8) != 0) {
			size_t len = strlen(source);
			char *patched = ralloc_array(prg, char, len + 22);
			memcpy(patched, "#version 100\n#line 1\n", 21);
			memcpy(patched + 21, source, len + 1);
			shader->Source = patched;
		} else {
			shader->Source = source;
		}
	}

	// "Compile" the shader
	_mesa_glsl_compile_shader(&gl_ctx, shader, false, false, true);
	if (shader->CompileStatus != COMPILE_SUCCESS)
	{
		glsl_frontend_log("Shader failed to compile.\n");
		if (shader->InfoLog && shader->InfoLog[0])
			glsl_frontend_log("%s\n", shader->InfoLog);
		goto _fail;
	}
	_mesa_clear_shader_program_data(&gl_ctx, prg);

	/* Auto-assign bindings to unbound sampler/image uniforms (ES 1.00
	 * doesn't have explicit binding layout qualifiers). Assign sequential
	 * binding numbers starting from 0, matching GLES2 default behavior
	 * where sampler N reads from texture unit N. */
	{
		int next_sampler_binding = 0;
		foreach_in_list(ir_instruction, node, shader->ir) {
			ir_variable *var = node->as_variable();
			if (!var || var->data.mode != ir_var_uniform)
				continue;
			const glsl_type *type = var->type->without_array();
			if ((type->is_sampler() || type->is_image()) &&
			    !var->data.explicit_binding) {
				var->data.explicit_binding = true;
				var->data.binding = next_sampler_binding;
				next_sampler_binding++;
			}
		}
	}

	/* Apply attribute bindings from glBindAttribLocation before linking */
	for (int i = 0; i < s_num_attrib_bindings; i++) {
		prg->AttributeBindings->put(
			VERT_ATTRIB_GENERIC0 + s_attrib_bindings[i].location,
			s_attrib_bindings[i].name);
	}

	// Link the shader
	link_shaders(&gl_ctx, prg);
	if (prg->data->LinkStatus != LINKING_SUCCESS)
	{
		glsl_frontend_log("Shader failed to link.\n");
		if (prg->data->InfoLog && prg->data->InfoLog[0])
			glsl_frontend_log("%s\n", prg->data->InfoLog);
		goto _fail;
	}
	else
	{
		struct gl_linked_shader *linked_shader = prg->_LinkedShaders[shader->Stage];

		// Do more optimizations
		add_neg_to_sub_visitor v;
		visit_list_elements(&v, linked_shader->ir);

		dead_variable_visitor dv;
		visit_list_elements(&dv, linked_shader->ir);
		dv.remove_dead_variables();

		// Print IR
		//_mesa_print_ir(stdout, linked_shader->ir, NULL);

		/* Collect vertex input (attribute) metadata from linked shader IR.
		 * Must be done BEFORE st_link_shader which may destroy IR nodes. */
		s_num_inputs = 0;
		if (stage == pipeline_stage_vertex) {
			foreach_in_list(ir_instruction, node, linked_shader->ir) {
				ir_variable *var = node->as_variable();
				if (!var || var->data.mode != ir_var_shader_in)
					continue;
				/* Skip built-in inputs (gl_VertexID, etc.) */
				if (var->data.location < (int)VERT_ATTRIB_GENERIC0)
					continue;
				if (s_num_inputs >= GLSL_INPUT_MAX)
					break;

				glsl_input_info_t *inp = &s_inputs[s_num_inputs];
				strncpy(inp->name, var->name, GLSL_UNIFORM_MAX_NAME - 1);
				inp->name[GLSL_UNIFORM_MAX_NAME - 1] = '\0';
				inp->location = var->data.location - VERT_ATTRIB_GENERIC0;
				inp->base_type = var->type->base_type;
				inp->vector_elements = var->type->vector_elements;
				inp->matrix_columns = var->type->matrix_columns;
				inp->pad = 0;
				s_num_inputs++;
			}
		}

		// Do the TGSI conversion
		if (!st_link_shader(&gl_ctx, prg))
		{
			glsl_frontend_log("st_link_shader failed\n");
			goto _fail;
		}

		// Force OriginUpperLeft
		if (linked_shader->Program->OriginUpperLeft)
			glsl_frontend_log("warning: origin_upper_left has no effect\n");
		linked_shader->Program->OriginUpperLeft = GL_TRUE;

		// Check for PixelCenterInteger (unsupported)
		if (linked_shader->Program->PixelCenterInteger == GL_TRUE) {
			glsl_frontend_log("error: pixel_center_integer is not supported\n");
			goto _fail;
		}

		// TGSI generation
		bool rc = false;
		switch (stage)
		{
			case pipeline_stage_vertex:
				rc = tgsi_translate_vertex(&gl_ctx, linked_shader->Program,
					gl_program_with_tgsi::from_ptr(linked_shader->Program)->vtx_in_locations);
				break;
			case pipeline_stage_tess_ctrl:
				rc = tgsi_translate_tessctrl(&gl_ctx, linked_shader->Program);
				break;
			case pipeline_stage_tess_eval:
				rc = tgsi_translate_tesseval(&gl_ctx, linked_shader->Program);
				break;
			case pipeline_stage_geometry:
				rc = tgsi_translate_geometry(&gl_ctx, linked_shader->Program);
				break;
			case pipeline_stage_fragment:
				rc = tgsi_translate_fragment(&gl_ctx, linked_shader->Program);
				break;
			case pipeline_stage_compute:
				rc = tgsi_translate_compute(&gl_ctx, linked_shader->Program);
				break;
			default:
				glsl_frontend_log("Unsupported stage\n");
				goto _fail;
		}

		if (!rc)
		{
			glsl_frontend_log("Translation failed\n");
			goto _fail;
		}

		/* Collect driver constbuf uniform metadata instead of rejecting.
		 * This enables direct ES 1.00 compilation where bare uniforms
		 * (without UBO blocks) go into the driver constbuf c[0x1].
		 */
		s_num_uniforms = 0;
		s_constbuf_size = 0;
		gl_program_parameter_list *pl = linked_shader->Program->Parameters;
		unsigned last_location = ~0U;
		for (unsigned i = 0; i < pl->NumParameters; i ++)
		{
			gl_program_parameter *p = &pl->Parameters[i];
			unsigned location = 0;
			if (!prg->UniformHash->get(location, p->Name))
				continue;
			gl_uniform_storage *storage = &prg->data->UniformStorage[location];
			if (storage->builtin || storage->hidden)
				continue;
			if (location != last_location && s_num_uniforms < GLSL_UNIFORM_MAX)
			{
				last_location = location;
				glsl_uniform_info_t *u = &s_uniforms[s_num_uniforms];
				strncpy(u->name, p->Name, GLSL_UNIFORM_MAX_NAME - 1);
				u->name[GLSL_UNIFORM_MAX_NAME - 1] = '\0';
				u->offset = 4 * pl->ParameterValueOffset[i];
				u->base_type = storage->type->base_type;
				u->vector_elements = storage->type->vector_elements;
				u->matrix_columns = storage->type->matrix_columns;
				u->is_sampler = storage->type->is_sampler() ? 1 : 0;
				u->array_elements = storage->array_elements;

				/* Compute size: vec4-aligned rows × columns × array */
				unsigned cols = storage->type->matrix_columns;
				unsigned rows = storage->type->vector_elements;
				unsigned count = storage->array_elements ? storage->array_elements : 1;
				unsigned elem_size;
				if (cols > 1) {
					/* Matrices: each column is padded to vec4 (16 bytes) */
					elem_size = 16 * cols;
				} else if (count > 1) {
					/* Array elements: each occupies a full vec4 (16 bytes)
					 * due to pad_and_align=true in _mesa_add_parameter */
					elem_size = 16;
				} else {
					/* Scalar/vector non-array: tight packing */
					elem_size = 4 * rows;
				}
				u->size_bytes = elem_size * count;

				uint32_t end = u->offset + u->size_bytes;
				if (end > s_constbuf_size)
					s_constbuf_size = end;

				s_num_uniforms++;
			}
		}

		/* Fallback: scan UniformStorage for uniforms not found via Parameters.
		 * Mesa may exclude boolean uniforms from the parameter list (lowered to int). */
		for (unsigned i = 0; i < prg->data->NumUniformStorage && s_num_uniforms < GLSL_UNIFORM_MAX; i++)
		{
			gl_uniform_storage *storage = &prg->data->UniformStorage[i];
			if (storage->builtin || storage->hidden) continue;
			if (storage->type->is_sampler()) continue;
			/* Check if already collected */
			bool already = false;
			for (int j = 0; j < s_num_uniforms; j++) {
				if (strcmp(s_uniforms[j].name, storage->name) == 0) {
					already = true;
					break;
				}
			}
			if (already) continue;
			/* Find offset in parameter list */
			int param_offset = -1;
			for (unsigned pi = 0; pi < pl->NumParameters; pi++) {
				if (strcmp(pl->Parameters[pi].Name, storage->name) == 0) {
					param_offset = (int)(4 * pl->ParameterValueOffset[pi]);
					break;
				}
			}
			if (param_offset < 0) continue;
			glsl_uniform_info_t *u = &s_uniforms[s_num_uniforms];
			strncpy(u->name, storage->name, GLSL_UNIFORM_MAX_NAME - 1);
			u->name[GLSL_UNIFORM_MAX_NAME - 1] = '\0';
			u->offset = (uint32_t)param_offset;
			u->base_type = storage->type->base_type;
			u->vector_elements = storage->type->vector_elements;
			u->matrix_columns = storage->type->matrix_columns;
			u->is_sampler = 0;
			u->array_elements = storage->array_elements;
			unsigned cols = storage->type->matrix_columns;
			unsigned rows = storage->type->vector_elements;
			unsigned count = storage->array_elements ? storage->array_elements : 1;
			unsigned elem_size;
			if (cols > 1) elem_size = 16 * cols;
			else if (count > 1) elem_size = 16;
			else elem_size = 4 * rows;
			u->size_bytes = elem_size * count;
			uint32_t end = u->offset + u->size_bytes;
			if (end > s_constbuf_size) s_constbuf_size = end;
			s_num_uniforms++;
		}

		/* Scan parameter list for gl_DepthRange state variable.
		 * Mesa maps gl_DepthRange to STATE_DEPTH_RANGE as a vec4 in the
		 * driver constbuf: [near, far, diff, unused]. Record the byte offset
		 * so SwitchGLES can update these values at draw time. */
		s_depth_range_offset = -1;
		for (unsigned i = 0; i < pl->NumParameters; i++)
		{
			gl_program_parameter *p = &pl->Parameters[i];
			if (p->Type == PROGRAM_STATE_VAR &&
			    p->StateIndexes[0] == STATE_DEPTH_RANGE)
			{
				s_depth_range_offset = (int)(4 * pl->ParameterValueOffset[i]);
				uint32_t end = (uint32_t)s_depth_range_offset + 16;
				if (end > s_constbuf_size)
					s_constbuf_size = end;
				break;
			}
		}

		/* Collect sampler metadata from UniformStorage.
		 * Mesa reports sampler arrays as a single gl_uniform_storage entry
		 * with name "s" and array_elements=N.  Expand into individual entries
		 * "s[0]", "s[1]", ... so SwitchGLES can look up each element by name.
		 * Each array element gets a sequential binding (base + i). */
		s_num_samplers = 0;
		for (unsigned i = 0; i < prg->data->NumUniformStorage && s_num_samplers < GLSL_SAMPLER_MAX; i++)
		{
			gl_uniform_storage *storage = &prg->data->UniformStorage[i];
			if (storage->builtin || storage->hidden) continue;
			if (!storage->type->is_sampler()) continue;

			/* Find the base binding from opaque index (check all stages) */
			int base_binding = -1;
			for (int st = 0; st < MESA_SHADER_STAGES; st++) {
				if (storage->opaque[st].active) {
					base_binding = storage->opaque[st].index;
					break;
				}
			}
			/* Determine sampler type */
			uint8_t stype = 0; /* default: sampler2D */
			if (storage->type->sampler_dimensionality == GLSL_SAMPLER_DIM_CUBE)
				stype = 1; /* samplerCube */

			unsigned count = storage->array_elements ? storage->array_elements : 1;
			if (count == 1) {
				/* Non-array sampler: store as-is */
				glsl_sampler_info_t *s = &s_samplers[s_num_samplers];
				strncpy(s->name, storage->name, GLSL_UNIFORM_MAX_NAME - 1);
				s->name[GLSL_UNIFORM_MAX_NAME - 1] = '\0';
				s->binding = base_binding;
				s->type = stype;
				s_num_samplers++;
			} else {
				/* Array sampler: expand to individual entries */
				for (unsigned e = 0; e < count && s_num_samplers < GLSL_SAMPLER_MAX; e++) {
					glsl_sampler_info_t *s = &s_samplers[s_num_samplers];
					snprintf(s->name, GLSL_UNIFORM_MAX_NAME, "%s[%u]", storage->name, e);
					s->binding = (base_binding >= 0) ? base_binding + (int)e : -1;
					s->type = stype;
					s_num_samplers++;
				}
			}
		}

	}

	return prg;

_fail:
	glsl_program_free(prg);
	return NULL;
}

static struct gl_linked_shader *_glsl_program_get_linked_shader(glsl_program prg)
{
	struct gl_linked_shader *linked_shader = NULL;
	for (int i = 0; !linked_shader && i < MESA_SHADER_STAGES; i ++)
		linked_shader = prg->_LinkedShaders[i];
	return linked_shader;
}

const tgsi_token* glsl_program_get_tokens(glsl_program prg, unsigned int& num_tokens)
{
	struct gl_linked_shader *linked_shader = _glsl_program_get_linked_shader(prg);
	if (!linked_shader)
	{
		num_tokens = 0;
		return NULL;
	}

	gl_program_with_tgsi* prog = gl_program_with_tgsi::from_ptr(linked_shader->Program);
	num_tokens = prog->tgsi_num_tokens;
	return prog->tgsi_tokens;
}

void* glsl_program_get_constant_buffer(glsl_program prg, unsigned int& out_size)
{
	struct gl_linked_shader *linked_shader = _glsl_program_get_linked_shader(prg);
	if (!linked_shader)
	{
		out_size = 0;
		return NULL;
	}

	gl_program_parameter_list *pl = linked_shader->Program->Parameters;
	out_size = 4*pl->NumParameterValues;
	return pl->ParameterValues;
}

int8_t const* glsl_program_vertex_get_in_locations(glsl_program prg)
{
	struct gl_linked_shader *linked_shader = _glsl_program_get_linked_shader(prg);
	if (!linked_shader)
		return nullptr;

	return gl_program_with_tgsi::from_ptr(linked_shader->Program)->vtx_in_locations;
}

unsigned glsl_program_compute_get_shared_size(glsl_program prg)
{
	struct gl_linked_shader *linked_shader = _glsl_program_get_linked_shader(prg);
	if (!linked_shader)
		return 0;

	return linked_shader->Program->info.cs.shared_size;
}

void glsl_program_free(glsl_program prg)
{
	for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
		if (prg->_LinkedShaders[i])
			ralloc_free(prg->_LinkedShaders[i]->Program);
	}

	delete prg->AttributeBindings;
	delete prg->FragDataBindings;
	delete prg->FragDataIndexBindings;

	ralloc_free(prg);
}

int glsl_program_get_num_uniforms(glsl_program prg)
{
	(void)prg;
	return s_num_uniforms;
}

const glsl_uniform_info_t* glsl_program_get_uniform_info(glsl_program prg, int index)
{
	(void)prg;
	if (index < 0 || index >= s_num_uniforms)
		return nullptr;
	return &s_uniforms[index];
}

uint32_t glsl_program_get_constbuf_size(glsl_program prg)
{
	(void)prg;
	return s_constbuf_size;
}

int glsl_program_get_num_samplers(glsl_program prg)
{
	(void)prg;
	return s_num_samplers;
}

const glsl_sampler_info_t* glsl_program_get_sampler_info(glsl_program prg, int index)
{
	(void)prg;
	if (index < 0 || index >= s_num_samplers)
		return nullptr;
	return &s_samplers[index];
}

int glsl_program_get_num_inputs(glsl_program prg)
{
	(void)prg;
	return s_num_inputs;
}

const glsl_input_info_t* glsl_program_get_input_info(glsl_program prg, int index)
{
	(void)prg;
	if (index < 0 || index >= s_num_inputs)
		return nullptr;
	return &s_inputs[index];
}

int glsl_program_get_depth_range_offset(glsl_program prg)
{
	(void)prg;
	return s_depth_range_offset;
}
