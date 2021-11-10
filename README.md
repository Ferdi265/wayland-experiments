# Wayland Examples

This repo contains the results of my personal experiments with writing a first
Wayland client.

The examples are roughly based on an [old Wayland Client Programming Tutorial][1],
but updated to use `xdg-shell` instead.

The main purpose of these experiments was to get a basic barebones client
running that can at least create "a window".

## Dependencies

- `CMake`
- `libwayland-client`
- `wayland-scanner`
- `wayland-protocols`

## Files

- `connect.c`: connect and disconnect to Wayland
- `registry.c`: list registered global interfaces, and get handle to compositor
- `surface.c`: create a surface
- `xdg_surface.c`: create an XDG surface and toplevel
- `application.c`: create a shm pool and buffer, and configure the window

[1]: https://jan.newmarch.name/Wayland/ProgrammingClient/
