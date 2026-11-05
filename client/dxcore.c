/* DXCore adapter enumeration. Its headers are C++ only, so the vtables are spelled out here. */
#include <string.h>

#include "shim.h"

static const GUID IID_IDXCoreAdapterFactory = {0x78ee5945, 0xc36e, 0x4b13, {0xa6, 0x69, 0x00, 0x5d, 0xd1, 0x1c, 0x0f, 0x06}};
static const GUID IID_IDXCoreAdapterList = {0x526c7776, 0x40e9, 0x459b, {0xb7, 0x11, 0xf3, 0x2a, 0xd7, 0x6d, 0xfc, 0x28}};
static const GUID IID_IDXCoreAdapter = {0xf0db4c7f, 0xfe5a, 0x42a2, {0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e}};

const IID *const dxcore_chain_factory[] = {&IID_IUnknown, &IID_IDXCoreAdapterFactory, NULL};
const IID *const dxcore_chain_list[] = {&IID_IUnknown, &IID_IDXCoreAdapterList, NULL};
const IID *const dxcore_chain_adapter[] = {&IID_IUnknown, &IID_IDXCoreAdapter, NULL};

typedef struct obj Obj;

struct factory_vtbl {
   HRESULT (*QueryInterface)(Obj *, REFIID, void **);
   ULONG (*AddRef)(Obj *);
   ULONG (*Release)(Obj *);
   HRESULT (*CreateAdapterList)(Obj *, uint32_t n, const GUID *attrs, REFIID, void **);
   HRESULT (*GetAdapterByLuid)(Obj *, const LUID *luid, REFIID, void **);
   bool (*IsNotificationTypeSupported)(Obj *, uint32_t type);
   HRESULT (*RegisterEventNotification)(Obj *);
   HRESULT (*UnregisterEventNotification)(Obj *);
};

struct list_vtbl {
   HRESULT (*QueryInterface)(Obj *, REFIID, void **);
   ULONG (*AddRef)(Obj *);
   ULONG (*Release)(Obj *);
   HRESULT (*GetAdapter)(Obj *, uint32_t index, REFIID, void **);
   uint32_t (*GetAdapterCount)(Obj *);
   bool (*IsStale)(Obj *);
   HRESULT (*GetFactory)(Obj *, REFIID, void **);
   HRESULT (*Sort)(Obj *, uint32_t n, const uint32_t *prefs);
   bool (*IsAdapterPreferenceSupported)(Obj *, uint32_t pref);
};

struct adapter_vtbl {
   HRESULT (*QueryInterface)(Obj *, REFIID, void **);
   ULONG (*AddRef)(Obj *);
   ULONG (*Release)(Obj *);
   bool (*IsValid)(Obj *);
   bool (*IsAttributeSupported)(Obj *, REFGUID attr);
   bool (*IsPropertySupported)(Obj *, uint32_t prop);
   HRESULT (*GetProperty)(Obj *, uint32_t prop, size_t size, void *data);
   HRESULT (*GetPropertySize)(Obj *, uint32_t prop, size_t *size);
   bool (*IsQueryStateSupported)(Obj *, uint32_t state);
   HRESULT (*QueryState)(Obj *, uint32_t state, size_t in_size, const void *in, size_t out_size, void *out);
   bool (*IsSetStateSupported)(Obj *, uint32_t state);
   HRESULT (*SetState)(Obj *, uint32_t state, size_t in_size, const void *in, size_t size, const void *data);
   HRESULT (*GetFactory)(Obj *, REFIID, void **);
};

static struct factory_vtbl factory_vtbl;
static struct list_vtbl list_vtbl;
static struct adapter_vtbl adapter_vtbl;

/* Creates an object of the given kind from a request that ends with its new id. */
static HRESULT create(uint32_t op, Obj *parent, enum kind kind, const void *vtbl, const void *args, size_t args_size,
                      REFIID riid, void **ppv)
{
   Obj *o = obj_new(sizeof(*o), kind, vtbl, NULL);
   msg_begin(op);
   w_obj(parent);
   w_u32(o->id);
   w_raw(args, args_size, 4);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

static HRESULT factory_CreateAdapterList(Obj *This, uint32_t n, const GUID *attrs, REFIID riid, void **ppv)
{
   char args[4 + 16 * 8];
   if (n > 8)
      return E_INVALIDARG;
   memcpy(args, &n, 4);
   memcpy(args + 4, attrs, n * sizeof(GUID));
   return create(OP_FACTORY_CREATE_LIST, This, K_LIST, &list_vtbl, args, 4 + n * sizeof(GUID), riid, ppv);
}

static HRESULT factory_GetAdapterByLuid(Obj *This, const LUID *luid, REFIID riid, void **ppv)
{
   return create(OP_FACTORY_ADAPTER_BY_LUID, This, K_ADAPTER, &adapter_vtbl, luid, sizeof(*luid), riid, ppv);
}

static bool factory_IsNotificationTypeSupported(Obj *This, uint32_t type)
{
   return false;
}

static HRESULT list_GetAdapter(Obj *This, uint32_t index, REFIID riid, void **ppv)
{
   return create(OP_LIST_GET_ADAPTER, This, K_ADAPTER, &adapter_vtbl, &index, 4, riid, ppv);
}

static uint32_t list_GetAdapterCount(Obj *This)
{
   msg_begin(OP_LIST_COUNT);
   w_obj(This);
   msg_call();
   uint32_t n = r_u32();
   msg_end();
   return n;
}

static bool list_IsStale(Obj *This)
{
   return false;
}

static HRESULT list_Sort(Obj *This, uint32_t n, const uint32_t *prefs)
{
   msg_begin(OP_LIST_SORT);
   w_obj(This);
   w_u32(n);
   w_raw(prefs, n * 4, 4);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return hr;
}

static bool list_IsAdapterPreferenceSupported(Obj *This, uint32_t pref)
{
   return pref <= 2;
}

enum adapter_query { QUERY_VALID, QUERY_ATTRIBUTE, QUERY_PROPERTY, QUERY_STATE, QUERY_SET_STATE };

static bool adapter_query(Obj *This, enum adapter_query what, uint32_t arg, const GUID *guid)
{
   static const GUID none;
   msg_begin(OP_ADAPTER_QUERY_BOOL);
   w_obj(This);
   w_u32(what);
   w_u32(arg);
   w_raw(guid ? guid : &none, sizeof(GUID), 4);
   msg_call();
   bool r = r_u32();
   msg_end();
   return r;
}

static bool adapter_IsValid(Obj *This) { return adapter_query(This, QUERY_VALID, 0, NULL); }
static bool adapter_IsAttributeSupported(Obj *This, REFGUID attr) { return adapter_query(This, QUERY_ATTRIBUTE, 0, attr); }
static bool adapter_IsPropertySupported(Obj *This, uint32_t prop) { return adapter_query(This, QUERY_PROPERTY, prop, NULL); }
static bool adapter_IsQueryStateSupported(Obj *This, uint32_t state) { return adapter_query(This, QUERY_STATE, state, NULL); }
static bool adapter_IsSetStateSupported(Obj *This, uint32_t state) { return false; }

/* Properties and states used by Mesa hold no pointers, so they pass through as bytes. */
static HRESULT adapter_GetProperty(Obj *This, uint32_t prop, size_t size, void *data)
{
   msg_begin(OP_ADAPTER_PROPERTY);
   w_obj(This);
   w_u32(prop);
   w_u32(size);
   msg_call();
   HRESULT hr = r_hr();
   if (SUCCEEDED(hr))
      r_blob(data, size);
   msg_end();
   return hr;
}

static HRESULT adapter_GetPropertySize(Obj *This, uint32_t prop, size_t *size)
{
   msg_begin(OP_ADAPTER_PROPERTY_SIZE);
   w_obj(This);
   w_u32(prop);
   msg_call();
   HRESULT hr = r_hr();
   *size = r_u64();
   msg_end();
   return hr;
}

static HRESULT adapter_QueryState(Obj *This, uint32_t state, size_t in_size, const void *in, size_t out_size, void *out)
{
   msg_begin(OP_ADAPTER_QUERY_STATE);
   w_obj(This);
   w_u32(state);
   w_blob(in, in_size);
   w_u32(out_size);
   /* Mostly memory budget polling, which may be slightly stale. */
   msg_call_cached(100);
   HRESULT hr = r_hr();
   if (SUCCEEDED(hr))
      r_blob(out, out_size);
   msg_end();
   return hr;
}

static HRESULT adapter_SetState(Obj *This, uint32_t state, size_t in_size, const void *in, size_t size, const void *data)
{
   return E_NOTIMPL;
}

static HRESULT not_implemented(Obj *This)
{
   return E_NOTIMPL;
}

uint32_t dxcore_adapter_id(IUnknown *adapter)
{
   Obj *o = (Obj *)adapter;
   return o->vtbl == &adapter_vtbl ? o->id : 0;
}

EXPORT HRESULT STDMETHODCALLTYPE DXCoreCreateAdapterFactory(REFIID riid, void **ppv)
{
   Obj *o = obj_new(sizeof(*o), K_FACTORY, &factory_vtbl, NULL);
   msg_begin(OP_DXCORE_FACTORY);
   w_u32(o->id);
   msg_call();
   HRESULT hr = r_hr();
   msg_end();
   return obj_finish(o, hr, riid, ppv);
}

__attribute__((constructor)) static void init(void)
{
   factory_vtbl = (struct factory_vtbl){
      (void *)obj_QueryInterface, (void *)obj_AddRef, (void *)obj_Release,
      factory_CreateAdapterList, factory_GetAdapterByLuid, factory_IsNotificationTypeSupported,
      not_implemented, not_implemented,
   };
   list_vtbl = (struct list_vtbl){
      (void *)obj_QueryInterface, (void *)obj_AddRef, (void *)obj_Release,
      list_GetAdapter, list_GetAdapterCount, list_IsStale, (void *)not_implemented, list_Sort,
      list_IsAdapterPreferenceSupported,
   };
   adapter_vtbl = (struct adapter_vtbl){
      (void *)obj_QueryInterface, (void *)obj_AddRef, (void *)obj_Release,
      adapter_IsValid, adapter_IsAttributeSupported, adapter_IsPropertySupported, adapter_GetProperty,
      adapter_GetPropertySize, adapter_IsQueryStateSupported, adapter_QueryState, adapter_IsSetStateSupported,
      adapter_SetState, (void *)not_implemented,
   };
}
