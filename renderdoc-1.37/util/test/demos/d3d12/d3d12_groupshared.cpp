/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2024 Baldur Karlsson
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

#include "d3d12_test.h"

RD_TEST(D3D12_Groupshared, D3D12GraphicsTest)
{
  static constexpr const char *Description = "Test of compute shader that uses groupshared memory.";

  std::string comp = R"EOSHADER(

RWStructuredBuffer<float> indata : register(u0);
RWStructuredBuffer<float4> outdata : register(u1);

groupshared float tmp[64];

[numthreads(64,1,1)]
void main(uint3 tid : SV_GroupThreadID)
{
  if(tid.x == 0)
  {
    for(int i=0; i < 64; i++) tmp[i] = 1.234f;
  }

  GroupMemoryBarrierWithGroupSync();

  float4 outval;

  // first write, should be the init value for all threads
  outval.x = tmp[tid.x];

  tmp[tid.x] = indata[tid.x];

  // second write, should be the read value because we're reading our own value
  outval.y = tmp[tid.x];

  GroupMemoryBarrierWithGroupSync();

  // third write, should be our pairwise neighbour's value
  outval.z = tmp[tid.x ^ 1];

  // do calculation with our neighbour
  tmp[tid.x] = (1.0f + tmp[tid.x]) * (1.0f + tmp[tid.x ^ 1]);

  GroupMemoryBarrierWithGroupSync();

  // fourth write, our neighbour should be identical to our value
  outval.w = tmp[tid.x] == tmp[tid.x ^ 1] ? 9.99f : -9.99f;

  outdata[tid.x] = outval;
}

)EOSHADER";

  int main()
  {
    // initialise, create window, create device, etc
    if(!Init())
      return 3;

    ID3D12RootSignaturePtr rs = MakeSig({
        uavParam(D3D12_SHADER_VISIBILITY_ALL, 0, 0),
        uavParam(D3D12_SHADER_VISIBILITY_ALL, 0, 1),
    });

    ID3DBlobPtr cs = Compile(comp, "main", "cs_5_0", CompileOptionFlags::SkipOptimise);

    ID3D12PipelineStatePtr pso50 = MakePSO().CS(cs).RootSig(rs);
    ID3D12PipelineStatePtr pso60;

    if(m_DXILSupport)
    {
      cs = Compile(comp, "main", "cs_6_0", CompileOptionFlags::SkipOptimise);

      pso60 = MakePSO().CS(cs).RootSig(rs);
    }

    float values[64];
    for(int i = 0; i < 64; i++)
      values[i] = RANDF(1.0f, 100.0f);
    ID3D12ResourcePtr inBuf = MakeBuffer().Data(values).UAV();
    ID3D12ResourcePtr outBuf = MakeBuffer().Size(sizeof(Vec4f) * 64 * 2).UAV();

    D3D12_GPU_DESCRIPTOR_HANDLE outUAVGPU =
        MakeUAV(outBuf).Format(DXGI_FORMAT_R32G32B32A32_FLOAT).CreateGPU(0);
    D3D12_CPU_DESCRIPTOR_HANDLE outUAVClearCPU =
        MakeUAV(outBuf).Format(DXGI_FORMAT_R32G32B32A32_FLOAT).CreateClearCPU(0);

    while(Running())
    {
      ID3D12GraphicsCommandListPtr cmd = GetCommandBuffer();

      Reset(cmd);

      ID3D12ResourcePtr bb = StartUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      cmd->SetDescriptorHeaps(1, &m_CBVUAVSRV.GetInterfacePtr());

      UINT zero[4] = {};
      D3D12_RECT rect = {0, 0, sizeof(Vec4f) * 64, 1};
      cmd->ClearUnorderedAccessViewUint(outUAVGPU, outUAVClearCPU, outBuf, zero, 1, &rect);

      ResourceBarrier(cmd);

      ClearRenderTargetView(cmd, BBRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      cmd->SetComputeRootSignature(rs);
      cmd->SetComputeRootUnorderedAccessView(0, inBuf->GetGPUVirtualAddress());
      cmd->SetComputeRootUnorderedAccessView(1, outBuf->GetGPUVirtualAddress());

      setMarker(cmd, "SM5");
      cmd->SetPipelineState(pso50);
      cmd->Dispatch(1, 1, 1);

      if(pso60)
      {
        setMarker(cmd, "SM6");
        cmd->SetComputeRootUnorderedAccessView(1,
                                               outBuf->GetGPUVirtualAddress() + sizeof(Vec4f) * 64);
        cmd->SetPipelineState(pso60);
        cmd->Dispatch(1, 1, 1);
      }

      FinishUsingBackbuffer(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);

      cmd->Close();

      SubmitAndPresent({cmd});
    }

    return 0;
  }
};

REGISTER_TEST();
