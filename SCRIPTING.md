# Lua scripting

ROTTRFramework runs [Lua 5.4](https://www.lua.org/manual/5.4/) scripts. A script can add its own tab to the F8 menu, keep its own settings, run code every frame, and use a few of the framework's game controls: Lara's wet look, hiding materials, and moving her collision markers.

Scripting is meant for small tools and for outfit authors: for example a menu tab that comes with an outfit and lets players hide parts of it. It isn't a general game-modding API; what a script can reach in the game is the list under [Game controls](#game-controls-rottr) below.

- [Where scripts go](#where-scripts-go)
- [A first script](#a-first-script)
- [How a script runs](#how-a-script-runs)
- [API reference](#api-reference)
- [Errors and the log](#errors-and-the-log)
- [Examples](#examples)

## Where scripts go

The framework loads every `.lua` file in two places when the game starts:

| Where | Script name | Its settings file |
|---|---|---|
| `ROTTRFramework\` in the game folder (not its subfolders) | the file name, e.g. `my_tool` | `ROTTRFramework\settings\my_tool.ini` |
| Next to a custom outfit's `outfit.ini`, in its folder or `.zip` package (see the README's custom outfits) | `<outfit>/<file name>` | `ROTTRFramework\settings\<outfit>.<file name>.ini` |

Scripts in `ROTTRFramework\` load first, in alphabetical order, then the outfits' scripts. **Scripts → Reload Lua scripts** in the menu bar loads them all again after you edit them, without restarting the game. The same menu shows how many scripts loaded.

## A first script

Save this as `ROTTRFramework\hello.lua`, then use **Scripts → Reload Lua scripts** (or restart the game) and open the menu with F8: a new **Hello** tab appears.

```lua
-- A tab with a checkbox and a number, both remembered between game sessions.
local enabled = framework.settings.get_bool("enabled", true)
local amount = framework.settings.get_number("amount", 1.5)

framework.register_menu_tab("Hello", function()
    ui.text("Hello from Lua!")
    ui.separator()

    local changed
    changed, enabled = ui.checkbox("Enabled", enabled)
    if changed then framework.settings.set_bool("enabled", enabled) end

    ui.set_next_item_width(150)
    changed, amount = ui.input_float("Amount", amount, 0.1, 1.0, "%.2f", not enabled)
    if changed then framework.settings.set_number("amount", amount) end

    if ui.button("Say hi in the log") then
        print("Hi! Amount is " .. amount)
    end
end)
```

## How a script runs

- **Each script is on its own.** Every script gets its own Lua state, so scripts can't see each other's variables.
- **Loading.** When a script loads, its file runs once from top to bottom. That first run is the only time it can call `framework.register_menu_tab`, `framework.on_update` and `framework.on_unload`; calling them later is an error.
- **Every frame.** Functions given to `framework.on_update` run once for every frame the game shows, including in menus and loading screens. They get the time since the last frame in seconds (at most 0.25).
- **Menu tabs.** A tab's function runs every frame while the F8 menu is open and that tab is selected. The `ui.*` functions only work inside it.
- **Unloading.** On a reload or when the game closes, each script's `framework.on_unload` functions run, then the script is closed. Anything the script changed in the game (hidden materials, marker positions, the wet look) stays as it is unless the script undoes it there.
- **What Lua has.** The standard `string`, `table`, `math`, `utf8` and `coroutine` libraries and the basic functions. There is no file, OS or module access: `io`, `os`, `debug`, `package`, `require`, `dofile` and `loadfile` are removed. Scripts must be Lua source text (not precompiled).

## API reference

Three global tables: `framework` (the script itself), `ui` (its menu tab) and `rottr` (the game). Arguments in square brackets are optional.

### framework

| Function | What it does |
|---|---|
| `framework.version` | The framework's version as text, e.g. `"1.0.0"` (the first 1.0.0 download still reports `"0.9.180"` here) |
| `framework.api_version` | The scripting API's version, a number (currently `3`). Check it if your script needs a newer API. |
| `framework.log(text)` | Writes `Lua [<script name>]: <text>` to `ROTTRFramework.log`. `print(text)` does the same (one text argument). |
| `framework.register_menu_tab(name, fn)` | Adds a tab called `name` to the F8 menu; `fn()` draws it. Only while the script loads. A script can add several tabs. |
| `framework.on_update(fn)` | Calls `fn(dt)` every frame, `dt` in seconds. Only while the script loads. |
| `framework.on_unload(fn)` | Calls `fn()` when the script is unloaded (a reload, or the game closing). Only while the script loads. |
| `framework.reload()` | Reloads every script at the next frame (as Scripts → Reload Lua scripts). |

### framework.settings

Each script has its own settings file (see [Where scripts go](#where-scripts-go)); values are written at once.

| Function | What it does |
|---|---|
| `framework.settings.get_bool(key [, default])` | The saved true/false value, or `default` (false if not given) |
| `framework.settings.set_bool(key, value)` | Saves a true/false value |
| `framework.settings.get_number(key [, default])` | The saved number, or `default` (0 if not given) |
| `framework.settings.set_number(key, value)` | Saves a number (it must be finite) |

Only true/false values and numbers can be saved.

### ui

Only inside a menu tab's function. These are [Dear ImGui](https://github.com/ocornut/imgui) controls, so two controls with the same label in one tab clash: add `##` and something unique after the label to keep the visible text the same (`"Size##hair"`, `"Size##coat"`). The `disabled` arguments grey a control out.

| Function | Returns |
|---|---|
| `ui.text(text)`, `ui.text_disabled(text)`, `ui.text_wrapped(text)` | A line of text: plain, greyed out, or wrapped to the window's width |
| `ui.checkbox(label, value [, disabled])` | `changed, value` |
| `ui.input_float(label, value [, step, step_fast, format, disabled])` | `changed, value`. With `step`, the field gets − and + buttons (`step_fast` with Ctrl held). `format` is printf-style, default `"%.3f"`. |
| `ui.input_text(label [, text, capacity, disabled])` | `changed, text`. `capacity` is the most bytes the field holds (2 to 255, default 64). |
| `ui.button(label [, disabled])` | `true` on the frame it is clicked |
| `ui.same_line([offset, spacing])` | Puts the next control on the same line |
| `ui.set_next_item_width(width)` | The width in pixels of the next field |
| `ui.separator()`, `ui.spacing()` | A line across the tab, or a little space |

The pattern for a control that changes a value:

```lua
local changed
changed, value = ui.checkbox("Label", value)
if changed then
    -- the player changed it this frame
end
```

### Game controls (rottr)

Each group has an `available()` function: it returns false when this game build's code doesn't match what the framework expects. Check it first; when it is false, the group's other functions have no effect in the game.

#### rottr.wet: Lara's wet look

A gradual transition of Lara's wet look (her `wet_hair` material constant) as she gets wet and as she dries, over times you choose. It is off until a script turns it on, and nothing else in the framework turns it on.

| Function | What it does |
|---|---|
| `rottr.wet.available()` | Whether it works on this game build |
| `rottr.wet.enabled()`, `rottr.wet.set_enabled(on)` | Whether the gradual transition is on |
| `rottr.wet.wet_in_seconds()`, `rottr.wet.set_wet_in_seconds(s)` | How long getting wet takes: 0.05 to 30 s, default 1 |
| `rottr.wet.dry_out_seconds()`, `rottr.wet.set_dry_out_seconds(s)` | How long drying takes: 0.25 to 300 s, default 30 |
| `rottr.wet.status()` | A line of text saying what it is doing, for showing in a tab |

#### rottr.material: hiding materials

Leaves out every draw that uses a material, so whatever uses it disappears; the copies a custom outfit makes of a material are hidden with it. A material is given by its ID: hover over a material in the **Materials** window (menu bar) and its tooltip shows the ID. That window marks hidden materials `[hidden]` and can hide them too.

| Function | What it does |
|---|---|
| `rottr.material.available()` | Whether it works on this game build |
| `rottr.material.set_hidden(id, hidden)` | Hides or shows the material; returns true if it worked |
| `rottr.material.toggle_hidden(id)` | Hides it if shown and shows it if hidden; returns `ok, hidden` |
| `rottr.material.is_hidden(id)` | Whether it is hidden |
| `rottr.material.suppressed_draw_count()` | A running count of the draws hidden materials have left out (to check that hiding works) |

#### rottr.marker: Lara's collision markers

Moves one of Lara's collision markers (the ones the Instances window's **Markers** tab lists for her) to a position of your own. Overrides are kept by marker name and applied again when Lara's model changes, for example after an outfit switch. Up to 32 at once.

| Function | What it does |
|---|---|
| `rottr.marker.available()` | Whether it works on this game build |
| `rottr.marker.set_position_override(name, x, y, z)` | Moves the marker named `name` (not case-sensitive) to `x, y, z`: its position relative to its bone, the first three values of the bottom row of the matrix the Markers tab shows. Returns true if it worked. |
| `rottr.marker.clear_position_override(name)` | Puts the marker back where it was; returns true if it worked |

## Errors and the log

Everything a script prints and every error goes to `ROTTRFramework.log` in the game folder, as `Lua [<script name>] ...`.

- **A script that fails to load** (a syntax error, or an error during its first run) is left out until the next reload; the log says where the error is.
- **A menu tab whose function fails** shows the error in the tab itself until the next reload.
- **An `on_update` function that fails** is stopped until the next reload.

Without `[Log] Verbose=1` in `ROTTRFramework.ini`, the log keeps the first 25 lines of script output in a session and leaves out the rest. While you're writing a script, add this to `ROTTRFramework.ini`:

```ini
[Log]
Verbose=1
```

## Examples

### A tool tab (ROTTRFramework\wet_look.lua)

[`examples\lua\wet_look.lua`](examples/lua/wet_look.lua) adds a **Wet look** tab that turns the gradual wet look on, sets its timings, and saves them.

### An outfit's own tab

A script shipped next to an outfit's `outfit.ini` can give that outfit a tab of options. This one hides or shows a material of the outfit; replace `123456` with the material's ID from the Materials window.

```lua
local coat_id = 123456   -- the coat's material ID (Materials window: hover it)
local hide_coat = framework.settings.get_bool("hide_coat", false)

local function apply()
    if rottr.material.available() then
        rottr.material.set_hidden(coat_id, hide_coat)
    end
end
apply()

framework.register_menu_tab("My outfit", function()
    local changed
    changed, hide_coat = ui.checkbox("Hide the coat", hide_coat)
    if changed then
        framework.settings.set_bool("hide_coat", hide_coat)
        apply()
    end
end)

framework.on_unload(function()
    if rottr.material.available() then
        rottr.material.set_hidden(coat_id, false)   -- show it again when the script goes away
    end
end)
```

### Timing something with on_update

```lua
local elapsed = 0

framework.on_update(function(dt)
    elapsed = elapsed + dt
    if elapsed >= 60 then
        elapsed = 0
        print("Another minute in the game")
    end
end)
```
