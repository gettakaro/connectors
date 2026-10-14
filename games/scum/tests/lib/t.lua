-- Minimal test runner: t.test(name, fn); t.eq / t.ok / t.deep; t.run() prints one line per test.
local json = require("takaro.json")
local T = { tests = {}, failures = 0 }

function T.test(name, fn) T.tests[#T.tests + 1] = { name = name, fn = fn } end

local function deep_eq(a, b)
  if type(a) ~= type(b) then return false end
  if type(a) ~= "table" then return a == b end
  for k, v in pairs(a) do if not deep_eq(v, b[k]) then return false end end
  for k in pairs(b) do if a[k] == nil then return false end end
  return true
end
T.deep_eq = deep_eq

local function show(v)
  if type(v) == "table" then
    local ok, s = pcall(json.encode, v)
    return ok and s or tostring(v)
  end
  return tostring(v)
end

function T.eq(a, b, msg) if a ~= b then error((msg or "eq") .. ": expected " .. show(b) .. ", got " .. show(a), 2) end end
function T.ok(v, msg) if not v then error((msg or "expected truthy") .. " (got " .. show(v) .. ")", 2) end end
function T.deep(a, b, msg)
  if not deep_eq(a, b) then error((msg or "deep") .. ": expected " .. show(b) .. ", got " .. show(a), 2) end
end

function T.run(suite)
  local passed = 0
  for _, tc in ipairs(T.tests) do
    local ok, err = xpcall(tc.fn, debug.traceback)
    if ok then
      passed = passed + 1
      print("  ok   " .. suite .. " :: " .. tc.name)
    else
      T.failures = T.failures + 1
      print("  FAIL " .. suite .. " :: " .. tc.name .. "\n" .. tostring(err))
    end
  end
  print(string.format("%s: %d passed, %d failed", suite, passed, T.failures))
  os.exit(T.failures == 0 and 0 or 1)
end

return T
