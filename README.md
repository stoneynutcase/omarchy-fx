# omarchy-fx

Window effects for [Omarchy](https://omarchy.org/), as a Hyprland compositor
plugin. Two of them so far, both built on the same deformable mesh:

- **Wobbly windows** — grab a window with `SUPER` + a mouse button and it
  deforms like a sheet of jelly while it follows the cursor, then springs back
  into shape when you let go. The KDE effect, ported.
  - `SUPER` + **left** mouse drag — move, wobbling from wherever you grabbed it
  - `SUPER` + **right** mouse drag — resize, wobbling from the grabbed corner
- **Elastic moves** — when the *layout* moves a window rather than the pointer,
  the window stretches like rubber as it travels and springs back when it
  arrives. Swapping two tiled windows is the obvious case; so is the reflow
  when a window opens or closes, and a keyboard resize.

## Why the effects are a Hyprland plugin, not an Omarchy shell plugin

Omarchy shell plugins (`~/.config/omarchy/plugins/`) are Quickshell/QML and draw
into the bar and overlays. They can't touch how the compositor rasterises a
window, and these effects *are* a change to how the window is rasterised: the
window texture has to be resampled through a deformed mesh every frame. That
only exists inside Hyprland's render pass, so the effects ship as a Hyprland
plugin (`.so`), loaded from the Omarchy Hyprland config.

The *settings* are a different matter, and those do ship as a shell plugin — a
bar widget with a panel behind it, in `shell/`. The two halves meet at one
`key=value` file, `~/.config/omarchy/omarchy-fx.conf`: the panel writes it and
runs a stock `hyprctl reload`, and the compositor plugin re-reads it whenever
the config reloads. Keys absent from that file fall through to the Hyprland
config, so configuring in Lua and configuring from the bar both keep working.

> The panel deliberately registers **no** custom hyprctl command. An earlier
> version did, and registering a dispatcher inside `pluginInit` crashed
> Hyprland — which, at startup, means the session never comes up and the fix
> has to be made from a TTY. Everything the panel does uses stock `hyprctl`.

## Requirements

- Hyprland with development headers (`pkg-config --exists hyprland`)
- A C++23 compiler and `make`

Hyprland's plugin ABI is not stable: **the plugin must be rebuilt after every
Hyprland update.** This is not a cosmetic requirement. A stale `.so` faults
inside `pluginInit`, and because the Hyprland config loads plugins during
compositor startup, that fault takes the session down *before* you can log in —
leaving a TTY as the only way back. `install.sh` sets up a pacman hook so the
rebuild happens for you; see [Staying in step with Hyprland](#staying-in-step-with-hyprland).

## Install

```bash
./install.sh
```

That builds `omarchy-fx.so`, installs it to
`~/.local/share/hyprland/plugins/`, drops `~/.config/hypr/omarchy_fx.lua`, and
appends `require("hypr.omarchy_fx")` to `~/.config/hypr/hyprland.lua` (keeping a
timestamped backup of anything it touches).

On an Omarchy shell it also installs the bar widget to
`~/.config/omarchy/plugins/omarchy-fx/` and enables it on the right of the bar.
Re-running the installer leaves the placement alone, so
`omarchy bar move omarchy-fx` sticks. Click the widget for the settings panel,
middle-click it to turn every effect off and on.

> Upgrading from the version that had only the wobble: the widget used to be
> `omarchy-fx.wobbly`, one widget per effect. The installer disables and removes
> that one and enables `omarchy-fx` in its place, so you get one widget with a
> section per effect — but it lands on the right of the bar, wherever the old
> one sat. Settings carry over; the panel reads the old key spellings.

Then load it into the running session:

```bash
hyprctl plugin load ~/.local/share/hyprland/plugins/omarchy-fx.so
```

Hyprland asks for permission the first time a plugin is loaded. The installed
config also declares `hl.permission({ ..., type = "plugin", mode = "allow" })`
so later launches don't prompt.

## Turning it off, and removing it

On an Omarchy shell, **the bar widget is the switch**. Middle-click it to turn
every effect off and on; the panel behind it turns each effect off separately.
Taking the widget off the bar also stops the effects:

```bash
omarchy plugin disable omarchy-fx    # or: omarchy plugin remove omarchy-fx
```

Omarchy has no way for a shell plugin to tell the compositor anything when it
is disabled, so the compositor plugin watches the shell's bar layout instead:
with the shell installed and `omarchy-fx` not on the bar, nothing deforms. That
takes effect within half a second, needs no reload, and holds across reboots.
Without an Omarchy shell the Hyprland config alone decides.

Either way the Hyprland plugin is still installed and still loads at startup —
inert, but there. The only thing that removes the `.so`, the config snippet,
the settings file and the pacman rebuild hook is:

```bash
./uninstall.sh
```

It unloads the plugin from the running compositor first, so no reboot is
needed for that either.

## Staying in step with Hyprland

`install.sh` installs a pacman hook, which needs root for two root-owned paths.
On a terminal that is a `sudo` prompt; run without one (from a launcher, a GUI,
an agent) it falls back to `pkexec`, so the password dialog comes from Omarchy's
polkit agent instead. `uninstall.sh` does the same to remove them.

| | |
| --- | --- |
| `/etc/pacman.d/hooks/95-omarchy-fx-rebuild.hook` | the trigger |
| `/usr/local/bin/omarchy-fx-rebuild` | the runner, generated with your paths baked in |

After any package that moves the plugin ABI, the hook rebuilds this checkout and
reinstalls the `.so`. It triggers on `hyprland` **and** on `aquamarine`,
`hyprcursor`, `hyprgraphics`, `hyprlang` and `hyprutils`: the version compiled
into a plugin is the Hyprland commit hash plus the versions of all five
libraries, so any of them can invalidate a built `.so`.

Two things the runner is careful about, both of which matter more than the
effects themselves:

- **It never fails the pacman transaction.** A broken build warns; it does not
  leave your system mid-upgrade.
- **If the rebuild fails, it disarms the plugin** by renaming the `.so` to
  `.so.stale`. A plugin that cannot load is a minor annoyance; a plugin that
  faults during compositor startup costs you the login.

Skip the hook with `./install.sh --no-hook` — then rebuilding after each
Hyprland update is back to being your job. `uninstall.sh` removes both files.

Relatedly, the installer never writes the `.so` in place. It installs to a
temporary name and `rename(2)`s it over the old one, because a running Hyprland
has the previous build mapped, and truncating that file underneath the
compositor is itself a way to crash it.

## Configuration

All options live under `plugin.omarchy_fx` and are set from
`~/.config/hypr/omarchy_fx.lua`.

### Wobbly windows

| Option | Default | Meaning |
| --- | --- | --- |
| `wobbly_enabled` | `true` | Master switch |
| `wobbly_on_move` | `true` | Wobble while drag-moving |
| `wobbly_on_resize` | `true` | Wobble while drag-resizing |
| `wobbly_wobbliness` | `1` | KWin's Wobbliness slider: `0` = least, `4` = most |
| `wobbly_tessellation` | `20` | Render mesh quads per axis (2–64) |
| `wobbly_stiffness` | `-1` | Override the preset's spring stiffness; `-1` inherits |
| `wobbly_drag` | `-1` | Override the preset's drag; `-1` inherits |
| `wobbly_move_factor` | `-1` | Override the preset's move factor; `-1` inherits |

The five presets are KWin's, verbatim:

| `wobbly_wobbliness` | | stiffness | drag | move factor |
| --- | --- | --- | --- | --- |
| 0 | Rigid | 0.15 | 0.80 | 0.10 |
| 1 | Subtle | 0.10 | 0.85 | 0.10 |
| 2 | Springy | 0.06 | 0.90 | 0.10 |
| 3 | Loose | 0.03 | 0.92 | 0.20 |
| 4 | Jelly | 0.01 | 0.97 | 0.25 |

### Elastic moves

| Option | Default | Meaning |
| --- | --- | --- |
| `elastic_enabled` | `true` | Master switch |
| `elastic_on_tiled` | `true` | React when the layout animates a tiled window |
| `elastic_on_floating` | `false` | React when something animates a floating window |
| `elastic_on_workspace` | `true` | Stretch a window you carry to another workspace and follow |
| `elastic_stretchiness` | `2` | Preset: `0` = taut, `4` = taffy |
| `elastic_tessellation` | `20` | Render mesh quads per axis (2–64) |
| `elastic_period` | `-1` | Override the spring period in ms; `-1` inherits |
| `elastic_damping` | `-1` | Override the damping ratio; `-1` inherits |
| `elastic_tilt` | `-1` | Override the leading/trailing stiffness split; `-1` inherits |
| `elastic_follow` | `-1` | Override how much the window trails as a whole; `-1` inherits |
| `elastic_max_stretch` | `-1` | Override the furthest a point may be dragged, in px; `-1` inherits |

| `elastic_stretchiness` | | period | damping | tilt | follow | max stretch |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | Taut | 110 ms | 0.80 | 0.35 | 0.30 | 30 px |
| 1 | Springy | 145 ms | 0.65 | 0.50 | 0.42 | 45 px |
| 2 | Elastic | 180 ms | 0.55 | 0.65 | 0.55 | 65 px |
| 3 | Rubber | 235 ms | 0.45 | 0.78 | 0.68 | 90 px |
| 4 | Taffy | 310 ms | 0.35 | 0.88 | 0.80 | 130 px |

The file guards the `hl.config` call with `hl.get_config`, because plugin
options don't exist during the config pass that loads the plugin. Hyprland
reloads the config immediately afterwards, and that pass applies them.

## How it works

Both effects deform the same thing: a **4×4 lattice of control points** laid
over the window frame, which the renderer reads as the control net of a bicubic
Bézier surface. Only the physics differs, so a new effect is a new model, not a
new renderer.

### The wobble

A port of KWin's `wobblywindows`, close enough that the parameters mean the same
thing and the presets produce the same feel.

Points are held together only by springs to their neighbours — nothing pulls a
point towards where the window says it should be. The single exception is the
*constrained* point, the one nearest where the cursor grabbed, which is pulled
towards its own rest position and drags the whole net along behind it. That one
anchor is also what lets the lattice find its way back onto the window rect
after you let go, so it is deliberately never released.

After computing accelerations, the acceleration field and then the velocity
field are each run through a 3×3 mean filter (KWin's `heightRingLinearMean`,
where the centre carries as much weight as all its neighbours together). This
is the part that matters visually: without it the motion stays bunched up around
the grabbed point, and with it the wobble spreads across the whole window.

Integration is `velocity = acceleration·dt + velocity·drag` and
`position += velocity·dt·moveFactor`, in fixed 10 ms steps.

During a resize, an edge stays rigid until it has actually been dragged away
from where the resize started — so `SUPER`+right-drag stretches from the corner
you grabbed rather than sloshing the whole window.

### The elasticity

A different mechanism, and original rather than a port. Every lattice point is a
damped spring anchored to *its own* rest position, with no coupling to its
neighbours at all. Nothing is grabbed; the deformation is pure inertia. When
Hyprland animates the window rect out from under the lattice the points cannot
follow instantly, so they trail; when the rect stops they overshoot and ring
themselves flat. The spring is stated as a period and a damping ratio, because
those are the two things you actually want to dial: how fast it snaps back, and
how much it rings.

Carrying a window to another workspace is the one case this cannot see by
watching geometry: a workspace does not move its windows to slide, it animates
one render offset that every window on it is drawn through, so the window's own
rect never moves. That case is picked up from the `moveToWorkspace` event
instead, and the lattice then works in the offset space the window is really
drawn in.

A lattice of identical springs would only ever lag as a rigid block — every
point lags equally, and the window merely trails without changing shape. The
stretch comes from **tilting the stiffness along the direction of travel**: the
leading edge is anchored hard and keeps up, the trailing edge is anchored softly
and drags behind, so the window is pulled longer while it moves. The tilt fades
out with the window's speed, so a lattice at rest is anchored evenly and settles
back onto the rect. A resize needs no tilt to deform — there the rest positions
themselves move by different amounts, and the lattice stretches on its own.

Two things keep it honest:

- **It saturates.** Hyprland covers most of a swap in the first frame of the
  animation, far faster than a spring soft enough to be worth watching can
  follow, so the deformation would otherwise be whatever the animation curve
  happened to do. Instead the spring stiffens as a point approaches
  `elastic_max_stretch` and stops dead at it — the stretch is a design
  parameter, not an accident of the animation speed. Bounding the stiffness is
  also what keeps the fixed-step integration stable; an unbounded progressive
  spring blows up within a handful of frames.
- **`elastic_follow` splits trailing from stretching.** The physics produces
  both a bulk lag (the whole window behind its rect) and a differential (the
  window longer than its rect). `follow` scales how much of the bulk lag is
  drawn, so you can have a window that stretches without swimming away from its
  own tile. It changes what is drawn, never how the springs behave.

The trigger is deliberately dumb: any window whose position or size animation is
running gets an entry, whatever started it. That is why a swap, a reflow after a
window closes and a keyboard resize all deform without three separate hooks.
Windows currently being dragged are left to the wobble — one effect per window,
and the drag wins.

### Rendering

The renderer treats the 4×4 lattice as the control net of a **bicubic Bézier
surface**, tessellated `*_tessellation` quads per axis. Warping a coarse
interpolating grid directly would put a visible crease at every cell boundary;
the surface between control points is a cubic instead, so the deformation is
smooth. A uniform control net reproduces the rectangle exactly, which keeps an
undeformed window pixel-exact.

The lattice is defined over the window *frame*, while the mesh covers the full
bounding box. Shadows and borders therefore fall outside `[0, 1]` and the Bézier
extrapolates there, so they ride along with the window instead of diluting the
deformation.

Rendering uses Hyprland's window-transformer hook. When a transformer is
attached, Hyprland renders the window into an offscreen buffer and hands it
over; `CMeshTransformer` draws that texture back through the deformed mesh into
a second buffer and returns it. Positions and texture coordinates are normalised
in the same framebuffer space, so an undeformed mesh is a pixel-exact copy — and
when the net is flat the transformer hands the buffer straight back rather than
paying for the copy at all. The plugin detaches itself entirely once a window has
settled.

Damage is expanded to the control net's bounds each frame, plus a margin that
scales with how far the lattice has been pushed — the mesh extrapolates past the
net for shadows and borders, so a fixed margin leaves trails under a heavy
deformation. That damage is both what lets a deformation draw outside the
window's rectangle and what keeps frames scheduled while a simulation runs.

## Layout

```
src/Mesh.hpp             the 4x4 control lattice every effect deforms
src/WobblyModel.*        KWin's wobble physics, no rendering, no Hyprland state
src/ElasticModel.*       the inertial rubber-band physics, same
src/MeshTransformer.*    IWindowTransformer + the Bezier mesh GL draw
src/EffectManager.*      triggers, per-frame stepping, damage, config
src/main.cpp             plugin entry points
hypr/omarchy_fx.lua      the config fragment install.sh drops into ~/.config/hypr
shell/                   the bar widget and settings panel
```

## Credits

The wobble physics is a port of KWin's
[`wobblywindows`](https://github.com/KDE/kwin/tree/master/src/plugins/wobblywindows)
effect by Cédric Borgese, GPL-2.0-or-later.

## Known limitations

- A plugin cannot log. `Log::logger` is an inline variable in Hyprland's
  headers, so a plugin links its own uninitialised copy of it and everything
  logged through it is silently dropped. Anything the user must be told goes
  through `HyprlandAPI::addNotification` instead.
- Blur behind a window is warped along with it (Hyprland runs the blur matte
  through the same transformer), but the blur sample itself is taken from the
  undeformed rectangle, so heavy deformation over a busy background can show a
  seam at the edge.
- A deformed window is drawn off its own rect while its input region stays put,
  so during an effect the two disagree by up to the current offset.
- Window open and close animations are left alone; they animate the geometry
  from nothing, and stretching that fights the animation rather than dressing it.
- `elastic_on_workspace` rides Hyprland's workspace slide, and Omarchy ships
  that animation disabled (`hl.animation({ leaf = "workspaces", enabled = false })`
  in its default `looknfeel.lua`). Without it a workspace switch is instant,
  there is no travel to dress, and the setting does nothing. Re-enable it in
  your own `~/.config/hypr/looknfeel.lua`, which loads after Omarchy's defaults.
- Written against the Hyprland 0.56 plugin API.

## License

GPL-2.0-or-later. The physics in `src/WobblyModel.*` is a port of KWin's
wobblywindows effect, which is GPL-2.0-or-later, so this project inherits that
licence — it cannot be MIT.
