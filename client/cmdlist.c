/* Command list recording. Everything except Close is queued without a reply. */
#include <string.h>

#include "shim.h"

typedef ID3D12GraphicsCommandList10 CL;

ID3D12GraphicsCommandList10Vtbl cmdlist_vtbl;

static void begin(uint32_t op, CL *This)
{
   msg_begin(op);
   w_obj(This);
}

static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE cl_GetType(CL *This)
{
   return ((struct cmdlist *)This)->type;
}

static HRESULT STDMETHODCALLTYPE cl_Close(CL *This)
{
   /* The server reports failures; waiting for them costs a round trip per submit. */
   begin(OP_CL_CLOSE, This);
   msg_send();
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE cl_Reset(CL *This, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso)
{
   begin(OP_CL_RESET, This);
   w_obj(alloc);
   w_obj(pso);
   msg_send();
   return S_OK;
}

static void STDMETHODCALLTYPE cl_ClearState(CL *This, ID3D12PipelineState *pso)
{
   begin(OP_CL_CLEAR_STATE, This);
   w_obj(pso);
   msg_send();
}

static void STDMETHODCALLTYPE cl_DrawInstanced(CL *This, UINT count, UINT instances, UINT start, UINT start_instance)
{
   begin(OP_CL_DRAW, This);
   w_u32(count);
   w_u32(instances);
   w_u32(start);
   w_u32(start_instance);
   msg_send();
}

static void STDMETHODCALLTYPE cl_DrawIndexedInstanced(CL *This, UINT count, UINT instances, UINT start, INT base, UINT start_instance)
{
   begin(OP_CL_DRAW_INDEXED, This);
   w_u32(count);
   w_u32(instances);
   w_u32(start);
   w_u32(base);
   w_u32(start_instance);
   msg_send();
}

static void STDMETHODCALLTYPE cl_Dispatch(CL *This, UINT x, UINT y, UINT z)
{
   begin(OP_CL_DISPATCH, This);
   w_u32(x);
   w_u32(y);
   w_u32(z);
   msg_send();
}

static void STDMETHODCALLTYPE cl_CopyBufferRegion(CL *This, ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT64 size)
{
   begin(OP_CL_COPY_BUFFER, This);
   w_obj(dst);
   w_obj(src);
   w_u64(dst_offset);
   w_u64(src_offset);
   w_u64(size);
   msg_send();
}

static void w_copy_location(const D3D12_TEXTURE_COPY_LOCATION *l)
{
   w_obj(l->pResource);
   w_u32(l->Type);
   if (l->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      w_u64(l->PlacedFootprint.Offset);
      w_raw(&l->PlacedFootprint.Footprint, sizeof(l->PlacedFootprint.Footprint), 4);
   } else {
      w_u32(l->SubresourceIndex);
   }
}

static void w_optional(const void *p, size_t size)
{
   w_u32(p != NULL);
   if (p)
      w_raw(p, size, 4);
}

static void STDMETHODCALLTYPE cl_CopyTextureRegion(CL *This, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
                                                   const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
   begin(OP_CL_COPY_TEXTURE, This);
   w_copy_location(dst);
   w_u32(x);
   w_u32(y);
   w_u32(z);
   w_copy_location(src);
   w_optional(box, sizeof(*box));
   msg_send();
}

static void STDMETHODCALLTYPE cl_CopyResource(CL *This, ID3D12Resource *dst, ID3D12Resource *src)
{
   begin(OP_CL_COPY_RESOURCE, This);
   w_obj(dst);
   w_obj(src);
   msg_send();
}

static void STDMETHODCALLTYPE cl_ResolveSubresource(CL *This, ID3D12Resource *dst, UINT dst_sub, ID3D12Resource *src, UINT src_sub, DXGI_FORMAT format)
{
   begin(OP_CL_RESOLVE, This);
   w_obj(dst);
   w_u32(dst_sub);
   w_obj(src);
   w_u32(src_sub);
   w_u32(format);
   msg_send();
}

static void STDMETHODCALLTYPE cl_ResolveSubresourceRegion(CL *This, ID3D12Resource *dst, UINT dst_sub, UINT x, UINT y, ID3D12Resource *src,
                                                          UINT src_sub, D3D12_RECT *rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode)
{
   begin(OP_CL_RESOLVE_REGION, This);
   w_obj(dst);
   w_u32(dst_sub);
   w_u32(x);
   w_u32(y);
   w_obj(src);
   w_u32(src_sub);
   w_optional(rect, sizeof(*rect));
   w_u32(format);
   w_u32(mode);
   msg_send();
}

static void STDMETHODCALLTYPE cl_IASetPrimitiveTopology(CL *This, D3D12_PRIMITIVE_TOPOLOGY topology)
{
   begin(OP_CL_TOPOLOGY, This);
   w_u32(topology);
   msg_send();
}

static void STDMETHODCALLTYPE cl_RSSetViewports(CL *This, UINT n, const D3D12_VIEWPORT *vps)
{
   begin(OP_CL_VIEWPORTS, This);
   w_u32(n);
   w_raw(vps, n * sizeof(*vps), 4);
   msg_send();
}

static void STDMETHODCALLTYPE cl_RSSetScissorRects(CL *This, UINT n, const D3D12_RECT *rects)
{
   begin(OP_CL_SCISSORS, This);
   w_u32(n);
   w_raw(rects, n * sizeof(*rects), 4);
   msg_send();
}

static void STDMETHODCALLTYPE cl_OMSetBlendFactor(CL *This, const FLOAT factor[4])
{
   static const FLOAT ones[4] = {1, 1, 1, 1};
   begin(OP_CL_BLEND_FACTOR, This);
   w_raw(factor ? factor : ones, 4 * sizeof(FLOAT), 4);
   msg_send();
}

static void STDMETHODCALLTYPE cl_OMSetStencilRef(CL *This, UINT ref)
{
   begin(OP_CL_STENCIL_REF, This);
   w_u32(ref);
   msg_send();
}

static void STDMETHODCALLTYPE cl_OMSetFrontAndBackStencilRef(CL *This, UINT front, UINT back)
{
   begin(OP_CL_STENCIL_REF2, This);
   w_u32(front);
   w_u32(back);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetPipelineState(CL *This, ID3D12PipelineState *pso)
{
   begin(OP_CL_SET_PSO, This);
   w_obj(pso);
   msg_send();
}

static void STDMETHODCALLTYPE cl_ResourceBarrier(CL *This, UINT n, const D3D12_RESOURCE_BARRIER *barriers)
{
   begin(OP_CL_RESOURCE_BARRIER, This);
   w_u32(n);
   for (UINT i = 0; i < n; i++) {
      const D3D12_RESOURCE_BARRIER *b = &barriers[i];
      w_u32(b->Type);
      w_u32(b->Flags);
      switch (b->Type) {
      case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
         w_obj(b->Transition.pResource);
         w_u32(b->Transition.Subresource);
         w_u32(b->Transition.StateBefore);
         w_u32(b->Transition.StateAfter);
         break;
      case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
         w_obj(b->Aliasing.pResourceBefore);
         w_obj(b->Aliasing.pResourceAfter);
         break;
      default:
         w_obj(b->UAV.pResource);
         break;
      }
   }
   msg_send();
}

static void STDMETHODCALLTYPE cl_Barrier(CL *This, UINT32 n, const D3D12_BARRIER_GROUP *groups)
{
   begin(OP_CL_BARRIER, This);
   w_u32(n);
   for (UINT32 g = 0; g < n; g++) {
      const D3D12_BARRIER_GROUP *grp = &groups[g];
      w_u32(grp->Type);
      w_u32(grp->NumBarriers);
      for (UINT32 i = 0; i < grp->NumBarriers; i++) {
         switch (grp->Type) {
         case D3D12_BARRIER_TYPE_GLOBAL:
            w_raw(&grp->pGlobalBarriers[i], sizeof(D3D12_GLOBAL_BARRIER), 4);
            break;
         case D3D12_BARRIER_TYPE_TEXTURE: {
            const D3D12_TEXTURE_BARRIER *b = &grp->pTextureBarriers[i];
            w_raw(b, offsetof(D3D12_TEXTURE_BARRIER, pResource), 4);
            w_obj(b->pResource);
            w_raw(&b->Subresources, sizeof(b->Subresources), 4);
            w_u32(b->Flags);
            break;
         }
         case D3D12_BARRIER_TYPE_BUFFER: {
            const D3D12_BUFFER_BARRIER *b = &grp->pBufferBarriers[i];
            w_raw(b, offsetof(D3D12_BUFFER_BARRIER, pResource), 4);
            w_obj(b->pResource);
            w_u64(b->Offset);
            w_u64(b->Size);
            break;
         }
         }
      }
   }
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetDescriptorHeaps(CL *This, UINT n, ID3D12DescriptorHeap *const *heaps)
{
   begin(OP_CL_SET_DESC_HEAPS, This);
   w_u32(n);
   for (UINT i = 0; i < n; i++)
      w_obj(heaps[i]);
   msg_send();
}

static void set_root_sig(CL *This, bool compute, ID3D12RootSignature *root)
{
   begin(OP_CL_SET_ROOT_SIG, This);
   w_u32(compute);
   w_obj(root);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetComputeRootSignature(CL *This, ID3D12RootSignature *root) { set_root_sig(This, true, root); }
static void STDMETHODCALLTYPE cl_SetGraphicsRootSignature(CL *This, ID3D12RootSignature *root) { set_root_sig(This, false, root); }

static void set_root_table(CL *This, bool compute, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
   begin(OP_CL_SET_ROOT_TABLE, This);
   w_u32(compute);
   w_u32(index);
   w_u64(base.ptr);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetComputeRootDescriptorTable(CL *This, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) { set_root_table(This, true, index, base); }
static void STDMETHODCALLTYPE cl_SetGraphicsRootDescriptorTable(CL *This, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) { set_root_table(This, false, index, base); }

static void set_root_constants(CL *This, bool compute, UINT index, UINT n, const void *data, UINT offset)
{
   begin(OP_CL_SET_ROOT_CONSTANTS, This);
   w_u32(compute);
   w_u32(index);
   w_u32(offset);
   w_u32(n);
   w_raw(data, n * 4, 4);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetComputeRoot32BitConstant(CL *This, UINT index, UINT data, UINT offset) { set_root_constants(This, true, index, 1, &data, offset); }
static void STDMETHODCALLTYPE cl_SetGraphicsRoot32BitConstant(CL *This, UINT index, UINT data, UINT offset) { set_root_constants(This, false, index, 1, &data, offset); }
static void STDMETHODCALLTYPE cl_SetComputeRoot32BitConstants(CL *This, UINT index, UINT n, const void *data, UINT offset) { set_root_constants(This, true, index, n, data, offset); }
static void STDMETHODCALLTYPE cl_SetGraphicsRoot32BitConstants(CL *This, UINT index, UINT n, const void *data, UINT offset) { set_root_constants(This, false, index, n, data, offset); }

enum root_view { ROOT_CBV, ROOT_SRV, ROOT_UAV };

static void set_root_view(CL *This, bool compute, enum root_view kind, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va)
{
   begin(OP_CL_SET_ROOT_VIEW, This);
   w_u32(compute);
   w_u32(kind);
   w_u32(index);
   w_u64(va);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetComputeRootConstantBufferView(CL *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { set_root_view(This, true, ROOT_CBV, i, va); }
static void STDMETHODCALLTYPE cl_SetGraphicsRootConstantBufferView(CL *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { set_root_view(This, false, ROOT_CBV, i, va); }
static void STDMETHODCALLTYPE cl_SetComputeRootShaderResourceView(CL *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { set_root_view(This, true, ROOT_SRV, i, va); }
static void STDMETHODCALLTYPE cl_SetGraphicsRootShaderResourceView(CL *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { set_root_view(This, false, ROOT_SRV, i, va); }
static void STDMETHODCALLTYPE cl_SetComputeRootUnorderedAccessView(CL *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { set_root_view(This, true, ROOT_UAV, i, va); }
static void STDMETHODCALLTYPE cl_SetGraphicsRootUnorderedAccessView(CL *This, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { set_root_view(This, false, ROOT_UAV, i, va); }

/* Buffer views hold a u64 first and have the same layout on both sides. */
static void STDMETHODCALLTYPE cl_IASetIndexBuffer(CL *This, const D3D12_INDEX_BUFFER_VIEW *view)
{
   begin(OP_CL_INDEX_BUFFER, This);
   w_u32(view != NULL);
   if (view)
      w_raw(view, sizeof(*view), 8);
   msg_send();
}

static void STDMETHODCALLTYPE cl_IASetVertexBuffers(CL *This, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW *views)
{
   begin(OP_CL_VERTEX_BUFFERS, This);
   w_u32(start);
   w_u32(n);
   w_u32(views != NULL);
   if (views)
      w_raw(views, n * sizeof(*views), 8);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SOSetTargets(CL *This, UINT start, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views)
{
   begin(OP_CL_SO_TARGETS, This);
   w_u32(start);
   w_u32(n);
   w_u32(views != NULL);
   if (views)
      w_raw(views, n * sizeof(*views), 8);
   msg_send();
}

static void STDMETHODCALLTYPE cl_OMSetRenderTargets(CL *This, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rts, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *ds)
{
   UINT handles = rts ? (single ? (n ? 1 : 0) : n) : 0;
   begin(OP_CL_RENDER_TARGETS, This);
   w_u32(n);
   w_u32(single);
   w_u32(handles);
   for (UINT i = 0; i < handles; i++)
      w_u64(cpu_handle(rts[i]));
   w_u32(ds != NULL);
   if (ds)
      w_u64(cpu_handle(*ds));
   msg_send();
}

static void w_rects(UINT n, const D3D12_RECT *rects)
{
   w_u32(n);
   w_raw(rects, n * sizeof(*rects), 4);
}

static void STDMETHODCALLTYPE cl_ClearDepthStencilView(CL *This, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                                                       UINT n, const D3D12_RECT *rects)
{
   begin(OP_CL_CLEAR_DSV, This);
   w_u64(cpu_handle(dsv));
   w_u32(flags);
   w_f32(depth);
   w_u32(stencil);
   w_rects(n, rects);
   msg_send();
}

static void STDMETHODCALLTYPE cl_ClearRenderTargetView(CL *This, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT n, const D3D12_RECT *rects)
{
   begin(OP_CL_CLEAR_RTV, This);
   w_u64(cpu_handle(rtv));
   w_raw(color, 4 * sizeof(FLOAT), 4);
   w_rects(n, rects);
   msg_send();
}

static void clear_uav(CL *This, bool is_float, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource *res,
                      const void *values, UINT n, const D3D12_RECT *rects)
{
   begin(OP_CL_CLEAR_UAV, This);
   w_u32(is_float);
   w_u64(gpu.ptr);
   w_u64(cpu_handle(cpu));
   w_obj(res);
   w_raw(values, 16, 4);
   w_rects(n, rects);
   msg_send();
}

static void STDMETHODCALLTYPE cl_ClearUnorderedAccessViewUint(CL *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource *res,
                                                              const UINT values[4], UINT n, const D3D12_RECT *rects)
{
   clear_uav(This, false, gpu, cpu, res, values, n, rects);
}

static void STDMETHODCALLTYPE cl_ClearUnorderedAccessViewFloat(CL *This, D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu, ID3D12Resource *res,
                                                               const FLOAT values[4], UINT n, const D3D12_RECT *rects)
{
   clear_uav(This, true, gpu, cpu, res, values, n, rects);
}

enum query_op { QUERY_BEGIN, QUERY_END };

static void query(CL *This, enum query_op op, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index)
{
   begin(OP_CL_QUERY, This);
   w_u32(op);
   w_obj(heap);
   w_u32(type);
   w_u32(index);
   msg_send();
}

static void STDMETHODCALLTYPE cl_BeginQuery(CL *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { query(This, QUERY_BEGIN, heap, type, index); }
static void STDMETHODCALLTYPE cl_EndQuery(CL *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) { query(This, QUERY_END, heap, type, index); }

static void STDMETHODCALLTYPE cl_ResolveQueryData(CL *This, ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT n, ID3D12Resource *dst, UINT64 offset)
{
   begin(OP_CL_RESOLVE_QUERY, This);
   w_obj(heap);
   w_u32(type);
   w_u32(start);
   w_u32(n);
   w_obj(dst);
   w_u64(offset);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetPredication(CL *This, ID3D12Resource *buffer, UINT64 offset, D3D12_PREDICATION_OP op)
{
   begin(OP_CL_PREDICATION, This);
   w_obj(buffer);
   w_u64(offset);
   w_u32(op);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetMarker(CL *This, UINT meta, const void *data, UINT size)
{
}

static void STDMETHODCALLTYPE cl_EndEvent(CL *This)
{
}

static void STDMETHODCALLTYPE cl_ExecuteIndirect(CL *This, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args, UINT64 args_offset,
                                                 ID3D12Resource *count, UINT64 count_offset)
{
   begin(OP_CL_EXECUTE_INDIRECT, This);
   w_obj(sig);
   w_u32(max);
   w_obj(args);
   w_u64(args_offset);
   w_obj(count);
   w_u64(count_offset);
   msg_send();
}

static void STDMETHODCALLTYPE cl_OMSetDepthBounds(CL *This, FLOAT min, FLOAT max)
{
   begin(OP_CL_DEPTH_BOUNDS, This);
   w_f32(min);
   w_f32(max);
   msg_send();
}

static void STDMETHODCALLTYPE cl_SetViewInstanceMask(CL *This, UINT mask)
{
   begin(OP_CL_VIEW_INSTANCE_MASK, This);
   w_u32(mask);
   msg_send();
}

/* The parameter struct is 12 bytes on i386 and 16 on x86-64. */
static void STDMETHODCALLTYPE cl_WriteBufferImmediate(CL *This, UINT n, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
                                                      const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes)
{
   begin(OP_CL_WRITE_IMMEDIATE, This);
   w_u32(n);
   for (UINT i = 0; i < n; i++) {
      w_u64(params[i].Dest);
      w_u32(params[i].Value);
   }
   w_u32(modes != NULL);
   if (modes)
      w_raw(modes, n * sizeof(*modes), 4);
   msg_send();
}

static void STDMETHODCALLTYPE cl_RSSetDepthBias(CL *This, FLOAT bias, FLOAT clamp, FLOAT slope)
{
   begin(OP_CL_DEPTH_BIAS, This);
   w_f32(bias);
   w_f32(clamp);
   w_f32(slope);
   msg_send();
}

static void STDMETHODCALLTYPE cl_IASetIndexBufferStripCutValue(CL *This, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value)
{
   begin(OP_CL_STRIP_CUT, This);
   w_u32(value);
   msg_send();
}

void cmdlist_init(void)
{
   ID3D12GraphicsCommandList10Vtbl *vt = &cmdlist_vtbl;
   vtbl_init(vt, sizeof(*vt), true);
   vt->GetType = cl_GetType;
   vt->Close = cl_Close;
   vt->Reset = cl_Reset;
   vt->ClearState = cl_ClearState;
   vt->DrawInstanced = cl_DrawInstanced;
   vt->DrawIndexedInstanced = cl_DrawIndexedInstanced;
   vt->Dispatch = cl_Dispatch;
   vt->CopyBufferRegion = cl_CopyBufferRegion;
   vt->CopyTextureRegion = cl_CopyTextureRegion;
   vt->CopyResource = cl_CopyResource;
   vt->ResolveSubresource = cl_ResolveSubresource;
   vt->IASetPrimitiveTopology = cl_IASetPrimitiveTopology;
   vt->RSSetViewports = cl_RSSetViewports;
   vt->RSSetScissorRects = cl_RSSetScissorRects;
   vt->OMSetBlendFactor = cl_OMSetBlendFactor;
   vt->OMSetStencilRef = cl_OMSetStencilRef;
   vt->SetPipelineState = cl_SetPipelineState;
   vt->ResourceBarrier = cl_ResourceBarrier;
   vt->SetDescriptorHeaps = cl_SetDescriptorHeaps;
   vt->SetComputeRootSignature = cl_SetComputeRootSignature;
   vt->SetGraphicsRootSignature = cl_SetGraphicsRootSignature;
   vt->SetComputeRootDescriptorTable = cl_SetComputeRootDescriptorTable;
   vt->SetGraphicsRootDescriptorTable = cl_SetGraphicsRootDescriptorTable;
   vt->SetComputeRoot32BitConstant = cl_SetComputeRoot32BitConstant;
   vt->SetGraphicsRoot32BitConstant = cl_SetGraphicsRoot32BitConstant;
   vt->SetComputeRoot32BitConstants = cl_SetComputeRoot32BitConstants;
   vt->SetGraphicsRoot32BitConstants = cl_SetGraphicsRoot32BitConstants;
   vt->SetComputeRootConstantBufferView = cl_SetComputeRootConstantBufferView;
   vt->SetGraphicsRootConstantBufferView = cl_SetGraphicsRootConstantBufferView;
   vt->SetComputeRootShaderResourceView = cl_SetComputeRootShaderResourceView;
   vt->SetGraphicsRootShaderResourceView = cl_SetGraphicsRootShaderResourceView;
   vt->SetComputeRootUnorderedAccessView = cl_SetComputeRootUnorderedAccessView;
   vt->SetGraphicsRootUnorderedAccessView = cl_SetGraphicsRootUnorderedAccessView;
   vt->IASetIndexBuffer = cl_IASetIndexBuffer;
   vt->IASetVertexBuffers = cl_IASetVertexBuffers;
   vt->SOSetTargets = cl_SOSetTargets;
   vt->OMSetRenderTargets = cl_OMSetRenderTargets;
   vt->ClearDepthStencilView = cl_ClearDepthStencilView;
   vt->ClearRenderTargetView = cl_ClearRenderTargetView;
   vt->ClearUnorderedAccessViewUint = cl_ClearUnorderedAccessViewUint;
   vt->ClearUnorderedAccessViewFloat = cl_ClearUnorderedAccessViewFloat;
   vt->BeginQuery = cl_BeginQuery;
   vt->EndQuery = cl_EndQuery;
   vt->ResolveQueryData = cl_ResolveQueryData;
   vt->SetPredication = cl_SetPredication;
   vt->SetMarker = cl_SetMarker;
   vt->BeginEvent = cl_SetMarker;
   vt->EndEvent = cl_EndEvent;
   vt->ExecuteIndirect = cl_ExecuteIndirect;
   vt->OMSetDepthBounds = cl_OMSetDepthBounds;
   vt->ResolveSubresourceRegion = cl_ResolveSubresourceRegion;
   vt->SetViewInstanceMask = cl_SetViewInstanceMask;
   vt->WriteBufferImmediate = cl_WriteBufferImmediate;
   vt->Barrier = cl_Barrier;
   vt->OMSetFrontAndBackStencilRef = cl_OMSetFrontAndBackStencilRef;
   vt->RSSetDepthBias = cl_RSSetDepthBias;
   vt->IASetIndexBufferStripCutValue = cl_IASetIndexBufferStripCutValue;
}
