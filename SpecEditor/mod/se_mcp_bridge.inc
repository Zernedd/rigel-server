// ── MCP bridge (included at the end of se_game.cpp, inside namespace se::<anon>) ───────────────────────────
// Lets an AI agent (Claude, Codex, ... through SpecEditor/mcp/rigel_mcp.py) drive the editor exactly like a
// person: every request becomes the same Command / test op / UI action the editor's own UI uses, on the game
// thread. Loopback only (127.0.0.1), port 47650 (or the next free one up to 47654; the port in use is written to
// %TEMP%\rigel_mcp_port.txt). Protocol, one line each way:
//   request:  <id>\t<cmd>\t<hex(arg1)>\t<hex(arg2)>...      (args are hex-encoded UTF-8)
//   reply:    {"id":<id>,"ok":true|false,"result":<json>|"error":"..."}
// Replies to requests that need the world to catch up (place, props, data, ...) come a few frames later.

struct McpReq { unsigned long long s = 0; std::string id, cmd; std::vector<std::string> a; };
std::string g_mcpGameLog;                         // this game process's own UE log (Luau output, script errors)

std::string McpHexDec(const std::string& h)
{
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o += static_cast<char>(strtoul(h.substr(i, 2).c_str(), nullptr, 16));
    return o;
}
std::string J(const std::string& s)               // JSON string literal
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}
std::string JV(const Vec3& v) { char b[128]; snprintf(b, sizeof(b), "[%.1f,%.1f,%.1f]", v.x, v.y, v.z); return b; }
std::string JR(const Rot& r) { char b[128]; snprintf(b, sizeof(b), "[%.2f,%.2f,%.2f]", r.pitch, r.yaw, r.roll); return b; }
std::string JN(double d) { char b[64]; snprintf(b, sizeof(b), "%.6g", d); return b; }

void McpOk(const McpReq& r, const std::string& json) { McpNetSend(r.s, "{\"id\":" + r.id + ",\"ok\":true,\"result\":" + json + "}"); }
void McpErr(const McpReq& r, const std::string& msg) { McpNetSend(r.s, "{\"id\":" + r.id + ",\"ok\":false,\"error\":" + J(msg) + "}"); }
McpReq McpParse(const McpRawReq& raw)
{
    McpReq r; r.s = raw.conn;
    size_t b = 0; int k = 0;
    const std::string& line = raw.line;
    while (b <= line.size())
    {
        size_t e = line.find('\t', b);
        if (e == std::string::npos) e = line.size();
        const std::string f = line.substr(b, e - b);
        if (k == 0) r.id = f; else if (k == 1) r.cmd = f; else r.a.push_back(McpHexDec(f));
        ++k; b = e + 1;
    }
    bool digits = !r.id.empty();
    for (char c : r.id) if (!isdigit(static_cast<unsigned char>(c))) digits = false;
    if (!digits) r.id = "0";
    return r;
}

// The UE log this process writes (Saved\Logs\A2*.log whose "Base Directory" is our exe's folder).
std::string McpFindGameLog()
{
    if (!g_mcpGameLog.empty()) return g_mcpGameLog;
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    dir = dir.substr(0, dir.find_last_of('\\') + 1);
    for (char& c : dir) if (c == '\\') c = '/';
    char la[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", la, MAX_PATH)) return "";
    const std::string logs = std::string(la) + "\\A2\\Saved\\Logs\\";
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((logs + "A2*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string best; FILETIME bestT{};
    do
    {
        const std::string p = logs + fd.cFileName;
        FILE* f = _fsopen(p.c_str(), "r", _SH_DENYNO);
        if (!f) continue;
        char head[16384] = {}; const size_t n = fread(head, 1, sizeof(head) - 1, f); fclose(f);
        std::string hs(head, n);
        for (char& c : hs) if (c == '\\') c = '/';
        if (hs.find(dir) != std::string::npos && CompareFileTime(&fd.ftLastWriteTime, &bestT) > 0) { best = p; bestT = fd.ftLastWriteTime; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    g_mcpGameLog = best;
    return best;
}

// Requests that finish on a later frame.
struct McpPending
{
    McpReq r; std::string kind; ULONGLONG deadline = 0;
    std::string cls, handle; Vec3 loc; std::vector<std::string> before; int serial = 0;
};
std::vector<McpPending> g_mcpPending;

std::string McpObjJson(const SceneObject& o)
{
    std::string s = "{\"handle\":" + J(o.handle) + ",\"class\":" + J(o.className) + ",\"label\":" + J(o.label) +
                    ",\"location\":" + JV(o.location) + ",\"rotation\":" + JR(o.rotation) + ",\"scale\":" + JV(o.scale);
    if (o.lockedByOther) s += ",\"lockedBy\":" + J(o.lockOwner);
    if (o.lockedByMe) s += ",\"lockedByMe\":true";
    return s + "}";
}
const SceneObject* McpFind(const Snapshot& snap, const std::string& handle)
{
    for (const auto& o : snap.objects) if (o.handle == handle) return &o;
    return nullptr;
}
double McpArg(const McpReq& r, size_t i, double def) { return i < r.a.size() && !r.a[i].empty() ? atof(r.a[i].c_str()) : def; }
std::string McpStr(const McpReq& r, size_t i) { return i < r.a.size() ? r.a[i] : std::string(); }

void McpHandle(const Snapshot& snap, const McpReq& r)
{
    const std::string& c = r.cmd;
    const ULONGLONG now = GetTickCount64();
    if (c == "ping") { McpOk(r, "\"pong\""); return; }
    if (c == "status")
    {
        std::string s = "{\"worldReady\":" + std::string(snap.worldReady ? "true" : "false") + ",\"inEditor\":" + (snap.inEditor ? "true" : "false") +
                        ",\"status\":" + J(snap.status) + ",\"camera\":{\"location\":" + JV(snap.cameraPos) + ",\"rotation\":" + JR(snap.cameraRot) + "}" +
                        ",\"openLevel\":" + J(CurrentSceneName()) + ",\"levelDirty\":" + (CurrentSceneDirty() ? "true" : "false") +
                        ",\"objects\":" + std::to_string(snap.objects.size()) + ",\"paletteItems\":" + std::to_string(snap.palette.size()) +
                        ",\"gameModes\":" + std::to_string(snap.gameModes.size()) + ",\"logSeq\":" + std::to_string(LogRingSeq()) +
                        ",\"gameLog\":" + J(McpFindGameLog());
        char tp[MAX_PATH]; GetTempPathA(MAX_PATH, tp);
        s += ",\"editorLog\":" + J(std::string(tp) + "spec_editor.log") + "}";
        McpOk(r, s);
        return;
    }
    if (c == "objects")                     // filter, cx, cy, cz, radius, limit
    {
        std::string f = McpStr(r, 0);
        for (char& ch : f) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        const bool nearQ = r.a.size() > 4 && !r.a[4].empty();
        const Vec3 cp{ McpArg(r, 1, 0), McpArg(r, 2, 0), McpArg(r, 3, 0) };
        const double rad = McpArg(r, 4, 0);
        const size_t lim = static_cast<size_t>(McpArg(r, 5, 200));
        std::vector<std::pair<double, const SceneObject*>> hits;
        for (const auto& o : snap.objects)
        {
            if (!f.empty())
            {
                std::string h = o.className + " " + o.label + " " + o.handle;
                for (char& ch : h) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                if (h.find(f) == std::string::npos) continue;
            }
            const double dx = o.location.x - (nearQ ? cp.x : snap.cameraPos.x), dy = o.location.y - (nearQ ? cp.y : snap.cameraPos.y),
                         dz = o.location.z - (nearQ ? cp.z : snap.cameraPos.z);
            const double d = sqrt(dx * dx + dy * dy + dz * dz);
            if (nearQ && d > rad) continue;
            hits.push_back({ d, &o });
        }
        std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        std::string s = "{\"total\":" + std::to_string(hits.size()) + ",\"objects\":[";
        for (size_t i = 0; i < hits.size() && i < lim; ++i) s += (i ? "," : "") + McpObjJson(*hits[i].second).insert(1, "\"distance\":" + JN(hits[i].first) + ",");
        McpOk(r, s + "]}");
        return;
    }
    if (c == "palette")                     // query, category, limit
    {
        std::string q = McpStr(r, 0), cat = McpStr(r, 1);
        for (char& ch : q) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        const size_t lim = static_cast<size_t>(McpArg(r, 2, 100));
        std::string s = "[";
        size_t n = 0;
        for (const auto& it : snap.palette)
        {
            std::string h = it.name + " " + it.path + " " + it.category;
            for (char& ch : h) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (!q.empty() && h.find(q) == std::string::npos) continue;
            if (!cat.empty() && it.category != cat) continue;
            if (n++ >= lim) break;
            s += (n > 1 ? "," : "") + std::string("{\"name\":") + J(it.name) + ",\"category\":" + J(it.category) + ",\"path\":" + J(it.path);
            if (!it.blocked.empty()) s += ",\"blocked\":" + J(it.blocked);
            if (!it.limited.empty()) s += ",\"note\":" + J(it.limited);
            s += "}";
        }
        McpOk(r, s + "]");
        return;
    }
    if (c == "place")                       // item, x, y, z, pitch, yaw, roll, sx, sy, sz
    {
        if (CurrentSceneName().empty()) { McpErr(r, "No level is open. Open or create one first (level_new / level_open): objects are placed into the open level."); return; }
        const std::string want = McpStr(r, 0);
        const PaletteItem* it = nullptr;
        for (const auto& p : snap.palette) if (p.name == want || p.path == want) { it = &p; break; }
        if (!it) for (const auto& p : snap.palette) if (p.name.find(want) != std::string::npos) { it = &p; break; }
        if (!it) { McpErr(r, "No palette item matches '" + want + "' (search with palette)."); return; }
        if (!it->blocked.empty()) { McpErr(r, it->name + " can't be placed: " + it->blocked); return; }
        Command cmd{ CmdType::SpawnItem };
        cmd.str = it->path;
        cmd.loc = { McpArg(r, 1, 0), McpArg(r, 2, 0), McpArg(r, 3, 0) };
        cmd.rot = { McpArg(r, 4, 0), McpArg(r, 5, 0), McpArg(r, 6, 0) };
        cmd.scale = { McpArg(r, 7, 1), McpArg(r, 8, 1), McpArg(r, 9, 1) };
        State().Push(cmd);
        McpPending p; p.r = r; p.kind = "place"; p.deadline = now + 12000; p.cls = it->name; p.loc = cmd.loc;
        for (const auto& o : snap.objects) p.before.push_back(o.handle);
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "transform")                   // handle, x, y, z, pitch, yaw, roll, sx, sy, sz (missing = unchanged)
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle (list them with objects)."); return; }
        if (o->lockedByOther) { McpErr(r, "Locked by " + o->lockOwner + ": only they can edit it."); return; }
        Command cmd{ CmdType::SetTransform };
        cmd.str = o->handle;
        cmd.loc = { McpArg(r, 1, o->location.x), McpArg(r, 2, o->location.y), McpArg(r, 3, o->location.z) };
        cmd.rot = { McpArg(r, 4, o->rotation.pitch), McpArg(r, 5, o->rotation.yaw), McpArg(r, 6, o->rotation.roll) };
        cmd.scale = { McpArg(r, 7, o->scale.x), McpArg(r, 8, o->scale.y), McpArg(r, 9, o->scale.z) };
        State().Push(cmd);
        McpOk(r, "{\"handle\":" + J(o->handle) + ",\"location\":" + JV(cmd.loc) + ",\"rotation\":" + JR(cmd.rot) + ",\"scale\":" + JV(cmd.scale) + "}");
        return;
    }
    if (c == "delete" || c == "select" || c == "focus_object")
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        if (c == "delete") { if (o->lockedByOther) { McpErr(r, "Locked by " + o->lockOwner + "."); return; } State().Push({ CmdType::DeleteObject, o->handle }); }
        else if (c == "select") RequestUiSelect(o->handle);
        else { Command cmd{ CmdType::FocusCamera }; cmd.loc = o->location; State().Push(cmd); }
        McpOk(r, "{\"handle\":" + J(o->handle) + "}");
        return;
    }
    if (c == "duplicate")                   // handle, x, y, z
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        Command cmd{ CmdType::Duplicate };
        cmd.str = o->handle;
        for (const auto& p : snap.palette) if (p.name == o->className) { cmd.str2 = p.path; break; }
        cmd.loc = { McpArg(r, 1, o->location.x + 200), McpArg(r, 2, o->location.y), McpArg(r, 3, o->location.z) };
        cmd.rot = o->rotation; cmd.scale = o->scale;
        State().Push(cmd);
        McpPending p; p.r = r; p.kind = "place"; p.deadline = now + 12000; p.cls = o->className; p.loc = cmd.loc;
        for (const auto& ob : snap.objects) p.before.push_back(ob.handle);
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "camera")                      // x, y, z, pitch, yaw  (pitch/yaw optional)
    {
        char b[160];
        snprintf(b, sizeof(b), "camto %.1f %.1f %.1f", McpArg(r, 0, snap.cameraPos.x), McpArg(r, 1, snap.cameraPos.y), McpArg(r, 2, snap.cameraPos.z));
        ScriptOp(snap, b);
        if (r.a.size() > 4) { snprintf(b, sizeof(b), "camlook %.2f %.2f", McpArg(r, 3, 0), McpArg(r, 4, 0)); ScriptOp(snap, b); }
        McpOk(r, "{\"ok\":true}");
        return;
    }
    if (c == "props")                       // handle [, sub-object path]
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        Command cmd{ CmdType::Inspect }; cmd.str = o->handle; cmd.str2 = McpStr(r, 1);
        State().Push(cmd);
        McpPending p; p.r = r; p.kind = "props"; p.deadline = now + 5000; p.handle = o->handle; p.cls = cmd.str2;
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "set_prop")                    // handle, path, value (wire encoding, see props)
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        Command cmd{ CmdType::SetProperty }; cmd.str = o->handle; cmd.str2 = McpStr(r, 1); cmd.str3 = McpStr(r, 2);
        State().Push(cmd);
        McpOk(r, "{\"ok\":true}");
        return;
    }
    if (c == "data")                        // handle: the object's synced Game data (serverData / Properties / state)
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        Command cmd{ CmdType::DataRequest }; cmd.str = o->handle;
        State().Push(cmd);
        McpPending p; p.r = r; p.kind = "data"; p.deadline = now + 6000; p.handle = o->handle; p.serial = snap.dataSerial;
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "data_set")                    // handle, path, kind (bool|num|str|text), value
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        Command cmd{ CmdType::DataSet }; cmd.str = o->handle; cmd.str2 = McpStr(r, 1); cmd.str3 = McpStr(r, 2); cmd.str4 = McpStr(r, 3);
        State().Push(cmd);
        McpOk(r, "{\"ok\":true}");
        return;
    }
    if (c == "set_text")                    // handle, text: a Text object's text ({name} templates allowed)
    {
        const SceneObject* o = McpFind(snap, McpStr(r, 0));
        if (!o) { McpErr(r, "No object with that handle."); return; }
        Command cmd{ CmdType::DataSet }; cmd.str = o->handle; cmd.str2 = "props/Text"; cmd.str3 = "text"; cmd.str4 = McpStr(r, 1);
        State().Push(cmd);
        McpOk(r, "{\"ok\":true}");
        return;
    }
    if (c == "luau_attach" || c == "luau_remove" || c == "luau_ref" || c == "luau_update")
    {
        Command cmd{ c == "luau_attach" ? CmdType::LuauAttach : c == "luau_remove" ? CmdType::LuauRemove : c == "luau_ref" ? CmdType::LuauRef : CmdType::LuauUpdate };
        if (c == "luau_update") { cmd.str2 = McpStr(r, 0); cmd.str3 = McpStr(r, 1); }
        else
        {
            const SceneObject* o = McpFind(snap, McpStr(r, 0));
            if (!o) { McpErr(r, "No object with that handle."); return; }
            cmd.str = o->handle; cmd.str2 = McpStr(r, 1);
            if (c == "luau_attach") cmd.str3 = McpStr(r, 2);
            if (c == "luau_ref") { cmd.str3 = McpStr(r, 2); cmd.str4 = McpStr(r, 3); cmd.str5 = McpStr(r, 4); }
        }
        State().Push(cmd);
        McpOk(r, "{\"ok\":true,\"logSeq\":" + std::to_string(LogRingSeq()) + "}");
        return;
    }
    if (c == "slot_candidates")             // Luau component type: objects that have one (for luau_ref targets)
    {
        Command cmd{ CmdType::SlotScan }; cmd.str = McpStr(r, 0);
        State().Push(cmd);
        McpPending p; p.r = r; p.kind = "slots"; p.deadline = now + 3000; p.cls = cmd.str;
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "game_scripts")                // [name]: the station's own Luau (names, or one script's source)
    {
        if (snap.gameScripts.empty()) State().Push({ CmdType::ScanScripts });
        McpPending p; p.r = r; p.kind = "scripts"; p.deadline = now + 8000; p.cls = McpStr(r, 0);
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "game_modes") { std::string s = "["; for (size_t i = 0; i < snap.gameModes.size(); ++i) s += (i ? "," : "") + J(snap.gameModes[i]); McpOk(r, s + "]"); return; }
    if (c == "levels_server")
    {
        std::string s = "[";
        for (size_t i = 0; i < snap.levels.size(); ++i)
        {
            const auto& l = snap.levels[i];
            s += (i ? "," : "") + std::string("{\"name\":") + J(l.name) + ",\"updated\":" + J(l.updated) + ",\"size\":" + std::to_string(l.size) +
                 ",\"loadedHere\":" + (l.here ? "true" : "false") + ",\"autoload\":" + (l.autoload ? "true" : "false") + "}";
        }
        McpOk(r, s + "]");
        return;
    }
    if (c == "raw") { const std::string l = McpStr(r, 0); if (l.rfind("SE|", 0) != 0) { McpErr(r, "raw lines start with SE|"); return; } State().Push({ CmdType::SendRaw, l }); McpOk(r, "{\"ok\":true}"); return; }
    if (c == "op" || c == "ui")             // a test-script op now / a UI action next frame; returns what it logged
    {
        const unsigned long long seq = LogRingSeq();
        if (c == "op") ScriptOp(snap, McpStr(r, 0));
        else RequestUiSelect("!" + McpStr(r, 0));
        McpPending p; p.r = r; p.kind = "log"; p.deadline = now + (c == "op" ? 400 : 900); p.serial = static_cast<int>(seq);
        g_mcpPending.push_back(p);
        return;
    }
    if (c == "logs")                        // since, filter, max
    {
        const unsigned long long since = static_cast<unsigned long long>(McpArg(r, 0, 0));
        const std::string f = McpStr(r, 1);
        const auto lines = LogRingSince(since, static_cast<size_t>(McpArg(r, 2, 300)));
        std::string s = "{\"seq\":" + std::to_string(LogRingSeq()) + ",\"lines\":[";
        bool first = true;
        for (const auto& e : lines) { if (!f.empty() && e.second.find(f) == std::string::npos) continue; s += (first ? "" : ",") + J(e.second); first = false; }
        McpOk(r, s + "]}");
        return;
    }
    if (c == "problems")
    {
        std::string s = "[";
        {
            std::lock_guard<std::mutex> lk(Problems().mx);
            for (size_t i = 0; i < Problems().items.size(); ++i)
            {
                const auto& it = Problems().items[i];
                s += (i ? "," : "") + std::string("{\"title\":") + J(it.title) + ",\"text\":" + J(it.text) + ",\"file\":" + J(it.file) + ",\"line\":" + std::to_string(it.line) + "}";
            }
        }
        McpOk(r, s + "]");
        return;
    }
    McpErr(r, "unknown command '" + c + "'");
}

void McpFinish(const Snapshot& snap)
{
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_mcpPending.size();)
    {
        McpPending& p = g_mcpPending[i];
        bool done = false;
        if (p.kind == "place")
        {
            for (const auto& o : snap.objects)
            {
                if (o.className != p.cls || std::find(p.before.begin(), p.before.end(), o.handle) != p.before.end()) continue;
                const double dx = o.location.x - p.loc.x, dy = o.location.y - p.loc.y, dz = o.location.z - p.loc.z;
                if (dx * dx + dy * dy + dz * dz > 600.0 * 600.0) continue;
                McpOk(p.r, McpObjJson(o));
                done = true;
                break;
            }
            if (!done && now > p.deadline) { McpErr(p.r, "Sent, but no new " + p.cls + " appeared near that spot within 12 s (see logs / problems: the server may have refused it)."); done = true; }
        }
        else if (p.kind == "props")
        {
            if (snap.inspectHandle == p.handle && snap.inspectPath == p.cls && !snap.props.empty())
            {
                std::string s = "{\"class\":" + J(snap.inspectClass) + ",\"props\":[";
                for (size_t k = 0; k < snap.props.size(); ++k)
                {
                    const auto& pi = snap.props[k];
                    s += (k ? "," : "") + std::string("{\"path\":") + J(pi.path) + ",\"value\":" + J(pi.value) + ",\"type\":" + std::to_string(pi.type) +
                         ",\"writable\":" + (pi.writable ? "true" : "false") + ",\"replicated\":" + (pi.net ? "true" : "false") +
                         (pi.link ? ",\"subObject\":true" : "") + (pi.inert ? ",\"inert\":true" : "") + "}";
                }
                McpOk(p.r, s + "]}");
                done = true;
            }
            else if (now > p.deadline) { McpErr(p.r, "No properties came back for that object."); done = true; }
        }
        else if (p.kind == "data")
        {
            if (snap.dataHandle == p.handle && snap.dataSerial != p.serial)
            {
                std::string s = "[";
                for (size_t k = 0; k < snap.data.size(); ++k)
                    s += (k ? "," : "") + std::string("{\"path\":") + J(snap.data[k].path) + ",\"kind\":" + J(snap.data[k].kind) + ",\"value\":" + J(snap.data[k].value) + "}";
                McpOk(p.r, s + "]");
                done = true;
            }
            else if (now > p.deadline) { McpErr(p.r, "The server sent no Game data for that object (not a sandbox object?)."); done = true; }
        }
        else if (p.kind == "slots")
        {
            if (snap.slotCandType == p.cls || now > p.deadline)
            {
                std::string s = "[";
                for (size_t k = 0; k < snap.slotCands.size(); ++k)
                    s += (k ? "," : "") + std::string("{\"handle\":") + J(snap.slotCands[k].handle) + ",\"component\":" + J(snap.slotCands[k].comp) + "}";
                McpOk(p.r, s + "]");
                done = true;
            }
        }
        else if (p.kind == "scripts")
        {
            if (!snap.gameScripts.empty() || now > p.deadline)
            {
                std::string s = "[";
                bool first = true;
                for (const auto& g : snap.gameScripts)
                {
                    if (!p.cls.empty() && g.name != p.cls) continue;
                    s += (first ? "" : ",") + std::string("{\"name\":") + J(g.name) + ",\"where\":" + J(g.where) +
                         (p.cls.empty() ? std::string() : ",\"source\":" + J(g.source)) + "}";
                    first = false;
                }
                McpOk(p.r, s + "]");
                done = true;
            }
        }
        else if (p.kind == "log")
        {
            if (now > p.deadline)
            {
                const auto lines = LogRingSince(static_cast<unsigned long long>(static_cast<unsigned>(p.serial)), 200);
                std::string s = "{\"log\":[";
                for (size_t k = 0; k < lines.size(); ++k) s += (k ? "," : "") + J(lines[k].second);
                McpOk(p.r, s + "]}");
                done = true;
            }
        }
        if (done) g_mcpPending.erase(g_mcpPending.begin() + i); else ++i;
    }
}

void McpPump(const Snapshot& snap)
{
    McpNetStart();
    std::vector<McpRawReq> in;
    McpNetPop(in);
    for (const auto& raw : in)
    {
        const McpReq r = McpParse(raw);
        try { McpHandle(snap, r); }
        catch (const std::exception& e) { McpErr(r, std::string("the editor failed on that request: ") + e.what()); }
        catch (...) { McpErr(r, "the editor failed on that request"); }
    }
    McpFinish(snap);
}
