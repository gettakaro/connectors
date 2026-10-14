-- Logger with secret redaction. Every line passes through redact(), so a token value can never reach
-- takaro.log even when a frame containing it is logged (LOG_FRAMES=true logs the identify frame too).
local M = {}
M.__index = M

-- sink(line): where finished lines go (native takaro_log in the game, a table in tests)
function M.new(sink)
  return setmetatable({ sink = sink or print, secrets = {}, counts = { warn = 0, error = 0 } }, M)
end

function M:add_secret(value)
  if type(value) == "string" and #value >= 4 then self.secrets[#self.secrets + 1] = value end
end

function M:redact(s)
  s = tostring(s)
  for _, secret in ipairs(self.secrets) do
    local i = 1
    while true do
      local a, b = s:find(secret, i, true)
      if not a then break end
      s = s:sub(1, a - 1) .. "***" .. s:sub(b + 1)
      i = a + 3
    end
  end
  return s
end

function M:line(level, fmt, ...)
  local ok, msg = pcall(string.format, fmt, ...)
  if not ok then msg = tostring(fmt) end
  if self.counts[level] then self.counts[level] = self.counts[level] + 1 end
  self.sink(self:redact("lua: [" .. level .. "] " .. msg))
end

function M:info(fmt, ...) self:line("info", fmt, ...) end
function M:warn(fmt, ...) self:line("warn", fmt, ...) end
function M:error(fmt, ...) self:line("error", fmt, ...) end
function M:debug(fmt, ...) if self.debug_enabled then self:line("debug", fmt, ...) end end

return M
