#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <wsl/winadapter.h>
#include <directx/d3d12.h>

#include "../proto.h"

#define EXPORT __attribute__((visibility("default")))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Assigns a method whose This type differs from the vtable's. */
#define SET(vt, method, fn) ((vt).method = (__typeof__((vt).method))(fn))

/* wire.c: one request at a time, guarded by a global lock taken in msg_begin. */
void msg_begin(uint32_t op);
void msg_send(void);  /* queue a request without a reply */
void msg_send_flush(void);  /* send it and everything queued before it */
bool msg_call(void);  /* flush and wait for the reply, read it with r_*() */
/* msg_call() for requests with a fixed answer; ttl_ms 0 caches it for good */
bool msg_call_cached(unsigned ttl_ms);
void msg_end(void);   /* release the lock after reading a reply */
void msg_fd(int fd);  /* pass an fd along with the current request */
bool server_lost(void);

void w_blob(const void *p, size_t size);

void r_align(unsigned align);
uint32_t r_u32(void);
uint64_t r_u64(void);
HRESULT r_hr(void);
void r_raw(void *p, size_t size, unsigned align);
size_t r_blob(void *p, size_t max);
const void *r_blob_ptr(size_t *size);

/* objects.c */
enum kind {
   K_FACTORY, K_LIST, K_ADAPTER, K_BLOB, K_DEVICE, K_QUEUE, K_ALLOCATOR, K_CMDLIST, K_FENCE,
   K_DESC_HEAP, K_QUERY_HEAP, K_ROOT_SIG, K_PSO, K_CMD_SIG, K_HEAP, K_RESOURCE,
};

struct obj {
   const void *vtbl;
   uint32_t refs;
   uint32_t id;
   enum kind kind;
   struct device *device;
};

struct shadow;

struct device {
   struct obj o;
   uint32_t level, nodes;
   UINT inc[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
   LUID luid;
};

struct heap {
   struct obj o;
   D3D12_HEAP_DESC desc;
   struct shadow *shadow;
};

struct resource {
   struct obj o;
   D3D12_RESOURCE_DESC1 desc;
   D3D12_HEAP_PROPERTIES props;
   D3D12_HEAP_FLAGS flags;
   uint64_t va;
   char *map;
   struct heap *heap;
   struct shadow *shadow;
};

struct desc_heap {
   struct obj o;
   D3D12_DESCRIPTOR_HEAP_DESC desc;
   uintptr_t cpu;
   size_t size;
   uint64_t gpu;
};

struct queue {
   struct obj o;
   D3D12_COMMAND_QUEUE_DESC desc;
};

#define ROOT_PARAMS 64 /* a root signature holds at most 64 DWORDs */

struct root_args {
   uint64_t table_valid;
   uint64_t table[ROOT_PARAMS];
   uint64_t const_valid[ROOT_PARAMS]; /* bit per DWORD */
   uint32_t constants[ROOT_PARAMS][ROOT_PARAMS];
};

/* Recorded state used to drop redundant commands. A clear valid bit means
 * the slot's value is unknown and the next set is always sent. */
struct cmdlist {
   struct obj o;
   D3D12_COMMAND_LIST_TYPE type;
   uint32_t level;
   uint32_t vb_valid;
   D3D12_VERTEX_BUFFER_VIEW vb[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
   struct root_args root[2]; /* graphics, compute */
};

struct fence {
   struct obj o;
   D3D12_FENCE_FLAGS flags;
   uint32_t slot;
};

#define FENCE_NO_SLOT UINT32_MAX
uint32_t fence_slot_alloc(int *fd);
void fence_slot_free(uint32_t slot);

void *obj_new(size_t size, enum kind kind, const void *vtbl, struct device *device);
HRESULT obj_finish(void *o, HRESULT hr, REFIID riid, void **ppv);
uint32_t obj_id(const void *iface);

HRESULT STDMETHODCALLTYPE obj_QueryInterface(void *This, REFIID riid, void **ppv);
ULONG STDMETHODCALLTYPE obj_AddRef(void *This);
ULONG STDMETHODCALLTYPE obj_Release(void *This);
void vtbl_init(void *vtbl, size_t size, bool d3d12_object);

HRESULT blob_create(const void *data, size_t size, ID3DBlob **out);

uint64_t cpu_handle(D3D12_CPU_DESCRIPTOR_HANDLE h);

/* cpu_access class for a heap, see proto.h */
enum cpu_access heap_cpu_access(const D3D12_HEAP_PROPERTIES *p);

/* device.c */
HRESULT device_create(uint32_t adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **ppv);

/* objects.c vtables used by device.c */
extern ID3D12CommandQueueVtbl queue_vtbl;
extern ID3D12CommandAllocatorVtbl allocator_vtbl;
extern ID3D12Fence1Vtbl fence_vtbl;
extern ID3D12DescriptorHeapVtbl desc_heap_vtbl;
extern ID3D12QueryHeapVtbl query_heap_vtbl;
extern ID3D12RootSignatureVtbl root_sig_vtbl;
extern ID3D12PipelineStateVtbl pso_vtbl;
extern ID3D12CommandSignatureVtbl cmd_sig_vtbl;
extern ID3D12Heap1Vtbl heap_vtbl;
extern ID3D12Resource2Vtbl resource_vtbl;
void objects_init(void);
void desc_heap_register(struct desc_heap *h, uint64_t server_cpu);

/* cmdlist.c */
extern ID3D12GraphicsCommandList10Vtbl cmdlist_vtbl;
void cmdlist_init(void);

/* shadow.c: client side of CPU-visible memory, mirrored by the server */
struct shadow {
   struct shadow *next;
   uint32_t owner;
   enum cpu_access access;
   char *ptr;
   size_t size;
   uint64_t offset;  /* in the memfd passed with it */
};

struct shadow *shadow_create(size_t size, enum cpu_access access, int *fd);
void shadow_attach(struct shadow *s, uint32_t owner);
void shadow_destroy(struct shadow *s);
void shadow_write_dirty(void);

/* root signature and pipeline serialization, device.c */
void w_root_sig_desc(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc);
void w_resource_desc1(const D3D12_RESOURCE_DESC1 *d);
void w_resource_desc(const D3D12_RESOURCE_DESC *d);
void r_resource_desc1(D3D12_RESOURCE_DESC1 *d);

void shim_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Request writers, inline because command recording calls them for every
 * field. Messages start 8-aligned, so payload offsets align like wire_len. */
extern char *wire_out;
extern size_t wire_len, wire_cap;
void wire_grow(size_t need);

static inline void *w_reserve(size_t size, unsigned align)
{
   size_t pos = (wire_len + align - 1) & ~(size_t)(align - 1);
   if (pos + size > wire_cap)
      wire_grow(pos + size);
   memset(wire_out + wire_len, 0, pos - wire_len);
   wire_len = pos + size;
   return wire_out + pos;
}

static inline void w_align(unsigned align) { w_reserve(0, align); }
static inline void w_raw(const void *p, size_t size, unsigned align) { memcpy(w_reserve(size, align), p, size); }
static inline void w_u16(uint16_t v) { memcpy(w_reserve(2, 2), &v, 2); }
static inline void w_u32(uint32_t v) { memcpy(w_reserve(4, 4), &v, 4); }
static inline void w_u64(uint64_t v) { memcpy(w_reserve(8, 8), &v, 8); }
static inline void w_f32(float v) { memcpy(w_reserve(4, 4), &v, 4); }
static inline void w_obj(const void *iface) { w_u32(iface ? ((const struct obj *)iface)->id : 0); }
