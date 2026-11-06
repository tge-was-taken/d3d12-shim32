#define _GNU_SOURCE
#define INITGUID
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shim.h"

/* Ids of freed objects are reused, which keeps the server's object table small. */
static pthread_mutex_t ids_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_id = 1, *free_ids, nfree_ids, free_ids_cap;

static uint32_t id_alloc(void)
{
   pthread_mutex_lock(&ids_lock);
   uint32_t id = nfree_ids ? free_ids[--nfree_ids] : next_id++;
   pthread_mutex_unlock(&ids_lock);
   return id;
}

static void id_free(uint32_t id)
{
   pthread_mutex_lock(&ids_lock);
   if (nfree_ids == free_ids_cap) {
      free_ids_cap = free_ids_cap ? free_ids_cap * 2 : 256;
      free_ids = realloc(free_ids, free_ids_cap * sizeof(*free_ids));
   }
   free_ids[nfree_ids++] = id;
   pthread_mutex_unlock(&ids_lock);
}

void *obj_new(size_t size, enum kind kind, const void *vtbl, struct device *device)
{
   struct obj *o = calloc(1, size);
   o->vtbl = vtbl;
   o->refs = 1;
   o->id = id_alloc();
   o->kind = kind;
   o->device = device;
   if (device)
      obj_AddRef(device);
   return o;
}

static void obj_free(struct obj *o);

/* Hands the new object to the caller through QueryInterface, or frees it on failure. */
HRESULT obj_finish(void *p, HRESULT hr, REFIID riid, void **ppv)
{
   struct obj *o = p;
   if (FAILED(hr)) {
      obj_free(o);
      if (ppv)
         *ppv = NULL;
      return hr;
   }
   if (!ppv) {
      obj_Release(o);
      return S_FALSE;
   }
   hr = obj_QueryInterface(o, riid, ppv);
   obj_Release(o);
   return hr;
}

uint32_t obj_id(const void *iface)
{
   return iface ? ((const struct obj *)iface)->id : 0;
}

/* Interfaces each kind answers to. A device or command list also answers to
 * its numbered versions up to the level the server reported. */
static const IID *const chain_device[] = {&IID_IUnknown, &IID_ID3D12Object, &IID_ID3D12Device, NULL};
static const IID *const device_versions[] = {
   &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4,
   &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9,
   &IID_ID3D12Device10, &IID_ID3D12Device11, &IID_ID3D12Device12, &IID_ID3D12Device13, &IID_ID3D12Device14,
};
static const IID *const chain_cmdlist[] = {&IID_IUnknown, &IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12CommandList, NULL};
static const IID *const cmdlist_versions[] = {
   &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1, &IID_ID3D12GraphicsCommandList2,
   &IID_ID3D12GraphicsCommandList3, &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5,
   &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7, &IID_ID3D12GraphicsCommandList8,
   &IID_ID3D12GraphicsCommandList9, &IID_ID3D12GraphicsCommandList10,
};

#define PAGEABLE &IID_IUnknown, &IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable
static const IID *const chain_blob[] = {&IID_IUnknown, &IID_ID3D10Blob, NULL};
static const IID *const chain_queue[] = {PAGEABLE, &IID_ID3D12CommandQueue, NULL};
static const IID *const chain_allocator[] = {PAGEABLE, &IID_ID3D12CommandAllocator, NULL};
static const IID *const chain_fence[] = {PAGEABLE, &IID_ID3D12Fence, &IID_ID3D12Fence1, NULL};
static const IID *const chain_desc_heap[] = {PAGEABLE, &IID_ID3D12DescriptorHeap, NULL};
static const IID *const chain_query_heap[] = {PAGEABLE, &IID_ID3D12QueryHeap, NULL};
static const IID *const chain_root_sig[] = {&IID_IUnknown, &IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12RootSignature, NULL};
static const IID *const chain_pso[] = {PAGEABLE, &IID_ID3D12PipelineState, NULL};
static const IID *const chain_cmd_sig[] = {PAGEABLE, &IID_ID3D12CommandSignature, NULL};
static const IID *const chain_heap[] = {PAGEABLE, &IID_ID3D12Heap, &IID_ID3D12Heap1, NULL};
static const IID *const chain_resource[] = {PAGEABLE, &IID_ID3D12Resource, &IID_ID3D12Resource1, &IID_ID3D12Resource2, NULL};

/* dxcore.c */
extern const IID *const dxcore_chain_factory[], *const dxcore_chain_list[], *const dxcore_chain_adapter[];

static const IID *const *const chains[] = {
   [K_FACTORY] = dxcore_chain_factory,
   [K_LIST] = dxcore_chain_list,
   [K_ADAPTER] = dxcore_chain_adapter,
   [K_BLOB] = chain_blob,
   [K_DEVICE] = chain_device,
   [K_QUEUE] = chain_queue,
   [K_ALLOCATOR] = chain_allocator,
   [K_CMDLIST] = chain_cmdlist,
   [K_FENCE] = chain_fence,
   [K_DESC_HEAP] = chain_desc_heap,
   [K_QUERY_HEAP] = chain_query_heap,
   [K_ROOT_SIG] = chain_root_sig,
   [K_PSO] = chain_pso,
   [K_CMD_SIG] = chain_cmd_sig,
   [K_HEAP] = chain_heap,
   [K_RESOURCE] = chain_resource,
};

static const char *const kind_names[] = {
   "IDXCoreAdapterFactory", "IDXCoreAdapterList", "IDXCoreAdapter", "ID3D10Blob", "ID3D12Device",
   "ID3D12CommandQueue", "ID3D12CommandAllocator", "ID3D12GraphicsCommandList", "ID3D12Fence",
   "ID3D12DescriptorHeap", "ID3D12QueryHeap", "ID3D12RootSignature", "ID3D12PipelineState",
   "ID3D12CommandSignature", "ID3D12Heap", "ID3D12Resource",
};

static bool in_chain(const IID *const *chain, REFIID riid)
{
   for (; *chain; chain++)
      if (!memcmp(*chain, riid, sizeof(IID)))
         return true;
   return false;
}

static bool in_versions(const IID *const *versions, uint32_t level, REFIID riid)
{
   for (uint32_t i = 0; i <= level; i++)
      if (!memcmp(versions[i], riid, sizeof(IID)))
         return true;
   return false;
}

HRESULT STDMETHODCALLTYPE obj_QueryInterface(void *This, REFIID riid, void **ppv)
{
   struct obj *o = This;
   bool ok = in_chain(chains[o->kind], riid);
   if (o->kind == K_DEVICE)
      ok = ok || in_versions(device_versions, ((struct device *)o)->level, riid);
   else if (o->kind == K_CMDLIST)
      ok = ok || in_versions(cmdlist_versions, ((struct cmdlist *)o)->level, riid);
   if (!ok) {
      *ppv = NULL;
      return E_NOINTERFACE;
   }
   obj_AddRef(o);
   *ppv = o;
   return S_OK;
}

ULONG STDMETHODCALLTYPE obj_AddRef(void *This)
{
   struct obj *o = This;
   return __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}

static void desc_heap_unregister(struct desc_heap *h);

static void obj_free(struct obj *o)
{
   switch (o->kind) {
   case K_DESC_HEAP:
      desc_heap_unregister((struct desc_heap *)o);
      break;
   case K_HEAP:
      shadow_destroy(((struct heap *)o)->shadow);
      break;
   case K_RESOURCE: {
      struct resource *r = (struct resource *)o;
      shadow_destroy(r->shadow);
      if (r->heap)
         obj_Release(r->heap);
      break;
   }
   default:
      break;
   }
   if (o->device)
      obj_Release(o->device);
   id_free(o->id);
   free(o);
}

ULONG STDMETHODCALLTYPE obj_Release(void *This)
{
   struct obj *o = This;
   ULONG refs = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
   if (refs)
      return refs;
   msg_begin(OP_RELEASE);
   w_u32(o->id);
   msg_send();
   if (o->kind == K_FENCE)
      fence_slot_free(((struct fence *)o)->slot);
   obj_free(o);
   return 0;
}

static HRESULT STDMETHODCALLTYPE obj_GetPrivateData(void *This, REFGUID guid, UINT *size, void *data)
{
   *size = 0;
   return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE obj_SetPrivateData(void *This, REFGUID guid, UINT size, const void *data)
{
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE obj_SetPrivateDataInterface(void *This, REFGUID guid, const IUnknown *data)
{
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE obj_SetName(void *This, LPCWSTR name)
{
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE obj_GetDevice(void *This, REFIID riid, void **ppv)
{
   return obj_QueryInterface(((struct obj *)This)->device, riid, ppv);
}

/* Unimplemented slots report themselves. Struct-returning methods must never
 * land here: on i386 their callee pops the hidden return pointer. */
static HRESULT trap(void *This, int slot)
{
   struct obj *o = This;
   shim_warn("%s method %d is not implemented", kind_names[o->kind], slot);
   return E_NOTIMPL;
}

#define T(n) static HRESULT STDMETHODCALLTYPE trap##n(void *This) { return trap(This, n); }
#define T10(n) T(n##0) T(n##1) T(n##2) T(n##3) T(n##4) T(n##5) T(n##6) T(n##7) T(n##8) T(n##9)
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9)
T10(1) T10(2) T10(3) T10(4) T10(5) T10(6) T10(7) T10(8) T10(9)
#define P10(n) trap##n##0, trap##n##1, trap##n##2, trap##n##3, trap##n##4, trap##n##5, trap##n##6, trap##n##7, trap##n##8, trap##n##9
static HRESULT (STDMETHODCALLTYPE *const traps[])(void *) = {
   trap0, trap1, trap2, trap3, trap4, trap5, trap6, trap7, trap8, trap9,
   P10(1), P10(2), P10(3), P10(4), P10(5), P10(6), P10(7), P10(8), P10(9),
};

/* Fills a vtable with traps, then the slots shared by every interface:
 * IUnknown, and for D3D12 objects ID3D12Object and ID3D12DeviceChild. */
void vtbl_init(void *vtbl, size_t size, bool d3d12_object)
{
   void **slots = vtbl;
   size_t n = size / sizeof(void *);
   if (n > ARRAY_SIZE(traps))
      abort();
   for (size_t i = 0; i < n; i++)
      slots[i] = (void *)traps[i];

   slots[0] = (void *)obj_QueryInterface;
   slots[1] = (void *)obj_AddRef;
   slots[2] = (void *)obj_Release;
   if (!d3d12_object)
      return;
   slots[3] = (void *)obj_GetPrivateData;
   slots[4] = (void *)obj_SetPrivateData;
   slots[5] = (void *)obj_SetPrivateDataInterface;
   slots[6] = (void *)obj_SetName;
   if (n > 7)
      slots[7] = (void *)obj_GetDevice;
}

/* ID3D10Blob */
struct blob {
   struct obj o;
   size_t size;
   char data[];
};

static ID3D10BlobVtbl blob_vtbl;

static void *STDMETHODCALLTYPE blob_GetBufferPointer(ID3D10Blob *This)
{
   return ((struct blob *)This)->data;
}

static SIZE_T STDMETHODCALLTYPE blob_GetBufferSize(ID3D10Blob *This)
{
   return ((struct blob *)This)->size;
}

HRESULT blob_create(const void *data, size_t size, ID3DBlob **out)
{
   struct blob *b = obj_new(sizeof(*b) + size, K_BLOB, &blob_vtbl, NULL);
   b->size = size;
   memcpy(b->data, data, size);
   *out = (ID3DBlob *)b;
   return S_OK;
}

/* A local blob has no server object, so Release must not reach the server. */
static ULONG STDMETHODCALLTYPE blob_Release(ID3D10Blob *This)
{
   struct obj *o = (struct obj *)This;
   ULONG refs = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
   if (!refs)
      free(o);
   return refs;
}

/* Descriptor heaps: the server's CPU handles are 64-bit addresses, so each heap
 * gets a reserved range in our address space and handles are translated by range. */
static pthread_rwlock_t heaps_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct heap_range {
   uintptr_t start, end;
   uint64_t server;
} *ranges;
static size_t nranges, ranges_cap;

void desc_heap_register(struct desc_heap *h, uint64_t server_cpu)
{
   size_t size = (size_t)h->desc.NumDescriptors * h->o.device->inc[h->desc.Type];
   if (!size)
      size = 1;
   void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
   if (p == MAP_FAILED) {
      shim_warn("cannot reserve %zu bytes for a descriptor heap", size);
      return;
   }
   h->cpu = (uintptr_t)p;
   h->size = size;

   pthread_rwlock_wrlock(&heaps_lock);
   if (nranges == ranges_cap) {
      ranges_cap = ranges_cap ? ranges_cap * 2 : 64;
      ranges = realloc(ranges, ranges_cap * sizeof(*ranges));
   }
   size_t i = nranges;
   while (i > 0 && ranges[i - 1].start > h->cpu) {
      ranges[i] = ranges[i - 1];
      i--;
   }
   ranges[i] = (struct heap_range){h->cpu, h->cpu + size, server_cpu};
   nranges++;
   pthread_rwlock_unlock(&heaps_lock);
}

static void desc_heap_unregister(struct desc_heap *h)
{
   if (!h->cpu)
      return;
   pthread_rwlock_wrlock(&heaps_lock);
   for (size_t i = 0; i < nranges; i++) {
      if (ranges[i].start == h->cpu) {
         memmove(&ranges[i], &ranges[i + 1], (nranges - i - 1) * sizeof(*ranges));
         nranges--;
         break;
      }
   }
   pthread_rwlock_unlock(&heaps_lock);
   munmap((void *)h->cpu, h->size);
}

uint64_t cpu_handle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
   uintptr_t p = handle.ptr;
   uint64_t result = 0;
   pthread_rwlock_rdlock(&heaps_lock);
   size_t lo = 0, hi = nranges;
   while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (p < ranges[mid].start)
         hi = mid;
      else if (p >= ranges[mid].end)
         lo = mid + 1;
      else {
         result = ranges[mid].server + (p - ranges[mid].start);
         break;
      }
   }
   pthread_rwlock_unlock(&heaps_lock);
   if (!result && p)
      shim_warn("unknown CPU descriptor handle %#zx", (size_t)p);
   return result;
}

ID3D12DescriptorHeapVtbl desc_heap_vtbl;

static D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE desc_heap_GetDesc(ID3D12DescriptorHeap *This)
{
   return ((struct desc_heap *)This)->desc;
}

static D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE desc_heap_GetCPUStart(ID3D12DescriptorHeap *This)
{
   return (D3D12_CPU_DESCRIPTOR_HANDLE){((struct desc_heap *)This)->cpu};
}

static D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE desc_heap_GetGPUStart(ID3D12DescriptorHeap *This)
{
   return (D3D12_GPU_DESCRIPTOR_HANDLE){((struct desc_heap *)This)->gpu};
}

enum cpu_access heap_cpu_access(const D3D12_HEAP_PROPERTIES *p)
{
   switch (p->Type) {
   case D3D12_HEAP_TYPE_UPLOAD:
   case D3D12_HEAP_TYPE_GPU_UPLOAD:
      return CPU_WRITE;
   case D3D12_HEAP_TYPE_READBACK:
      return CPU_READ_WRITE;
   case D3D12_HEAP_TYPE_CUSTOM:
      /* The GPU may write custom heaps, and the server maps them cached. */
      if (p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
          p->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK)
         return CPU_READ_WRITE;
      return CPU_NONE;
   default:
      return CPU_NONE;
   }
}

/* ID3D12Heap1 */
ID3D12Heap1Vtbl heap_vtbl;

static D3D12_HEAP_DESC STDMETHODCALLTYPE heap_GetDesc(ID3D12Heap1 *This)
{
   return ((struct heap *)This)->desc;
}

/* ID3D12Resource2 */
ID3D12Resource2Vtbl resource_vtbl;

static HRESULT STDMETHODCALLTYPE resource_Map(ID3D12Resource2 *This, UINT sub, const D3D12_RANGE *range, void **data)
{
   struct resource *r = (struct resource *)This;
   if (!r->map) {
      shim_warn("Map is only supported on buffers in CPU-visible heaps");
      return E_INVALIDARG;
   }
   if (data)
      *data = r->map;
   return S_OK;
}

static void STDMETHODCALLTYPE resource_Unmap(ID3D12Resource2 *This, UINT sub, const D3D12_RANGE *range)
{
}

static D3D12_RESOURCE_DESC STDMETHODCALLTYPE resource_GetDesc(ID3D12Resource2 *This)
{
   D3D12_RESOURCE_DESC d;
   memcpy(&d, &((struct resource *)This)->desc, sizeof(d));
   return d;
}

static D3D12_RESOURCE_DESC1 STDMETHODCALLTYPE resource_GetDesc1(ID3D12Resource2 *This)
{
   return ((struct resource *)This)->desc;
}

static D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE resource_GetGPUVirtualAddress(ID3D12Resource2 *This)
{
   return ((struct resource *)This)->va;
}

static HRESULT STDMETHODCALLTYPE resource_GetHeapProperties(ID3D12Resource2 *This, D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS *flags)
{
   struct resource *r = (struct resource *)This;
   if (props)
      *props = r->props;
   if (flags)
      *flags = r->flags;
   return S_OK;
}

/* ID3D12Fence1 */
ID3D12Fence1Vtbl fence_vtbl;

/* Completed fence values, written by the server after it has copied the GPU
 * writes they cover. One slot per fence, so reading a value needs no request. */
#define FENCE_SLOTS 4096
static uint64_t *fence_values;
static pthread_mutex_t slots_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_slot, free_slots[FENCE_SLOTS], nfree;

/* Returns a slot, or FENCE_NO_SLOT. Sets *fd to the table the first time, which
 * the caller sends to the server and then closes. */
uint32_t fence_slot_alloc(int *fd)
{
   uint32_t slot = FENCE_NO_SLOT;
   *fd = -1;
   pthread_mutex_lock(&slots_lock);
   if (!fence_values) {
      int table = memfd_create("d3d12-fences", MFD_CLOEXEC);
      void *p = MAP_FAILED;
      if (table >= 0 && !ftruncate(table, FENCE_SLOTS * sizeof(uint64_t)))
         p = mmap(NULL, FENCE_SLOTS * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_SHARED, table, 0);
      if (p != MAP_FAILED) {
         fence_values = p;
         *fd = table;
      } else if (table >= 0) {
         close(table);
      }
   }
   if (fence_values) {
      if (nfree)
         slot = free_slots[--nfree];
      else if (next_slot < FENCE_SLOTS)
         slot = next_slot++;
   }
   pthread_mutex_unlock(&slots_lock);
   return slot;
}

/* Called once the release of the slot's fence has been sent, so the server
 * stops writing the slot before a new fence gets it. */
void fence_slot_free(uint32_t slot)
{
   if (slot == FENCE_NO_SLOT)
      return;
   pthread_mutex_lock(&slots_lock);
   free_slots[nfree++] = slot;
   pthread_mutex_unlock(&slots_lock);
}

static UINT64 STDMETHODCALLTYPE fence_GetCompletedValue(ID3D12Fence1 *This)
{
   struct fence *f = (struct fence *)This;
   /* A removed device reports every fence as complete. */
   if (server_lost())
      return UINT64_MAX;
   if (f->slot != FENCE_NO_SLOT)
      return __atomic_load_n(&fence_values[f->slot], __ATOMIC_ACQUIRE);
   msg_begin(OP_FENCE_COMPLETED);
   w_obj(This);
   uint64_t v = msg_call() ? r_u64() : UINT64_MAX;
   msg_end();
   return v;
}

static HRESULT STDMETHODCALLTYPE fence_SetEventOnCompletion(ID3D12Fence1 *This, UINT64 value, HANDLE event)
{
   struct fence *f = (struct fence *)This;
   if (f->slot != FENCE_NO_SLOT && value <= __atomic_load_n(&fence_values[f->slot], __ATOMIC_ACQUIRE)) {
      if (event)
         eventfd_write((int)(intptr_t)event, 1);
      return S_OK;
   }
   /* The server signals the event when the fence reaches the value, or at
    * once if it cannot watch the fence. */
   int fd = event ? (int)(intptr_t)event : eventfd(0, EFD_CLOEXEC);
   msg_begin(OP_FENCE_SET_EVENT);
   w_obj(This);
   w_u64(value);
   msg_fd(fd);
   msg_send_flush();
   if (event) {
      if (server_lost())
         eventfd_write(fd, 1);
   } else {
      msg_wait_fd(fd);
      close(fd);
   }
   return S_OK;
}

/* Waits for the server, which publishes the new value before replying. */
static HRESULT STDMETHODCALLTYPE fence_Signal(ID3D12Fence1 *This, UINT64 value)
{
   msg_begin(OP_FENCE_SIGNAL);
   w_obj(This);
   w_u64(value);
   msg_call();
   msg_end();
   return S_OK;
}

static D3D12_FENCE_FLAGS STDMETHODCALLTYPE fence_GetCreationFlags(ID3D12Fence1 *This)
{
   return ((struct fence *)This)->flags;
}

/* ID3D12CommandQueue */
ID3D12CommandQueueVtbl queue_vtbl;

static void STDMETHODCALLTYPE queue_ExecuteCommandLists(ID3D12CommandQueue *This, UINT n, ID3D12CommandList *const *lists)
{
   msg_begin(OP_QUEUE_EXECUTE);
   w_obj(This);
   shadow_write_dirty();
   w_u32(n);
   for (UINT i = 0; i < n; i++)
      w_obj(lists[i]);
   msg_send_flush();
}

static void STDMETHODCALLTYPE queue_SetMarker(ID3D12CommandQueue *This, UINT meta, const void *data, UINT size)
{
}

static void STDMETHODCALLTYPE queue_EndEvent(ID3D12CommandQueue *This)
{
}

static HRESULT queue_fence_op(uint32_t op, ID3D12CommandQueue *This, ID3D12Fence *fence, UINT64 value)
{
   msg_begin(op);
   w_obj(This);
   w_obj(fence);
   w_u64(value);
   msg_send();
   return S_OK;
}

static HRESULT STDMETHODCALLTYPE queue_Signal(ID3D12CommandQueue *This, ID3D12Fence *fence, UINT64 value)
{
   return queue_fence_op(OP_QUEUE_SIGNAL, This, fence, value);
}

static HRESULT STDMETHODCALLTYPE queue_Wait(ID3D12CommandQueue *This, ID3D12Fence *fence, UINT64 value)
{
   return queue_fence_op(OP_QUEUE_WAIT, This, fence, value);
}

static HRESULT STDMETHODCALLTYPE queue_GetTimestampFrequency(ID3D12CommandQueue *This, UINT64 *freq)
{
   msg_begin(OP_QUEUE_TIMESTAMP_FREQ);
   w_obj(This);
   msg_call();
   HRESULT hr = r_hr();
   *freq = r_u64();
   msg_end();
   return hr;
}

static HRESULT STDMETHODCALLTYPE queue_GetClockCalibration(ID3D12CommandQueue *This, UINT64 *gpu, UINT64 *cpu)
{
   msg_begin(OP_QUEUE_CLOCK_CALIBRATION);
   w_obj(This);
   msg_call();
   HRESULT hr = r_hr();
   *gpu = r_u64();
   *cpu = r_u64();
   msg_end();
   return hr;
}

static D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE queue_GetDesc(ID3D12CommandQueue *This)
{
   return ((struct queue *)This)->desc;
}

/* ID3D12CommandAllocator */
ID3D12CommandAllocatorVtbl allocator_vtbl;

static HRESULT STDMETHODCALLTYPE allocator_Reset(ID3D12CommandAllocator *This)
{
   msg_begin(OP_ALLOCATOR_RESET);
   w_obj(This);
   msg_send();
   return S_OK;
}

ID3D12QueryHeapVtbl query_heap_vtbl;
ID3D12RootSignatureVtbl root_sig_vtbl;
ID3D12PipelineStateVtbl pso_vtbl;
ID3D12CommandSignatureVtbl cmd_sig_vtbl;

void objects_init(void)
{
   vtbl_init(&blob_vtbl, sizeof(blob_vtbl), false);
   blob_vtbl.Release = blob_Release;
   blob_vtbl.GetBufferPointer = blob_GetBufferPointer;
   blob_vtbl.GetBufferSize = blob_GetBufferSize;

   vtbl_init(&desc_heap_vtbl, sizeof(desc_heap_vtbl), true);
   desc_heap_vtbl.GetDesc = desc_heap_GetDesc;
   desc_heap_vtbl.GetCPUDescriptorHandleForHeapStart = desc_heap_GetCPUStart;
   desc_heap_vtbl.GetGPUDescriptorHandleForHeapStart = desc_heap_GetGPUStart;

   vtbl_init(&heap_vtbl, sizeof(heap_vtbl), true);
   heap_vtbl.GetDesc = heap_GetDesc;

   vtbl_init(&resource_vtbl, sizeof(resource_vtbl), true);
   resource_vtbl.Map = resource_Map;
   resource_vtbl.Unmap = resource_Unmap;
   resource_vtbl.GetDesc = resource_GetDesc;
   resource_vtbl.GetDesc1 = resource_GetDesc1;
   resource_vtbl.GetGPUVirtualAddress = resource_GetGPUVirtualAddress;
   resource_vtbl.GetHeapProperties = resource_GetHeapProperties;

   vtbl_init(&fence_vtbl, sizeof(fence_vtbl), true);
   fence_vtbl.GetCompletedValue = fence_GetCompletedValue;
   fence_vtbl.SetEventOnCompletion = fence_SetEventOnCompletion;
   fence_vtbl.Signal = fence_Signal;
   fence_vtbl.GetCreationFlags = fence_GetCreationFlags;

   vtbl_init(&queue_vtbl, sizeof(queue_vtbl), true);
   queue_vtbl.ExecuteCommandLists = queue_ExecuteCommandLists;
   queue_vtbl.SetMarker = queue_SetMarker;
   queue_vtbl.BeginEvent = queue_SetMarker;
   queue_vtbl.EndEvent = queue_EndEvent;
   queue_vtbl.Signal = queue_Signal;
   queue_vtbl.Wait = queue_Wait;
   queue_vtbl.GetTimestampFrequency = queue_GetTimestampFrequency;
   queue_vtbl.GetClockCalibration = queue_GetClockCalibration;
   queue_vtbl.GetDesc = queue_GetDesc;

   vtbl_init(&allocator_vtbl, sizeof(allocator_vtbl), true);
   allocator_vtbl.Reset = allocator_Reset;

   vtbl_init(&query_heap_vtbl, sizeof(query_heap_vtbl), true);
   vtbl_init(&root_sig_vtbl, sizeof(root_sig_vtbl), true);
   vtbl_init(&pso_vtbl, sizeof(pso_vtbl), true);
   vtbl_init(&cmd_sig_vtbl, sizeof(cmd_sig_vtbl), true);
}
