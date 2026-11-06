# AGENTS.md

This file provides guidance to coding agents working with code in this repository.

32-bit `libd3d12.so`/`libdxcore.so` for WSL that forward every D3D12 and DXCore
call over a socket to a 64-bit server, which calls the real
`/usr/lib/wsl/lib/libd3d12.so`. Used by a 32-bit Mesa (Gallium d3d12, dzn).
README.md covers the design and limits; install-readme.md is copied to the
install prefix by `make install` and documents the 32-bit Mesa build.

## Commands

```
make              # build/libd3d12.so (i386) and build/d3d12-shim-server (x86-64)
make install      # into ~/.local/opt/mesa-dzn32, plus the i686 Vulkan ICD link
make check        # builds and runs all four tests; glxtest and vkwsi need DISPLAY
make tests && ./build/vktest   # one test
```

- Tests load the installed library, so run `make install` before `make check`.
- Tests need `~/.local/opt/mesa-dzn32/lib` in `LD_LIBRARY_PATH` and
  `GALLIUM_DRIVER=d3d12` (both set by `~/.profile`).
- The server path is compiled into the client (`SERVER_PATH`);
  `D3D12_SHIM_SERVER=build/d3d12-shim-server` tests an uninstalled server.
- `D3D12_SHIM_STATS=1` prints per-op request counts and handling time at exit.
- DirectX headers come from `~/src/mesa-dzn/subprojects/DirectX-Headers-1.0`
  (`DX=` overrides). The 32-bit Mesa is rebuilt with
  `ninja -C ~/src/mesa-dzn/build32 install` using `i386-cross.ini`.
- No linter, no unit tests beyond `tests/`.

## Architecture

- `proto.h`: shared wire protocol. `enum op` lists every request. Payloads use
  x86-64 struct layout and alignment so the server can memcpy them; the client
  must write fields individually (see `w_resource_desc1` in `client/device.c`).
- `client/` (C, `-m32`, hidden visibility, only `EXPORT` symbols are public):
  - `wire.c`: spawns one server per process over a socketpair (fd 3), one
    request at a time under a global lock. `msg_begin` / `w_*` / `msg_send`
    (queued, no reply) or `msg_call` (flush and wait, read with `r_*`, then
    `msg_end`). `msg_call_cached` caches fixed answers.
  - `objects.c`: COM object base (`struct obj` with a client-assigned id that
    the server uses as its key), vtables for everything except device and
    command list, and `vtbl_init`, which fills unimplemented slots with traps
    that print `is not implemented` and return `E_NOTIMPL`. A struct-returning
    method must never be left as a trap (i386 callee pops the hidden return
    pointer).
  - `device.c`: ID3D12Device14, root signature and PSO stream serialization.
  - `cmdlist.c`: command list recording; everything is queued with no reply.
  - `shadow.c`: memfd shadows of CPU-visible memory, dirty pages tracked with
    userfaultfd write protection and PAGEMAP_SCAN, pushed at ExecuteCommandLists.
  - `dxcore.c`: DXCore vtables spelled out by hand (its headers are C++ only).
  - `cache.cpp`: C++ reply cache; libstdc++ is linked statically.
- `server/server.cpp` (one file): object table indexed by client id,
  `handle()` dispatches by op range (DXCore ops, command list ops from
  `OP_CL_CLOSE` on, then the rest). Rebuilds pointer-holding structs, mirrors
  shadows, records which ranges each command list writes and copies them back
  when a fence completes, and publishes completed fence values in a shared
  memfd table.

### Adding a method

1. Add an op to `enum op` in `proto.h` (command list ops go after `OP_CL_CLOSE`,
   since the server treats ops in that range as reply-less commands).
2. Client: write the vtable slot, serialize args in x86-64 layout.
3. Server: handle the op in `handle()` or `command()`. A non-command op that
   returns no reply must also be listed in `has_reply()`.
4. If the method writes GPU memory the CPU can read, record it with `wrote()`
   so the result is copied back.
