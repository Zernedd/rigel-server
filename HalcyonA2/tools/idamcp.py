"""Tiny client for ida-pro-mcp's streamable-HTTP endpoint (usable before a Claude restart)."""
import json, urllib.request, sys
class Ida:
    def __init__(self, port=13337):
        self.url=f"http://127.0.0.1:{port}/mcp"; self.sid=None; self.n=0
        self.call("initialize",{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"halcyon","version":"0"}})
    def call(self, method, params=None, timeout=300):
        self.n+=1
        body=json.dumps({"jsonrpc":"2.0","id":self.n,"method":method,"params":params or {}}).encode()
        h={"Content-Type":"application/json","Accept":"application/json, text/event-stream"}
        if self.sid: h["Mcp-Session-Id"]=self.sid
        req=urllib.request.Request(self.url,data=body,method="POST",headers=h)
        with urllib.request.urlopen(req,timeout=timeout) as r:
            self.sid=r.headers.get("Mcp-Session-Id",self.sid); raw=r.read().decode()
        if raw.startswith(("event:","data:")): raw="\n".join(l[5:].strip() for l in raw.splitlines() if l.startswith("data:"))
        return json.loads(raw)
    def tool(self, name, args=None, timeout=300):
        r=self.call("tools/call",{"name":name,"arguments":args or {}},timeout=timeout)
        if "error" in r: return {"error": r["error"]}
        c=r.get("result",{}).get("content",[])
        txt="\n".join(x.get("text","") for x in c if x.get("type")=="text")
        try: return json.loads(txt)
        except Exception: return txt
