# Wayland Experiments

This repo contains the results of my personal experiments with writing a first
Wayland client.

The experiments are roughly based on an [old Wayland Client Programming Tutorial][1],
but updated to use `xdg_shell` instead of `wl_shell`.

The main purpose of these experiments was to get a basic barebones client
running that can at least create "a window".

Later I have extended these clients to experiment with the wlr screencopy and
export-dmabuf protocols, and especially around how to handle DMA-BUFs.

## Dependencies

- `CMake`
- `pkg-config`
- `wayland-client`
- `wayland-egl`
- `wayland-scanner`
- `wayland-protocols`
- `wlr-protocols` (git submodule)
- `EGL`
- `GLESv2`
- `libdrm`
- `libgbm`

## Files

- `connect.c`: connect and disconnect to Wayland
- `registry.c`: list registered global interfaces, and get handle to compositor
- `surface.c`: create a surface
- `xdg_surface.c`: create an XDG surface and toplevel
- `application.c`: create a shm pool and buffer, and configure the window
- `fractional_scale_checkerboard.c`: render a pixel checkerboard in a client that supports fractional scale
- `egl.c`: create an egl window and configure a window, rendering a yellow screen
- `screencopy_shm.c`: capture one frame with screencopy and display it using a shm buffer
- `screencopy_dmabuf.c`: capture one frame with screencopy and display it using a dmabuf buffer
- `screencopy_shm_egl.c`: capture one frame with screencopy using a shm buffer, upload it to egl, and display it
- `screencopy_dmabuf_egl.c`: capture one frame with screencopy using a dmabuf buffer, import it to egl, and display it
- `export_dmabuf.c`: capture one frame with export-dmabuf and display it
- `export_dmabuf_egl.c`: capture one frame with export-dmabuf, import it into egl, and display

[1]: https://jan.newmarch.name/Wayland/ProgrammingClient/
