// se_mcp_net.cpp -- the MCP bridge's sockets (its own file: winsock2.h must come before windows.h).
// Loopback-only TCP listener; lines in, lines out. The editor side (se_mcp_bridge.inc, game thread) pops
// request lines and sends replies through here.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <share.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#pragma comment(lib, "ws2_32.lib")

namespace se {
void Log(const char* fmt, ...);
struct McpRawReq { unsigned long long conn; std::string line; };   // (as in se_core.h, which pulls in windows.h first)
namespace {
std::mutex g_inMx, g_sendMx;
std::vector<McpRawReq> g_in;
std::atomic<int> g_port{ 0 };

DWORD WINAPI ConnThread(LPVOID p)
{
    const SOCKET s = static_cast<SOCKET>(reinterpret_cast<uintptr_t>(p));
    std::string buf;
    std::vector<char> rb(1 << 16);
    for (;;)
    {
        const int n = recv(s, rb.data(), static_cast<int>(rb.size()), 0);
        if (n <= 0) break;
        buf.append(rb.data(), n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos)
        {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::lock_guard<std::mutex> lk(g_inMx);
            g_in.push_back({ static_cast<unsigned long long>(s), line });
        }
    }
    closesocket(s);
    return 0;
}
DWORD WINAPI ListenThread(LPVOID)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("[mcp] WSAStartup failed"); return 0; }
    SOCKET ls = INVALID_SOCKET;
    for (int port = 47650; port <= 47654; ++port)
    {
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<u_short>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);                  // loopback only: never reachable from outside
        if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(ls, 4) == 0) { g_port = port; break; }
        closesocket(ls); ls = INVALID_SOCKET;
    }
    if (ls == INVALID_SOCKET) { Log("[mcp] no free port in 47650-47654: the MCP bridge is off"); return 0; }
    char tp[MAX_PATH]; GetTempPathA(MAX_PATH, tp);
    const std::string pf = std::string(tp) + "rigel_mcp_port.txt";
    if (FILE* f = _fsopen(pf.c_str(), "w", _SH_DENYNO)) { fprintf(f, "%d\n", g_port.load()); fclose(f); }
    Log("[mcp] bridge listening on 127.0.0.1:%d (the Rigel MCP server connects here)", g_port.load());
    for (;;)
    {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) { Sleep(200); continue; }
        Log("[mcp] an agent connected");
        CreateThread(nullptr, 0, ConnThread, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(c)), 0, nullptr);
    }
}
}  // namespace

void McpNetStart() { static bool s = false; if (!s) { s = true; CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr); } }
int McpNetPort() { return g_port.load(); }
void McpNetPop(std::vector<McpRawReq>& out) { std::lock_guard<std::mutex> lk(g_inMx); out.swap(g_in); }
void McpNetSend(unsigned long long conn, const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_sendMx);
    const std::string out = line + "\n";
    size_t off = 0;
    while (off < out.size())
    {
        const int n = send(static_cast<SOCKET>(conn), out.data() + off, static_cast<int>(out.size() - off), 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}
}  // namespace se
