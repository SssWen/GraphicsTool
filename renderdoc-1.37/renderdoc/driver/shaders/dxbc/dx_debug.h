/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include <map>

namespace DXBC
{
enum ResourceRetType;
enum class InterpolationMode : uint8_t;
};

namespace DXBCBytecode
{
enum ResourceDimension;
enum SamplerMode;
};

namespace DXDebug
{
typedef DXBC::ResourceRetType ResourceRetType;
typedef DXBCBytecode::ResourceDimension ResourceDimension;
typedef DXBCBytecode::SamplerMode SamplerMode;

struct PSInputElement
{
  PSInputElement(int regster, int element, int numWords, ShaderBuiltin attr, bool inc)
  {
    reg = regster;
    elem = element;
    numwords = numWords;
    sysattribute = attr;
    included = inc;
  }

  int reg;
  int elem;
  ShaderBuiltin sysattribute;

  int numwords;

  bool included;
};

void GatherPSInputDataForInitialValues(const rdcarray<SigParameter> &stageInputSig,
                                       const rdcarray<SigParameter> &prevStageOutputSig,
                                       const rdcarray<DXBC::InterpolationMode> &interpModes,
                                       rdcarray<PSInputElement> &initialValues,
                                       rdcarray<rdcstr> &floatInputs, rdcarray<rdcstr> &inputVarNames,
                                       rdcstr &psInputDefinition, int &structureStride,
                                       std::map<ShaderBuiltin, rdcstr> &usedInputs);

enum class GatherChannel : uint8_t
{
  Red = 0,
  Green = 1,
  Blue = 2,
  Alpha = 3,
};

enum class HeapDescriptorType : uint8_t
{
  NoHeap = 0,
  CBV_SRV_UAV,
  Sampler,
};

struct BindingSlot
{
  BindingSlot()
      : shaderRegister(UINT32_MAX),
        registerSpace(UINT32_MAX),
        heapType(HeapDescriptorType::NoHeap),
        descriptorIndex(UINT32_MAX)
  {
  }
  BindingSlot(uint32_t shaderReg, uint32_t regSpace)
      : shaderRegister(shaderReg),
        registerSpace(regSpace),
        heapType(HeapDescriptorType::NoHeap),
        descriptorIndex(UINT32_MAX)
  {
  }
  BindingSlot(HeapDescriptorType type, uint32_t index)
      : shaderRegister(UINT32_MAX), registerSpace(UINT32_MAX), heapType(type), descriptorIndex(index)
  {
  }
  bool operator<(const BindingSlot &o) const
  {
    if(registerSpace != o.registerSpace)
      return registerSpace < o.registerSpace;
    if(shaderRegister != o.shaderRegister)
      return shaderRegister < o.shaderRegister;
    if(heapType != o.heapType)
      return heapType < o.heapType;
    return descriptorIndex < o.descriptorIndex;
  }
  bool operator==(const BindingSlot &o) const
  {
    return registerSpace == o.registerSpace && shaderRegister == o.shaderRegister &&
           heapType == o.heapType && descriptorIndex == o.descriptorIndex;
  }
  uint32_t shaderRegister;
  uint32_t registerSpace;
  HeapDescriptorType heapType;
  uint32_t descriptorIndex;
};

struct SampleGatherResourceData
{
  ResourceDimension dim;
  ResourceRetType retType;
  int sampleCount;
  BindingSlot binding;
};

struct SampleGatherSamplerData
{
  SamplerMode mode;
  float bias;
  BindingSlot binding;
};

float dxbc_min(float a, float b);
double dxbc_min(double a, double b);
float dxbc_max(float a, float b);
double dxbc_max(double a, double b);
float round_ne(float x);
double round_ne(double x);
float flush_denorm(const float f);

uint32_t BitwiseReverseLSB16(uint32_t x);
uint32_t PopCount(uint32_t x);

void get_sample_position(uint32_t sampleIndex, uint32_t sampleCount, float *position);
};
