import sys
import time

sys.path.insert(0, r"G:\git\caf-plugin-system\tools")
from cluster_tui import BridgeLink

events = []
link = BridgeLink(events.append)
if not link.connect("127.0.0.1", 48063):
    print("CONNECT FAIL")
    sys.exit(1)
print("CONNECTED")


def call(svc, payload, cid):
    link.call(svc, payload)
    deadline = time.time() + 15
    while time.time() < deadline:
        for e in events:
            if e["type"] == "result" and e["cid"] == cid:
                return e
        time.sleep(0.2)
    return None


r1 = call("system.ping", "ping", 1)
print(f"system.ping    -> {r1 and (r1['ok'], r1['payload'])}")
assert r1 and r1["ok"] and r1["payload"] == b"pong", "ping FAILED"

r2 = call("system.services", "services", 2)
print(f"system.services-> {r2 and (r2['ok'], r2['payload'])}")
assert r2 and r2["ok"], "services FAILED"
s = r2["payload"].decode("utf-8", "replace")
assert "logging_service" in s and "system.nodes" in s, f"services missing entries: {s}"

r3 = call("system.nodes", "nodes", 3)
print(f"system.nodes   -> {r3 and (r3['ok'], r3['payload'])}")
assert r3 and r3["ok"], "nodes FAILED"
t = r3["payload"].decode("utf-8", "replace")
assert "master" in t and "bridge-a" in t, f"topology missing nodes: {t}"

link.disconnect()
print("ALL ADMIN PASS")
