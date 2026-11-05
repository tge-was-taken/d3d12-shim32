#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "shim.h"

static ID3D12Device14Vtbl device_vtbl;

#define DEV(This) ((struct device *)(This))

/* Structs below are written in their x86-64 layout, see proto.h. */
void w_resource_desc1(const D3D12_RESOURCE_DESC1 *d)
{
   w_align(8);
   w_u32(d->Dimension);
   w_u64(d->Alignment);
   w_u64(d->Width);
   w_u32(d->Height);
   w_u16(d->DepthOrArraySize);
   w_u16(d->MipLevels);
   w_u32(d->Format);
   w_u32(d->SampleDesc.Count);
   w_u32(d->SampleDesc.Quality);
   w_u32(d->Layout);
   w_u32(d->Flags);
   w_raw(&d->SamplerFeedbackMipRegion, sizeof(d->SamplerFeedbackMipRegion), 4);
   w_align(8);
}

void w_resource_desc(const D3D12_RESOURCE_DESC *d)
{
   D3D12_RESOURCE_DESC1 d1 = {0};
   memcpy(&d1, d, sizeof(*d));
   w_resource_desc1(&d1);
}

void r_resource_desc1(D3D12_RESOURCE_DESC1 *d)
{
   r_align(8);
   d->Dimension = r_u32();
   d->Alignment = r_u64();
   d->Width = r_u64();
   d->Height = r_u32();
   uint32_t v = r_u32();
   d->DepthOrArraySize = v & 0xffff;
   d->MipLevels = v >> 16;
   d->Format = r_u32();
   d->SampleDesc.Count = r_u32();
   d->SampleDesc.Quality = r_u32();
   d->Layout = r_u32();
   d->Flags = r_u32();
   r_raw(&d->SamplerFeedbackMipRegion, sizeof(d->SamplerFeedbackMipRegion), 4);
   r_align(8);
}

static void w_heap_desc(const D3D12_HEAP_DESC *d)
{
   w_align(8);
   w_u64(d->SizeInBytes);
   w_raw(&d->Properties, sizeof(d->Properties), 4);
   w_u64(d->Alignment);
   w_u32(d->Flags);
   w_align(8);
}

static void r_heap_desc(D3D12_HEAP_DESC *d)
{
   r_align(8);
   d->SizeInBytes = r_u64();
   r_raw(&d->Properties, sizeof(d->Properties), 4);
   d->Alignment = r_u64();
   d->Flags = r_u32();
   r_align(8);
}

void w_root_sig_desc(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc)
{
   /* All three versions share the field order, and ROOT_PARAMETER and
    * ROOT_PARAMETER1 share their layout. */
   const D3D12_ROOT_SIGNATURE_DESC1 *d = &desc->Desc_1_1;
   size_t range_size = desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ?
      sizeof(D3D12_DESCRIPTOR_RANGE) : sizeof(D3D12_DESCRIPTOR_RANGE1);
   size_t sampler_size = desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_2 ?
      sizeof(D3D12_STATIC_SAMPLER_DESC1) : sizeof(D3D12_STATIC_SAMPLER_DESC);

   w_u32(desc->Version);
   w_u32(d->NumParameters);
   for (UINT i = 0; i < d->NumParameters; i++) {
      const D3D12_ROOT_PARAMETER1 *p = &d->pParameters[i];
      w_u32(p->ParameterType);
      w_u32(p->ShaderVisibility);
      if (p->ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
         w_u32(p->DescriptorTable.NumDescriptorRanges);
         w_raw(p->DescriptorTable.pDescriptorRanges, p->DescriptorTable.NumDescriptorRanges * range_size, 4);
      } else {
         w_raw(&p->Constants, sizeof(p->Constants), 4);
      }
   }
   w_u32(d->NumStaticSamplers);
   w_raw(d->pStaticSamplers, d->NumStaticSamplers * sampler_size, 4);
   w_u32(d->Flags);
}

/* Pipeline subobjects are sent as a list of (type, payload) and rebuilt by the server. */
static void w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const D3D12_SHADER_BYTECODE *b)
{
   if (!b->pShaderBytecode)
      return;
   w_u32(type);
   w_blob(b->pShaderBytecode, b->BytecodeLength);
}

static void w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const void *p, size_t size)
{
   w_u32(type);
   w_u32(size);
   w_raw(p, size, 4);
}

static void w_string(const char *s)
{
   w_blob(s, s ? strlen(s) + 1 : 0);
}

static void w_stream_output(const D3D12_STREAM_OUTPUT_DESC *so)
{
   if (!so->NumEntries)
      return;
   w_u32(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT);
   w_u32(so->NumEntries);
   for (UINT i = 0; i < so->NumEntries; i++) {
      const D3D12_SO_DECLARATION_ENTRY *e = &so->pSODeclaration[i];
      w_u32(e->Stream);
      w_string(e->SemanticName);
      w_u32(e->SemanticIndex);
      w_u32(e->StartComponent | e->ComponentCount << 8 | e->OutputSlot << 16);
   }
   w_u32(so->NumStrides);
   w_raw(so->pBufferStrides, so->NumStrides * sizeof(UINT), 4);
   w_u32(so->RasterizedStream);
}

static void w_input_layout(const D3D12_INPUT_LAYOUT_DESC *il)
{
   w_u32(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT);
   w_u32(il->NumElements);
   for (UINT i = 0; i < il->NumElements; i++) {
      const D3D12_INPUT_ELEMENT_DESC *e = &il->pInputElementDescs[i];
      w_string(e->SemanticName);
      w_raw(&e->SemanticIndex, sizeof(*e) - offsetof(D3D12_INPUT_ELEMENT_DESC, SemanticIndex), 4);
   }
}

static void w_cached_pso(const D3D12_CACHED_PIPELINE_STATE *c)
{
   if (!c->CachedBlobSizeInBytes)
      return;
   w_u32(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO);
   w_blob(c->pCachedBlob, c->CachedBlobSizeInBytes);
}

static size_t subobject_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
   switch (type) {
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(ID3D12RootSignature *);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(D3D12_SHADER_BYTECODE);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(D3D12_STREAM_OUTPUT_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(D3D12_BLEND_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(UINT);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(D3D12_RASTERIZER_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(D3D12_DEPTH_STENCIL_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(D3D12_INPUT_LAYOUT_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(struct D3D12_RT_FORMAT_ARRAY);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(DXGI_FORMAT);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(DXGI_SAMPLE_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(UINT);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(D3D12_CACHED_PIPELINE_STATE);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(D3D12_PIPELINE_STATE_FLAGS);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(D3D12_DEPTH_STENCIL_DESC1);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(D3D12_VIEW_INSTANCING_DESC);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return sizeof(D3D12_DEPTH_STENCIL_DESC2);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return sizeof(D3D12_RASTERIZER_DESC1);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return sizeof(D3D12_RASTERIZER_DESC2);
   case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: return sizeof(D3D12_SERIALIZED_ROOT_SIGNATURE_DESC);
   default: return 0;
   }
}

static bool w_stream(const D3D12_PIPELINE_STATE_STREAM_DESC *desc)
{
   const char *p = desc->pPipelineStateSubobjectStream;
   size_t pos = 0;
   while (pos < desc->SizeInBytes) {
      D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = *(const uint32_t *)(p + pos);
      const void *inner = p + pos + 4;
      size_t size = subobject_size(type);
      if (!size) {
         shim_warn("unknown pipeline subobject type %u", type);
         return false;
      }
      switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
         w_u32(type);
         w_obj(*(ID3D12RootSignature *const *)inner);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
         w_bytecode(type, inner);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
         w_stream_output(inner);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
         w_input_layout(inner);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
         w_cached_pso(inner);
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
         const D3D12_VIEW_INSTANCING_DESC *v = inner;
         w_u32(type);
         w_u32(v->ViewInstanceCount);
         w_raw(v->pViewInstanceLocations, v->ViewInstanceCount * sizeof(D3D12_VIEW_INSTANCE_LOCATION), 4);
         w_u32(v->Flags);
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: {
         const D3D12_SERIALIZED_ROOT_SIGNATURE_DESC *s = inner;
         w_u32(type);
         w_blob(s->pSerializedBlob, s->SerializedBlobSizeInBytes);
         break;
      }
      default:
         w_subobject_raw(type, inner, size);
         break;
      }
      /* Each subobject is aligned to the pointer size. */
      pos = (pos + 4 + size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
   }
   return true;
}

static void w_graphics_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d)
{
   w_u32(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
   w_obj(d->pRootSignature);
   w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &d->VS);
   w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &d->PS);
   w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, &d->DS);
   w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, &d->HS);
   w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, &d->GS);
   w_stream_output(&d->StreamOutput);
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &d->BlendState, sizeof(d->BlendState));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &d->SampleMask, sizeof(d->SampleMask));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &d->RasterizerState, sizeof(d->RasterizerState));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, &d->DepthStencilState, sizeof(d->DepthStencilState));
   w_input_layout(&d->InputLayout);
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, &d->IBStripCutValue, sizeof(d->IBStripCutValue));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &d->PrimitiveTopologyType, sizeof(d->PrimitiveTopologyType));
   struct D3D12_RT_FORMAT_ARRAY rts = {.NumRenderTargets = d->NumRenderTargets};
   memcpy(rts.RTFormats, d->RTVFormats, sizeof(rts.RTFormats));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rts, sizeof(rts));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &d->DSVFormat, sizeof(d->DSVFormat));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &d->SampleDesc, sizeof(d->SampleDesc));
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, &d->NodeMask, sizeof(d->NodeMask));
   w_cached_pso(&d->CachedPSO);
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, &d->Flags, sizeof(d->Flags));
}

static void w_compute_pso(const D3D12_COMPUTE_PIPELINE_STATE_DESC *d)
{
   w_u32(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
   w_obj(d->pRootSignature);
   w_bytecode(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &d->CS);
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, &d->NodeMask, sizeof(d->NodeMask));
   w_cached_pso(&d->CachedPSO);
   w_subobject_raw(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, &d->Flags, sizeof(d->Flags));
}

#define PSO_END 0xffffffffu

static HRESULT create_pso(struct device *d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *g,
                          const D3D12_COMPUTE_PIPELINE_STATE_DESC *c, const D3D12_PIPELINE_STATE_STREAM_DESC *s,
                          REFIID riid, void **ppv)
{
   struct obj *o = obj_new(sizeof(*o), K_PSO, &pso_vtbl, d);
   msg_begin(OP_CREATE_PSO);
   w_obj(d);
   w_u32(o->id);
   bool ok = true;
   if (g)
      w_graphics_pso(g);
   else if (c)
      w_compute_pso(c);
   else
      ok = w_stream(s);
   w_u32(PSO_END);
   msg_call();
   HRESULT hr = ok ? r_hr() : E_INVALIDARG;
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateGraphicsPipelineState(ID3D12Device14 *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **ppv)
{
   return create_pso(DEV(This), desc, NULL, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateComputePipelineState(ID3D12Device14 *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **ppv)
{
   return create_pso(DEV(This), NULL, desc, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreatePipelineState(ID3D12Device14 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **ppv)
{
   return create_pso(DEV(This), NULL, NULL, desc, riid, ppv);
}

static UINT STDMETHODCALLTYPE dev_GetNodeCount(ID3D12Device14 *This)
{
   return DEV(This)->nodes;
}

static LUID STDMETHODCALLTYPE dev_GetAdapterLuid(ID3D12Device14 *This)
{
   return DEV(This)->luid;
}

static UINT STDMETHODCALLTYPE dev_GetDescriptorHandleIncrementSize(ID3D12Device14 *This, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   return type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES ? DEV(This)->inc[type] : 0;
}

static HRESULT create_queue(struct device *d, const D3D12_COMMAND_QUEUE_DESC *desc, const IID *creator, REFIID riid, void **ppv)
{
   struct queue *q = obj_new(sizeof(*q), K_QUEUE, &queue_vtbl, d);
   q->desc = *desc;
   msg_begin(OP_CREATE_QUEUE);
   w_obj(d);
   w_u32(q->o.id);
   w_raw(desc, sizeof(*desc), 4);
   w_u32(creator != NULL);
   if (creator)
      w_raw(creator, sizeof(*creator), 4);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(q, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandQueue(ID3D12Device14 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **ppv)
{
   return create_queue(DEV(This), desc, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandQueue1(ID3D12Device14 *This, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator, REFIID riid, void **ppv)
{
   return create_queue(DEV(This), desc, creator, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandAllocator(ID3D12Device14 *This, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **ppv)
{
   struct obj *o = obj_new(sizeof(*o), K_ALLOCATOR, &allocator_vtbl, DEV(This));
   msg_begin(OP_CREATE_ALLOCATOR);
   w_obj(This);
   w_u32(o->id);
   w_u32(type);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

static HRESULT create_list(struct device *d, uint32_t variant, UINT node, D3D12_COMMAND_LIST_TYPE type,
                           ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, D3D12_COMMAND_LIST_FLAGS flags,
                           REFIID riid, void **ppv)
{
   struct cmdlist *l = obj_new(sizeof(*l), K_CMDLIST, &cmdlist_vtbl, d);
   l->type = type;
   msg_begin(OP_CREATE_LIST);
   w_obj(d);
   w_u32(l->o.id);
   w_u32(variant);
   w_u32(node);
   w_u32(type);
   w_obj(alloc);
   w_obj(pso);
   w_u32(flags);
   msg_call();
   HRESULT hr = r_hr();
   l->level = r_u32();
   msg_end();
   return obj_finish(l, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandList(ID3D12Device14 *This, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *alloc,
                                                     ID3D12PipelineState *pso, REFIID riid, void **ppv)
{
   return create_list(DEV(This), 0, node, type, alloc, pso, 0, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandList1(ID3D12Device14 *This, UINT node, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
                                                      REFIID riid, void **ppv)
{
   return create_list(DEV(This), 1, node, type, NULL, NULL, flags, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CheckFeatureSupport(ID3D12Device14 *This, D3D12_FEATURE feature, void *data, UINT size)
{
   if (feature == D3D12_FEATURE_FEATURE_LEVELS && size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
      return E_INVALIDARG;
   msg_begin(OP_CHECK_FEATURE);
   w_obj(This);
   w_u32(feature);
   w_u32(size);
   if (feature == D3D12_FEATURE_FEATURE_LEVELS) {
      D3D12_FEATURE_DATA_FEATURE_LEVELS *fl = data;
      w_u32(fl->NumFeatureLevels);
      w_raw(fl->pFeatureLevelsRequested, fl->NumFeatureLevels * sizeof(D3D_FEATURE_LEVEL), 4);
   } else {
      w_blob(data, size);
   }
   msg_call_cached(0);
   HRESULT hr = r_hr();
   if (feature == D3D12_FEATURE_FEATURE_LEVELS)
      ((D3D12_FEATURE_DATA_FEATURE_LEVELS *)data)->MaxSupportedFeatureLevel = r_u32();
   else
      r_blob(data, size);
   msg_end();
   return hr;
}

static HRESULT STDMETHODCALLTYPE dev_CreateDescriptorHeap(ID3D12Device14 *This, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **ppv)
{
   struct desc_heap *h = obj_new(sizeof(*h), K_DESC_HEAP, &desc_heap_vtbl, DEV(This));
   h->desc = *desc;
   msg_begin(OP_CREATE_DESC_HEAP);
   w_obj(This);
   w_u32(h->o.id);
   w_raw(desc, sizeof(*desc), 4);
   msg_call();
   HRESULT hr = r_hr();
   uint64_t cpu = r_u64();
   h->gpu = r_u64();
   msg_end();
   if (SUCCEEDED(hr))
      desc_heap_register(h, cpu);
   return obj_finish(h, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateRootSignature(ID3D12Device14 *This, UINT node, const void *blob, SIZE_T size, REFIID riid, void **ppv)
{
   struct obj *o = obj_new(sizeof(*o), K_ROOT_SIG, &root_sig_vtbl, DEV(This));
   msg_begin(OP_CREATE_ROOT_SIG);
   w_obj(This);
   w_u32(o->id);
   w_u32(node);
   w_blob(blob, size);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

static void w_view_header(ID3D12Device14 *dev, enum view_kind kind, const void *res, const void *counter, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   msg_begin(OP_CREATE_VIEW);
   w_obj(dev);
   w_u32(kind);
   w_u64(cpu_handle(dest));
   w_obj(res);
   w_obj(counter);
}

/* View descs start with a few u32s followed by a union that may hold u64s;
 * the union moves to an 8-byte aligned offset on x86-64. */
static void w_view_desc(const void *desc, size_t head, size_t size)
{
   w_u32(desc != NULL);
   if (!desc)
      return;
   w_u32(head);
   w_raw(desc, head, 4);
   w_u32(size - head);
   w_raw((const char *)desc + head, size - head, 8);
}

static void STDMETHODCALLTYPE dev_CreateConstantBufferView(ID3D12Device14 *This, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_CBV, NULL, NULL, dest);
   w_u32(desc != NULL);
   if (desc) {
      w_u64(desc->BufferLocation);
      w_u32(desc->SizeInBytes);
   }
   msg_send();
}

static void STDMETHODCALLTYPE dev_CreateShaderResourceView(ID3D12Device14 *This, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_SRV, res, NULL, dest);
   w_view_desc(desc, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Buffer), sizeof(*desc));
   msg_send();
}

static void STDMETHODCALLTYPE dev_CreateUnorderedAccessView(ID3D12Device14 *This, ID3D12Resource *res, ID3D12Resource *counter,
                                                           const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_UAV, res, counter, dest);
   w_view_desc(desc, offsetof(D3D12_UNORDERED_ACCESS_VIEW_DESC, Buffer), sizeof(*desc));
   msg_send();
}

static void STDMETHODCALLTYPE dev_CreateRenderTargetView(ID3D12Device14 *This, ID3D12Resource *res, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_RTV, res, NULL, dest);
   w_view_desc(desc, offsetof(D3D12_RENDER_TARGET_VIEW_DESC, Buffer), sizeof(*desc));
   msg_send();
}

static void STDMETHODCALLTYPE dev_CreateDepthStencilView(ID3D12Device14 *This, ID3D12Resource *res, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_DSV, res, NULL, dest);
   w_view_desc(desc, sizeof(*desc), sizeof(*desc));
   msg_send();
}

static void STDMETHODCALLTYPE dev_CreateSampler(ID3D12Device14 *This, const D3D12_SAMPLER_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_SAMPLER, NULL, NULL, dest);
   w_view_desc(desc, sizeof(*desc), sizeof(*desc));
   msg_send();
}

static void STDMETHODCALLTYPE dev_CreateSampler2(ID3D12Device14 *This, const D3D12_SAMPLER_DESC2 *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
   w_view_header(This, VIEW_SAMPLER2, NULL, NULL, dest);
   w_view_desc(desc, sizeof(*desc), sizeof(*desc));
   msg_send();
}

static void w_ranges(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *starts, const UINT *sizes)
{
   w_u32(n);
   for (UINT i = 0; i < n; i++) {
      w_u64(cpu_handle(starts[i]));
      w_u32(sizes ? sizes[i] : 1);
   }
}

static void STDMETHODCALLTYPE dev_CopyDescriptors(ID3D12Device14 *This, UINT ndst, const D3D12_CPU_DESCRIPTOR_HANDLE *dst, const UINT *dst_sizes,
                                                  UINT nsrc, const D3D12_CPU_DESCRIPTOR_HANDLE *src, const UINT *src_sizes,
                                                  D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   msg_begin(OP_COPY_DESCRIPTORS);
   w_obj(This);
   w_u32(type);
   w_ranges(ndst, dst, dst_sizes);
   w_ranges(nsrc, src, src_sizes);
   msg_send();
}

static void STDMETHODCALLTYPE dev_CopyDescriptorsSimple(ID3D12Device14 *This, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                                        D3D12_DESCRIPTOR_HEAP_TYPE type)
{
   dev_CopyDescriptors(This, 1, &dst, &n, 1, &src, &n, type);
}

/* variant: 0 GetResourceAllocationInfo, 1 ..Info1, 2 ..Info2, 3 ..Info3 */
static D3D12_RESOURCE_ALLOCATION_INFO allocation_info(ID3D12Device14 *This, uint32_t variant, UINT mask, UINT n,
                                                      const D3D12_RESOURCE_DESC *descs, const D3D12_RESOURCE_DESC1 *descs1,
                                                      const UINT32 *ncast, const DXGI_FORMAT *const *cast,
                                                      D3D12_RESOURCE_ALLOCATION_INFO1 *info1)
{
   D3D12_RESOURCE_ALLOCATION_INFO info;
   msg_begin(OP_ALLOCATION_INFO);
   w_obj(This);
   w_u32(variant);
   w_u32(mask);
   w_u32(n);
   for (UINT i = 0; i < n; i++) {
      if (descs1)
         w_resource_desc1(&descs1[i]);
      else
         w_resource_desc(&descs[i]);
      uint32_t nc = ncast ? ncast[i] : 0;
      w_u32(nc);
      w_raw(nc ? cast[i] : NULL, nc * sizeof(DXGI_FORMAT), 4);
   }
   w_u32(info1 != NULL);
   msg_call_cached(0);
   r_raw(&info, sizeof(info), 8);
   if (info1)
      r_raw(info1, n * sizeof(*info1), 8);
   msg_end();
   return info;
}

static D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE dev_GetResourceAllocationInfo(ID3D12Device14 *This, UINT mask, UINT n, const D3D12_RESOURCE_DESC *descs)
{
   return allocation_info(This, 0, mask, n, descs, NULL, NULL, NULL, NULL);
}

static D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE dev_GetResourceAllocationInfo1(ID3D12Device14 *This, UINT mask, UINT n, const D3D12_RESOURCE_DESC *descs,
                                                                                      D3D12_RESOURCE_ALLOCATION_INFO1 *info1)
{
   return allocation_info(This, 1, mask, n, descs, NULL, NULL, NULL, info1);
}

static D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE dev_GetResourceAllocationInfo2(ID3D12Device14 *This, UINT mask, UINT n, const D3D12_RESOURCE_DESC1 *descs,
                                                                                      D3D12_RESOURCE_ALLOCATION_INFO1 *info1)
{
   return allocation_info(This, 2, mask, n, NULL, descs, NULL, NULL, info1);
}

static D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE dev_GetResourceAllocationInfo3(ID3D12Device14 *This, UINT mask, UINT n, const D3D12_RESOURCE_DESC1 *descs,
                                                                                      const UINT32 *ncast, const DXGI_FORMAT *const *cast,
                                                                                      D3D12_RESOURCE_ALLOCATION_INFO1 *info1)
{
   return allocation_info(This, 3, mask, n, NULL, descs, ncast, cast, info1);
}

static D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE dev_GetCustomHeapProperties(ID3D12Device14 *This, UINT node, D3D12_HEAP_TYPE type)
{
   D3D12_HEAP_PROPERTIES p;
   msg_begin(OP_CUSTOM_HEAP_PROPS);
   w_obj(This);
   w_u32(node);
   w_u32(type);
   msg_call_cached(0);
   r_raw(&p, sizeof(p), 4);
   msg_end();
   return p;
}

static HRESULT create_resource(struct device *d, enum resource_variant variant, ID3D12Heap *heap_iface, UINT64 offset,
                               const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC1 *desc,
                               UINT state, const D3D12_CLEAR_VALUE *clear, UINT32 ncast, const DXGI_FORMAT *cast,
                               REFIID riid, void **ppv)
{
   struct resource *r = obj_new(sizeof(*r), K_RESOURCE, &resource_vtbl, d);
   struct heap *heap = (struct heap *)heap_iface;
   bool buffer = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
   enum cpu_access access = props ? heap_cpu_access(props) : CPU_NONE;
   struct shadow *shadow = NULL;
   int fd = -1;
   if (access && buffer)
      shadow = shadow_create(desc->Width, access, &fd);

   D3D12_HEAP_PROPERTIES no_props = {0};
   msg_begin(OP_CREATE_RESOURCE);
   w_obj(d);
   w_u32(r->o.id);
   w_u32(variant);
   w_obj(heap);
   w_u64(offset);
   w_raw(props ? props : &no_props, sizeof(no_props), 4);
   w_u32(flags);
   w_resource_desc1(desc);
   w_u32(state);
   w_u32(clear != NULL);
   if (clear)
      w_raw(clear, sizeof(*clear), 4);
   w_u32(ncast);
   w_raw(cast, ncast * sizeof(DXGI_FORMAT), 4);
   w_u32(shadow ? access : CPU_NONE);
   if (shadow) {
      w_u64(shadow->offset);
      msg_fd(fd);
   }
   msg_call();
   HRESULT hr = r_hr();
   r_resource_desc1(&r->desc);
   r->va = r_u64();
   r_raw(&r->props, sizeof(r->props), 4);
   r->flags = r_u32();
   bool shadow_ok = r_u32();
   msg_end();
   if (fd >= 0)
      close(fd);

   if (SUCCEEDED(hr) && shadow && shadow_ok) {
      shadow_attach(shadow, r->o.id);
      r->shadow = shadow;
      r->map = shadow->ptr;
   } else {
      shadow_destroy(shadow);
   }
   if (SUCCEEDED(hr) && heap) {
      obj_AddRef(heap);
      r->heap = heap;
      if (heap->shadow && buffer)
         r->map = heap->shadow->ptr + offset;
   }
   return obj_finish(r, hr, riid, ppv);
}

static D3D12_RESOURCE_DESC1 desc1(const D3D12_RESOURCE_DESC *d)
{
   D3D12_RESOURCE_DESC1 d1 = {0};
   memcpy(&d1, d, sizeof(*d));
   return d1;
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags,
                                                            const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
                                                            const D3D12_CLEAR_VALUE *clear, REFIID riid, void **ppv)
{
   D3D12_RESOURCE_DESC1 d1 = desc1(desc);
   return create_resource(DEV(This), RES_COMMITTED, NULL, 0, props, flags, &d1, state, clear, 0, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource1(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags,
                                                             const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
                                                             const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
                                                             REFIID riid, void **ppv)
{
   if (session)
      return E_NOTIMPL;
   return dev_CreateCommittedResource(This, props, flags, desc, state, clear, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource2(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags,
                                                             const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state,
                                                             const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
                                                             REFIID riid, void **ppv)
{
   if (session)
      return E_NOTIMPL;
   return create_resource(DEV(This), RES_COMMITTED2, NULL, 0, props, flags, desc, state, clear, 0, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommittedResource3(ID3D12Device14 *This, const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags,
                                                             const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT layout,
                                                             const D3D12_CLEAR_VALUE *clear, ID3D12ProtectedResourceSession *session,
                                                             UINT32 ncast, const DXGI_FORMAT *cast, REFIID riid, void **ppv)
{
   if (session)
      return E_NOTIMPL;
   return create_resource(DEV(This), RES_COMMITTED3, NULL, 0, props, flags, desc, layout, clear, ncast, cast, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource(ID3D12Device14 *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC *desc,
                                                         D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **ppv)
{
   D3D12_RESOURCE_DESC1 d1 = desc1(desc);
   return create_resource(DEV(This), RES_PLACED, heap, offset, NULL, 0, &d1, state, clear, 0, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource1(ID3D12Device14 *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc,
                                                          D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **ppv)
{
   return create_resource(DEV(This), RES_PLACED1, heap, offset, NULL, 0, desc, state, clear, 0, NULL, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreatePlacedResource2(ID3D12Device14 *This, ID3D12Heap *heap, UINT64 offset, const D3D12_RESOURCE_DESC1 *desc,
                                                          D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE *clear, UINT32 ncast,
                                                          const DXGI_FORMAT *cast, REFIID riid, void **ppv)
{
   return create_resource(DEV(This), RES_PLACED2, heap, offset, NULL, 0, desc, layout, clear, ncast, cast, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateHeap(ID3D12Device14 *This, const D3D12_HEAP_DESC *desc, REFIID riid, void **ppv)
{
   struct heap *h = obj_new(sizeof(*h), K_HEAP, &heap_vtbl, DEV(This));
   enum cpu_access access = heap_cpu_access(&desc->Properties);
   int fd = -1;
   struct shadow *shadow = access ? shadow_create(desc->SizeInBytes, access, &fd) : NULL;

   msg_begin(OP_CREATE_HEAP);
   w_obj(This);
   w_u32(h->o.id);
   w_heap_desc(desc);
   w_u32(shadow ? access : CPU_NONE);
   if (shadow) {
      w_u64(shadow->offset);
      msg_fd(fd);
   }
   msg_call();
   HRESULT hr = r_hr();
   r_heap_desc(&h->desc);
   bool shadow_ok = r_u32();
   msg_end();
   if (fd >= 0)
      close(fd);

   if (SUCCEEDED(hr) && shadow && shadow_ok) {
      shadow_attach(shadow, h->o.id);
      h->shadow = shadow;
   } else {
      shadow_destroy(shadow);
   }
   return obj_finish(h, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateHeap1(ID3D12Device14 *This, const D3D12_HEAP_DESC *desc, ID3D12ProtectedResourceSession *session, REFIID riid, void **ppv)
{
   if (session)
      return E_NOTIMPL;
   return dev_CreateHeap(This, desc, riid, ppv);
}

static HRESULT residency(ID3D12Device14 *This, uint32_t what, D3D12_RESIDENCY_FLAGS flags, UINT n, ID3D12Pageable *const *objs,
                         ID3D12Fence *fence, UINT64 value)
{
   msg_begin(OP_RESIDENCY);
   w_obj(This);
   w_u32(what);
   w_u32(flags);
   w_obj(fence);
   w_u64(value);
   w_u32(n);
   for (UINT i = 0; i < n; i++)
      w_obj(objs[i]);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return hr;
}

static HRESULT STDMETHODCALLTYPE dev_MakeResident(ID3D12Device14 *This, UINT n, ID3D12Pageable *const *objs)
{
   return residency(This, 0, 0, n, objs, NULL, 0);
}

static HRESULT STDMETHODCALLTYPE dev_Evict(ID3D12Device14 *This, UINT n, ID3D12Pageable *const *objs)
{
   return residency(This, 1, 0, n, objs, NULL, 0);
}

static HRESULT STDMETHODCALLTYPE dev_EnqueueMakeResident(ID3D12Device14 *This, D3D12_RESIDENCY_FLAGS flags, UINT n, ID3D12Pageable *const *objs,
                                                        ID3D12Fence *fence, UINT64 value)
{
   return residency(This, 2, flags, n, objs, fence, value);
}

static HRESULT STDMETHODCALLTYPE dev_SetResidencyPriority(ID3D12Device14 *This, UINT n, ID3D12Pageable *const *objs, const D3D12_RESIDENCY_PRIORITY *prio)
{
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE dev_CreateFence(ID3D12Device14 *This, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void **ppv)
{
   struct fence *f = obj_new(sizeof(*f), K_FENCE, &fence_vtbl, DEV(This));
   f->flags = flags;
   int table;
   f->slot = fence_slot_alloc(&table);
   msg_begin(OP_CREATE_FENCE);
   w_obj(This);
   w_u32(f->o.id);
   w_u64(initial);
   w_u32(flags);
   w_u32(f->slot);
   w_u32(table >= 0);
   if (table >= 0)
      msg_fd(table);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   if (table >= 0)
      close(table);
   return obj_finish(f, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_GetDeviceRemovedReason(ID3D12Device14 *This)
{
   msg_begin(OP_REMOVED_REASON);
   w_obj(This);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return hr;
}

static void footprints(ID3D12Device14 *This, const D3D12_RESOURCE_DESC1 *desc, UINT first, UINT n, UINT64 base,
                       D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *row_sizes, UINT64 *total)
{
   msg_begin(OP_FOOTPRINTS);
   w_obj(This);
   w_resource_desc1(desc);
   w_u32(first);
   w_u32(n);
   w_u64(base);
   msg_call_cached(0);
   for (UINT i = 0; i < n; i++) {
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT l;
      r_align(8);
      l.Offset = r_u64();
      r_raw(&l.Footprint, sizeof(l.Footprint), 4);
      r_align(8);
      UINT r = r_u32();
      UINT64 rs = r_u64();
      if (layouts)
         layouts[i] = l;
      if (rows)
         rows[i] = r;
      if (row_sizes)
         row_sizes[i] = rs;
   }
   UINT64 t = r_u64();
   if (total)
      *total = t;
   msg_end();
}

static void STDMETHODCALLTYPE dev_GetCopyableFootprints(ID3D12Device14 *This, const D3D12_RESOURCE_DESC *desc, UINT first, UINT n, UINT64 base,
                                                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *row_sizes, UINT64 *total)
{
   D3D12_RESOURCE_DESC1 d1 = desc1(desc);
   footprints(This, &d1, first, n, base, layouts, rows, row_sizes, total);
}

static void STDMETHODCALLTYPE dev_GetCopyableFootprints1(ID3D12Device14 *This, const D3D12_RESOURCE_DESC1 *desc, UINT first, UINT n, UINT64 base,
                                                         D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows, UINT64 *row_sizes, UINT64 *total)
{
   footprints(This, desc, first, n, base, layouts, rows, row_sizes, total);
}

static HRESULT STDMETHODCALLTYPE dev_CreateQueryHeap(ID3D12Device14 *This, const D3D12_QUERY_HEAP_DESC *desc, REFIID riid, void **ppv)
{
   struct obj *o = obj_new(sizeof(*o), K_QUERY_HEAP, &query_heap_vtbl, DEV(This));
   msg_begin(OP_CREATE_QUERY_HEAP);
   w_obj(This);
   w_u32(o->id);
   w_raw(desc, sizeof(*desc), 4);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_CreateCommandSignature(ID3D12Device14 *This, const D3D12_COMMAND_SIGNATURE_DESC *desc, ID3D12RootSignature *root,
                                                           REFIID riid, void **ppv)
{
   struct obj *o = obj_new(sizeof(*o), K_CMD_SIG, &cmd_sig_vtbl, DEV(This));
   msg_begin(OP_CREATE_CMD_SIG);
   w_obj(This);
   w_u32(o->id);
   w_obj(root);
   w_u32(desc->ByteStride);
   w_u32(desc->NumArgumentDescs);
   w_raw(desc->pArgumentDescs, desc->NumArgumentDescs * sizeof(D3D12_INDIRECT_ARGUMENT_DESC), 4);
   w_u32(desc->NodeMask);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

static HRESULT STDMETHODCALLTYPE dev_SetEventOnMultipleFenceCompletion(ID3D12Device14 *This, ID3D12Fence *const *fences, const UINT64 *values,
                                                                      UINT n, D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event)
{
   int fd = event ? (int)(intptr_t)event : eventfd(0, EFD_CLOEXEC);
   msg_begin(OP_MULTI_FENCE_EVENT);
   w_obj(This);
   w_u32(flags);
   w_u32(n);
   for (UINT i = 0; i < n; i++) {
      w_obj(fences[i]);
      w_u64(values[i]);
   }
   msg_fd(fd);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   if (!event) {
      struct pollfd p = {fd, POLLIN, 0};
      while (SUCCEEDED(hr) && poll(&p, 1, -1) < 0 && errno == EINTR)
         ;
      close(fd);
   }
   return hr;
}

HRESULT device_create(uint32_t adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **ppv)
{
   struct device *d = obj_new(sizeof(*d), K_DEVICE, &device_vtbl, NULL);
   msg_begin(OP_CREATE_DEVICE);
   w_u32(adapter);
   w_u32(d->o.id);
   w_u32(level);
   msg_call();
   HRESULT hr = r_hr();
   d->level = r_u32();
   d->nodes = r_u32();
   for (int i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; i++)
      d->inc[i] = r_u32();
   r_raw(&d->luid, sizeof(d->luid), 4);
   msg_end();
   return obj_finish(d, hr, riid, ppv);
}

__attribute__((constructor)) static void init(void)
{
   ID3D12Device14Vtbl *vt = &device_vtbl;
   vtbl_init(vt, sizeof(*vt), true);
   vt->GetNodeCount = dev_GetNodeCount;
   vt->CreateCommandQueue = dev_CreateCommandQueue;
   vt->CreateCommandAllocator = dev_CreateCommandAllocator;
   vt->CreateGraphicsPipelineState = dev_CreateGraphicsPipelineState;
   vt->CreateComputePipelineState = dev_CreateComputePipelineState;
   vt->CreateCommandList = dev_CreateCommandList;
   vt->CheckFeatureSupport = dev_CheckFeatureSupport;
   vt->CreateDescriptorHeap = dev_CreateDescriptorHeap;
   vt->GetDescriptorHandleIncrementSize = dev_GetDescriptorHandleIncrementSize;
   vt->CreateRootSignature = dev_CreateRootSignature;
   vt->CreateConstantBufferView = dev_CreateConstantBufferView;
   vt->CreateShaderResourceView = dev_CreateShaderResourceView;
   vt->CreateUnorderedAccessView = dev_CreateUnorderedAccessView;
   vt->CreateRenderTargetView = dev_CreateRenderTargetView;
   vt->CreateDepthStencilView = dev_CreateDepthStencilView;
   vt->CreateSampler = dev_CreateSampler;
   vt->CopyDescriptors = dev_CopyDescriptors;
   vt->CopyDescriptorsSimple = dev_CopyDescriptorsSimple;
   vt->GetResourceAllocationInfo = dev_GetResourceAllocationInfo;
   vt->GetCustomHeapProperties = dev_GetCustomHeapProperties;
   vt->CreateCommittedResource = dev_CreateCommittedResource;
   vt->CreateHeap = dev_CreateHeap;
   vt->CreatePlacedResource = dev_CreatePlacedResource;
   vt->MakeResident = dev_MakeResident;
   vt->Evict = dev_Evict;
   vt->CreateFence = dev_CreateFence;
   vt->GetDeviceRemovedReason = dev_GetDeviceRemovedReason;
   vt->GetCopyableFootprints = dev_GetCopyableFootprints;
   vt->CreateQueryHeap = dev_CreateQueryHeap;
   vt->CreateCommandSignature = dev_CreateCommandSignature;
   vt->GetAdapterLuid = dev_GetAdapterLuid;
   vt->SetEventOnMultipleFenceCompletion = dev_SetEventOnMultipleFenceCompletion;
   vt->SetResidencyPriority = dev_SetResidencyPriority;
   vt->CreatePipelineState = dev_CreatePipelineState;
   vt->EnqueueMakeResident = dev_EnqueueMakeResident;
   vt->CreateCommandList1 = dev_CreateCommandList1;
   vt->CreateCommittedResource1 = dev_CreateCommittedResource1;
   vt->CreateHeap1 = dev_CreateHeap1;
   vt->GetResourceAllocationInfo1 = dev_GetResourceAllocationInfo1;
   vt->GetResourceAllocationInfo2 = dev_GetResourceAllocationInfo2;
   vt->CreateCommittedResource2 = dev_CreateCommittedResource2;
   vt->CreatePlacedResource1 = dev_CreatePlacedResource1;
   vt->GetCopyableFootprints1 = dev_GetCopyableFootprints1;
   vt->CreateCommandQueue1 = dev_CreateCommandQueue1;
   vt->CreateCommittedResource3 = dev_CreateCommittedResource3;
   vt->CreatePlacedResource2 = dev_CreatePlacedResource2;
   vt->CreateSampler2 = dev_CreateSampler2;
   vt->GetResourceAllocationInfo3 = dev_GetResourceAllocationInfo3;

   objects_init();
   cmdlist_init();
}

/* dxcore.c */
uint32_t dxcore_adapter_id(IUnknown *adapter);

EXPORT HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **ppv)
{
   uint32_t id = 0;
   if (adapter && !(id = dxcore_adapter_id(adapter)))
      return E_INVALIDARG;
   return device_create(id, level, riid, ppv);
}

static HRESULT serialize_root_sig(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **error)
{
   msg_begin(OP_SERIALIZE_ROOT_SIG);
   w_root_sig_desc(desc);
   msg_call();
   HRESULT hr = r_hr();
   size_t size, err_size;
   const void *data = r_blob_ptr(&size);
   const void *err = r_blob_ptr(&err_size);
   if (blob)
      *blob = NULL;
   if (error)
      *error = NULL;
   if (SUCCEEDED(hr) && blob)
      blob_create(data, size, blob);
   if (err_size && error)
      blob_create(err, err_size, error);
   msg_end();
   return hr;
}

EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **error)
{
   return serialize_root_sig(desc, blob, error);
}

EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob **blob, ID3DBlob **error)
{
   D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {.Version = version, .Desc_1_0 = *desc};
   return serialize_root_sig(&v, blob, error);
}

EXPORT HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT n, const IID *iids, void *configs, UINT *sizes)
{
   if (configs)
      return E_NOTIMPL;
   msg_begin(OP_ENABLE_EXPERIMENTAL);
   w_u32(n);
   w_raw(iids, n * sizeof(IID), 4);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return hr;
}

EXPORT HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **ppv)
{
   if (ppv)
      *ppv = NULL;
   return E_NOINTERFACE;
}

EXPORT HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void **ppv)
{
   if (ppv)
      *ppv = NULL;
   return E_NOINTERFACE;
}

EXPORT HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **ppv)
{
   return E_NOTIMPL;
}

EXPORT HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *data, SIZE_T size, REFIID riid, void **ppv)
{
   return E_NOTIMPL;
}
