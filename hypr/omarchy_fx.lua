-- omarchy-fx — window effects for Omarchy / Hyprland.
--
-- Installed to ~/.config/hypr/omarchy_fx.lua by ./install.sh and pulled in from
-- ~/.config/hypr/hyprland.lua with:
--
--     require("hypr.omarchy_fx")

local plugin = os.getenv("HOME") .. "/.local/share/hyprland/plugins/omarchy-fx.so"

-- Skip the permission prompt on every launch.
hl.permission({ binary = plugin, type = "plugin", mode = "allow" })

hl.plugin.load(plugin)

-- Tuning. The plugin registers these options when it loads, which happens after
-- this file is first evaluated, so the guard keeps the very first config pass
-- from erroring on keys that do not exist yet. Hyprland reloads the config right
-- after loading plugins, and the second pass applies them.
local _, missing = hl.get_config("plugin.omarchy_fx.wobbly_wobbliness")
if not missing then
    hl.config({
        plugin = {
            omarchy_fx = {
                -- Wobbly windows: the window deforms under the pointer while
                -- you drag it, and springs back when you let go.
                wobbly_enabled     = true,
                wobbly_on_move     = true, -- SUPER + left mouse drag
                wobbly_on_resize   = true, -- SUPER + right mouse drag

                -- KWin's "Wobbliness" slider: 0 = least, 4 = most.
                wobbly_wobbliness  = 1,

                -- Render mesh quads per axis. Higher is smoother, 20 is KWin's.
                wobbly_tessellation = 20,

                -- Per-parameter overrides of the preset. -1 keeps the preset's
                -- value; these are KWin's stiffness / drag / move factor.
                wobbly_stiffness   = -1,
                wobbly_drag        = -1,
                wobbly_move_factor = -1,

                -- Elastic moves: the window stretches like rubber when the
                -- layout animates it somewhere — a tiled swap, a reflow after
                -- a window opens or closes, a keyboard resize.
                elastic_enabled      = true,
                elastic_on_tiled     = true,
                elastic_on_floating  = false,

                -- Carrying a window to another workspace. A carried window is
                -- slid across by the workspace's render offset, not by its own
                -- geometry, so this only ever shows if Hyprland's workspace
                -- animation is on — and Omarchy ships it off. To turn it on,
                -- put this in ~/.config/hypr/looknfeel.lua:
                --
                --   hl.animation({ leaf = "workspaces", enabled = true,
                --                  speed = 3, bezier = "easeOutQuint",
                --                  style = "slide" })
                elastic_on_workspace = true,

                -- Stretchiness: 0 = taut, 4 = taffy.
                elastic_stretchiness = 2,

                elastic_tessellation = 20,

                -- Per-parameter overrides of the preset, -1 keeps the preset's.
                --   period      spring period in ms; how fast it snaps back
                --   damping     damping ratio; 1 is critical, lower rings more
                --   tilt        how much stiffer the leading edge is, 0 to 0.95
                --   follow      how much the window trails as a whole, 0 to 1
                --   max_stretch furthest a point may be dragged, in px
                elastic_period       = -1,
                elastic_damping      = -1,
                elastic_tilt         = -1,
                elastic_follow       = -1,
                elastic_max_stretch  = -1,

                -- Focus pulse: the window that just became active swells out
                -- a few pixels and settles back, so your eye finds it.
                pulse_enabled   = true,
                pulse_on_switch = true,  -- keyboard, a dispatcher, or focus moving on its own
                pulse_on_click  = true,  -- a click that gives a window focus
                pulse_on_hover  = false, -- focus following the mouse; noisy with follow_mouse

                -- Strength: 0 = subtle, 4 = bouncy.
                pulse_strength = 2,

                pulse_tessellation = 20,

                -- Extras. Shake the pointer and the cursor grows until you
                -- stop, so you can find it — KDE Plasma's "Shake Cursor".
                shake_enabled = true,

                -- Per-parameter overrides of the preset, -1 keeps the preset's.
                --   amount   how far the edges swell at the peak, in px
                --   period   swell period in ms; how fast it settles
                --   damping  damping ratio; 1 is critical, lower rings more
                pulse_amount  = -1,
                pulse_period  = -1,
                pulse_damping = -1,
            },
        },
    })
end
