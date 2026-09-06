# omarchy-fx

Wobbly windows for [Omarchy](https://omarchy.org/). Grab a window with
`SUPER` + a mouse button and it deforms like a sheet of jelly while it follows
the cursor, then springs back into shape when you let go — the KDE
"Wobbly Windows" effect, done as a Hyprland compositor plugin.

- `SUPER` + **right** mouse drag — resize, wobbling from the grabbed corner
- `SUPER` + **left** mouse drag — move, wobbling from wherever you grabbed it

## Why the effect is a Hyprland plugin, not an Omarchy shell plugin

Omarchy shell plugins (`~/.config/omarchy/plugins/`) are Quickshell/QML and draw
into the bar and overlays. They can't touch how the compositor rasterises a
window, and a wobble *is* a change to how the window is rasterised: the window
texture has to be resampled through a deformed mesh every frame. That only
exists inside Hyprland's render pass, so the effect ships as a Hyprland plugin
(`.so`), loaded from the Omarchy Hyprland config.

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
`~/.config/omarchy/plugins/omarchy-fx.wobbly/` and enables it on the right of
the bar. Re-running the installer leaves the placement alone, so
`omarchy bar move omarchy-fx.wobbly` sticks. Click the widget for the settings
panel, middle-click it to toggle the effect.

Then load it into the running session:

```bash
hyprctl plugin load ~/.local/share/hyprland/plugins/omarchy-fx.so
```

Hyprland asks for permission the first time a plugin is loaded. The installed
config also declares `hl.permission({ ..., type = "plugin", mode = "allow" })`
so later launches don't prompt.

Remove it with `./uninstall.sh`.

## Staying in step with Hyprland

`install.sh` installs a pacman hook, which needs `sudo` for two root-owned
paths:

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
effect itself:

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
`~/.config/hypr/omarchy_fx.lua`:

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

| `wobbly_wobbliness` | stiffness | drag | move factor |
| --- | --- | --- | --- |
| 0 | 0.15 | 0.80 | 0.10 |
| 1 | 0.10 | 0.85 | 0.10 |
| 2 | 0.06 | 0.90 | 0.10 |
| 3 | 0.03 | 0.92 | 0.20 |
| 4 | 0.01 | 0.97 | 0.25 |

The file guards the `hl.config` call with `hl.get_config`, because plugin
options don't exist during the config pass that loads the plugin. Hyprland
reloads the config immediately afterwards, and that pass applies them.

## How it works

The physics is a port of KWin's `wobblywindows` effect, and follows it closely
enough that the parameters mean the same thing and the presets produce the same
feel.

A **4×4 lattice of control points** is laid over the window frame. Points are
held together only by springs to their neighbours — nothing pulls a point
towards where the window says it should be. The single exception is the
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
`position += velocity·dt·moveFactor`, in fixed 10 ms steps, so the effect looks
identical at 60 and 240 Hz.

During a resize, an edge stays rigid until it has actually been dragged away
from where the resize started — so `SUPER`+right-drag stretches from the corner
you grabbed rather than sloshing the whole window.

The renderer treats that 4×4 lattice as the **control net of a bicubic Bézier
surface**, tessellated `wobbly_tessellation` quads per axis. Warping a coarse
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
over; `CWobblyTransformer` draws that texture back through the deformed mesh
into a second buffer and returns it. Positions and texture coordinates are
normalised in the same framebuffer space, so an undeformed mesh is a pixel-exact
copy — and when the net is flat the transformer hands the buffer straight back
rather than paying for the copy at all. The plugin detaches itself entirely once
a window has settled.

Damage is expanded to the control net's bounds each frame, which is both what
lets the wobble draw outside the window's rectangle and what keeps frames
scheduled while the simulation is running.

## Layout

```
src/WobblyModel.*        the 4x4 spring lattice, no rendering, no Hyprland state
src/WobblyTransformer.*  IWindowTransformer + the Bezier mesh GL draw
src/WobblyManager.*      drag tracking, per-frame stepping, damage, config
src/main.cpp             plugin entry points
hypr/omarchy_fx.lua      the config fragment install.sh drops into ~/.config/hypr
```

## Credits

The physics is a port of KWin's
[`wobblywindows`](https://github.com/KDE/kwin/tree/master/src/plugins/wobblywindows)
effect by Cédric Borgese, GPL-2.0-or-later.

## Known limitations

- Only interactive (mouse) drags wobble. Keyboard moves, window open/close and
  workspace switches use Hyprland's own animations.
- Blur behind the window is warped along with it (Hyprland runs the blur matte
  through the same transformer), but the blur sample itself is taken from the
  undeformed rectangle, so heavy deformation over a busy background can show a
  seam at the edge.
- Written against the Hyprland 0.56 plugin API.

## License

GPL-2.0-or-later. The physics in `src/WobblyModel.*` is a port of KWin's
wobblywindows effect, which is GPL-2.0-or-later, so this project inherits that
licence — it cannot be MIT.
