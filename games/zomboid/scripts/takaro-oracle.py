#!/usr/bin/env python3
"""
takaro-oracle.py — drive Takaro REST API to inspect/prove connector actions.

Reads credentials from environment (NEVER hardcode secrets):
  TAKARO_HOST       e.g. https://api.takaro.io
  TAKARO_USERNAME   dashboard user email
  TAKARO_PASSWORD   dashboard user password
Optional:
  TAKARO_GAMESERVER_ID     gameserver UUID to act on
  TAKARO_IDENTITY_TOKEN    identityToken to resolve a gameserver by (e.g. takaro-dev-zomboid)

Auth model (from openapi.json securitySchemes):
  - domainAuth: the login token (data.token, an Ory session token) is presented as
    `Authorization: Bearer <token>` (also accepted as cookie `takaro-token`).
  - Domain scoping is SERVER-SIDE "selected domain": POST /selected-domain/{domainId}.
    A user with a single domain has it auto-selected. `GET /me` returns .data.domains
    (the domains the user belongs to) and .data.domain (the currently selected id).
  - Cross-domain admin (domain/search enumerating all domains) needs adminAuth
    (x-takaro-admin-token) which a dashboard user does not hold.

Usage:
  python3 takaro-oracle.py me
  python3 takaro-oracle.py find                 # locate gameserver by id/identityToken/name
  python3 takaro-oracle.py list                 # list all gameservers in selected domain
  python3 takaro-oracle.py drive <gameServerId> # run the full action round-trip suite
"""
import json, os, sys, urllib.request, urllib.error

HOST = os.environ.get("TAKARO_HOST", "https://api.takaro.io").rstrip("/")
USER = os.environ.get("TAKARO_USERNAME")
PW   = os.environ.get("TAKARO_PASSWORD")
GSID = os.environ.get("TAKARO_GAMESERVER_ID")
IDTOK = os.environ.get("TAKARO_IDENTITY_TOKEN", "takaro-dev-zomboid")

def _req(method, path, token=None, body=None):
    url = HOST + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", "takaro-oracle/1.0")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read().decode() or "{}")
        except Exception:
            return e.code, {}

TOKEN_CACHE = os.environ.get("TAKARO_TOKEN_CACHE", "")

def login():
    # Reuse a cached Ory session token when valid — the /login endpoint is
    # rate-limited under rapid repeated calls, so we log in ONCE and cache it.
    if TOKEN_CACHE and os.path.exists(TOKEN_CACHE):
        try:
            tok = open(TOKEN_CACHE).read().strip()
            st, _ = _req("GET", "/me", token=tok)
            if st == 200:
                return tok
        except Exception:
            pass
    if not (USER and PW):
        sys.exit("Set TAKARO_USERNAME and TAKARO_PASSWORD in the environment.")
    st, body = _req("POST", "/login", body={"username": USER, "password": PW})
    if st != 200:
        sys.exit(f"login failed http={st}: {body}")
    tok = body["data"]["token"]
    if TOKEN_CACHE:
        try:
            open(TOKEN_CACHE, "w").write(tok)
        except Exception:
            pass
    return tok

def resolve_player(token, gsid, game_id):
    """Resolve the Takaro global playerId + pog record for a gameId on a server."""
    st, b = _req("POST", "/gameserver/player/search", token=token,
                 body={"filters": {"gameServerId": [gsid], "gameId": [game_id]}})
    data = b.get("data", [])
    return data[0] if data else None

def me(token):
    st, body = _req("GET", "/me", token=token)
    d = body.get("data", {})
    return d

def search_gs(token, filters=None, limit=100):
    st, body = _req("POST", "/gameserver/search", token=token,
                    body={"filters": filters or {}, "limit": limit})
    return st, body

def cmd_me(token):
    d = me(token)
    print("selected domain:", d.get("domain"))
    print("domains the user belongs to:")
    for dom in d.get("domains", []):
        print(f"  - {dom['id']}  name={dom.get('name')!r}  state={dom.get('state')}")
    print("user:", d.get("user", {}).get("name"))

def cmd_find(token):
    for label, filt in [("by id", {"id": [GSID]} if GSID else None),
                        ("by identityToken", {"identityToken": [IDTOK]}),
                        ("by name", {"name": [IDTOK]})]:
        if filt is None:
            continue
        st, body = search_gs(token, filt)
        print(f"{label}: http={st} total={body.get('meta',{}).get('total')} -> {[g['id'] for g in body.get('data',[])]}")
    if GSID:
        st, body = _req("GET", f"/gameserver/{GSID}", token=token)
        print(f"GET /gameserver/{GSID}: http={st} {json.dumps(body)[:200]}")

def cmd_list(token):
    st, body = search_gs(token)
    print("total:", body.get("meta", {}).get("total"))
    for g in body.get("data", []):
        print(f"  {g['id']} | {g['name']!r} | idToken={g.get('identityToken')} | reachable={g.get('reachable')}")

def cmd_drive(token, gsid):
    """Run the full action suite against a gameserver (must be in the selected domain)."""
    results = {}
    st, b = _req("GET", f"/gameserver/{gsid}/reachability", token=token); results["testReachability"] = (st, b.get("data", b))
    st, b = _req("GET", f"/gameserver/{gsid}/players", token=token); results["getPlayers"] = (st, b.get("data", b))
    st, b = _req("POST", f"/gameserver/{gsid}/command", token=token, body={"command": "players"}); results["cmd:players"] = (st, b.get("data", b))
    st, b = _req("POST", f"/gameserver/{gsid}/command", token=token, body={"command": 'servermsg "takaro roundtrip test"'}); results["cmd:servermsg"] = (st, b.get("data", b))
    st, b = _req("POST", f"/gameserver/{gsid}/message", token=token, body={"message": "takaro roundtrip test"}); results["sendMessage"] = (st, b.get("data", b))
    st, b = _req("GET", f"/gameserver/{gsid}/bans", token=token); results["listBans"] = (st, b.get("data", b))
    st, b = _req("POST", "/items/search", token=token, body={"filters": {"gameserverId": [gsid]}, "limit": 1}); results["listItems(total)"] = (st, b.get("meta", {}).get("total"))
    st, b = _req("POST", "/entities/search", token=token, body={"filters": {"gameserverId": [gsid]}, "limit": 1}); results["listEntities(total)"] = (st, b.get("meta", {}).get("total"))
    for k, v in results.items():
        print(f"{k}: {json.dumps(v)[:300]}")

def _pr(label, st, body):
    print(f"{label}: http={st} {json.dumps(body)[:600]}")
    return st, body

def cmd_player(token, gsid, game_id):
    """getPlayer/getPlayers/getPlayerLocation/getPlayerInventory oracle view."""
    pog = resolve_player(token, gsid, game_id)
    if not pog:
        print(f"no pog for {game_id}"); return None
    st, p = _req("GET", f"/player/{pog['playerId']}", token=token)
    pd = p.get("data", {})
    print(json.dumps({
        "playerId": pog["playerId"], "gameId": pog["gameId"], "online": pog["online"],
        "name": pd.get("name"), "steamId": pd.get("steamId"), "platformId": pd.get("platformId"),
        "ping": pog.get("ping"), "ip": pog.get("ip"),
        "location": {"x": pog.get("positionX"), "y": pog.get("positionY"), "z": pog.get("positionZ")},
        "inventory": pog.get("inventory"),
    }, indent=1))
    return pog

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "me"
    a = sys.argv
    token = login()
    if cmd == "me": cmd_me(token)
    elif cmd == "find": cmd_find(token)
    elif cmd == "list": cmd_list(token)
    elif cmd == "drive":
        gsid = a[2] if len(a) > 2 else GSID
        if not gsid: sys.exit("provide a gameServerId (arg or TAKARO_GAMESERVER_ID)")
        cmd_drive(token, gsid)
    elif cmd == "player":   # player <gsid> <gameId>
        cmd_player(token, a[2], a[3])
    elif cmd == "giveitem": # giveitem <gsid> <gameId> <name> [amount] [quality]
        pog = resolve_player(token, a[2], a[3])
        _pr("giveItem", *_req("POST", f"/gameserver/{a[2]}/player/{pog['playerId']}/giveItem", token=token,
            body={"name": a[4], "amount": int(a[5]) if len(a) > 5 else 1, "quality": a[6] if len(a) > 6 else "1"}))
    elif cmd == "teleport": # teleport <gsid> <gameId> <x> <y> <z>
        pog = resolve_player(token, a[2], a[3])
        _pr("teleport", *_req("POST", f"/gameserver/{a[2]}/player/{pog['playerId']}/teleport", token=token,
            body={"x": float(a[4]), "y": float(a[5]), "z": float(a[6])}))
    elif cmd == "kick":     # kick <gsid> <gameId> [reason]
        pog = resolve_player(token, a[2], a[3])
        _pr("kick", *_req("POST", f"/gameserver/{a[2]}/player/{pog['playerId']}/kick", token=token,
            body={"reason": a[4] if len(a) > 4 else "takaro-proof"}))
    elif cmd == "ban":      # ban <gsid> <gameId> [reason] [expiresAtISO]
        pog = resolve_player(token, a[2], a[3])
        body = {"reason": a[4] if len(a) > 4 else "takaro-proof"}
        if len(a) > 5: body["expiresAt"] = a[5]
        _pr("ban", *_req("POST", f"/gameserver/{a[2]}/player/{pog['playerId']}/ban", token=token, body=body))
    elif cmd == "unban":    # unban <gsid> <gameId>
        pog = resolve_player(token, a[2], a[3])
        _pr("unban", *_req("POST", f"/gameserver/{a[2]}/player/{pog['playerId']}/unban", token=token, body={}))
    elif cmd == "bans":     # bans <gsid>
        _pr("listBans", *_req("GET", f"/gameserver/{a[2]}/bans", token=token))
    elif cmd == "msg":      # msg <gsid> <message> [recipientGameId]
        body = {"message": a[3]}
        if len(a) > 4: body["opts"] = {"recipient": {"gameId": a[4]}}
        _pr("message", *_req("POST", f"/gameserver/{a[2]}/message", token=token, body=body))
    elif cmd == "cmd":      # cmd <gsid> <command...>
        _pr("command", *_req("POST", f"/gameserver/{a[2]}/command", token=token, body={"command": " ".join(a[3:])}))
    elif cmd == "shutdown": # shutdown <gsid>
        _pr("shutdown", *_req("POST", f"/gameserver/{a[2]}/shutdown", token=token, body={}))
    elif cmd == "reach":    # reach <gsid>
        _pr("reachability", *_req("GET", f"/gameserver/{a[2]}/reachability", token=token))
    elif cmd == "events":   # events <gsid> [eventName] [limit]
        filt = {"gameserverId": [a[2]]}
        if len(a) > 3 and a[3] != "-": filt["eventName"] = [a[3]]
        _pr("events", *_req("POST", "/event/search", token=token,
            body={"filters": filt, "limit": int(a[4]) if len(a) > 4 else 10, "sortBy": "createdAt", "sortDirection": "desc"}))
    else:
        sys.exit(f"unknown command {cmd!r}")

if __name__ == "__main__":
    main()
