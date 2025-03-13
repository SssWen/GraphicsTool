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

#include "vk_test.h"

RD_TEST(VK_Groupshared, VulkanGraphicsTest)
{
  static constexpr const char *Description = "Test of compute shader that uses groupshared memory.";

  std::string comp = R"EOSHADER(
#version 460 core

layout(binding = 0, std430) buffer indataBuf
{
  float indata[64];
};

layout(binding = 1, std430) buffer outdataBuf
{
  vec4 outdata[64];
};

shared float tmp[64];

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define GroupMemoryBarrierWithGroupSync() memoryBarrierShared();groupMemoryBarrier();barrier();

void main()
{
  uvec3 tid = gl_LocalInvocationID;

  if(gl_LocalInvocationID.x == 0)
  {
    for(int i=0; i < 64; i++) tmp[i] = 1.234f;
  }

  GroupMemoryBarrierWithGroupSync();

  vec4 outval;

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
    // initialise, create window, create context, etc
    if(!Init())
      return 3;

    VkDescriptorSetLayout setLayout = createDescriptorSetLayout(vkh::DescriptorSetLayoutCreateInfo({
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
    }));
    VkPipelineLayout layout = createPipelineLayout(vkh::PipelineLayoutCreateInfo({setLayout}));

    VkPipeline pipe = createComputePipeline(vkh::ComputePipelineCreateInfo(
        layout, CompileShaderModule(comp, ShaderLang::glsl, ShaderStage::comp)));

    VkDescriptorSet descSet = allocateDescriptorSet(setLayout);

    float values[64];
    for(int i = 0; i < 64; i++)
      values[i] = RANDF(1.0f, 100.0f);
    AllocatedBuffer inBuf(this,
                          vkh::BufferCreateInfo(sizeof(values), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
                          VmaAllocationCreateInfo({0, VMA_MEMORY_USAGE_CPU_TO_GPU}));
    inBuf.upload(values);

    AllocatedBuffer outBuf(
        this,
        vkh::BufferCreateInfo(sizeof(Vec4f) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT),
        VmaAllocationCreateInfo({0, VMA_MEMORY_USAGE_GPU_ONLY}));

    vkh::updateDescriptorSets(
        device, {
                    vkh::WriteDescriptorSet(descSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            {vkh::DescriptorBufferInfo(inBuf.buffer)}),
                    vkh::WriteDescriptorSet(descSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            {vkh::DescriptorBufferInfo(outBuf.buffer)}),
                });

    while(Running())
    {
      VkCommandBuffer cmd = GetCommandBuffer();

      vkBeginCommandBuffer(cmd, vkh::CommandBufferBeginInfo());

      VkImage swapimg = StartUsingBackbuffer(cmd);

      vkh::cmdClearImage(cmd, swapimg, vkh::ClearColorValue(0.2f, 0.2f, 0.2f, 1.0f));

      vkh::cmdPipelineBarrier(
          cmd, {},
          {vkh::BufferMemoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    outBuf.buffer)});

      vkCmdFillBuffer(cmd, outBuf.buffer, 0, sizeof(Vec4f) * 64, 0);

      vkh::cmdPipelineBarrier(cmd, {},
                              {vkh::BufferMemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT,
                                                        VK_ACCESS_SHADER_WRITE_BIT, outBuf.buffer)});

      vkh::cmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, {descSet}, {});
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);

      vkCmdDispatch(cmd, 1, 1, 1);

      FinishUsingBackbuffer(cmd);

      vkEndCommandBuffer(cmd);

      SubmitAndPresent({cmd});
    }

    return 0;
  }
};

REGISTER_TEST();
