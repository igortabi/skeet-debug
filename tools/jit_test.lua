-- In-game test for the restored jit table. Load it and watch the console.

local keys = {}
for key in pairs(jit) do keys[#keys + 1] = key end
table.sort(keys)
print("jit has: " .. table.concat(keys, ", "))
print(string.format("jit.version %s | version_num %s | os %s | arch %s",
    tostring(jit.version), tostring(jit.version_num), tostring(jit.os), tostring(jit.arch)))
print("require('jit.opt') == jit.opt: " .. tostring(require("jit.opt") == jit.opt))
print("jit.status(): " .. table.concat({ tostring(jit.status()) , select(2, jit.status()) }, " "))

-- Turn two optimizations off and change two parameters, then show the flags; then back to the defaults.
jit.opt.start("-fold", "nocse", "hotloop=100", "maxtrace=2000")
print("after -fold nocse: " .. table.concat({ select(2, jit.status()) }, " "))
jit.opt.start()
print("after jit.opt.start(): " .. table.concat({ select(2, jit.status()) }, " "))
jit.opt.start("hotloop=56", "maxtrace=1000")

local ok, message = pcall(jit.opt.start, "bogus")
print("bad option raises: " .. tostring(not ok) .. " (" .. tostring(message) .. ")")

-- jit.attach is the cheat's own: its LuaJIT is built without VM events, so it always raises an error.
ok, message = pcall(jit.attach, function() end, "trace")
print("jit.attach: " .. (ok and "works" or ("raises (" .. tostring(message) .. "), as expected")))
