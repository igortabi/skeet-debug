-- In-game test for containers with more than 1024 controls (skeet-debug). Load it from the LUA tab like any script.
--
-- Before the fix a container handed only its first 1024 controls to its click handler (hidden ones count) and to
-- its drawing (visible ones count); the rest drew but did not react, or were not drawn at all.
--
--   LUA > A    1200 hidden checkboxes, then the controls to try at the bottom. All of them must react:
--              "Past 1024: checkbox" logs when toggled, "Past 1024: button" logs when clicked, the slider drags and
--              logs, the combobox opens over the controls below it and logs the pick, the hotkey takes a key.
--              Without the fix none of them react.
--   LUA > B    1100 visible labels and a checkbox after them. Scroll the container to the bottom: the last labels
--              ("Label 1100") and "After 1100 labels" must be drawn and the checkbox must toggle.
-- The ok / FAIL lines and a summary are printed to the console. Reload and unload the script a few times: no
-- crash, no slowdown in the menu.

local passed, failed = 0, 0

local function check(ok, what)
    if ok then
        passed = passed + 1
        client.color_log(150, 200, 60, "[children test] ok    " .. what)
    else
        failed = failed + 1
        client.color_log(255, 80, 80, "[children test] FAIL  " .. what)
    end
end

local function log_value(reference)
    client.log("[children test] ", ui.name(reference), " = ", tostring(ui.get(reference)))
end

-- --- LUA > A: past 1024 controls for the click handler, most of them hidden ------------------------

local hidden_count = 1200
local made = 0
for i = 1, hidden_count do
    local reference = ui.new_checkbox("LUA", "A", "Hidden filler " .. i)
    if type(reference) == "number" then
        ui.set_visible(reference, false)
        made = made + 1
    end
end
check(made == hidden_count, hidden_count .. " hidden checkboxes made in LUA > A")

ui.new_label("LUA", "A", "Everything below is past control 1024:")

local checkbox = ui.new_checkbox("LUA", "A", "Past 1024: checkbox")
ui.set_callback(checkbox, log_value)

ui.new_button("LUA", "A", "Past 1024: button", function()
    client.log("[children test] button clicked")
end)

local slider = ui.new_slider("LUA", "A", "Past 1024: slider", 0, 100, 50)
ui.set_callback(slider, log_value)

local combobox = ui.new_combobox("LUA", "A", "Past 1024: combobox", "First", "Second", "Third")
ui.set_callback(combobox, log_value)
ui.new_checkbox("LUA", "A", "Below the combobox (its list opens over this)")

local hotkey = ui.new_hotkey("LUA", "A", "Past 1024: hotkey")
check(type(checkbox) == "number" and type(slider) == "number" and type(combobox) == "number"
    and type(hotkey) == "number", "controls past 1024 made")

-- --- LUA > B: past 1024 visible controls for drawing ------------------------------------------------

local label_count = 1100
local labels = 0
for i = 1, label_count do
    if type(ui.new_label("LUA", "B", "Label " .. i)) == "number" then labels = labels + 1 end
end
check(labels == label_count, label_count .. " visible labels made in LUA > B")

local after = ui.new_checkbox("LUA", "B", "After 1100 labels")
ui.set_callback(after, log_value)

local summary = string.format("[children test] %d passed, %d failed. Now try the controls at the bottom of LUA > A and B.",
    passed, failed)
if failed == 0 then
    client.color_log(150, 200, 60, summary)
else
    client.color_log(255, 80, 80, summary)
end
