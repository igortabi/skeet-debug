-- In-game test for skeetsdk.persist. Load this script, then load or unload any other script (or reload this one)
-- a few times: the load count goes up and the saved table comes back each time. It is gone after a game restart.

local data = skeetsdk.persist("persist_test")

data.loads = (data.loads or 0) + 1

local saved = data.saved or { first_load = globals.realtime(), history = {} }
saved.history[#saved.history + 1] = data.loads
data.saved = saved -- a read gives a copy, so assign it back to keep the change

print(string.format(
    "persist_test: loaded %d time(s), first load at %.1f s, history: %s, keys: %s",
    data.loads, saved.first_load, table.concat(saved.history, " "), table.concat(skeetsdk.persist.keys("persist_test"), ", ")
))

local ok, message = pcall(function() data.bad = print end)
print("persist_test: storing a function fails as it should: " .. tostring(not ok) .. " (" .. tostring(message) .. ")")
