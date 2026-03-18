#include "uam.h"
#include "compiler_iface.h"

void uam_get_version(int *major, int *minor, int *micro) {
    if (major)
        *major = UAM_VERSION_MAJOR;
    if (minor)
        *minor = UAM_VERSION_MINOR;
    if (micro)
        *micro = UAM_VERSION_MICRO;
}

int uam_get_version_nb() {
    return (UAM_VERSION_MAJOR << 16) | (UAM_VERSION_MINOR << 8) | (UAM_VERSION_MICRO << 0);
}

inline pipeline_stage map_pipeline_stage(DkStage stage) {
    switch (stage) {
        case DkStage_Vertex:
            return pipeline_stage_vertex;
        case DkStage_TessCtrl:
            return pipeline_stage_tess_ctrl;
        case DkStage_TessEval:
            return pipeline_stage_tess_eval;
        case DkStage_Geometry:
            return pipeline_stage_geometry;
        case DkStage_Fragment:
            return pipeline_stage_fragment;
        case DkStage_Compute:
            return pipeline_stage_compute;
        default:
            return static_cast<pipeline_stage>(-1);
    }
}

uam_compiler *uam_create_compiler(DkStage stage) {
    auto pstage = map_pipeline_stage(stage);
    if (pstage == static_cast<pipeline_stage>(-1))
        return NULL;

    return reinterpret_cast<uam_compiler *>(new DekoCompiler(pstage));
}

uam_compiler *uam_create_compiler_ex(DkStage stage, int opt_level) {
    auto pstage = map_pipeline_stage(stage);
    if (pstage == static_cast<pipeline_stage>(-1))
        return NULL;

    return reinterpret_cast<uam_compiler *>(new DekoCompiler(pstage, opt_level));
}

void uam_free_compiler(uam_compiler *compiler) {
    delete reinterpret_cast<DekoCompiler *>(compiler);
}

void uam_set_attrib_binding(uam_compiler *compiler, const char *name, int location) {
    reinterpret_cast<DekoCompiler *>(compiler)->SetAttribBinding(name, location);
}

bool uam_compile_dksh(uam_compiler *compiler, const char *glsl) {
    return reinterpret_cast<DekoCompiler *>(compiler)->CompileGlsl(glsl);
}

bool uam_compile_spirv(uam_compiler *compiler, const void *spirv_data, size_t spirv_size) {
    if (!spirv_data || spirv_size < 20 || (spirv_size % 4) != 0)
        return false;
    return reinterpret_cast<DekoCompiler *>(compiler)->CompileSpirv(
        static_cast<const uint32_t *>(spirv_data), spirv_size / 4);
}

size_t uam_get_code_size(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->CalculateDkshSize();
}

void uam_write_code(const uam_compiler *compiler, void *memory) {
    reinterpret_cast<const DekoCompiler *>(compiler)->OutputDkshToMemory(memory);
}

const char *uam_get_error_log(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetErrorLog();
}

int uam_get_num_gprs(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetNumGprs();
}

unsigned int uam_get_raw_code_size(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetCodeSize();
}

int uam_get_num_uniforms(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetNumUniforms();
}

bool uam_get_uniform_info(const uam_compiler *compiler, int index, uam_uniform_info_t *info) {
    const glsl_uniform_info_t *src =
        reinterpret_cast<const DekoCompiler *>(compiler)->GetUniformInfo(index);
    if (!src || !info)
        return false;

    info->name = src->name;
    info->offset = src->offset;
    info->size_bytes = src->size_bytes;
    info->base_type = src->base_type;
    info->vector_elements = src->vector_elements;
    info->matrix_columns = src->matrix_columns;
    info->is_sampler = src->is_sampler;
    info->array_elements = src->array_elements;
    return true;
}

uint32_t uam_get_constbuf_size(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetConstbufSize();
}

int uam_get_num_samplers(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetNumSamplers();
}

bool uam_get_sampler_info(const uam_compiler *compiler, int index, uam_sampler_info_t *info) {
    const glsl_sampler_info_t *src =
        reinterpret_cast<const DekoCompiler *>(compiler)->GetSamplerInfo(index);
    if (!src || !info)
        return false;

    info->name = src->name;
    info->binding = src->binding;
    info->type = src->type;
    return true;
}

int uam_get_num_inputs(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetNumInputs();
}

bool uam_get_input_info(const uam_compiler *compiler, int index, uam_input_info_t *info) {
    const glsl_input_info_t *src =
        reinterpret_cast<const DekoCompiler *>(compiler)->GetInputInfo(index);
    if (!src || !info)
        return false;

    info->name = src->name;
    info->location = src->location;
    info->base_type = src->base_type;
    info->vector_elements = src->vector_elements;
    info->matrix_columns = src->matrix_columns;
    info->pad = 0;
    return true;
}

bool uam_is_constbuf_remapped(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->IsConstbufRemapped();
}

const void *uam_get_constbuf_initial_data(const uam_compiler *compiler, uint32_t *size) {
    const DekoCompiler *c = reinterpret_cast<const DekoCompiler *>(compiler);
    if (size)
        *size = c->GetConstbufDataSize();
    return c->GetConstbufData();
}

int uam_get_depth_range_offset(const uam_compiler *compiler) {
    return reinterpret_cast<const DekoCompiler *>(compiler)->GetDepthRangeOffset();
}
