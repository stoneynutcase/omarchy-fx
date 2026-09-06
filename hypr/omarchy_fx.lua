-- omarchy-fx — wobbly windows for Omarchy / Hyprland.
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
            },
        },
    })
end
