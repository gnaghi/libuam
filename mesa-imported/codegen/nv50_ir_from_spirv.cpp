/*
 * Copyright 2024 UAM Contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "codegen/nv50_ir.h"
#include "codegen/nv50_ir_util.h"
#include "codegen/nv50_ir_build_util.h"

#include "spirv_frontend.h"
#include "tgsi/tgsi_scan.h" // for TGSI_SEMANTIC_* used in slot assignment

#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <map>
#include <algorithm>

namespace {

using namespace nv50_ir;

// ============================================================================
// Helper: map SPIR-V BuiltIn → TGSI semantic name/index
// (needed because slot assignment uses TGSI semantics)
// ============================================================================

static void
builtInToTgsiSemantic(SpvBuiltIn bi, uint8_t& sn, uint8_t& si)
{
   si = 0;
   switch (bi) {
   case SpvBuiltInPosition:
   case SpvBuiltInFragCoord:
      sn = TGSI_SEMANTIC_POSITION;
      break;
   case SpvBuiltInPointSize:
      sn = TGSI_SEMANTIC_PSIZE;
      break;
   case SpvBuiltInClipDistance:
      sn = TGSI_SEMANTIC_CLIPDIST;
      break;
   case SpvBuiltInCullDistance:
      sn = TGSI_SEMANTIC_CLIPDIST;
      si = 2; // cull distances start after clip
      break;
   case SpvBuiltInVertexId:
   case SpvBuiltInVertexIndex:
      sn = TGSI_SEMANTIC_VERTEXID;
      break;
   case SpvBuiltInInstanceId:
   case SpvBuiltInInstanceIndex:
      sn = TGSI_SEMANTIC_INSTANCEID;
      break;
   case SpvBuiltInPrimitiveId:
      sn = TGSI_SEMANTIC_PRIMID;
      break;
   case SpvBuiltInInvocationId:
      sn = TGSI_SEMANTIC_INVOCATIONID;
      break;
   case SpvBuiltInLayer:
      sn = TGSI_SEMANTIC_LAYER;
      break;
   case SpvBuiltInViewportIndex:
      sn = TGSI_SEMANTIC_VIEWPORT_INDEX;
      break;
   case SpvBuiltInTessLevelOuter:
      sn = TGSI_SEMANTIC_TESSOUTER;
      break;
   case SpvBuiltInTessLevelInner:
      sn = TGSI_SEMANTIC_TESSINNER;
      break;
   case SpvBuiltInTessCoord:
      sn = TGSI_SEMANTIC_TESSCOORD;
      break;
   case SpvBuiltInFrontFacing:
      sn = TGSI_SEMANTIC_FACE;
      break;
   case SpvBuiltInSampleId:
      sn = TGSI_SEMANTIC_SAMPLEID;
      break;
   case SpvBuiltInSamplePosition:
      sn = TGSI_SEMANTIC_SAMPLEPOS;
      break;
   case SpvBuiltInSampleMask:
      sn = TGSI_SEMANTIC_SAMPLEMASK;
      break;
   case SpvBuiltInFragDepth:
      sn = TGSI_SEMANTIC_POSITION;
      break;
   case SpvBuiltInPointCoord:
      sn = TGSI_SEMANTIC_PCOORD;
      break;
   default:
      sn = TGSI_SEMANTIC_GENERIC;
      break;
   }
}

// ============================================================================
// Helper: map SPIR-V BuiltIn → NV50 IR SVSemantic
// ============================================================================

static SVSemantic
builtInToSVSemantic(SpvBuiltIn bi)
{
   switch (bi) {
   case SpvBuiltInPosition:
   case SpvBuiltInFragCoord:
      return SV_POSITION;
   case SpvBuiltInVertexId:
   case SpvBuiltInVertexIndex:
      return SV_VERTEX_ID;
   case SpvBuiltInInstanceId:
   case SpvBuiltInInstanceIndex:
      return SV_INSTANCE_ID;
   case SpvBuiltInPrimitiveId:
      return SV_PRIMITIVE_ID;
   case SpvBuiltInInvocationId:
      return SV_INVOCATION_ID;
   case SpvBuiltInLayer:
      return SV_LAYER;
   case SpvBuiltInViewportIndex:
      return SV_VIEWPORT_INDEX;
   case SpvBuiltInPointSize:
      return SV_POINT_SIZE;
   case SpvBuiltInPointCoord:
      return SV_POINT_COORD;
   case SpvBuiltInClipDistance:
      return SV_CLIP_DISTANCE;
   case SpvBuiltInFrontFacing:
      return SV_FACE;
   case SpvBuiltInSampleId:
      return SV_SAMPLE_INDEX;
   case SpvBuiltInSamplePosition:
      return SV_SAMPLE_POS;
   case SpvBuiltInSampleMask:
      return SV_SAMPLE_MASK;
   case SpvBuiltInFragDepth:
      return SV_POSITION;
   case SpvBuiltInTessLevelOuter:
      return SV_TESS_OUTER;
   case SpvBuiltInTessLevelInner:
      return SV_TESS_INNER;
   case SpvBuiltInTessCoord:
      return SV_TESS_COORD;
   case SpvBuiltInLocalInvocationId:
      return SV_TID;
   case SpvBuiltInWorkgroupId:
      return SV_CTAID;
   case SpvBuiltInNumWorkgroups:
      return SV_NCTAID;
   default:
      return SV_UNDEFINED;
   }
}

// ============================================================================
// Helper: SPIR-V type → NV50 IR DataType
// ============================================================================

static DataType
spvTypeToDataType(const spirv::Type* ty)
{
   if (!ty) return TYPE_U32;

   switch (ty->kind) {
   case spirv::TYPE_BOOL:
      return TYPE_U32; // booleans are 32-bit in IR
   case spirv::TYPE_INT:
      if (ty->bitWidth == 32)
         return ty->signedness ? TYPE_S32 : TYPE_U32;
      if (ty->bitWidth == 16)
         return ty->signedness ? TYPE_S16 : TYPE_U16;
      if (ty->bitWidth == 8)
         return ty->signedness ? TYPE_S8 : TYPE_U8;
      if (ty->bitWidth == 64)
         return TYPE_U64;
      return TYPE_U32;
   case spirv::TYPE_FLOAT:
      if (ty->bitWidth == 32) return TYPE_F32;
      if (ty->bitWidth == 64) return TYPE_F64;
      if (ty->bitWidth == 16) return TYPE_F16;
      return TYPE_F32;
   case spirv::TYPE_VECTOR:
   case spirv::TYPE_MATRIX:
   case spirv::TYPE_ARRAY:
   case spirv::TYPE_STRUCT:
   case spirv::TYPE_POINTER:
      return TYPE_U32; // composite access is done per-component
   default:
      return TYPE_U32;
   }
}

// Get the scalar element type of a possibly-vector type
static const spirv::Type*
getScalarType(const spirv::Program* prog, uint32_t typeId)
{
   const spirv::Type* ty = prog->getType(typeId);
   if (!ty) return nullptr;
   if (ty->kind == spirv::TYPE_VECTOR)
      return prog->getType(ty->elementTypeId);
   return ty;
}

static unsigned
getTypeComponents(const spirv::Program* prog, uint32_t typeId)
{
   const spirv::Type* ty = prog->getType(typeId);
   if (!ty) return 1;
   if (ty->kind == spirv::TYPE_VECTOR)
      return ty->componentCount;
   return 1;
}

// ============================================================================
// Converter class: SPIR-V → NV50_IR
// ============================================================================

class Converter : public BuildUtil
{
public:
   Converter(Program *ir, const spirv::Program *spv, struct nv50_ir_prog_info *info);

   bool run();

private:
   // Info population
   void scanVariables();
   void addInput(uint32_t varId, const spirv::Variable& var);
   void addOutput(uint32_t varId, const spirv::Variable& var);
   void addUniform(uint32_t varId, const spirv::Variable& var);
   void addSysVal(SpvBuiltIn bi, bool isInput);

   // Register number of components for a type (for flat/interp tracking)
   uint8_t getMaskForType(uint32_t typeId) const;

   // Function/block conversion
   bool convertFunction(const spirv::Function& func);
   bool convertBlock(const spirv::BasicBlock& block);
   bool convertInstruction(const spirv::Instruction& inst);

   // Instruction handlers
   void handleLoad(const spirv::Instruction& inst);
   void handleStore(const spirv::Instruction& inst);
   void handleAccessChain(const spirv::Instruction& inst);
   void handleVariable(const spirv::Instruction& inst);

   void handleBinaryOp(const spirv::Instruction& inst, operation op, DataType ty);
   void handleUnaryOp(const spirv::Instruction& inst, operation op, DataType ty);
   void handleCompare(const spirv::Instruction& inst, CondCode cc, DataType ty);
   void handleSelect(const spirv::Instruction& inst);
   void handleConvert(const spirv::Instruction& inst);
   void handleBitcast(const spirv::Instruction& inst);
   void handleNegate(const spirv::Instruction& inst, DataType ty);

   void handleCompositeExtract(const spirv::Instruction& inst);
   void handleCompositeConstruct(const spirv::Instruction& inst);
   void handleVectorShuffle(const spirv::Instruction& inst);
   void handleCopyObject(const spirv::Instruction& inst);

   void handleExtInst(const spirv::Instruction& inst);
   void handleGlslStd450(const spirv::Instruction& inst, GLSLstd450 op);

   void handleBranch(const spirv::Instruction& inst);
   void handleBranchConditional(const spirv::Instruction& inst);
   void handlePhi(const spirv::Instruction& inst);
   void handleReturn(const spirv::Instruction& inst);
   void handleKill(const spirv::Instruction& inst);

   void handleVectorTimesScalar(const spirv::Instruction& inst);
   void handleDot(const spirv::Instruction& inst);
   void handleMatrixTimesVector(const spirv::Instruction& inst);

   void handleBitOp(const spirv::Instruction& inst, operation op);
   void handleShift(const spirv::Instruction& inst, operation op);

   void handleImageSample(const spirv::Instruction& inst);
   void handleImageFetch(const spirv::Instruction& inst);

   void handleDerivative(const spirv::Instruction& inst, operation op);

   // Value management
   Value *getSSAValue(uint32_t id);
   void setSSAValue(uint32_t id, Value *val);
   Value *getSSAComponent(uint32_t id, unsigned c);
   void setSSAComponent(uint32_t id, unsigned c, Value *val);

   Value *loadConstant(uint32_t id);
   Value *loadConstantComponent(uint32_t id, unsigned c);

   Value *loadInput(uint32_t varId, unsigned comp);
   void storeOutput(uint32_t varId, unsigned comp, Value *val);

   // Fragment shader input interpolation
   Value *interpolateInput(unsigned idx, unsigned comp);

   // Export outputs at function end
   void exportOutputs();

   // Access chain resolution
   struct AccessInfo {
      DataFile file;
      int8_t fileIndex;
      uint32_t offset;  // byte offset
      Value *indirect;  // indirect offset value, if any
      uint32_t typeId;  // type of what's being accessed
   };
   AccessInfo resolveAccessChain(uint32_t id);

   // Helpers
   DataType getResultType(const spirv::Instruction& inst);
   unsigned getResultComponents(const spirv::Instruction& inst);

   const spirv::Program *spv;
   struct nv50_ir_prog_info *info;

   // SSA value storage: id → vector of component Values
   std::map<uint32_t, std::vector<Value*>> ssaValues;

   // Access chain results: id → AccessInfo
   std::map<uint32_t, AccessInfo> accessChains;

   // Block label → BasicBlock mapping
   std::map<uint32_t, BasicBlock*> labelToBB;

   // Variable → input/output index mapping
   std::map<uint32_t, unsigned> inputVarToIndex;
   std::map<uint32_t, unsigned> outputVarToIndex;
   std::map<uint32_t, unsigned> sysValToIndex;

   // Fragment shader: 1/w for perspective interpolation
   Value *fragCoordW;

   // Temporary data array for fragment outputs
   DataArray oData;

   // Value map for DataArray
   ValueMap valueMap;
};

Converter::Converter(Program *ir, const spirv::Program *spv,
                     struct nv50_ir_prog_info *info_)
   : BuildUtil(ir), spv(spv), info(info_), fragCoordW(nullptr), oData(this)
{
}

// ============================================================================
// Variable scanning: populate nv50_ir_prog_info in/out/sv arrays
// ============================================================================

uint8_t
Converter::getMaskForType(uint32_t typeId) const
{
   const spirv::Type* ty = spv->getType(typeId);
   if (!ty) return 0x1;
   if (ty->kind == spirv::TYPE_VECTOR) {
      uint8_t mask = 0;
      for (unsigned i = 0; i < ty->componentCount && i < 4; i++)
         mask |= (1 << i);
      return mask;
   }
   return 0x1; // scalar
}

void
Converter::addInput(uint32_t varId, const spirv::Variable& var)
{
   const spirv::DecorationData& dec = spv->getDecoration(varId);
   const spirv::Type* ptrTy = spv->getType(var.typeId);
   if (!ptrTy) return;
   const spirv::Type* pointeeTy = spv->getType(ptrTy->elementTypeId);
   if (!pointeeTy) return;

   // If the variable has a BuiltIn decoration, treat as system value
   if (dec.hasBuiltIn) {
      addSysVal(dec.builtIn, true);
      return;
   }

   // For struct types with BuiltIn members (e.g. gl_PerVertex block), each
   // member becomes a separate input/sysval
   if (pointeeTy->kind == spirv::TYPE_STRUCT) {
      for (size_t m = 0; m < pointeeTy->memberTypeIds.size(); m++) {
         auto& md = spv->getMemberDecoration(pointeeTy->id, (uint32_t)m);
         if (md.hasBuiltIn) {
            addSysVal(md.builtIn, true);
         }
      }
      return;
   }

   unsigned idx = info->numInputs++;
   inputVarToIndex[varId] = idx;

   auto& in = info->in[idx];
   memset(&in, 0, sizeof(in));
   in.id = idx;

   if (dec.location >= 0) {
      in.sn = TGSI_SEMANTIC_GENERIC;
      in.si = dec.location;
   }

   in.mask = getMaskForType(ptrTy->elementTypeId);

   if (dec.flat)
      in.flat = 1;
   if (dec.noPerspective)
      in.linear = 1;
   if (dec.centroid)
      in.centroid = 1;
}

void
Converter::addOutput(uint32_t varId, const spirv::Variable& var)
{
   const spirv::DecorationData& dec = spv->getDecoration(varId);
   const spirv::Type* ptrTy = spv->getType(var.typeId);
   if (!ptrTy) return;
   const spirv::Type* pointeeTy = spv->getType(ptrTy->elementTypeId);
   if (!pointeeTy) return;

   // BuiltIn struct (gl_PerVertex)
   if (pointeeTy->kind == spirv::TYPE_STRUCT) {
      for (size_t m = 0; m < pointeeTy->memberTypeIds.size(); m++) {
         auto& md = spv->getMemberDecoration(pointeeTy->id, (uint32_t)m);
         if (md.hasBuiltIn) {
            // For output builtins, add as proper outputs with TGSI semantics
            unsigned idx = info->numOutputs++;
            outputVarToIndex[(varId << 16) | (uint32_t)m] = idx;

            auto& out = info->out[idx];
            memset(&out, 0, sizeof(out));
            out.id = idx;
            builtInToTgsiSemantic(md.builtIn, out.sn, out.si);
            // gl_Position is vec4
            if (md.builtIn == SpvBuiltInPosition)
               out.mask = 0xf;
            else if (md.builtIn == SpvBuiltInPointSize)
               out.mask = 0x1;
            else if (md.builtIn == SpvBuiltInClipDistance)
               out.mask = 0xf;
            else
               out.mask = 0x1;
         }
      }
      return;
   }

   // BuiltIn variable
   if (dec.hasBuiltIn) {
      unsigned idx = info->numOutputs++;
      outputVarToIndex[varId] = idx;

      auto& out = info->out[idx];
      memset(&out, 0, sizeof(out));
      out.id = idx;
      builtInToTgsiSemantic(dec.builtIn, out.sn, out.si);

      if (dec.builtIn == SpvBuiltInFragDepth) {
         out.mask = 0x4; // z component
         info->io.fragDepth = idx;
         info->prop.fp.writesDepth = true;
      } else if (dec.builtIn == SpvBuiltInSampleMask) {
         out.mask = 0x1;
         info->io.sampleMask = idx;
      } else {
         out.mask = getMaskForType(ptrTy->elementTypeId);
      }
      return;
   }

   unsigned idx = info->numOutputs++;
   outputVarToIndex[varId] = idx;

   auto& out = info->out[idx];
   memset(&out, 0, sizeof(out));
   out.id = idx;

   if (info->type == PIPE_SHADER_FRAGMENT) {
      // Fragment shader outputs are color outputs
      out.sn = TGSI_SEMANTIC_COLOR;
      out.si = dec.location >= 0 ? dec.location : 0;
      out.mask = getMaskForType(ptrTy->elementTypeId);
      info->prop.fp.numColourResults = std::max(
         info->prop.fp.numColourResults,
         (unsigned)(out.si + 1));
   } else {
      out.sn = TGSI_SEMANTIC_GENERIC;
      out.si = dec.location >= 0 ? dec.location : 0;
      out.mask = getMaskForType(ptrTy->elementTypeId);
   }
}

void
Converter::addSysVal(SpvBuiltIn bi, bool isInput)
{
   uint8_t sn, si;
   builtInToTgsiSemantic(bi, sn, si);

   // Check if already added
   for (unsigned i = 0; i < info->numSysVals; i++) {
      if (info->sv[i].sn == sn && info->sv[i].si == si)
         return;
   }

   unsigned idx = info->numSysVals++;
   auto& sv = info->sv[idx];
   memset(&sv, 0, sizeof(sv));
   sv.id = idx;
   sv.sn = sn;
   sv.si = si;
   sv.input = isInput ? 1 : 0;
   sv.mask = 0x1;

   if (bi == SpvBuiltInPosition || bi == SpvBuiltInFragCoord)
      sv.mask = 0xf;
   if (bi == SpvBuiltInTessCoord)
      sv.mask = 0x7;
}

void
Converter::addUniform(uint32_t varId, const spirv::Variable& var)
{
   // UBOs are accessed via FILE_MEMORY_CONST
   // The binding decoration determines the constbuf slot
   (void)varId;
   (void)var;
}

void
Converter::scanVariables()
{
   // Initialize io fields to sentinel values
   info->io.fragDepth = PIPE_MAX_SHADER_OUTPUTS;
   info->io.sampleMask = PIPE_MAX_SHADER_OUTPUTS;
   info->io.edgeFlagIn = 0;
   info->io.edgeFlagOut = 0;
   info->io.viewportId = -1;
   info->numInputs = 0;
   info->numOutputs = 0;
   info->numSysVals = 0;
   info->numPatchConstants = 0;
   info->prop.fp.numColourResults = 0;

   // Process interface variables declared in the entry point
   for (uint32_t varId : spv->entryPointInterfaces) {
      const spirv::Variable* var = spv->getVariable(varId);
      if (!var) continue;

      switch (var->storageClass) {
      case SpvStorageClassInput:
         addInput(varId, *var);
         break;
      case SpvStorageClassOutput:
         addOutput(varId, *var);
         break;
      case SpvStorageClassUniform:
      case SpvStorageClassUniformConstant:
         addUniform(varId, *var);
         break;
      default:
         break;
      }
   }

   // Also scan all variables (some may not be in the interface list in older SPIR-V)
   for (size_t i = 0; i < spv->allVariables.size(); i++) {
      const spirv::Variable& var = spv->allVariables[i];
      if (spv->variables[var.id] == nullptr) continue;

      // Skip if already processed
      bool isInterface = false;
      for (uint32_t iid : spv->entryPointInterfaces) {
         if (iid == var.id) { isInterface = true; break; }
      }
      if (isInterface) continue;

      switch (var.storageClass) {
      case SpvStorageClassInput:
         addInput(var.id, var);
         break;
      case SpvStorageClassOutput:
         addOutput(var.id, var);
         break;
      case SpvStorageClassUniform:
      case SpvStorageClassUniformConstant:
         addUniform(var.id, var);
         break;
      default:
         break;
      }
   }
}

// ============================================================================
// Value management
// ============================================================================

Value *
Converter::getSSAValue(uint32_t id)
{
   auto it = ssaValues.find(id);
   if (it != ssaValues.end() && !it->second.empty())
      return it->second[0];

   // Check if it's a constant
   const spirv::Constant* c = spv->getConstant(id);
   if (c)
      return loadConstant(id);

   return nullptr;
}

void
Converter::setSSAValue(uint32_t id, Value *val)
{
   ssaValues[id] = { val };
}

Value *
Converter::getSSAComponent(uint32_t id, unsigned c)
{
   auto it = ssaValues.find(id);
   if (it != ssaValues.end() && c < it->second.size())
      return it->second[c];

   // Try constant
   const spirv::Constant* cnst = spv->getConstant(id);
   if (cnst)
      return loadConstantComponent(id, c);

   // If scalar value exists and c==0, return it
   if (it != ssaValues.end() && !it->second.empty() && c == 0)
      return it->second[0];

   return nullptr;
}

void
Converter::setSSAComponent(uint32_t id, unsigned c, Value *val)
{
   auto& vec = ssaValues[id];
   if (vec.size() <= c)
      vec.resize(c + 1, nullptr);
   vec[c] = val;
}

Value *
Converter::loadConstant(uint32_t id)
{
   return loadConstantComponent(id, 0);
}

Value *
Converter::loadConstantComponent(uint32_t id, unsigned c)
{
   const spirv::Constant* cnst = spv->getConstant(id);
   if (!cnst) return loadImm(nullptr, 0u);

   if (cnst->isNull || cnst->isFalse)
      return loadImm(nullptr, 0u);
   if (cnst->isTrue)
      return loadImm(nullptr, (uint32_t)0xffffffff);

   if (cnst->isComposite) {
      if (c < cnst->constituents.size())
         return loadConstantComponent(cnst->constituents[c], 0);
      return loadImm(nullptr, 0u);
   }

   const spirv::Type* ty = spv->getType(cnst->typeId);
   if (ty && ty->kind == spirv::TYPE_FLOAT && ty->bitWidth == 32)
      return loadImm(nullptr, cnst->value.f32);
   return loadImm(nullptr, cnst->value.u32);
}

// ============================================================================
// Input/Output access
// ============================================================================

Value *
Converter::loadInput(uint32_t varId, unsigned comp)
{
   auto it = inputVarToIndex.find(varId);
   if (it == inputVarToIndex.end())
      return loadImm(nullptr, 0u);

   unsigned idx = it->second;

   if (prog->getType() == Program::TYPE_FRAGMENT)
      return interpolateInput(idx, comp);

   Symbol *sym = mkSymbol(FILE_SHADER_INPUT, 0, TYPE_F32,
                           info->in[idx].slot[comp] * 4);
   return mkLoadv(TYPE_U32, sym, nullptr);
}

Value *
Converter::interpolateInput(unsigned idx, unsigned comp)
{
   // Check if the input is actually used (has a slot assigned)
   if (!(info->in[idx].mask & (1 << comp)))
      return loadImm(nullptr, comp == 3 ? 1.0f : 0.0f);

   Symbol *sym = mkSymbol(FILE_SHADER_INPUT, 0, TYPE_F32,
                           info->in[idx].slot[comp] * 4);

   operation op;
   uint8_t mode = NV50_IR_INTERP_PERSPECTIVE;

   if (info->in[idx].flat)
      mode = NV50_IR_INTERP_FLAT;
   else if (info->in[idx].linear)
      mode = NV50_IR_INTERP_LINEAR;

   op = (mode == NV50_IR_INTERP_PERSPECTIVE) ? OP_PINTERP : OP_LINTERP;

   if (info->in[idx].centroid)
      mode |= NV50_IR_INTERP_CENTROID;

   Instruction *insn = new_Instruction(func, op, TYPE_F32);
   insn->setDef(0, getScratch());
   insn->setSrc(0, sym);
   if (op == OP_PINTERP)
      insn->setSrc(1, fragCoordW);
   insn->setInterpolate(mode);
   bb->insertTail(insn);
   return insn->getDef(0);
}

void
Converter::storeOutput(uint32_t varId, unsigned comp, Value *val)
{
   auto it = outputVarToIndex.find(varId);
   if (it == outputVarToIndex.end())
      return;

   unsigned idx = it->second;

   if (prog->getType() == Program::TYPE_FRAGMENT) {
      // Store to oData array for later export
      oData.store(valueMap, idx, comp, nullptr, val);
   } else {
      Symbol *sym = mkSymbol(FILE_SHADER_OUTPUT, 0, TYPE_F32,
                              info->out[idx].slot[comp] * 4);
      mkStore(OP_EXPORT, TYPE_F32, sym, nullptr, val);
   }
}

void
Converter::exportOutputs()
{
   if (prog->getType() != Program::TYPE_FRAGMENT)
      return;

   for (unsigned i = 0; i < info->numOutputs; ++i) {
      for (unsigned c = 0; c < 4; ++c) {
         if (!oData.exists(valueMap, i, c))
            continue;
         Symbol *sym = mkSymbol(FILE_SHADER_OUTPUT, 0, TYPE_F32,
                                 info->out[i].slot[c] * 4);
         Value *val = oData.load(valueMap, i, c, nullptr);
         if (val) {
            if (info->out[i].sn == TGSI_SEMANTIC_POSITION)
               mkOp1(OP_SAT, TYPE_F32, val, val);
            mkStore(OP_EXPORT, TYPE_F32, sym, nullptr, val);
         }
      }
   }
}

// ============================================================================
// Access chain resolution
// ============================================================================

Converter::AccessInfo
Converter::resolveAccessChain(uint32_t id)
{
   auto it = accessChains.find(id);
   if (it != accessChains.end())
      return it->second;

   // Not an access chain, check if it's a variable
   const spirv::Variable* var = spv->getVariable(id);
   if (var) {
      AccessInfo ai;
      ai.indirect = nullptr;

      const spirv::Type* ptrTy = spv->getType(var->typeId);
      ai.typeId = ptrTy ? ptrTy->elementTypeId : 0;

      switch (var->storageClass) {
      case SpvStorageClassInput:
         ai.file = FILE_SHADER_INPUT;
         ai.fileIndex = 0;
         ai.offset = 0;
         break;
      case SpvStorageClassOutput:
         ai.file = FILE_SHADER_OUTPUT;
         ai.fileIndex = 0;
         ai.offset = 0;
         break;
      case SpvStorageClassUniform:
      case SpvStorageClassUniformConstant: {
         ai.file = FILE_MEMORY_CONST;
         auto& dec = spv->getDecoration(id);
         // binding maps to constbuf slot (binding+1 because slot 0 is driver)
         ai.fileIndex = dec.binding >= 0 ? (dec.binding + 1) : 1;
         ai.offset = 0;
         break;
      }
      case SpvStorageClassPushConstant:
         ai.file = FILE_MEMORY_CONST;
         ai.fileIndex = 0;
         ai.offset = 0;
         break;
      case SpvStorageClassFunction:
      case SpvStorageClassPrivate:
         ai.file = FILE_MEMORY_LOCAL;
         ai.fileIndex = 0;
         ai.offset = 0;
         break;
      default:
         ai.file = FILE_GPR;
         ai.fileIndex = 0;
         ai.offset = 0;
         break;
      }
      return ai;
   }

   // Fallback
   AccessInfo ai;
   ai.file = FILE_GPR;
   ai.fileIndex = 0;
   ai.offset = 0;
   ai.indirect = nullptr;
   ai.typeId = 0;
   return ai;
}

// ============================================================================
// Result type helpers
// ============================================================================

DataType
Converter::getResultType(const spirv::Instruction& inst)
{
   if (!inst.hasResultType()) return TYPE_U32;
   const spirv::Type* ty = spv->getType(inst.resultType());
   if (!ty) return TYPE_U32;
   // For vector types, use the element type
   if (ty->kind == spirv::TYPE_VECTOR)
      return spvTypeToDataType(spv->getType(ty->elementTypeId));
   return spvTypeToDataType(ty);
}

unsigned
Converter::getResultComponents(const spirv::Instruction& inst)
{
   if (!inst.hasResultType()) return 1;
   return getTypeComponents(spv, inst.resultType());
}

// ============================================================================
// Instruction conversion
// ============================================================================

void
Converter::handleBinaryOp(const spirv::Instruction& inst, operation op, DataType ty)
{
   uint32_t resId = inst.resultId();
   uint32_t src0Id = inst.word(3);
   uint32_t src1Id = inst.word(4);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *s0 = getSSAComponent(src0Id, c);
      Value *s1 = getSSAComponent(src1Id, c);
      if (!s0) s0 = loadImm(nullptr, 0u);
      if (!s1) s1 = loadImm(nullptr, 0u);
      Value *dst = getSSA();
      mkOp2(op, ty, dst, s0, s1);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleUnaryOp(const spirv::Instruction& inst, operation op, DataType ty)
{
   uint32_t resId = inst.resultId();
   uint32_t srcId = inst.word(3);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *s = getSSAComponent(srcId, c);
      if (!s) s = loadImm(nullptr, 0u);
      Value *dst = getSSA();
      mkOp1(op, ty, dst, s);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleNegate(const spirv::Instruction& inst, DataType ty)
{
   uint32_t resId = inst.resultId();
   uint32_t srcId = inst.word(3);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *s = getSSAComponent(srcId, c);
      if (!s) s = loadImm(nullptr, 0u);
      Value *dst = getSSA();
      if (isFloatType(ty))
         mkOp1(OP_NEG, ty, dst, s);
      else
         mkOp2(OP_SUB, ty, dst, loadImm(nullptr, 0u), s);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleCompare(const spirv::Instruction& inst, CondCode cc, DataType ty)
{
   uint32_t resId = inst.resultId();
   uint32_t src0Id = inst.word(3);
   uint32_t src1Id = inst.word(4);
   unsigned comps = getTypeComponents(spv, spv->getType(inst.resultType()) ?
                                      inst.resultType() : 0);
   // Compare operations: result components = components of the operands
   unsigned srcComps = getTypeComponents(spv,
      spv->getConstant(src0Id) ? spv->getConstant(src0Id)->typeId :
      inst.word(1) /* result type is bool/bvecN, operands define component count */);

   // For comparison ops, the result is bool (or boolN), but sources
   // determine the number of components. Let's look at the source type.
   // Actually, for scalar compare, result is bool (1 comp). For vector compare,
   // result is bvecN matching source dimensions.
   comps = getTypeComponents(spv, inst.resultType());

   for (unsigned c = 0; c < comps; c++) {
      Value *s0 = getSSAComponent(src0Id, c);
      Value *s1 = getSSAComponent(src1Id, c);
      if (!s0) s0 = loadImm(nullptr, 0u);
      if (!s1) s1 = loadImm(nullptr, 0u);
      Value *dst = getSSA();
      mkCmp(OP_SET, cc, TYPE_U32, dst, ty, s0, s1);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleSelect(const spirv::Instruction& inst)
{
   // OpSelect %result %cond %trueVal %falseVal
   uint32_t resId = inst.resultId();
   uint32_t condId = inst.word(3);
   uint32_t trueId = inst.word(4);
   uint32_t falseId = inst.word(5);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *cond = getSSAComponent(condId, c < getTypeComponents(spv, spv->getConstant(condId) ?
         spv->getConstant(condId)->typeId : 0) ? c : 0);
      if (!cond) cond = getSSAComponent(condId, 0);
      if (!cond) cond = loadImm(nullptr, 0u);
      Value *tVal = getSSAComponent(trueId, c);
      Value *fVal = getSSAComponent(falseId, c);
      if (!tVal) tVal = loadImm(nullptr, 0u);
      if (!fVal) fVal = loadImm(nullptr, 0u);

      Value *pred = getSSA(1, FILE_PREDICATE);
      mkCmp(OP_SET, CC_NE, TYPE_U32, pred, TYPE_U32, cond, loadImm(nullptr, 0u));

      Value *dst = getSSA();
      mkOp3(OP_SELP, TYPE_U32, dst, tVal, fVal, pred);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleConvert(const spirv::Instruction& inst)
{
   uint32_t resId = inst.resultId();
   uint32_t srcId = inst.word(3);
   unsigned comps = getResultComponents(inst);
   DataType dstTy = getResultType(inst);
   // Infer source type from the source value's SPIR-V type
   DataType srcTy = TYPE_U32;

   // Determine source type based on the opcode
   switch (inst.opcode) {
   case SpvOpConvertFToU:
      srcTy = TYPE_F32;
      dstTy = TYPE_U32;
      break;
   case SpvOpConvertFToS:
      srcTy = TYPE_F32;
      dstTy = TYPE_S32;
      break;
   case SpvOpConvertSToF:
      srcTy = TYPE_S32;
      dstTy = TYPE_F32;
      break;
   case SpvOpConvertUToF:
      srcTy = TYPE_U32;
      dstTy = TYPE_F32;
      break;
   default:
      break;
   }

   for (unsigned c = 0; c < comps; c++) {
      Value *s = getSSAComponent(srcId, c);
      if (!s) s = loadImm(nullptr, 0u);
      Value *dst = getSSA();
      mkCvt(OP_CVT, dstTy, dst, srcTy, s);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleBitcast(const spirv::Instruction& inst)
{
   uint32_t resId = inst.resultId();
   uint32_t srcId = inst.word(3);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *s = getSSAComponent(srcId, c);
      if (!s) s = loadImm(nullptr, 0u);
      // Bitcast is a no-op at the IR level (just reinterpret bits)
      Value *dst = getSSA();
      mkOp1(OP_MOV, TYPE_U32, dst, s);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleCompositeExtract(const spirv::Instruction& inst)
{
   // OpCompositeExtract %resultType %result %composite index0 [index1...]
   uint32_t resId = inst.resultId();
   uint32_t compositeId = inst.word(3);

   // For now handle single-level extraction (vectors)
   if (inst.wordCount >= 5) {
      uint32_t index = inst.word(4);

      // Check if result is a vector (multi-level extract)
      unsigned resComps = getResultComponents(inst);
      if (resComps == 1) {
         Value *val = getSSAComponent(compositeId, index);
         if (!val) val = loadImm(nullptr, 0u);
         setSSAComponent(resId, 0, val);
      } else {
         // Extracting a vector from a matrix or nested structure
         // For matrices: index selects column, result is a vector
         for (unsigned c = 0; c < resComps; c++) {
            Value *val = getSSAComponent(compositeId, index * resComps + c);
            if (!val) val = loadImm(nullptr, 0u);
            setSSAComponent(resId, c, val);
         }
      }

      // Handle deeper indexing (e.g., extract scalar from matrix)
      if (inst.wordCount >= 6) {
         uint32_t index2 = inst.word(5);
         Value *val = getSSAComponent(compositeId, index * 4 + index2);
         if (!val) {
            // Fallback: re-extract from what we got
            val = getSSAComponent(resId, index2);
         }
         if (!val) val = loadImm(nullptr, 0u);
         ssaValues[resId] = { val };
      }
   }
}

void
Converter::handleCompositeConstruct(const spirv::Instruction& inst)
{
   // OpCompositeConstruct %resultType %result %constituent0 %constituent1 ...
   uint32_t resId = inst.resultId();
   unsigned numConstituents = inst.wordCount - 3;

   for (unsigned i = 0; i < numConstituents; i++) {
      uint32_t constituentId = inst.word(3 + i);
      unsigned srcComps = getTypeComponents(spv,
         spv->getConstant(constituentId) ? spv->getConstant(constituentId)->typeId : 0);

      if (srcComps <= 1) {
         Value *val = getSSAComponent(constituentId, 0);
         if (!val) val = loadImm(nullptr, 0u);
         setSSAComponent(resId, i, val);
      } else {
         // Source is a vector; copy all its components
         // This handles cases like constructing a vec4 from vec2+vec2
         for (unsigned c = 0; c < srcComps; c++) {
            Value *val = getSSAComponent(constituentId, c);
            if (!val) val = loadImm(nullptr, 0u);
            setSSAComponent(resId, i + c, val);
         }
         i += srcComps - 1; // skip ahead
      }
   }
}

void
Converter::handleVectorShuffle(const spirv::Instruction& inst)
{
   // OpVectorShuffle %resultType %result %vec1 %vec2 literal0 literal1 ...
   uint32_t resId = inst.resultId();
   uint32_t vec1Id = inst.word(3);
   uint32_t vec2Id = inst.word(4);
   unsigned vec1Comps = getTypeComponents(spv,
      spv->getConstant(vec1Id) ? spv->getConstant(vec1Id)->typeId : 0);
   // If we can't determine vec1 components from constant, try the type system
   if (vec1Comps <= 1) {
      // Check ssaValues
      auto it = ssaValues.find(vec1Id);
      if (it != ssaValues.end())
         vec1Comps = (unsigned)it->second.size();
   }
   if (vec1Comps == 0) vec1Comps = 4; // assume vec4

   unsigned numComponents = inst.wordCount - 5;
   for (unsigned i = 0; i < numComponents; i++) {
      uint32_t idx = inst.word(5 + i);
      Value *val;
      if (idx == 0xFFFFFFFF) {
         val = loadImm(nullptr, 0u); // undefined component
      } else if (idx < vec1Comps) {
         val = getSSAComponent(vec1Id, idx);
      } else {
         val = getSSAComponent(vec2Id, idx - vec1Comps);
      }
      if (!val) val = loadImm(nullptr, 0u);
      setSSAComponent(resId, i, val);
   }
}

void
Converter::handleCopyObject(const spirv::Instruction& inst)
{
   uint32_t resId = inst.resultId();
   uint32_t srcId = inst.word(3);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *val = getSSAComponent(srcId, c);
      if (!val) val = loadImm(nullptr, 0u);
      setSSAComponent(resId, c, val);
   }
}

void
Converter::handleVectorTimesScalar(const spirv::Instruction& inst)
{
   uint32_t resId = inst.resultId();
   uint32_t vecId = inst.word(3);
   uint32_t scalarId = inst.word(4);
   unsigned comps = getResultComponents(inst);

   Value *scalar = getSSAComponent(scalarId, 0);
   if (!scalar) scalar = loadImm(nullptr, 0u);

   for (unsigned c = 0; c < comps; c++) {
      Value *v = getSSAComponent(vecId, c);
      if (!v) v = loadImm(nullptr, 0u);
      Value *dst = getSSA();
      mkOp2(OP_MUL, TYPE_F32, dst, v, scalar);
      setSSAComponent(resId, c, dst);
   }
}

void
Converter::handleDot(const spirv::Instruction& inst)
{
   // OpDot %resultType %result %vec1 %vec2
   uint32_t resId = inst.resultId();
   uint32_t vec1Id = inst.word(3);
   uint32_t vec2Id = inst.word(4);

   // Determine vector dimension from source type
   unsigned dim = 4;
   auto it = ssaValues.find(vec1Id);
   if (it != ssaValues.end())
      dim = (unsigned)it->second.size();
   if (dim == 0) dim = 4;

   Value *acc = nullptr;
   for (unsigned c = 0; c < dim; c++) {
      Value *a = getSSAComponent(vec1Id, c);
      Value *b = getSSAComponent(vec2Id, c);
      if (!a) a = loadImm(nullptr, 0.0f);
      if (!b) b = loadImm(nullptr, 0.0f);

      Value *prod = getSSA();
      mkOp2(OP_MUL, TYPE_F32, prod, a, b);

      if (acc) {
         Value *sum = getSSA();
         mkOp2(OP_ADD, TYPE_F32, sum, acc, prod);
         acc = sum;
      } else {
         acc = prod;
      }
   }

   setSSAComponent(resId, 0, acc);
}

void
Converter::handleMatrixTimesVector(const spirv::Instruction& inst)
{
   // OpMatrixTimesVector %resultType %result %matrix %vector
   uint32_t resId = inst.resultId();
   uint32_t matId = inst.word(3);
   uint32_t vecId = inst.word(4);
   unsigned comps = getResultComponents(inst);

   // Matrix is column-major: mat[col][row]
   // result[row] = sum(mat[col][row] * vec[col]) for each col
   const spirv::Type* resTy = spv->getType(inst.resultType());
   unsigned cols = 4;
   if (resTy && resTy->kind == spirv::TYPE_VECTOR)
      cols = resTy->componentCount;

   for (unsigned row = 0; row < comps; row++) {
      Value *acc = nullptr;
      for (unsigned col = 0; col < cols; col++) {
         Value *matElem = getSSAComponent(matId, col * comps + row);
         Value *vecElem = getSSAComponent(vecId, col);
         if (!matElem) matElem = loadImm(nullptr, 0.0f);
         if (!vecElem) vecElem = loadImm(nullptr, 0.0f);

         Value *prod = getSSA();
         mkOp2(OP_MUL, TYPE_F32, prod, matElem, vecElem);

         if (acc) {
            Value *sum = getSSA();
            mkOp2(OP_ADD, TYPE_F32, sum, acc, prod);
            acc = sum;
         } else {
            acc = prod;
         }
      }
      setSSAComponent(resId, row, acc);
   }
}

void
Converter::handleBitOp(const spirv::Instruction& inst, operation op)
{
   handleBinaryOp(inst, op, TYPE_U32);
}

void
Converter::handleShift(const spirv::Instruction& inst, operation op)
{
   handleBinaryOp(inst, op, TYPE_U32);
}

void
Converter::handleDerivative(const spirv::Instruction& inst, operation op)
{
   uint32_t resId = inst.resultId();
   uint32_t srcId = inst.word(3);
   unsigned comps = getResultComponents(inst);

   for (unsigned c = 0; c < comps; c++) {
      Value *s = getSSAComponent(srcId, c);
      if (!s) s = loadImm(nullptr, 0.0f);
      Value *dst = getSSA();
      mkOp1(op, TYPE_F32, dst, s);
      setSSAComponent(resId, c, dst);
   }
}

// ============================================================================
// Load / Store / AccessChain
// ============================================================================

void
Converter::handleAccessChain(const spirv::Instruction& inst)
{
   // OpAccessChain %resultType %result %base index0 [index1...]
   uint32_t resId = inst.resultId();
   uint32_t baseId = inst.word(3);

   AccessInfo base = resolveAccessChain(baseId);
   const spirv::Variable* baseVar = spv->getVariable(baseId);

   // Walk through indices to compute final offset
   uint32_t currentTypeId = base.typeId;
   uint32_t byteOffset = base.offset;
   Value *indirectOffset = base.indirect;

   for (uint16_t i = 4; i < inst.wordCount; i++) {
      uint32_t indexId = inst.word(i);
      const spirv::Type* currentTy = spv->getType(currentTypeId);
      if (!currentTy) break;

      const spirv::Constant* indexConst = spv->getConstant(indexId);

      switch (currentTy->kind) {
      case spirv::TYPE_STRUCT: {
         // Struct member access: index must be constant
         if (!indexConst) break;
         uint32_t member = indexConst->value.u32;
         auto& md = spv->getMemberDecoration(currentTypeId, member);
         if (md.offset >= 0)
            byteOffset += md.offset;
         if (member < currentTy->memberTypeIds.size())
            currentTypeId = currentTy->memberTypeIds[member];
         break;
      }
      case spirv::TYPE_ARRAY:
      case spirv::TYPE_RUNTIME_ARRAY: {
         uint32_t elemSize = spv->getTypeByteSize(currentTy->elementTypeId);
         if (indexConst) {
            byteOffset += indexConst->value.u32 * elemSize;
         } else {
            // Dynamic index
            Value *idx = getSSAValue(indexId);
            if (!idx) idx = loadImm(nullptr, 0u);
            Value *offset = mkOp2v(OP_MUL, TYPE_U32, getSSA(), idx,
                                    loadImm(nullptr, elemSize));
            if (indirectOffset) {
               indirectOffset = mkOp2v(OP_ADD, TYPE_U32, getSSA(),
                                        indirectOffset, offset);
            } else {
               indirectOffset = offset;
            }
         }
         currentTypeId = currentTy->elementTypeId;
         break;
      }
      case spirv::TYPE_VECTOR: {
         // Component access within a vector (e.g. vec.x)
         if (indexConst) {
            byteOffset += indexConst->value.u32 * 4;
         } else {
            Value *idx = getSSAValue(indexId);
            if (!idx) idx = loadImm(nullptr, 0u);
            Value *offset = mkOp2v(OP_SHL, TYPE_U32, getSSA(), idx,
                                    loadImm(nullptr, (uint32_t)2));
            if (indirectOffset) {
               indirectOffset = mkOp2v(OP_ADD, TYPE_U32, getSSA(),
                                        indirectOffset, offset);
            } else {
               indirectOffset = offset;
            }
         }
         currentTypeId = currentTy->elementTypeId;
         break;
      }
      case spirv::TYPE_MATRIX: {
         // Column access in a matrix
         uint32_t colSize = spv->getTypeByteSize(currentTy->elementTypeId);
         if (indexConst) {
            byteOffset += indexConst->value.u32 * colSize;
         } else {
            Value *idx = getSSAValue(indexId);
            if (!idx) idx = loadImm(nullptr, 0u);
            Value *offset = mkOp2v(OP_MUL, TYPE_U32, getSSA(), idx,
                                    loadImm(nullptr, colSize));
            if (indirectOffset) {
               indirectOffset = mkOp2v(OP_ADD, TYPE_U32, getSSA(),
                                        indirectOffset, offset);
            } else {
               indirectOffset = offset;
            }
         }
         currentTypeId = currentTy->elementTypeId;
         break;
      }
      case spirv::TYPE_POINTER:
         // Dereference pointer
         currentTypeId = currentTy->elementTypeId;
         break;
      default:
         break;
      }
   }

   AccessInfo ai;
   ai.file = base.file;
   ai.fileIndex = base.fileIndex;
   ai.offset = byteOffset;
   ai.indirect = indirectOffset;
   ai.typeId = currentTypeId;
   accessChains[resId] = ai;
}

void
Converter::handleLoad(const spirv::Instruction& inst)
{
   // OpLoad %resultType %result %pointer
   uint32_t resId = inst.resultId();
   uint32_t ptrId = inst.word(3);
   unsigned comps = getResultComponents(inst);
   DataType ty = getResultType(inst);

   // Check if it's a variable load (input/output) or memory load
   const spirv::Variable* var = spv->getVariable(ptrId);

   if (var) {
      if (var->storageClass == SpvStorageClassInput) {
         // Load from shader input
         const spirv::DecorationData& dec = spv->getDecoration(ptrId);

         // BuiltIn variable loaded as system value
         if (dec.hasBuiltIn) {
            SVSemantic sv = builtInToSVSemantic(dec.builtIn);
            for (unsigned c = 0; c < comps; c++) {
               Value *dst = getSSA();
               mkOp1(OP_RDSV, TYPE_U32, dst, mkSysVal(sv, c));
               setSSAComponent(resId, c, dst);
            }
            return;
         }

         // Struct (gl_PerVertex) — should have been decomposed in scanVariables
         const spirv::Type* ptrTy = spv->getType(var->typeId);
         const spirv::Type* pointeeTy = ptrTy ? spv->getType(ptrTy->elementTypeId) : nullptr;
         if (pointeeTy && pointeeTy->kind == spirv::TYPE_STRUCT) {
            // Loading a whole struct — expand as needed
            // This typically shouldn't happen (access chains are used instead)
            return;
         }

         for (unsigned c = 0; c < comps; c++) {
            Value *val = loadInput(ptrId, c);
            setSSAComponent(resId, c, val);
         }
         return;
      }

      if (var->storageClass == SpvStorageClassFunction ||
          var->storageClass == SpvStorageClassPrivate) {
         // Function-local variable: load from SSA
         for (unsigned c = 0; c < comps; c++) {
            Value *val = getSSAComponent(ptrId, c);
            if (!val) val = loadImm(nullptr, 0u);
            setSSAComponent(resId, c, val);
         }
         return;
      }
   }

   // Check access chains
   auto it = accessChains.find(ptrId);
   if (it != accessChains.end()) {
      AccessInfo& ai = it->second;

      if (ai.file == FILE_SHADER_INPUT) {
         // Access chain into input — find which variable and component
         // Walk back to find the base variable
         // For now, handle simple cases
         for (unsigned c = 0; c < comps; c++) {
            Symbol *sym = mkSymbol(ai.file, ai.fileIndex, TYPE_F32,
                                    ai.offset + c * 4);
            Value *val;
            if (prog->getType() == Program::TYPE_FRAGMENT) {
               // Need interpolation
               Instruction *insn = new_Instruction(func, OP_LINTERP, TYPE_F32);
               insn->setDef(0, getScratch());
               insn->setSrc(0, sym);
               insn->setInterpolate(NV50_IR_INTERP_PERSPECTIVE);
               bb->insertTail(insn);
               val = insn->getDef(0);
            } else {
               val = mkLoadv(TYPE_U32, sym, ai.indirect);
            }
            setSSAComponent(resId, c, val);
         }
         return;
      }

      if (ai.file == FILE_MEMORY_CONST) {
         for (unsigned c = 0; c < comps; c++) {
            Symbol *sym = mkSymbol(ai.file, ai.fileIndex, TYPE_U32,
                                    ai.offset + c * 4);
            Value *val = mkLoadv(ty, sym, ai.indirect);
            setSSAComponent(resId, c, val);
         }
         return;
      }

      // Memory/GPR load
      for (unsigned c = 0; c < comps; c++) {
         Value *val = getSSAComponent(ptrId, c);
         if (!val) val = loadImm(nullptr, 0u);
         setSSAComponent(resId, c, val);
      }
      return;
   }

   // Fallback: copy SSA values
   for (unsigned c = 0; c < comps; c++) {
      Value *val = getSSAComponent(ptrId, c);
      if (!val) val = loadImm(nullptr, 0u);
      setSSAComponent(resId, c, val);
   }
}

void
Converter::handleStore(const spirv::Instruction& inst)
{
   // OpStore %pointer %value
   uint32_t ptrId = inst.word(1);
   uint32_t valId = inst.word(2);

   const spirv::Variable* var = spv->getVariable(ptrId);

   if (var) {
      if (var->storageClass == SpvStorageClassOutput) {
         const spirv::DecorationData& dec = spv->getDecoration(ptrId);
         const spirv::Type* ptrTy = spv->getType(var->typeId);
         const spirv::Type* pointeeTy = ptrTy ? spv->getType(ptrTy->elementTypeId) : nullptr;

         // Struct output (gl_PerVertex)
         if (pointeeTy && pointeeTy->kind == spirv::TYPE_STRUCT) {
            // Store to whole struct — this shouldn't happen typically
            return;
         }

         // BuiltIn output
         if (dec.hasBuiltIn) {
            unsigned comps = pointeeTy ? getTypeComponents(spv, ptrTy->elementTypeId) : 1;
            for (unsigned c = 0; c < comps; c++) {
               Value *val = getSSAComponent(valId, c);
               if (!val) val = loadImm(nullptr, 0u);
               storeOutput(ptrId, c, val);
            }
            return;
         }

         // Regular output
         unsigned comps = pointeeTy ? getTypeComponents(spv, ptrTy->elementTypeId) : 1;
         for (unsigned c = 0; c < comps; c++) {
            Value *val = getSSAComponent(valId, c);
            if (!val) val = loadImm(nullptr, 0u);
            storeOutput(ptrId, c, val);
         }
         return;
      }

      if (var->storageClass == SpvStorageClassFunction ||
          var->storageClass == SpvStorageClassPrivate) {
         // Function-local variable: store as SSA
         const spirv::Type* ptrTy = spv->getType(var->typeId);
         unsigned comps = ptrTy ? getTypeComponents(spv, ptrTy->elementTypeId) : 1;
         for (unsigned c = 0; c < comps; c++) {
            Value *val = getSSAComponent(valId, c);
            if (!val) val = loadImm(nullptr, 0u);
            setSSAComponent(ptrId, c, val);
         }
         return;
      }
   }

   // Check access chains
   auto it = accessChains.find(ptrId);
   if (it != accessChains.end()) {
      AccessInfo& ai = it->second;

      if (ai.file == FILE_SHADER_OUTPUT) {
         unsigned comps = getTypeComponents(spv, ai.typeId);
         // Find which output variable this corresponds to
         for (auto& pair : outputVarToIndex) {
            // Store using the access chain offset
            unsigned idx = pair.second;
            for (unsigned c = 0; c < comps; c++) {
               Value *val = getSSAComponent(valId, c);
               if (!val) val = loadImm(nullptr, 0u);
               unsigned comp = (ai.offset / 4) + c;
               if (prog->getType() == Program::TYPE_FRAGMENT) {
                  oData.store(valueMap, idx, comp, nullptr, val);
               } else {
                  Symbol *sym = mkSymbol(FILE_SHADER_OUTPUT, 0, TYPE_F32,
                                          info->out[idx].slot[comp] * 4);
                  mkStore(OP_EXPORT, TYPE_F32, sym, nullptr, val);
               }
            }
            break; // take first match for now
         }
         return;
      }
   }

   // Fallback: SSA store
   auto jt = ssaValues.find(valId);
   if (jt != ssaValues.end()) {
      ssaValues[ptrId] = jt->second;
   }
}

void
Converter::handleVariable(const spirv::Instruction& inst)
{
   // OpVariable %resultType %result StorageClass [Initializer]
   uint32_t resId = inst.word(2);
   SpvStorageClass sc = (SpvStorageClass)inst.word(3);

   if (sc == SpvStorageClassFunction || sc == SpvStorageClassPrivate) {
      // Initialize function-local variable to zero
      const spirv::Type* ptrTy = spv->getType(inst.word(1));
      unsigned comps = ptrTy ? getTypeComponents(spv, ptrTy->elementTypeId) : 1;
      if (comps == 0) comps = 1;
      for (unsigned c = 0; c < comps; c++)
         setSSAComponent(resId, c, loadImm(nullptr, 0u));
   }
}

// ============================================================================
// Extended instruction set (GLSL.std.450)
// ============================================================================

void
Converter::handleExtInst(const spirv::Instruction& inst)
{
   // OpExtInst %resultType %result %set %instruction operands...
   uint32_t setId = inst.word(3);
   uint32_t extOp = inst.word(4);

   auto it = spv->extInstImports.find(setId);
   if (it != spv->extInstImports.end() &&
       it->second.find("GLSL.std.450") != std::string::npos) {
      handleGlslStd450(inst, (GLSLstd450)extOp);
   }
}

void
Converter::handleGlslStd450(const spirv::Instruction& inst, GLSLstd450 op)
{
   uint32_t resId = inst.resultId();
   unsigned comps = getResultComponents(inst);

   switch (op) {
   case GLSLstd450FAbs: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_ABS, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450SAbs: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0u);
         Value *dst = getSSA();
         mkOp1(OP_ABS, TYPE_S32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Floor: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_FLOOR, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Ceil: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_CEIL, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Trunc: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_TRUNC, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Sin: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *presin = getSSA();
         mkOp1(OP_PRESIN, TYPE_F32, presin, s);
         Value *dst = getSSA();
         mkOp1(OP_SIN, TYPE_F32, dst, presin);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Cos: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *presin = getSSA();
         mkOp1(OP_PRESIN, TYPE_F32, presin, s);
         Value *dst = getSSA();
         mkOp1(OP_COS, TYPE_F32, dst, presin);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Sqrt: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_SQRT, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450InverseSqrt: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_RSQ, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Exp2: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *preex2 = getSSA();
         mkOp1(OP_PREEX2, TYPE_F32, preex2, s);
         Value *dst = getSSA();
         mkOp1(OP_EX2, TYPE_F32, dst, preex2);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Log2: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp1(OP_LG2, TYPE_F32, dst, s);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Pow: {
      uint32_t baseId = inst.word(5);
      uint32_t expId = inst.word(6);
      for (unsigned c = 0; c < comps; c++) {
         Value *base = getSSAComponent(baseId, c);
         Value *exp = getSSAComponent(expId, c);
         if (!base) base = loadImm(nullptr, 0.0f);
         if (!exp) exp = loadImm(nullptr, 0.0f);
         // pow(x,y) = exp2(y * log2(x))
         Value *lg = getSSA();
         mkOp1(OP_LG2, TYPE_F32, lg, base);
         Value *mul = getSSA();
         mkOp2(OP_MUL, TYPE_F32, mul, exp, lg);
         Value *preex = getSSA();
         mkOp1(OP_PREEX2, TYPE_F32, preex, mul);
         Value *dst = getSSA();
         mkOp1(OP_EX2, TYPE_F32, dst, preex);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450FMin: {
      uint32_t aId = inst.word(5);
      uint32_t bId = inst.word(6);
      for (unsigned c = 0; c < comps; c++) {
         Value *a = getSSAComponent(aId, c);
         Value *b = getSSAComponent(bId, c);
         if (!a) a = loadImm(nullptr, 0.0f);
         if (!b) b = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp2(OP_MIN, TYPE_F32, dst, a, b);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450FMax: {
      uint32_t aId = inst.word(5);
      uint32_t bId = inst.word(6);
      for (unsigned c = 0; c < comps; c++) {
         Value *a = getSSAComponent(aId, c);
         Value *b = getSSAComponent(bId, c);
         if (!a) a = loadImm(nullptr, 0.0f);
         if (!b) b = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp2(OP_MAX, TYPE_F32, dst, a, b);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450SMin:
   case GLSLstd450UMin: {
      DataType dty = (op == GLSLstd450SMin) ? TYPE_S32 : TYPE_U32;
      uint32_t aId = inst.word(5);
      uint32_t bId = inst.word(6);
      for (unsigned c = 0; c < comps; c++) {
         Value *a = getSSAComponent(aId, c);
         Value *b = getSSAComponent(bId, c);
         if (!a) a = loadImm(nullptr, 0u);
         if (!b) b = loadImm(nullptr, 0u);
         Value *dst = getSSA();
         mkOp2(OP_MIN, dty, dst, a, b);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450SMax:
   case GLSLstd450UMax: {
      DataType dty = (op == GLSLstd450SMax) ? TYPE_S32 : TYPE_U32;
      uint32_t aId = inst.word(5);
      uint32_t bId = inst.word(6);
      for (unsigned c = 0; c < comps; c++) {
         Value *a = getSSAComponent(aId, c);
         Value *b = getSSAComponent(bId, c);
         if (!a) a = loadImm(nullptr, 0u);
         if (!b) b = loadImm(nullptr, 0u);
         Value *dst = getSSA();
         mkOp2(OP_MAX, dty, dst, a, b);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450FClamp: {
      uint32_t xId = inst.word(5);
      uint32_t minId = inst.word(6);
      uint32_t maxId = inst.word(7);
      for (unsigned c = 0; c < comps; c++) {
         Value *x = getSSAComponent(xId, c);
         Value *mn = getSSAComponent(minId, c);
         Value *mx = getSSAComponent(maxId, c);
         if (!x) x = loadImm(nullptr, 0.0f);
         if (!mn) mn = loadImm(nullptr, 0.0f);
         if (!mx) mx = loadImm(nullptr, 1.0f);
         Value *t = getSSA();
         mkOp2(OP_MAX, TYPE_F32, t, x, mn);
         Value *dst = getSSA();
         mkOp2(OP_MIN, TYPE_F32, dst, t, mx);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450SClamp:
   case GLSLstd450UClamp: {
      DataType dty = (op == GLSLstd450SClamp) ? TYPE_S32 : TYPE_U32;
      uint32_t xId = inst.word(5);
      uint32_t minId = inst.word(6);
      uint32_t maxId = inst.word(7);
      for (unsigned c = 0; c < comps; c++) {
         Value *x = getSSAComponent(xId, c);
         Value *mn = getSSAComponent(minId, c);
         Value *mx = getSSAComponent(maxId, c);
         if (!x) x = loadImm(nullptr, 0u);
         if (!mn) mn = loadImm(nullptr, 0u);
         if (!mx) mx = loadImm(nullptr, (uint32_t)0xffffffff);
         Value *t = getSSA();
         mkOp2(OP_MAX, dty, t, x, mn);
         Value *dst = getSSA();
         mkOp2(OP_MIN, dty, dst, t, mx);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450FMix: {
      // mix(x, y, a) = x * (1-a) + y * a
      uint32_t xId = inst.word(5);
      uint32_t yId = inst.word(6);
      uint32_t aId = inst.word(7);
      for (unsigned c = 0; c < comps; c++) {
         Value *x = getSSAComponent(xId, c);
         Value *y = getSSAComponent(yId, c);
         Value *a = getSSAComponent(aId, c);
         if (!x) x = loadImm(nullptr, 0.0f);
         if (!y) y = loadImm(nullptr, 0.0f);
         if (!a) a = loadImm(nullptr, 0.0f);
         // (1-a)*x + a*y = x + a*(y-x)
         Value *diff = getSSA();
         mkOp2(OP_SUB, TYPE_F32, diff, y, x);
         Value *mad = getSSA();
         mkOp3(OP_MAD, TYPE_F32, mad, a, diff, x);
         setSSAComponent(resId, c, mad);
      }
      break;
   }
   case GLSLstd450Normalize: {
      uint32_t srcId = inst.word(5);
      // normalize(v) = v / length(v) = v * rsq(dot(v,v))
      // First compute dot(v,v)
      Value *dotProd = nullptr;
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *sq = getSSA();
         mkOp2(OP_MUL, TYPE_F32, sq, s, s);
         if (dotProd) {
            Value *sum = getSSA();
            mkOp2(OP_ADD, TYPE_F32, sum, dotProd, sq);
            dotProd = sum;
         } else {
            dotProd = sq;
         }
      }
      Value *invLen = getSSA();
      mkOp1(OP_RSQ, TYPE_F32, invLen, dotProd);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkOp2(OP_MUL, TYPE_F32, dst, s, invLen);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Length: {
      uint32_t srcId = inst.word(5);
      unsigned srcComps = comps;
      auto sit = ssaValues.find(srcId);
      if (sit != ssaValues.end())
         srcComps = (unsigned)sit->second.size();
      if (srcComps == 0) srcComps = 1;

      Value *dotProd = nullptr;
      for (unsigned c = 0; c < srcComps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *sq = getSSA();
         mkOp2(OP_MUL, TYPE_F32, sq, s, s);
         if (dotProd) {
            Value *sum = getSSA();
            mkOp2(OP_ADD, TYPE_F32, sum, dotProd, sq);
            dotProd = sum;
         } else {
            dotProd = sq;
         }
      }
      Value *dst = getSSA();
      mkOp1(OP_SQRT, TYPE_F32, dst, dotProd);
      setSSAComponent(resId, 0, dst);
      break;
   }
   case GLSLstd450Distance: {
      uint32_t aId = inst.word(5);
      uint32_t bId = inst.word(6);
      unsigned srcComps = 1;
      auto sit = ssaValues.find(aId);
      if (sit != ssaValues.end())
         srcComps = (unsigned)sit->second.size();
      if (srcComps == 0) srcComps = 1;

      Value *dotProd = nullptr;
      for (unsigned c = 0; c < srcComps; c++) {
         Value *a = getSSAComponent(aId, c);
         Value *b = getSSAComponent(bId, c);
         if (!a) a = loadImm(nullptr, 0.0f);
         if (!b) b = loadImm(nullptr, 0.0f);
         Value *diff = getSSA();
         mkOp2(OP_SUB, TYPE_F32, diff, a, b);
         Value *sq = getSSA();
         mkOp2(OP_MUL, TYPE_F32, sq, diff, diff);
         if (dotProd) {
            Value *sum = getSSA();
            mkOp2(OP_ADD, TYPE_F32, sum, dotProd, sq);
            dotProd = sum;
         } else {
            dotProd = sq;
         }
      }
      Value *dst = getSSA();
      mkOp1(OP_SQRT, TYPE_F32, dst, dotProd);
      setSSAComponent(resId, 0, dst);
      break;
   }
   case GLSLstd450Cross: {
      uint32_t aId = inst.word(5);
      uint32_t bId = inst.word(6);
      // cross(a,b) = (a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x)
      for (unsigned c = 0; c < 3; c++) {
         unsigned i1 = (c + 1) % 3;
         unsigned i2 = (c + 2) % 3;
         Value *a1 = getSSAComponent(aId, i1);
         Value *b2 = getSSAComponent(bId, i2);
         Value *a2 = getSSAComponent(aId, i2);
         Value *b1 = getSSAComponent(bId, i1);
         if (!a1) a1 = loadImm(nullptr, 0.0f);
         if (!b2) b2 = loadImm(nullptr, 0.0f);
         if (!a2) a2 = loadImm(nullptr, 0.0f);
         if (!b1) b1 = loadImm(nullptr, 0.0f);
         Value *t1 = getSSA();
         mkOp2(OP_MUL, TYPE_F32, t1, a1, b2);
         Value *t2 = getSSA();
         mkOp2(OP_MUL, TYPE_F32, t2, a2, b1);
         Value *dst = getSSA();
         mkOp2(OP_SUB, TYPE_F32, dst, t1, t2);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Reflect: {
      // reflect(I, N) = I - 2 * dot(N, I) * N
      uint32_t iId = inst.word(5);
      uint32_t nId = inst.word(6);
      // dot(N, I)
      Value *dotProd = nullptr;
      for (unsigned c = 0; c < comps; c++) {
         Value *n = getSSAComponent(nId, c);
         Value *i = getSSAComponent(iId, c);
         if (!n) n = loadImm(nullptr, 0.0f);
         if (!i) i = loadImm(nullptr, 0.0f);
         Value *prod = getSSA();
         mkOp2(OP_MUL, TYPE_F32, prod, n, i);
         if (dotProd) {
            Value *sum = getSSA();
            mkOp2(OP_ADD, TYPE_F32, sum, dotProd, prod);
            dotProd = sum;
         } else {
            dotProd = prod;
         }
      }
      Value *two_dot = getSSA();
      mkOp2(OP_MUL, TYPE_F32, two_dot, dotProd, loadImm(nullptr, 2.0f));
      for (unsigned c = 0; c < comps; c++) {
         Value *i = getSSAComponent(iId, c);
         Value *n = getSSAComponent(nId, c);
         if (!i) i = loadImm(nullptr, 0.0f);
         if (!n) n = loadImm(nullptr, 0.0f);
         Value *t = getSSA();
         mkOp2(OP_MUL, TYPE_F32, t, two_dot, n);
         Value *dst = getSSA();
         mkOp2(OP_SUB, TYPE_F32, dst, i, t);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450FSign: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *pos = getSSA();
         mkCmp(OP_SET, CC_GT, TYPE_F32, pos, TYPE_F32, s, loadImm(nullptr, 0.0f));
         Value *neg = getSSA();
         mkCmp(OP_SET, CC_LT, TYPE_F32, neg, TYPE_F32, s, loadImm(nullptr, 0.0f));
         Value *dst = getSSA();
         mkOp2(OP_SUB, TYPE_F32, dst, pos, neg);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Step: {
      // step(edge, x) = x < edge ? 0.0 : 1.0
      uint32_t edgeId = inst.word(5);
      uint32_t xId = inst.word(6);
      for (unsigned c = 0; c < comps; c++) {
         Value *edge = getSSAComponent(edgeId, c);
         Value *x = getSSAComponent(xId, c);
         if (!edge) edge = loadImm(nullptr, 0.0f);
         if (!x) x = loadImm(nullptr, 0.0f);
         Value *cmp = getSSA();
         mkCmp(OP_SET, CC_GE, TYPE_F32, cmp, TYPE_F32, x, edge);
         // cmp is 1.0 if x >= edge, else 0.0 (since SET with F32 result)
         Value *dst = getSSA();
         mkOp2(OP_AND, TYPE_U32, dst, cmp, loadImm(nullptr, 1.0f));
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450SmoothStep: {
      // smoothstep(e0, e1, x):
      // t = clamp((x - e0) / (e1 - e0), 0, 1)
      // return t * t * (3 - 2*t)
      uint32_t e0Id = inst.word(5);
      uint32_t e1Id = inst.word(6);
      uint32_t xId = inst.word(7);
      for (unsigned c = 0; c < comps; c++) {
         Value *e0 = getSSAComponent(e0Id, c);
         Value *e1 = getSSAComponent(e1Id, c);
         Value *x = getSSAComponent(xId, c);
         if (!e0) e0 = loadImm(nullptr, 0.0f);
         if (!e1) e1 = loadImm(nullptr, 1.0f);
         if (!x) x = loadImm(nullptr, 0.0f);

         Value *diff = getSSA();
         mkOp2(OP_SUB, TYPE_F32, diff, x, e0);
         Value *range = getSSA();
         mkOp2(OP_SUB, TYPE_F32, range, e1, e0);
         Value *rcp = getSSA();
         mkOp1(OP_RCP, TYPE_F32, rcp, range);
         Value *t = getSSA();
         mkOp2(OP_MUL, TYPE_F32, t, diff, rcp);
         // clamp
         Value *t1 = getSSA();
         mkOp2(OP_MAX, TYPE_F32, t1, t, loadImm(nullptr, 0.0f));
         Value *t2 = getSSA();
         mkOp2(OP_MIN, TYPE_F32, t2, t1, loadImm(nullptr, 1.0f));
         // t*t*(3 - 2*t)
         Value *two_t = getSSA();
         mkOp2(OP_MUL, TYPE_F32, two_t, t2, loadImm(nullptr, 2.0f));
         Value *three_minus = getSSA();
         mkOp2(OP_SUB, TYPE_F32, three_minus, loadImm(nullptr, 3.0f), two_t);
         Value *t_sq = getSSA();
         mkOp2(OP_MUL, TYPE_F32, t_sq, t2, t2);
         Value *dst = getSSA();
         mkOp2(OP_MUL, TYPE_F32, dst, t_sq, three_minus);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case GLSLstd450Fract: {
      uint32_t srcId = inst.word(5);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *fl = getSSA();
         mkOp1(OP_FLOOR, TYPE_F32, fl, s);
         Value *dst = getSSA();
         mkOp2(OP_SUB, TYPE_F32, dst, s, fl);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   default:
      // Unhandled GLSL.std.450 op; emit zero for safety
      for (unsigned c = 0; c < comps; c++)
         setSSAComponent(resId, c, loadImm(nullptr, 0u));
      fprintf(stderr, "SPIR-V: unhandled GLSL.std.450 op %d\n", op);
      break;
   }
}

// ============================================================================
// Texture ops (Phase 3, stubbed for now)
// ============================================================================

void
Converter::handleImageSample(const spirv::Instruction& inst)
{
   uint32_t resId = inst.resultId();
   unsigned comps = getResultComponents(inst);
   // TODO: Full texture sampling support
   for (unsigned c = 0; c < comps; c++)
      setSSAComponent(resId, c, loadImm(nullptr, 0.0f));
   fprintf(stderr, "SPIR-V: texture sampling not yet implemented\n");
}

void
Converter::handleImageFetch(const spirv::Instruction& inst)
{
   uint32_t resId = inst.resultId();
   unsigned comps = getResultComponents(inst);
   for (unsigned c = 0; c < comps; c++)
      setSSAComponent(resId, c, loadImm(nullptr, 0u));
   fprintf(stderr, "SPIR-V: texel fetch not yet implemented\n");
}

// ============================================================================
// Control flow
// ============================================================================

void
Converter::handleBranch(const spirv::Instruction& inst)
{
   // OpBranch %target
   uint32_t targetLabel = inst.word(1);
   auto it = labelToBB.find(targetLabel);
   if (it != labelToBB.end()) {
      mkFlow(OP_BRA, it->second, CC_ALWAYS, nullptr);
      // HACK: connect blocks in CFG
      bb->cfg.attach(&it->second->cfg, Graph::Edge::TREE);
   }
}

void
Converter::handleBranchConditional(const spirv::Instruction& inst)
{
   // OpBranchConditional %condition %trueLabel %falseLabel
   uint32_t condId = inst.word(1);
   uint32_t trueLabel = inst.word(2);
   uint32_t falseLabel = inst.word(3);

   Value *cond = getSSAComponent(condId, 0);
   if (!cond) cond = loadImm(nullptr, 0u);

   // Convert boolean to predicate
   Value *pred = getSSA(1, FILE_PREDICATE);
   mkCmp(OP_SET, CC_NE, TYPE_U32, pred, TYPE_U32, cond, loadImm(nullptr, 0u));

   auto itTrue = labelToBB.find(trueLabel);
   auto itFalse = labelToBB.find(falseLabel);

   if (itTrue != labelToBB.end()) {
      mkFlow(OP_BRA, itTrue->second, CC_P, pred);
      bb->cfg.attach(&itTrue->second->cfg, Graph::Edge::TREE);
   }
   if (itFalse != labelToBB.end()) {
      mkFlow(OP_BRA, itFalse->second, CC_ALWAYS, nullptr);
      bb->cfg.attach(&itFalse->second->cfg, Graph::Edge::TREE);
   }
}

void
Converter::handlePhi(const spirv::Instruction& inst)
{
   // OpPhi %resultType %result (variable, parent)...
   uint32_t resId = inst.resultId();
   unsigned comps = getResultComponents(inst);

   // For now, just take the first available value
   // Full PHI support requires proper SSA/CFG handling
   for (unsigned c = 0; c < comps; c++) {
      bool found = false;
      for (uint16_t i = 3; i + 1 < inst.wordCount; i += 2) {
         uint32_t valId = inst.word(i);
         Value *val = getSSAComponent(valId, c);
         if (val) {
            setSSAComponent(resId, c, val);
            found = true;
            break;
         }
      }
      if (!found)
         setSSAComponent(resId, c, loadImm(nullptr, 0u));
   }
}

void
Converter::handleReturn(const spirv::Instruction& inst)
{
   (void)inst;
   // Don't emit OP_RET for main function; we'll just fall through
}

void
Converter::handleKill(const spirv::Instruction& inst)
{
   (void)inst;
   mkOp(OP_DISCARD, TYPE_NONE, nullptr);
   info->prop.fp.usesDiscard = true;
}

// ============================================================================
// Main instruction dispatch
// ============================================================================

bool
Converter::convertInstruction(const spirv::Instruction& inst)
{
   switch (inst.opcode) {
   // Variables and memory
   case SpvOpVariable:
      handleVariable(inst);
      break;
   case SpvOpLoad:
      handleLoad(inst);
      break;
   case SpvOpStore:
      handleStore(inst);
      break;
   case SpvOpAccessChain:
   case SpvOpInBoundsAccessChain:
      handleAccessChain(inst);
      break;

   // Arithmetic (float)
   case SpvOpFAdd:
      handleBinaryOp(inst, OP_ADD, TYPE_F32);
      break;
   case SpvOpFSub:
      handleBinaryOp(inst, OP_SUB, TYPE_F32);
      break;
   case SpvOpFMul:
      handleBinaryOp(inst, OP_MUL, TYPE_F32);
      break;
   case SpvOpFDiv: {
      // fdiv(a,b) = a * rcp(b)
      uint32_t resId = inst.resultId();
      uint32_t aId = inst.word(3);
      uint32_t bId = inst.word(4);
      unsigned comps = getResultComponents(inst);
      for (unsigned c = 0; c < comps; c++) {
         Value *a = getSSAComponent(aId, c);
         Value *b = getSSAComponent(bId, c);
         if (!a) a = loadImm(nullptr, 0.0f);
         if (!b) b = loadImm(nullptr, 1.0f);
         Value *rcp = getSSA();
         mkOp1(OP_RCP, TYPE_F32, rcp, b);
         Value *dst = getSSA();
         mkOp2(OP_MUL, TYPE_F32, dst, a, rcp);
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case SpvOpFRem:
   case SpvOpFMod:
      handleBinaryOp(inst, OP_MOD, TYPE_F32);
      break;
   case SpvOpFNegate:
      handleNegate(inst, TYPE_F32);
      break;

   // Arithmetic (integer)
   case SpvOpIAdd:
      handleBinaryOp(inst, OP_ADD, TYPE_U32);
      break;
   case SpvOpISub:
      handleBinaryOp(inst, OP_SUB, TYPE_U32);
      break;
   case SpvOpIMul:
      handleBinaryOp(inst, OP_MUL, TYPE_U32);
      break;
   case SpvOpUDiv:
      handleBinaryOp(inst, OP_DIV, TYPE_U32);
      break;
   case SpvOpSDiv:
      handleBinaryOp(inst, OP_DIV, TYPE_S32);
      break;
   case SpvOpUMod:
      handleBinaryOp(inst, OP_MOD, TYPE_U32);
      break;
   case SpvOpSRem:
   case SpvOpSMod:
      handleBinaryOp(inst, OP_MOD, TYPE_S32);
      break;
   case SpvOpSNegate:
      handleNegate(inst, TYPE_S32);
      break;

   // Vector/scalar ops
   case SpvOpVectorTimesScalar:
      handleVectorTimesScalar(inst);
      break;
   case SpvOpMatrixTimesScalar:
      handleVectorTimesScalar(inst); // same logic, scale all components
      break;
   case SpvOpDot:
      handleDot(inst);
      break;
   case SpvOpMatrixTimesVector:
      handleMatrixTimesVector(inst);
      break;
   case SpvOpVectorTimesMatrix:
      // VectorTimesMatrix: result[col] = dot(vec, mat[col])
      // TODO: implement properly
      handleMatrixTimesVector(inst);
      break;

   // Comparisons (float, ordered)
   case SpvOpFOrdEqual:
      handleCompare(inst, CC_EQ, TYPE_F32);
      break;
   case SpvOpFOrdNotEqual:
      handleCompare(inst, CC_NEU, TYPE_F32);
      break;
   case SpvOpFOrdLessThan:
      handleCompare(inst, CC_LT, TYPE_F32);
      break;
   case SpvOpFOrdGreaterThan:
      handleCompare(inst, CC_GT, TYPE_F32);
      break;
   case SpvOpFOrdLessThanEqual:
      handleCompare(inst, CC_LE, TYPE_F32);
      break;
   case SpvOpFOrdGreaterThanEqual:
      handleCompare(inst, CC_GE, TYPE_F32);
      break;

   // Comparisons (float, unordered)
   case SpvOpFUnordEqual:
      handleCompare(inst, CC_EQU, TYPE_F32);
      break;
   case SpvOpFUnordNotEqual:
      handleCompare(inst, CC_NE, TYPE_F32);
      break;
   case SpvOpFUnordLessThan:
      handleCompare(inst, CC_LTU, TYPE_F32);
      break;
   case SpvOpFUnordGreaterThan:
      handleCompare(inst, CC_GTU, TYPE_F32);
      break;
   case SpvOpFUnordLessThanEqual:
      handleCompare(inst, CC_LEU, TYPE_F32);
      break;
   case SpvOpFUnordGreaterThanEqual:
      handleCompare(inst, CC_GEU, TYPE_F32);
      break;

   // Comparisons (integer)
   case SpvOpIEqual:
      handleCompare(inst, CC_EQ, TYPE_U32);
      break;
   case SpvOpINotEqual:
      handleCompare(inst, CC_NE, TYPE_U32);
      break;
   case SpvOpUGreaterThan:
      handleCompare(inst, CC_GT, TYPE_U32);
      break;
   case SpvOpSGreaterThan:
      handleCompare(inst, CC_GT, TYPE_S32);
      break;
   case SpvOpUGreaterThanEqual:
      handleCompare(inst, CC_GE, TYPE_U32);
      break;
   case SpvOpSGreaterThanEqual:
      handleCompare(inst, CC_GE, TYPE_S32);
      break;
   case SpvOpULessThan:
      handleCompare(inst, CC_LT, TYPE_U32);
      break;
   case SpvOpSLessThan:
      handleCompare(inst, CC_LT, TYPE_S32);
      break;
   case SpvOpULessThanEqual:
      handleCompare(inst, CC_LE, TYPE_U32);
      break;
   case SpvOpSLessThanEqual:
      handleCompare(inst, CC_LE, TYPE_S32);
      break;

   // Logical
   case SpvOpLogicalEqual:
      handleCompare(inst, CC_EQ, TYPE_U32);
      break;
   case SpvOpLogicalNotEqual:
      handleCompare(inst, CC_NE, TYPE_U32);
      break;
   case SpvOpLogicalOr:
      handleBinaryOp(inst, OP_OR, TYPE_U32);
      break;
   case SpvOpLogicalAnd:
      handleBinaryOp(inst, OP_AND, TYPE_U32);
      break;
   case SpvOpLogicalNot:
      handleUnaryOp(inst, OP_NOT, TYPE_U32);
      break;

   // Bitwise
   case SpvOpBitwiseOr:
      handleBitOp(inst, OP_OR);
      break;
   case SpvOpBitwiseXor:
      handleBitOp(inst, OP_XOR);
      break;
   case SpvOpBitwiseAnd:
      handleBitOp(inst, OP_AND);
      break;
   case SpvOpNot:
      handleUnaryOp(inst, OP_NOT, TYPE_U32);
      break;

   // Shifts
   case SpvOpShiftRightLogical:
      handleShift(inst, OP_SHR);
      break;
   case SpvOpShiftRightArithmetic:
      handleBinaryOp(inst, OP_SHR, TYPE_S32);
      break;
   case SpvOpShiftLeftLogical:
      handleShift(inst, OP_SHL);
      break;

   // Conversions
   case SpvOpConvertFToU:
   case SpvOpConvertFToS:
   case SpvOpConvertSToF:
   case SpvOpConvertUToF:
   case SpvOpUConvert:
   case SpvOpSConvert:
   case SpvOpFConvert:
      handleConvert(inst);
      break;
   case SpvOpBitcast:
      handleBitcast(inst);
      break;

   // Select
   case SpvOpSelect:
      handleSelect(inst);
      break;

   // Composite
   case SpvOpCompositeExtract:
      handleCompositeExtract(inst);
      break;
   case SpvOpCompositeConstruct:
      handleCompositeConstruct(inst);
      break;
   case SpvOpVectorShuffle:
      handleVectorShuffle(inst);
      break;
   case SpvOpCopyObject:
      handleCopyObject(inst);
      break;
   case SpvOpCompositeInsert:
      // TODO: implement properly
      break;

   // Extended instructions (GLSL.std.450)
   case SpvOpExtInst:
      handleExtInst(inst);
      break;

   // Control flow
   case SpvOpBranch:
      handleBranch(inst);
      break;
   case SpvOpBranchConditional:
      handleBranchConditional(inst);
      break;
   case SpvOpPhi:
      handlePhi(inst);
      break;
   case SpvOpReturn:
   case SpvOpReturnValue:
      handleReturn(inst);
      break;
   case SpvOpKill:
      handleKill(inst);
      break;

   // Derivatives
   case SpvOpDPdx:
   case SpvOpDPdxFine:
   case SpvOpDPdxCoarse:
      handleDerivative(inst, OP_DFDX);
      break;
   case SpvOpDPdy:
   case SpvOpDPdyFine:
   case SpvOpDPdyCoarse:
      handleDerivative(inst, OP_DFDY);
      break;

   // NaN/Inf checks
   case SpvOpIsNan: {
      uint32_t resId = inst.resultId();
      uint32_t srcId = inst.word(3);
      unsigned comps = getResultComponents(inst);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *dst = getSSA();
         mkCmp(OP_SET, CC_NE, TYPE_U32, dst, TYPE_F32, s, s); // NaN != NaN
         setSSAComponent(resId, c, dst);
      }
      break;
   }
   case SpvOpIsInf: {
      uint32_t resId = inst.resultId();
      uint32_t srcId = inst.word(3);
      unsigned comps = getResultComponents(inst);
      for (unsigned c = 0; c < comps; c++) {
         Value *s = getSSAComponent(srcId, c);
         if (!s) s = loadImm(nullptr, 0.0f);
         Value *abs = getSSA();
         mkOp1(OP_ABS, TYPE_F32, abs, s);
         Value *dst = getSSA();
         // Compare abs(x) == +inf (0x7f800000)
         uint32_t inf_bits = 0x7f800000;
         float inf_val;
         memcpy(&inf_val, &inf_bits, 4);
         mkCmp(OP_SET, CC_EQ, TYPE_U32, dst, TYPE_F32, abs, loadImm(nullptr, inf_val));
         setSSAComponent(resId, c, dst);
      }
      break;
   }

   // Boolean constants
   case SpvOpAny:
   case SpvOpAll:
      // TODO: implement reductions
      break;

   // Texture operations (Phase 3)
   case SpvOpSampledImage:
      // Just pass through the image operand
      if (inst.hasResultId()) {
         uint32_t resId = inst.resultId();
         uint32_t imgId = inst.word(3);
         ssaValues[resId] = ssaValues[imgId];
      }
      break;
   case SpvOpImageSampleImplicitLod:
   case SpvOpImageSampleExplicitLod:
   case SpvOpImageSampleDrefImplicitLod:
   case SpvOpImageSampleDrefExplicitLod:
      handleImageSample(inst);
      break;
   case SpvOpImageFetch:
      handleImageFetch(inst);
      break;

   // Things we can safely ignore
   case SpvOpNop:
   case SpvOpUndef:
   case SpvOpSource:
   case SpvOpSourceContinued:
   case SpvOpSourceExtension:
   case SpvOpName:
   case SpvOpMemberName:
   case SpvOpString:
   case SpvOpLine:
   case SpvOpNoLine:
   case SpvOpDecorate:
   case SpvOpMemberDecorate:
   case SpvOpDecorationGroup:
   case SpvOpGroupDecorate:
   case SpvOpGroupMemberDecorate:
   case SpvOpExtension:
   case SpvOpExtInstImport:
   case SpvOpMemoryModel:
   case SpvOpEntryPoint:
   case SpvOpExecutionMode:
   case SpvOpCapability:
   case SpvOpTypeVoid:
   case SpvOpTypeBool:
   case SpvOpTypeInt:
   case SpvOpTypeFloat:
   case SpvOpTypeVector:
   case SpvOpTypeMatrix:
   case SpvOpTypeImage:
   case SpvOpTypeSampler:
   case SpvOpTypeSampledImage:
   case SpvOpTypeArray:
   case SpvOpTypeRuntimeArray:
   case SpvOpTypeStruct:
   case SpvOpTypePointer:
   case SpvOpTypeFunction:
   case SpvOpConstant:
   case SpvOpConstantTrue:
   case SpvOpConstantFalse:
   case SpvOpConstantComposite:
   case SpvOpConstantNull:
   case SpvOpConstantSampler:
   case SpvOpSpecConstantTrue:
   case SpvOpSpecConstantFalse:
   case SpvOpSpecConstant:
   case SpvOpSpecConstantComposite:
   case SpvOpSpecConstantOp:
   case SpvOpFunction:
   case SpvOpFunctionParameter:
   case SpvOpFunctionEnd:
   case SpvOpLabel:
   case SpvOpSelectionMerge:
   case SpvOpLoopMerge:
   case SpvOpFunctionCall: // TODO
   case SpvOpTranspose: // TODO
   case SpvOpOuterProduct: // TODO
   case SpvOpMatrixTimesMatrix: // TODO
      break;

   default:
      fprintf(stderr, "SPIR-V: unhandled opcode %u\n", inst.opcode);
      break;
   }

   return true;
}

// ============================================================================
// Main conversion flow
// ============================================================================

bool
Converter::run()
{
   // 1. Scan variables to populate info arrays
   scanVariables();

   // 2. Call slot assignment (same as TGSI path)
   if (info->assignSlots)
      info->assignSlots(info);

   // 3. Set up the IR structure
   BasicBlock *entry = new BasicBlock(prog->main);
   BasicBlock *leave = new BasicBlock(prog->main);

   prog->main->setEntry(entry);
   prog->main->setExit(leave);

   setPosition(entry, true);

   // 4. For fragment shaders, set up 1/w for perspective interpolation and oData
   if (prog->getType() == Program::TYPE_FRAGMENT) {
      Symbol *sv = mkSysVal(SV_POSITION, 3);
      fragCoordW = mkOp1v(OP_RDSV, TYPE_F32, getSSA(), sv);
      mkOp1(OP_RCP, TYPE_F32, fragCoordW, fragCoordW);

      oData.setup(0 /*array*/, 0 /*arrayIdx*/, 0 /*base*/,
                  info->numOutputs, 4, 4, FILE_GPR, 0);
   }

   // 5. Find the entry point function
   const spirv::Function* entryFunc = nullptr;
   for (auto& func : spv->functions) {
      if (func.id == spv->entryPointId) {
         entryFunc = &func;
         break;
      }
   }

   if (!entryFunc) {
      fprintf(stderr, "SPIR-V: entry point function %u not found\n", spv->entryPointId);
      return false;
   }

   // 6. Pre-create all basic blocks for forward references
   for (auto& block : entryFunc->blocks) {
      BasicBlock *bb;
      if (&block == &entryFunc->blocks.front()) {
         bb = entry;
      } else {
         bb = new BasicBlock(prog->main);
      }
      labelToBB[block.labelId] = bb;
   }

   // 7. Convert all instructions
   for (auto& block : entryFunc->blocks) {
      BasicBlock *currBB = labelToBB[block.labelId];
      setPosition(currBB, true);

      for (auto instPtr : block.instructions) {
         if (!convertInstruction(*instPtr))
            return false;
      }
   }

   // 8. Export fragment shader outputs
   if (prog->getType() == Program::TYPE_FRAGMENT) {
      // Make sure we're in the last basic block
      setPosition(leave, true);
      exportOutputs();
   }

   // 9. Ensure control flow reaches the exit block
   if (bb != leave && prog->getType() != Program::TYPE_FRAGMENT) {
      mkFlow(OP_BRA, leave, CC_ALWAYS, nullptr);
      bb->cfg.attach(&leave->cfg, Graph::Edge::TREE);
   }

   // For fragment shaders, connect last instruction block to leave
   if (prog->getType() == Program::TYPE_FRAGMENT && bb != leave) {
      // The exports are already in leave, just connect
      BasicBlock *lastBB = bb;
      setPosition(leave, true);
      // Re-export if we moved
      if (lastBB != entry) {
         mkFlow(OP_BRA, leave, CC_ALWAYS, nullptr);
         lastBB->cfg.attach(&leave->cfg, Graph::Edge::TREE);
      }
   }

   return true;
}

} // unnamed namespace

// ============================================================================
// Entry point (called from nv50_ir.cpp)
// ============================================================================

namespace nv50_ir {

bool
Program::makeFromSPIRV(struct nv50_ir_prog_info *info)
{
   const spirv::Program *spv = static_cast<const spirv::Program *>(info->bin.source);
   if (!spv)
      return false;

   tlsSize = info->bin.tlsSpace;

   Converter builder(this, spv, info);
   return builder.run();
}

} // namespace nv50_ir
