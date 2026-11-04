# openxr-sim

An OpenXR runtime whose headset is a window. An OpenXR application runs as it would on a headset; its two
eyes appear side by side on the desktop, and the headset and both controllers are moved and pressed with
the mouse and keyboard -- or by a program, over a socket, or by an agent, through an MCP server.

Without the simulated headset switched on it is the other useful thing: a runtime that is there and says
"no headset connected" (`XR_ERROR_FORM_FACTOR_UNAVAILABLE`), so an application with an optional VR mode
starts instead of failing to create its instance.

It began as LOS's runtime (`apps/los-openxr`) and is its own project since 0.7.0.

## What it does

* Vulkan, OpenGL and OpenGL ES sessions (Linux); Windows is being added (D3D11, D3D12, Vulkan, WGL).
* Composition layers: projection, quad, cylinder, equirect (2), each composited where it belongs.
* Touch-style controllers by any profile the application suggests; `XR_EXT_hand_tracking` and
  `XR_EXT_hand_interaction` with hands that follow the controllers.
* A control socket and `openxr-sim-mcp`, an MCP server: see both eyes, set and move poses, hold buttons.

`docs/DESIGN.md` says how it works and why; `docs/NOTES.md` is what was found out on the way.

## Building

Linux, with CMake 3.21+, a C11 and C++23 compiler, and the development files of: the OpenXR SDK
(`openxr`), Vulkan headers, GLFW 3.4+, fontconfig, jsoncpp (for the MCP server), EGL (for the tests).

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ninja -C build
    XR_RUNTIME_JSON=build/test_runtime.json build/openxr-sim-probe   # what an application sees
    tests/sim-test.sh                                                # the whole chain, headless, on lavapipe

Installed, the runtime is `<libdir>/openxr-sim/libopenxr_sim.so` with its manifest in
`share/openxr/1/openxr_sim.json`; `-DOPENXR_SIM_INSTALL_ACTIVE_RUNTIME=ON` also makes it the system's
default runtime. GLFW, Clay and stb are in `third_party/` (GLFW is built from there on Windows).

## Using it

    openxr-sim --use        # OpenXR applications get the simulated headset
    openxr-sim --unuse      # they find none connected again
    OPENXR_SIM_DEVICE=sim some-openxr-app    # per process

Then start an OpenXR application: the simulator's window opens with its session. Drag to turn, W A S D Q E
to move, Tab for the next device, Space/G/Z/X/M/C and the arrows for a hand's controls.

    claude mcp add openxr-sim -- openxr-sim-mcp

gives an agent the running simulators as tools.

## Licence

Apache-2.0 (`LICENSE`). Vendored: GLFW (zlib), Clay (zlib), stb (MIT or public domain), each with its own
licence in `third_party/`.
