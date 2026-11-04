# openxr-sim — notes

## What it is (0.1.0, 2026-10-01)

An OpenXR runtime (LOS's, until it became a repository of its own in 0.7.0). An OpenXR application asks the Khronos loader (openxr-sdk) for the active
runtime; with none, or a manifest whose `library_path` is empty or wrong, `xrCreateInstance` fails
(measured: an empty `library_path` → "failed to find negotiate function", "failed to load a runtime")
and most applications treat that as fatal. With this runtime the instance is created normally and
`xrGetSystem` for a headset answers `XR_ERROR_FORM_FACTOR_UNAVAILABLE` -- OpenXR's own "supported,
not connected" -- which applications are written to handle: they say "connect your headset" and
keep polling.

Installed: `<libdir>/openxr-sim/libopenxr_sim.so` (a module, off the library path),
`/usr/share/openxr/1/openxr_sim.json`, and -- with `-DOPENXR_SIM_INSTALL_ACTIVE_RUNTIME=ON`, as LOS builds
it -- `/etc/xdg/openxr/1/active_runtime.json` making it the system default. A user's `~/.config/openxr/1/active_runtime.json` -- SteamVR writes one
-- still wins, by the loader's search order; `XR_RUNTIME_JSON` wins over both.

## Shape: a runtime in front, a device behind

`runtime/runtime.c` is OpenXR's instance level: negotiation (`xrNegotiateLoaderRuntimeInterface`, the only
export), instances, extensions, systems, graphics requirements, paths, debug utils, timespec conversion,
the string functions, and the dispatch (`lookup`). Since 0.2.0 the rest is real too: `runtime/session.c`
(the session's states, frame timing, spaces, views), `runtime/swapchain.c` with `swapchain_vk.c` and
`swapchain_gl.c` (images in the application's own API, copied back at release), `runtime/input.c` (action
sets, actions, bindings, haptics). `common/device.h` is what a headset answers; `OPENXR_SIM_DEVICE`, else
`device =` in the configuration file (DESIGN.md, "Choosing the device"), chooses between `runtime/device_null.c` (the default) and
`runtime/device_sim.c`, the simulated headset -- see DESIGN.md. `OPENXR_SIM_LOG=1` says what the runtime is
doing on stderr.

* **Every function exists.** The loader fills its dispatch table from `xrGetInstanceProcAddr` at
  instance creation; a function left out is a NULL an application jumps through. Names come from
  `openxr_reflection.h`'s `XR_LIST_FUNCTIONS_*` (core 1.0, 1.1, and each offered extension), so a new
  SDK adds them without anyone typing a list. Those needing a system return
  `XR_ERROR_SYSTEM_INVALID`, the rest `XR_ERROR_HANDLE_INVALID` -- two shared bodies cast to each
  type, relying on the x86-64/AArch64 calling conventions for a callee that reads no arguments.
* **Extensions offered** are every graphics binding an application might REQUIRE at instance
  creation (vulkan_enable, vulkan_enable2, opengl_enable, opengl_es_enable, MNDX_egl_enable),
  composition_layer_depth, composition_layer_cylinder, composition_layer_equirect2,
  composition_layer_color_scale_bias (since 0.3.0), EXT_hand_tracking (since 0.4.0), EXT_hand_interaction (since 0.5.0), convert_timespec_time,
  debug_utils, EXT_local_floor, MND_headless: an unoffered required extension fails `xrCreateInstance`
  exactly as no runtime does. The graphics names are spelt out (their macros sit behind
  `XR_USE_GRAPHICS_API_*`, which would pull in Vulkan and GL headers for nothing).
* **Action sets and actions are real handles**, because engines create them before asking for a
  system. Handles are checked against a live list, so a stale one is HANDLE_INVALID, not a crash.

Verified: `tests/probe.c` through the real loader (run by the recipe at build time, and shipped as
`/usr/libexec/openxr-sim/openxr-sim-probe`): instance with vulkan_enable2 + opengl_enable +
debug_utils, runtime named openxr-sim (LOS until 0.7.0), headset FORM_FACTOR_UNAVAILABLE, no events, paths, an action set
before any system, a system call without a system answered rather than crashing.

## The simulated headset (0.2.0, 2026-10-21)

The owner's second half of the 2026-10-01 request: an application's two eyes in a window, the headset and
both controllers moved with the mouse and keys. The design, the window and the keys are DESIGN.md; what
follows is what is not obvious from it.

* **The simulator is the application's child** (`sim_open`, runtime/device_sim.c): posix_spawn at
  `xrCreateSession`, the memory file on descriptor 3 and the socket on 4 (both first lifted to 10 or
  above, so the dup2s cannot overwrite each other), its own session id, the default signal mask and
  dispositions, and `LD_PRELOAD`/`LD_LIBRARY_PATH` left out of its environment -- an application's
  overlay or its bundled libraries are not the simulator's. It is found at `OPENXR_SIM_EXE`, else
  `/usr/bin/openxr-sim`; without one there `xrGetSystem` answers FORM_FACTOR_UNAVAILABLE, as the
  null device does.
* **The socket is the lifetime.** The runtime sends a byte per frame; the simulator never sends. Either end
  closing ends the other: the window closed is `STOPPING` for the application (`session_poll`), the
  session destroyed closes the window (`App::on_frame_ready`). The runtime gives the child half a second
  to go (`reap`) and otherwise leaves it to the application's own exit, rather than blocking a frame.
* **Frames are three slots** in the memory file (common/sim_protocol.h): the runtime writes one that is
  neither published nor being read, so neither side waits. The simulator converts straight out of the
  slot to what the window will show -- four samples a pixel, linear values to sRGB, GL's rows turned
  over (`convert_eye`, sim/link.cpp) -- so a 2048-pixel eye is never copied whole a second time.
* **Wine**: wineopenxr passes the Windows application's Vulkan calls to the Linux loader in the same
  process, so the runtime runs inside the Wine process and the simulator is a child of it. Its extension
  list parsing (`parse_extensions("")`) gives no extensions, so the runtime answers empty lists for
  `xrGetVulkanInstanceExtensionsKHR`/`DeviceExtensionsKHR`. Not yet run with a Windows OpenXR program.
* **A Windows OpenGL program's session** (0.2.1, 2026-10-21): a Windows OpenGL program under Wine failed
  `xrCreateSession` with GRAPHICS_DEVICE_INVALID. It renders in OpenGL, saw `XR_KHR_opengl_enable` (which
  wineopenxr passes through), and gave an `XrGraphicsBindingOpenGLWin32KHR`; wineopenxr converts only
  Vulkan, D3D11 and D3D12 bindings, so the Win32 one arrived unchanged and the runtime knew no such type.
  Behind WGL, Wine's context is a host EGL or GLX one, current on the calling thread, with the host's
  texture names -- so the runtime now takes the Win32 binding as "the current context" and, for every
  binding but MNDX's EGL one, uses whichever of EGL and GLX has a context current (`current_gpa`,
  runtime/swapchain_gl.c). A session that fails for want of a binding or a context now says why on stderr.
  Tested by `sim_client --gl-win32` (an EGL context named only by an empty Win32 binding): 0.2.0 gives
  exactly the -38 seen; 0.2.1 runs it through. Not yet run inside Wine itself.

Verified (2026-10-21): `tests/sim-test.sh`, Vulkan on lavapipe and desktop GL on EGL surfaceless
(llvmpipe), the latter also through a Win32 binding: FOCUSED, the Touch profile from the client's suggestions, the simulator's scripted head turn
in `xrLocateViews` (30.0 degrees) and its trigger and A in the client's actions, both eyes' pixels
(colour, and the square at the top left in both APIs), the window drawn with no layout faults. In a
nested headless compositor the window mapped (app id `openxr-sim`), drew about 600 frames, and was
gone within two seconds of the session ending. The recipe renders the idle window at build time; the
end-to-end test needs lavapipe, so it is run by hand.

## Hand tracking (0.4.0, 2026-10-23)

The owner asked for `XR_EXT_hand_tracking`. `XR_EXT_hand_interaction` (hand poses and pinch as actions) was
proposed alongside and not asked for then; it came in 0.5.0 (below). The design is in DESIGN.md, "Hand tracking". What is
not obvious from it:

* **The grip pose turned.** Until 0.4.0 the grip pointed where the aim did (its -Z ahead), which is a
  handle lying flat. A hand closed round that has its wrist above its knuckles. `oxs_hand_poses` now
  raises the grip 60 degrees, as a controller held out is; the aim pose did not move, so pointing and
  every existing test are unchanged, but an application drawing a controller model at the grip now sees
  it upright. The simulator's yaw and pitch are still the pointing.
* **A full curl does not close the hand into the handle.** A uniform fist on one shared angle put the
  little finger's last joint 5 mm from the handle's axis, inside it. Each finger now has its own angles,
  fitted so all three of its joints sit 2.7 cm from the axis (a 3.6 cm handle, a finger 9 mm thick).
* **The test cannot tell curl by "below the palm" alone.** A fingertip in a full fist comes back up to
  the palm, so the check is the tip nearer the palm than the open hand's AND on the palm's side.

Verified (2026-10-23): `tests/sim-test.sh`, all six runs; the new `hands` run checks, after the scripted
move, every joint of both hands located and tracked, every bone along its joint's -Z, each palm within
6 cm of its grip with the back of the hand outwards and the thumb inside, the open left hand reaching
ahead, the right index curled (8.5 cm from the palm against the left's 12) and the right thumb down,
and that 25 joints, no hand, and a destroyed tracker are refused.

## Hand interaction (0.5.0, 2026-10-25)

The owner said yes to `XR_EXT_hand_interaction` after hand tracking. The design is in DESIGN.md, "Hand
interaction". What is not obvious from it:

* **Its profile is the last preference, not the first.** Applications that support hands usually suggest
  a controller's bindings as well; the simulated device is a controller, so the controller's win and the
  hands' profile is what an application that knows only hands gets.
* **The poke and pinch do not follow the curl.** Taking them from the hand-tracking skeleton as it is
  curled would put the poke on the index wherever the trigger has bent it -- and a press would move the
  point being pressed with. They are the open hand's, a fixed offset; with hand tracking enabled too, the
  open hand's joints are exactly where the poses say.
* **`_ext` comes off the component names** in `xrGetInputSourceLocalizedName` (`capitalised`), so it
  says "Pinch" and not "Pinch_ext".

Verified (2026-10-25): `tests/sim-test.sh`, all seven runs. Every earlier run now also checks that the
hands' profile and a controller's `poke_ext/pose` are refused without the extension. The new
`hand-interaction` run (with `--hands`) suggests only the hands' profile and checks it is the one chosen,
the scripted trigger arriving as the right pinch's value and its `ready_ext` true, both hands' pinch and
poke located and tracked a hand's length from the grip, the poke's +Y the back of the finger and -Z away
from the hand, the pinch's +Y across the back of the hand, the left hand's poses the right's mirrored
(within 1 mm, though the right index is curled), and the open left hand's poke on its index tip's surface
and its pinch between the tips (both 0.0 mm off).

## Composition layers (0.3.0, 2026-10-23)

The owner asked for OpenXR layers to be supported. "Layers" there meant composition layers, not API layers.
Until then `los_EndFrame` showed only the last projection layer and accepted everything else without
drawing it. The design is in DESIGN.md, "Composition layers". What is not obvious from it:

* **Composited first in the runtime, then moved to the simulator.** A runtime-side version (in
  `xrEndFrame`, at the application's eye size) passed every check. Measured at 1024 pixels, it cost
  1.7 ms per eye just to copy, 6.2 ms per eye with one quad, and 50 ms per eye for a full-sphere
  equirect, all on the application's thread. In the simulator, at the window's 466 pixels, the same
  costs 0.75 and 9 ms per eye, and only the window's frame rate pays.
* **A frame of one plain projection layer takes the old path, unchanged.** It is shown as it is. Only a
  projection layer that is unpremultiplied with its alpha enabled, or that has a colour scale, changes
  colour over nothing; the three original sim-test runs prove that path did not move.
* **The shared memory's version is 2.** Each slot holds every layer for each eye (`struct
  oxs_shm_layer`), the two cameras, and a 32 MiB arena for the sub-images after the eyes.
  * A sub-image over 2048 pixels a side is subsampled to fit.
  * One image both eyes see is copied once.
  * The eye the frame is seen through is not copied again.
  * A layer that does not fit is left out and said once on stderr.
  * The simulator checks every offset against the arena before reading it.
  * Runtime and simulator must come from one package; `Link::attach` refuses another version.
* **Validation got stricter.** An unknown layer type, or a cylinder or equirect without its extension
  enabled, is now `XR_ERROR_LAYER_INVALID` where it used to be ignored. That is what the specification
  asks; a runtime that ignores a layer is how a menu goes missing without anyone noticing.
* **Not done:** cube layers (no six-face swapchains), equirect version 1, and depth (accepted, never
  read). A projection layer other than the first is drawn through its own field of view from the
  eye's position, not reprojected from its own.

Verified (2026-10-23): `tests/sim-test.sh`, all five runs. The new `vulkan-layers` and `gl-layers` runs
place a STAGE quad where the head is turned, a left-eye-only cylinder, a right-eye-only equirect and a
half-transparent unpremultiplied quad. They check:

* each layer's colour where it lands;
* the blue corner of each layer's image (not mirrored, not upside down, in either API);
* each layer absent from the eye it is kept from;
* the blend (255 188 188 over red, the exact linear-light result).

## The MCP server and the control socket (0.6.0, 2026-10-29)

The owner asked for "an MCP for los openxr simulator": an agent that can see what an OpenXR application
draws and drive the headset and controllers. The protocol and tools are DESIGN.md, "The control socket" and
"The MCP server". What is not obvious from them:

* **Abstract, not a file in $XDG_RUNTIME_DIR.** The simulator is the application's child, so it is inside
  the application's sandbox, which may not let it make a file there. LOS's sandbox has no network
  namespace, so an abstract name is reachable from outside it. That also makes it reachable by every user,
  so `accept_client` closes any connection whose `SO_PEERCRED` uid is not ours (not tested against a
  second user). Not yet tried either: a simulator under an application started from the menu, inside
  los-shell's sandbox, reached from outside it; every test so far launched the application from the server.
* **The name carries a random id, not a pid**: the simulator's pid is the application's pid namespace's.
* **The server's stdout is the protocol.** A stray line there drops the server with no reason given, so
  `launch` sends the application's output to a log, never to ours, and double-forks it into its own
  session: it outlives the server, which a client restarts at will.
* **`press` holds for 150 ms**, so a frame sees it: an application samples its actions once a frame.

Verified (2026-10-29) in a headless compositor (its own runtime dir, no modules) with the dev build, all
of it through `openxr-sim-mcp` on stdio: `launch` started `openxr-sim-client` and returned its
simulator; `screenshot` gave both eyes (red left, green right, each with its blue square at the top left);
`turn headset 30` and `hold right trigger a` reached the client, whose own checks passed (the head turn in
`xrLocateViews`, the trigger and A in its actions, FOCUSED); a left hand read with `get_poses` and written
back with `set_pose`, with the head turned 30 degrees and moved, moved 0.000 mm; a bad control name was
refused; the simulator was gone from `list_simulators` once the session ended; and a launched application
and its simulator outlived the server. `tests/sim-test.sh` still passes (headless mode does not open the
socket).


## Its own repository, and the window rebuilt (0.7.0, 2026-11-04)

The owner: "split out our openxr simulator in to its own repo, seperate it from LOS code, and give it
windows support", with GLFW and Clay for the window. LOS's names went: `los-openxr*` is `openxr-sim*`,
the library `libopenxr_sim`, the identifiers `oxs_`, the variables `OPENXR_SIM_*`, the configuration
`~/.config/openxr-sim/config` (the old variable and file read after the new ones until 2027-05-01). The
shared memory's version went to 3, so a simulator from before the split refuses this runtime's region and
says so, rather than misreading it.

* **The runtime's system calls are one file per platform** (`runtime/plat.h`): the child's start, its
  shared memory and wake channel, symbol lookup, the clock, the configuration's path. pthreads are
  `common/threads.h` (SRW locks and condition variables on Windows) and the shared memory's atomics have an
  MSVC path (`common/sim_protocol.h`) -- x86-64 only, where TSO makes acquire and release compiler barriers.
* **The window was los-ui on los-display**; it is GLFW, Clay and a software canvas now (DESIGN.md, "The
  window's toolkit"). The panel's look is the old one's -- cards, a segmented control, push buttons, badges
  -- in one dark theme of its own (0.7.1: the desktop's). The title bar is the system's.
* **No glBegin.** Mesa 26.2's llvmpipe on Wayland freed a buffer of the window's surface and read it again
  at the next swap after any `glBegin`/`glEnd` -- the window died on its first frame. A twenty-line GLFW
  program showed it with nothing of ours in it; the textured quad is drawn from vertex arrays, as old (1.1).
* **A variable font has no bold for stb_truetype**: it reads only the default instance. Noto Sans here is
  one file whose Bold is a named instance (fontconfig's index, high 16 bits), so that face is emboldened
  by smearing each glyph a twentieth of its size to the right (`Fonts::embolden`). Segoe UI Semibold is a
  file of its own.
* **Resizing draws in the loop, not the callback** on Linux: GLFW calls it from inside libdecor's
  configure. On Windows it draws at once, because the loop does not run while the window is being dragged.

Verified (2026-11-04), on Linux: the probe through the real loader; `tests/sim-test.sh`, 173 checks, all
seven runs (Vulkan, GL, GL through a Win32 binding, layers in both APIs, hand tracking, hand interaction),
every window drawn with no layout faults; the fault check itself fires on a window too small for its text
(`--layout-check --size 160x300`: 2 faults). In a nested headless compositor, the real window over GLFW
(Wayland, llvmpipe): the test client's session reached FOCUSED, `turn headset 30 0` and `hold right
trigger a` over the control socket reached the client's own checks, and the framebuffer read back after a
swap was the panel and both eyes as `--screenshot` draws them.

## The desktop's frame and colours (0.7.1, 2026-11-04)

The owner, on 0.7.0's window wearing libdecor's bar on LOS: "doesn't the LOS-style bar come from the
provided wayland window client decoration? we should use LOS palette colours ideally, but the package
shouldn't link against any LOS code, could it grab them from xdg or something similar?"

* **GLFW draws with libdecor whenever it can load it, and libdecor never asks the compositor for a frame**
  (libdecor 0.2.5, `src/libdecor.c`: it records the compositor's first answer and only re-asserts server
  side if that answer already was). LOS's compositor draws a frame only for a window that asks, so the
  window was told to draw its own and got libdecor's bar. GLFW without libdecor asks for server side
  through xdg-decoration itself. So `compositor_draws_frames()` (`sim/os_posix.cpp`) looks for
  `zxdg_decoration_manager_v1` with one registry round trip before `glfwInit`, and only then is
  `GLFW_WAYLAND_DISABLE_LIBDECOR` set: GNOME offers no manager and keeps libdecor's bar; KDE, sway and LOS
  draw their own.
* **The colours come from the XDG portal**, the cross-desktop answer (there is no Wayland protocol for
  it): `org.freedesktop.portal.Settings.ReadOne("org.freedesktop.appearance", "color-scheme" |
  "accent-color")`, falling back to `Read` on a portal too old for `ReadOne`. libdbus on a private
  connection, with auto-start ON and 1 s: in the VM nothing had started the portal yet, and with auto-start
  off (as los-shell asks, for a reason a shell has and an application does not) the window was told "no
  preference" by a portal that, once started, said dark. Not reading LOS's own configuration is the point:
  it would tie this project to that file's format.
* **On LOS the accent does not arrive yet.** LOS's portal backend is xdg-desktop-portal-gtk 1.15.3, which
  answers `color-scheme` and `contrast` and has no `accent-color` at all (`src/settings.c`); NotFound, and
  the window uses its own (LOS's default, terracotta). GNOME's and KDE's backends answer it.
* **The neutrals no standard publishes**: surface, card, field, text, separator are LOS's
  (`los::palette_from`) as numbers in `sim/ui.cpp`. The text on a filled control is chosen by the fill's
  luma (`ui::on`), since the accent is anyone's now.
* **Asked once, at start**: a window open across a change of scheme keeps the one it started with.

Verified (2026-11-04): `--screenshot` and `--layout-check` in both themes (0 faults); `tests/sim-test.sh`
with `OPENXR_SIM_THEME=light`, all runs passing; under a nested headless los-compositor, `WAYLAND_DEBUG`
showed `set_mode(2)` answered by `configure(2)` -- a server-side frame -- and with a stand-in portal on the
private bus answering light and (0.47, 0.565, 0.533), the window logged (`OPENXR_SIM_LOG=1`) "scheme:
light; accent #789088"; with no portal, "no preference" and its own accent. In the LOS VM, installed from
the package: the compositor's title bar over the window (once los-compositor 0.54.11 fitted a window too
big for the screen inside its frame, not under it), and "scheme: dark".
