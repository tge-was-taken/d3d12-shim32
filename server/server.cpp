// Owns the real D3D12 objects for one 32-bit client process and runs its
// requests. The client talks to it over a socket on fd SHIM_SERVER_FD.
#define INITGUID
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>

#include <dlfcn.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../proto.h"

static void warn(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   fputs("d3d12-shim-server: ", stderr);
   vfprintf(stderr, fmt, ap);
   fputc('\n', stderr);
   va_end(ap);
}

// Wire format, see proto.h. Offsets are relative to the payload start.
struct Reader {
   const char *base;
   size_t size, pos = 0;

   void align(size_t a) { pos = (pos + a - 1) / a * a; }
   const void *raw(size_t n, size_t a)
   {
      align(a);
      if (pos + n > size)
         throw std::runtime_error("truncated request");
      const void *p = base + pos;
      pos += n;
      return p;
   }
   template <class T> T get()
   {
      T v;
      memcpy(&v, raw(sizeof(T), alignof(T)), sizeof(T));
      return v;
   }
   uint32_t u32() { return get<uint32_t>(); }
   uint64_t u64() { return get<uint64_t>(); }
   float f32() { return get<float>(); }
   template <class T> const T *array(size_t n, size_t a = 4) { return static_cast<const T *>(raw(n * sizeof(T), a)); }
   std::pair<const void *, size_t> blob()
   {
      size_t n = u32();
      return {raw(n, 8), n};
   }
};

struct Writer {
   std::vector<char> buf;

   void align(size_t a) { buf.resize((buf.size() + a - 1) / a * a); }
   void raw(const void *p, size_t n, size_t a)
   {
      align(a);
      size_t o = buf.size();
      buf.resize(o + n);
      if (n)
         memcpy(&buf[o], p, n);
   }
   template <class T> void put(const T &v) { raw(&v, sizeof(T), alignof(T)); }
   void u32(uint32_t v) { put(v); }
   void u64(uint64_t v) { put(v); }
   void blob(const void *p, size_t n)
   {
      u32(n);
      raw(p, n, 8);
   }
};

// CPU-visible memory. `shadow` is the client's memfd and `real` the D3D12
// mapping. GPU writes the server cannot locate exactly (views, stream output)
// are found by comparing with `last`, a copy that is only kept when needed.
struct Shadow {
   uint64_t serial = 0;
   char *shadow = nullptr, *real = nullptr;
   std::vector<char> last;
   bool compare_always = false;
   size_t size = 0, map_size = 0;
   uint32_t access = CPU_NONE;
   ID3D12Resource *mapped = nullptr;
   bool owns_mapped = false;
};

// A GPU write into a shadow. `compare` means the range is unknown.
struct Write {
   uint64_t shadow;
   size_t start, end;
   bool compare;
};

struct Submission {
   uint64_t value;
   std::vector<Write> writes;
   std::vector<IUnknown *> refs;  // released once the GPU is done
};

// Private fence signalled after each submit. Writes recorded by the submitted
// lists are copied to the client when it passes their value.
struct Queue {
   ID3D12Fence *fence = nullptr;
   uint64_t submitted = 0, seen = 0;
   std::deque<Submission> pending;
};

struct Object {
   IUnknown *p = nullptr;
   uint32_t level = 0;
   Shadow *shadow = nullptr;  // owned
   Queue *queue = nullptr;
   // Resources: the shadow holding the resource's memory, and for buffers
   // their size and GPU address.
   Shadow *backing = nullptr;
   uint64_t offset = 0, size = 0, va = 0;
   // Command lists: GPU writes into shadows since the last Reset, and the
   // buffers that flipped row copies go through (see flip_flush). Replaced
   // buffers are kept until the next Reset.
   std::vector<Write> writes;
   ID3D12Resource *flip_buf[2] = {};
   D3D12_RESOURCE_STATES flip_state[2] = {};
   uint64_t flip_size = 0;
   std::vector<ID3D12Resource *> flip_old;
   // Devices: one CPU-only descriptor that CBVs are built in, see create_view.
   ID3D12DescriptorHeap *cbv_scratch = nullptr;
};

// Indexed by id. The client reuses ids, so this stays as large as the most
// objects alive at once; a deque keeps references valid as it grows.
static std::deque<Object> objects;
static std::deque<int> fds;

static std::mutex sync_lock;
static std::unordered_map<uint64_t, Shadow *> shadows;
static std::vector<Queue *> queues;
static std::map<uint64_t, uint32_t> buffers_by_va;

static decltype(&D3D12CreateDevice) create_device;
static decltype(&D3D12SerializeVersionedRootSignature) serialize_root_signature;
static decltype(&D3D12EnableExperimentalFeatures) enable_experimental;
static HRESULT (*create_dxcore_factory)(REFIID, void **);

static Object *find(uint32_t id)
{
   if (!id)
      return nullptr;
   if (id >= objects.size() || !objects[id].p) {
      warn("unknown object %u", id);
      return nullptr;
   }
   return &objects[id];
}

template <class T = IUnknown> static T *lookup(uint32_t id)
{
   Object *o = find(id);
   return o ? static_cast<T *>(o->p) : nullptr;
}

static bool need(const Object *o, uint32_t level, const char *what)
{
   if (o && o->level >= level)
      return true;
   warn("%s needs interface version %u", what, level);
   return false;
}

static int take_fd()
{
   if (fds.empty())
      throw std::runtime_error("expected a file descriptor");
   int fd = fds.front();
   fds.pop_front();
   return fd;
}

// QueryInterface for the newest of `iids` and replaces p with it.
static IUnknown *newest(IUnknown *p, const IID *const *iids, uint32_t n, uint32_t *level)
{
   for (uint32_t i = n; i-- > 1;) {
      IUnknown *q;
      if (SUCCEEDED(p->QueryInterface(*iids[i], (void **)&q))) {
         p->Release();
         *level = i;
         return q;
      }
   }
   *level = 0;
   return p;
}

static const IID *const device_iids[] = {
   &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4,
   &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9,
   &IID_ID3D12Device10, &IID_ID3D12Device11, &IID_ID3D12Device12, &IID_ID3D12Device13, &IID_ID3D12Device14,
};
static const IID *const cmdlist_iids[] = {
   &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1, &IID_ID3D12GraphicsCommandList2,
   &IID_ID3D12GraphicsCommandList3, &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5,
   &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7, &IID_ID3D12GraphicsCommandList8,
   &IID_ID3D12GraphicsCommandList9, &IID_ID3D12GraphicsCommandList10,
};

static void add(uint32_t id, IUnknown *p, uint32_t level = 0)
{
   if (id >= objects.size())
      objects.resize(id + 1);
   Object &o = objects[id];
   if (o.p)
      warn("object id %u reused", id);
   o.p = p;
   o.level = level;
}

/* Shadow memory */

static Shadow *shadow_create(int fd, uint64_t offset, size_t size, uint32_t access, ID3D12Resource *mapped, bool owns)
{
   auto *s = new Shadow;
   s->size = size;
   s->map_size = (size + 4095) & ~size_t(4095);
   s->access = access;
   s->mapped = mapped;
   s->owns_mapped = owns;
   void *p = mmap(nullptr, s->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
   close(fd);
   D3D12_RANGE none = {0, 0};
   void *real = nullptr;
   if (p == MAP_FAILED || FAILED(mapped->Map(0, access == CPU_WRITE ? &none : nullptr, &real))) {
      warn("cannot map shadow memory");
      if (p != MAP_FAILED)
         munmap(p, s->map_size);
      if (owns)
         mapped->Release();
      delete s;
      return nullptr;
   }
   static uint64_t serial;
   s->serial = ++serial;
   s->shadow = static_cast<char *>(p);
   s->real = static_cast<char *>(real);
   if (access == CPU_READ_WRITE)
      memcpy(s->shadow, s->real, size);
   std::lock_guard<std::mutex> g(sync_lock);
   shadows[s->serial] = s;
   return s;
}

static void shadow_destroy(Shadow *s)
{
   if (!s)
      return;
   {
      std::lock_guard<std::mutex> g(sync_lock);
      shadows.erase(s->serial);
   }
   s->mapped->Unmap(0, nullptr);
   if (s->owns_mapped)
      s->mapped->Release();
   munmap(s->shadow, s->map_size);
   delete s;
}

// Copies the pages the client wrote into the real mapping.
static void push(Reader &r)
{
   std::lock_guard<std::mutex> g(sync_lock);
   uint32_t count = r.u32();
   for (uint32_t i = 0; i < count; i++) {
      Object *o = find(r.u32());
      uint32_t n = r.u32();
      for (uint32_t j = 0; j < n; j++) {
         size_t start = r.u32(), end = r.u32();
         Shadow *s = o ? o->shadow : nullptr;
         if (!s)
            continue;
         end = std::min(end, s->size);
         if (start >= end)
            continue;
         memcpy(s->real + start, s->shadow + start, end - start);
         if (!s->last.empty())
            memcpy(&s->last[start], s->shadow + start, end - start);
      }
   }
}

// Starts keeping the copy that GPU writes are found against. Called before
// the GPU work that needs it is submitted.
static void need_compare(Shadow *s, bool always)
{
   std::lock_guard<std::mutex> g(sync_lock);
   if (s->last.empty())
      s->last.assign(s->real, s->real + s->size);
   s->compare_always |= always;
}

// Large copies, like a frame read back for display, are limited by memory
// bandwidth, which one thread does not use up.
static void copy(char *dst, const char *src, size_t size)
{
   const size_t threads = 4;
   if (size < (4u << 20)) {
      memcpy(dst, src, size);
      return;
   }
   size_t chunk = (size / threads + 4095) & ~size_t(4095);
   std::thread helpers[threads - 1];
   for (size_t i = 1; i < threads; i++) {
      size_t at = std::min(i * chunk, size);
      helpers[i - 1] = std::thread(memcpy, dst + at, src + at, std::min(chunk, size - at));
   }
   memcpy(dst, src, std::min(chunk, size));
   for (auto &t : helpers)
      t.join();
}

// Copies pages the GPU changed since the last sync into the client's memory.
static void pull(Shadow *s)
{
   for (size_t off = 0; off < s->size; off += 4096) {
      size_t n = std::min<size_t>(4096, s->size - off);
      if (memcmp(s->real + off, &s->last[off], n)) {
         memcpy(&s->last[off], s->real + off, n);
         memcpy(s->shadow + off, &s->last[off], n);
      }
   }
}

static void pull_range(Shadow *s, size_t start, size_t end)
{
   end = std::min(end, s->size);
   if (start >= end)
      return;
   copy(s->shadow + start, s->real + start, end - start);
   if (!s->last.empty())
      memcpy(&s->last[start], s->real + start, end - start);
}

static void sync_locked()
{
   bool advanced = false;
   std::vector<Shadow *> compare;
   for (Queue *q : queues) {
      uint64_t v = q->fence->GetCompletedValue();
      if (v == q->seen)
         continue;
      q->seen = v;
      advanced = true;
      while (!q->pending.empty() && q->pending.front().value <= v) {
         for (const Write &w : q->pending.front().writes) {
            auto it = shadows.find(w.shadow);
            if (it == shadows.end())
               continue;
            if (w.compare)
               compare.push_back(it->second);
            else
               pull_range(it->second, w.start, w.end);
         }
         for (IUnknown *p : q->pending.front().refs)
            p->Release();
         q->pending.pop_front();
      }
   }
   if (!advanced)
      return;
   for (auto &[serial, s] : shadows)
      if (s->compare_always)
         compare.push_back(s);
   std::sort(compare.begin(), compare.end());
   compare.erase(std::unique(compare.begin(), compare.end()), compare.end());
   for (Shadow *s : compare)
      pull(s);
}

/* Completed values of the client's fences, shared with the client. A value is
 * read before the sync and written after it, so the memory it covers is in
 * place by the time the client sees it. */

#define FENCE_SLOTS 4096
static uint64_t *fence_values;

struct Published {
   ID3D12Fence *fence;
   uint32_t slot;
};
static std::unordered_map<uint32_t, Published> published;

static void sync_from_gpu()
{
   std::lock_guard<std::mutex> g(sync_lock);
   std::vector<uint64_t> values;
   values.reserve(published.size());
   for (auto &[id, p] : published)
      values.push_back(p.fence->GetCompletedValue());
   sync_locked();
   size_t i = 0;
   for (auto &[id, p] : published)
      __atomic_store_n(&fence_values[p.slot], values[i++], __ATOMIC_RELEASE);
}

/* GPU write tracking for command lists */

// Records that `list` writes `size` bytes at `start` in resource `res`; a size
// of 0 means the range is unknown.
static void wrote(Object *list, const Object *res, uint64_t start = 0, uint64_t size = 0)
{
   if (!res || !res->backing)
      return;
   Shadow *s = res->backing;
   if (res->size)
      size = std::min(size, res->size - std::min(start, res->size));
   if (!size)
      need_compare(s, false);
   size_t begin = res->offset + start;
   list->writes.push_back({s->serial, begin, begin + size, size == 0});
}

static void wrote(Object *list, uint32_t res, uint64_t start = 0, uint64_t size = 0)
{
   if (res)
      wrote(list, find(res), start, size);
}

static void wrote_va(Object *list, uint64_t va, uint64_t size = 0)
{
   auto it = buffers_by_va.upper_bound(va);
   if (it == buffers_by_va.begin())
      return;
   Object *res = find((--it)->second);
   if (res && va < res->va + res->size)
      wrote(list, res, va - res->va, size);
}

/* Fence events: D3D12 signals our eventfd, then the waiter thread syncs
 * memory before it signals the client's. */

static int epoll_fd;

struct Waiter {
   int event, client;
};

static void wait_loop()
{
   epoll_event ev[16];
   for (;;) {
      int n = epoll_wait(epoll_fd, ev, 16, -1);
      if (n > 0)
         sync_from_gpu();
      for (int i = 0; i < n; i++) {
         auto *w = static_cast<Waiter *>(ev[i].data.ptr);
         uint64_t one = 1;
         if (w->client >= 0 && write(w->client, &one, sizeof(one)) < 0)
            warn("cannot signal client event");
         if (w->client >= 0)
            close(w->client);
         // D3D12 may still hold a dup of the eventfd, which keeps it registered.
         epoll_ctl(epoll_fd, EPOLL_CTL_DEL, w->event, nullptr);
         close(w->event);
         delete w;
      }
   }
}

template <class F> static HRESULT watch(int client, F set_event)
{
   int event = eventfd(0, EFD_CLOEXEC);
   HRESULT hr = set_event((HANDLE)(intptr_t)event);
   if (FAILED(hr)) {
      close(event);
      if (client >= 0) {
         // Wake the client rather than leave it waiting forever.
         uint64_t one = 1;
         if (write(client, &one, sizeof(one)) < 0)
            warn("cannot signal client event");
         close(client);
      }
      return hr;
   }
   epoll_event ev = {};
   ev.events = EPOLLIN | EPOLLONESHOT;
   ev.data.ptr = new Waiter{event, client};
   epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event, &ev);
   return hr;
}

/* SetEventOnCompletion takes about 70 us, so a thread makes those calls and
 * the request thread only queues them. */

struct FenceWatch {
   ID3D12Fence *fence;  // referenced
   uint64_t value;
   int client;  // eventfd to signal, or -1
};

// Never destroyed: at exit the condition variable's destructor would wait for
// the thread blocked on it.
struct WatchQueue {
   std::mutex lock;
   std::condition_variable cond;
   std::deque<FenceWatch> items;
};
static WatchQueue &watches = *new WatchQueue;

static void watch_loop()
{
   for (;;) {
      std::unique_lock<std::mutex> g(watches.lock);
      watches.cond.wait(g, [] { return !watches.items.empty(); });
      FenceWatch fw = watches.items.front();
      watches.items.pop_front();
      g.unlock();
      HRESULT hr = watch(fw.client, [&](HANDLE e) { return fw.fence->SetEventOnCompletion(fw.value, e); });
      if (FAILED(hr))
         warn("SetEventOnCompletion failed: %#x", (unsigned)hr);
      fw.fence->Release();
   }
}

static void watch_later(ID3D12Fence *fence, uint64_t value, int client)
{
   fence->AddRef();
   std::lock_guard<std::mutex> g(watches.lock);
   watches.items.push_back({fence, value, client});
   watches.cond.notify_one();
}

// Publishes the fence's value once it reaches `value`.
static void watch_fence(uint32_t id, ID3D12Fence *fence, uint64_t value)
{
   {
      std::lock_guard<std::mutex> g(sync_lock);
      if (!published.count(id))
         return;
   }
   watch_later(fence, value, -1);
}

/* Root signatures and pipelines */

static void serialize_root_sig(Reader &r, Writer &w)
{
   auto version = (D3D_ROOT_SIGNATURE_VERSION)r.u32();
   uint32_t n = r.u32();
   size_t range_size = version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? sizeof(D3D12_DESCRIPTOR_RANGE) : sizeof(D3D12_DESCRIPTOR_RANGE1);
   size_t sampler_size = version == D3D_ROOT_SIGNATURE_VERSION_1_2 ? sizeof(D3D12_STATIC_SAMPLER_DESC1) : sizeof(D3D12_STATIC_SAMPLER_DESC);
   std::vector<D3D12_ROOT_PARAMETER1> params(n);
   for (auto &p : params) {
      p.ParameterType = (D3D12_ROOT_PARAMETER_TYPE)r.u32();
      p.ShaderVisibility = (D3D12_SHADER_VISIBILITY)r.u32();
      if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
         p.DescriptorTable.NumDescriptorRanges = r.u32();
         p.DescriptorTable.pDescriptorRanges = static_cast<const D3D12_DESCRIPTOR_RANGE1 *>(
            r.raw(p.DescriptorTable.NumDescriptorRanges * range_size, 4));
      } else {
         memcpy(&p.Constants, r.raw(12, 4), 12);
      }
   }
   uint32_t nsamplers = r.u32();
   const void *samplers = r.raw(nsamplers * sampler_size, 4);
   auto flags = (D3D12_ROOT_SIGNATURE_FLAGS)r.u32();

   // The 1.0, 1.1 and 1.2 descs share one layout up to the sampler type.
   D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
   desc.Version = version;
   desc.Desc_1_1 = {n, params.data(), nsamplers, static_cast<const D3D12_STATIC_SAMPLER_DESC *>(samplers), flags};
   ID3DBlob *blob = nullptr, *error = nullptr;
   HRESULT hr = serialize_root_signature(&desc, &blob, &error);
   w.put(hr);
   w.blob(blob ? blob->GetBufferPointer() : nullptr, blob ? blob->GetBufferSize() : 0);
   w.blob(error ? error->GetBufferPointer() : nullptr, error ? error->GetBufferSize() : 0);
   if (blob)
      blob->Release();
   if (error)
      error->Release();
}

// Builds a pipeline state stream in the x86-64 layout: each subobject is
// pointer aligned, with its inner struct at the inner struct's alignment.
struct Stream {
   std::vector<char> data;

   void add(uint32_t type, const void *inner, size_t size, size_t align)
   {
      size_t pos = (data.size() + 7) & ~size_t(7);
      size_t inner_pos = (pos + 4 + align - 1) / align * align;
      data.resize(inner_pos + size);
      memcpy(&data[pos], &type, 4);
      memcpy(&data[inner_pos], inner, size);
   }
   template <class T> void add(uint32_t type, const T &inner) { add(type, &inner, sizeof(T), alignof(T)); }
};

static const char *string(Reader &r)
{
   auto [p, n] = r.blob();
   return n ? static_cast<const char *>(p) : nullptr;
}

static void create_pso(Reader &r, Writer &w)
{
   Object *dev = find(r.u32());
   uint32_t id = r.u32();
   Stream s;
   std::vector<D3D12_SO_DECLARATION_ENTRY> so;
   std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
   for (;;) {
      uint32_t type = r.u32();
      if (type == 0xffffffff)
         break;
      switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
         s.add(type, lookup<ID3D12RootSignature>(r.u32()));
         break;
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
         auto [p, n] = r.blob();
         s.add(type, D3D12_SHADER_BYTECODE{p, n});
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
         so.resize(r.u32());
         for (auto &e : so) {
            e.Stream = r.u32();
            e.SemanticName = string(r);
            e.SemanticIndex = r.u32();
            uint32_t packed = r.u32();
            e.StartComponent = packed & 0xff;
            e.ComponentCount = (packed >> 8) & 0xff;
            e.OutputSlot = (packed >> 16) & 0xff;
         }
         D3D12_STREAM_OUTPUT_DESC d = {so.data(), (UINT)so.size()};
         d.NumStrides = r.u32();
         d.pBufferStrides = r.array<UINT>(d.NumStrides);
         d.RasterizedStream = r.u32();
         s.add(type, d);
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
         elements.resize(r.u32());
         for (auto &e : elements) {
            e.SemanticName = string(r);
            size_t tail = sizeof(e) - offsetof(D3D12_INPUT_ELEMENT_DESC, SemanticIndex);
            memcpy(&e.SemanticIndex, r.raw(tail, 4), tail);
         }
         s.add(type, D3D12_INPUT_LAYOUT_DESC{elements.data(), (UINT)elements.size()});
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
         auto [p, n] = r.blob();
         s.add(type, D3D12_CACHED_PIPELINE_STATE{p, n});
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
         D3D12_VIEW_INSTANCING_DESC d;
         d.ViewInstanceCount = r.u32();
         d.pViewInstanceLocations = r.array<D3D12_VIEW_INSTANCE_LOCATION>(d.ViewInstanceCount);
         d.Flags = (D3D12_VIEW_INSTANCING_FLAGS)r.u32();
         s.add(type, d);
         break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE: {
         auto [p, n] = r.blob();
         s.add(type, D3D12_SERIALIZED_ROOT_SIGNATURE_DESC{p, n});
         break;
      }
      default: {
         size_t n = r.u32();
         s.add(type, r.raw(n, 4), n, 4);
         break;
      }
      }
   }
   s.data.resize((s.data.size() + 7) & ~size_t(7));

   HRESULT hr = E_NOINTERFACE;
   ID3D12PipelineState *pso = nullptr;
   if (need(dev, 2, "CreatePipelineState")) {
      D3D12_PIPELINE_STATE_STREAM_DESC desc = {s.data.size(), s.data.data()};
      hr = static_cast<ID3D12Device2 *>(dev->p)->CreatePipelineState(&desc, IID_ID3D12PipelineState, (void **)&pso);
   }
   if (SUCCEEDED(hr))
      add(id, pso);
   w.put(hr);
}

/* Device */

static IDXCoreAdapter *default_adapter()
{
   IDXCoreAdapterFactory *factory = nullptr;
   IDXCoreAdapterList *list = nullptr;
   IDXCoreAdapter *adapter = nullptr;
   if (SUCCEEDED(create_dxcore_factory(IID_IDXCoreAdapterFactory, (void **)&factory)) &&
       SUCCEEDED(factory->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, IID_IDXCoreAdapterList, (void **)&list)) &&
       list->GetAdapterCount() > 0)
      list->GetAdapter(0, IID_IDXCoreAdapter, (void **)&adapter);
   if (list)
      list->Release();
   if (factory)
      factory->Release();
   return adapter;
}

static void device_create(Reader &r, Writer &w)
{
   IUnknown *adapter = lookup(r.u32());
   uint32_t id = r.u32();
   auto level = (D3D_FEATURE_LEVEL)r.u32();
   IDXCoreAdapter *own = adapter ? nullptr : default_adapter();
   ID3D12Device *dev = nullptr;
   HRESULT hr = create_device(adapter ? adapter : own, level, IID_ID3D12Device, (void **)&dev);
   if (own)
      own->Release();
   w.put(hr);
   if (FAILED(hr))
      return;
   uint32_t version;
   dev = static_cast<ID3D12Device *>(newest(dev, device_iids, 15, &version));
   add(id, dev, version);
   w.u32(version);
   w.u32(dev->GetNodeCount());
   for (int i = 0; i < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; i++)
      w.u32(dev->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)i));
   w.put(dev->GetAdapterLuid());
}

// Reads a view desc whose union starts at `head` on x86-64, see w_view_desc().
template <class T> static const T *view_desc(Reader &r, T *d, size_t head)
{
   if (!r.u32())
      return nullptr;
   memset(d, 0, sizeof(*d));
   size_t n = r.u32();
   memcpy(d, r.raw(n, 4), std::min(n, head));
   n = r.u32();
   memcpy(reinterpret_cast<char *>(d) + head, r.raw(n, 8), std::min(n, sizeof(T) - head));
   return d;
}

static void create_view(Reader &r)
{
   Object *dev = find(r.u32());
   auto *d = static_cast<ID3D12Device *>(dev ? dev->p : nullptr);
   uint32_t kind = r.u32();
   D3D12_CPU_DESCRIPTOR_HANDLE dest = {(SIZE_T)r.u64()};
   Object *res_obj = find(r.u32()), *counter_obj = find(r.u32());
   auto *res = res_obj ? static_cast<ID3D12Resource *>(res_obj->p) : nullptr;
   auto *counter = counter_obj ? static_cast<ID3D12Resource *>(counter_obj->p) : nullptr;
   if (!d)
      return;
   // Writes through views are not tracked per command.
   if (kind == VIEW_UAV || kind == VIEW_RTV || kind == VIEW_DSV) {
      for (Object *o : {res_obj, counter_obj})
         if (o && o->backing)
            need_compare(o->backing, true);
   }
   switch (kind) {
   case VIEW_CBV: {
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
      bool has = r.u32();
      if (has) {
         cbv.BufferLocation = r.u64();
         cbv.SizeInBytes = r.u32();
      }
      // NVIDIA reads a CBV back after writing it, which takes about 3 us in a
      // shader-visible (write-combined) heap. Built in CPU memory and copied, 0.1 us.
      if (!dev->cbv_scratch) {
         D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
         if (FAILED(d->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, (void **)&dev->cbv_scratch)))
            dev->cbv_scratch = nullptr;
      }
      if (dev->cbv_scratch) {
         D3D12_CPU_DESCRIPTOR_HANDLE scratch = dev->cbv_scratch->GetCPUDescriptorHandleForHeapStart();
         d->CreateConstantBufferView(has ? &cbv : nullptr, scratch);
         d->CopyDescriptorsSimple(1, dest, scratch, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      } else {
         d->CreateConstantBufferView(has ? &cbv : nullptr, dest);
      }
      break;
   }
   case VIEW_SRV: {
      D3D12_SHADER_RESOURCE_VIEW_DESC v;
      d->CreateShaderResourceView(res, view_desc(r, &v, offsetof(D3D12_SHADER_RESOURCE_VIEW_DESC, Buffer)), dest);
      break;
   }
   case VIEW_UAV: {
      D3D12_UNORDERED_ACCESS_VIEW_DESC v;
      d->CreateUnorderedAccessView(res, counter, view_desc(r, &v, offsetof(D3D12_UNORDERED_ACCESS_VIEW_DESC, Buffer)), dest);
      break;
   }
   case VIEW_RTV: {
      D3D12_RENDER_TARGET_VIEW_DESC v;
      d->CreateRenderTargetView(res, view_desc(r, &v, offsetof(D3D12_RENDER_TARGET_VIEW_DESC, Buffer)), dest);
      break;
   }
   case VIEW_DSV: {
      D3D12_DEPTH_STENCIL_VIEW_DESC v;
      d->CreateDepthStencilView(res, view_desc(r, &v, sizeof(v)), dest);
      break;
   }
   case VIEW_SAMPLER: {
      D3D12_SAMPLER_DESC v;
      if (view_desc(r, &v, sizeof(v)))
         d->CreateSampler(&v, dest);
      break;
   }
   case VIEW_SAMPLER2: {
      D3D12_SAMPLER_DESC2 v;
      if (view_desc(r, &v, sizeof(v)) && need(dev, 11, "CreateSampler2"))
         static_cast<ID3D12Device11 *>(d)->CreateSampler2(&v, dest);
      break;
   }
   }
}

static void copy_descriptors(Reader &r)
{
   auto *dev = lookup<ID3D12Device>(r.u32());
   auto type = (D3D12_DESCRIPTOR_HEAP_TYPE)r.u32();
   std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> starts[2];
   std::vector<UINT> sizes[2];
   for (int i = 0; i < 2; i++) {
      uint32_t n = r.u32();
      for (uint32_t j = 0; j < n; j++) {
         starts[i].push_back({(SIZE_T)r.u64()});
         sizes[i].push_back(r.u32());
      }
   }
   if (dev)
      dev->CopyDescriptors(starts[0].size(), starts[0].data(), sizes[0].data(), starts[1].size(), starts[1].data(),
                           sizes[1].data(), type);
}

static D3D12_RESOURCE_DESC desc0(const D3D12_RESOURCE_DESC1 &d1)
{
   D3D12_RESOURCE_DESC d;
   memcpy(&d, &d1, sizeof(d));
   return d;
}

static void allocation_info(Reader &r, Writer &w)
{
   Object *dev = find(r.u32());
   uint32_t variant = r.u32(), mask = r.u32(), n = r.u32();
   std::vector<D3D12_RESOURCE_DESC1> d1(n);
   std::vector<D3D12_RESOURCE_DESC> d0(n);
   std::vector<UINT32> ncast(n);
   std::vector<const DXGI_FORMAT *> cast(n);
   for (uint32_t i = 0; i < n; i++) {
      d1[i] = r.get<D3D12_RESOURCE_DESC1>();
      d0[i] = desc0(d1[i]);
      ncast[i] = r.u32();
      cast[i] = r.array<DXGI_FORMAT>(ncast[i]);
   }
   bool want_info1 = r.u32();
   std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> info1(n);
   D3D12_RESOURCE_ALLOCATION_INFO info = {};
   D3D12_RESOURCE_ALLOCATION_INFO1 *p1 = want_info1 ? info1.data() : nullptr;
   auto *d = static_cast<ID3D12Device *>(dev ? dev->p : nullptr);
   if (variant == 0 && d)
      info = d->GetResourceAllocationInfo(mask, n, d0.data());
   else if (variant == 1 && need(dev, 4, "GetResourceAllocationInfo1"))
      info = static_cast<ID3D12Device4 *>(d)->GetResourceAllocationInfo1(mask, n, d0.data(), p1);
   else if (variant == 2 && need(dev, 8, "GetResourceAllocationInfo2"))
      info = static_cast<ID3D12Device8 *>(d)->GetResourceAllocationInfo2(mask, n, d1.data(), p1);
   else if (variant == 3 && need(dev, 12, "GetResourceAllocationInfo3"))
      info = static_cast<ID3D12Device12 *>(d)->GetResourceAllocationInfo3(mask, n, d1.data(), ncast.data(), cast.data(), p1);
   w.put(info);
   if (want_info1)
      w.raw(info1.data(), n * sizeof(info1[0]), 8);
}

static void footprints(Reader &r, Writer &w)
{
   Object *dev = find(r.u32());
   auto d1 = r.get<D3D12_RESOURCE_DESC1>();
   uint32_t first = r.u32(), n = r.u32();
   uint64_t base = r.u64();
   std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(n);
   std::vector<UINT> rows(n);
   std::vector<UINT64> row_sizes(n);
   UINT64 total = 0;
   if (dev && dev->level >= 8) {
      static_cast<ID3D12Device8 *>(dev->p)->GetCopyableFootprints1(&d1, first, n, base, layouts.data(), rows.data(), row_sizes.data(), &total);
   } else if (dev) {
      auto d0 = desc0(d1);
      static_cast<ID3D12Device *>(dev->p)->GetCopyableFootprints(&d0, first, n, base, layouts.data(), rows.data(), row_sizes.data(), &total);
   }
   for (uint32_t i = 0; i < n; i++) {
      w.u64(layouts[i].Offset);
      w.raw(&layouts[i].Footprint, sizeof(layouts[i].Footprint), 4);
      w.align(8);
      w.u32(rows[i]);
      w.u64(row_sizes[i]);
   }
   w.u64(total);
}

static D3D12_HEAP_PROPERTIES cached(D3D12_HEAP_PROPERTIES p);

static void create_resource(Reader &r, Writer &w)
{
   Object *dev = find(r.u32());
   auto *d = static_cast<ID3D12Device *>(dev ? dev->p : nullptr);
   uint32_t id = r.u32(), variant = r.u32();
   Object *heap_obj = find(r.u32());
   auto *heap = heap_obj ? static_cast<ID3D12Heap *>(heap_obj->p) : nullptr;
   uint64_t offset = r.u64();
   auto requested = r.get<D3D12_HEAP_PROPERTIES>();
   auto props = cached(requested);
   auto flags = (D3D12_HEAP_FLAGS)r.u32();
   auto d1 = r.get<D3D12_RESOURCE_DESC1>();
   auto d0 = desc0(d1);
   uint32_t state = r.u32();
   bool has_clear = r.u32();
   D3D12_CLEAR_VALUE clear = has_clear ? r.get<D3D12_CLEAR_VALUE>() : D3D12_CLEAR_VALUE{};
   const D3D12_CLEAR_VALUE *pclear = has_clear ? &clear : nullptr;
   uint32_t ncast = r.u32();
   auto *cast = r.array<DXGI_FORMAT>(ncast);
   uint32_t access = r.u32();
   uint64_t shadow_offset = access ? r.u64() : 0;
   int fd = access ? take_fd() : -1;

   ID3D12Resource *res = nullptr;
   void **out = (void **)&res;
   auto rstate = (D3D12_RESOURCE_STATES)state;
   auto layout = (D3D12_BARRIER_LAYOUT)state;
   HRESULT hr = E_NOINTERFACE;
   if (!d)
      hr = E_INVALIDARG;
   else if (variant == RES_COMMITTED)
      hr = d->CreateCommittedResource(&props, flags, &d0, rstate, pclear, IID_ID3D12Resource, out);
   else if (variant == RES_COMMITTED2 && need(dev, 8, "CreateCommittedResource2"))
      hr = static_cast<ID3D12Device8 *>(d)->CreateCommittedResource2(&props, flags, &d1, rstate, pclear, nullptr, IID_ID3D12Resource, out);
   else if (variant == RES_COMMITTED3 && need(dev, 10, "CreateCommittedResource3"))
      hr = static_cast<ID3D12Device10 *>(d)->CreateCommittedResource3(&props, flags, &d1, layout, pclear, nullptr, ncast, cast, IID_ID3D12Resource, out);
   else if (variant == RES_PLACED)
      hr = d->CreatePlacedResource(heap, offset, &d0, rstate, pclear, IID_ID3D12Resource, out);
   else if (variant == RES_PLACED1 && need(dev, 8, "CreatePlacedResource1"))
      hr = static_cast<ID3D12Device8 *>(d)->CreatePlacedResource1(heap, offset, &d1, rstate, pclear, IID_ID3D12Resource, out);
   else if (variant == RES_PLACED2 && need(dev, 10, "CreatePlacedResource2"))
      hr = static_cast<ID3D12Device10 *>(d)->CreatePlacedResource2(heap, offset, &d1, layout, pclear, ncast, cast, IID_ID3D12Resource, out);

   D3D12_RESOURCE_DESC1 desc = {};
   D3D12_HEAP_PROPERTIES real_props = {};
   D3D12_HEAP_FLAGS real_flags = D3D12_HEAP_FLAG_NONE;
   Shadow *shadow = nullptr;
   if (SUCCEEDED(hr)) {
      add(id, res);
      ID3D12Resource2 *res2;
      if (SUCCEEDED(res->QueryInterface(IID_ID3D12Resource2, (void **)&res2))) {
         desc = res2->GetDesc1();
         res2->Release();
      } else {
         auto d0 = res->GetDesc();
         memcpy(&desc, &d0, sizeof(d0));
      }
      res->GetHeapProperties(&real_props, &real_flags);
      if (variant <= RES_COMMITTED3)
         real_props.CPUPageProperty = requested.CPUPageProperty;
      Object &o = objects[id];
      if (fd >= 0) {
         shadow = shadow_create(fd, shadow_offset, desc.Width, access, res, false);
         o.shadow = o.backing = shadow;
         fd = -1;
      } else if (variant >= RES_PLACED && heap_obj && heap_obj->shadow) {
         o.backing = heap_obj->shadow;
         o.offset = offset;
      }
      if (o.backing && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
         o.size = desc.Width;
         o.va = res->GetGPUVirtualAddress();
         buffers_by_va[o.va] = id;
      }
   }
   if (fd >= 0)
      close(fd);
   w.put(hr);
   w.put(desc);
   w.u64(SUCCEEDED(hr) ? res->GetGPUVirtualAddress() : 0);
   w.put(real_props);
   w.u32(real_flags);
   w.u32(shadow != nullptr);
}

// GPU writes to CPU-visible memory are found by comparing it with a copy, so
// custom heaps are allocated cached. The client only sees its shadow.
static D3D12_HEAP_PROPERTIES cached(D3D12_HEAP_PROPERTIES p)
{
   if (p.Type == D3D12_HEAP_TYPE_CUSTOM && p.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE &&
       p.MemoryPoolPreference != D3D12_MEMORY_POOL_L1)
      p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
   return p;
}

static D3D12_RESOURCE_STATES heap_buffer_state(D3D12_HEAP_TYPE type)
{
   switch (type) {
   case D3D12_HEAP_TYPE_UPLOAD:
   case D3D12_HEAP_TYPE_GPU_UPLOAD:
      return D3D12_RESOURCE_STATE_GENERIC_READ;
   case D3D12_HEAP_TYPE_READBACK:
      return D3D12_RESOURCE_STATE_COPY_DEST;
   default:
      return D3D12_RESOURCE_STATE_COMMON;
   }
}

static void create_heap(Reader &r, Writer &w)
{
   auto *dev = lookup<ID3D12Device>(r.u32());
   uint32_t id = r.u32();
   auto desc = r.get<D3D12_HEAP_DESC>();
   uint32_t access = r.u32();
   uint64_t shadow_offset = access ? r.u64() : 0;
   int fd = access ? take_fd() : -1;

   auto requested = desc.Properties;
   desc.Properties = cached(requested);
   ID3D12Heap *heap = nullptr;
   HRESULT hr = dev ? dev->CreateHeap(&desc, IID_ID3D12Heap, (void **)&heap) : E_INVALIDARG;
   Shadow *shadow = nullptr;
   if (SUCCEEDED(hr)) {
      add(id, heap);
      desc = heap->GetDesc();
      desc.Properties.CPUPageProperty = requested.CPUPageProperty;
      // The heap is mapped through a buffer that covers all of it.
      D3D12_RESOURCE_DESC buf = {};
      buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buf.Width = desc.SizeInBytes;
      buf.Height = buf.DepthOrArraySize = buf.MipLevels = 1;
      buf.SampleDesc.Count = 1;
      buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ID3D12Resource *res;
      if (fd >= 0 && SUCCEEDED(dev->CreatePlacedResource(heap, 0, &buf, heap_buffer_state(desc.Properties.Type), nullptr,
                                                         IID_ID3D12Resource, (void **)&res))) {
         shadow = shadow_create(fd, shadow_offset, desc.SizeInBytes, access, res, true);
         objects[id].shadow = shadow;
         fd = -1;
      }
   }
   if (fd >= 0)
      close(fd);
   w.put(hr);
   w.put(desc);
   w.u32(shadow != nullptr);
}

static void create_queue(Reader &r, Writer &w)
{
   Object *dev = find(r.u32());
   auto *d = static_cast<ID3D12Device *>(dev ? dev->p : nullptr);
   uint32_t id = r.u32();
   auto desc = r.get<D3D12_COMMAND_QUEUE_DESC>();
   bool has_creator = r.u32();
   GUID creator = has_creator ? r.get<GUID>() : GUID{};
   ID3D12CommandQueue *q = nullptr;
   HRESULT hr = E_INVALIDARG;
   if (!has_creator && d)
      hr = d->CreateCommandQueue(&desc, IID_ID3D12CommandQueue, (void **)&q);
   else if (has_creator && need(dev, 9, "CreateCommandQueue1"))
      hr = static_cast<ID3D12Device9 *>(d)->CreateCommandQueue1(&desc, creator, IID_ID3D12CommandQueue, (void **)&q);
   if (SUCCEEDED(hr)) {
      auto *queue = new Queue;
      hr = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void **)&queue->fence);
      if (FAILED(hr)) {
         delete queue;
         q->Release();
      } else {
         add(id, q);
         objects[id].queue = queue;
         std::lock_guard<std::mutex> g(sync_lock);
         queues.push_back(queue);
      }
   }
   w.put(hr);
}

static void release(uint32_t id)
{
   if (id >= objects.size() || !objects[id].p)
      return;
   Object &o = objects[id];
   if (o.va) {
      auto va = buffers_by_va.find(o.va);
      if (va != buffers_by_va.end() && va->second == id)
         buffers_by_va.erase(va);
   }
   shadow_destroy(o.shadow);
   {
      std::lock_guard<std::mutex> g(sync_lock);
      published.erase(id);
   }
   if (o.queue) {
      {
         std::lock_guard<std::mutex> g(sync_lock);
         queues.erase(std::find(queues.begin(), queues.end(), o.queue));
      }
      o.queue->fence->Release();
      for (Submission &s : o.queue->pending)
         for (IUnknown *p : s.refs)
            p->Release();
      delete o.queue;
   }
   if (o.cbv_scratch)
      o.cbv_scratch->Release();
   for (auto *b : o.flip_buf)
      if (b)
         b->Release();
   for (auto *b : o.flip_old)
      b->Release();
   o.p->Release();
   o = Object();
}

/* Command lists */

typedef ID3D12GraphicsCommandList10 CL;

static D3D12_TEXTURE_COPY_LOCATION copy_location(Reader &r, Object **res)
{
   D3D12_TEXTURE_COPY_LOCATION l = {};
   *res = find(r.u32());
   l.pResource = *res ? static_cast<ID3D12Resource *>((*res)->p) : nullptr;
   l.Type = (D3D12_TEXTURE_COPY_TYPE)r.u32();
   if (l.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      l.PlacedFootprint.Offset = r.u64();
      memcpy(&l.PlacedFootprint.Footprint, r.raw(sizeof(l.PlacedFootprint.Footprint), 4), sizeof(l.PlacedFootprint.Footprint));
   } else {
      l.SubresourceIndex = r.u32();
   }
   return l;
}

// Bytes ResolveQueryData writes per query, 0 if unknown.
static uint64_t query_size(D3D12_QUERY_TYPE type)
{
   switch (type) {
   case D3D12_QUERY_TYPE_OCCLUSION:
   case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
   case D3D12_QUERY_TYPE_TIMESTAMP:
      return 8;
   case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
      return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
   case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1:
      return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
   case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
   case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
   case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
   case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
      return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
   default:
      return 0;
   }
}

/* Mesa flips depth images upside down with one CopyTextureRegion per row,
 * which costs the driver about 16 us each. A run of such copies is replaced
 * by a copy into a buffer, one CopyBufferRegion per row into a second buffer
 * in reverse order, and one copy back into the texture. */

static uint64_t flip_rows;  // rows copied through buffers, for D3D12_SHIM_STATS

static struct {
   Object *list = nullptr, *dst_obj = nullptr;
   D3D12_TEXTURE_COPY_LOCATION dst, src;
   UINT x, y, z;  // destination of the first row
   D3D12_BOX box;  // source of the first row
   int step = 0;   // source row step; the destination steps the other way
   UINT count = 0;
} flip;

static uint8_t plane_count(ID3D12Device *dev, DXGI_FORMAT format)
{
   static std::unordered_map<DXGI_FORMAT, uint8_t> planes;
   auto it = planes.find(format);
   if (it != planes.end())
      return it->second;
   D3D12_FEATURE_DATA_FORMAT_INFO info = {format};
   if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))))
      info.PlaneCount = 0;
   return planes[format] = info.PlaneCount;
}

static void transition(CL *l, ID3D12Resource *r, D3D12_RESOURCE_STATES &state, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b = {};
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, to};
   l->ResourceBarrier(1, &b);
   state = to;
}

static bool flip_through_buffers()
{
   Object *o = flip.list;
   auto *l = static_cast<CL *>(o->p);
   auto *src = static_cast<ID3D12Resource *>(flip.src.pResource);
   D3D12_RESOURCE_DESC sd = src->GetDesc();
   D3D12_RESOURCE_DESC dd = static_cast<ID3D12Resource *>(flip.dst.pResource)->GetDesc();
   if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.SampleDesc.Count != 1 || dd.Format != sd.Format)
      return false;
   ID3D12Device *dev;
   if (FAILED(l->GetDevice(IID_ID3D12Device, (void **)&dev)))
      return false;
   dev->Release();  // the list keeps it alive
   if (plane_count(dev, sd.Format) != 1)
      return false;

   D3D12_RESOURCE_DESC rows_desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, flip.box.right - flip.box.left, flip.count, 1, 1,
                                    sd.Format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
   UINT rows;
   UINT64 row_size, total;
   dev->GetCopyableFootprints(&rows_desc, 0, 1, 0, &fp, &rows, &row_size, &total);
   if (rows != flip.count || total == UINT64_MAX)
      return false;
   if (o->flip_size < total) {
      ID3D12Resource *bufs[2] = {};
      D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1,
                                DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
      for (auto *&b : bufs)
         if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_ID3D12Resource, (void **)&b))) {
            if (bufs[0])
               bufs[0]->Release();
            return false;
         }
      for (int i = 0; i < 2; i++) {
         if (o->flip_buf[i])
            o->flip_old.push_back(o->flip_buf[i]);
         o->flip_buf[i] = bufs[i];
         o->flip_state[i] = D3D12_RESOURCE_STATE_COMMON;
      }
      o->flip_size = total;
   }

   ID3D12Resource *a = o->flip_buf[0], *b = o->flip_buf[1];
   D3D12_TEXTURE_COPY_LOCATION la = {a, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, lb = {b, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
   la.PlacedFootprint = lb.PlacedFootprint = fp;
   UINT last = flip.count - 1;
   D3D12_BOX box = flip.box;
   box.top = flip.step > 0 ? flip.box.top : flip.box.top - last;
   box.bottom = box.top + flip.count;
   UINT y = flip.step > 0 ? flip.y - last : flip.y;

   if (o->flip_state[0] != D3D12_RESOURCE_STATE_COPY_DEST)
      transition(l, a, o->flip_state[0], D3D12_RESOURCE_STATE_COPY_DEST);
   if (o->flip_state[1] != D3D12_RESOURCE_STATE_COPY_DEST)
      transition(l, b, o->flip_state[1], D3D12_RESOURCE_STATE_COPY_DEST);
   l->CopyTextureRegion(&la, 0, 0, 0, &flip.src, &box);
   transition(l, a, o->flip_state[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
   for (UINT i = 0; i < flip.count; i++)
      l->CopyBufferRegion(b, (UINT64)(last - i) * fp.Footprint.RowPitch, a, (UINT64)i * fp.Footprint.RowPitch, row_size);
   transition(l, b, o->flip_state[1], D3D12_RESOURCE_STATE_COPY_SOURCE);
   l->CopyTextureRegion(&flip.dst, flip.x, y, flip.z, &lb, nullptr);
   return true;
}

static void flip_flush()
{
   if (!flip.count)
      return;
   if (flip.count >= 8 && flip_through_buffers()) {
      flip_rows += flip.count;
   } else {
      auto *l = static_cast<CL *>(flip.list->p);
      for (UINT i = 0; i < flip.count; i++) {
         D3D12_BOX box = flip.box;
         box.top += i * flip.step;
         box.bottom += i * flip.step;
         l->CopyTextureRegion(&flip.dst, flip.x, flip.y - i * flip.step, flip.z, &flip.src, &box);
      }
   }
   wrote(flip.list, flip.dst_obj);
   flip.count = 0;
}

// Adds a texture copy to the pending run of flipped rows, flushing the run if
// the copy does not continue it. False if the copy cannot start a run.
static bool flip_add(Object *o, const D3D12_TEXTURE_COPY_LOCATION &dst, Object *dst_obj, UINT x, UINT y, UINT z,
                     const D3D12_TEXTURE_COPY_LOCATION &src, const D3D12_BOX *box)
{
   bool row = box && box->bottom == box->top + 1 && box->back == box->front + 1 && box->right > box->left &&
              dst.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
              dst.pResource && src.pResource && dst.pResource != src.pResource;
   if (row && flip.count && flip.list == o && flip.dst.pResource == dst.pResource && flip.src.pResource == src.pResource &&
       flip.dst.SubresourceIndex == dst.SubresourceIndex && flip.src.SubresourceIndex == src.SubresourceIndex &&
       flip.x == x && flip.z == z && box->left == flip.box.left && box->right == flip.box.right && box->front == flip.box.front) {
      int ds = (int)box->top - (int)flip.box.top, dd = (int)y - (int)flip.y;
      if (flip.count == 1 && (ds == 1 || ds == -1) && dd == -ds) {
         flip.step = ds;
         flip.count = 2;
         return true;
      }
      if (flip.count > 1 && ds == flip.step * (int)flip.count && dd == -ds) {
         flip.count++;
         return true;
      }
   }
   flip_flush();
   if (!row)
      return false;
   flip.list = o;
   flip.dst_obj = dst_obj;
   flip.dst = dst;
   flip.src = src;
   flip.x = x;
   flip.y = y;
   flip.z = z;
   flip.box = *box;
   flip.count = 1;
   return true;
}

template <class T> static const T *optional(Reader &r, T *v)
{
   if (!r.u32())
      return nullptr;
   *v = r.get<T>();
   return v;
}

static void barrier(Reader &r, CL *l)
{
   uint32_t n = r.u32();
   std::vector<D3D12_BARRIER_GROUP> groups(n);
   std::vector<std::vector<D3D12_GLOBAL_BARRIER>> global(n);
   std::vector<std::vector<D3D12_TEXTURE_BARRIER>> texture(n);
   std::vector<std::vector<D3D12_BUFFER_BARRIER>> buffer(n);
   for (uint32_t g = 0; g < n; g++) {
      groups[g].Type = (D3D12_BARRIER_TYPE)r.u32();
      groups[g].NumBarriers = r.u32();
      for (uint32_t i = 0; i < groups[g].NumBarriers; i++) {
         switch (groups[g].Type) {
         case D3D12_BARRIER_TYPE_GLOBAL:
            global[g].push_back(r.get<D3D12_GLOBAL_BARRIER>());
            break;
         case D3D12_BARRIER_TYPE_TEXTURE: {
            D3D12_TEXTURE_BARRIER b;
            memcpy(&b, r.raw(offsetof(D3D12_TEXTURE_BARRIER, pResource), 4), offsetof(D3D12_TEXTURE_BARRIER, pResource));
            b.pResource = lookup<ID3D12Resource>(r.u32());
            b.Subresources = r.get<D3D12_BARRIER_SUBRESOURCE_RANGE>();
            b.Flags = (D3D12_TEXTURE_BARRIER_FLAGS)r.u32();
            texture[g].push_back(b);
            break;
         }
         case D3D12_BARRIER_TYPE_BUFFER: {
            D3D12_BUFFER_BARRIER b;
            memcpy(&b, r.raw(offsetof(D3D12_BUFFER_BARRIER, pResource), 4), offsetof(D3D12_BUFFER_BARRIER, pResource));
            b.pResource = lookup<ID3D12Resource>(r.u32());
            b.Offset = r.u64();
            b.Size = r.u64();
            buffer[g].push_back(b);
            break;
         }
         }
      }
      if (groups[g].Type == D3D12_BARRIER_TYPE_GLOBAL)
         groups[g].pGlobalBarriers = global[g].data();
      else if (groups[g].Type == D3D12_BARRIER_TYPE_TEXTURE)
         groups[g].pTextureBarriers = texture[g].data();
      else
         groups[g].pBufferBarriers = buffer[g].data();
   }
   l->Barrier(n, groups.data());
}

static void resource_barrier(Reader &r, CL *l)
{
   uint32_t n = r.u32();
   std::vector<D3D12_RESOURCE_BARRIER> b(n);
   for (auto &x : b) {
      x.Type = (D3D12_RESOURCE_BARRIER_TYPE)r.u32();
      x.Flags = (D3D12_RESOURCE_BARRIER_FLAGS)r.u32();
      switch (x.Type) {
      case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
         x.Transition.pResource = lookup<ID3D12Resource>(r.u32());
         x.Transition.Subresource = r.u32();
         x.Transition.StateBefore = (D3D12_RESOURCE_STATES)r.u32();
         x.Transition.StateAfter = (D3D12_RESOURCE_STATES)r.u32();
         break;
      case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
         x.Aliasing.pResourceBefore = lookup<ID3D12Resource>(r.u32());
         x.Aliasing.pResourceAfter = lookup<ID3D12Resource>(r.u32());
         break;
      default:
         x.UAV.pResource = lookup<ID3D12Resource>(r.u32());
         break;
      }
   }
   l->ResourceBarrier(n, b.data());
}

static void command(uint32_t op, Reader &r, Writer &w)
{
   Object *o = find(r.u32());
   if (!o)
      throw std::runtime_error("command for unknown list");
   auto *l = static_cast<CL *>(o->p);

   switch (op) {
   case OP_CL_CLOSE: {
      HRESULT hr = l->Close();
      if (FAILED(hr))
         warn("command list Close failed: %#x", (unsigned)hr);
      break;
   }
   case OP_CL_RESET: {
      o->writes.clear();
      for (auto *b : o->flip_old)
         b->Release();
      o->flip_old.clear();
      o->flip_state[0] = o->flip_state[1] = D3D12_RESOURCE_STATE_COMMON;
      auto *alloc = lookup<ID3D12CommandAllocator>(r.u32());
      HRESULT hr = l->Reset(alloc, lookup<ID3D12PipelineState>(r.u32()));
      if (FAILED(hr))
         warn("command list Reset failed: %#x", (unsigned)hr);
      break;
   }
   case OP_CL_CLEAR_STATE:
      l->ClearState(lookup<ID3D12PipelineState>(r.u32()));
      break;
   case OP_CL_DRAW: {
      uint32_t a = r.u32(), b = r.u32(), c = r.u32(), d = r.u32();
      l->DrawInstanced(a, b, c, d);
      break;
   }
   case OP_CL_DRAW_INDEXED: {
      uint32_t a = r.u32(), b = r.u32(), c = r.u32(), d = r.u32(), e = r.u32();
      l->DrawIndexedInstanced(a, b, c, (INT)d, e);
      break;
   }
   case OP_CL_DISPATCH: {
      uint32_t x = r.u32(), y = r.u32(), z = r.u32();
      l->Dispatch(x, y, z);
      break;
   }
   case OP_CL_COPY_BUFFER: {
      uint32_t dst_id = r.u32();
      auto *dst = lookup<ID3D12Resource>(dst_id);
      auto *src = lookup<ID3D12Resource>(r.u32());
      uint64_t dst_off = r.u64(), src_off = r.u64(), size = r.u64();
      l->CopyBufferRegion(dst, dst_off, src, src_off, size);
      if (size)
         wrote(o, dst_id, dst_off, size);
      break;
   }
   case OP_CL_COPY_TEXTURE: {
      Object *dst_obj, *src_obj;
      auto dst = copy_location(r, &dst_obj);
      uint32_t x = r.u32(), y = r.u32(), z = r.u32();
      auto src = copy_location(r, &src_obj);
      D3D12_BOX box;
      const D3D12_BOX *pbox = optional(r, &box);
      if (flip_add(o, dst, dst_obj, x, y, z, src, pbox))
         break;
      l->CopyTextureRegion(&dst, x, y, z, &src, pbox);
      if (dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
         const auto &f = dst.PlacedFootprint.Footprint;
         wrote(o, dst_obj, dst.PlacedFootprint.Offset, (uint64_t)f.RowPitch * f.Height * f.Depth);
      } else {
         wrote(o, dst_obj);
      }
      break;
   }
   case OP_CL_COPY_RESOURCE: {
      uint32_t dst_id = r.u32();
      Object *dst = find(dst_id);
      l->CopyResource(dst ? static_cast<ID3D12Resource *>(dst->p) : nullptr, lookup<ID3D12Resource>(r.u32()));
      wrote(o, dst, 0, dst ? dst->size : 0);
      break;
   }
   case OP_CL_RESOLVE: {
      uint32_t dst_id = r.u32();
      auto *dst = lookup<ID3D12Resource>(dst_id);
      wrote(o, dst_id);
      uint32_t dst_sub = r.u32();
      auto *src = lookup<ID3D12Resource>(r.u32());
      uint32_t src_sub = r.u32();
      l->ResolveSubresource(dst, dst_sub, src, src_sub, (DXGI_FORMAT)r.u32());
      break;
   }
   case OP_CL_RESOLVE_REGION: {
      uint32_t dst_id = r.u32();
      auto *dst = lookup<ID3D12Resource>(dst_id);
      wrote(o, dst_id);
      uint32_t dst_sub = r.u32(), x = r.u32(), y = r.u32();
      auto *src = lookup<ID3D12Resource>(r.u32());
      uint32_t src_sub = r.u32();
      D3D12_RECT rect;
      auto *prect = optional(r, &rect);
      auto format = (DXGI_FORMAT)r.u32();
      auto mode = (D3D12_RESOLVE_MODE)r.u32();
      if (need(o, 1, "ResolveSubresourceRegion"))
         l->ResolveSubresourceRegion(dst, dst_sub, x, y, src, src_sub, const_cast<D3D12_RECT *>(prect), format, mode);
      break;
   }
   case OP_CL_TOPOLOGY:
      l->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)r.u32());
      break;
   case OP_CL_VIEWPORTS: {
      uint32_t n = r.u32();
      l->RSSetViewports(n, r.array<D3D12_VIEWPORT>(n));
      break;
   }
   case OP_CL_SCISSORS: {
      uint32_t n = r.u32();
      l->RSSetScissorRects(n, r.array<D3D12_RECT>(n));
      break;
   }
   case OP_CL_BLEND_FACTOR:
      l->OMSetBlendFactor(r.array<FLOAT>(4));
      break;
   case OP_CL_STENCIL_REF:
      l->OMSetStencilRef(r.u32());
      break;
   case OP_CL_STENCIL_REF2: {
      uint32_t front = r.u32(), back = r.u32();
      if (need(o, 8, "OMSetFrontAndBackStencilRef"))
         l->OMSetFrontAndBackStencilRef(front, back);
      break;
   }
   case OP_CL_SET_PSO:
      l->SetPipelineState(lookup<ID3D12PipelineState>(r.u32()));
      break;
   case OP_CL_RESOURCE_BARRIER:
      resource_barrier(r, l);
      break;
   case OP_CL_BARRIER:
      if (need(o, 7, "Barrier"))
         barrier(r, l);
      break;
   case OP_CL_SET_DESC_HEAPS: {
      uint32_t n = r.u32();
      std::vector<ID3D12DescriptorHeap *> heaps(n);
      for (auto &h : heaps)
         h = lookup<ID3D12DescriptorHeap>(r.u32());
      l->SetDescriptorHeaps(n, heaps.data());
      break;
   }
   case OP_CL_SET_ROOT_SIG: {
      bool compute = r.u32();
      auto *root = lookup<ID3D12RootSignature>(r.u32());
      if (compute)
         l->SetComputeRootSignature(root);
      else
         l->SetGraphicsRootSignature(root);
      break;
   }
   case OP_CL_SET_ROOT_TABLE: {
      bool compute = r.u32();
      uint32_t index = r.u32();
      D3D12_GPU_DESCRIPTOR_HANDLE base = {r.u64()};
      if (compute)
         l->SetComputeRootDescriptorTable(index, base);
      else
         l->SetGraphicsRootDescriptorTable(index, base);
      break;
   }
   case OP_CL_SET_ROOT_CONSTANTS: {
      bool compute = r.u32();
      uint32_t index = r.u32(), offset = r.u32(), n = r.u32();
      const void *data = r.raw(n * 4, 4);
      if (compute)
         l->SetComputeRoot32BitConstants(index, n, data, offset);
      else
         l->SetGraphicsRoot32BitConstants(index, n, data, offset);
      break;
   }
   case OP_CL_SET_ROOT_VIEW: {
      bool compute = r.u32();
      uint32_t kind = r.u32(), index = r.u32();
      uint64_t va = r.u64();
      if (kind == 0)
         compute ? l->SetComputeRootConstantBufferView(index, va) : l->SetGraphicsRootConstantBufferView(index, va);
      else if (kind == 1)
         compute ? l->SetComputeRootShaderResourceView(index, va) : l->SetGraphicsRootShaderResourceView(index, va);
      else
         compute ? l->SetComputeRootUnorderedAccessView(index, va) : l->SetGraphicsRootUnorderedAccessView(index, va);
      if (kind == 2)
         wrote_va(o, va);
      break;
   }
   case OP_CL_INDEX_BUFFER: {
      D3D12_INDEX_BUFFER_VIEW v;
      l->IASetIndexBuffer(optional(r, &v));
      break;
   }
   case OP_CL_VERTEX_BUFFERS: {
      uint32_t start = r.u32(), n = r.u32();
      bool has = r.u32();
      l->IASetVertexBuffers(start, n, has ? r.array<D3D12_VERTEX_BUFFER_VIEW>(n, 8) : nullptr);
      break;
   }
   case OP_CL_SO_TARGETS: {
      uint32_t start = r.u32(), n = r.u32();
      bool has = r.u32();
      auto *views = has ? r.array<D3D12_STREAM_OUTPUT_BUFFER_VIEW>(n, 8) : nullptr;
      l->SOSetTargets(start, n, views);
      for (uint32_t i = 0; views && i < n; i++) {
         wrote_va(o, views[i].BufferLocation);
         wrote_va(o, views[i].BufferFilledSizeLocation);
      }
      break;
   }
   case OP_CL_RENDER_TARGETS: {
      uint32_t n = r.u32();
      bool single = r.u32();
      uint32_t count = r.u32();
      std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rts(count);
      for (auto &h : rts)
         h.ptr = r.u64();
      D3D12_CPU_DESCRIPTOR_HANDLE ds;
      bool has_ds = r.u32();
      if (has_ds)
         ds.ptr = r.u64();
      l->OMSetRenderTargets(n, count ? rts.data() : nullptr, single, has_ds ? &ds : nullptr);
      break;
   }
   case OP_CL_CLEAR_DSV: {
      D3D12_CPU_DESCRIPTOR_HANDLE h = {(SIZE_T)r.u64()};
      auto flags = (D3D12_CLEAR_FLAGS)r.u32();
      float depth = r.f32();
      uint8_t stencil = r.u32();
      uint32_t n = r.u32();
      l->ClearDepthStencilView(h, flags, depth, stencil, n, r.array<D3D12_RECT>(n));
      break;
   }
   case OP_CL_CLEAR_RTV: {
      D3D12_CPU_DESCRIPTOR_HANDLE h = {(SIZE_T)r.u64()};
      auto *color = r.array<FLOAT>(4);
      uint32_t n = r.u32();
      l->ClearRenderTargetView(h, color, n, r.array<D3D12_RECT>(n));
      break;
   }
   case OP_CL_CLEAR_UAV: {
      bool is_float = r.u32();
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = {r.u64()};
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = {(SIZE_T)r.u64()};
      uint32_t res_id = r.u32();
      auto *res = lookup<ID3D12Resource>(res_id);
      wrote(o, res_id);
      const void *values = r.raw(16, 4);
      uint32_t n = r.u32();
      auto *rects = r.array<D3D12_RECT>(n);
      if (is_float)
         l->ClearUnorderedAccessViewFloat(gpu, cpu, res, static_cast<const FLOAT *>(values), n, rects);
      else
         l->ClearUnorderedAccessViewUint(gpu, cpu, res, static_cast<const UINT *>(values), n, rects);
      break;
   }
   case OP_CL_QUERY: {
      uint32_t end = r.u32();
      auto *heap = lookup<ID3D12QueryHeap>(r.u32());
      auto type = (D3D12_QUERY_TYPE)r.u32();
      uint32_t index = r.u32();
      if (end)
         l->EndQuery(heap, type, index);
      else
         l->BeginQuery(heap, type, index);
      break;
   }
   case OP_CL_RESOLVE_QUERY: {
      auto *heap = lookup<ID3D12QueryHeap>(r.u32());
      auto type = (D3D12_QUERY_TYPE)r.u32();
      uint32_t start = r.u32(), n = r.u32();
      uint32_t dst_id = r.u32();
      uint64_t offset = r.u64();
      l->ResolveQueryData(heap, type, start, n, lookup<ID3D12Resource>(dst_id), offset);
      wrote(o, dst_id, offset, n * query_size(type));
      break;
   }
   case OP_CL_PREDICATION: {
      auto *buf = lookup<ID3D12Resource>(r.u32());
      uint64_t offset = r.u64();
      l->SetPredication(buf, offset, (D3D12_PREDICATION_OP)r.u32());
      break;
   }
   case OP_CL_EXECUTE_INDIRECT: {
      auto *sig = lookup<ID3D12CommandSignature>(r.u32());
      uint32_t max = r.u32();
      auto *args = lookup<ID3D12Resource>(r.u32());
      uint64_t args_off = r.u64();
      auto *count = lookup<ID3D12Resource>(r.u32());
      l->ExecuteIndirect(sig, max, args, args_off, count, r.u64());
      break;
   }
   case OP_CL_DEPTH_BOUNDS: {
      float min = r.f32(), max = r.f32();
      if (need(o, 1, "OMSetDepthBounds"))
         l->OMSetDepthBounds(min, max);
      break;
   }
   case OP_CL_VIEW_INSTANCE_MASK: {
      uint32_t mask = r.u32();
      if (need(o, 1, "SetViewInstanceMask"))
         l->SetViewInstanceMask(mask);
      break;
   }
   case OP_CL_WRITE_IMMEDIATE: {
      uint32_t n = r.u32();
      std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> params(n);
      for (auto &p : params) {
         p.Dest = r.u64();
         p.Value = r.u32();
      }
      bool has_modes = r.u32();
      auto *modes = has_modes ? r.array<D3D12_WRITEBUFFERIMMEDIATE_MODE>(n) : nullptr;
      if (need(o, 2, "WriteBufferImmediate"))
         l->WriteBufferImmediate(n, params.data(), modes);
      for (const auto &p : params)
         wrote_va(o, p.Dest, 4);
      break;
   }
   case OP_CL_DEPTH_BIAS: {
      float bias = r.f32(), clamp = r.f32(), slope = r.f32();
      if (need(o, 9, "RSSetDepthBias"))
         l->RSSetDepthBias(bias, clamp, slope);
      break;
   }
   case OP_CL_STRIP_CUT: {
      auto value = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)r.u32();
      if (need(o, 9, "IASetIndexBufferStripCutValue"))
         l->IASetIndexBufferStripCutValue(value);
      break;
   }
   }
}

/* DXCore */

static void dxcore(uint32_t op, Reader &r, Writer &w)
{
   if (op == OP_DXCORE_FACTORY) {
      uint32_t id = r.u32();
      IUnknown *f = nullptr;
      HRESULT hr = create_dxcore_factory ? create_dxcore_factory(IID_IDXCoreAdapterFactory, (void **)&f) : E_FAIL;
      if (SUCCEEDED(hr))
         add(id, f);
      w.put(hr);
      return;
   }

   IUnknown *o = lookup(r.u32());
   if (!o)
      throw std::runtime_error("dxcore call on unknown object");
   auto *factory = static_cast<IDXCoreAdapterFactory *>(o);
   auto *list = static_cast<IDXCoreAdapterList *>(o);
   auto *adapter = static_cast<IDXCoreAdapter *>(o);
   IUnknown *created = nullptr;
   HRESULT hr;
   switch (op) {
   case OP_FACTORY_CREATE_LIST: {
      uint32_t id = r.u32(), n = r.u32();
      hr = factory->CreateAdapterList(n, r.array<GUID>(n), IID_IDXCoreAdapterList, (void **)&created);
      if (SUCCEEDED(hr))
         add(id, created);
      w.put(hr);
      break;
   }
   case OP_FACTORY_ADAPTER_BY_LUID: {
      uint32_t id = r.u32();
      auto luid = r.get<LUID>();
      hr = factory->GetAdapterByLuid(luid, IID_IDXCoreAdapter, (void **)&created);
      if (SUCCEEDED(hr))
         add(id, created);
      w.put(hr);
      break;
   }
   case OP_LIST_GET_ADAPTER: {
      uint32_t id = r.u32(), index = r.u32();
      hr = list->GetAdapter(index, IID_IDXCoreAdapter, (void **)&created);
      if (SUCCEEDED(hr))
         add(id, created);
      w.put(hr);
      break;
   }
   case OP_LIST_COUNT:
      w.u32(list->GetAdapterCount());
      break;
   case OP_LIST_SORT: {
      uint32_t n = r.u32();
      w.put(list->Sort(n, r.array<DXCoreAdapterPreference>(n)));
      break;
   }
   case OP_ADAPTER_QUERY_BOOL: {
      uint32_t what = r.u32(), arg = r.u32();
      auto guid = r.get<GUID>();
      bool result = false;
      switch (what) {
      case 0: result = adapter->IsValid(); break;
      case 1: result = adapter->IsAttributeSupported(guid); break;
      case 2: result = adapter->IsPropertySupported((DXCoreAdapterProperty)arg); break;
      case 3: result = adapter->IsQueryStateSupported((DXCoreAdapterState)arg); break;
      }
      w.u32(result);
      break;
   }
   case OP_ADAPTER_PROPERTY_SIZE: {
      size_t size = 0;
      w.put(adapter->GetPropertySize((DXCoreAdapterProperty)r.u32(), &size));
      w.u64(size);
      break;
   }
   case OP_ADAPTER_PROPERTY: {
      auto prop = (DXCoreAdapterProperty)r.u32();
      std::vector<char> buf(r.u32());
      hr = adapter->GetProperty(prop, buf.size(), buf.data());
      w.put(hr);
      w.blob(buf.data(), buf.size());
      break;
   }
   case OP_ADAPTER_QUERY_STATE: {
      auto state = (DXCoreAdapterState)r.u32();
      auto [in, in_size] = r.blob();
      std::vector<char> out(r.u32());
      hr = adapter->QueryState(state, in_size, in, out.size(), out.data());
      w.put(hr);
      w.blob(out.data(), out.size());
      break;
   }
   }
}

/* Requests */

static bool has_reply(uint32_t op)
{
   switch (op) {
   case OP_RELEASE:
   case OP_CREATE_VIEW:
   case OP_COPY_DESCRIPTORS:
   case OP_QUEUE_EXECUTE:
   case OP_QUEUE_SIGNAL:
   case OP_QUEUE_WAIT:
   case OP_FENCE_SET_EVENT:
   case OP_ALLOCATOR_RESET:
      return false;
   default:
      return op < OP_CL_CLOSE;
   }
}

static void handle(uint32_t op, Reader &r, Writer &w)
{
   if (op != OP_CL_COPY_TEXTURE)
      flip_flush();
   if (op >= OP_DXCORE_FACTORY && op <= OP_ADAPTER_QUERY_STATE)
      return dxcore(op, r, w);
   if (op >= OP_CL_CLOSE && op < OP_COUNT)
      return command(op, r, w);

   switch (op) {
   case OP_RELEASE:
      release(r.u32());
      break;
   case OP_ENABLE_EXPERIMENTAL: {
      uint32_t n = r.u32();
      w.put(enable_experimental(n, r.array<IID>(n), nullptr, nullptr));
      break;
   }
   case OP_SERIALIZE_ROOT_SIG:
      serialize_root_sig(r, w);
      break;
   case OP_CREATE_DEVICE:
      device_create(r, w);
      break;
   case OP_CHECK_FEATURE: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      auto feature = (D3D12_FEATURE)r.u32();
      uint32_t size = r.u32();
      if (feature == D3D12_FEATURE_FEATURE_LEVELS) {
         D3D12_FEATURE_DATA_FEATURE_LEVELS d = {};
         d.NumFeatureLevels = r.u32();
         d.pFeatureLevelsRequested = r.array<D3D_FEATURE_LEVEL>(d.NumFeatureLevels);
         w.put(dev->CheckFeatureSupport(feature, &d, sizeof(d)));
         w.u32(d.MaxSupportedFeatureLevel);
      } else {
         auto [p, n] = r.blob();
         std::vector<char> data(static_cast<const char *>(p), static_cast<const char *>(p) + n);
         data.resize(size);
         w.put(dev->CheckFeatureSupport(feature, data.data(), size));
         w.blob(data.data(), size);
      }
      break;
   }
   case OP_REMOVED_REASON:
      w.put(lookup<ID3D12Device>(r.u32())->GetDeviceRemovedReason());
      break;
   case OP_CUSTOM_HEAP_PROPS: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t node = r.u32();
      w.put(dev->GetCustomHeapProperties(node, (D3D12_HEAP_TYPE)r.u32()));
      break;
   }
   case OP_ALLOCATION_INFO:
      allocation_info(r, w);
      break;
   case OP_FOOTPRINTS:
      footprints(r, w);
      break;
   case OP_CREATE_QUEUE:
      create_queue(r, w);
      break;
   case OP_CREATE_ALLOCATOR: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t id = r.u32();
      IUnknown *a;
      HRESULT hr = dev->CreateCommandAllocator((D3D12_COMMAND_LIST_TYPE)r.u32(), IID_ID3D12CommandAllocator, (void **)&a);
      if (SUCCEEDED(hr))
         add(id, a);
      w.put(hr);
      break;
   }
   case OP_CREATE_LIST: {
      Object *dev = find(r.u32());
      auto *d = static_cast<ID3D12Device *>(dev->p);
      uint32_t id = r.u32(), variant = r.u32(), node = r.u32();
      auto type = (D3D12_COMMAND_LIST_TYPE)r.u32();
      auto *alloc = lookup<ID3D12CommandAllocator>(r.u32());
      auto *pso = lookup<ID3D12PipelineState>(r.u32());
      auto flags = (D3D12_COMMAND_LIST_FLAGS)r.u32();
      IUnknown *l = nullptr;
      HRESULT hr = E_NOINTERFACE;
      if (variant == 0)
         hr = d->CreateCommandList(node, type, alloc, pso, IID_ID3D12GraphicsCommandList, (void **)&l);
      else if (need(dev, 4, "CreateCommandList1"))
         hr = static_cast<ID3D12Device4 *>(d)->CreateCommandList1(node, type, flags, IID_ID3D12GraphicsCommandList, (void **)&l);
      uint32_t version = 0;
      if (SUCCEEDED(hr)) {
         l = newest(l, cmdlist_iids, 11, &version);
         add(id, l, version);
      }
      w.put(hr);
      w.u32(version);
      break;
   }
   case OP_CREATE_FENCE: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t id = r.u32();
      uint64_t initial = r.u64();
      auto flags = (D3D12_FENCE_FLAGS)r.u32();
      uint32_t slot = r.u32();
      if (r.u32()) {
         int fd = take_fd();
         void *p = mmap(nullptr, FENCE_SLOTS * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
         close(fd);
         if (p != MAP_FAILED && !fence_values)
            fence_values = static_cast<uint64_t *>(p);
      }
      ID3D12Fence *f;
      HRESULT hr = dev->CreateFence(initial, flags, IID_ID3D12Fence, (void **)&f);
      if (SUCCEEDED(hr)) {
         add(id, f);
         if (fence_values && slot < FENCE_SLOTS) {
            std::lock_guard<std::mutex> g(sync_lock);
            fence_values[slot] = f->GetCompletedValue();
            published[id] = {f, slot};
         }
      }
      w.put(hr);
      break;
   }
   case OP_CREATE_DESC_HEAP: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t id = r.u32();
      auto desc = r.get<D3D12_DESCRIPTOR_HEAP_DESC>();
      ID3D12DescriptorHeap *h = nullptr;
      HRESULT hr = dev->CreateDescriptorHeap(&desc, IID_ID3D12DescriptorHeap, (void **)&h);
      w.put(hr);
      if (SUCCEEDED(hr)) {
         add(id, h);
         w.u64(h->GetCPUDescriptorHandleForHeapStart().ptr);
         w.u64(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE ? h->GetGPUDescriptorHandleForHeapStart().ptr : 0);
      }
      break;
   }
   case OP_CREATE_QUERY_HEAP: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t id = r.u32();
      auto desc = r.get<D3D12_QUERY_HEAP_DESC>();
      IUnknown *h;
      HRESULT hr = dev->CreateQueryHeap(&desc, IID_ID3D12QueryHeap, (void **)&h);
      if (SUCCEEDED(hr))
         add(id, h);
      w.put(hr);
      break;
   }
   case OP_CREATE_ROOT_SIG: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t id = r.u32(), node = r.u32();
      auto [p, n] = r.blob();
      IUnknown *rs;
      HRESULT hr = dev->CreateRootSignature(node, p, n, IID_ID3D12RootSignature, (void **)&rs);
      if (SUCCEEDED(hr))
         add(id, rs);
      w.put(hr);
      break;
   }
   case OP_CREATE_PSO:
      create_pso(r, w);
      break;
   case OP_CREATE_CMD_SIG: {
      auto *dev = lookup<ID3D12Device>(r.u32());
      uint32_t id = r.u32();
      auto *root = lookup<ID3D12RootSignature>(r.u32());
      D3D12_COMMAND_SIGNATURE_DESC desc;
      desc.ByteStride = r.u32();
      desc.NumArgumentDescs = r.u32();
      desc.pArgumentDescs = r.array<D3D12_INDIRECT_ARGUMENT_DESC>(desc.NumArgumentDescs);
      desc.NodeMask = r.u32();
      IUnknown *s;
      HRESULT hr = dev->CreateCommandSignature(&desc, root, IID_ID3D12CommandSignature, (void **)&s);
      if (SUCCEEDED(hr))
         add(id, s);
      w.put(hr);
      break;
   }
   case OP_CREATE_HEAP:
      create_heap(r, w);
      break;
   case OP_CREATE_RESOURCE:
      create_resource(r, w);
      break;
   case OP_CREATE_VIEW:
      create_view(r);
      break;
   case OP_COPY_DESCRIPTORS:
      copy_descriptors(r);
      break;
   case OP_RESIDENCY: {
      Object *dev = find(r.u32());
      uint32_t what = r.u32();
      auto flags = (D3D12_RESIDENCY_FLAGS)r.u32();
      uint32_t fence_id = r.u32();
      auto *fence = lookup<ID3D12Fence>(fence_id);
      uint64_t value = r.u64();
      std::vector<ID3D12Pageable *> objs(r.u32());
      for (auto &p : objs)
         p = lookup<ID3D12Pageable>(r.u32());
      auto *d = static_cast<ID3D12Device *>(dev->p);
      HRESULT hr = E_NOINTERFACE;
      if (what == 0)
         hr = d->MakeResident(objs.size(), objs.data());
      else if (what == 1)
         hr = d->Evict(objs.size(), objs.data());
      else if (need(dev, 3, "EnqueueMakeResident"))
         hr = static_cast<ID3D12Device3 *>(d)->EnqueueMakeResident(flags, objs.size(), objs.data(), fence, value);
      if (what == 2 && SUCCEEDED(hr))
         watch_fence(fence_id, fence, value);
      w.put(hr);
      break;
   }
   case OP_MULTI_FENCE_EVENT: {
      Object *dev = find(r.u32());
      auto flags = (D3D12_MULTIPLE_FENCE_WAIT_FLAGS)r.u32();
      uint32_t n = r.u32();
      std::vector<ID3D12Fence *> fences(n);
      std::vector<UINT64> values(n);
      for (uint32_t i = 0; i < n; i++) {
         fences[i] = lookup<ID3D12Fence>(r.u32());
         values[i] = r.u64();
      }
      int fd = take_fd();
      if (!need(dev, 1, "SetEventOnMultipleFenceCompletion")) {
         close(fd);
         w.put(E_NOINTERFACE);
         break;
      }
      auto *d = static_cast<ID3D12Device1 *>(dev->p);
      w.put(watch(fd, [&](HANDLE e) { return d->SetEventOnMultipleFenceCompletion(fences.data(), values.data(), n, flags, e); }));
      break;
   }
   case OP_QUEUE_EXECUTE: {
      Object *q = find(r.u32());
      push(r);
      uint32_t n = r.u32();
      std::vector<ID3D12CommandList *> lists(n);
      std::vector<Write> writes;
      std::vector<IUnknown *> refs;
      for (auto &l : lists) {
         Object *o = find(r.u32());
         l = o ? static_cast<ID3D12CommandList *>(o->p) : nullptr;
         if (!o)
            continue;
         writes.insert(writes.end(), o->writes.begin(), o->writes.end());
         for (auto *b : o->flip_buf)
            if (b)
               refs.push_back(b);
         refs.insert(refs.end(), o->flip_old.begin(), o->flip_old.end());
      }
      for (IUnknown *p : refs)
         p->AddRef();
      auto *queue = static_cast<ID3D12CommandQueue *>(q->p);
      queue->ExecuteCommandLists(n, lists.data());
      std::lock_guard<std::mutex> g(sync_lock);
      Queue *pq = q->queue;
      queue->Signal(pq->fence, ++pq->submitted);
      if (!writes.empty() || !refs.empty())
         pq->pending.push_back({pq->submitted, std::move(writes), std::move(refs)});
      break;
   }
   case OP_QUEUE_SIGNAL:
   case OP_QUEUE_WAIT: {
      auto *q = lookup<ID3D12CommandQueue>(r.u32());
      uint32_t id = r.u32();
      auto *fence = lookup<ID3D12Fence>(id);
      uint64_t value = r.u64();
      HRESULT hr = op == OP_QUEUE_SIGNAL ? q->Signal(fence, value) : q->Wait(fence, value);
      if (FAILED(hr))
         warn("queue %s failed: %#x", op == OP_QUEUE_SIGNAL ? "Signal" : "Wait", (unsigned)hr);
      else if (op == OP_QUEUE_SIGNAL)
         watch_fence(id, fence, value);
      break;
   }
   case OP_QUEUE_TIMESTAMP_FREQ: {
      UINT64 freq = 0;
      w.put(lookup<ID3D12CommandQueue>(r.u32())->GetTimestampFrequency(&freq));
      w.u64(freq);
      break;
   }
   case OP_QUEUE_CLOCK_CALIBRATION: {
      UINT64 gpu = 0, cpu = 0;
      w.put(lookup<ID3D12CommandQueue>(r.u32())->GetClockCalibration(&gpu, &cpu));
      w.u64(gpu);
      w.u64(cpu);
      break;
   }
   case OP_ALLOCATOR_RESET: {
      HRESULT hr = lookup<ID3D12CommandAllocator>(r.u32())->Reset();
      if (FAILED(hr))
         warn("command allocator Reset failed: %#x", (unsigned)hr);
      break;
   }
   case OP_FENCE_COMPLETED: {
      uint64_t v = lookup<ID3D12Fence>(r.u32())->GetCompletedValue();
      sync_from_gpu();
      w.u64(v);
      break;
   }
   case OP_FENCE_SET_EVENT: {
      auto *fence = lookup<ID3D12Fence>(r.u32());
      uint64_t value = r.u64();
      int fd = take_fd();
      if (fence) {
         watch_later(fence, value, fd);
      } else if (fd >= 0) {
         uint64_t one = 1;
         if (write(fd, &one, sizeof(one)) < 0)
            warn("cannot signal client event");
         close(fd);
      }
      break;
   }
   case OP_FENCE_SIGNAL: {
      auto *fence = lookup<ID3D12Fence>(r.u32());
      fence->Signal(r.u64());
      sync_from_gpu();
      w.u32(0);
      break;
   }
   default:
      throw std::runtime_error("unknown request " + std::to_string(op));
   }
}

/* Connection */

static std::vector<char> in(1 << 20);
static size_t in_start, in_end;

static bool receive(int sock)
{
   if (in_start) {
      memmove(in.data(), in.data() + in_start, in_end - in_start);
      in_end -= in_start;
      in_start = 0;
   }
   if (in.size() - in_end < 65536)
      in.resize(in.size() * 2);
   iovec iov = {in.data() + in_end, in.size() - in_end};
   union {
      char buf[CMSG_SPACE(16 * sizeof(int))];
      cmsghdr align;
   } ctrl;
   msghdr mh = {};
   mh.msg_iov = &iov;
   mh.msg_iovlen = 1;
   mh.msg_control = ctrl.buf;
   mh.msg_controllen = sizeof(ctrl.buf);
   ssize_t n;
   do
      n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
   while (n < 0 && errno == EINTR);
   if (n <= 0)
      return false;
   for (cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
      if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
         continue;
      size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      for (size_t i = 0; i < count; i++) {
         int fd;
         memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
         fds.push_back(fd);
      }
   }
   in_end += n;
   return true;
}

static void send_reply(int sock, Writer &w)
{
   uint32_t hdr[2] = {(uint32_t)w.buf.size(), 0};
   w.align(8);
   std::vector<char> out(sizeof(hdr) + w.buf.size());
   memcpy(out.data(), hdr, sizeof(hdr));
   memcpy(out.data() + sizeof(hdr), w.buf.data(), w.buf.size());
   size_t pos = 0;
   while (pos < out.size()) {
      ssize_t n = write(sock, out.data() + pos, out.size() - pos);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         exit(0);
      pos += n;
   }
}

static void *load(const char *lib, const char *sym)
{
   void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
   void *p = h ? dlsym(h, sym) : nullptr;
   if (!p)
      warn("cannot load %s from %s: %s", sym, lib, dlerror());
   return p;
}

int main()
{
   // Ctrl+C and timeout(1) signal the client's whole process group. Let the
   // client shut down cleanly; we exit when its socket closes.
   signal(SIGPIPE, SIG_IGN);
   signal(SIGINT, SIG_IGN);
   signal(SIGTERM, SIG_IGN);
   signal(SIGHUP, SIG_IGN);
   create_device = (decltype(create_device))load("libd3d12.so", "D3D12CreateDevice");
   serialize_root_signature = (decltype(serialize_root_signature))load("libd3d12.so", "D3D12SerializeVersionedRootSignature");
   enable_experimental = (decltype(enable_experimental))load("libd3d12.so", "D3D12EnableExperimentalFeatures");
   create_dxcore_factory = (decltype(create_dxcore_factory))load("libdxcore.so", "DXCoreCreateAdapterFactory");
   if (!create_device || !serialize_root_signature || !enable_experimental || !create_dxcore_factory)
      return 1;

   epoll_fd = epoll_create1(EPOLL_CLOEXEC);
   std::thread(wait_loop).detach();
   std::thread(watch_loop).detach();

   // D3D12_SHIM_STATS=1 prints how often each request was made and the time
   // spent handling it, at exit.
   static uint64_t counts[OP_COUNT];
   static double op_ms[OP_COUNT];
   bool stats = getenv("D3D12_SHIM_STATS");
   if (stats)
      atexit([] {
         double total = 0;
         for (int op = 0; op < OP_COUNT; op++) {
            if (counts[op])
               warn("op %d%s: %llu %.1f ms", op, has_reply(op) ? " (sync)" : "", (unsigned long long)counts[op], op_ms[op]);
            total += op_ms[op];
         }
         warn("total %.1f ms", total);
         warn("rows flipped through buffers: %llu", (unsigned long long)flip_rows);
      });

   int sock = SHIM_SERVER_FD;
   try {
      for (;;) {
         while (in_end - in_start < 8)
            if (!receive(sock))
               exit(0);
         uint32_t hdr[2];
         memcpy(hdr, in.data() + in_start, sizeof(hdr));
         size_t total = 8 + ((hdr[1] + 7) & ~7u);
         while (in_end - in_start < total)
            if (!receive(sock))
               exit(0);
         Reader r = {in.data() + in_start + 8, hdr[1]};
         Writer w;
         if (stats && hdr[0] < OP_COUNT) {
            auto t = std::chrono::steady_clock::now();
            handle(hdr[0], r, w);
            counts[hdr[0]]++;
            op_ms[hdr[0]] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
         } else {
            handle(hdr[0], r, w);
         }
         in_start += total;
         if (has_reply(hdr[0]))
            send_reply(sock, w);
      }
   } catch (const std::exception &e) {
      warn("%s", e.what());
      return 1;
   }
}
