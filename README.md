# d3d12-shim32

Lets 32-bit Linux apps on WSL use the GPU. WSL ships `libd3d12.so` and
`libdxcore.so` for x86-64 only, so a 32-bit Mesa (Gallium d3d12 for GL, dzn
for Vulkan) has nothing to load. This project provides 32-bit versions of both
libraries that forward every call to a 64-bit server process, which owns the
real D3D12 objects.

```
32-bit app -> 32-bit Mesa -> client/libd3d12.so --socket--> d3d12-shim-server -> /usr/lib/wsl/lib/libd3d12.so
```

## Build and install

Needs `gcc-multilib`, the DirectX headers from the Mesa tree in
`~/src/mesa-dzn`, and a 32-bit Mesa build installed in
`~/.local/opt/mesa-dzn32` (meson cross file: `i386-cross.ini`).

```
make install    # lib, server and Vulkan ICD link
make check      # GL and Vulkan tests, headless and windowed (needs DISPLAY)
```

`make install` also writes `README.md` into the prefix, describing what is
installed there, how apps find it, and how to rebuild or remove it.

32-bit processes need `~/.local/opt/mesa-dzn32/lib` in `LD_LIBRARY_PATH`;
`~/.profile` sets it. The server path is compiled in and can be overridden
with `D3D12_SHIM_SERVER`.

## How it works

- Each client process starts its own server over a socketpair. Calls that
  return nothing (command recording, view creation, Release) are batched and
  sent at the next call that needs a reply.
- Structs are written in the x86-64 layout. Pipeline state streams, root
  signatures and other pointer-holding structs are rebuilt by the server.
- CPU descriptor handles are 64-bit server addresses; the client reserves an
  address range per descriptor heap and translates by range.
- CPU-visible memory cannot be shared with the server (WSL returns E_NOTIMPL
  for OpenExistingHeapFromAddress and FromFileMapping), so the client maps a
  memfd shadow. Shadows are ranges of one 256 MB memfd when they fit. Pages
  the CPU writes are found with userfaultfd write protection and one
  PAGEMAP_SCAN over that mapping, and copied in at each ExecuteCommandLists.
- The server records which ranges each command list writes (copies, query
  resolves, WriteBufferImmediate). When a submission completes, it copies those
  ranges back, using 4 threads for copies of 4 MB or more (a frame read back
  for display). Writes it cannot bound (UAVs, render targets, resolves) fall
  back to comparing the pages with a copy.
- Completed fence values live in a memfd table shared with the client. The
  server updates it only after the GPU writes up to that value are copied, so
  GetCompletedValue is a memory read.
- Replies to fixed queries (feature support, allocation info, footprints) are
  cached in the client.
- `D3D12_SHIM_STATS=1` makes the server print request counts at exit.

## Performance

RTX 4070 SUPER, second pass of the OpenArena timedemo, on a loaded host, so
expect about ±20%:

| Resolution | 32-bit with shim | Native 64-bit |
|---|---|---|
| 1280x720 | 188-276 fps | 113-136 fps |
| 2560x1440 | 113-115 fps | 98-102 fps |
| 3840x2160 | 45-53 fps | 67 fps |

Mesa on WSL shows each frame by reading it back to the CPU, and the shim
copies it once more from the server's mapping, about 33 MB per frame at 4K.
That copy is why the shim falls behind native at 4K.

SuperTuxKart's 1280x720 profile race runs at 90 fps with the shim and 43 fps
on llvmpipe.

Mesa needs upstream commits 1761974a and 8835df35 (d3d12 buffer reclaim),
otherwise GL apps can deadlock while loading or hang at exit.

## Limits

- If the CPU and the GPU write the same page of a CPU-cached heap between two
  syncs, the GPU's copy wins.
- Mapping textures, shared handles, the debug layer, video, tiled resource
  mappings and protected sessions are not supported. Unimplemented methods
  print `d3d12-shim: ... is not implemented` and return `E_NOTIMPL`.
- Apps that write through UAVs or render to CPU-readable heaps pay for a page
  compare of those heaps on every completed submission.
- At most 4096 live fences use the shared table; more fall back to a round
  trip per GetCompletedValue.
