#!/bin/bash
# THE SIMULATED HEADSET, END TO END, with no window and no GPU:
#
#   tests/sim-test.sh [BUILD_DIR] [OUT_DIR]
#
# A WINDOWS BUILD (BUILD_DIR holding openxr-sim-client.exe) is run under Wine in WINEPREFIX (default
# los-wine's), with no display: the client opens the runtime itself rather than through a loader
# (sim_client.c, "ON WINDOWS"), Vulkan only -- the OpenGL runs need EGL and are left out.
#
# An OpenXR application (sim_client.c) runs a Vulkan session on lavapipe through the real loader, against
# the runtime in BUILD_DIR (test_runtime.json), and then again in desktop OpenGL on an EGL surfaceless
# context (llvmpipe), whose readback comes back bottom to top; the runtime starts openxr-sim from BUILD_DIR, which
# OPENXR_SIM_HEADLESS turns into the test's headset: it makes the scripted move and, when the session
# ends, writes the last frame's eyes and the window it would have shown into OUT_DIR. Then the pixels are
# checked: the left eye red, the right green, a blue square at each one's top left. OUT_DIR/vulkan and
# OUT_DIR/gl hold each run's. And once more as Wine hands a Windows OpenGL program's session on: the same
# context, named only by a Win32 binding with no handles in it (OUT_DIR/win32). And in both APIs with
# composition layers over the eyes (sim_client --layers; OUT_DIR/vulkan-layers, OUT_DIR/gl-layers): each
# layer's colour where it should land, its blue square where its image's top left should be, the eye it
# is kept from, and a half-transparent layer blended. And with XR_EXT_hand_tracking (sim_client --hands;
# OUT_DIR/hands): both hands' joints against the grips, the scripted trigger and A in the right hand's.
# And with XR_EXT_hand_interaction (sim_client --hand-interaction; OUT_DIR/hand-interaction, with hand
# tracking as well): only the hands' profile suggested, the trigger as the pinch, the pinch and poke poses
# against the grips and the open hand's joints.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${1:-$here/../build}"
out="${2:-$build/sim-test}"
build="$(cd "$build" && pwd)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
icd=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
[ -f "$icd" ] || { echo "no lavapipe ($icd)"; exit 2; }
rc=0
wine=""
if [ -f "$build/openxr-sim-client.exe" ]; then
    wine=yes
    export WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/los-wine/prefix}"
fi

# A PIXEL OF A PPM: its R G B, decimal.
pixel() {   # file x y
    python3 -I - "$@" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
x, y = int(sys.argv[2]), int(sys.argv[3])
o = (y * w + x) * 3
print(*data[len(data) - w * h * 3 + o:][:3])
EOF
}
expect() {   # what file x y "R G B"
    local got; got="$(pixel "$2" "$3" "$4" 2>/dev/null || echo none)"
    if [ "$got" = "$5" ]; then printf '  %-50s ok\n' "$1"; else printf '  %-50s FAILED (%s, wanted %s)\n' "$1" "$got" "$5"; rc=1; fi
}
# Within 2 of each channel: a blend's rounding.
expect_near() {   # what file x y "R G B"
    local got; got="$(pixel "$2" "$3" "$4" 2>/dev/null || echo none)"
    if python3 -I -c 'import sys; a=sys.argv[1].split(); b=sys.argv[2].split(); sys.exit(0 if len(a)==3 and all(abs(int(x)-int(y))<=2 for x,y in zip(a,b)) else 1)' "$got" "$5"
    then printf '  %-50s ok (%s)\n' "$1" "$got"; else printf '  %-50s FAILED (%s, wanted %s)\n' "$1" "$got" "$5"; rc=1; fi
}
# DIRECT3D UNDER WINE comes two ways. Wine's own: vkd3d for 12, and wined3d for 11 -- which, without a
# display, works only through its Vulkan renderer and even then wants a window for the device's implicit
# swapchain, so 11 is not run that way. And DXVK's and vkd3d-proton's, which is what los-wine gives Windows
# programs when those packages are installed (forge/recipes/los-wine/NOTES.md, apply_dxvk): their DLLs are
# put beside a copy of the client ("$out/dxvk-bin"), where Windows looks first, and set native.
dxvk=/usr/lib/x86_64-linux-gnu/dxvk/x64
vkd3d_proton=/usr/lib/x86_64-linux-gnu/vkd3d-proton/x64
client_dir="$build"
overrides=""
run() {   # api [client arguments]
    local dir="$out/$1"
    mkdir -p "$dir"
    rm -f "${dir:?}"/left.ppm "${dir:?}"/right.ppm "${dir:?}"/window.png "${dir:?}"/summary
    echo "== $1"
    echo "the application:"
    if [ -n "$wine" ]; then
        # The simulator and the runtime are the ones beside the client (plat_win32.c); the folder is Wine's
        # name for it. No display: nothing here needs one, and a program with windows replaces the
        # prefix's display-less server (los-wine).
        env -u DISPLAY -u WAYLAND_DISPLAY WINEDEBUG=-all OPENXR_SIM_DEVICE=sim WINE_D3D_CONFIG=renderer=vulkan \
            WINEDLLOVERRIDES="$overrides" OPENXR_SIM_HEADLESS="Z:${dir//\//\\}" VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" \
            timeout -k 5 60 wine "Z:${client_dir//\//\\}\\openxr-sim-client.exe" ${2:-} < /dev/null || rc=1
    else
    XR_RUNTIME_JSON="$build/test_runtime.json" OPENXR_SIM_DEVICE=sim OPENXR_SIM_EXE="$build/openxr-sim" \
        OPENXR_SIM_HEADLESS="$dir" VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd" \
        EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 \
        timeout 60 "$build/openxr-sim-client" ${2:-} || rc=1   # unquoted: one or more arguments
    fi
    # The simulator outlives the session by as long as it takes to write; its summary is written last.
    for _ in $(seq 100); do [ -f "$dir/summary" ] && break; sleep 0.1; done
    echo "the simulator:"
    if [ -f "$dir/summary" ]; then sed 's/^/  /' "$dir/summary"; grep -q 'layout faults 0' "$dir/summary" || rc=1
    else echo "  wrote no summary"; rc=1; fi
    echo "the pixels:"
    # The eyes are 256 square; the blue square is 64 at the top left. (sRGB-encoded red is 255 0 0.)
    expect "left eye: red"                    "$dir/left.ppm" 200 200 "255 0 0"
    expect "left eye: blue square top left"   "$dir/left.ppm" 10 10 "0 0 255"
    expect "right eye: green"                 "$dir/right.ppm" 200 200 "0 255 0"
    expect "right eye: blue square top left"  "$dir/right.ppm" 10 10 "0 0 255"
    expect "right eye: no square bottom left" "$dir/right.ppm" 10 240 "0 255 0"
    [ -s "$dir/window.png" ] && echo "  the window: $dir/window.png" || { echo "  no window.png"; rc=1; }
}
# WHERE EACH LAYER LANDS in a 256-pixel eye of 90 degrees (sim_client.c, --layers), after the head turn:
#   quad A     both eyes, x and y 96..160 (two pixels over for the eye's offset); blue top left to 128
#   cylinder   left eye, x 88..168, y 80..99; blue for x below about 108
#   equirect   right eye, x 95..161, y 175..215; blue for x below 112 and y above about 193
#   quad B     both eyes, x 32..83, y 179..205; blue (opaque) for x below 58, white at half alpha after
layers() {   # dir
    local d="$out/$1"
    echo "the layers:"
    expect "quad A: yellow, both eyes"            "$d/left.ppm" 148 148 "255 255 0"
    expect "quad A: yellow in the right eye too"  "$d/right.ppm" 148 148 "255 255 0"
    expect "quad A: its blue square top left"     "$d/left.ppm" 108 108 "0 0 255"
    expect "quad A: not mirrored (top right)"     "$d/left.ppm" 148 108 "255 255 0"
    expect "quad A: not flipped (bottom left)"    "$d/left.ppm" 108 148 "255 255 0"
    expect "cylinder: magenta, left eye"          "$d/left.ppm" 128 90 "255 0 255"
    expect "cylinder: its blue at its left"       "$d/left.ppm" 96 90 "0 0 255"
    expect "cylinder: not in the right eye"       "$d/right.ppm" 128 90 "0 255 0"
    expect "equirect: cyan, right eye"            "$d/right.ppm" 140 205 "0 255 255"
    expect "equirect: its blue top left"          "$d/right.ppm" 100 185 "0 0 255"
    expect "equirect: not in the left eye"        "$d/left.ppm" 140 205 "255 0 0"
    expect "quad B: its opaque blue square"       "$d/left.ppm" 45 192 "0 0 255"
    expect_near "quad B: half white over red"     "$d/left.ppm" 70 192 "255 188 188"
    expect_near "quad B: half white over green"   "$d/right.ppm" 70 192 "188 255 188"
}
run vulkan
if [ -z "$wine" ]; then
    run gl --gl
    run win32 --gl-win32
fi
run vulkan-layers --layers
layers vulkan-layers
if [ -z "$wine" ]; then
    run gl-layers "--gl --layers"
    layers gl-layers
fi
run hands --hands
run hand-interaction "--hand-interaction --hands"
if [ -n "$wine" ]; then
    run d3d12 --d3d12
    if [ -f "$dxvk/d3d11.dll" ] && [ -f "$vkd3d_proton/d3d12.dll" ]; then
        client_dir="$out/dxvk-bin"
        mkdir -p "$client_dir"
        cp "$build"/openxr-sim-client.exe "$build"/openxr_sim.dll "$build"/openxr-sim.exe \
            "$dxvk"/d3d11.dll "$dxvk"/dxgi.dll "$vkd3d_proton"/d3d12.dll "$vkd3d_proton"/d3d12core.dll "$client_dir"/
        overrides="d3d11,dxgi,d3d12,d3d12core=n"
        run dxvk-d3d11 --d3d11
        run dxvk-d3d11-layers "--d3d11 --layers"
        layers dxvk-d3d11-layers
        run dxvk-d3d12 --d3d12
        run dxvk-d3d12-layers "--d3d12 --layers"
        layers dxvk-d3d12-layers
    else
        echo "== no DXVK and vkd3d-proton ($dxvk, $vkd3d_proton): Direct3D 11 is not tested"
    fi
fi
exit $rc
