local t = require("lib.t")
local json = require("takaro.json")

t.test("json.object encodes empty as {} and json.null as null", function()
  t.eq(json.encode(json.object()), "{}")
  t.eq(json.encode({}), "[]")
  t.eq(json.encode({ a = json.null }), '{"a":null}')
end)

t.test("objects encode with sorted keys (deterministic frames)", function()
  t.eq(json.encode({ b = 1, a = 2 }), '{"a":2,"b":1}')
end)

t.test("decode: null members vanish (treated like absent keys), unicode survives", function()
  local v = json.decode('{"a":null,"b":"\\u00e9"}')
  t.eq(v.a, nil)
  t.eq(v.b, "\195\169")
end)

t.run("json")
