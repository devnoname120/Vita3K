# Local WAW emulator build

Build and deploy with `tools/waw/build-macos.sh`, then launch with
`tools/waw/run-macos.sh`. Deployment always uses
`~/Applications/Vita3K-WAW.app`, bundle ID `org.vita3k.WAWLocal`, and the
Developer ID identity configured in `deploy-macos.sh`. Intermediate build
products are not launch targets; the deployment step signs and verifies the
complete bundle before the launcher will execute it.

The launcher sets `VITA3K_PORTABLE_DIR` to `build/waw-runtime`. This keeps
configuration, emulated storage, logs and caches separate from personal
Vita3K storage. It expects WAW000001 and its data, plus firmware, to have been
installed in this isolated storage. No physical-console operations are used.

The WAW loader must be built with `ENABLE_TTBR_REMAP_NATIVE_RUNNER=OFF` and
`ENABLE_LOWVA_PLAN_INSTALLER=OFF`. Its fixed-address requests are handled by
Vita3K's flat guest allocator through kubridge HLE exports. The TTBR kernel
plugin is not loaded or emulated.

Only one 4096-byte guest page is reserved for null. On hosts with larger pages,
allocating adjacent low guest pages necessarily relaxes hardware protection
on the shared host page; page zero remains reserved in the guest allocator.
Guest addresses remain offsets into emulated RAM, not fixed host pointers.

This is a development configuration. Passing allocator tests is not evidence
that WAW reaches its main screen; verify actual application output after each
relevant emulator change.

For a bounded startup observation, set `VITA3K_WAW_CAPTURE=1` when calling the
launcher. On WAW000001 and BIOS00001 only, the SDL frontend invokes the existing rendered-frame
screenshot function every five seconds, at most 24 times. Images go to the
isolated runtime's screenshots folder. This is optional observation overhead,
not a performance measurement or synthetic main-screen image.

The WAW build disables intermediate Qt bundling (`VITA3K_BUNDLE_QT=OFF`) and
uses `prepare-local-macos.py` before fixed-path deployment. This local-only app
uses the installed Homebrew Qt frameworks, with exact version checks and
provider hashes, rather than inheriting mixed framework/plugin copies. It is
not a self-contained distributable. The final app is still signed and verified
with the original identity; intermediate build products must not be launched.
Vita3K must remain on Vulkan for this task; OpenGL is not an allowed fallback.


## Required flat-port GUI initialization

The emulator changes alone do not correct the WAW menu layout. The port's
TTBR execution gate also installed its existing 480x320 GUI-canvas adaptation;
disabling that gate skipped the adaptation while retaining the 544x960 drawable.
That omission produced the displaced background, undersized title and missing
start prompt. It was not a Vulkan viewport or texture-coordinate bug.

`mach-o-vita-flat-gui.patch` contains the companion change for the separate
`mach-o-vita` checkout. From that checkout, apply it with `git apply` using the
absolute path to this patch, unless the change is already present. It invokes
the existing signature-checked GUI installer after successful GL initialization
only when `ML_ENABLE_TTBR_REMAP_NATIVE_RUNNER` is disabled. TTBR-enabled behavior
is unchanged. Rebuild the flat WAW executable and install that EBOOT into the
isolated emulator storage. Do not install the flat build on a physical Vita.

The corrected title screen was compared with a physical-Vita reference and
confirmed again in a matched control/fixed run using the same Vulkan emulator
and initial Documents files. It restores the centered 816x544 canvas with
72-pixel side margins and the visible start prompt. This does not establish
correct gameplay rendering, performance or compatibility with other games.
The tested flat-port EBOOT SHA-256 is
`78c2e1e7ed593cda1947a8ff279d0887f877c99aa46f7e908e7d59497a917dbe`.

## Focused regression checks

After building the emulator and deploying the signed local app, run from the
Vita3K checkout:

```sh
mkdir -p build/waw-checks
python3 tools/waw/test-app-param-string.py
python3 tools/waw/test-low-callbacks.py
python3 tools/waw/test-pfs-adapter.py
python3 tools/waw/test-portability.py
python3 tools/waw/test-system-events.py
```

These are local macOS checks against the actual source fragments. The low-address
callback check links the current Xcode build's static libraries; the portability
check also creates an instance using the deployed MoltenVK library. The app-param
and PFS checks enable AddressSanitizer and UndefinedBehaviorSanitizer. The normal
`mem-tests` target contains the low-page allocation regression. The separate port
checkout contains the GUI-initialization and existing GUI/geometry tests.

The signing scripts intentionally target this local fork's fixed identity and
bundle path. Firmware, the shader compiler, WAW assets and savedata are not part
of this branch. Existing local submodule revisions were not rewritten by this
work; use a compatible dependency checkout when reproducing the build.


## Idle system-event polling

Both public and internal app-manager event exports now return an empty event
when no event queue is implemented. The previous stubs fabricated ON_RESUME
on every poll, repeatedly invoking guest lifecycle callbacks and, in BioShock,
preferences synchronization. Null output is rejected and the complete output
structure is cleared. The regression extracts and tests both exported routines.
This remains a no-events stub, not implementation of real suspend/resume delivery.
The change does not by itself establish a BioShock frame-rate improvement.


## AvPlayer movie timing and looping

Decoded audio and video timestamps are rescaled from each stream's time base
into milliseconds. The decoder drains delayed frames and keeps the other
stream's queued packets when one stream ends first. A looping source restarts
only after both enabled decoder streams finish; queued sources keep priority.
The data exports return false without replacing the caller's frame description
when no new frame is ready. Audio/video polling and source/stop operations are
serialized per player, without holding that lock across guest event callbacks.

The BioShock menu movie uses a 90 kHz video time base at 24 FPS. Previously,
its second frame's 3,750 timestamp ticks were reported as milliseconds, rather
than approximately 42 ms, and the looping option had no effect. The corrected
menu was confirmed working by the owner in the signed Vulkan emulator. This
is not a gameplay compatibility result or a claim about the game's overall FPS.

Run the focused decoder and HLE regressions after the emulator build:

```sh
python3 tools/waw/test-player.py
python3 tools/waw/test-avplayer-data.py
python3 tools/waw/test-capture-schedule.py
```

The first command generates a small H.264/AAC test movie with the local ffmpeg
CLI and compiles the actual decoder sources against the emulator's FFmpeg
libraries under AddressSanitizer and UndefinedBehaviorSanitizer. The HLE check
also exercises concurrent audio/video consumers. No game movies are included.
For a separately available 24 FPS source with a valid frame count, the resulting
`build/waw-checks/player/test-player source <movie>` checks timestamps, full
frame delivery and two loops. Generic streaming, seeking, stream disabling and
all media-format combinations are not covered by these targeted regressions.

## SDL keyboard controls

The SDL frontend publishes its focused keyboard state to the same controller
state used by the Qt frontend and guest controller APIs. Primary and alternate
bindings are combined, opposing stick directions cancel, and normal/extended
shoulder-button masks remain distinct. State is cleared during text entry,
on focus loss, and at session shutdown. Physical USB keyboard-page bindings
are supported; this does not add Qt-only hotkeys or consumer/media-key bindings.

The previous SDL event loop handled key-down only for text-entry dialogs and
never populated the virtual controller. Correct YAML mappings therefore had
no effect on gameplay controls. BioShock menu navigation and activation were
confirmed by the owner after the corrected signed build was relaunched.

```sh
python3 tools/waw/test-sdl-keyboard.py
```

The sanitizer-backed regression extracts the actual SDL event pump and
controller reader, with the real configuration/controller types and controlled
SDL keyboard state. It covers presses/releases, primary and alternate bindings,
both sticks, focus changes, text-entry suppression and input-array bounds.

BioShock gameplay is not yet validated: selecting New Game now works and the
intro finishes, but a black screen is reported afterward. Loading/UI activity
continues; its cause remains under investigation independently of menu controls.
