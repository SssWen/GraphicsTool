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

#include "core/settings.h"
#include "driver/dxgi/dxgi_common.h"
#include "d3d12_command_list.h"
#include "d3d12_command_queue.h"
#include "d3d12_debug.h"
#include "d3d12_device.h"
#include "d3d12_manager.h"
#include "d3d12_resources.h"

RDOC_EXTERN_CONFIG(bool, D3D12_Debug_SingleSubmitFlushing);

RDOC_CONFIG(bool, D3D12_Debug_DriverASSerialisation, false,
            "Use driver-side serialisation for saving and restoring ASs");

RDOC_EXTERN_CONFIG(bool, D3D12_Debug_RT_Auditing);

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, ASBuildData::RVAWithStride &el)
{
  SERIALISE_MEMBER(RVA);
  SERIALISE_MEMBER(StrideInBytes);
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, ASBuildData::RVATrianglesDesc &el)
{
  SERIALISE_MEMBER(Transform3x4);
  SERIALISE_MEMBER(IndexFormat);
  SERIALISE_MEMBER(VertexFormat);
  SERIALISE_MEMBER(IndexCount);
  SERIALISE_MEMBER(VertexCount);
  SERIALISE_MEMBER(IndexBuffer);
  SERIALISE_MEMBER(VertexBuffer);
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, ASBuildData::RVAAABBDesc &el)
{
  SERIALISE_MEMBER(AABBCount);
  SERIALISE_MEMBER(AABBs);
}

template <class SerialiserType>
void DoSerialise(SerialiserType &ser, ASBuildData::RTGeometryDesc &el)
{
  SERIALISE_MEMBER(Type);
  SERIALISE_MEMBER(Flags);

  if(el.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES)
  {
    SERIALISE_MEMBER(Triangles);
  }
  else
  {
    SERIALISE_MEMBER(AABBs);
  }
}

bool D3D12ResourceManager::Prepare_InitialState(ID3D12DeviceChild *res)
{
  ResourceId id = GetResID(res);
  D3D12ResourceType type = IdentifyTypeByPtr(res);

  if(type == Resource_DescriptorHeap)
  {
    WrappedID3D12DescriptorHeap *heap = (WrappedID3D12DescriptorHeap *)res;

    UINT numElems = heap->GetDesc().NumDescriptors;

    D3D12Descriptor *descs = new D3D12Descriptor[numElems];
    memcpy(descs, heap->GetDescriptors(), sizeof(D3D12Descriptor) * numElems);

    D3D12InitialContents initContents(descs, numElems);

    if(heap->HasNames())
      initContents.descriptorNames = heap->GetNames();

    SetInitialContents(heap->GetResourceID(), initContents);
    return true;
  }
  else if(type == Resource_Resource)
  {
    WrappedID3D12Resource *r = (WrappedID3D12Resource *)res;
    ID3D12Pageable *unwrappedPageable = r->UnwrappedResidencyPageable();

    bool nonresident = false;
    if(!r->IsResident())
      nonresident = true;

    D3D12_RESOURCE_DESC desc = r->GetDesc();

    D3D12InitialContents initContents;

    Sparse::PageTable *sparseTable = NULL;

    if(GetRecord(r)->sparseTable)
      sparseTable = new Sparse::PageTable(*GetRecord(r)->sparseTable);

    if(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
      if(r->IsAccelerationStructureResource())
      {
        initContents = D3D12InitialContents(D3D12InitialContents::AccelerationStructure, NULL);
        SetInitialContents(GetResID(r), initContents);
        return true;
      }

      D3D12_HEAP_PROPERTIES heapProps = {};

      if(sparseTable == NULL)
        r->GetHeapProperties(&heapProps, NULL);

      HRESULT hr = S_OK;

      if(heapProps.Type == D3D12_HEAP_TYPE_READBACK)
      {
        // readback resources can't be copied by the GPU but are always immediately CPU readable, so
        // copy to a buffer now
        size_t size = size_t(desc.Width);
        byte *buffer = AllocAlignedBuffer(RDCMAX(desc.Width, 64ULL));

        byte *bufData = NULL;
        hr = r->GetReal()->Map(0, NULL, (void **)&bufData);

        if(SUCCEEDED(hr))
        {
          memcpy(buffer, bufData, size);

          D3D12_RANGE range = {};
          r->GetReal()->Unmap(0, &range);
        }
        else
        {
          RDCERR("Couldn't map directly readback buffer: HRESULT: %s", ToStr(hr).c_str());
        }

        SetInitialContents(GetResID(r), D3D12InitialContents(buffer, size));
        return true;
      }

      const bool isUploadHeap = (heapProps.Type == D3D12_HEAP_TYPE_UPLOAD);

      desc.Flags = D3D12_RESOURCE_FLAG_NONE;

      ID3D12Resource *copyDst = NULL;
      hr = m_Device->CreateInitialStateBuffer(desc, &copyDst);

      if(nonresident)
        m_Device->GetReal()->MakeResident(1, &unwrappedPageable);

      const SubresourceStateVector &states = m_Device->GetSubresourceStates(GetResID(res));
      RDCASSERT(states.size() == 1);

      D3D12_RESOURCE_BARRIER barrier;
      // upload heap resources can't be transitioned, and any resources in the new layouts don't
      // need to either since each submit does a big flush
      const bool needsTransition = !isUploadHeap && states[0].IsStates() &&
                                   (states[0].ToStates() & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;

      if(needsTransition)
      {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = r->GetReal();
        barrier.Transition.Subresource = (UINT)0;
        barrier.Transition.StateBefore = states[0].ToStates();
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      }

      ID3D12GraphicsCommandList *list = Unwrap(m_Device->GetInitialStateList());

      // transition to copy source
      if(needsTransition)
        list->ResourceBarrier(1, &barrier);

      if(SUCCEEDED(hr))
      {
        list->CopyResource(copyDst, r->GetReal());
      }
      else
      {
        RDCERR("Couldn't create readback buffer: HRESULT: %s", ToStr(hr).c_str());
      }

      // transition back to whatever it was before
      if(needsTransition)
      {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
      }

      if(nonresident)
      {
        m_Device->CloseInitialStateList();

        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists();

        m_Device->GetReal()->Evict(1, &unwrappedPageable);
      }
      else if(D3D12_Debug_SingleSubmitFlushing())
      {
        m_Device->CloseInitialStateList();
        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists(true);
      }

      initContents = D3D12InitialContents(copyDst);
    }
    else
    {
      if(nonresident)
        m_Device->GetReal()->MakeResident(1, &unwrappedPageable);

      ID3D12Resource *arrayTexture = NULL;
      BarrierSet::AccessType accessType = BarrierSet::CopySourceAccess;
      ID3D12Resource *unwrappedCopySource = r->GetReal();

      bool isDepth =
          IsDepthFormat(desc.Format) || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;

      bool isMSAA = false;

      if(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count > 1)
      {
        desc.Alignment = 0;
        desc.DepthOrArraySize *= (UINT16)desc.SampleDesc.Count;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        if(isDepth)
          desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        else
          desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES defaultHeap;
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;

        // we don't want to serialise this resource's creation, so wrap it manually
        HRESULT hr = m_Device->GetReal()->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            __uuidof(ID3D12Resource), (void **)&arrayTexture);
        RDCASSERTEQUAL(hr, S_OK);

        accessType = BarrierSet::SRVAccess;
        isMSAA = true;
      }

      ID3D12GraphicsCommandListX *list = m_Device->GetInitialStateList();

      BarrierSet barriers;

      barriers.Configure(r, m_Device->GetSubresourceStates(GetResID(r)), accessType);
      barriers.Apply(list);

      if(arrayTexture)
      {
        // execute the above barriers
        m_Device->CloseInitialStateList();

        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists();

        // expand multisamples out to array
        m_Device->GetDebugManager()->CopyTex2DMSToArray(NULL, arrayTexture, r->GetReal());

        // open the initial state list again for the remainder of the work
        list = m_Device->GetInitialStateList();

        D3D12_RESOURCE_BARRIER b = {};
        b.Transition.pResource = arrayTexture;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore =
            isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // arrayTexture is not wrapped so we need to call the unwrapped command directly
        Unwrap(list)->ResourceBarrier(1, &b);

        unwrappedCopySource = arrayTexture;
      }

      D3D12_RESOURCE_DESC bufDesc;

      bufDesc.Alignment = 0;
      bufDesc.DepthOrArraySize = 1;
      bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
      bufDesc.Format = DXGI_FORMAT_UNKNOWN;
      bufDesc.Height = 1;
      bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      bufDesc.MipLevels = 1;
      bufDesc.SampleDesc.Count = 1;
      bufDesc.SampleDesc.Quality = 0;
      bufDesc.Width = 0;

      UINT numSubresources = desc.MipLevels;
      if(desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        numSubresources *= desc.DepthOrArraySize;

      // account for multiple planes (i.e. depth and stencil)
      {
        D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = {};
        formatInfo.Format = desc.Format;
        m_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo));

        UINT planes = RDCMAX((UINT8)1, formatInfo.PlaneCount);

        numSubresources *= planes;
      }

      D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};

      rdcarray<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> copyLayouts;
      rdcarray<uint32_t> subresources;

      if(IsBlockFormat(desc.Format) && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
      {
        RDCDEBUG("Removing UAV flag from BCn desc to allow GetCopyableFootprints");
        desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      }

      for(UINT i = 0; i < numSubresources; i++)
      {
        // skip non-MSAA sparse subresources that are not mapped at all
        if(!isMSAA && sparseTable && !sparseTable->getPageRangeMapping(i).isMapped())
          continue;

        UINT64 subSize = 0;
        m_Device->GetCopyableFootprints(&desc, i, 1, bufDesc.Width, &layout, NULL, NULL, &subSize);

        if(subSize == ~0ULL)
        {
          RDCERR("Failed to call GetCopyableFootprints on %s! skipping copy", ToStr(id).c_str());
          continue;
        }

        copyLayouts.push_back(layout);
        subresources.push_back(i);
        bufDesc.Width += subSize;
        bufDesc.Width = AlignUp<UINT64>(bufDesc.Width, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }

      if(bufDesc.Width == 0)
        bufDesc.Width = 1U;

      ID3D12Resource *copyDst = NULL;
      HRESULT hr = m_Device->CreateInitialStateBuffer(bufDesc, &copyDst);

      if(SUCCEEDED(hr))
      {
        for(UINT i = 0; i < copyLayouts.size(); i++)
        {
          D3D12_TEXTURE_COPY_LOCATION dst, src;

          src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          src.pResource = unwrappedCopySource;
          src.SubresourceIndex = subresources[i];

          dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          dst.pResource = copyDst;
          dst.PlacedFootprint = copyLayouts[i];

          Unwrap(list)->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
        }
      }
      else
      {
        RDCERR("Couldn't create readback buffer: HRESULT: %s", ToStr(hr).c_str());
      }

      // If we're not a sparse single-sampled texture, we copy the whole resource with all
      // subresources. (In the loop above the continue will never be hit, so we can indicate quickly
      // here that all subresources are present without needing to have {0...n}
      if(isMSAA || sparseTable == NULL)
        subresources = {~0U};

      // transition back
      barriers.Unapply(list);

      if(nonresident || arrayTexture)
      {
        m_Device->CloseInitialStateList();

        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists();

        if(nonresident)
          m_Device->GetReal()->Evict(1, &unwrappedPageable);
      }
      else if(D3D12_Debug_SingleSubmitFlushing())
      {
        m_Device->CloseInitialStateList();
        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists(true);
      }

      SAFE_RELEASE(arrayTexture);

      initContents = D3D12InitialContents(copyDst);
      initContents.subresources = subresources;
    }

    initContents.sparseTable = sparseTable;

    SetInitialContents(GetResID(r), initContents);
    return true;
  }
  else if(type == Resource_AccelerationStructure)
  {
    D3D12AccelerationStructure *r = (D3D12AccelerationStructure *)res;

    D3D12InitialContents initContents(D3D12InitialContents::AccelerationStructure, NULL);
    initContents.resourceType = Resource_AccelerationStructure;

    if(D3D12_Debug_DriverASSerialisation())
    {
      D3D12_GPU_VIRTUAL_ADDRESS asAddress = r->GetVirtualAddress();

      D3D12_RESOURCE_DESC desc;

      desc.Alignment = 0;
      desc.DepthOrArraySize = 1;
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      desc.Format = DXGI_FORMAT_UNKNOWN;
      desc.Height = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.SampleDesc.Quality = 0;

      ID3D12GraphicsCommandList4 *list4 = NULL;

      UINT64 blasCount = 0;

      // get the size
      {
        D3D12GpuBuffer *ASQueryBuffer = GetRTManager()->ASQueryBuffer;

        list4 = Unwrap4(m_Device->GetInitialStateList());

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC emitDesc = {};
        emitDesc.DestBuffer = ASQueryBuffer->Address();
        emitDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;

        list4->EmitRaytracingAccelerationStructurePostbuildInfo(&emitDesc, 1, &asAddress);

        m_Device->CloseInitialStateList();

        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC *serSize =
            (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC *)
                ASQueryBuffer->Map();

        if(!serSize)
        {
          RDCERR("Couldn't map AS query buffer");
          return false;
        }

        desc.Width = serSize->SerializedSizeInBytes;
        blasCount = serSize->NumBottomLevelAccelerationStructurePointers;

        ASQueryBuffer->Unmap();

        // no other copies are in flight because of the above sync so we can resize this
        GetRTManager()->ResizeSerialisationBuffer(desc.Width);
      }

      ID3D12Resource *copyDst = NULL;
      HRESULT hr = m_Device->CreateInitialStateBuffer(desc, &copyDst);

      if(FAILED(hr))
      {
        RDCERR("Couldn't create serialisation buffer: HRESULT: %s", ToStr(hr).c_str());
        return false;
      }

      list4 = Unwrap4(m_Device->GetInitialStateList());

      if(SUCCEEDED(hr))
      {
        D3D12GpuBuffer *ASSerialiseBuffer = GetRTManager()->ASSerialiseBuffer;

        list4->CopyRaytracingAccelerationStructure(
            ASSerialiseBuffer->Address(), r->GetVirtualAddress(),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);

        D3D12_RESOURCE_BARRIER b = {};
        b.Transition.pResource = ASSerialiseBuffer->Resource();
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        list4->ResourceBarrier(1, &b);

        list4->CopyBufferRegion(copyDst, 0, ASSerialiseBuffer->Resource(),
                                ASSerialiseBuffer->Offset(), desc.Width);
      }
      else
      {
        RDCERR("Couldn't create readback buffer: HRESULT: %s", ToStr(hr).c_str());
      }

      initContents.resource = copyDst;
      copyDst->AddRef();

      if(D3D12_Debug_SingleSubmitFlushing())
      {
        m_Device->CloseInitialStateList();
        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists(true);
      }
    }
    else
    {
      // on D3D12 ASs are created on the fly by a build, so we should always have build data.
      if(!r->buildData)
      {
        RDCERR("AS with no build data");
        return false;
      }

      initContents.buildData = r->buildData;
      SAFE_ADDREF(r->buildData);
    }

    SetInitialContents(r->GetResourceID(), initContents);
    return true;
  }
  else
  {
    RDCERR("Unexpected type needing an initial state prepared: %d", type);
  }

  return false;
}

uint64_t D3D12ResourceManager::GetSize_InitialState(ResourceId id, const D3D12InitialContents &data)
{
  if(data.resourceType == Resource_DescriptorHeap)
  {
    // the initial contents are just the descriptors. Estimate the serialise size here
    const uint64_t descriptorSerSize = 40 + sizeof(D3D12_SAMPLER_DESC);

    // add a little extra room for fixed overhead
    return 64 + data.numDescriptors * descriptorSerSize;
  }
  else if(data.resourceType == Resource_Resource)
  {
    ID3D12Resource *buf = (ID3D12Resource *)data.resource;

    uint64_t ret = WriteSerialiser::GetChunkAlignment() + 64;

    if(data.tag == D3D12InitialContents::AccelerationStructure)
      return ret;

    if(data.sparseTable)
      ret += 16 + data.sparseTable->GetSerialiseSize();

    // readback heaps have already been copied to a buffer, so use that length
    if(data.tag == D3D12InitialContents::MapDirect)
      return ret + uint64_t(data.dataSize);

    return ret + uint64_t(buf ? buf->GetDesc().Width : 0);
  }
  else if(data.resourceType == Resource_AccelerationStructure)
  {
    uint64_t ret = WriteSerialiser::GetChunkAlignment();

    if(D3D12_Debug_DriverASSerialisation())
    {
      ID3D12Resource *buf = (ID3D12Resource *)data.resource;

      // driver serialisation flag
      ret += 64;

      return ret + uint64_t(buf ? buf->GetDesc().Width : 0);
    }
    else
    {
      ASBuildData *buildData = data.buildData;

      // driver serialisation flag
      // type/flags/count
      ret += 64;

      // geometries serialise size is no larger than the desc because it's all single elements with
      // no expansion or array counts
      ret += 64 + sizeof(D3D12_RAYTRACING_GEOMETRY_DESC) * buildData->geoms.size();

      if(buildData->buffer)
        ret += 64 + buildData->buffer->Size();

      ret += 64 + buildData->diskCache.size;

      return ret;
    }
  }
  else
  {
    RDCERR("Unexpected type needing an initial state serialised: %d", data.resourceType);
  }

  return 16;
}

SparseBinds::SparseBinds(const Sparse::PageTable &table)
{
  const uint32_t pageSize = 64 * 1024;

  // in theory some of these subresources may share a single binding but we don't try to extract
  // that out again. If we can get one bind per subresource and avoid falling down to per-page
  // mappings we're happy
  for(uint32_t sub = 0; sub < RDCMAX(1U, table.getNumSubresources());)
  {
    const Sparse::PageRangeMapping &mapping =
        table.isSubresourceInMipTail(sub) ? table.getMipTailMapping(sub) : table.getSubresource(sub);

    if(mapping.hasSingleMapping())
    {
      Bind bind;
      bind.heap = mapping.singleMapping.memory;
      bind.rangeOffset = uint32_t(mapping.singleMapping.offset / pageSize);
      bind.rangeCount = uint32_t(table.isSubresourceInMipTail(sub)
                                     ? (table.getMipTailSliceSize() + pageSize - 1) / pageSize
                                     : (table.getSubresourceByteSize(sub) + pageSize - 1) / pageSize);
      bind.regionStart = {0, 0, 0, sub};
      bind.regionSize = {bind.rangeCount, FALSE, bind.rangeCount, 1, 1};
      bind.rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
      if(bind.heap == ResourceId())
        bind.rangeFlag = D3D12_TILE_RANGE_FLAG_NULL;
      else if(mapping.singlePageReused)
        bind.rangeFlag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
      binds.push_back(bind);
    }
    else
    {
      Sparse::Coord texelShape = table.calcSubresourcePageDim(sub);

      // march the pages for this subresource in linear order
      for(uint32_t page = 0; page < mapping.pages.size(); page++)
      {
        Bind bind;
        bind.heap = mapping.pages[page].memory;
        bind.rangeOffset = uint32_t(mapping.pages[page].offset / pageSize);

        // do simple coalescing. If the previous bind was in the same heap, one tile back, make it
        // cover this tile
        if(page > 0 && binds.back().heap == bind.heap &&
           (binds.back().rangeOffset + binds.back().rangeCount == bind.rangeOffset ||
            binds.back().heap == ResourceId()))
        {
          binds.back().regionSize.NumTiles++;
          binds.back().regionSize.Width++;
          binds.back().rangeCount++;
          continue;
        }

        // otherwise add a new bind
        if(table.isSubresourceInMipTail(sub))
        {
          bind.regionStart = {page, 0, 0, sub};
        }
        else
        {
          bind.regionStart.Subresource = sub;
          // set the starting co-ord as appropriate for this page
          bind.regionStart.X = page % texelShape.x;
          bind.regionStart.Y = (page / texelShape.x) % texelShape.y;
          bind.regionStart.Z = page / (texelShape.x * texelShape.y);
        }

        bind.rangeCount = 1;
        bind.regionSize = {1, FALSE, 1, 1, 1};
        bind.rangeFlag = D3D12_TILE_RANGE_FLAG_NONE;
        if(bind.heap == ResourceId())
          bind.rangeFlag = D3D12_TILE_RANGE_FLAG_NULL;

        binds.push_back(bind);
      }
    }

    if(table.isSubresourceInMipTail(sub))
    {
      // move to the next subresource after the miptail, since we handle the miptail all at once
      sub = ((sub / table.getMipCount()) + 1) * table.getMipCount();
    }
    else
    {
      sub++;
    }
  }
}

SparseBinds::SparseBinds(int)
{
  null = true;
}

void SparseBinds::Apply(WrappedID3D12Device *device, ID3D12Resource *resource)
{
  if(null)
  {
    D3D12_TILE_RANGE_FLAGS rangeFlags = D3D12_TILE_RANGE_FLAG_NULL;

    // do a single whole-resource bind of NULL
    device->GetQueue()->UpdateTileMappings(resource, 1, NULL, NULL, NULL, 1, &rangeFlags, NULL,
                                           NULL, D3D12_TILE_MAPPING_FLAG_NONE);
  }
  else
  {
    D3D12ResourceManager *rm = device->GetResourceManager();
    for(const Bind &bind : binds)
    {
      device->GetQueue()->UpdateTileMappings(
          resource, 1, &bind.regionStart, &bind.regionSize,
          bind.heap == ResourceId() ? NULL : (ID3D12Heap *)rm->GetLiveResource(bind.heap), 1,
          &bind.rangeFlag, &bind.rangeOffset, &bind.rangeCount, D3D12_TILE_MAPPING_FLAG_NONE);
    }
  }
}

template <typename SerialiserType>
bool D3D12ResourceManager::Serialise_InitialState(SerialiserType &ser, ResourceId id,
                                                  D3D12ResourceRecord *record,
                                                  const D3D12InitialContents *initial)
{
  m_State = m_Device->GetState();

  bool ret = true;

  SERIALISE_ELEMENT(id).TypedAs("ID3D12DeviceChild *"_lit).Important();
  SERIALISE_ELEMENT_LOCAL(type, record->type);

  if(IsReplayingAndReading())
  {
    m_Device->AddResourceCurChunk(id);
  }

  if(type == Resource_DescriptorHeap)
  {
    D3D12Descriptor *Descriptors = initial ? initial->descriptors : NULL;
    uint32_t numElems = initial ? initial->numDescriptors : 0;
    rdcarray<rdcstr> names = initial ? initial->descriptorNames : rdcarray<rdcstr>();

    // there's no point in setting up a lazy array when we're structured exporting because we KNOW
    // we're going to need all the data anyway.
    if(!IsStructuredExporting(m_State))
      ser.SetLazyThreshold(1000);

    SERIALISE_ELEMENT_ARRAY(Descriptors, numElems);
    SERIALISE_ELEMENT(numElems).Named("NumDescriptors"_lit).Important();

    if(ser.VersionAtLeast(0x13))
    {
      SERIALISE_ELEMENT(names).Hidden();
    }

    ser.SetLazyThreshold(0);

    SERIALISE_CHECK_READ_ERRORS();

    if(IsReplayingAndReading())
    {
      WrappedID3D12DescriptorHeap *heap = (WrappedID3D12DescriptorHeap *)GetLiveResource(id);

      if(!names.empty())
        heap->GetNames() = names;

      D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();

      // this heap doesn't have to be shader visible, we just use it to copy from
      desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

      ID3D12DescriptorHeap *copyheap = NULL;
      HRESULT hr = m_Device->GetReal()->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                                             (void **)&copyheap);

      if(FAILED(hr))
      {
        RDCERR("Failed to create CPU descriptor heap for initial state: HRESULT: %s",
               ToStr(hr).c_str());
        return false;
      }

      if(Descriptors == NULL)
      {
        RDCERR("Failed to correctly serialise descriptor heap initial state");
        return false;
      }

      copyheap = new WrappedID3D12DescriptorHeap(copyheap, m_Device, desc, heap->GetNumDescriptors());

      D3D12_CPU_DESCRIPTOR_HANDLE handle = copyheap->GetCPUDescriptorHandleForHeapStart();

      UINT increment = m_Device->GetDescriptorHandleIncrementSize(desc.Type);

      // only iterate over the 'real' number of descriptors, not the number after we've patched
      desc.NumDescriptors = heap->GetNumDescriptors();

      // to remove any ray query work, force AS descriptors to NULL
      if(D3D12_Debug_RT_Auditing())
      {
        for(uint32_t i = 0; i < RDCMIN(numElems, desc.NumDescriptors); i++)
        {
          if(Descriptors[i].GetType() == D3D12DescriptorType::SRV)
          {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = Descriptors[i].GetSRV();
            if(srvDesc.ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
            {
              srvDesc.RaytracingAccelerationStructure.Location = 0;
              Descriptors[i].Init(NULL, &srvDesc);
            }
          }
        }
      }

      for(uint32_t i = 0; i < RDCMIN(numElems, desc.NumDescriptors); i++)
      {
        Descriptors[i].Create(desc.Type, m_Device, handle);

        handle.ptr += increment;
      }

      SetInitialContents(id, D3D12InitialContents(copyheap));
    }
  }
  else if(type == Resource_Resource)
  {
    byte *ResourceContents = NULL;
    uint64_t ContentsLength = 0;
    byte *dummy = NULL;
    ID3D12Resource *mappedBuffer = NULL;

    ID3D12Resource *liveRes = NULL;

    if(IsReplayingAndReading())
    {
      liveRes = (ID3D12Resource *)GetLiveResource(id);
    }

    SparseBinds *sparseBinds = NULL;
    rdcarray<uint32_t> subresourcesIncluded;
    if(initial)
      subresourcesIncluded = initial->subresources;

    // default to {~0U} if this isn't present, which means 'all subresources serialised', since an
    // empty array is valid and means NO subresources were serialised.
    if(ser.VersionAtLeast(0xE))
    {
      SERIALISE_ELEMENT(subresourcesIncluded);
    }
    else
    {
      subresourcesIncluded = {~0U};
    }

    if(ser.VersionAtLeast(0xB))
    {
      Sparse::PageTable *sparseTable = initial ? initial->sparseTable : NULL;

      SERIALISE_ELEMENT_OPT(sparseTable);

      if(sparseTable)
        sparseBinds = new SparseBinds(*sparseTable);
    }

    if(ser.IsWriting())
    {
      m_Device->ExecuteLists(NULL, true);
      m_Device->FlushLists();

      RDCASSERT(initial);

      mappedBuffer = (ID3D12Resource *)initial->resource;

      if(initial->tag == D3D12InitialContents::AccelerationStructure)
      {
        mappedBuffer = NULL;
      }
      else if(initial->tag == D3D12InitialContents::MapDirect)
      {
        // this was a readback heap, so we did the readback in Prepare already to a buffer
        ResourceContents = initial->srcData;
        ContentsLength = uint64_t(initial->dataSize);
        mappedBuffer = NULL;
      }
      else if(mappedBuffer)
      {
        HRESULT hr = mappedBuffer->Map(0, NULL, (void **)&ResourceContents);
        ContentsLength = mappedBuffer->GetDesc().Width;

        if(FAILED(hr) || ResourceContents == NULL)
        {
          ContentsLength = 0;
          ResourceContents = NULL;
          mappedBuffer = NULL;

          RDCERR("Failed to map buffer for readback! %s", ToStr(hr).c_str());
          ret = false;
        }
      }
    }

    // serialise the size separately so we can recreate on replay
    SERIALISE_ELEMENT(ContentsLength);

    // only map on replay if we haven't encountered any errors so far
    if(IsReplayingAndReading() && !ser.IsErrored())
    {
      D3D12_RESOURCE_DESC resDesc = liveRes->GetDesc();

      D3D12_HEAP_PROPERTIES heapProps = {};
      if(!m_Device->IsSparseResource(GetResID(liveRes)))
        liveRes->GetHeapProperties(&heapProps, NULL);

      const bool isCPUCopyHeap =
          heapProps.Type == D3D12_HEAP_TYPE_CUSTOM &&
          (heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
           heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE) &&
          heapProps.MemoryPoolPreference == D3D12_MEMORY_POOL_L0;

      if(((WrappedID3D12Resource *)liveRes)->IsAccelerationStructureResource())
      {
        mappedBuffer = NULL;

        D3D12InitialContents initContents(D3D12InitialContents::AccelerationStructure, NULL);
        SetInitialContents(id, initContents);
      }
      else if(heapProps.Type == D3D12_HEAP_TYPE_UPLOAD || isCPUCopyHeap)
      {
        // if destination is on the upload heap, it's impossible to copy via the device,
        // so we have to CPU copy. To save time and make a more optimal copy, we just keep the data
        // CPU-side
        mappedBuffer = NULL;

        D3D12InitialContents initContents(D3D12InitialContents::Copy, type);
        ResourceContents = initContents.srcData = AllocAlignedBuffer(RDCMAX(ContentsLength, 64ULL));
        initContents.resourceType = Resource_Resource;
        SetInitialContents(id, initContents);
      }
      else
      {
        // create an upload buffer to contain the contents
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc;
        desc.Alignment = 0;
        desc.DepthOrArraySize = 1;
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.Height = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Width = RDCMAX(ContentsLength, 64ULL);

        ID3D12Resource *copySrc = NULL;
        HRESULT hr = m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                       __uuidof(ID3D12Resource), (void **)&copySrc);
        m_Device->RemoveReplayResource(GetResID(copySrc));

        if(SUCCEEDED(hr))
        {
          mappedBuffer = copySrc;

          // map the upload buffer to serialise into
          hr = copySrc->Map(0, NULL, (void **)&ResourceContents);

          if(FAILED(hr))
          {
            RDCERR("Created but couldn't map upload buffer: %s", ToStr(hr).c_str());
            ret = false;
            SAFE_RELEASE(copySrc);
            mappedBuffer = NULL;
            ResourceContents = NULL;
          }
        }
        else
        {
          RDCERR("Couldn't create upload buffer: %s", ToStr(hr).c_str());
          ret = false;
          mappedBuffer = NULL;
          ResourceContents = NULL;
        }
      }

      // need to create a dummy buffer to serialise into if anything went wrong
      if(ResourceContents == NULL && ContentsLength > 0)
        ResourceContents = dummy = new byte[(size_t)ContentsLength];
    }

    // not using SERIALISE_ELEMENT_ARRAY so we can deliberately avoid allocation - we serialise
    // directly into upload memory
    ser.Serialise("ResourceContents"_lit, ResourceContents, ContentsLength, SerialiserFlags::NoFlags)
        .Important();

    if(mappedBuffer)
      mappedBuffer->Unmap(0, NULL);

    SAFE_DELETE_ARRAY(dummy);

    SERIALISE_CHECK_READ_ERRORS();

    if(IsReplayingAndReading() && mappedBuffer)
    {
      D3D12InitialContents initContents(D3D12InitialContents::Copy, type);
      initContents.resourceType = Resource_Resource;
      initContents.resource = mappedBuffer;

      initContents.sparseBinds = sparseBinds;

      initContents.subresources = subresourcesIncluded;

      D3D12_RESOURCE_DESC resDesc = liveRes->GetDesc();

      // for MSAA textures we upload to an MSAA texture here so we're ready to copy the image in
      // Apply_InitState
      if(resDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && resDesc.SampleDesc.Count > 1)
      {
        if(ContentsLength == 0)
        {
          // backwards compatibility - older captures will have no data for MSAA textures.
          initContents.resource = NULL;
          SAFE_RELEASE(mappedBuffer);
        }
        else
        {
          D3D12_HEAP_PROPERTIES heapProps = {};
          if(!m_Device->IsSparseResource(GetResID(liveRes)))
            liveRes->GetHeapProperties(&heapProps, NULL);

          ID3D12GraphicsCommandList *list = Unwrap(m_Device->GetInitialStateList());

          if(!list)
            return false;

          D3D12_RESOURCE_DESC arrayDesc = resDesc;
          arrayDesc.Alignment = 0;
          arrayDesc.DepthOrArraySize *= (UINT16)arrayDesc.SampleDesc.Count;
          arrayDesc.SampleDesc.Count = 1;
          arrayDesc.SampleDesc.Quality = 0;
          arrayDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

          bool isDepth = IsDepthFormat(resDesc.Format) ||
                         (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;

          if(isDepth)
            arrayDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

          D3D12_RESOURCE_DESC msaaDesc = resDesc;
          msaaDesc.Alignment = 0;
          msaaDesc.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                   : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

          ID3D12Resource *arrayTex = NULL;
          HRESULT hr = m_Device->CreateCommittedResource(
              &heapProps, D3D12_HEAP_FLAG_NONE, &arrayDesc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
              __uuidof(ID3D12Resource), (void **)&arrayTex);
          if(FAILED(hr))
          {
            RDCERR("Couldn't create temporary array texture: %s", ToStr(hr).c_str());
            ret = false;
          }

          ID3D12Resource *msaaTex = NULL;
          hr = m_Device->CreateCommittedResource(
              &heapProps, D3D12_HEAP_FLAG_NONE, &msaaDesc,
              isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
              __uuidof(ID3D12Resource), (void **)&msaaTex);
          RDCASSERTEQUAL(hr, S_OK);
          if(FAILED(hr))
          {
            RDCERR("Couldn't create init state MSAA texture: %s", ToStr(hr).c_str());
            ret = false;
          }

          // copy buffer to array texture
          if(arrayTex)
          {
            uint32_t numSubresources = arrayDesc.DepthOrArraySize;

            {
              D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = {};
              formatInfo.Format = arrayDesc.Format;
              m_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo,
                                            sizeof(formatInfo));

              UINT planes = RDCMAX((UINT8)1, formatInfo.PlaneCount);

              numSubresources *= planes;
            }

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts =
                new D3D12_PLACED_SUBRESOURCE_FOOTPRINT[numSubresources];

            m_Device->GetCopyableFootprints(&arrayDesc, 0, numSubresources, 0, layouts, NULL, NULL,
                                            NULL);

            for(UINT i = 0; i < numSubresources; i++)
            {
              D3D12_TEXTURE_COPY_LOCATION dst, src;

              dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              dst.pResource = Unwrap(arrayTex);
              dst.SubresourceIndex = i;

              src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
              src.pResource = Unwrap(mappedBuffer);
              src.PlacedFootprint = layouts[i];

              // copy buffer into this array slice
              list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

              // this slice now needs to be in shader-read to copy to the MSAA texture
              D3D12_RESOURCE_BARRIER b = {};
              b.Transition.pResource = Unwrap(arrayTex);
              b.Transition.Subresource = i;
              b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
              b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
              list->ResourceBarrier(1, &b);
            }

            delete[] layouts;
          }

          m_Device->CloseInitialStateList();
          m_Device->ExecuteLists(NULL, true);
          m_Device->FlushLists(true);

          // compact array into MSAA texture
          if(msaaTex && arrayTex)
            m_Device->GetDebugManager()->CopyArrayToTex2DMS(msaaTex, arrayTex, ~0U);

          // move MSAA texture permanently to copy source state
          if(msaaTex)
          {
            list = Unwrap(m_Device->GetInitialStateList());

            if(!list)
              return false;

            D3D12_RESOURCE_BARRIER b = {};
            b.Transition.pResource = Unwrap(msaaTex);
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore =
                isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);

            m_Device->CloseInitialStateList();
            m_Device->ExecuteLists(NULL, true);
            m_Device->FlushLists(true);
          }

          // subsequent copy comes from msaa texture
          initContents.resource = msaaTex;

          // we can release the buffer now, and the temporary array texture
          SAFE_RELEASE(mappedBuffer);
          SAFE_RELEASE(arrayTex);
        }
      }

      if(initContents.resource)
        SetInitialContents(id, initContents);
    }
  }
  else if(type == Resource_AccelerationStructure)
  {
    SERIALISE_ELEMENT_LOCAL(opaqueBlob, D3D12_Debug_DriverASSerialisation());

    if(opaqueBlob)
    {
      byte *ResourceContents = NULL;
      uint64_t ContentsLength = 0;
      byte *dummy = NULL;
      ID3D12Resource *mappedBuffer = NULL;

      if(ser.IsWriting())
      {
        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists();

        RDCASSERT(initial);

        mappedBuffer = (ID3D12Resource *)initial->resource;

        HRESULT hr = mappedBuffer->Map(0, NULL, (void **)&ResourceContents);
        ContentsLength = mappedBuffer->GetDesc().Width;

        if(FAILED(hr) || ResourceContents == NULL)
        {
          ContentsLength = 0;
          ResourceContents = NULL;
          mappedBuffer = NULL;

          RDCERR("Failed to map buffer for readback! %s", ToStr(hr).c_str());
          ret = false;
        }
      }

      // serialise the size separately so we can recreate on replay
      SERIALISE_ELEMENT(ContentsLength);

      // only map on replay if we haven't encountered any errors so far
      if(IsReplayingAndReading() && !ser.IsErrored())
      {
        // create an upload buffer to contain the contents
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc;
        desc.Alignment = 0;
        desc.DepthOrArraySize = 1;
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.Height = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Width = RDCMAX(ContentsLength, 64ULL);

        ID3D12Resource *copySrc = NULL;
        HRESULT hr = m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                       __uuidof(ID3D12Resource), (void **)&copySrc);

        if(SUCCEEDED(hr))
        {
          mappedBuffer = copySrc;

          // map the upload buffer to serialise into
          hr = copySrc->Map(0, NULL, (void **)&ResourceContents);

          if(FAILED(hr))
          {
            RDCERR("Created but couldn't map upload buffer: %s", ToStr(hr).c_str());
            ret = false;
            SAFE_RELEASE(copySrc);
            mappedBuffer = NULL;
            ResourceContents = NULL;
          }
        }
        else
        {
          RDCERR("Couldn't create upload buffer: %s", ToStr(hr).c_str());
          ret = false;
          mappedBuffer = NULL;
          ResourceContents = NULL;
        }

        // need to create a dummy buffer to serialise into if anything went wrong
        if(ResourceContents == NULL && ContentsLength > 0)
          ResourceContents = dummy = new byte[(size_t)ContentsLength];
      }

      // not using SERIALISE_ELEMENT_ARRAY so we can deliberately avoid allocation - we serialise
      // directly into upload memory
      ser.Serialise("ResourceContents"_lit, ResourceContents, ContentsLength, SerialiserFlags::NoFlags)
          .Important();

      if(mappedBuffer)
      {
        if(IsReplayingAndReading())
        {
          // this is highly inefficient, but temporary. Read-back and patch the addresses of any BLASs
          D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header;
          memcpy(&header, ResourceContents, sizeof(header));

          D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS status =
              m_Device->GetReal5()->CheckDriverMatchingIdentifier(
                  D3D12_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE,
                  &header.DriverMatchingIdentifier);
          if(status != D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE)
          {
            RDResult err;
            SET_ERROR_RESULT(err, ResultCode::APIHardwareUnsupported,
                             "Serialised AS is not compatible with current device");
            m_Device->ReportFatalError(err);
            return false;
          }

          UINT64 numBLAS = header.NumBottomLevelAccelerationStructurePointersAfterHeader;
          D3D12_GPU_VIRTUAL_ADDRESS *blasAddrs =
              (D3D12_GPU_VIRTUAL_ADDRESS *)(ResourceContents + sizeof(header));
          for(UINT64 i = 0; i < numBLAS; i++)
          {
            // silently ignore NULL BLASs
            if(blasAddrs[i] == 0)
              continue;

            ResourceId asbId;
            UINT64 offsInASB;
            m_Device->GetResIDFromOrigAddr(blasAddrs[i], asbId, offsInASB);

            ID3D12Resource *asb = GetLiveAs<ID3D12Resource>(asbId);

            if(asbId == ResourceId() || asb == NULL)
            {
              RDCWARN("BLAS referenced by TLAS is not available on replay - possibly stale TLAS");
              blasAddrs[i] = 0;
              continue;
            }

            blasAddrs[i] = asb->GetGPUVirtualAddress() + offsInASB;
          }
        }

        mappedBuffer->Unmap(0, NULL);
      }

      SAFE_DELETE_ARRAY(dummy);

      SERIALISE_CHECK_READ_ERRORS();

      if(IsReplayingAndReading() && mappedBuffer)
      {
        D3D12InitialContents initContents(D3D12InitialContents::AccelerationStructure, mappedBuffer);
        initContents.resourceType = Resource_AccelerationStructure;

        if(initContents.resource)
          SetInitialContents(id, initContents);
      }
    }
    else
    {
      ASBuildData *buildData = NULL;
      byte *BufferContents = NULL;
      uint64_t ContentsLength = 0;
      byte *tempAlloc = NULL;
      D3D12GpuBuffer *mappedBuffer = NULL;

      if(ser.IsWriting())
      {
        m_Device->ExecuteLists(NULL, true);
        m_Device->FlushLists();

        RDCASSERT(initial && initial->buildData);

        mappedBuffer = initial->buildData->buffer;

        if(mappedBuffer)
        {
          BufferContents = (byte *)mappedBuffer->Map();
          ContentsLength = mappedBuffer->Size();

          if(BufferContents == NULL)
          {
            ContentsLength = 0;
            BufferContents = NULL;
            mappedBuffer = NULL;

            RDCERR("Failed to map builddata buffer for readback!");
            ret = false;
          }
        }
        else if(initial->buildData->diskCache.Valid())
        {
          ContentsLength = initial->buildData->diskCache.size;
        }

        buildData = initial->buildData;
      }
      else
      {
        buildData = new ASBuildData;
      }

      SERIALISE_ELEMENT(buildData->Type);
      SERIALISE_ELEMENT(buildData->Flags);
      SERIALISE_ELEMENT(buildData->NumBLAS);
      SERIALISE_ELEMENT(buildData->geoms);

      // serialise the size separately so we can recreate on replay
      SERIALISE_ELEMENT(ContentsLength);

      // only map on replay if we haven't encountered any errors so far
      if(IsReplayingAndReading() && !ser.IsErrored() && ContentsLength > 0)
      {
        m_GPUBufferAllocator.Alloc(D3D12GpuBufferHeapType::UploadHeap,
                                   D3D12GpuBufferHeapMemoryFlag::Default, ContentsLength, 256,
                                   &buildData->buffer);

        if(buildData->buffer)
        {
          mappedBuffer = buildData->buffer;

          // for BLASs, map the upload buffer to serialise into directly.
          // for TLASs, put it into temporary memory so that we can patch the BLAS addresses in CPU
          // memory before upload. We expect TLASs to not be much memory - each BLAS instance is 64
          // bytes so even 100k BLASs in a TLAS is only ~6MB
          if(buildData->NumBLAS == 0)
            BufferContents = (byte *)buildData->buffer->Map();
          else
            BufferContents = tempAlloc = new byte[(size_t)ContentsLength];

          if(!BufferContents)
          {
            RDCERR("Created but couldn't map upload AS data buffer");
            ret = false;
            SAFE_RELEASE(buildData->buffer);
            SAFE_RELEASE(buildData);
            BufferContents = NULL;
          }
        }
        else
        {
          RDCERR("Couldn't create upload AS data buffer");
          ret = false;
          BufferContents = NULL;
          SAFE_RELEASE(buildData);
        }

        // need to create a dummy buffer to serialise into if anything went wrong
        if(BufferContents == NULL && ContentsLength > 0)
          BufferContents = tempAlloc = new byte[(size_t)ContentsLength];
      }

      if(buildData->diskCache.Valid() && ser.IsWriting())
      {
        GetRTManager()->ReadDiskCache(ser, "BufferContents"_lit, buildData->diskCache);
      }
      else
      {
        // not using SERIALISE_ELEMENT_ARRAY so we can deliberately avoid allocation - we serialise
        // directly into already allocated memory (either directly upload memory for BLAS, or
        // temporary memory to patch for TLASs)
        ser.Serialise("BufferContents"_lit, BufferContents, ContentsLength, SerialiserFlags::NoFlags)
            .Important();
      }

      if(buildData)
      {
        if(IsReplayingAndReading())
        {
          D3D12AccelerationStructure *as = (D3D12AccelerationStructure *)GetLiveResource(id);

          // if this is a TLAS, patch the addresses of any BLASs in the instance data before uploading it
          if(buildData->NumBLAS > 0)
          {
            D3D12_RAYTRACING_INSTANCE_DESC *instances =
                (D3D12_RAYTRACING_INSTANCE_DESC *)BufferContents;
            for(UINT64 i = 0; i < buildData->NumBLAS; i++)
            {
              // silently ignore NULL BLASs
              if(instances[i].AccelerationStructure == 0)
                continue;

              ResourceId blasId;
              UINT64 blasOffs;
              m_Device->GetResIDFromOrigAddr(instances[i].AccelerationStructure, blasId, blasOffs);

              WrappedID3D12Resource *blasASB = GetLiveAs<WrappedID3D12Resource>(blasId);

              D3D12AccelerationStructure *blasCheck = NULL;

              // check and log more fine-grained if we're auditing
              if(D3D12_Debug_RT_Auditing())
              {
                rdcstr invalid;

                if(blasId == ResourceId() || blasASB == NULL)
                  invalid = StringFormat::Fmt("Address references non-existant buffer");
                else if(!blasASB->GetAccStructIfExist(blasOffs, &blasCheck))
                  invalid = StringFormat::Fmt("No valid AS created at buffer location");
                else if(blasCheck->Type() == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
                  invalid = StringFormat::Fmt("TLAS referenced, assuming overwritten");

                if(!invalid.empty())
                {
                  RDCWARN("%s %u: %s", ToStr(id).c_str(), i, invalid.c_str());
                  instances[i].AccelerationStructure = 0;
                  continue;
                }
              }

              if(blasId == ResourceId() || blasASB == NULL ||
                 !blasASB->GetAccStructIfExist(blasOffs, &blasCheck))
              {
                RDCWARN(
                    "  %u: BLAS referenced by TLAS is not available on replay - possibly stale "
                    "TLAS",
                    i);
                instances[i].AccelerationStructure = 0;
                continue;
              }

              if(id < GetOriginalID(blasCheck->GetResourceID()))
              {
                RDCWARN("  %u: BLAS referenced by TLAS is newer than TLAS - possibly stale TLAS", i);
                instances[i].AccelerationStructure = 0;
                continue;
              }

              if(blasCheck->Type() != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
              {
                RDCWARN("  %u: BLAS is not of correct type - possibly stale TLAS", i);
                instances[i].AccelerationStructure = 0;
                continue;
              }

              if(D3D12_Debug_RT_Auditing())
              {
                RDCLOG("%s %u: remapped from %llx to %llx", ToStr(id).c_str(), i,
                       instances[i].AccelerationStructure,
                       blasASB->GetGPUVirtualAddress() + blasOffs);

                as->children.push_back(blasCheck);
              }

              RDCASSERTEQUAL(blasCheck->GetVirtualAddress(),
                             blasASB->GetGPUVirtualAddress() + blasOffs);

              instances[i].AccelerationStructure = blasASB->GetGPUVirtualAddress() + blasOffs;
            }

            void *upload = mappedBuffer->Map();
            if(upload)
            {
              memcpy(upload, BufferContents, (size_t)ContentsLength);
            }
            else
            {
              RDCERR("Created but couldn't map upload AS data buffer");
              ret = false;
              SAFE_RELEASE(mappedBuffer);
              SAFE_RELEASE(buildData);
            }
          }

          // rebase all the geometries to the new address
          uint64_t baseVA = mappedBuffer ? mappedBuffer->Address() : 0;
          for(ASBuildData::RTGeometryDesc &desc : buildData->geoms)
          {
            if(desc.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
            {
              if(desc.AABBs.AABBCount != 0)
                desc.AABBs.AABBs.RVA += baseVA;
              else
                desc.AABBs.AABBs.RVA = 0;
            }
            else
            {
              if(desc.Triangles.Transform3x4 != ASBuildData::NULLVA)
                desc.Triangles.Transform3x4 += baseVA;
              else
                desc.Triangles.Transform3x4 = 0;

              if(desc.Triangles.IndexBuffer != ASBuildData::NULLVA)
                desc.Triangles.IndexBuffer += baseVA;
              else
                desc.Triangles.IndexBuffer = 0;

              // VB is always present, no need for NULL check
              desc.Triangles.VertexBuffer.RVA += baseVA;
            }
          }
        }

        if(mappedBuffer)
          mappedBuffer->Unmap();
      }

      SAFE_DELETE_ARRAY(tempAlloc);

      SERIALISE_CHECK_READ_ERRORS();

      if(IsReplayingAndReading() && buildData)
      {
        D3D12InitialContents initContents(D3D12InitialContents::AccelerationStructure, NULL);
        initContents.resourceType = Resource_AccelerationStructure;
        initContents.buildData = buildData;

        SetInitialContents(id, initContents);
      }
    }
  }
  else
  {
    RDCERR("Unexpected type needing an initial state serialised: %d", type);
    return false;
  }

  return ret;
}

template bool D3D12ResourceManager::Serialise_InitialState(ReadSerialiser &ser, ResourceId id,
                                                           D3D12ResourceRecord *record,
                                                           const D3D12InitialContents *initial);
template bool D3D12ResourceManager::Serialise_InitialState(WriteSerialiser &ser, ResourceId id,
                                                           D3D12ResourceRecord *record,
                                                           const D3D12InitialContents *initial);

void D3D12ResourceManager::Create_InitialState(ResourceId id, ID3D12DeviceChild *live, bool)
{
  D3D12ResourceType type = IdentifyTypeByPtr(live);

  if(type == Resource_DescriptorHeap)
  {
    // set a NULL heap, if there are no initial contents for a descriptor heap we just leave
    // it all entirely undefined.
    SetInitialContents(id, D3D12InitialContents((ID3D12DescriptorHeap *)NULL));
  }
  else if(type == Resource_Resource)
  {
    ID3D12Resource *res = ((ID3D12Resource *)live);

    WrappedID3D12Resource *wrappedResource = (WrappedID3D12Resource *)res;

    if(wrappedResource->IsAccelerationStructureResource())
    {
      SetInitialContents(id, D3D12InitialContents(D3D12InitialContents::AccelerationStructure,
                                                  (ID3D12Resource *)NULL));
      return;
    }

    D3D12_RESOURCE_DESC resDesc = res->GetDesc();

    D3D12_HEAP_PROPERTIES heapProps = {};
    if(!m_Device->IsSparseResource(GetResID(live)))
      res->GetHeapProperties(&heapProps, NULL);

    const bool isCPUCopyHeap = heapProps.Type == D3D12_HEAP_TYPE_CUSTOM &&
                               (heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
                                heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE) &&
                               heapProps.MemoryPoolPreference == D3D12_MEMORY_POOL_L0;

    if(heapProps.Type == D3D12_HEAP_TYPE_UPLOAD || isCPUCopyHeap)
    {
      // if destination is on the upload heap, it's impossible to copy via the device,
      // so we have to CPU copy. To save time and make a more optimal copy, we just keep the data
      // CPU-side
      D3D12InitialContents initContents(D3D12InitialContents::Copy, Resource_Resource);
      uint64_t size = RDCMAX(resDesc.Width, 64ULL);
      initContents.srcData = AllocAlignedBuffer(size);
      memset(initContents.srcData, 0, (size_t)size);
      SetInitialContents(id, initContents);
    }
    else
    {
      // create a GPU-local copy of the resource
      heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heapProps.CreationNodeMask = 1;
      heapProps.VisibleNodeMask = 1;

      bool isDepth = IsDepthFormat(resDesc.Format) ||
                     (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;

      resDesc.Alignment = 0;
      resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

      if(resDesc.SampleDesc.Count > 1)
      {
        if(isDepth)
          resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        else
          resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      }

      ID3D12Resource *copy = NULL;
      HRESULT hr = m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                                                     D3D12_RESOURCE_STATE_COMMON, NULL,
                                                     __uuidof(ID3D12Resource), (void **)&copy);
      if(FAILED(hr))
      {
        RDCERR("Couldn't create initial state copy: %s", ToStr(hr).c_str());
        CHECK_HR(m_Device, hr);
      }
      else
      {
        D3D12InitialContents initContents(D3D12InitialContents::ForceCopy, type);
        initContents.resourceType = Resource_Resource;
        initContents.resource = copy;

        if(m_Device->IsSparseResource(GetResID(live)))
          initContents.sparseBinds = new SparseBinds(0);

        SetInitialContents(id, initContents);
      }
    }
  }
  else if(type == Resource_AccelerationStructure)
  {
    // don't create 'default' AS contents as it's not possible. ASs must be written before being
    // used by definition
  }
  else
  {
    RDCERR("Unexpected type needing an initial state created: %d", type);
  }
}

void D3D12ResourceManager::Apply_InitialState(ID3D12DeviceChild *live, D3D12InitialContents &data)
{
  if(m_Device->HasFatalError())
    return;

  D3D12ResourceType type = (D3D12ResourceType)data.resourceType;

  if(type == Resource_DescriptorHeap)
  {
    WrappedID3D12DescriptorHeap *dstheap = (WrappedID3D12DescriptorHeap *)live;
    WrappedID3D12DescriptorHeap *srcheap = (WrappedID3D12DescriptorHeap *)data.resource;

    if(srcheap)
    {
      // copy the whole heap
      m_Device->CopyDescriptorsSimple(
          srcheap->GetNumDescriptors(), dstheap->GetCPUDescriptorHandleForHeapStart(),
          srcheap->GetCPUDescriptorHandleForHeapStart(), srcheap->GetDesc().Type);
    }
  }
  else if(type == Resource_Resource)
  {
    if(data.tag == D3D12InitialContents::AccelerationStructure)
      return;

    ResourceId id = GetResID(live);

    if(IsActiveReplaying(m_State) && m_Device->IsReadOnlyResource(id))
    {
    }
    else if(data.tag == D3D12InitialContents::Copy || data.tag == D3D12InitialContents::ForceCopy)
    {
      ID3D12Resource *copyDst = (ID3D12Resource *)live;

      if(!copyDst)
      {
        RDCERR("Missing copy destination in initial state apply (%p)", copyDst);
        return;
      }

      D3D12_HEAP_PROPERTIES heapProps = {};
      if(data.sparseBinds)
      {
        if(IsLoading(m_State) || m_Device->GetQueue()->IsSparseUpdatedResource(GetResID(live)))
          data.sparseBinds->Apply(m_Device, (ID3D12Resource *)live);

        if(m_Device->HasFatalError())
          return;
      }
      else
      {
        copyDst->GetHeapProperties(&heapProps, NULL);
      }

      const bool isCPUCopyHeap =
          heapProps.Type == D3D12_HEAP_TYPE_CUSTOM &&
          (heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
           heapProps.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE) &&
          heapProps.MemoryPoolPreference == D3D12_MEMORY_POOL_L0;

      // if destination is on the upload heap, it's impossible to copy via the device,
      // so we have to CPU copy. We assume that we detected this case above and never uploaded a
      // device copy in the first place, and just kept the data CPU-side to source from.
      if(heapProps.Type == D3D12_HEAP_TYPE_UPLOAD || isCPUCopyHeap)
      {
        byte *src = data.srcData, *dst = NULL;

        if(!src)
        {
          RDCERR("Doing CPU-side copy, don't have source data");
          return;
        }

        HRESULT hr = S_OK;

        D3D12_RESOURCE_DESC desc = copyDst->GetDesc();

        if(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
          hr = Unwrap(copyDst)->Map(0, NULL, (void **)&dst);
          CHECK_HR(m_Device, hr);

          if(FAILED(hr))
          {
            RDCERR("Doing CPU-side copy, couldn't map destination: HRESULT: %s", ToStr(hr).c_str());
            dst = NULL;
          }

          if(src && dst)
            memcpy(dst, src, (size_t)copyDst->GetDesc().Width);

          if(dst)
            Unwrap(copyDst)->Unmap(0, NULL);
        }
        else
        {
          UINT numSubresources = desc.MipLevels;
          if(desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            numSubresources *= desc.DepthOrArraySize;

          D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts =
              new D3D12_PLACED_SUBRESOURCE_FOOTPRINT[numSubresources];
          UINT *numrows = new UINT[numSubresources];
          UINT64 *rowsizes = new UINT64[numSubresources];

          m_Device->GetCopyableFootprints(&desc, 0, numSubresources, 0, layouts, numrows, rowsizes,
                                          NULL);

          for(UINT i = 0; i < numSubresources; i++)
          {
            if(desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN)
              hr = Unwrap(copyDst)->Map(i, NULL, NULL);
            else
              hr = Unwrap(copyDst)->Map(i, NULL, (void **)&dst);
            CHECK_HR(m_Device, hr);

            if(FAILED(hr))
            {
              RDCERR("Doing CPU-side copy, couldn't map source: HRESULT: %s", ToStr(hr).c_str());
              dst = NULL;
            }

            if(src && dst)
            {
              byte *bufPtr = src + layouts[i].Offset;
              byte *texPtr = dst;

              D3D12_BOX box = {};

              box.right = layouts[i].Footprint.Width;
              box.back = 1;

              for(UINT d = 0; d < layouts[i].Footprint.Depth; d++)
              {
                box.top = 0;
                box.bottom = 1;
                for(UINT r = 0; r < numrows[i]; r++)
                {
                  if(texPtr)
                    memcpy(bufPtr, texPtr, (size_t)rowsizes[i]);
                  else
                    copyDst->WriteToSubresource(i, &box, bufPtr, (UINT)rowsizes[i],
                                                (UINT)rowsizes[i]);

                  bufPtr += layouts[i].Footprint.RowPitch;
                  if(texPtr)
                    texPtr += rowsizes[i];

                  box.top++;
                  box.bottom++;
                }

                box.front++;
                box.back++;
              }
            }

            if(dst)
              Unwrap(copyDst)->Unmap(i, NULL);
          }

          delete[] layouts;
          delete[] numrows;
          delete[] rowsizes;
        }
      }
      else
      {
        ID3D12Resource *copySrc = (ID3D12Resource *)data.resource;

        if(!copySrc)
        {
          RDCERR("Missing copy source in initial state apply (%p)", copySrc);
          return;
        }

        ID3D12GraphicsCommandListX *list = m_Device->GetInitialStateList();

        if(!list)
          return;

        BarrierSet barriers;

        barriers.Configure(copyDst, m_Device->GetSubresourceStates(GetResID(live)),
                           BarrierSet::CopyDestAccess);
        barriers.Apply(list);

        if(copyDst->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
          D3D12_RESOURCE_DESC srcDesc = copySrc->GetDesc();
          D3D12_RESOURCE_DESC dstDesc = copyDst->GetDesc();

          list->CopyBufferRegion(copyDst, 0, copySrc, 0, RDCMIN(srcDesc.Width, dstDesc.Width));
        }
        else if(copyDst->GetDesc().SampleDesc.Count > 1 || data.tag == D3D12InitialContents::ForceCopy)
        {
          // MSAA texture was pre-uploaded and decoded, just copy the texture.
          // Similarly for created initial states
          list->CopyResource(copyDst, copySrc);
        }
        else
        {
          D3D12_RESOURCE_DESC desc = copyDst->GetDesc();

          UINT numSubresources = desc.MipLevels;
          if(desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            numSubresources *= desc.DepthOrArraySize;

          // we only accounted for planes in version 0x6, before then we only copied the first plane
          // so the buffer won't have enough data
          if(m_Device->GetCaptureVersion() >= 0x6)
          {
            D3D12_FEATURE_DATA_FORMAT_INFO formatInfo = {};
            formatInfo.Format = desc.Format;
            m_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &formatInfo, sizeof(formatInfo));

            UINT planes = RDCMAX((UINT8)1, formatInfo.PlaneCount);

            numSubresources *= planes;
          }

          const uint32_t *nextIncludedSubresource = data.subresources.begin();
          // if no subresources were serialised, just skip!
          if(data.subresources.empty())
            numSubresources = 0;
          // if ALL subresources were serialised, serialise them all
          else if(*nextIncludedSubresource == ~0U)
            nextIncludedSubresource = NULL;

          UINT64 offset = 0;
          UINT64 subSize = 0;

          if(IsBlockFormat(desc.Format) && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
          {
            RDCDEBUG("Removing UAV flag from BCn desc to allow GetCopyableFootprints");
            desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
          }

          for(UINT i = 0; i < numSubresources; i++)
          {
            // if we have a list of subresources included, only copy those
            if(nextIncludedSubresource && *nextIncludedSubresource != i)
              continue;

            D3D12_TEXTURE_COPY_LOCATION dst, src;

            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = copyDst;
            dst.SubresourceIndex = i;

            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.pResource = copySrc;

            m_Device->GetCopyableFootprints(&desc, i, 1, offset, &src.PlacedFootprint, NULL, NULL,
                                            &subSize);

            if(subSize == ~0ULL)
            {
              RDCERR("Failed to call GetCopyableFootprints on %s! skipping copy", ToStr(id).c_str());
              continue;
            }

            list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

            offset += subSize;
            offset = AlignUp<UINT64>(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

            if(nextIncludedSubresource)
            {
              nextIncludedSubresource++;
              // no more subresource after this one were included, even if they exist
              if(nextIncludedSubresource >= data.subresources.end())
                break;
            }
          }
        }

        barriers.Unapply(list);

        if(D3D12_Debug_SingleSubmitFlushing())
        {
          m_Device->CloseInitialStateList();
          m_Device->ExecuteLists(NULL, true);
          m_Device->FlushLists(true);
        }
      }
    }
    else
    {
      RDCERR("Unexpected tag: %u", data.tag);
    }
  }
  else if(type == Resource_AccelerationStructure)
  {
    D3D12AccelerationStructure *as = (D3D12AccelerationStructure *)live;

    if(!as)
    {
      RDCERR("Missing AS in initial state apply");
      return;
    }

    ASBuildData *buildData = data.buildData;

    if(buildData)
    {
      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};

      // if we've already cached this AS don't bother doing any work to determine a rebuild
      if(data.cachedBuiltAS == NULL)
      {
        desc.Inputs.Type = buildData->Type;
        desc.Inputs.Flags = buildData->Flags;
        desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        // we're not updating
        desc.Inputs.Flags &= ~D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        if(buildData->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
        {
          desc.Inputs.NumDescs = buildData->NumBLAS;
          desc.Inputs.InstanceDescs = buildData->buffer ? buildData->buffer->Address() : 0;
        }
        else
        {
          desc.Inputs.NumDescs = buildData->geoms.count();
          // can be safely cast as the RVAs have been rebased to real VAs on serialise
          desc.Inputs.pGeometryDescs = (D3D12_RAYTRACING_GEOMETRY_DESC *)buildData->geoms.data();
        }

        m_Device->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &prebuild);

        if(!GetRTManager()->ASSerialiseBuffer ||
           prebuild.ScratchDataSizeInBytes > GetRTManager()->ASSerialiseBuffer->Size())
        {
          if(GetRTManager()->ASSerialiseBuffer)
          {
            // synchronise the GPU to ensure any previous work is done before resizing
            m_Device->GetInitialStateList();
            m_Device->CloseInitialStateList();
            m_Device->ExecuteLists(NULL, true);
            m_Device->FlushLists(true);
          }

          // discourage resizes by claiming at least 4MB
          GetRTManager()->ResizeSerialisationBuffer(
              RDCMAX(4 * 1024 * 1024ULL, prebuild.ScratchDataSizeInBytes));
        }

        if(m_Device->HasFatalError() || GetRTManager()->ASSerialiseBuffer == NULL)
          return;

        desc.ScratchAccelerationStructureData = GetRTManager()->ASSerialiseBuffer->Address();
      }

      ID3D12GraphicsCommandListX *list = m_Device->GetInitialStateList();

      if(!list)
        return;

      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;

      // don't cache TLASs, rebuild every time
      if(buildData->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
      {
        desc.DestAccelerationStructureData = as->GetVirtualAddress();

        UINT numPostBuilds = 0;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postDesc = {};
        if(GetRTManager()->PostbuildReadbackBuffer)
        {
          postDesc.DestBuffer = GetRTManager()->PostbuildReadbackBuffer->Address();
          postDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
          numPostBuilds++;
        }

        list->BuildRaytracingAccelerationStructure(&desc, numPostBuilds, &postDesc);

        if(D3D12_Debug_RT_Auditing())
        {
          RDCLOG("Apply TLAS - Rebuilding %s to %llx",
                 ToStr(GetOriginalID(as->GetResourceID())).c_str(),
                 desc.DestAccelerationStructureData);

          // verify that all children we intended to reference have now been built.
          for(size_t i = 0; i < as->children.size(); i++)
          {
            if(!as->children[i]->seenReplayBuild)
            {
              RDCERR("TLAS child %u did not get built with initial contents");
            }
          }

          if(GetRTManager()->PostbuildReadbackBuffer)
          {
            m_Device->CloseInitialStateList();
            m_Device->ExecuteLists(NULL, true);
            m_Device->FlushLists(true);

            uint64_t *curSize = (uint64_t *)GetRTManager()->PostbuildReadbackBuffer->Map();

            if(*curSize > as->Size())
            {
              RDCERR("BLAS built larger than recorded size - overlap checks will be incorrect");
            }

            GetRTManager()->PostbuildReadbackBuffer->Unmap();

            list = m_Device->GetInitialStateList();
          }
        }
      }
      // if we haven't cached it, build and cache the AS then copy into place
      else if(data.cachedBuiltAS == NULL)
      {
        m_GPUBufferAllocator.Alloc(D3D12GpuBufferHeapType::AccStructDefaultHeap,
                                   D3D12GpuBufferHeapMemoryFlag::Default,
                                   prebuild.ResultDataMaxSizeInBytes, 256, &data.cachedBuiltAS);

        if(!data.cachedBuiltAS)
          return;

        ResourceId origId = GetOriginalID(as->GetResourceID());

        UINT numPostBuilds = 0;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postDesc[2] = {};
        if(GetRTManager()->PostbuildReadbackBuffer)
        {
          postDesc[0].DestBuffer = GetRTManager()->PostbuildReadbackBuffer->Address();
          postDesc[0].InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
          numPostBuilds++;
          postDesc[1].DestBuffer = GetRTManager()->PostbuildReadbackBuffer->Address() + 8;
          postDesc[1].InfoType =
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
          numPostBuilds++;
        }

        desc.DestAccelerationStructureData = data.cachedBuiltAS->Address();
        list->BuildRaytracingAccelerationStructure(&desc, numPostBuilds, postDesc);

        list->ResourceBarrier(1, &barrier);

        // copy to the real location
        list->CopyRaytracingAccelerationStructure(
            as->GetVirtualAddress(), desc.DestAccelerationStructureData,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);

        if(D3D12_Debug_RT_Auditing())
        {
          RDCLOG("Apply BLAS - Caching %s to %llx then copying to %llx", ToStr(origId).c_str(),
                 desc.DestAccelerationStructureData, as->GetVirtualAddress());

          if(GetRTManager()->PostbuildReadbackBuffer)
          {
            m_Device->CloseInitialStateList();
            m_Device->ExecuteLists(NULL, true);
            m_Device->FlushLists(true);

            uint64_t *curSize = (uint64_t *)GetRTManager()->PostbuildReadbackBuffer->Map();

            if(*curSize > as->Size())
            {
              RDCERR(
                  "BLAS built is %llu which is larger than recorded size %llu (compacted size is "
                  "%llu) - overlap checks will be incorrect",
                  curSize[0], as->Size(), curSize[1]);
            }

            GetRTManager()->PostbuildReadbackBuffer->Unmap();

            list = m_Device->GetInitialStateList();
          }
        }

        as->seenReplayBuild = true;
      }
      // if we have a cached AS, just copy from it
      else
      {
        // in future we might want to keep this to reference the geometry data, for now we can
        // release this since we know the GPU has been synced since last time we applied initial
        // contents and did the actual build
        SAFE_RELEASE(data.buildData->buffer);

        list->CopyRaytracingAccelerationStructure(
            as->GetVirtualAddress(), data.cachedBuiltAS->Address(),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);

        if(D3D12_Debug_RT_Auditing())
        {
          RDCLOG("Apply BLAS - Copying %s from %llx to %llx",
                 ToStr(GetOriginalID(as->GetResourceID())).c_str(), data.cachedBuiltAS->Address(),
                 as->GetVirtualAddress());
        }
      }

      list->ResourceBarrier(1, &barrier);
    }
    else
    {
      ID3D12Resource *copySrc = (ID3D12Resource *)data.resource;

      if(!copySrc)
      {
        RDCERR("Missing copy source in initial state apply");
        return;
      }

      ID3D12GraphicsCommandListX *list = m_Device->GetInitialStateList();

      if(!list)
        return;

      Unwrap4(list)->CopyRaytracingAccelerationStructure(
          as->GetVirtualAddress(), copySrc->GetGPUVirtualAddress(),
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
    }

    if(D3D12_Debug_SingleSubmitFlushing())
    {
      m_Device->CloseInitialStateList();
      m_Device->ExecuteLists(NULL, true);
      m_Device->FlushLists(true);
    }
  }
  else
  {
    RDCERR("Unexpected type needing an initial state created: %d", type);
  }
}
