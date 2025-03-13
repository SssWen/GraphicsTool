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

#include "d3d11_test.h"

RD_TEST(D3D11_Groupshared, D3D11GraphicsTest)
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

    float values[64];
    for(int i = 0; i < 64; i++)
      values[i] = RANDF(1.0f, 100.0f);
    ID3D11BufferPtr inBuf = MakeBuffer().Data(values).UAV().Structured(4);
    ID3D11BufferPtr outBuf = MakeBuffer().Size(sizeof(Vec4f) * 64).UAV().Structured(sizeof(Vec4f));

    ID3D11UnorderedAccessViewPtr inUAV = MakeUAV(inBuf);
    ID3D11UnorderedAccessViewPtr outUAV = MakeUAV(outBuf);

    ID3D11ComputeShaderPtr shad = CreateCS(Compile(comp, "main", "cs_5_0", true));

    while(Running())
    {
      ClearRenderTargetView(bbRTV, {0.2f, 0.2f, 0.2f, 1.0f});

      ClearUnorderedAccessView(outUAV, Vec4u());

      ctx->CSSetShader(shad, NULL, 0);
      ctx->CSSetUnorderedAccessViews(0, 1, &inUAV.GetInterfacePtr(), NULL);
      ctx->CSSetUnorderedAccessViews(1, 1, &outUAV.GetInterfacePtr(), NULL);

      ctx->Dispatch(1, 1, 1);

      Present();
    }

    return 0;
  }
};

REGISTER_TEST();
