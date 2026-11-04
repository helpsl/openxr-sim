# openxr-sim: the simulated headset (design)

Written for LOS, where it began as `apps/los-openxr`; its own repository since 0.7.0 (2026-11-04), for
Linux and Windows. The owner, 2026-10-01: "a dummy openxr runtime package for LOS, which when identified by an openxr app will
just come back with a disconnected openxr headset. Later on I might expand this to provide a simulated OpenXR
device such as something like https://github.com/meta-quest/immersive-web-emulator/". The first half is
0.1.0 (`NOTES.md`). This is the second, asked for on 2026-10-21: an OpenXR application runs as it would on a
headset, its two eyes appear in a window on the desktop, and the headset and both controllers are moved and
pressed with the mouse and keyboard.

## Two parts, two processes

* **The runtime** (`libopenxr_sim.so`, C) is loaded INTO the application by the OpenXR loader. It owns
  everything OpenXR: sessions and their states, frame timing, swapchains in the application's own graphics
  API, spaces, views, actions and their bindings. It knows nothing about windows or input devices.
* **The simulator** (`openxr-sim`, C++: GLFW, Clay, drawn in software -- "The window's toolkit") is a desktop window. It shows what the application
  rendered, and it is where the headset's and the controllers' poses and buttons come from.

The runtime starts the simulator when a session is created and hands it two things: a shared-memory
region (frames one way, the simulated devices' state the other) and a wake channel (wake-ups, and "the
window was closed") -- on Linux a memfd and a socket pair, descriptors 3 and 4 in the child; on Windows a
file mapping and an anonymous pipe, inherited and named on its command line (`runtime/plat.h`). A child of the application, not a service found by name, so it works the same inside a confined
application's view, under Wine (wineopenxr hands every call to the Linux loader in the same process), and
with nothing installed but the package. Closing the window is the person taking the headset off: the
session is asked to exit (`STOPPING`), as `xrRequestExitSession` would.

## Choosing the device

`OPENXR_SIM_DEVICE` (`null` or `sim`), else `device =` in the configuration file --
`$XDG_CONFIG_HOME/openxr-sim/config` (`~/.config/openxr-sim/config`), `%APPDATA%\openxr-sim\config` on
Windows -- else `null`. Until 2027-05-01 the names from before the split are read too, after these:
`LOS_OPENXR_DEVICE`, and `~/.config/los/openxr` when the new file does not exist. The default
stays the disconnected headset: a simulator window appearing because a game probed for VR would be a
surprise. `openxr-sim --use` / `--unuse` write the setting.

## What the simulated headset is

* Stereo, `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`, opaque blend. Each eye 1024x1024 recommended,
  4096 at most, one sample, 90 degrees of field of view (symmetric, so the window can show it undistorted).
  1024, not a real headset's 1440 or more: every pixel is copied back to memory each frame, and the
  window shows the two eyes at well under that anyway. Up to 2048 of each eye reaches the window.
* 72 Hz. `xrWaitFrame` paces the application on its own clock: the simulator is not a display with a
  vblank, and a window that is hidden must not stop the application.
* Interpupillary distance 63 mm; the head starts 1.6 m above the floor, at the stage's origin, facing -Z.
* Reference spaces VIEW, LOCAL, STAGE and LOCAL_FLOOR. LOCAL is where the head was when the session began.
* Two controllers. The application's suggested bindings decide which controller they claim to be: the
  first suggested of Touch, Touch Plus, Index, Vive, Windows Mixed Reality and the simple controller, else
  whatever it suggested first (`input_choose_profile`, runtime/input.c). A binding is answered by what its path names (`trigger`, `squeeze`, `thumbstick`, `a`,
  `menu`, `grip/pose` ...), so one simulated controller serves every profile.

## Frames: copied, not shared

At `xrReleaseSwapchainImage` the runtime copies the image to memory, in the application's own API: a
Vulkan copy on the application's queue, or a GL framebuffer read with the application's context current
(which OpenXR requires of it at that call). `xrEndFrame` copies the first projection layer's two eye
rectangles into the shared file and wakes the simulator. Any other layer's image goes with them (next
section).

OpenGL's context is the one current on the calling thread, found through EGL or GLX, whatever binding
named it -- including a Windows program's Win32 binding, which arrives from Wine unchanged and whose
context behind WGL is a host one.

A copy costs a few milliseconds a frame and works for every API, every driver, Wine's and llvmpipe's
included. Sharing the images themselves (external memory into the simulator's own Vulkan) is the step if
the copy is ever the bottleneck; it is not needed to see an application work. So:
* only 8-bit RGBA/BGRA colour formats are offered (sRGB first), and depth formats, which are kept but never
  read;
* array swapchains (multiview: one image, a layer per eye) are read layer by layer;
* the simulator converts a non-sRGB image (linear values) to sRGB as it draws.

## Composition layers

Since 0.3.0, every layer an application submits is drawn, in the order submitted, with the specification's
blending (10.6.2): the core's projection and quad, `XR_KHR_composition_layer_cylinder`,
`XR_KHR_composition_layer_equirect2`, and `XR_KHR_composition_layer_color_scale_bias` on any of them.
Before that only the last projection layer was shown, and an application's menu in a quad never
appeared. The cube layer is not offered: a cube map is a swapchain of six faces, which `swapchain.c`
does not make.

**The simulator composites, not the runtime.** That is where a headset's compositor runs too: in its own
process. A frame of one plain projection layer goes as it always did. For any other frame, the runtime
does three things:

* checks every layer (an unknown type, or one whose extension was not enabled, is
  `XR_ERROR_LAYER_INVALID`);
* resolves each layer for each eye: its pose in STAGE at the display time, and its sub-image;
* copies the sub-images into the frame's slot (`sim_protocol.h`, version 2).

The simulator then draws each eye at the size the window shows it (`common/composite.c`). Each of its
pixels is a ray from the eye: into a quad's plane (front face only), the inside of a cylinder, a sphere
or, with no radius, a direction for an equirect, or another projection's view. Only the rectangle a
layer's outline projects to is visited.

The eye is seen through the first projection layer's views, or from the head if there is none.

Measured, a 466-pixel eye (the window's): a metre quad at 1.5 m costs 0.75 ms; a full-sphere equirect
9 ms. Done in `xrEndFrame` at the application's 1024 pixels, which was tried first, those were 3.6 and
43 ms, on the application's own frame.

## Hand tracking

Since 0.4.0, `XR_EXT_hand_tracking`: the 26 joints of each hand (`runtime/hand.c`), with no change to the
simulator. It has no hands of its own, so a hand is a fixed skeleton held round each controller:

* **Placed on the grip pose.** The skeleton is built in its palm's frame -- every joint's -Z down its bone
  towards the tip, +Y out of the back of the hand, the extension's own convention -- and laid on the
  controller (`palm_on_grip`): the palm against the handle's side, the handle across it, 3 cm under it
  and 5 cm towards the fingers. The left hand is the right one mirrored in the grip's YZ plane.
* **The grip is OpenXR's grip** (`oxs_hand_poses`, common/xrmath.h): its -Z along the handle through the
  fist, rising 60 degrees above where the hand points; the aim pose is the pointing itself, 5 cm ahead.
* **Curled by what is held.** The trigger curls the index, the grip the other three, a face button or
  the stick puts the thumb down. A full curl lays every finger joint 2.7 cm from the handle's axis, each
  finger by its own angles (`struct finger`, `fist`); at rest the fingers are open, slightly bent.
* **Tracked while the session is FOCUSED**, as actions are; otherwise `isActive` is false. No velocities
  are reported (nothing simulated moves on its own), so a chained `XrHandJointVelocitiesEXT` has none.
* The functions are there only with the extension enabled (`xrGetInstanceProcAddr` says
  `XR_ERROR_FUNCTION_UNSUPPORTED` otherwise), and `XrSystemHandTrackingPropertiesEXT` says yes then.

## Hand interaction

Since 0.5.0, `XR_EXT_hand_interaction` (spec version 2): the hands as an interaction profile, answered by
the same simulated controller (`runtime/input.c`), with no change to the simulator.

* **`/interaction_profiles/ext/hand_interaction_ext` is chosen last.** It comes after every controller's
  profile in `PREFERRED_PROFILES`: the device is a controller, so an application that also suggested a
  controller's bindings gets those, and one that suggested only hands gets hands.
* **The values are the controller's.** `pinch_ext/value` and `aim_activate_ext/value` are the trigger,
  `grasp_ext/value` the grip; each `ready_ext` is always true -- a controller's hand is always in a pose to
  pinch -- so a value is never reported while its ready is false, as the extension requires.
* **The pinch and poke poses are a fixed offset from the grip** (`hand_pinch_poke`, runtime/hand.c), taken
  from the open hand of "Hand tracking": the poke on the index tip's surface (the tip pushed its radius
  along its -Z), oriented as the tip -- +Z back along the bone, +Y the back of the finger; the pinch
  midway between the index and thumb tips, its -Z pointing away from the midpoint of the two proximal
  joints and its +Y towards the thumb's side (up, when the palms face each other). Fixed rather than
  following the curl, as a real runtime keeps them steady while the hand closes: a poke that moved with the
  trigger would jump as it is pressed.
* **`pinch_ext/pose` and `poke_ext/pose` on the controllers' profiles** too, which the extension adds to
  every one of them; they answer the same poses.
* **Only while enabled.** Without the extension, suggesting its profile, or any binding under
  `/input/pinch_ext/`, `/input/poke_ext/`, `/input/aim_activate_ext/` or `/input/grasp_ext/`, is
  `XR_ERROR_PATH_UNSUPPORTED`. `palm_ext/pose` is not offered: it needs `XR_EXT_palm_pose`, which is not.

## The simulator window

Both eyes side by side, and a side panel: which device the mouse and keys move (headset, right hand, left
hand), each hand's buttons as buttons that can be held, the poses, the application's name, its frame rate
and session state, and the keys.

* Drag in the view: turn the selected device. W A S D: move it, Q E down and up, Shift slower.
* Tab: the next device. R (or "Put everything back"): every device back where it started -- the same poses
  the runtime answers before the simulator has written any (`oxs_tracking_home`, common/xrmath.h).
* Space trigger, G grip, Z primary button (A/X), X secondary (B/Y), M menu, arrow keys the thumbstick, C
  the thumbstick's click: on the selected hand, or the right hand while the headset is selected.

The panel's buttons hold the same controls with the mouse, for as long as the button is down. A key or a
button let go while the keyboard is in another window never arrives, so losing the keyboard lets go of
everything. Shift is read from each key's own event: pressing it while W is already held slows the walk
at the next key, not at once.

The hands go where the head goes: each is kept relative to the body -- the head's position, turned by
its yaw alone -- so walking carries them along and looking down does not (`Rig`, sim/rig.hpp).

Keys are GLFW's key codes, which are positions: W is where W is on a US keyboard, whatever the layout says
it is -- what a game's WASD means too.

Started by itself with no session, the window is the one setting: "Use the simulated headset", which
writes `device =` as `--use`/`--unuse` do.

## The window's toolkit (0.7.0)

GLFW makes the window (Wayland, X11, Windows) and its OpenGL 1.1 context; Clay lays the frame out; the
frame is drawn in software into one buffer (`sim/canvas.hpp`) and shown as one texture. Text is
stb_truetype over the system's own fonts: fontconfig's sans-serif, bold and monospace on Linux, Segoe UI,
Segoe UI Semibold and Consolas on Windows (`sim/os.hpp`) -- the only platform code in the window.

* **In software** because the window is rounded rectangles, some text and two images: the same pixels with
  a GPU, without one, under a remote desktop, headless. `--screenshot`, the headless test's `window.png`
  and the control socket's `shot` are that buffer, not a capture.
* **Tokens, not values** (`sim/ui.hpp`): a theme by role, six type roles, a spacing scale. The app chooses
  roles, never a colour or a size.
* **The desktop's look** (`sim/os.hpp`, `desktop_appearance`; `sim/ui.cpp`, `set_theme`): light or dark and
  the accent are the desktop's, asked of it in the standard way -- on Linux the XDG portal's
  `org.freedesktop.appearance` `color-scheme` and `accent-color`, which GNOME, KDE and LOS answer
  (LOS with its own backend: NOTES.md, 0.7.1); on Windows
  the personalisation settings. The neutrals are LOS's palette's, copied as numbers (nothing of LOS's is
  linked), so on LOS the window and the compositor's frame round it are one surface. No answer: dark, and
  LOS's default accent. `--screenshot`, `--layout-check` and the headless test do not ask: the same picture
  on every machine; `OPENXR_SIM_THEME=dark|light` picks for them, and overrides the desktop.
* **Sizes are logical**: laid out at the window's size over its content scale, drawn at the framebuffer's.
* **Layout faults** are what the headless test counts: Clay's own errors, and any element outside its parent
  on an axis the parent does not clip -- a word too long for its card, a row wider than the panel.
* **Three threads in headset mode**: the window's does everything; the link's only waits on the wake
  channel and wakes it; the control socket's hands each command to it and waits for the answer.
* **The desktop's decorations**: no title bar of its own. On Wayland the compositor's frame wherever it
  offers one (xdg-decoration), libdecor's where it does not (GNOME); the native frame on Windows. See
  NOTES.md, 0.7.1, for why GLFW has to be told.

## Platforms (0.7.0)

Linux (x86-64; Vulkan, OpenGL through EGL or GLX, OpenGL ES through EGL), and Windows (x86-64) being
added: D3D11, then OpenGL through WGL, Vulkan, D3D12. Everything the runtime asks of the system is `runtime/plat.h`
(`plat_posix.c`, `plat_win32.c`); the window's, `sim/os.hpp`, `sim/link.cpp` and `sim/control.cpp`. Windows
will be built from Linux with Visual Studio's build tools under Wine.

## The control socket (0.6.0)

A headset-mode simulator also listens on a Unix socket for a program to drive it -- on Linux an abstract
one, `\0openxr-sim/<uid>/<16 hex>`; on Windows a file, `%LOCALAPPDATA%\openxr-sim\<16 hex>.sock`
(`sim/control.hpp` says why each) -- one command a line, one JSON object back, always with `"ok"`
(`sim/commands.cpp`). Commands run on the window's thread, between frames.

| command | answers |
|---|---|
| `status` | app, profile, session state (number and name), fps, frames, each eye's size, haptics, the selected part |
| `poses` | headset, left, right: position (STAGE metres), yaw (degrees, left positive), pitch; each hand's held controls |
| `set PART X Y Z YAW PITCH` | the pose as `poses` reports it; a hand's is turned into the body's frame, so read-and-write-back moves nothing |
| `move PART FORWARD RIGHT UP` | metres, as W A S D Q E move it |
| `turn PART YAW PITCH` | degrees, as a drag turns it |
| `hold HAND CONTROL...` / `release HAND [CONTROL...]` | trigger grip a b x y menu stick stick-up/down/left/right; held alongside the person's own |
| `reset` | everything home, as R does |
| `shot [MAX]` | both eyes side by side, each at most MAX pixels (default 512), as base64 PNG |

PART is `headset`, `left` or `right`. A connection from another user is closed unanswered.

## The MCP server (0.6.0)

`openxr-sim-mcp` is a Model Context Protocol server over stdio (`mcp/main.cpp`): an agent registers
it (`claude mcp add openxr-sim -- openxr-sim-mcp`) and gets the simulators as tools. It finds them in
`/proc/net/unix` and connects per call. (Linux only so far.) Tools: `list_simulators`, `launch` (an application started with
`OPENXR_SIM_DEVICE=sim`, detached, its output in `~/.local/state/openxr-sim-mcp/`, waiting for its simulator
to appear), `status`, `screenshot` (an image), `get_poses`, `set_pose`, `move`, `turn`, `press` (hold,
150 ms, release), `hold`, `release`, `reset`. Each takes `simulator` (an id or its start) when more than one
is running.

## Testing it

`tests/sim-test.sh` runs the whole chain with no window and no GPU: `tests/sim_client.c`, an OpenXR
application, runs a session through the real loader on lavapipe (and then, with `--gl`, in desktop
OpenGL on an EGL surfaceless context through `XR_MNDX_egl_enable`), drawing the left eye red and the right
green, each with a blue square at its top left. `OPENXR_SIM_HEADLESS=DIR` in the simulator's
environment makes it the test's headset: no window; once frames arrive it turns the head 30 degrees left
and holds the right trigger and A, which the client must see through `xrLocateViews` and its actions
(with `--layers`, the client also submits a quad, a cylinder, an equirect and a half-transparent quad, and
the script checks where each one landed, its image's corner, the eye it is kept from, and the blend;
with `--hands`, it locates both hands' joints and checks each bone along its joint's -Z, each palm on its
grip with the back of the hand outwards, the open hand reaching ahead, and the right index curled by the
trigger and its thumb down for A; with `--hand-interaction`, it suggests only the hands' profile and
checks the trigger arriving as the pinch, the pinch and poke poses against the grips, and -- with
`--hands` too -- against the open hand's joints);
when the session ends it writes the last frame's eyes (`left.ppm`, `right.ppm`), the window it would have
drawn (`window.png`, and its layout faults) and a `summary`. The script then checks the pixels -- the
square's corner is what catches a picture flipped or eyes swapped.
