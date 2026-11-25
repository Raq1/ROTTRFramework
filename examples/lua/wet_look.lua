-- Wet look: a menu tab that turns on the gradual wet look (rottr.wet) and sets its timings.
-- Copy this file into ROTTRFramework\ in the game folder, then Scripts > Reload Lua scripts (or restart the game).
-- See SCRIPTING.md for the API.

local enabled = framework.settings.get_bool("enabled", true)
local wet_in = framework.settings.get_number("wet_in_seconds", 1.0)
local dry_out = framework.settings.get_number("dry_out_seconds", 30.0)

local function apply()
    if not rottr.wet.available() then return end
    rottr.wet.set_wet_in_seconds(wet_in)
    rottr.wet.set_dry_out_seconds(dry_out)
    rottr.wet.set_enabled(enabled)
end
apply()

framework.register_menu_tab("Wet look", function()
    if not rottr.wet.available() then
        ui.text_wrapped("The wet look can't be changed on this game build (see ROTTRFramework.log).")
        return
    end

    local changed
    changed, enabled = ui.checkbox("Gradual wet look", enabled)
    if changed then
        framework.settings.set_bool("enabled", enabled)
        apply()
    end

    ui.set_next_item_width(150)
    changed, wet_in = ui.input_float("Getting wet (s)", wet_in, 0.5, 5.0, "%.2f", not enabled)
    if changed then
        wet_in = math.max(0.05, math.min(30.0, wet_in))
        framework.settings.set_number("wet_in_seconds", wet_in)
        apply()
    end

    ui.set_next_item_width(150)
    changed, dry_out = ui.input_float("Drying (s)", dry_out, 5.0, 30.0, "%.1f", not enabled)
    if changed then
        dry_out = math.max(0.25, math.min(300.0, dry_out))
        framework.settings.set_number("dry_out_seconds", dry_out)
        apply()
    end

    ui.spacing()
    ui.text_disabled(rottr.wet.status())
end)

-- Leave the game as it was when the script goes away.
framework.on_unload(function()
    if rottr.wet.available() then rottr.wet.set_enabled(false) end
end)
