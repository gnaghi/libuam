#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_dump.h"

#include "codegen/nv50_ir_driver.h"

#include "glsl_frontend.h"
#include "spirv_frontend.h"

#include "nv_attributes.h"
#include "nv_shader_header.h"
#include "dksh.h"

class DekoCompiler
{
	pipeline_stage m_stage;
	glsl_program m_glsl;
	const struct tgsi_token* m_tgsi;
	unsigned int m_tgsiNumTokens;
	nv50_ir_prog_info m_info;
	void* m_code;
	uint32_t m_codeSize;
	void* m_data;
	uint32_t m_dataSize;

	NvShaderHeader m_nvsh;
	DkshProgramHeader m_dkph;

	std::string m_errorLog;

	/* Driver constbuf uniform metadata (for ES 1.00 bare uniforms) */
	glsl_uniform_info_t m_uniforms[GLSL_UNIFORM_MAX];
	int m_numUniforms;
	uint32_t m_constbufSize;

	/* Sampler metadata (for ES 1.00 auto-bound samplers) */
	glsl_sampler_info_t m_samplers[GLSL_SAMPLER_MAX];
	int m_numSamplers;

	/* Vertex input (attribute) metadata */
	glsl_input_info_t m_inputs[GLSL_INPUT_MAX];
	int m_numInputs;

	/* gl_DepthRange offset in driver constbuf (-1 = not used) */
	int m_depthRangeOffset;

	/* Attribute bindings (from glBindAttribLocation) */
	glsl_attrib_binding_t m_attribBindings[GLSL_ATTRIB_BINDING_MAX];
	int m_numAttribBindings;

	void RetrieveAndPadCode();
	void GenerateHeaders();

public:
	DekoCompiler(pipeline_stage stage, int optLevel = 3);
	~DekoCompiler();

	bool CompileGlsl(const char* glsl);
	bool CompileSpirv(const uint32_t* words, size_t wordCount);
	void OutputDksh(const char* dkshFile);
	void OutputRawCode(const char* rawFile);
	void OutputTgsi(const char* tgsiFile);

	void OutputDkshToMemory(void *mem) const;
	size_t CalculateDkshSize() const;

	const char* GetErrorLog() const { return m_errorLog.c_str(); }
	int GetNumGprs() const { return m_dkph.num_gprs; }
	uint32_t GetCodeSize() const { return m_codeSize; }

	/* Uniform metadata accessors */
	int GetNumUniforms() const { return m_numUniforms; }
	const glsl_uniform_info_t* GetUniformInfo(int index) const {
		return (index >= 0 && index < m_numUniforms) ? &m_uniforms[index] : nullptr;
	}
	uint32_t GetConstbufSize() const { return m_constbufSize; }
	bool IsConstbufRemapped() const { return m_numUniforms > 0 || m_numSamplers > 0; }
	const void* GetConstbufData() const { return m_data; }
	uint32_t GetConstbufDataSize() const { return m_dataSize; }

	/* Sampler metadata accessors */
	int GetNumSamplers() const { return m_numSamplers; }
	const glsl_sampler_info_t* GetSamplerInfo(int index) const {
		return (index >= 0 && index < m_numSamplers) ? &m_samplers[index] : nullptr;
	}

	/* Vertex input (attribute) metadata accessors */
	int GetNumInputs() const { return m_numInputs; }
	const glsl_input_info_t* GetInputInfo(int index) const {
		return (index >= 0 && index < m_numInputs) ? &m_inputs[index] : nullptr;
	}

	/* gl_DepthRange offset (-1 if not used by shader) */
	int GetDepthRangeOffset() const { return m_depthRangeOffset; }

	/* Attribute binding (call before CompileGlsl) */
	void SetAttribBinding(const char *name, int location);
};
