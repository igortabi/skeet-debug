-- In-game test for listboxes that fit their items: ui.new_listbox(..., true) (skeet-debug). Load it from the
-- LUA tab like any script.
--
-- The automatic checks print to the console (ok / FAIL lines and a summary). Then look at the menu:
--   LUA > A    "Fitted": 4 items, as tall as its search box and the 4 rows, no scrollbar, and the checkbox right
--              below it. "Add item" / "Remove item" change its items with ui.update: it grows and shrinks and the
--              checkbox moves with it. Down to no items it keeps one empty row.
--              "Fitted (strings)": the strings form with a trailing true, 3 items.
--   LUA > B    "Plain": the same 4 items without `true`, the game's 300 px with empty rows, for comparison.
--              "Fitted (empty)": one empty row. "Fitted (30 items)": all 30 rows; the container scrolls instead.
-- Change the menu scale (MISC > Settings > DPI scale): the fitted listboxes keep showing every item, no scrollbar.
-- Typing in a fitted listbox's search box filters it; its height stays. Clicking an item logs the selection.
-- Reload and unload the script a few times: the listboxes go away and come back with it, no crash.

local passed, failed = 0, 0

local function check(ok, what)
    if ok then
        passed = passed + 1
        client.color_log(150, 200, 60, "[listbox test] ok    " .. what)
    else
        failed = failed + 1
        client.color_log(255, 80, 80, "[listbox test] FAIL  " .. what)
    end
end

check(type(skeetsdk) == "table" and type(skeetsdk._fit_listbox) == "function", "skeetsdk._fit_listbox exists")

local function log_selection(item)
    client.log("[listbox test] ", ui.name(item), ": item ", tostring(ui.get(item)), " selected")
end

-- --- LUA > A: a fitted listbox whose items change ---------------------------------------------------

local items = { "Item 1", "Item 2", "Item 3", "Item 4" }
local fitted = ui.new_listbox("LUA", "A", "Fitted", items, true)
check(type(fitted) == "number", "ui.new_listbox(tab, container, name, items, true) returns a reference")
ui.set_callback(fitted, log_selection)
ui.new_checkbox("LUA", "A", "Right below the fitted listbox")

local added = #items
ui.new_button("LUA", "A", "Add item", function()
    added = added + 1
    items[#items + 1] = "Item " .. added
    ui.update(fitted, items)
end)
ui.new_button("LUA", "A", "Remove item", function()
    items[#items] = nil
    ui.update(fitted, items)
end)

local strings = ui.new_listbox("LUA", "A", "Fitted (strings)", "First", "Second", "Third", true)
check(type(strings) == "number", "the strings form with a trailing true returns a reference")
ui.set_callback(strings, log_selection)

-- --- LUA > B: for comparison, empty, and long ---------------------------------------------------------

local plain = ui.new_listbox("LUA", "B", "Plain", { "Item 1", "Item 2", "Item 3", "Item 4" })
ui.set_callback(plain, log_selection)
local plain_false = ui.new_listbox("LUA", "B", "Plain (false)", { "Not fitted" }, false)
check(type(plain_false) == "number", "a trailing false makes a plain listbox")

local empty = ui.new_listbox("LUA", "B", "Fitted (empty)", true)
check(type(empty) == "number", "ui.new_listbox(tab, container, name, true) makes an empty fitted listbox")

local many = {}
for i = 1, 30 do many[i] = "Row " .. i end
local long = ui.new_listbox("LUA", "B", "Fitted (30 items)", many, true)
ui.set_callback(long, log_selection)
ui.new_checkbox("LUA", "B", "Below 30 rows")

-- --- the game's own behaviour is unchanged -----------------------------------------------------------

ui.set(fitted, 2)
check(ui.get(fitted) == 2, "ui.set / ui.get select items as before")
check(pcall(ui.update, fitted, { "Item 1", "Item 2", "Item 3", "Item 4", "Item 5" }), "ui.update on a fitted listbox")
check(pcall(ui.update, fitted, items), "ui.update back to the 4 items")
check(pcall(ui.update, plain, { "Item 1", "Item 2" }), "ui.update on a plain listbox")
check(not pcall(ui.new_listbox, "NOPE", "A", "x", { "a" }, true), "an unknown tab still raises")
local ok, message = skeetsdk._fit_listbox(ui.new_checkbox("LUA", "B", "Not a listbox"))
check(ok == nil and type(message) == "string", "_fit_listbox refuses a checkbox")

client.color_log(failed == 0 and 150 or 255, failed == 0 and 200 or 80, failed == 0 and 60 or 80,
    string.format("[listbox test] %d passed, %d failed. Now check the menu (see the top of the script).", passed, failed))
