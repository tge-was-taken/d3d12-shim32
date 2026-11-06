# 32-bit Mesa with D3D12 for WSL

GPU drivers for 32-bit (i386) Linux apps on WSL: GL through Gallium d3d12 and
Vulkan through dzn, both on the Windows GPU. Installed by `make install` in
`~/projects/d3d12-shim32`, which also writes this file.

## Contents

| Path | What |
|---|---|
| `lib/libGLX_mesa.so.0`, `lib/libEGL_mesa.so.0` | 32-bit Mesa GLX and EGL (glvnd vendor libraries) |
| `lib/dri/d3d12_dri.so`, `lib/libgallium-*.so` | Gallium d3d12 GL driver |
| `lib/libvulkan_dzn.so`, `share/vulkan/icd.d/dzn_icd.i686.json` | dzn Vulkan driver and its ICD file |
| `lib/libd3d12.so`, `lib/libdxcore.so` (link) | D3D12 shim client; WSL ships these libraries for x86-64 only |
| `libexec/d3d12-shim-server` | 64-bit server the client starts per process; it calls the real `/usr/lib/wsl/lib/libd3d12.so` |
| `bin/spirv2dxil` | Mesa tool, not needed at run time |

## How apps find it

- GL: `~/.profile` exports `GALLIUM_DRIVER=d3d12` and puts `lib/` first in
  `LD_LIBRARY_PATH`. 64-bit processes skip these libraries, so the variable
  is safe for everything.
- Vulkan: `~/.local/share/vulkan/icd.d/dzn_icd.i686.json` links to the ICD
  file here. The loader uses it with no environment variables.
- A process started without the profile (`env -i`, a Windows start menu
  shortcut) needs both variables set by hand:

  ```
  GALLIUM_DRIVER=d3d12 LD_LIBRARY_PATH=$HOME/.local/opt/mesa-dzn32/lib app
  ```

## Check

```
glxinfo.i386-linux-gnu -B | grep renderer   # D3D12 (NVIDIA GeForce RTX 4070 SUPER)
make -C ~/projects/d3d12-shim32 check        # GL and Vulkan tests, needs DISPLAY
```

`llvmpipe` in the renderer string means `GALLIUM_DRIVER` is missing.
`D3D12_SHIM_STATS=1` prints the server's request counts and handling time when the app exits.

## Rebuild

Mesa comes from `~/src/mesa-dzn` (26.2.2 plus upstream commits 1761974a and
8835df35, which fix d3d12 buffer reclaim deadlocks), built in `build32` with
the cross file `~/projects/d3d12-shim32/i386-cross.ini`:

```
ninja -C ~/src/mesa-dzn/build32 install
make -C ~/projects/d3d12-shim32 install
```

To start over, configure `build32` with `-Dprefix=$HOME/.local/opt/mesa-dzn32
-Dlibdir=lib -Dbuildtype=release -Dgallium-drivers=d3d12
-Dvulkan-drivers=microsoft-experimental -Dplatforms=x11 -Dglx=dri
-Degl=enabled -Dgles2=enabled -Dglvnd=enabled -Dllvm=disabled` and the cross
file. The full option list is in `build32/meson-logs/meson-log.txt`.

## Uninstall

```
rm -r ~/.local/opt/mesa-dzn32
rm ~/.local/share/vulkan/icd.d/dzn_icd.i686.json
```

Then remove the `LD_LIBRARY_PATH` line from `~/.profile`.
