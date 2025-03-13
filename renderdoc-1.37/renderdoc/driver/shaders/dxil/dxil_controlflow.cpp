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

#include "dxil_controlflow.h"
#include "common/formatting.h"
#include "core/settings.h"

RDOC_EXTERN_CONFIG(bool, D3D12_DXILShaderDebugger_Logging);

/*

Inputs are links of blocks : from -> to (can be forwards or backwards links)
Output is a list of uniform control flow blocks which all possible flows go through (not diverged)
and are not in a loop

The algorithm is:

1. Setup
  * Compute all possible known blocks.
  * For each block generate a list of "to" blocks from the input links
  * Any block without links in the input are set to link to the end sentinel (PATH_END)

2. Generate all possible paths
  * Paths can terminate at the end block (PATH_END)
  * Paths can also terminate at a block before the end, if that block has had all its possible paths
already computed

3. Find Uniform Blocks
  * Generate a list of path indexes for each block in the paths
  * Generate a list of all paths blocks which are blocks which appear in all possible paths
    * all paths includes walking any paths linked at the end node of the path being walked
  * Generate a list of loop blocks which are blocks which appear in any path starting from the block
  * uniform blocks are defined to be blocks which are all paths blocks minus loop blocks
*/

namespace DXIL
{
bool ControlFlow::IsBlockConnected(uint32_t from, uint32_t to) const
{
  for(uint32_t pathIdx = 0; pathIdx < m_Paths.size(); ++pathIdx)
  {
    m_CheckedPaths.clear();
    m_CheckedPaths.resize(m_Paths.size());
    for(size_t i = 0; i < m_CheckedPaths.size(); ++i)
      m_CheckedPaths[i] = false;
    int32_t startIdx = -1;
    for(uint32_t i = 0; i < m_Paths[pathIdx].size() - 1; ++i)
    {
      if(m_Paths[pathIdx][i] == from)
      {
        startIdx = i;
        break;
      }
    }
    // BlockInAnyPath will also check all paths linked to from the end node of the path
    if(startIdx != -1 && (BlockInAnyPath(to, pathIdx, startIdx + 1, 0) != -1))
    {
      return true;
    }
  }
  return false;
}

bool ControlFlow::TraceBlockFlow(const uint32_t from, BlockPath &path)
{
  if(from == PATH_END)
  {
    m_Paths.push_back(path);
    return true;
  }
  if(m_BlockLinks[from].empty())
  {
    m_Paths.push_back(path);
    return true;
  }
  if(m_TracedBlocks[from])
  {
    m_Paths.push_back(path);
    return true;
  }
  m_TracedBlocks[from] = true;
  BlockPath newPath = path;
  const rdcarray<uint32_t> &gotos = m_BlockLinks.at(from);
  for(uint32_t to : gotos)
  {
    newPath.push_back(to);
    if(TraceBlockFlow(to, newPath))
      newPath = path;
  }
  return true;
}

int32_t ControlFlow::BlockInAnyPath(uint32_t block, uint32_t pathIdx, int32_t startIdx,
                                    int32_t steps) const
{
  const BlockPath &path = m_Paths[pathIdx];
  if(path.size() == 0)
    return -1;

  // Check the current path
  for(uint32_t i = startIdx; i < path.size(); ++i)
  {
    if(block == path[i])
      return steps;
    ++steps;
  }

  uint32_t endNode = path[path.size() - 1];
  if(endNode == PATH_END)
    return -1;

  // Check any paths linked to by the end node of the current path
  const rdcarray<uint32_t> &childPathsToCheck = m_BlockPathLinks.at(endNode);
  for(uint32_t childPathIdx : childPathsToCheck)
  {
    if(m_CheckedPaths[childPathIdx])
      continue;

    m_CheckedPaths[childPathIdx] = true;
    const BlockPath &childPath = m_Paths[childPathIdx];
    int32_t childPartStartIdx = -1;
    int32_t newSteps = steps;
    for(uint32_t i = 0; i < childPath.size(); ++i)
    {
      if(childPath[i] == endNode)
      {
        childPartStartIdx = i;
        break;
      }
      ++newSteps;
    }
    if(childPartStartIdx != -1)
    {
      newSteps = BlockInAnyPath(block, childPathIdx, childPartStartIdx, newSteps);
      if(newSteps != -1)
        return newSteps;
    }
  }
  return -1;
}

bool ControlFlow::BlockInAllPaths(uint32_t block, uint32_t pathIdx, int32_t startIdx) const
{
  const BlockPath &path = m_Paths[pathIdx];
  if(path.size() == 0)
    return false;

  // Check the current path
  for(uint32_t i = startIdx; i < path.size(); ++i)
  {
    if(block == path[i])
      return true;
  }

  m_CheckedPaths[pathIdx] = true;
  uint32_t endNode = path[path.size() - 1];
  if(endNode == PATH_END)
    return false;

  // Check any paths linked to by the end node of the current path
  const rdcarray<uint32_t> &childPathsToCheck = m_BlockPathLinks.at(endNode);
  for(uint32_t childPathIdx : childPathsToCheck)
  {
    if(m_CheckedPaths[childPathIdx])
      continue;

    m_CheckedPaths[childPathIdx] = true;
    int32_t childPartStartIdx = m_Paths[childPathIdx].indexOf(endNode);
    RDCASSERTNOTEQUAL(childPartStartIdx, -1);
    if(!BlockInAllPaths(block, childPathIdx, childPartStartIdx))
      return false;
  }
  return true;
}

void ControlFlow::Construct(const rdcarray<rdcpair<uint32_t, uint32_t>> &links)
{
  m_Blocks.clear();
  m_BlockLinks.clear();
  m_BlockPathLinks.clear();
  m_TracedBlocks.clear();
  m_CheckedPaths.clear();
  m_Paths.clear();

  m_UniformBlocks.clear();
  m_LoopBlocks.clear();

  // 1. Setup
  // Compute all possible known blocks
  uint32_t maxBlockIndex = 0;
  for(const auto &link : links)
  {
    uint32_t from = link.first;
    uint32_t to = link.second;
    m_Blocks.insert(from);
    m_Blocks.insert(to);
    maxBlockIndex = RDCMAX(maxBlockIndex, from);
    maxBlockIndex = RDCMAX(maxBlockIndex, to);
  }
  ++maxBlockIndex;
  m_TracedBlocks.resize(maxBlockIndex);
  for(size_t i = 0; i < maxBlockIndex; ++i)
    m_TracedBlocks[i] = false;
  m_BlockLinks.resize(maxBlockIndex);
  m_BlockPathLinks.resize(maxBlockIndex);

  // For each block a list of "to" blocks
  for(const auto &link : links)
  {
    uint32_t from = link.first;
    uint32_t to = link.second;
    m_BlockLinks[from].push_back(to);
  }

  // Any block without links in the input are set to link to the end sentinel (PATH_END)
  for(uint32_t b : m_Blocks)
  {
    if(m_BlockLinks[b].empty())
      m_BlockLinks[b].push_back(PATH_END);
  }

  // 2. Generate all possible paths

  // Paths can terminate at the end block (PATH_END)
  // Paths can also terminate at a block before the end, if that block has had all its possible paths already computed
  for(size_t i = 0; i < m_BlockLinks.size(); ++i)
  {
    uint32_t from = (uint32_t)i;
    if(m_BlockLinks[i].empty())
      continue;
    if(m_TracedBlocks[from])
      continue;
    BlockPath path;
    path.push_back(from);
    TraceBlockFlow(from, path);
  }

  // 3. Find Uniform Blocks
  for(uint32_t b : m_Blocks)
    m_BlockPathLinks[b].clear();

  // Generate a list of path indexes for each block in the paths
  for(uint32_t pathIdx = 0; pathIdx < m_Paths.size(); ++pathIdx)
  {
    for(uint32_t block : m_Paths[pathIdx])
    {
      if(block == PATH_END)
        break;
      m_BlockPathLinks[block].push_back(pathIdx);
    }
  }

  // For each path that does not end with PATH_END
  // append the child path nodes which only have a single path and are not already in the path
  // This is an optimisation to reduce the amount of recursion required when tracing paths
  // In particular to help the speed of BlockInAnyPath()
  for(uint32_t p = 0; p < m_Paths.size(); ++p)
  {
    BlockPath &path = m_Paths[p];
    uint32_t endNode = path[path.size() - 1];
    while(endNode != PATH_END)
    {
      const rdcarray<uint32_t> &gotos = m_BlockLinks.at(endNode);
      if(gotos.size() > 1)
        break;
      endNode = gotos[0];
      if(path.contains(endNode))
        break;
      path.push_back(endNode);
    }
  }

  // Generate the connections 2D map for quick lookup of forward connections
  // IsBlock B in any path ahead of Block A
  m_Connections.resize(maxBlockIndex);
  for(uint32_t from = 0; from < maxBlockIndex; ++from)
  {
    m_Connections[from].resize(maxBlockIndex);
    for(uint32_t to = 0; to < maxBlockIndex; ++to)
      m_Connections[from][to] = ConnectionState::Unknown;
  }

  for(uint32_t p = 0; p < m_Paths.size(); ++p)
  {
    const BlockPath &path = m_Paths[p];
    for(size_t i = 0; i < path.size() - 1; ++i)
    {
      uint32_t from = path[i];
      for(size_t j = i + 1; j < path.size(); ++j)
      {
        uint32_t to = path[j];
        if(to == PATH_END)
          break;
        m_Connections[from][to] = ConnectionState::Connected;
      }
    }
  }

  // A loop block is defined by any block which appears in any path starting from the block
  for(uint32_t block : m_Blocks)
  {
    if(IsForwardConnection(block, block))
      m_LoopBlocks.push_back(block);
  }

  rdcarray<uint32_t> allPathsBlocks;

  // An all paths block is defined by any block which appears in all paths
  // all paths includes walking any paths linked at the end node of the path being walked
  for(uint32_t block : m_Blocks)
  {
    bool uniform = true;
    for(uint32_t pathIdx = 0; pathIdx < m_Paths.size(); ++pathIdx)
    {
      m_CheckedPaths.clear();
      m_CheckedPaths.resize(m_Paths.size());
      for(size_t i = 0; i < m_CheckedPaths.size(); ++i)
        m_CheckedPaths[i] = false;
      // BlockInAllPaths will also check all paths linked to from the end node of the path
      if(!BlockInAllPaths(block, pathIdx, 0))
      {
        uniform = false;
        break;
      }
    }
    if(uniform)
      allPathsBlocks.push_back(block);
  }

  // A uniform block is defined as an all paths block which is not part of a loop
  for(uint32_t block : allPathsBlocks)
  {
    if(!m_LoopBlocks.contains(block))
      m_UniformBlocks.push_back(block);
  }

  if(D3D12_DXILShaderDebugger_Logging())
  {
    RDCLOG("Block Links:");
    for(size_t i = 0; i < m_BlockLinks.size(); ++i)
    {
      uint32_t from = (uint32_t)i;
      if(m_BlockLinks[i].empty())
        continue;
      for(uint32_t to : m_BlockLinks[i])
        RDCLOG("Block:%d->Block:%d", from, to);
    }

    RDCLOG("Thread Paths:");
    rdcstr output = "";
    for(uint32_t pathIdx = 0; pathIdx < m_Paths.size(); ++pathIdx)
    {
      bool start = true;
      for(uint32_t block : m_Paths[pathIdx])
      {
        if(start)
        {
          output = StringFormat::Fmt("%d : %d", pathIdx, block);
          start = false;
        }
        else
        {
          if(block != PATH_END)
            output += " -> " + ToStr(block);
          else
            output += " : END";
        }
      }
      RDCLOG(output.c_str());
    }

    output = "";
    bool needComma = false;
    for(uint32_t block : m_LoopBlocks)
    {
      if(needComma)
        output += ", ";
      output += ToStr(block);
      needComma = true;
    }
    RDCLOG("Blocks in Loops: %s", output.c_str());

    output = "";
    needComma = false;
    for(uint32_t block : allPathsBlocks)
    {
      if(needComma)
        output += ", ";
      output += ToStr(block);
      needComma = true;
    }
    RDCLOG("Blocks in All-Paths: %s", output.c_str());

    output = "";
    needComma = false;
    for(uint32_t block : m_UniformBlocks)
    {
      if(needComma)
        output += ", ";
      output += ToStr(block);
      needComma = true;
    }
    RDCLOG("Uniform Blocks: %s", output.c_str());
  }

  // Clear temporary data
  m_TracedBlocks.clear();
  m_CheckedPaths.clear();
}

uint32_t ControlFlow::GetNextUniformBlock(uint32_t from) const
{
  // find the closest uniform block when walking the path starting at the from block
  int32_t minSteps = INT_MAX;
  uint32_t bestBlock = from;
  for(uint32_t uniform : m_UniformBlocks)
  {
    for(uint32_t pathIdx = 0; pathIdx < m_Paths.size(); ++pathIdx)
    {
      m_CheckedPaths.clear();
      m_CheckedPaths.resize(m_Paths.size());
      for(size_t i = 0; i < m_CheckedPaths.size(); ++i)
        m_CheckedPaths[i] = false;
      int32_t startIdx = m_Paths[pathIdx].indexOf(from);
      // BlockInAnyPath will also check all paths linked to from the end node of the path
      if(startIdx != -1)
      {
        int32_t steps = BlockInAnyPath(uniform, pathIdx, startIdx + 1, 0);
        if(steps != -1)
        {
          if(steps < minSteps)
          {
            minSteps = steps;
            bestBlock = uniform;
          }
        }
      }
    }
  }
  return bestBlock;
}

bool ControlFlow::IsForwardConnection(uint32_t from, uint32_t to) const
{
  if(m_Connections[from][to] == ConnectionState::Unknown)
  {
    if(IsBlockConnected(from, to))
      m_Connections[from][to] = ConnectionState::Connected;
    else
      m_Connections[from][to] = ConnectionState::NotConnected;
  }
  return m_Connections[from][to] == ConnectionState::Connected;
}
};    // namespace DXIL

#if ENABLED(ENABLE_UNIT_TESTS)

#include <limits>
#include "catch/catch.hpp"

using namespace DXIL;

TEST_CASE("DXIL Control Flow", "[dxil][controlflow]")
{
  SECTION("FindUniformBlocks")
  {
    ControlFlow controlFlow;
    rdcarray<uint32_t> uniformBlocks;
    rdcarray<uint32_t> loopBlocks;
    SECTION("Degenerate Case")
    {
      // Degenerate case
      rdcarray<BlockLink> inputs;
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(0 == uniformBlocks.count());
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(0 == loopBlocks.count());
    }
    SECTION("Just Start and End")
    {
      // Only uniform flow is the start and end
      // 0 -> 1
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(2 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(1U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(0 == loopBlocks.count());
    }

    SECTION("Single Uniform Flow")
    {
      // Single uniform flow between start and end
      // 0 -> 1 -> 3
      // 0 -> 2 -> 3
      // 3 -> 4
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({1, 3});
      inputs.push_back({0, 2});
      inputs.push_back({2, 3});
      inputs.push_back({3, 4});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(3 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(3U));
      REQUIRE(uniformBlocks.contains(4U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(0 == loopBlocks.count());
    }

    SECTION("Simple Branch")
    {
      // Simple branch
      // 0 -> 1
      // 0 -> 2
      // 1 -> 2
      // 2 -> 3
      // 2 -> 4
      // 3 -> 4
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({0, 2});
      inputs.push_back({1, 2});
      inputs.push_back({2, 3});
      inputs.push_back({2, 4});
      inputs.push_back({3, 4});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(3 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(2U));
      REQUIRE(uniformBlocks.contains(4U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(0 == loopBlocks.count());
    }
    SECTION("Finite Loop1")
    {
      // Finite loop (3 -> 4 -> 5 -> 3)
      // 0 -> 1 -> 3
      // 0 -> 2 -> 3
      // 3 -> 4 -> 5
      // 4 -> 6
      // 5 -> 3
      // 5 -> 6
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({1, 3});
      inputs.push_back({0, 2});
      inputs.push_back({2, 3});
      inputs.push_back({3, 4});
      inputs.push_back({4, 5});
      inputs.push_back({4, 6});
      inputs.push_back({5, 3});
      inputs.push_back({5, 6});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(2 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(6U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(3 == loopBlocks.count());
      REQUIRE(loopBlocks.contains(3U));
      REQUIRE(loopBlocks.contains(4U));
      REQUIRE(loopBlocks.contains(5U));
    }
    SECTION("Finite Loop2")
    {
      // Finite loop (3 -> 4 -> 5 -> 3)
      // 0 -> 1 -> 2
      // 0 -> 2
      // 2 -> 3
      // 3 -> 4 -> 5 -> 6
      // 3 -> 5 -> 3
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({1, 2});
      inputs.push_back({0, 2});
      inputs.push_back({2, 3});
      inputs.push_back({3, 4});
      inputs.push_back({4, 5});
      inputs.push_back({5, 6});
      inputs.push_back({3, 5});
      inputs.push_back({5, 3});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(3 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(2U));
      REQUIRE(uniformBlocks.contains(6U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(3 == loopBlocks.count());
      REQUIRE(loopBlocks.contains(3U));
      REQUIRE(loopBlocks.contains(4U));
      REQUIRE(loopBlocks.contains(5U));
    }

    SECTION("Infinite Loop")
    {
      // Infinite loop which never converges (3 -> 4 -> 3)
      // 0 -> 1 -> 3
      // 0 -> 2 -> 3
      // 3 -> 4
      // 4 -> 3
      // 1 -> 6
      // 2 -> 6
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({1, 3});
      inputs.push_back({0, 2});
      inputs.push_back({2, 3});
      inputs.push_back({3, 4});
      inputs.push_back({4, 3});
      inputs.push_back({1, 6});
      inputs.push_back({2, 6});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(2 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(6U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(2 == loopBlocks.count());
      REQUIRE(loopBlocks.contains(3U));
      REQUIRE(loopBlocks.contains(4U));
    }

    SECTION("Complex Case Two Loops")
    {
      // Complex case with two loops
      // Loop: 7 -> 9 -> 7, 13 -> 15 -> 13
      // 0 -> 1 -> 3
      // 0 -> 2 -> 3
      // 3 -> 4 -> 5
      // 3 -> 5
      // 5 -> 6 -> 7
      // 5 -> 11
      // 7 -> 8 -> 11
      // 7 -> 9 -> 7
      // 9 -> 10 -> 11 -> 12 -> 13 -> 14 -> 17 -> 18 -> 19 -> 20 -> 21 -> 22 -> 23 -> 26
      // 13 -> 15 -> 13
      // 15 -> 16 -> 17 -> 19 -> 21 -> 26
      // 11 -> 17
      // 22 -> 24 -> 25 -> 26
      // 24 -> 26
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({0, 2});
      inputs.push_back({2, 3});
      inputs.push_back({1, 3});
      inputs.push_back({3, 4});
      inputs.push_back({4, 5});
      inputs.push_back({3, 5});
      inputs.push_back({5, 6});
      inputs.push_back({9, 7});
      inputs.push_back({6, 7});
      inputs.push_back({7, 8});
      inputs.push_back({7, 9});
      inputs.push_back({9, 10});
      inputs.push_back({10, 11});
      inputs.push_back({8, 11});
      inputs.push_back({5, 11});
      inputs.push_back({11, 12});
      inputs.push_back({15, 13});
      inputs.push_back({12, 13});
      inputs.push_back({13, 14});
      inputs.push_back({13, 15});
      inputs.push_back({15, 16});
      inputs.push_back({16, 17});
      inputs.push_back({14, 17});
      inputs.push_back({11, 17});
      inputs.push_back({17, 18});
      inputs.push_back({18, 19});
      inputs.push_back({17, 19});
      inputs.push_back({19, 20});
      inputs.push_back({20, 21});
      inputs.push_back({19, 21});
      inputs.push_back({21, 22});
      inputs.push_back({22, 23});
      inputs.push_back({22, 24});
      inputs.push_back({24, 25});
      inputs.push_back({25, 26});
      inputs.push_back({24, 26});
      inputs.push_back({23, 26});
      inputs.push_back({21, 26});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(8 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(3U));
      REQUIRE(uniformBlocks.contains(5U));
      REQUIRE(uniformBlocks.contains(11U));
      REQUIRE(uniformBlocks.contains(17U));
      REQUIRE(uniformBlocks.contains(19U));
      REQUIRE(uniformBlocks.contains(21U));
      REQUIRE(uniformBlocks.contains(26U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(4 == loopBlocks.count());
      REQUIRE(loopBlocks.contains(7U));
      REQUIRE(loopBlocks.contains(9U));
      REQUIRE(loopBlocks.contains(13U));
      REQUIRE(loopBlocks.contains(15U));
    }
    SECTION("Complex Case Multiple Loops")
    {
      // Complex case with multiple loops: 4 -> 5 -> 6 -> 4, 10 -> 11 -> 12 -> 10, 68
      // 0 -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 4
      // 0 -> 2 -> 8 -> 9 -> 10 -> 11 -> 12 -> 10
      // 4 -> 6 -> 7 -> 8 -> 14 -> 15 -> 19 -> 20 -> 24 -> 25 -> 29 -> 31 -> 32 -> 33
      // 10 -> 12 -> 13 -> 14 -> 16 -> 17 -> 19
      // 16 -> 18 -> 19 -> 21 -> 22 -> 23 -> 24 -> 26 -> 27 -> 29 -> 30 -> 33 -> 35 -> 37
      // 22 -> 24
      // 26 -> 28 -> 29

      // 31 -> 33 -> 34 -> 37 -> 39 -> 40 -> 41 -> 42 -> 43 -> 44 -> 45 -> 47 -> 49 -> 51 -> 52 ->
      //   53 -> 54 -> 55 -> 57 -> 58 -> 59 -> 60 -> 61 -> 62 -> 63 -> 64 -> 65 -> 66 -> 68 -> 67 ->
      //   69 -> 70 -> 71 -> 72 -> 73 -> 74 -> 75 -> 76 -> 77 -> 78 -> 79 -> END

      // 35 -> 36 -> 37 -> 38 -> 41 39 -> 41 -> 43 -> 45 -> 46 -> 47 -> 48 -> 49 -> 50 -> 51
      // 51 -> 53 -> 55 -> 56 -> 57 -> 59 -> 61 -> 63 -> 65 -> 69 -> 71 -> 73 -> 75 -> 77 -> 79
      // 68 -> 68
      rdcarray<BlockLink> inputs;
      inputs.push_back({8, 9});
      inputs.push_back({8, 14});
      inputs.push_back({64, 65});
      inputs.push_back({0, 1});
      inputs.push_back({0, 2});
      inputs.push_back({1, 2});
      inputs.push_back({2, 3});
      inputs.push_back({2, 8});
      inputs.push_back({6, 4});
      inputs.push_back({6, 7});
      inputs.push_back({3, 4});
      inputs.push_back({4, 5});
      inputs.push_back({4, 6});
      inputs.push_back({5, 6});
      inputs.push_back({7, 8});
      inputs.push_back({12, 10});
      inputs.push_back({12, 13});
      inputs.push_back({9, 10});
      inputs.push_back({10, 11});
      inputs.push_back({10, 12});
      inputs.push_back({11, 12});
      inputs.push_back({13, 14});
      inputs.push_back({14, 15});
      inputs.push_back({14, 16});
      inputs.push_back({16, 17});
      inputs.push_back({16, 18});
      inputs.push_back({18, 19});
      inputs.push_back({17, 19});
      inputs.push_back({15, 19});
      inputs.push_back({19, 20});
      inputs.push_back({19, 21});
      inputs.push_back({21, 22});
      inputs.push_back({21, 23});
      inputs.push_back({23, 24});
      inputs.push_back({22, 24});
      inputs.push_back({20, 24});
      inputs.push_back({24, 25});
      inputs.push_back({24, 26});
      inputs.push_back({26, 27});
      inputs.push_back({26, 28});
      inputs.push_back({28, 29});
      inputs.push_back({27, 29});
      inputs.push_back({25, 29});
      inputs.push_back({29, 30});
      inputs.push_back({29, 31});
      inputs.push_back({31, 32});
      inputs.push_back({31, 33});
      inputs.push_back({32, 33});
      inputs.push_back({30, 33});
      inputs.push_back({33, 34});
      inputs.push_back({33, 35});
      inputs.push_back({35, 37});
      inputs.push_back({35, 36});
      inputs.push_back({36, 37});
      inputs.push_back({34, 37});
      inputs.push_back({37, 38});
      inputs.push_back({37, 39});
      inputs.push_back({39, 40});
      inputs.push_back({39, 41});
      inputs.push_back({40, 41});
      inputs.push_back({38, 41});
      inputs.push_back({41, 42});
      inputs.push_back({41, 43});
      inputs.push_back({42, 43});
      inputs.push_back({43, 44});
      inputs.push_back({43, 45});
      inputs.push_back({44, 45});
      inputs.push_back({45, 46});
      inputs.push_back({45, 47});
      inputs.push_back({46, 47});
      inputs.push_back({47, 48});
      inputs.push_back({47, 49});
      inputs.push_back({48, 49});
      inputs.push_back({49, 50});
      inputs.push_back({49, 51});
      inputs.push_back({50, 51});
      inputs.push_back({51, 52});
      inputs.push_back({51, 53});
      inputs.push_back({52, 53});
      inputs.push_back({53, 54});
      inputs.push_back({53, 55});
      inputs.push_back({54, 55});
      inputs.push_back({55, 56});
      inputs.push_back({55, 57});
      inputs.push_back({56, 57});
      inputs.push_back({57, 58});
      inputs.push_back({57, 59});
      inputs.push_back({58, 59});
      inputs.push_back({59, 60});
      inputs.push_back({59, 61});
      inputs.push_back({60, 61});
      inputs.push_back({61, 62});
      inputs.push_back({61, 63});
      inputs.push_back({62, 63});
      inputs.push_back({63, 64});
      inputs.push_back({63, 65});
      inputs.push_back({65, 66});
      inputs.push_back({65, 69});
      inputs.push_back({68, 67});
      inputs.push_back({68, 68});
      inputs.push_back({66, 68});
      inputs.push_back({67, 69});
      inputs.push_back({69, 70});
      inputs.push_back({69, 71});
      inputs.push_back({70, 71});
      inputs.push_back({71, 72});
      inputs.push_back({71, 73});
      inputs.push_back({72, 73});
      inputs.push_back({73, 74});
      inputs.push_back({73, 75});
      inputs.push_back({74, 75});
      inputs.push_back({75, 76});
      inputs.push_back({75, 77});
      inputs.push_back({76, 77});
      inputs.push_back({77, 78});
      inputs.push_back({77, 79});
      inputs.push_back({78, 79});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(28 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(2U));
      REQUIRE(uniformBlocks.contains(8U));
      REQUIRE(uniformBlocks.contains(14U));
      REQUIRE(uniformBlocks.contains(19U));
      REQUIRE(uniformBlocks.contains(24U));
      REQUIRE(uniformBlocks.contains(29U));
      REQUIRE(uniformBlocks.contains(33U));
      REQUIRE(uniformBlocks.contains(37U));
      REQUIRE(uniformBlocks.contains(41U));
      REQUIRE(uniformBlocks.contains(43U));
      REQUIRE(uniformBlocks.contains(45U));
      REQUIRE(uniformBlocks.contains(47U));
      REQUIRE(uniformBlocks.contains(49U));
      REQUIRE(uniformBlocks.contains(51U));
      REQUIRE(uniformBlocks.contains(53U));
      REQUIRE(uniformBlocks.contains(55U));
      REQUIRE(uniformBlocks.contains(57U));
      REQUIRE(uniformBlocks.contains(59U));
      REQUIRE(uniformBlocks.contains(61U));
      REQUIRE(uniformBlocks.contains(63U));
      REQUIRE(uniformBlocks.contains(65U));
      REQUIRE(uniformBlocks.contains(69U));
      REQUIRE(uniformBlocks.contains(71U));
      REQUIRE(uniformBlocks.contains(73U));
      REQUIRE(uniformBlocks.contains(75U));
      REQUIRE(uniformBlocks.contains(77U));
      REQUIRE(uniformBlocks.contains(79U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(7 == loopBlocks.count());
      REQUIRE(loopBlocks.contains(4U));
      REQUIRE(loopBlocks.contains(5U));
      REQUIRE(loopBlocks.contains(6U));
      REQUIRE(loopBlocks.contains(10U));
      REQUIRE(loopBlocks.contains(11U));
      REQUIRE(loopBlocks.contains(12U));
      REQUIRE(loopBlocks.contains(68U));
    }
    SECTION("Single Loop Specific Setup")
    {
      // Specific loop case where a block (2) in a loop is only in a single path
      // 0 -> 1 -> 3 - 1
      // 0 -> 1 -> 2 -> 3
      // 3 -> 4 -> END
      rdcarray<BlockLink> inputs;
      inputs.push_back({0, 1});
      inputs.push_back({1, 3});
      inputs.push_back({3, 1});
      inputs.push_back({1, 2});
      inputs.push_back({2, 3});
      inputs.push_back({3, 4});
      controlFlow.Construct(inputs);
      uniformBlocks = controlFlow.GetUniformBlocks();
      REQUIRE(2 == uniformBlocks.count());
      REQUIRE(uniformBlocks.contains(0U));
      REQUIRE(uniformBlocks.contains(4U));
      loopBlocks = controlFlow.GetLoopBlocks();
      REQUIRE(3 == loopBlocks.count());
      REQUIRE(loopBlocks.contains(1U));
      REQUIRE(loopBlocks.contains(2U));
      REQUIRE(loopBlocks.contains(3U));
    }
  };
};
#endif    // ENABLED(ENABLE_UNIT_TESTS)
