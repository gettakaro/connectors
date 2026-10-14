-- Small helpers shared by the TakaroSCUM Lua modules. No game or transport symbols.
local json = require("takaro.json")

local M = {}

function M.trim(s)
  return (tostring(s):gsub("^%s+", ""):gsub("%s+$", ""))
end

function M.is_array(t)
  if type(t) ~= "table" or getmetatable(t) ~= nil then return false end
  local n = 0
  for k in pairs(t) do
    if type(k) ~= "number" then return false end
    n = n + 1
  end
  return n == #t
end

-- A value that is absent, JSON null or the wrong type all count as "not given".
function M.opt_string(v)
  if type(v) == "string" then return v end
  if type(v) == "number" then return tostring(math.tointeger(v) or v) end
  return nil
end

function M.opt_number(v)
  if type(v) == "number" then return v end
  if type(v) == "string" then return tonumber(M.trim(v)) end
  return nil
end

function M.opt_table(v)
  if type(v) == "table" and v ~= json.null then return v end
  return nil
end

function M.shallow_copy(t)
  local o = {}
  for k, v in pairs(t) do o[k] = v end
  return o
end

function M.truncate(s, n)
  s = tostring(s)
  if #s <= n then return s end
  return s:sub(1, n) .. "...(" .. #s .. " bytes)"
end

return M
