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

#include <unordered_set>

namespace DXIL
{
typedef rdcpair<uint32_t, uint32_t> BlockLink;

struct ControlFlow
{
public:
  ControlFlow() = default;
  ControlFlow(const rdcarray<rdcpair<uint32_t, uint32_t>> &links) { Construct(links); }
  void Construct(const rdcarray<rdcpair<uint32_t, uint32_t>> &links);
  rdcarray<uint32_t> GetUniformBlocks() const { return m_UniformBlocks; }
  rdcarray<uint32_t> GetLoopBlocks() const { return m_LoopBlocks; }
  uint32_t GetNextUniformBlock(uint32_t from) const;
  bool IsForwardConnection(uint32_t from, uint32_t to) const;

private:
  typedef rdcarray<uint32_t> BlockPath;

  enum class ConnectionState : uint8_t
  {
    Unknown,
    NotConnected,
    Connected,
  };

  bool TraceBlockFlow(const uint32_t from, BlockPath &path);
  bool BlockInAllPaths(uint32_t block, uint32_t pathIdx, int32_t startIdx) const;
  int32_t BlockInAnyPath(uint32_t block, uint32_t pathIdx, int32_t startIdx, int32_t steps) const;
  bool ControlFlow::IsBlockConnected(uint32_t from, uint32_t to) const;

  const uint32_t PATH_END = ~0U;

  std::unordered_set<uint32_t> m_Blocks;
  rdcarray<BlockPath> m_BlockLinks;

  rdcarray<rdcarray<uint32_t>> m_BlockPathLinks;
  mutable rdcarray<bool> m_TracedBlocks;
  mutable rdcarray<bool> m_CheckedPaths;
  rdcarray<BlockPath> m_Paths;

  rdcarray<uint32_t> m_UniformBlocks;
  rdcarray<uint32_t> m_LoopBlocks;
  mutable rdcarray<rdcarray<ConnectionState>> m_Connections;
};
};    // namespace DXIL
