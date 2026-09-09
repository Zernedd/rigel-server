// dllmain.cpp — HalcyonA2 bootstrap.
//
// Milestone 1: open the real map, drop local player 0, and flip the client
// into server mode. This game auto-listens once GIsServer/GIsClient are set,
// so no manual net-driver / InitListen code is needed (unlike the Fortnite
// reference project, see CLAUDE.md §4.2).
#include "pch.h"
#include "MinHook.h"
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <cstdarg>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

// ---------------------------------------------------------------------------
// Globals located in IDA (RVAs from module base). Both are adjacent bool bytes.
//   GIsClient : RVA 0x9650702
//   GIsServer : RVA 0x9650703
// ---------------------------------------------------------------------------
static constexpr uintptr_t GIsClient_RVA = HX::GIsClient_RVA;
static constexpr uintptr_t GIsServer_RVA = HX::GIsServer_RVA;

// ---------------------------------------------------------------------------
// Station ID. The server-login path that would normally populate this was
// compiled out of the client build, so this global has no writer in the image.
// It is a UE FString stored inline (a TArray<wchar_t>); the readers
// sub_541CB00 / sub_541C1B0 use it as the {stationId} in the REST path
// "/stations/{stationId}/users/{uid}/roles".
//   0x95579C0 : wchar_t* Data      ("qword_95579C0")
//   0x95579C8 : int32    ArrayNum  ("dword_95579C8" -- the "is set" / length check)
//   0x95579CC : int32    ArrayMax
// ArrayNum counts the null terminator (empty FString -> Num 0 -> "not set").
// ---------------------------------------------------------------------------
static constexpr uintptr_t StationStr_Data_RVA = HX::StationStr_Data_RVA;
static constexpr uintptr_t StationStr_Num_RVA = HX::StationStr_Num_RVA;
static constexpr uintptr_t StationStr_Max_RVA = HX::StationStr_Max_RVA;

// The station id now comes from the backend at spin-up (POST /register_server).
// g_stationId holds it for the process lifetime so the pointer handed to the game
// stays valid; kStationFallback is used only if registration fails.
static const wchar_t kStationFallback[] = L"2832873737dsfhjbsdbds3832";
static std::wstring   g_stationId;
static std::wstring   g_deploymentIdW;   // -DashboardDeploymentId (unique per running server); used to scope the Vivox channel per-server
static std::string    g_deploymentId;    // same id, narrow — the key for the periodic player-count heartbeat (POST /update_player_count)

// Backend (Astra emulator) host the DLL registers itself with. Set to the VPS IP;
// change to L"127.0.0.1" if you reach the backend through a local proxy/tunnel.
static const wchar_t* kBackendHost = L"157.173.194.216";
static constexpr int  kBackendPort = 78; // A2StationDbServer

static uintptr_t GetBase()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

// Write a value of arbitrary width to an address, restoring protection after.
template <typename T>
static void WriteMem(uintptr_t addr, T val)
{
    DWORD old;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), sizeof(T), PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile T*>(addr) = val;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), sizeof(T), old, &old);
}

static void WriteByte(uintptr_t addr, uint8_t val) { WriteMem<uint8_t>(addr, val); }

// Minimal blocking WinHTTP POST to a local server. Returns the response body
// (empty string on any failure).
static std::string HttpPostLocal(const wchar_t* host, int port, const wchar_t* path, const std::string& body)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"HalcyonA2/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (hConnect)
    {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest)
        {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                   (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
                && WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail)
                {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read))
                        break;
                    chunk.resize(read);
                    result += chunk;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// Log to BOTH the console and %TEMP%\HalcyonA2.log (our console prints don't land in the UE .log,
// which makes headless-server diagnosis painful — the file persists).
static FILE* g_hxLog = nullptr;
static void HxLog(const char* fmt, ...)
{
    if (!g_hxLog)
    {
        wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
        wchar_t path[MAX_PATH]{}; swprintf_s(path, L"%sHalcyonA2.log", tmp);
        _wfopen_s(&g_hxLog, path, L"a");
        if (g_hxLog) fwprintf(stdout, L"[HalcyonA2] file log -> %s\n", path);
    }
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_hxLog) { vfprintf(g_hxLog, fmt, ap2); fflush(g_hxLog); }
    va_end(ap2);
}

// General blocking WinHTTP request: any method, custom headers (CRLF-terminated), optional
// body. Returns the response body (empty on failure); *status receives the HTTP status code.
static std::string HttpReq(const wchar_t* host, int port, const wchar_t* method,
                           const wchar_t* path, const std::string& body,
                           const std::wstring& extraHeaders, DWORD* status = nullptr)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"HalcyonA2/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (hConnect)
    {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest)
        {
            const wchar_t* hdr = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                      : extraHeaders.c_str();
            DWORD hdrLen = extraHeaders.empty() ? 0 : (DWORD)-1L;
            if (WinHttpSendRequest(hRequest, hdr, hdrLen,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                   (DWORD)body.size(), (DWORD)body.size(), 0)
                && WinHttpReceiveResponse(hRequest, nullptr))
            {
                if (status)
                {
                    DWORD code = 0, sz = sizeof(code);
                    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
                    *status = code;
                }
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail)
                {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
                    chunk.resize(read);
                    result += chunk;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// Detect this box's PUBLIC (WAN) IP at runtime — the address clients actually connect to. Queries a
// plain-text IP-echo service, so it returns the public IP even behind NAT (AWS/VPS/home), NOT the
// private adapter IP. Tries a couple of services; returns "" if none give a valid IPv4.
static std::string GetPublicIp()
{
    struct EchoSvc { const wchar_t* host; const wchar_t* path; };
    static const EchoSvc svcs[] = {
        { L"checkip.amazonaws.com", L"/" },   // returns "1.2.3.4\n" (AWS-hosted, reliable on/off EC2)
        { L"api.ipify.org",         L"/" },   // returns "1.2.3.4"
        { L"ifconfig.me",           L"/ip" }, // returns "1.2.3.4"
    };
    for (const auto& s : svcs)
    {
        DWORD code = 0;
        std::string r = HttpReq(s.host, 80, L"GET", s.path, "", L"", &code);
        if (code != 200) continue;
        // trim surrounding whitespace/newlines
        size_t b = r.find_first_not_of(" \t\r\n");
        size_t e = r.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        r = r.substr(b, e - b + 1);
        // validate it's a bare IPv4 (7..15 chars, exactly 3 dots, digits+dots only)
        int dots = 0; bool ok = !r.empty() && r.size() <= 15;
        for (char c : r) { if (c == '.') ++dots; else if (c < '0' || c > '9') { ok = false; break; } }
        if (ok && dots == 3 && r.size() >= 7) return r;
    }
    return "";
}

// Pull a string value out of a flat JSON object: "key":"value".
static std::string ExtractJsonString(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Pull an integer out of JSON: "key": 123  (search starts at `from`).
static int ExtractJsonInt(const std::string& json, const std::string& key, size_t from = 0)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return 0;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return 0;
    return atoi(json.c_str() + colon + 1);
}

// Standard base64 decode (ignores non-alphabet chars / padding).
static std::string Base64Decode(const std::string& in)
{
    static int T[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)a[i]] = i;
        init = true;
    }
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in)
    {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// ---- Mothership per-player quest fetch (backend on 157.173.194.216:90) -------
// Confirmed flow: POST auth begin+complete (QUEST attestation is not validated) -> token
// (JWT sub = PlayerId GUID) -> GET userdata key_name=player_quests. The stored `value` is
// base64( each JSON byte - 1 ); we decode + (+1) back to the FA2QuestProgression JSON.
static const wchar_t* kMotherHost = L"157.173.194.216";
static const int      kMotherPort = 90;
// Shared secret with the backend (MothershipServer.SERVER_API_KEY). MUST match that value exactly.
// Sent as x-server-api-key so (a) our dummy-attestation quest-fetch logins are exempt from Meta
// verification, and (b) we can call the server-only /v1/server/authorized join-gate endpoint.
static const wchar_t* kServerApiKey = L"7b93a32d659d7bb73eb74c6f3f46a6a1934d47b383295a878f91ea2852d917d2";

// Return THIS process's bound UE game UDP port (default 7777, which UE auto-increments to 7778,
// 7779... if the port is already taken by another instance). Enumerates our own process's UDP
// sockets so multiple server instances on one box each register their REAL port instead of a
// hardcoded 7777 (which would publish the same ip:port for all of them). Returns 0 if not bound yet.
// AF_INET is the literal 2 to avoid winsock header-ordering hassle.
static int GetOurListenPort()
{
    const DWORD pid = GetCurrentProcessId();
    ULONG sz = 0;
    GetExtendedUdpTable(nullptr, &sz, FALSE, 2 /*AF_INET*/, UDP_TABLE_OWNER_PID, 0);
    if (!sz) return 0;
    std::vector<unsigned char> buf(sz);
    if (GetExtendedUdpTable(buf.data(), &sz, FALSE, 2 /*AF_INET*/, UDP_TABLE_OWNER_PID, 0) != NO_ERROR)
        return 0;
    auto* tbl = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(buf.data());
    int best = 0;
    for (DWORD i = 0; i < tbl->dwNumEntries; ++i)
    {
        if (tbl->table[i].dwOwningPid != pid) continue;
        const unsigned short np = static_cast<unsigned short>(tbl->table[i].dwLocalPort & 0xFFFF); // net order
        const int port = (np >> 8) | ((np & 0xFF) << 8);   // -> host order
        if (port >= 7777 && port <= 7877 && (best == 0 || port < best)) best = port;   // lowest UE port
    }
    return best;
}

static std::string MothershipLogin(const std::string& orgId)
{
    const std::wstring hdr = L"Content-Type: application/json\r\nx-server-api-key: " + std::wstring(kServerApiKey) + L"\r\n";
    HttpReq(kMotherHost, kMotherPort, L"POST", L"/v2/player/client/auth/begin/QUEST",
            "{\"UserId\":\"" + orgId + "\"}", hdr);
    std::string resp = HttpReq(kMotherHost, kMotherPort, L"POST",
            L"/v2/player/client/auth/complete/QUEST",
            "{\"UserId\":\"" + orgId + "\",\"AttestationToken\":\"x\",\"MetaNonce\":\"x\"}", hdr);
    return ExtractJsonString(resp, "Token");
}

// Returns the decoded player_quests JSON for an org id (empty on any failure).
static std::string FetchPlayerQuestsJson(const std::string& orgId)
{
    std::string token = MothershipLogin(orgId);
    HxLog("[HalcyonA2][QUEST] login org=%s tokenLen=%zu\n", orgId.c_str(), token.size());
    if (token.empty()) return "";
    std::wstring wtok(token.begin(), token.end());   // JWT is ASCII
    std::wstring hdr = L"x-server-api-key: " + std::wstring(kServerApiKey) + L"\r\nx-mothership-token: " + wtok + L"\r\n";
    DWORD code = 0;
    std::string resp = HttpReq(kMotherHost, kMotherPort, L"GET",
            L"/v1/userdata/client?key_name=player_quests", "", hdr, &code);
    std::string val = ExtractJsonString(resp, "value");
    HxLog("[HalcyonA2][QUEST] userdata GET http=%lu respLen=%zu valueLen=%zu\n",
          (unsigned long)code, resp.size(), val.size());
    if (code != 200 || val.empty()) return "";
    std::string raw = Base64Decode(val);
    for (char& c : raw) c = char((unsigned char)c + 1);   // Mothership stores each byte -1
    return raw;
}

using ProcessEvent_t = void (*)(SDK::UObject*, SDK::UFunction*, void*);
static ProcessEvent_t ProcessEvent_Orig = nullptr;

// ---------------------------------------------------------------------------
// Vivox VOIP config. UA2VOIPSubsystem has four Config FStrings normally loaded from
// the [/Script/A2.A2VOIPSubsystem] ini section — but only a real dedicated server's
// config carries them, so on our client-as-server they're empty and every getter
// early-outs ("...not running a dedicated server" / "we don't know our issuer or
// domain"). We set them on the live subsystem ourselves. Member offsets from the SDK:
//   Issuer @0x108, Domain @0x118, SigningKey @0x128, Server @0x138  (each an FString).
// Values are string literals (static storage), so the FString Data pointers stay valid
// for the process lifetime; these members are only read, never reassigned/freed.
// ---------------------------------------------------------------------------
static const wchar_t* kVoipIssuer     = L"20066-a2-61679-udash";
static const wchar_t* kVoipDomain     = L"mt2p.vivox.com";
static const wchar_t* kVoipSigningKey = L"1i5DWqTkWfNctrpVhiI6yvFyf8woZ6jZ";
static const wchar_t* kVoipServer     = L"https://mt2p.www.vivox.com/api2";
static bool g_voipConfigDone = false;

// Point an FString member (Data@+0, ArrayNum@+8, ArrayMax@+12) at a wchar_t* literal.
static void SetFStringMember(void* obj, size_t off, const wchar_t* str)
{
    int32_t n = 1;                          // count the null terminator
    for (const wchar_t* p = str; *p; ++p) ++n;
    const uintptr_t p = reinterpret_cast<uintptr_t>(obj) + off;
    *reinterpret_cast<const wchar_t**>(p) = str;   // Data
    *reinterpret_cast<int32_t*>(p + 8)    = n;     // ArrayNum
    *reinterpret_cast<int32_t*>(p + 12)   = n;     // ArrayMax
}

// One-shot: fill the Vivox config on every live UA2VOIPSubsystem. Retries each tick
// until the subsystem exists, then latches.
static void ApplyVoipConfigIfNeeded()
{
    auto* cls = SDK::UObject::FindClassFast("A2VOIPSubsystem");
    if (!cls)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
    int applied = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls))
            continue;
        SetFStringMember(o, HX::Voip_Issuer, kVoipIssuer);
        SetFStringMember(o, HX::Voip_Domain, kVoipDomain);
        SetFStringMember(o, HX::Voip_SigningKey, kVoipSigningKey);
        SetFStringMember(o, HX::Voip_Server, kVoipServer);
        ++applied;
    }
    if (applied > 0)
    {
        g_voipConfigDone = true;
        printf("[HalcyonA2] applied Vivox VOIP config to %d subsystem instance(s)\n", applied);
    }
}

// NOTE: station board population is handled the ORIGINAL way — the server fetches its
// deployment with include_station_config (sub_54198E0 -> GET {DashboardApiUrl}/v1/deployments/
// {DashboardDeploymentId}?include_station_config=true, x-api-key), and sub_5409770 natively
// applies the returned station_config to the netvars. That requires the -DashboardApiUrl /
// -DashboardApiKey / -DashboardDeploymentId cmdline args + a backend serving those endpoints;
// it is NOT done in this DLL. (The old LoadJSONIntoRootObject path was a client stub and the
// sub_52A0BF0 builder-hook / SetValue netvar hacks were removed once the real path was found.)

// ---------------------------------------------------------------------------
// Vivox login-token "f" claim fix.
//
// RequestVivoxLoginToken_Implementation (sub_53EF080) mints the client's Vivox login
// token but ignores the AccountId and builds the token's "f" (user URI) claim EMPTY
// (via sub_53E7F00 -> the JWT assembler sub_53FD500). Vivox then rejects the login with
// 20123 "Access Token Claims Mismatch" because "f" must equal the account being logged
// in: sip:.<issuer>.<userid>.@<domain>.
//
// We fix it with two hooks: the login-impl hook captures the AccountId and builds the
// URI into a thread-local; the JWT-assembler hook substitutes it into the empty "f"
// argument (a7) before signing. Claims are UTF-8 std::string (MSVC layout: size@0x10,
// capacity@0x18), same ABI as our DLL, so we pass our own std::string straight in.
// ---------------------------------------------------------------------------
static const char* kVoipIssuerA = "20066-a2-61679-udash";
static const char* kVoipDomainA = "mt2p.vivox.com";
static thread_local std::string t_voipLoginF;   // "f" URI for the in-flight login token

// Read a UE FString (wchar Data@0, Num@8) into a narrow ASCII std::string.
static std::string FStringToNarrow(void* fstr)
{
    if (!fstr) return "";
    wchar_t* data = *reinterpret_cast<wchar_t**>(fstr);
    int32_t  num  = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(fstr) + 8);
    if (!data || num <= 1) return "";
    std::string s;
    s.reserve(num);
    for (int32_t i = 0; i < num - 1 && data[i]; ++i) s.push_back(static_cast<char>(data[i]));
    return s;
}

// Build the Vivox user URI for the "f" claim. If AccountId is already a full sip: URI,
// use it verbatim; otherwise wrap the bare id: sip:.<issuer>.<id>.@<domain>.
static std::string BuildVoipUserUri(const std::string& accountId)
{
    if (accountId.empty()) return "";
    if (accountId.rfind("sip:", 0) == 0) return accountId;
    return std::string("sip:.") + kVoipIssuerA + "." + accountId + ".@" + kVoipDomainA;
}

// sub_53FD500 — the Vivox JWT assembler. (out, key, iss, exp, vxa, vxi, f, t); f/t are
// std::string*. When we're inside a login-token request (t_voipLoginF set) and "f" (a7)
// is empty, pass our URI instead so Vivox's claims check passes.
static constexpr uintptr_t JwtBuild_RVA = HX::JwtBuild_RVA;
using JwtBuild_t = void* (__fastcall*)(void*, void*, void*, unsigned, void*, unsigned, void*, void*);
static JwtBuild_t JwtBuild_Orig = nullptr;
static void* __fastcall JwtBuild_Hook(void* out, void* key, void* iss, unsigned exp,
                                      void* vxa, unsigned vxi, void* f, void* t)
{
    if (!t_voipLoginF.empty() && f &&
        *reinterpret_cast<size_t*>(reinterpret_cast<uintptr_t>(f) + 0x10) == 0)  // f is empty std::string
    {
        std::string uri = t_voipLoginF;   // lives through the (synchronous) build
        return JwtBuild_Orig(out, key, iss, exp, vxa, vxi, &uri, t);
    }
    return JwtBuild_Orig(out, key, iss, exp, vxa, vxi, f, t);
}

// sub_53EF080 — RequestVivoxLoginToken_Implementation. rdx = AccountId (FString*). The
// stock code drops it; we capture it, build the "f" URI, and let JwtBuild_Hook inject it.
static constexpr uintptr_t ReqLogin_RVA = HX::ReqLogin_RVA;
using ReqLogin_t = __int64 (__fastcall*)(void*, void*);
static ReqLogin_t ReqLogin_Orig = nullptr;
static __int64 __fastcall ReqLogin_Hook(void* comp, void* accountId)
{
    std::string prev = t_voipLoginF;
    std::string acct = FStringToNarrow(accountId);
    t_voipLoginF = BuildVoipUserUri(acct);
    printf("[HalcyonA2] VOIP login-token request: AccountId='%s' -> f='%s'\n",
           acct.c_str(), t_voipLoginF.c_str());
    __int64 r = ReqLogin_Orig(comp, accountId);
    t_voipLoginF = prev;
    return r;
}

// ---------------------------------------------------------------------------
// Vivox channel-join fix. RequestChannelJoinTokens_Impl = UA2VOIPSubsystem::BuildChannelUrl,
// which mints three join tokens (spatial/PTT/echo) via sub_53E7730 but with an empty "f"
// (user URI) AND an empty channel (both the token's "t" and the channel-ids sent to the
// client) — A2 keys the channel off voice-zone game state absent on our headless server. We
// inject one fixed GROUP channel ("confctl-g" = non-positional; everyone hears everyone) so
// connected players share a voice channel. Positional falloff is a later upgrade (per-zone
// channels + the subsystem's AudibleDistance/ConversationalDistance params).
//   sub_53E7730 (join-token builder): set the user URI (JwtBuild_Hook injects "f") + force
//     the channel arg (a4) so the token's "t" = our channel URI.
//   sub_52AD8B0 (sender): fill the empty channel-id outputs with the same URI. token "t" ==
//     channel id, so Vivox's claims check passes.
static const wchar_t kVoipChannelUri[] = L"sip:confctl-g-20066-a2-61679-udash.halcyon@mt2p.vivox.com";
struct FStringView { const wchar_t* Data; int32_t Num; int32_t Max; };
static FStringView g_voipChannel = {
    kVoipChannelUri,
    static_cast<int32_t>(sizeof(kVoipChannelUri) / sizeof(wchar_t)),   // Num incl. null
    static_cast<int32_t>(sizeof(kVoipChannelUri) / sizeof(wchar_t)),
};

// The channel name above was FIXED ("halcyon") so EVERY station joined the same Vivox channel ->
// players in different stations heard each other. Scope it per-SERVER instead: append this server's
// deployment id (unique per running server; falls back to the station id) to the channel name, so
// each server gets its own channel and no two ever share one. Built once, lazily, when the id is
// known (register runs before any client joins); the persistent std::wstring backs the FStringView.
static std::wstring g_voipChannelStr;
static void EnsureVoipChannel()
{
    if (!g_voipChannelStr.empty()) return;                       // already built
    const std::wstring& id = !g_deploymentIdW.empty() ? g_deploymentIdW : g_stationId;
    if (id.empty()) return;                                      // no id yet -> keep default this call
    std::wstring safe;                                           // Vivox channel token: alnum/-/_ only
    for (wchar_t c : id) if (iswalnum(c) || c == L'-' || c == L'_') safe += c;
    if (safe.empty()) return;
    g_voipChannelStr = L"sip:confctl-g-20066-a2-61679-udash.halcyon" + safe + L"@mt2p.vivox.com";
    g_voipChannel.Data = g_voipChannelStr.c_str();
    g_voipChannel.Num  = static_cast<int32_t>(g_voipChannelStr.size()) + 1;   // incl null
    g_voipChannel.Max  = g_voipChannel.Num;
    printf("[HalcyonA2] VOIP channel scoped per-server: %ls\n", g_voipChannelStr.c_str());
}

// sub_53E7730 — Vivox join-token builder (a1=subsystem, a2=&out, a3=AccountId FString,
// a4=channel FString). Set the user URI for the "f" injection, and force a4 to our channel.
static constexpr uintptr_t JoinBuild_RVA = HX::JoinBuild_RVA;
using JoinBuild_t = __int64 (__fastcall*)(void*, void*, void*, void*);
static JoinBuild_t JoinBuild_Orig = nullptr;
static __int64 __fastcall JoinBuild_Hook(void* subsystem, void* out, void* accountId, void* /*channel*/)
{
    EnsureVoipChannel();   // scope the channel to this server before injecting it
    std::string prev = t_voipLoginF;
    std::string acct = FStringToNarrow(accountId);
    t_voipLoginF = BuildVoipUserUri(acct);
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        printf("[HalcyonA2] VOIP join-token: AccountId='%s' f='%s' channel='%ls'\n",
               acct.c_str(), t_voipLoginF.c_str(), g_voipChannel.Data);
    }
    __int64 r = JoinBuild_Orig(subsystem, out, accountId, &g_voipChannel);   // t = our channel
    t_voipLoginF = prev;
    return r;
}

// sub_52AD8B0 — packs 3 (joinToken, channelId) pairs and sends ReceiveChannelJoinTokens.
// channelId args (a3/a5/a7) come in empty; substitute our channel URI for any empty one.
static constexpr uintptr_t SendJoin_RVA = HX::SendJoin_RVA;
using SendJoin_t = __int64 (__fastcall*)(void*, void*, void*, void*, void*, void*, void*);
static SendJoin_t SendJoin_Orig = nullptr;
static void* PickChannel(void* chan)   // FString*: Num@0x8. Fill only if empty.
{
    if (chan && *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(chan) + 8) == 0)
        return &g_voipChannel;
    return chan;
}
static __int64 __fastcall SendJoin_Hook(void* a1, void* tok1, void* chan1, void* tok2,
                                        void* chan2, void* tok3, void* chan3)
{
    EnsureVoipChannel();   // ensure the per-server channel is built before substituting empties
    return SendJoin_Orig(a1, tok1, PickChannel(chan1), tok2, PickChannel(chan2),
                         tok3, PickChannel(chan3));
}

// Latches / tick timers driven from the ProcessEvent hook (game thread).
static bool      g_trackerDone         = false;
static bool      g_ballSimDone         = false;
static void*     g_ballSimMgr          = nullptr;
static void*     g_ballSimSub          = nullptr;
static ULONGLONG g_lastPhysTick        = 0;
static ULONGLONG g_lastEnableGoals     = 0;
static ULONGLONG g_lastDetectGoals     = 0;
static ULONGLONG g_lastStationCfg      = 0;
static ULONGLONG g_lastOwnWatch        = 0;   // TEMP ownership-watch timer
static int       g_rbLogged            = 0;   // TEMP rollback-RPC trace counter
static int       g_vrPawnCount         = 0;   // live VRPawn count (set by WireVRPawns)
static std::unordered_map<void*, int> g_discSeen;   // TEMP: pawn -> last IN-SIM/SKIPPED verdict
static int       g_lastReconcileCount  = -1;  // player count at last reconcile
static int       g_newestInFrame       = -1;  // newest client input frame seen (rollback clock)
static int*      g_activeSimFramePtr    = nullptr; // frame counter of the sim a player occupies
static int       g_activeSimIdx         = -1;  // simIdx of g_activeSimFramePtr
// Per-player newest input frame (indexed by playerIdx) — so the step pump can hold the sim behind
// the SLOWEST player's confirmed inputs (min), not the fastest (global max). Holding behind the max
// makes every slower player's inputs perpetually stale = the residual ~15-35 MI.
struct PlayerFrame { int simIdx; int newest; ULONGLONG seen; };
static PlayerFrame g_playerFrames[256] = {};
static SDK::UClass* g_goalCls           = nullptr; // UGoalComponent class (for goal-overlap trace)
static ULONGLONG g_lastBallBuildTick   = 0;
static ULONGLONG g_lastBallStepTick    = 0;
static ULONGLONG g_lastBallOverlapTick = 0;
static ULONGLONG g_lastTeamColor       = 0;   // team-color init throttle
static ULONGLONG g_lastNetTune         = 0;   // net-driver send-rate re-apply throttle
static ULONGLONG g_lastFreqProbe       = 0;   // frequent-data timestamp probe throttle
static ULONGLONG g_lastBallPos         = 0;   // ball-position probe throttle
static ULONGLONG g_lastBallWire        = 0;   // sim-wire probe throttle
static void*     g_probeEntity         = nullptr; // cached active-remote UA2PlayerEntity for rate sampling
static int       g_probePidx           = -1;
// PING-STAMP FIX: cached {entity, pingMs} for every connected player with a real ping. The server
// knows each player's RTT (PlayerState::GetPingInMilliseconds) but the headless relay never copies it
// into FrequentData.Ping (stays 0), which collapses client-side interpolation -> remote players snap.
// We refresh this list at 1Hz (reflected ping call) and write the ping into both freq-data copies at
// fast-path rate so it survives the relay/replication. Ping@+0x8 within FrequentData; FrequentData is
// at localData@0xF0 and VRPlayerRepData@0x308 on UA2PlayerEntity.
struct FPingTarget { void* entity; float pingMs; };
static FPingTarget g_pingTargets[128];          // up to 115 players/station
static int         g_pingTargetCount = 0;
static bool        g_pingStampEnabled = true;   // console-toggleable A/B switch
// Last non-zero ping per entity. GetPingInMilliseconds() momentarily reads 0 on any network blip;
// without this hold, that transient drops the player from the stamp list for a second -> FrequentData.Ping
// goes 0 -> the other client's interpolation buffer COLLAPSES and latches -> persistent mid-match snap
// (only a grab re-inits it). Holding the last good ping (and flooring to 100 if never seen) means we
// never emit 0, so the collapse can't happen.
static std::unordered_map<void*, float> g_lastGoodPing;
static constexpr float kPingFloorMs = 100.0f;

// TEAM-COLOR STAMP FIX (mirrors the ping fix). Server_SetCurrentColor writes the Mass fragment, but
// the fragment->replicated-copy sync only carries pose (FrequentData), NOT the color, so the entity's
// replicated VRPlayerRepData@0x308 CurrentTeamColor@0x500 / TeamIndex@0x418 stay default and clients
// never see team colors. We write them directly on the entity (same struct that already replicates
// for pose) at fast-path rate. Registered at admit with the TicketManager palette color + team.
// Offsets within the entity object: VRPlayerRepData CurrentTeamColor@0x500 (0x14) / TeamIndex@0x418;
// localData CurrentTeamColor@0x2E8 / TeamIndex@0x200.
struct FColorTarget { void* entity; unsigned char color[HX::TeamColor_Size]; signed char teamIndex; };
static FColorTarget g_colorTargets[32];
static int          g_colorTargetCount = 0;
static bool         g_colorStampEnabled = true;
static void RegisterColorTarget(void* entity, const unsigned char* color, int teamIndex)
{
    if (!entity || teamIndex < 0) return;
    for (int i = 0; i < g_colorTargetCount; ++i)          // update existing
        if (g_colorTargets[i].entity == entity)
        { memcpy(g_colorTargets[i].color, color, HX::TeamColor_Size); g_colorTargets[i].teamIndex = (signed char)teamIndex; return; }
    if (g_colorTargetCount >= 32) return;                 // else append
    g_colorTargets[g_colorTargetCount].entity = entity;
    memcpy(g_colorTargets[g_colorTargetCount].color, color, HX::TeamColor_Size);
    g_colorTargets[g_colorTargetCount].teamIndex = (signed char)teamIndex;
    ++g_colorTargetCount;
}
static bool        g_freqDebug        = false;  // gate the [FREQ]/[WTIME]/[FREQRATE] diagnostic spam (fix confirmed; keep off)
static ULONGLONG g_lastQuestInit       = 0;   // player quest-init throttle
static ULONGLONG g_lastTraining        = 0;   // shooting/goalie practice tick throttle

// ---------------------------------------------------------------------------
// PrintString capture — read a UE FString and echo it to our console.
//
// `fstr` points at an FString = { wchar_t* Data @0x00; int32 Num @0x08; int32 Max }.
// Num counts the null terminator, so an empty string has Num <= 1.
//
// Reads raw memory that may be a bad pointer for a half-built object, so it is
// SEH-guarded — and therefore holds no C++ unwinding objects (MSVC C2712).
// ---------------------------------------------------------------------------
static void PrintCapturedString(void* fstr)
{
    __try
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(fstr);
        wchar_t* data = *reinterpret_cast<wchar_t**>(p);
        int32_t  num  = *reinterpret_cast<int32_t*>(p + 8);
        if (data && num > 1)
            printf("[HalcyonA2][Print] %ls\n", data);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Pump A2PhysicsSync::SendLatestData() on every physics-sync component. The ball's
// owner (the server, for free balls) simulates it and must call SendLatestData to
// replicate PhysicsSyncRepData to clients. On our client-as-server the engine's
// normal send path doesn't run, so the server sims the ball (it falls) but never
// sends the state -> clients see no free physics. Pump it ourselves (CLAUDE.md §4.3).
// -QuietSims (A/B test for the player-lag flood): when set, ALL our per-tick ball network work is
// skipped — the mgr+0x412 results-send pin, PumpBallSimStep (which emits Client_SendServerSimResults),
// and PumpPhysicsSync (SendLatestData). Balls stop syncing, but if players go smooth in every district
// with this on, our ball sends were the flood crowding out player pose. If still laggy, it's the
// game's own running-gamemode netcode, not our pumps.
static bool g_quietSims = false;

static void PumpPhysicsSync()
{
    auto* cls = SDK::UObject::FindClassFast("A2PhysicsSync");
    if (!cls)
        return;
    static SDK::UFunction* fnSend = nullptr;

    // CACHED sync-component + player-position lists, rebuilt ~1s. The old per-call full GObjects walk
    // (120k+) at 30Hz was a game-thread hog AND it SendLatestData'd every moving ball in EVERY running
    // gamemode to every client (station-wide flood that snapped player pose — "running gamemodes fry
    // it"). Now: iterate the small cached list, and only send a ball that's within ~100m of a player
    // (a ball nobody is near is invisible to everyone -> no reason to replicate it -> the 7 unwatched
    // arenas stop flooding). Proximity IS the "occupied arena" gate, no slot bookkeeping needed.
    static SDK::UObject* syncs[512]; static int nSync = 0;
    static SDK::FVector players[64]; static int nPlayers = 0;
    static ULONGLONG lastRebuild = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastRebuild > 1000 || nSync == 0)
    {
        lastRebuild = now; nSync = 0; nPlayers = 0;
        auto* pawnCls = SDK::UObject::FindClassFast("VRPawn");
        if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("BP_VRPawn_C");
        const int32_t num = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (nSync < 512 && o->IsA(cls)) { syncs[nSync++] = o; }
            else if (pawnCls && nPlayers < 64 && o->IsA(pawnCls))
            {
                auto* a = static_cast<SDK::AActor*>(o);
                if (a->RootComponent) players[nPlayers++] = a->RootComponent->K2_GetComponentLocation();
            }
        }
    }
    if (nPlayers == 0)   // nobody connected -> nothing to send
        return;

    const double THRESH2 = 1.0e8;   // (10000uu = 100m)^2 — only sync balls a player could see
    // [PUMP] instrumentation: prove the frozen-skip gate is actually engaging. Counts, per ~1s,
    // how many in-range balls we SENT vs SKIPPED-because-frozen. If skipped>0 for a resting
    // heartball, the gate works and any remaining teleport is native client-driven networking.
    static ULONGLONG pumpLastLog = 0; static int pumpSent = 0, pumpFrozen = 0, pumpFirst = 0;
    for (int i = 0; i < nSync; ++i)
    {
        auto* o = syncs[i];
        // SendLatestData derefs PropMovement(@0xA8) + its UpdatedComponent(@0xA8); both null on
        // pooled/inactive components (the null+0x252 crash). Guard, and reuse UpdatedComponent to
        // get the ball's world position for the proximity gate.
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        void* propMovement = *reinterpret_cast<void**>(p + 0xA8);
        if (!propMovement) continue;
        void* updated = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(propMovement) + 0xA8);
        if (!updated) continue;

        SDK::FVector loc = static_cast<SDK::USceneComponent*>(updated)->K2_GetComponentLocation();
        bool inRange = false;
        for (int j = 0; j < nPlayers && !inRange; ++j)
        {
            const double dx = loc.X - players[j].X, dy = loc.Y - players[j].Y, dz = loc.Z - players[j].Z;
            if (dx * dx + dy * dy + dz * dz < THRESH2) inRange = true;
        }
        if (!inRange) continue;   // no player nearby -> don't blast this ball to everyone

        // ONLY send balls the SERVER is actually moving. Client-driven grabbables (heartballs) are
        // FROZEN server-side; SendLatestData'ing their stale pos at 30Hz stomps the client's real
        // networked motion -> the ball snaps between the frozen server pos and the client pos (the
        // "teleport between two points"). If the server pos hasn't changed since last tick, the server
        // isn't simulating this ball -> skip it and let its native replication drive it.
        static std::unordered_map<void*, SDK::FVector> lastPos;
        auto it = lastPos.find(o);
        if (it != lastPos.end())
        {
            const double dx = loc.X - it->second.X, dy = loc.Y - it->second.Y, dz = loc.Z - it->second.Z;
            const bool movedServerSide = (dx * dx + dy * dy + dz * dz) > 0.25;   // >0.5uu since last tick
            lastPos[o] = loc;
            if (!movedServerSide) { ++pumpFrozen; continue; }   // frozen server-side (client-driven) -> don't override
        }
        else { lastPos[o] = loc; ++pumpFirst; continue; }  // first sight -> record, don't send yet

        if (!fnSend) fnSend = o->Class->GetFunction("A2PhysicsSync", "SendLatestData");
        if (!fnSend) return;
        o->ProcessEvent(fnSend, nullptr);
        ++pumpSent;
    }
    if (now - pumpLastLog > 1000)
    {
        pumpLastLog = now;
        printf("[HalcyonA2][PUMP] sent=%d frozen-skip=%d first-skip=%d (nSync=%d nPlayers=%d)\n",
               pumpSent, pumpFrozen, pumpFirst, nSync, nPlayers);
        pumpSent = pumpFrozen = pumpFirst = 0;
    }
}

// AVRPawn::Server_SubmitInputs (sub_5496E10) drops every client input unless
// VRPawn->BallSimManager (@0x1A58) is non-null. On our server the pawn's ref is
// likely never set (the arena/sim-join server path that would set it doesn't run),
// so the inputs we see arriving get discarded before reaching the sim. Point every
// VRPawn at our manager. Also logs the prior value so we learn whether it was null.
static void WireVRPawnBallSimManagers()
{
    if (!g_ballSimMgr)
        return;
    auto* cls = SDK::UObject::FindClassFast("VRPawn");   // native AVRPawn (catches BP_VRPawn_C)
    if (!cls)
        cls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    if (!cls)
    {
        printf("[HalcyonA2][RB] WireVRPawns: VRPawn class not found\n");
        return;
    }
    int found = 0, wired = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls))
            continue;
        ++found;

        // NET TUNING (player-movement snap fix). Outside the tackleball arenas (where pose rides the
        // forced-90Hz ball sim), remote player bodies snap: player pawns compete with the thousands of
        // static gamemode actors we load across every district for the per-connection actor-replication
        // budget, and get updated rarely. Pin each VRPawn to a high, non-adaptive update rate + high
        // priority so it always wins that budget, and never cull it by distance. AActor net fields:
        // NetCullDistanceSquared@0x170, NetUpdateFrequency@0x178, MinNetUpdateFrequency@0x17C, NetPriority@0x180.
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(o);
            *reinterpret_cast<uint8_t*>(p + 0x60) |= 0x08;    // bAlwaysRelevant — relevant to every connection regardless
                                                              // of distance from the (single, TKB-parked) relevancy viewer
            *reinterpret_cast<float*>(p + 0x170) = 1.0e12f;   // NetCullDistanceSquared — never distance-cull players
            *reinterpret_cast<float*>(p + 0x178) = 100.0f;    // NetUpdateFrequency — target 100Hz
            *reinterpret_cast<float*>(p + 0x17C) = 100.0f;    // MinNetUpdateFrequency — pin it (defeat adaptive down-throttle)
            *reinterpret_cast<float*>(p + 0x180) = 10.0f;     // NetPriority — win the actor budget over static objects
        }

        void** ref = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + HX::VRPawn_BallSimManager);
        if (*ref != g_ballSimMgr)
        {
            printf("[HalcyonA2][RB] VRPawn %s BallSimManager@0x1A58 was %p -> wiring to %p\n",
                   o->GetName().c_str(), *ref, g_ballSimMgr);
            *ref = g_ballSimMgr;
            ++wired;
        }

        // DISC-INTO-SIM check: sub_540D970 only adds a player to a ball sim if
        // *(pawn+0x328) != null AND that comp's +0x440 != null (+0x2FA is a byte flag).
        // Log per-pawn, once + on verdict change, so a joining REMOTE pawn always prints
        // (a global cap gets eaten by the local pawn re-logging every second).
        {
            const uintptr_t pw = reinterpret_cast<uintptr_t>(o);
            void* comp = *reinterpret_cast<void**>(pw + 0x328);
            void* f440 = comp ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(comp) + 0x440) : nullptr;
            const int verdict = (comp && f440) ? 1 : 0;
            auto it = g_discSeen.find(o);
            if (it == g_discSeen.end() || it->second != verdict)
            {
                printf("[HalcyonA2][DISC] %s comp@0x328=%p +0x440=%p => %s\n",
                       o->GetName().c_str(), comp, f440, verdict ? "IN-SIM" : "SKIPPED");
                g_discSeen[o] = verdict;
            }
        }
    }
    g_vrPawnCount = found;
}

// ---------------------------------------------------------------------------
// GamemodesTracker spawn.
//
// A2.GamemodesTracker::GlobalGetGamemodesInfo reads a global singleton weak-ptr
// (unk_9BD06B0) that a tracker sets on its own BeginPlay; with no tracker in the
// world it logs "No GamemodesTracker in scene". The dedicated-server path that
// would spawn BP_GamemodesTracker never ran on our client-as-server, so we spawn
// it ourselves. It replicates (Net trackedModes), so spawning on the server (we
// hold authority) reaches the whole lobby; each side's BeginPlay sets its own
// local singleton. SpawnActor must run on the game thread, so this is driven from
// the ProcessEvent hook as a latched one-shot.
// ---------------------------------------------------------------------------
static void SpawnGamemodesTrackerIfReady()
{
    if (g_trackerDone)
        return;

    auto* world = SDK::UWorld::GetWorld();
    if (!world)
        return;

    // UWorld::AuthorityGameMode @ 0x158 — non-null once a game world is up.
    auto* gameMode = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(world) + 0x158);
    if (!gameMode)
        return;

    // Only present once Station_Prime / arena content is loaded (not the frontend).
    auto* cls = SDK::UObject::FindClassFast("BP_GamemodesTracker_C");
    if (!cls)
        return;

    g_trackerDone = true; // latch before spawning (spawn re-enters ProcessEvent)

    SDK::FTransform xform{};
    xform.Rotation.X = 0.0; xform.Rotation.Y = 0.0; xform.Rotation.Z = 0.0; xform.Rotation.W = 1.0;
    xform.Scale3D.X = 1.0; xform.Scale3D.Y = 1.0; xform.Scale3D.Z = 1.0;

    auto* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        world, cls, xform,
        SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn,
        nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

    if (actor)
    {
        SDK::UGameplayStatics::FinishSpawningActor(actor, xform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
        printf("[HalcyonA2] spawned BP_GamemodesTracker -> 0x%llX\n", (unsigned long long)actor);
    }
    else
    {
        printf("[HalcyonA2] BeginDeferredActorSpawnFromClass returned null\n");
    }
}

// The ball sim is the ABallSimManager actor; offline the OfflineBallSimSubsystem
// spawns it, but on our forced-dedicated server that spawn never fired (managers=0)
// even though the subsystem already holds all 70 props + a local player. Spawn the
// manager ourselves on authority and wire it back to the subsystem (deferred spawn,
// CLAUDE.md §5 R6: set the back-ref BEFORE BeginPlay). It has Net properties
// (SimulationsOutline, Stats) so once it ticks its state replicates to clients.
static void SpawnBallSimManagerIfNeeded()
{
    if (g_ballSimDone)
        return;
    auto* world = SDK::UWorld::GetWorld();
    if (!world)
        return;
    auto* mgrCls = SDK::UObject::FindClassFast("BallSimManager");
    auto* subCls = SDK::UObject::FindClassFast("OfflineBallSimSubsystem");
    if (!mgrCls || !subCls)
        return;

    // Find the already-populated offline subsystem instance.
    SDK::UObject* sub = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(subCls)) { sub = o; break; }
    }
    if (!sub)
        return;

    // If a manager already exists (weakptr @0x3C set), don't double-spawn.
    int32_t mgrIdx = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(sub) + 0x3C);
    if (mgrIdx != 0)
    {
        g_ballSimDone = true;
        return;
    }

    g_ballSimDone = true;    // latch before spawning (spawn re-enters ProcessEvent)
    g_ballSimSub  = sub;

    SDK::FTransform xform{};
    xform.Rotation.W = 1.0;
    xform.Scale3D.X = 1.0; xform.Scale3D.Y = 1.0; xform.Scale3D.Z = 1.0;

    auto* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        world, mgrCls, xform,
        SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!actor)
    {
        printf("[HalcyonA2] BallSimManager deferred-spawn returned null\n");
        return;
    }

    // R6: set ABallSimManager::OfflineBallSimSubsystem @0x2F0 before BeginPlay so the
    // manager wires itself to the subsystem (and, we hope, registers back into it).
    *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(actor) + 0x2F0) = sub;

    SDK::UGameplayStatics::FinishSpawningActor(actor, xform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    g_ballSimMgr = actor;
    printf("[HalcyonA2] spawned ABallSimManager -> 0x%llX wired to OfflineBallSimSubsystem 0x%llX\n",
           (unsigned long long)actor, (unsigned long long)sub);
}

// The manager now exists + is wired, but SimulationsOutline stays 0 — nothing calls
// the "reconcile sims against current players" path (BallSimManager::sub_540D970:
// destroys stale sims, then walks the world player list and builds a sim per valid
// player via sub_540E8A0). On a real server a player-join event drives it; our
// headless join never fires it. Drive it ourselves so sims get built for connected
// VRPawns, then the manager's own tick steps them.
static void PumpBallSimBuild()
{
    if (!g_ballSimMgr)
        return;
    static auto reconcileSims = reinterpret_cast<void (*)(void*)>(GetBase() + 0x540D970);
    reconcileSims(g_ballSimMgr);
}

// The manager's per-frame stepper (BallSimManager tick, takes deltaSeconds). It runs
// the fixed 90Hz step loop that actually advances the balls. Offline the actor ticks
// itself; on our headless server it doesn't step (balls frozen), so drive it here.
static void PumpBallSimStep()
{
    if (!g_ballSimMgr)
        return;
    static auto stepSim = reinterpret_cast<void (*)(void*, float)>(GetBase() + 0x543F2D0);
    const uintptr_t gate = reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x412;
    const float FIXED_DT = 1.0f / 90.0f;   // rollback sim runs at a fixed 90Hz timestep
    const int   DELAY    = 2;

    // Keep the results-send phase (Client_SendServerSimResults) armed EVERY tick, not just inside
    // the (often-skipped) catch-up loop below. The game itself advances the sim on input ingest, so
    // our loop frequently doesn't run — but the send only fires when mgr+0x412 is set, and a stale 0
    // means clients stop getting confirmations and predict far ahead (MI climbs). Arm it always.
    *reinterpret_cast<uint8_t*>(gate) = 1;

    // INPUT-DELAY + CATCH-UP (anti-MI + anti-lag). Target frame = newest confirmed input - DELAY.
    // - If the sim is BEHIND the target (it fell behind at 1 step/tick while inputs arrive faster),
    //   step MULTIPLE fixed frames this tick to CATCH UP (capped) — otherwise it lags forever,
    //   inputs pile up and get dropped as stale (the MI + rubber-band the user saw).
    // - If the sim is AT/PAST the target, HOLD — never run ahead of confirmed inputs.
    // (SEH from the SafePumpBallSimStep wrapper — a stale frame ptr just skips this tick.)
    if (g_activeSimFramePtr && g_newestInFrame >= 0)
    {
        // Hold behind the SLOWEST active player in the active sim (min of their newest frames), so
        // every player's inputs for a frame have arrived before the sim steps it -> no missed inputs.
        // (Holding behind the global max/fastest player left the slower player's inputs stale = MI.)
        const ULONGLONG now = GetTickCount64();
        int minNewest = 0x7FFFFFFF, maxNewest = -1, activeN = 0;
        for (int p = 0; p < 256; ++p)
        {
            const PlayerFrame& pf = g_playerFrames[p];
            if (pf.newest < 0 || pf.simIdx != g_activeSimIdx) continue;
            if (now - pf.seen > 500) continue;   // player stopped submitting -> drop from the min
            if (pf.newest < minNewest) minNewest = pf.newest;
            if (pf.newest > maxNewest) maxNewest = pf.newest;
            ++activeN;
        }
        const int newestForTarget = (activeN > 0) ? minNewest : g_newestInFrame;
        const int target = newestForTarget - DELAY;

        int budget = 12;   // cap frames/tick so a big backlog can't cause one giant hitch
        int stepped = 0;
        while (*g_activeSimFramePtr < target && budget-- > 0)
        {
            *reinterpret_cast<uint8_t*>(gate) = 1;   // arm the results/send phase (sub_543F2D0)
            stepSim(g_ballSimMgr, FIXED_DT);
            ++stepped;
        }

        static ULONGLONG lastLog = 0;
        if (now - lastLog > 1000)
        {
            lastLog = now;
            HxLog("[HalcyonA2][MI] sim=%d simFrame=%d target=%d players=%d spread=%d (min=%d max=%d) stepped=%d DELAY=%d\n",
                  g_activeSimIdx, *g_activeSimFramePtr, target, activeN,
                  (maxNewest >= 0 ? maxNewest - minNewest : 0), minNewest, maxNewest, stepped, DELAY);
        }
        return;
    }

    // No active sim yet (pre-join / idle) — keep the manager ticking at wall-clock.
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    float dt = last ? static_cast<float>(now - last) / 1000.0f : FIXED_DT;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    *reinterpret_cast<uint8_t*>(gate) = 1;
    stepSim(g_ballSimMgr, dt);
}

// TEMP diagnostic: HeartBalls replicate/sync between clients but arena JakeBalls don't.
// Are there multiple BallSimManagers (per-arena) we're NOT driving, or one manager whose
// arena sims we don't reconcile? Log every live BallSimManager + its SimulationsOutline
// count (Num@0x358), and the role/repl of jakeball vs heartball actors. Latched to a few
// prints so it isn't spam. Remove once diagnosed.
static int      g_ballDbg     = 0;
static ULONGLONG g_lastBallDbg = 0;
static void DumpBallStructure()
{
    const int32_t num = SDK::UObject::GObjects->Num();
    auto* mgrCls   = SDK::UObject::FindClassFast("BallSimManager");
    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    printf("[HalcyonA2][BallDbg] --- ball structure ---\n");
    int mgrCount = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !mgrCls || !o->IsA(mgrCls)) continue;
        int32_t simNum = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x358);
        auto* ma = static_cast<SDK::AActor*>(o);
        printf("[HalcyonA2][BallDbg]   BallSimManager %s sims=%d role=%d remote=%d repl=%d%s\n",
               o->GetName().c_str(), simNum, (int)ma->Role, (int)ma->RemoteRole, (int)ma->bReplicates,
               (o == g_ballSimMgr) ? " (ours,driven)" : " (NOT driven)");
        ++mgrCount;
    }
    int jake = 0, heart = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !actorCls || !o->IsA(actorCls)) continue;
        std::string n = o->GetName();
        bool isJake  = n.rfind("BP_JakeBall_C", 0) == 0;
        bool isHeart = (n.rfind("BP_HeartBall_C", 0) == 0 || n.rfind("BP_HeartBall_small_C", 0) == 0);
        if (!isJake && !isHeart) continue;
        auto* a = static_cast<SDK::AActor*>(o);
        if (isJake && jake < 8)
        {
            SDK::FVector wl{};
            if (a->RootComponent) wl = a->RootComponent->K2_GetComponentLocation();
            printf("[HalcyonA2][BallDbg]   JAKE  %-26s role=%d remote=%d repl=%d world=(%.0f,%.0f,%.0f)\n",
                   n.c_str(), (int)a->Role, (int)a->RemoteRole, (int)a->bReplicates,
                   (double)wl.X, (double)wl.Y, (double)wl.Z);
        }
        if (isHeart && heart < 4)
            printf("[HalcyonA2][BallDbg]   HEART %-26s role=%d remote=%d repl=%d\n",
                   n.c_str(), (int)a->Role, (int)a->RemoteRole, (int)a->bReplicates);
        if (isJake) ++jake; if (isHeart) ++heart;
    }
    printf("[HalcyonA2][BallDbg]   managers=%d jakeballs=%d heartballs=%d (role: 3=Auth 2=AutoProxy 1=SimProxy)\n",
           mgrCount, jake, heart);
}

// SEH guards for the per-tick sim driving. The step/build/send paths advance rollback
// renderers whose skeletal meshes can be half-initialized (null mesh -> AV in native
// bone-transform code, sub_1B7BC80). Guard so a bad object skips a frame instead of
// killing the process (these wrappers hold no C++ unwinding objects, so __try is legal).
static void SafePumpBallSimBuild()  { __try { PumpBallSimBuild(); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SafePumpBallSimStep()   { __try { PumpBallSimStep(); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SafePumpPhysicsSync()   { __try { PumpPhysicsSync(); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }

// PLAYER-MOVEMENT SMOOTHNESS. Outside the tackleball arenas, player pose rides NORMAL actor
// replication, whose send rate is capped by the net driver's NetServerMaxTickRate. If that's the UE
// default (~30) it's too coarse for VR head/hands -> remote players snap. In TKB the pose instead
// rides the ball rollback sim's Client_SendServerSimResults at 90Hz, bypassing the cap -> smooth.
// That's why per-pawn NetUpdateFrequency=100 did nothing (the DRIVER cap overrides it). Raise the
// driver's rate to 90 so normal replication matches the arena rate everywhere. UNetDriver fields:
// NetServerMaxTickRate@0x84, MaxNetTickRate@0xA0.
static void TuneNetDriver()
{
    auto* world = SDK::UWorld::GetWorld();
    static int dbg0 = 0;
    if (!world || !world->NetDriver)
    {
        if (dbg0 < 20) { ++dbg0; printf("[HalcyonA2][NET][dbg] world=%p NetDriver=%p (null -> waiting)\n",
                                        (void*)world, (void*)(world ? world->NetDriver : nullptr)); }
        return;
    }
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    // VALIDITY GATE — this runs on a 2s ticker that can fire mid-travel, when GWorld is the half-built
    // new world and world->NetDriver is stale/garbage (during LoadMap it briefly pointed at a NON-
    // UNetDriver object; writing NetServerMaxTickRate into it corrupted a shader-map struct -> crash in
    // the material registry reading null+0x10). So prove it's a real, LISTENING GameNetDriver first:
    //   (a) its vtable pointer lands inside the exe image (a genuine UNetDriver vtable is in .rdata),
    //   (b) ClientConnections.Num() (@0xD8) > 0 -> InitListen finished and a client is actually bound.
    // Both reads are cheap ints; the caller (SafeTuneNetDriver) SEH-wraps us so a faulting read on a
    // garbage pointer just aborts this pass instead of crashing.
    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase)
    {
        imgBase = GetBase();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
        imgSize   = nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);       // UNetDriver vtable ptr
    const int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8);   // ClientConnections.Num()
    const int32_t cur      = *reinterpret_cast<int32_t*>(nd + 0x84);   // NetServerMaxTickRate

    // DIAGNOSTIC (throttled ~20x): show what we actually see so we know which gate is failing when the
    // "-> 90" line never prints. Remove once tuning confirmed working.
    static int dbg = 0;
    if (dbg < 20)
    {
        ++dbg;
        printf("[HalcyonA2][NET][dbg] NetDriver=%p vt=%p inImg=%d numConns=%d curRate=%d\n",
               (void*)nd, (void*)vt, (vt >= imgBase && vt < imgBase + imgSize) ? 1 : 0, numConns, cur);
    }

    if (vt < imgBase || vt >= imgBase + imgSize) return;          // not a real in-image object -> bail
    if (numConns <= 0) return;                                    // ClientConnections.Num() == 0 -> not listening yet

    if (cur != 90)
    {
        printf("[HalcyonA2][NET] NetServerMaxTickRate %d -> 90 (MaxNetTickRate -> 90) — un-cap non-arena player pose\n", cur);
        *reinterpret_cast<int32_t*>(nd + 0x84) = 90;   // NetServerMaxTickRate
        *reinterpret_cast<int32_t*>(nd + 0xA0) = 90;   // MaxNetTickRate
    }
}
static void SafeTuneNetDriver() { __try { TuneNetDriver(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SNAP DIAGNOSIS. Remote players snap because the client can't interpolate their pose: each pose
// snapshot (FReplicatedFrequentData) carries FTimestamp{Seconds@0,fractional@4} (server clock), and
// the client lerps buffered snapshots against it. This probe reads, per VRPawn on the SERVER, that
// timestamp from LocalFreqData (AVRPawn+0xF20 -> Timestamp@+0xF20/.frac@+0xF24, Ping@+0xF28) once/sec.
// If Seconds/frac advance smoothly ~real-time, the server stream is fine and the bug is client
// interp / server-time sync; if frozen/zero/sparse, the server isn't producing/stamping it and we fix
// it server-side. PlayerIndex@0x1B48 identifies the pawn.
static void ProbeFreqTimestamps()
{
    auto* cls = SDK::UObject::FindClassFast("VRPawn");
    if (!cls) cls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    if (!cls) return;
    // Server world-time sync: clients sync their clock to AGameState.ReplicatedWorldTimeSecondsDouble
    // (@0x2C8, Net). If it's frozen/zero the server isn't advancing/replicating world time -> clients
    // can't place the pose Timestamps on their timeline -> snap. Compare its value to the [FREQ] ts.
    if (g_freqDebug) if (auto* w = SDK::UWorld::GetWorld())
    {
        void* gs = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(w) + 0x160);   // UWorld.GameState
        if (gs)
        {
            const double rwtsD = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(gs) + 0x2C8);
            const float  rwtsF = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(gs) + 0x2C4);
            printf("[HalcyonA2][WTIME] ReplicatedWorldTimeSecondsDouble=%.3f float=%.3f\n", rwtsD, rwtsF);
        }
        else printf("[HalcyonA2][WTIME] GameState=null\n");
    }

    // [PARR] TEMP diagnostic: dump every PlayerState (GameState.PlayerArray@0x2B0) with its
    // CompressedPing@0x2A0, bIsSpectator@0x2A2 bit1, and reflected GetPingInMilliseconds. Goal: does
    // the SPECTATOR's PlayerState have a real ping, or is CompressedPing 0 (so its client thinks its
    // own ping is 0 -> snaps everyone it watches)? VR players' PlayerState ping worked (62ms).
    if (g_freqDebug) if (auto* w2 = SDK::UWorld::GetWorld())
    {
        void* gs = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(w2) + 0x160);
        if (gs)
        {
            void** arr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(gs) + 0x2B0);
            const int pnum = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(gs) + 0x2B8);
            for (int k = 0; arr && k < pnum && k < 12; ++k)
            {
                auto* ps = reinterpret_cast<SDK::UObject*>(arr[k]);
                if (!ps) continue;
                const uintptr_t psa = reinterpret_cast<uintptr_t>(ps);
                const uint8_t cping = *reinterpret_cast<uint8_t*>(psa + 0x2A0);
                const bool isSpec = (*reinterpret_cast<uint8_t*>(psa + 0x2A2) >> 1) & 1;
                float pingMs = -1.0f;
                if (ps->Class) { auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
                                 if (fn) { char pb[8] = {}; ps->ProcessEvent(fn, pb); pingMs = *reinterpret_cast<float*>(pb); } }
                const wchar_t* nm = *reinterpret_cast<wchar_t**>(psa + 0x330);   // PlayerNamePrivate.Data
                printf("[HalcyonA2][PARR] ps[%d]=%s name='%ls' compressedPing=%u spec=%d GetPingMs=%.1f\n",
                       k, ps->GetName().c_str(), nm ? nm : L"", cping, isSpec ? 1 : 0, pingMs);
            }
        }
    }

    const int32_t num = SDK::UObject::GObjects->Num();
    int shown = 0;           // gates per-pawn debug prints to the first few (spam guard at scale)
    g_pingTargetCount = 0;   // rebuild the ping-stamp target list each pass
    for (int32_t i = 0; i < num; ++i)   // process EVERY player (up to 115/station), not just the first 8
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const int      pidx = *reinterpret_cast<unsigned char*>(p + HX::VRPawn_PlayerIndex);

        // Real player pose is on the Entity (AVRPawn.Entity@0x840 -> UA2PlayerEntity), NOT the pawn's
        // LocalFreqData (that's a client-only mirror, zero on the server). Two copies of
        // FReplicatedVRPlayerData, whose FrequentData{Timestamp@0,Ping@8,Root.pos@10} is at offset 0:
        //   localData@0xF0        = server's working copy from the incoming Server_SetFrequentData RPC
        //   VRPlayerRepData@0x308 = the Net-replicated copy actually sent to other clients
        void* entity = *reinterpret_cast<void**>(p + HX::VRPawn_Entity);
        if (!entity) { if (g_freqDebug && shown < 8) printf("[HalcyonA2][FREQ] pawn=%s pidx=%d Entity=null\n", o->GetName().c_str(), pidx); ++shown; continue; }
        const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
        auto rd = [&](uintptr_t off, int32_t& s, unsigned& f, float& pg, double& x) {
            s  = *reinterpret_cast<int32_t*>(e + off + 0x0);
            f  = *reinterpret_cast<uint16_t*>(e + off + 0x4);
            pg = *reinterpret_cast<float*>(e + off + 0x8);
            x  = *reinterpret_cast<double*>(e + off + 0x10);   // Root.position.X
        };
        int32_t ls, rs; unsigned lf, rf; float lp, rp; double lx, rx;
        rd(HX::Ent_FreqLocal, ls, lf, lp, lx);   // localData
        rd(HX::Ent_FreqRep, rs, rf, rp, rx);   // VRPlayerRepData (replicated)

        // Does the server actually KNOW this player's ping? Call APlayerState::GetPingInMilliseconds
        // (reflected, returns ExactPing-ms or compressedPing*4). PlayerState @ pawn+0x2B8. If nonzero,
        // the fix is to feed it into FrequentData.Ping; if zero, the server isn't measuring RTT at all.
        float psPingMs = -1.0f;
        auto* ps = *reinterpret_cast<SDK::UObject**>(p + 0x2B8);
        if (ps && ps->Class)
        {
            auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
            if (fn) { char pb[8] = {}; ps->ProcessEvent(fn, pb); psPingMs = *reinterpret_cast<float*>(pb); }
        }
        if (g_freqDebug && shown < 8)
            printf("[HalcyonA2][FREQ] pidx=%d local ts=%d+%u ping=%.1f X=%.0f | REP ts=%d+%u ping=%.1f X=%.0f | PS.ping=%.1fms\n",
                   pidx, ls, lf, lp, lx, rs, rf, rp, rx, psPingMs);
        // Cache an ACTIVE remote (real pose: nonzero rep ts/X) for the fast-path rate sampler.
        if (rs != 0 || rx != 0.0) { g_probeEntity = entity; g_probePidx = pidx; }
        // NOTE: ping-stamp targets are now built by the UA2PlayerEntity walk BELOW (class-agnostic), so
        // SPECTATOR pawns (BP_SpectatorPawn / camera pawn — NOT a VRPawn) are covered too. This VR-pawn
        // loop stays only for the [FREQ]/[TCOL2] debug + the active-remote cache above.

        // [TCOL2] Watch the entity's OBJECT-SIDE color copies over time. Server_SetCurrentColor writes
        // the Mass FRAGMENT; a Mass processor is supposed to sync fragment -> VRPlayerRepData@0x308
        // (the replicated copy) -> clients. localData CurrentTeamColor@0x2E8/TeamIdx@0x200;
        // VRPlayerRepData CurrentTeamColor@0x500/TeamIdx@0x418. If 0x500 never becomes the real color,
        // the fragment->replicated sync is dropping it (the bug); clients only ever see the default.
        if (g_freqDebug && shown < 8)
        {
            const unsigned char* ld = reinterpret_cast<unsigned char*>(e + HX::Ent_ColorLocal);
            const unsigned char* rd2 = reinterpret_cast<unsigned char*>(e + HX::Ent_ColorRep);
            const signed char ldTi = *reinterpret_cast<signed char*>(e + HX::Ent_TeamIdxLocal);
            const signed char rdTi = *reinterpret_cast<signed char*>(e + HX::Ent_TeamIdxRep);
            printf("[HalcyonA2][TCOL2] pidx=%d local(color=%u,%u,%u,%u ti=%d) REP(color=%u,%u,%u,%u ti=%d)\n",
                   pidx, ld[0], ld[1], ld[2], ld[3], ldTi, rd2[0], rd2[1], rd2[2], rd2[3], rdTi);
        }
        ++shown;
    }

    // Build the ping-stamp targets from UA2PlayerEntity DIRECTLY (class-agnostic). The entity is what
    // carries FrequentData (Ping@0xF0+8 / 0x308+8); it exists for VR players AND spectators, whereas the
    // VR-pawn loop above only sees VRPawns. Walking entities is why the SPECTATOR now gets ping-stamped
    // too — before, its FrequentData.Ping stayed 0 so it snapped both ways (others saw it snap, and its
    // own 0-ping entity collapsed its interp of everyone else). Ping comes from the entity's Pawn
    // (UA2PlayerEntity.Pawn@0xE8 -> APawn.PlayerState@0x2B8 -> GetPingInMilliseconds), floored + held.
    auto* entCls = SDK::UObject::FindClassFast("A2PlayerEntity");
    if (entCls)
    {
        g_pingTargetCount = 0;
        int withPawn = 0;
        for (int32_t i = 0; i < num && g_pingTargetCount < 128; ++i)
        {
            auto* eo = SDK::UObject::GObjects->GetByIndex(i);
            if (!eo || eo->IsDefaultObject() || !eo->IsA(entCls)) continue;
            const uintptr_t e2 = reinterpret_cast<uintptr_t>(eo);
            void* pawn = *reinterpret_cast<void**>(e2 + 0xE8);          // UA2PlayerEntity.Pawn
            float psPing = -1.0f;
            if (pawn)
            {
                ++withPawn;
                auto* ps = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pawn) + 0x2B8);   // APawn.PlayerState
                if (ps && ps->Class)
                {
                    auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
                    if (fn) { char pb[8] = {}; ps->ProcessEvent(fn, pb); psPing = *reinterpret_cast<float*>(pb); }
                }
            }
            float stamp = psPing;
            if (stamp > 0.0f) g_lastGoodPing[eo] = stamp;                // hold last-good so a transient 0 read never propagates
            else { auto it = g_lastGoodPing.find(eo); stamp = (it != g_lastGoodPing.end()) ? it->second : kPingFloorMs; }
            g_pingTargets[g_pingTargetCount].entity = eo;
            g_pingTargets[g_pingTargetCount].pingMs = stamp;
            ++g_pingTargetCount;
        }
        if (g_freqDebug)
            printf("[HalcyonA2][PINGSTAMP] entities=%d withPawn=%d (VR + spectators)\n", g_pingTargetCount, withPawn);
    }
}
static void SafeProbeFreqTimestamps() { __try { ProbeFreqTimestamps(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// BALL-SYNC DIAGNOSIS. Golf/heart balls "teleport between spawn and hit point" for remote clients =
// the server only has 2 states (spawn, post-hit rest) because the ball's flight isn't simulated
// server-side. Confirm by logging any DiscEntity whose SERVER position moved >10u since the last
// ~200ms sample: a hit ball that STREAMS (many [BALLPOS] lines tracing the arc) = server simulates it
// (then it's a SendLatestData rate/interp fix); a single JUMP = no server simulation (the root).
struct FBallPos { double x, y, z; };
static void ProbeBallPositions()
{
    static std::unordered_map<void*, FBallPos> lastPos;
    auto* cls = SDK::UObject::FindClassFast("DiscEntity");
    if (!cls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls) || !o->Class) continue;
        auto* fn = o->Class->GetFunction("Actor", "K2_GetActorLocation");
        if (!fn) continue;
        double loc[3] = {};
        o->ProcessEvent(fn, loc);
        auto it = lastPos.find(o);
        if (it != lastPos.end())
        {
            const double dx = loc[0]-it->second.x, dy = loc[1]-it->second.y, dz = loc[2]-it->second.z;
            const double d2 = dx*dx + dy*dy + dz*dz;
            if (d2 > 100.0)
                printf("[HalcyonA2][BALLPOS] %s -> (%.0f,%.0f,%.0f) moved=%.0f\n",
                       o->GetName().c_str(), loc[0], loc[1], loc[2], sqrt(d2));
        }
        lastPos[o] = FBallPos{ loc[0], loc[1], loc[2] };
    }
}
static void SafeProbeBallPositions() { __try { ProbeBallPositions(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SIM-WIRE probe (golf + heartball fix). reconcile sub_540D970 adds a player+ball to the ball sim
// only if the pawn's disc component (pawn+0x328) has a ball at +0x440 AND the flag at +0x2FA(762) is
// set. Jakeball admission wires this; golf/heart balls (server-spawned via BallSpawner, not admission)
// likely don't → reconcile skips them → frozen server-side. Dump the wire state per pawn so we confirm
// the gap and see which ball (class) is/should be wired. Read-only (no sim mutation).
// Is p a plausible live UObject (vtable ptr lands in the exe image)? SEH-guarded read.
static bool WirePtrLooksReal(void* p)
{
    if (!p) return false;
    static uintptr_t ib = 0, is = 0;
    if (!ib) { ib = GetBase(); auto* d = reinterpret_cast<IMAGE_DOS_HEADER*>(ib);
               is = reinterpret_cast<IMAGE_NT_HEADERS*>(ib + d->e_lfanew)->OptionalHeader.SizeOfImage; }
    __try { const uintptr_t vt = *reinterpret_cast<uintptr_t*>(p); return vt >= ib && vt < ib + is; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Read an actor's world position (K2_GetActorLocation) into out[3]; false if not resolvable.
static bool WireActorLoc(void* actor, double out[3])
{
    if (!WirePtrLooksReal(actor)) return false;
    auto* o = reinterpret_cast<SDK::UObject*>(actor);
    if (!o->Class) return false;
    auto* fn = o->Class->GetFunction("Actor", "K2_GetActorLocation");
    if (!fn) return false;
    o->ProcessEvent(fn, out);
    return true;
}
// One pawn's real ball refs (correct AVRPawn offsets): currentDiscEntity@0x1130, HeartBall@0x1DC0,
// Entity@0x840, BallSimManager@0x1A58, PersonalBallSpawner@0x1B40. SEH lives in the Safe wrapper.
static void ProbeOnePawnWire(SDK::UObject* o)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(o);
    const int pidx = *reinterpret_cast<unsigned char*>(p + HX::VRPawn_PlayerIndex);
    void* curDisc = *reinterpret_cast<void**>(p + HX::VRPawn_CurrentDisc);
    void* heart   = *reinterpret_cast<void**>(p + HX::VRPawn_HeartBall);
    void* simMgr  = *reinterpret_cast<void**>(p + HX::VRPawn_BallSimManager);
    auto nm = [](void* x) -> std::string {
        return WirePtrLooksReal(x) ? reinterpret_cast<SDK::UObject*>(x)->GetName() : std::string(x ? "(bad)" : "(null)");
    };
    double dloc[3] = {}, hloc[3] = {};
    const bool haveD = WireActorLoc(curDisc, dloc);
    const bool haveH = WireActorLoc(heart, hloc);
    printf("[HalcyonA2][BALLWIRE] pidx=%d curDisc=%s%s heartBall=%s%s simMgr=%p\n",
           pidx, nm(curDisc).c_str(), haveD ? "" : " (noloc)",
           nm(heart).c_str(), haveH ? "" : " (noloc)", simMgr);
    if (haveD) printf("[HalcyonA2][BALLWIRE]   curDisc pos=(%.0f,%.0f,%.0f)\n", dloc[0], dloc[1], dloc[2]);
    if (haveH) printf("[HalcyonA2][BALLWIRE]   heart   pos=(%.0f,%.0f,%.0f)\n", hloc[0], hloc[1], hloc[2]);
}
static void SafeProbeOnePawnWire(SDK::UObject* o)
{ __try { ProbeOnePawnWire(o); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[HalcyonA2][BALLWIRE] (pawn read faulted)\n"); } }
static void ProbeOneBallOwner(SDK::UObject* o)
{
    const std::string nm = o->GetName();
    if (nm.find("Golf") == std::string::npos && nm.find("Heart") == std::string::npos) return;
    void* spawner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x408);
    printf("[HalcyonA2][BALLWIRE] ball=%s spawnerPawn=%p\n", nm.c_str(), spawner);
}
static void SafeProbeOneBallOwner(SDK::UObject* o)
{ __try { ProbeOneBallOwner(o); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void ProbeBallWire()
{
    auto* cls = SDK::UObject::FindClassFast("VRPawn");
    if (!cls) cls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    const auto* dcls = SDK::UObject::FindClassFast("DiscEntity");
    const int32_t num = SDK::UObject::GObjects->Num();
    int shown = 0, dshown = 0;
    for (int32_t i = 0; i < num && (shown < 8 || dshown < 8); ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        if (cls && shown < 8 && o->IsA(cls)) { SafeProbeOnePawnWire(o); ++shown; continue; }
        if (dcls && dshown < 8 && o->IsA(dcls)) { SafeProbeOneBallOwner(o); ++dshown; }
    }
}
static void SafeProbeBallWire() { __try { ProbeBallWire(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SafeWireVRPawns()       { __try { WireVRPawnBallSimManagers(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SPECTATOR SMOOTHNESS (direction B: VR players see the spectator pawn snap). The spectator pose rides
// UA2SpectatorEntity.RepData (a bare FReplicatedTransformData — NO Timestamp/Ping/interp, unlike VR's
// FReplicatedFrequentData), so receivers can only snap to the latest replicated transform at whatever
// cadence it arrives. We can't add client-side interp (clients are Android, no injection), so the only
// server-side lever is REPLICATION DENSITY: pin the spectator pawn's net fields high + always-relevant
// so its transform reaches VR viewers as often as possible → raw motion looks smooth. (This is the
// OPPOSITE of VR pawns, where rate did nothing because interp was the lever; here there IS no interp.)
// ASpectatorCameraManagerPawn net fields (SDK struct): NetCullDistanceSquared@0x170, NetUpdateFrequency
// @0x178, MinNetUpdateFrequency@0x17C, NetPriority@0x180, bAlwaysRelevant@0x60 bit3.
static void TuneSpectatorPawns()
{
    static SDK::UClass* cls = nullptr;
    static const char* clsName = nullptr;
    if (!cls) { cls = SDK::UObject::FindClassFast("SpectatorCameraManagerPawn"); if (cls) clsName = "SpectatorCameraManagerPawn"; }
    if (!cls) { cls = SDK::UObject::FindClassFast("BP_SpectatorPawn_C");         if (cls) clsName = "BP_SpectatorPawn_C"; }
    if (!cls)
    {
        static bool warned = false;
        if (!warned) { warned = true; printf("[SPEC] NO spectator class resolved (SpectatorCameraManagerPawn / BP_SpectatorPawn_C both null) — tune is a NO-OP\n"); }
        return;
    }
    int tuned = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        *reinterpret_cast<uint8_t*>(p + 0x60) |= 0x08;    // bAlwaysRelevant — always relevant to every connection
        *reinterpret_cast<float*>(p + 0x170) = 1.0e12f;   // NetCullDistanceSquared — never distance-cull
        *reinterpret_cast<float*>(p + 0x178) = 100.0f;    // NetUpdateFrequency — target 100Hz
        *reinterpret_cast<float*>(p + 0x17C) = 100.0f;    // MinNetUpdateFrequency — pin (defeat adaptive down-throttle)
        *reinterpret_cast<float*>(p + 0x180) = 10.0f;     // NetPriority — win the actor budget
        ++tuned;
    }
    static int lastTuned = -1;
    if (tuned != lastTuned) { lastTuned = tuned; printf("[SPEC] tuned %d spectator pawn(s) (cls=%s)\n", tuned, clsName ? clsName : "?"); }
}
static void SafeTuneSpectatorPawns() { __try { TuneSpectatorPawns(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Watch each jakeball's physics-sync owningActor (physicsSync@ball+0x4F0, owningActor@+0xC8
// — the field Server_HitProp/SendPhysicsPropData gate on). Log only on change. When a player
// hits the ball, if ownership transfers we'll see `-> <VRPawn>`; if it never leaves null/server,
// the Server_HitProp ownership handshake is being rejected (the snap-back cause).
static std::unordered_map<void*, SDK::FVector> g_ballPos;   // last server-side world pos per jakeball
static int g_ownLogged = 0;
static void WatchBallOwnership()
{
    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    if (!actorCls || g_ownLogged >= 100000)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(actorCls))
            continue;
        std::string n = o->GetName();
        if (n.rfind("BP_JakeBall_C", 0) != 0)
            continue;
        auto* a = static_cast<SDK::AActor*>(o);
        if (!a->RootComponent)
            continue;
        SDK::FVector p = a->RootComponent->K2_GetComponentLocation();
        SDK::FVector& last = g_ballPos[o];
        const float dx = p.X - last.X, dy = p.Y - last.Y, dz = p.Z - last.Z;
        if (dx * dx + dy * dy + dz * dz > 10000.0f)   // server ball moved > 100 units (skip settle-bounce, catch hits)
        {
            void* physSync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x4F0);
            void* owner = physSync ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physSync) + 0xC8) : nullptr;
            printf("[HalcyonA2][OWN] %s SERVER-moved (%.0f,%.0f,%.0f) owner=%s\n",
                   n.c_str(), (double)p.X, (double)p.Y, (double)p.Z,
                   owner ? static_cast<SDK::UObject*>(owner)->GetName().c_str() : "null");
            ++g_ownLogged;
        }
        last = p;
    }
}
static void SafeWatchBallOwnership() { __try { WatchBallOwnership(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Dump arena-admission state: are there spawned ATicketManagers, and do our VRPawns have a
// GamemodeSlot / CurrentTicketManager / ticket? A pawn with ticketMgr=null / slot=null is
// never admitted -> lands in the -2 template sim -> no inputs, no team. Offsets (SDK map):
//   ATicketManager: SlotID@0x318 (Num@0x320), VerifiedTicketHolders num @0x390.
//   AVRPawn: CurrentTicketManager@0x1128, PlayerIndex@0x1B48, GamemodeSlot@0x1C40,
//            SlotID Num@0x1CB0, PawnCurrentTicket@0x1CB8.
static int      g_admDump = 0;
static ULONGLONG g_lastAdmDump = 0;
static void DumpAdmissionState()
{
    auto* tmCls   = SDK::UObject::FindClassFast("TicketManager");
    auto* pawnCls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    const int32_t num = SDK::UObject::GObjects->Num();
    int tmCount = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject())
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        if (tmCls && o->IsA(tmCls))
        {
            const int slotLen = *reinterpret_cast<int*>(p + 0x320);
            const int th      = *reinterpret_cast<int*>(p + 0x380);   // TicketHolders num
            const int vth     = *reinterpret_cast<int*>(p + 0x390);   // VerifiedTicketHolders num
            const float cur   = *reinterpret_cast<float*>(p + 0x36C); // CurrentTicket
            const float req   = *reinterpret_cast<float*>(p + 0x370); // RequiredTicket
            const int   tcNum = *reinterpret_cast<int*>(p + 0x2F0);    // TeamColors.Num (@0x2E8+8)
            auto* tcData = *reinterpret_cast<unsigned char**>(p + 0x2E8);  // TeamColors.Data
            int c0r = -1, c0g = -1, c0b = -1;   // first entry's Primary FColor (mem = B,G,R,A)
            if (tcNum > 0 && tcData) { c0b = tcData[0]; c0g = tcData[1]; c0r = tcData[2]; }
            if (th || vth || tcNum)
                printf("[HalcyonA2][ADM] TicketManager %s slotLen=%d holders=%d verified=%d cur=%.1f req=%.1f teamColors=%d primary0=(%d,%d,%d)\n",
                       o->GetName().c_str(), slotLen, th, vth, cur, req, tcNum, c0r, c0g, c0b);
            ++tmCount;
        }
        else if (pawnCls && o->IsA(pawnCls))
        {
            void* ctm  = *reinterpret_cast<void**>(p + HX::VRPawn_CurrentTicketMgr);
            void* slot = *reinterpret_cast<void**>(p + HX::VRPawn_GamemodeSlot);
            const int sidLen = *reinterpret_cast<int*>(p + HX::VRPawn_SlotIdNum);
            const int ticket = *reinterpret_cast<int*>(p + HX::VRPawn_PawnCurrentTicket);
            const int pidx   = *reinterpret_cast<unsigned char*>(p + HX::VRPawn_PlayerIndex);
            printf("[HalcyonA2][ADM] VRPawn %s ticketMgr=%p slot=%p slotIDlen=%d ticket=%d playerIdx=%d\n",
                   o->GetName().c_str(), ctm, slot, sidLen, ticket, pidx);
        }
    }
    printf("[HalcyonA2][ADM] ticketManagers=%d\n", tmCount);
}
static void SafeDumpAdmissionState() { __try { DumpAdmissionState(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Dump each ABallSimManager's SimulationsOutline (@0x350, num@0x358): one
// FBallSimulationOutline{SimIndex@0, TicketMan@0x8, PlayerIndexes@0x10 (TArray<uint8>),
// Balls@0x20} per TicketManager (stride 0x30). This is the seat map the rollback pump
// reads: if an admitted pawn's PlayerIndex shows under a real SimIndex here, admission
// -> sim seat works and the -2 problem is downstream; if it's absent, the seat/rebuild
// isn't picking up VerifiedTicketHolders on our manager.
static void DumpSimSeats()
{
    auto* mgrCls = SDK::UObject::FindClassFast("BallSimManager");
    if (!mgrCls)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(mgrCls))
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const uintptr_t olData = *reinterpret_cast<uintptr_t*>(p + 0x350);
        const int       olNum  = *reinterpret_cast<int*>(p + 0x358);
        printf("[HalcyonA2][SEAT] mgr=%p outlines=%d\n", reinterpret_cast<void*>(p), olNum);
        if (!olData || olNum <= 0 || olNum > 64)
            continue;
        for (int k = 0; k < olNum; ++k)
        {
            const uintptr_t ol = olData + static_cast<uintptr_t>(k) * 0x30;
            const int    simIdx = *reinterpret_cast<int*>(ol + 0x0);
            void*        tm     = *reinterpret_cast<void**>(ol + 0x8);
            uint8_t*     piD    = *reinterpret_cast<uint8_t**>(ol + 0x10);
            const int    piN    = *reinterpret_cast<int*>(ol + 0x18);
            void*        ball0  = *reinterpret_cast<void**>(ol + 0x20) ?
                                  *reinterpret_cast<void**>(*reinterpret_cast<uintptr_t*>(ol + 0x20)) : nullptr;
            const int    ballN  = *reinterpret_cast<int*>(ol + 0x28);   // Balls TArray num
            char buf[160]; int bl = 0; buf[0] = 0;
            if (piD && piN > 0 && piN <= 32)
                for (int j = 0; j < piN; ++j)
                    bl += sprintf_s(buf + bl, sizeof(buf) - bl, "%d ", piD[j]);
            if (simIdx != -2 || piN > 0 || ballN > 0)
                printf("[HalcyonA2][SEAT]   sim=%d tm=%p players=[%s] n=%d balls=%d ball0=%p\n",
                       simIdx, tm, buf, piN, ballN, ball0);
        }
    }
}
static void SafeDumpSimSeats() { __try { DumpSimSeats(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// AVRPawn::Server_SendPhysicsPropData_Implementation (sub_5496650) applies the client's
// streamed ball state ONLY IF physicsSync->owningActor(@0xC8) == the sending pawn. On our
// server ownership never transfers (stays null), so every hit-stream is dropped -> the
// authoritative ball never moves from a hit -> snap-back. Force it: stamp owningActor to
// the sender right before the apply so the server accepts the stream (last-writer-wins).
static constexpr uintptr_t SendPhysImpl_RVA = HX::SendPhysImpl_RVA;
using SendPhys_t = void(__fastcall*)(void*, void*, void*, void*);
static SendPhys_t SendPhys_Orig = nullptr;
// Pending hit velocity to inject into the sim's ball state (set by SendPhys_Hook,
// consumed by SimIntegrate_Hook).
static SDK::FVector g_pendingVel{};
static SDK::FVector g_pendingPos{};              // client's streamed ball position (data+0x18)
// Latest client-streamed position per ball. For golf balls the actor RootComponent is FROZEN
// server-side (no sim moves it); the live position only exists in this Server_SendPhysicsPropData
// stream (data+0x18). GolfSinkDetect reads THIS for stroke/sink detection. Game-thread only (RPC
// handler + PE ticker are the same thread) so no lock needed.
struct BallStreamPos { SDK::FVector pos; unsigned long long seen; };
static std::unordered_map<void*, BallStreamPos> g_ballStreamPos;
static void*        g_pendingRoot   = nullptr;   // ball RootComponent
static void*        g_pendingPrim   = nullptr;   // physicsSync primitiveComp (@physicsSync+0xB0)
static bool         g_pendingActive = false;
static int          g_injLog        = 0;
static void __fastcall SendPhys_Hook(void* pawn, void* ball, void* data, void* a4)
{
    __try {
        if (ball)
        {
            void* physSync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ball) + 0x4F0);
            if (physSync)
                *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physSync) + 0xC8) = pawn;

            // OPTION 1: relay the client's streamed velocity into the sim buffer so the step
            // carries the hit instead of stomping it. FReplicatedPhysicsObjectData.Velocity
            // is @0x48 (FVector_NetQuantize10 = 3 doubles). Server_SetVelocity writes it into
            // the sim's FBallSimBallState.QueuedSetVelocity — the store the sim step reads.
            // Stash the client's post-hit velocity + the ball's candidate sim components,
            // for the sub_5434CE0 hook to write into FBallSimBallState.QueuedSetVelocity.
            if (data)
            {
                // Record the live streamed position for EVERY ball (data+0x18) — this is the moving
                // golf-ball position the frozen RootComponent doesn't have.
                g_ballStreamPos[ball] = { *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x18), GetTickCount64() };

                SDK::FVector vel = *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x48);
                const double sp2 = vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z;
                if (sp2 > 100.0)   // only inject a meaningful velocity (skip ~rest)
                {
                    g_pendingVel  = vel;
                    g_pendingPos  = *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x18);
                    g_pendingRoot = static_cast<SDK::AActor*>(ball)->RootComponent;
                    void* pssync  = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ball) + 0x4F0);
                    g_pendingPrim = pssync ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pssync) + 0xB0) : nullptr;
                    g_pendingActive = true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    SendPhys_Orig(pawn, ball, data, a4);
}

// sub_5434CE0(manager, simIndex, FBallSimulation, dt, a5, a6) — the per-sim integrator.
// a3 = FBallSimulation: Balls@0x48 (TArray<UPrimitiveComponent*>), States@0x58
// (TArray<FBallSimState>, 0x28 stride; BallStates@0x18, FBallSimBallState 0xD0 stride,
// QueuedSetVelocity@0x88). On a pending hit, find the ball's index in Balls and write the
// client's velocity into the latest state's ball entry, so the integrator applies it.
static constexpr uintptr_t SimIntegrate_RVA = HX::SimIntegrate_RVA;
using SimIntegrate_t = __int64(__fastcall*)(void*, unsigned int, void*, double, char, char);
static SimIntegrate_t SimIntegrate_Orig = nullptr;
static __int64 __fastcall SimIntegrate_Hook(void* mgr, unsigned int simIndex, void* fsim, double dt, char a5, char a6)
{
    // Run the integrator FIRST (it appends the freshly-computed state), then overwrite the
    // just-appended state's ball entry with the client's hit pos+vel — so the authoritative
    // result that gets sent to clients IS the hit, not spawn.
    const __int64 r = SimIntegrate_Orig(mgr, simIndex, fsim, dt, a5, a6);
    __try {
        if (g_pendingActive && fsim && (g_pendingRoot || g_pendingPrim))
        {
            const uintptr_t s = reinterpret_cast<uintptr_t>(fsim);
            void** balls   = *reinterpret_cast<void***>(s + 0x48);
            const int nBalls = *reinterpret_cast<int*>(s + 0x50);
            void* statesD  = *reinterpret_cast<void**>(s + 0x58);
            const int nStates = *reinterpret_cast<int*>(s + 0x60);
            if (balls && nBalls > 0 && statesD && nStates > 0)
            {
                for (int i = 0; i < nBalls; ++i)
                {
                    if (balls[i] != g_pendingRoot && balls[i] != g_pendingPrim)
                        continue;
                    // latest (just-appended) state — the one sent to clients
                    const uintptr_t st = reinterpret_cast<uintptr_t>(statesD) + (size_t)(nStates - 1) * 0x28;
                    void* bsD  = *reinterpret_cast<void**>(st + 0x18);
                    const int nBs = *reinterpret_cast<int*>(st + 0x20);
                    if (bsD && i < nBs)
                    {
                        const uintptr_t bs = reinterpret_cast<uintptr_t>(bsD) + (size_t)i * 0xD0;
                        *reinterpret_cast<double*>(bs + 0x00) = g_pendingPos.X;   // position
                        *reinterpret_cast<double*>(bs + 0x08) = g_pendingPos.Y;
                        *reinterpret_cast<double*>(bs + 0x10) = g_pendingPos.Z;
                        *reinterpret_cast<double*>(bs + 0x20) = g_pendingVel.X;   // Velocity
                        *reinterpret_cast<double*>(bs + 0x28) = g_pendingVel.Y;
                        *reinterpret_cast<double*>(bs + 0x30) = g_pendingVel.Z;
                    }
                    (void)simIndex; (void)nBalls; (void)nStates;   // [INJ] logging silenced
                    g_pendingActive = false;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// sub_540B010 = the input-ingest called by Server_SubmitInputs. Returns 1 if the input
// (disc/hand poses) was ADDED to the sim's player buffer, 0 if SKIPPED (frame delta >100
// vs the sim frame, or player lookup miss). If our remote hits are SKIPPED, the disc never
// enters the sim -> no disc->ball collision -> frozen ball. Log ADDED/SKIPPED + frames.
static constexpr uintptr_t IngestInput_RVA = HX::IngestInput_RVA;
using IngestInput_t = char(__fastcall*)(void*, int, int, void*, int, void*);
static IngestInput_t IngestInput_Orig = nullptr;
static int g_ingestLog = 0;
// Replicate sub_540B010's sim-entry hash lookup to reach a sim's frame counter (entry+0xC).
// mgr+0x300 = entry array (0xF8 stride, next-link @+0xF0); hash @mgr+0x340 (or mgr+0x338 if
// null), cap @mgr+0x348; empty when mgr+0x308==mgr+0x334.
static int* GetSimFramePtr(uintptr_t mgr, int simIdx)
{
    if (*reinterpret_cast<int*>(mgr + 0x308) == *reinterpret_cast<int*>(mgr + 0x334))
        return nullptr;
    uintptr_t hashBase = *reinterpret_cast<uintptr_t*>(mgr + 0x340);
    if (!hashBase) hashBase = mgr + 0x338;
    const int cap = *reinterpret_cast<int*>(mgr + 0x348);
    if (cap <= 0) return nullptr;
    int idx = *reinterpret_cast<int*>(hashBase + 4LL * (simIdx & (cap - 1)));
    if (idx == -1) return nullptr;
    const uintptr_t simArray = *reinterpret_cast<uintptr_t*>(mgr + 0x300);
    if (!simArray) return nullptr;
    for (int guard = 0; guard < 4096; ++guard)
    {
        const uintptr_t entry = simArray + 248LL * idx;
        if (*reinterpret_cast<int*>(entry) == simIdx)
            return reinterpret_cast<int*>(entry + 0xC);
        idx = *reinterpret_cast<int*>(entry + 0xF0);
        if (idx == -1) return nullptr;
    }
    return nullptr;
}

// a5 of sub_540B010 = "apply/overwrite even if this frame is already buffered" — i.e. this is an
// authoritative CORRECTION for a predicted frame. Headless it comes in a5=0, so late corrective
// inputs are dropped (skipRecent) instead of overwriting the prediction -> missedCaught stays 0 ->
// rubber-band. Force it to 1 so corrections land + trigger the rollback catch. Safe: for an
// already-confirmed frame it overwrites with the same value (no re-sim); only predicted frames
// (byte@+1==0) actually correct + set the caller's re-sim signal. Flag so we can A/B it.
static bool g_forceInputOverwrite = true;

static char __fastcall IngestInput_Hook(void* mgr, int simIdx, int playerIdx, void* inputData, int a5, void* outByte)
{
    int inFrame = -1, simFrame = -1;
    bool resynced = false;
    __try {
        if (inputData) inFrame = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(inputData) + 4);
        int* fp = GetSimFramePtr(reinterpret_cast<uintptr_t>(mgr), simIdx);
        if (fp)
        {
            simFrame = *fp;
            // Track the sim a player occupies + the newest confirmed input frame, for the
            // step pump's input-delay gate. A big backward jump = a fresh session -> reset.
            g_activeSimFramePtr = fp;
            g_activeSimIdx = simIdx;
            if (inFrame > g_newestInFrame || inFrame < g_newestInFrame - 200)
                g_newestInFrame = inFrame;
            // Track per-player newest frame (session-reset on a big backward jump) so the step pump
            // can hold behind the slowest player.
            if (playerIdx >= 0 && playerIdx < 256 && inFrame >= 0)
            {
                PlayerFrame& pf = g_playerFrames[playerIdx];
                if (inFrame > pf.newest || inFrame < pf.newest - 200) pf.newest = inFrame;
                pf.simIdx = simIdx;
                pf.seen   = GetTickCount64();
            }
            // The gate (sub_540B010) rejects inputs where abs(input.frame - sim.frame) > 100.
            // Our sim free-ran far ahead of the client's fresh frame counter, so every input
            // is dropped as stale. When out of window, yank the sim's frame to the client's so
            // the input lands at the live frame and the next step applies it. Fires only when
            // drifted; normal ingestion holds it in sync afterward (both advance ~90Hz).
            if (inFrame >= 0)
            {
                int d = inFrame - simFrame; if (d < 0) d = -d;
                if (d > 100) { *fp = inFrame; simFrame = inFrame; resynced = true; }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int applyFlag = g_forceInputOverwrite ? 1 : a5;   // force overwrite so late corrections catch
    char r = IngestInput_Orig(mgr, simIdx, playerIdx, inputData, applyFlag, outByte);
    // Per-second census answering "why are inputs missed — is the server SKIPPING ones that arrived?"
    //   added       = ingest accepted it (r != 0)
    //   skipRecent  = REJECTED an input for a RECENT frame (< 60 behind newest) — THE smoking gun:
    //                 if high, the game/gate is throwing away recent/rollback inputs -> misprediction
    //                 never corrected -> the jitter/snap. If ~0, missed = pure network loss (frames
    //                 that never arrived at all).
    //   skipOld     = REJECTED an ancient redundant re-send (>= 60 behind) — harmless.
    //   resync      = our abs>100 yank fired (a big frame jump = a snap source) — should be ~0 mid-play.
    static int cAdd = 0, cSkipRecent = 0, cSkipOld = 0, cResync = 0;
    static ULONGLONG lastInLog = 0;
    if (r) ++cAdd;
    else if (inFrame >= 0 && (g_newestInFrame - inFrame) < 60) ++cSkipRecent;
    else ++cSkipOld;
    if (resynced) ++cResync;
    const ULONGLONG now = GetTickCount64();
    if (now - lastInLog > 1000)
    {
        lastInLog = now;
        HxLog("[HalcyonA2][IN] sim=%d added=%d skipRecent=%d skipOld=%d resync=%d simFrame=%d newest=%d\n",
              simIdx, cAdd, cSkipRecent, cSkipOld, cResync, simFrame, g_newestInFrame);
        cAdd = cSkipRecent = cSkipOld = cResync = 0;
    }
    return r;
}

// Read the ACTUAL missedInputs/missedCaught the server computes and ships to the client each step
// (Client_SendServerSimResults = sub_5309C40; args: pawn, state, &players, &inputs, missedInputs,
// missedCaught). The client's HUD "MI/MIB" = these. Logging them tells us whether MI is server-side
// (a real miss count we can attack) or client-side prediction/clock.
// sub_5309C40 signature (confirmed from IDA): the last two args are CHAR (single bytes), not int —
// missedInputs/missedCaught are bytes (0-255), which is why the client HUD tops out ~44. Reading them
// as int grabbed 3 garbage stack bytes -> the 2-billion/negative junk we saw. Read as char, treat as
// unsigned count.
static constexpr uintptr_t SendResults_RVA = HX::SendResults_RVA;
using SendResults_t = void(__fastcall*)(void*, void*, void*, void*, char, char);
static SendResults_t SendResults_Orig = nullptr;
static ULONGLONG g_lastSendLog = 0;
static int g_sendMax = 0;   // peak missedInputs seen since last log
static void __fastcall SendResults_Hook(void* pawn, void* state, void* players, void* inputs,
                                        char missedInputsRaw, char missedCaughtRaw)
{
    const int missedInputs  = static_cast<unsigned char>(missedInputsRaw);
    const int missedCaught  = static_cast<unsigned char>(missedCaughtRaw);
    if (missedInputs > g_sendMax) g_sendMax = missedInputs;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastSendLog > 1000)
    {
        g_lastSendLog = now;
        HxLog("[HalcyonA2][SEND] missedInputs=%d (peak=%d) missedCaught=%d simFrame=%d newest=%d\n",
              missedInputs, g_sendMax, missedCaught,
              g_activeSimFramePtr ? *g_activeSimFramePtr : -1, g_newestInFrame);
        g_sendMax = 0;
    }
    SendResults_Orig(pawn, state, players, inputs, missedInputsRaw, missedCaughtRaw);
}

// DIAGNOSTIC: who advances the sim frame? Our pump logs stepped=0, yet simFrame climbs ~90/s and
// the server predicts ~30 frames (missedInputs). Hook the step (sub_543F2D0) and measure how many
// times/sec it's called + how much the active sim's frame advances PER call. If frameAdv/call≈1 the
// STEP drives the frame (so enforcing the input-delay hold by throttling the step would shrink the
// prediction window/MI); if frameAdv≈0 the frame is advanced by INGEST instead (throttling the step
// would just stutter physics, not lower MI).
// SNAP DIAGNOSIS rate sampler — the 1Hz probe can't see the true pose update rate. StepSim runs
// ~800/s, so sample the cached active-remote entity here and count how often localData (@0xF0,
// incoming RPC) and VRPlayerRepData (@0x308, replicated copy) actually change per second. localHz =
// how fast the client's pose reaches the server; repHz = how fast the server updates the copy it
// replicates. If either is ~10-20 that sparse rate IS the snap; if both ~72-90 the send-to-client
// cadence / client interp is the culprit instead. Root.position.X (@+0x10) is the change witness.
static double g_lastLocalX = 1e300, g_lastRepX = 1e300;
static int g_localChanges = 0, g_repChanges = 0;
static ULONGLONG g_lastFreqRateLog = 0;
static void SampleFreqRate()
{
    // PING-STAMP FIX — write each player's real server-measured ping into FrequentData.Ping on both
    // freq-data copies, at fast-path rate so the relay/replication can't leave it 0. This is the whole
    // fix for the remote-player snapping: ping=0 collapses client interpolation.
    if (g_pingStampEnabled)
    {
        for (int i = 0; i < g_pingTargetCount; ++i)
        {
            const uintptr_t e = reinterpret_cast<uintptr_t>(g_pingTargets[i].entity);
            const float pm = g_pingTargets[i].pingMs;
            *reinterpret_cast<float*>(e + HX::Ent_FreqLocal + 0x8) = pm;   // localData.FrequentData.Ping
            *reinterpret_cast<float*>(e + HX::Ent_FreqRep + 0x8) = pm;   // VRPlayerRepData.FrequentData.Ping (replicated)
        }
    }

    // TEAM-COLOR stamp — write the palette color + team into the entity's replicated VRPlayerRepData
    // (and localData) directly, since the Mass fragment's color never syncs to the replicated copy.
    if (g_colorStampEnabled)
    {
        for (int i = 0; i < g_colorTargetCount; ++i)
        {
            const uintptr_t e = reinterpret_cast<uintptr_t>(g_colorTargets[i].entity);
            const unsigned char* c = g_colorTargets[i].color;
            const signed char ti = g_colorTargets[i].teamIndex;
            memcpy(reinterpret_cast<void*>(e + HX::Ent_ColorRep), c, HX::TeamColor_Size);  // VRPlayerRepData.CurrentTeamColor (replicated)
            *reinterpret_cast<signed char*>(e + HX::Ent_TeamIdxRep) = ti;      // VRPlayerRepData.TeamIndex
            memcpy(reinterpret_cast<void*>(e + HX::Ent_ColorLocal), c, HX::TeamColor_Size);  // localData.CurrentTeamColor
            *reinterpret_cast<signed char*>(e + HX::Ent_TeamIdxLocal) = ti;      // localData.TeamIndex
        }
    }

    if (!g_probeEntity) return;
    const uintptr_t e = reinterpret_cast<uintptr_t>(g_probeEntity);
    const double lx = *reinterpret_cast<double*>(e + HX::Ent_FreqLocal + 0x10);
    const double rx = *reinterpret_cast<double*>(e + HX::Ent_FreqRep + 0x10);
    if (lx != g_lastLocalX) { g_lastLocalX = lx; ++g_localChanges; }
    if (rx != g_lastRepX)   { g_lastRepX = rx;   ++g_repChanges; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastFreqRateLog > 1000)
    {
        g_lastFreqRateLog = now;
        if (g_freqDebug)
            printf("[HalcyonA2][FREQRATE] pidx=%d localHz=%d repHz=%d\n", g_probePidx, g_localChanges, g_repChanges);
        g_localChanges = 0; g_repChanges = 0;
    }
}
static void SafeSampleFreqRate() { __try { SampleFreqRate(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static constexpr uintptr_t StepSim_RVA = HX::StepSim_RVA;
using StepSim_t = void(__fastcall*)(void*, float);
static StepSim_t StepSim_Orig = nullptr;
static ULONGLONG g_lastStepLog = 0;
static int g_stepCalls = 0, g_stepFrameAdv = 0;
static void __fastcall StepSim_Hook(void* mgr, float dt)
{
    SafeSampleFreqRate();
    const int before = g_activeSimFramePtr ? *g_activeSimFramePtr : -1;
    StepSim_Orig(mgr, dt);
    const int after = g_activeSimFramePtr ? *g_activeSimFramePtr : -1;
    ++g_stepCalls;
    if (before >= 0 && after >= before) g_stepFrameAdv += (after - before);
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastStepLog > 1000)
    {
        g_lastStepLog = now;
        HxLog("[HalcyonA2][STEP] calls/s=%d frameAdvViaStep/s=%d (ours-stepped=%d) lastDt=%.4f\n",
              g_stepCalls, g_stepFrameAdv, 0, dt);
        g_stepCalls = 0; g_stepFrameAdv = 0;
    }
}

// P2 — push the team color to the joining client. Admission (SetColorAndTeamIndex on the server
// pawn) doesn't recolor the owning client's view, so send the AVRPawn NetClient RPC
// Client_UpdateTeamColors(FTeamColor, int8) explicitly: UE marshals it to the owning connection
// (the VR player), whose client runs SetColorAndTeamIndex locally. Color comes from the TM's now-
// populated TeamColors[teamIndex] (P1). No SEH here (GetFunction uses std::string); the ProcessEvent
// is isolated in SafeProcessEvent (defined later — forward-declared).
static bool SafeProcessEvent(SDK::UObject* o, SDK::UFunction* fn, void* parms);
static void PushTeamColorToClient(void* pawnV, uintptr_t tm, int teamIndex)
{
    auto* pawn = reinterpret_cast<SDK::UObject*>(pawnV);
    if (!pawn || !pawn->Class || teamIndex < 0) { printf("[HalcyonA2][TCOL] Client bail: pawn/class/team (team=%d)\n", teamIndex); return; }
    static SDK::UFunction* fn = nullptr;
    if (!fn) fn = pawn->Class->GetFunction("VRPawn", "Client_UpdateTeamColors");
    if (!fn) { printf("[HalcyonA2][TCOL] Client bail: Client_UpdateTeamColors fn not found\n"); return; }
    auto* tcData    = *reinterpret_cast<unsigned char**>(tm + 0x2E8);   // TeamColors.Data
    const int tcNum = *reinterpret_cast<int*>(tm + 0x2F0);              // TeamColors.Num
    if (!tcData || teamIndex >= tcNum) { printf("[HalcyonA2][TCOL] Client bail: TeamColors empty/oob (data=%p num=%d team=%d)\n", tcData, tcNum, teamIndex); return; }
    struct { unsigned char TeamColor[HX::TeamColor_Size]; signed char TeamIndex; unsigned char pad[7]; } parms{};
    memcpy(parms.TeamColor, tcData + teamIndex * HX::TeamColor_Size, HX::TeamColor_Size);           // FTeamColor is 0x14 bytes
    parms.TeamIndex = static_cast<signed char>(teamIndex);
    const bool ok = SafeProcessEvent(pawn, fn, &parms);
    printf("[HalcyonA2][TCOL] Client_UpdateTeamColors SENT pawn=%s team=%d color=(%u,%u,%u,%u) pe_ok=%d\n",
           pawn->GetName().c_str(), teamIndex, parms.TeamColor[0], parms.TeamColor[1], parms.TeamColor[2], parms.TeamColor[3], ok);
}

// P0 — write the REPLICATED per-player team+color so it sticks and every client sees it. The
// client-only Client_UpdateTeamColors above doesn't write UA2PlayerEntity.VRPlayerRepData, so the
// default gray OnReps back over the local paint. Server_SetCurrentColor(FTeamColor,int8) is NetServer;
// on our authority ProcessEvent runs the impl locally and writes replicated TeamIndex@0x418 +
// CurrentTeamColor@0x500 on the entity (AVRPawn.Entity@0x840) -> replicates to all.
static void PushReplicatedTeamColor(void* pawnV, uintptr_t tm, int teamIndex)
{
    auto* pawn = reinterpret_cast<SDK::UObject*>(pawnV);
    if (!pawn || teamIndex < 0) { printf("[HalcyonA2][TCOL] Rep bail: pawn/team (team=%d)\n", teamIndex); return; }
    void* entityV = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + HX::VRPawn_Entity);  // AVRPawn.Entity
    if (!entityV) { printf("[HalcyonA2][TCOL] Rep bail: Entity@0x840 null\n"); return; }
    auto* entity = reinterpret_cast<SDK::UObject*>(entityV);
    if (!entity->Class) { printf("[HalcyonA2][TCOL] Rep bail: entity->Class null\n"); return; }
    static SDK::UFunction* fn = nullptr;
    if (!fn) fn = entity->Class->GetFunction("A2PlayerEntity", "Server_SetCurrentColor");
    if (!fn) { printf("[HalcyonA2][TCOL] Rep bail: Server_SetCurrentColor fn not found\n"); return; }
    auto* tcData    = *reinterpret_cast<unsigned char**>(tm + 0x2E8);   // TeamColors.Data
    const int tcNum = *reinterpret_cast<int*>(tm + 0x2F0);              // TeamColors.Num
    if (!tcData || teamIndex >= tcNum) { printf("[HalcyonA2][TCOL] Rep bail: TeamColors empty/oob (data=%p num=%d team=%d)\n", tcData, tcNum, teamIndex); return; }
    struct { unsigned char TeamColor[HX::TeamColor_Size]; signed char NewTeamIndex; unsigned char pad[3]; } parms{};
    memcpy(parms.TeamColor, tcData + teamIndex * HX::TeamColor_Size, HX::TeamColor_Size);
    parms.NewTeamIndex = static_cast<signed char>(teamIndex);
    // The Server_SetCurrentColor RPC writes only the Mass fragment, whose color never syncs to the
    // replicated copy -> register this player so the fast path stamps the color into the entity's
    // replicated VRPlayerRepData directly (the actual fix; the RPC below is now just belt-and-braces).
    RegisterColorTarget(entityV, parms.TeamColor, teamIndex);
    // The impl gates on sub_53AB0C0(entity) = (entity+0xC8 != 0 && entity+0xCC != 0) = the Mass entity
    // handle {Index@0xC8, Serial@0xCC}. If either is 0 the color write silently no-ops. Log it so we
    // see whether the gate passes on our headless server.
    const uintptr_t e = reinterpret_cast<uintptr_t>(entityV);
    const uint32_t massIdx = *reinterpret_cast<uint32_t*>(e + 0xC8);
    const uint32_t massSer = *reinterpret_cast<uint32_t*>(e + 0xCC);
    const bool ok = SafeProcessEvent(entity, fn, &parms);
    printf("[HalcyonA2][TCOL] Server_SetCurrentColor SENT entity=%s team=%d color=(%u,%u,%u,%u) pe_ok=%d | MassHandle idx=%u ser=%u gate=%s\n",
           entity->GetName().c_str(), teamIndex, parms.TeamColor[0], parms.TeamColor[1], parms.TeamColor[2], parms.TeamColor[3], ok,
           massIdx, massSer, (massIdx != 0 && massSer != 0) ? "PASS" : "FAIL(no write)");
}

// P1 — set the replicated player-state team so the arena roster / per-player display resolves the
// team (AAxPlayerState.TeamIndex@0x380, Net/RepNotify; default -1 renders "None"). Reached via
// APawn.PlayerState@0x2B8.
static void SetPlayerStateTeam(void* pawnV, int teamIndex)
{
    auto* pawn = reinterpret_cast<SDK::UObject*>(pawnV);
    if (!pawn || teamIndex < 0) { printf("[HalcyonA2][TCOL] PSTeam bail: pawn/team (team=%d)\n", teamIndex); return; }
    void* psV = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + 0x2B8);  // APawn.PlayerState
    if (!psV) { printf("[HalcyonA2][TCOL] PSTeam bail: PlayerState@0x2B8 null\n"); return; }
    auto* ps = reinterpret_cast<SDK::UObject*>(psV);
    if (!ps->Class) { printf("[HalcyonA2][TCOL] PSTeam bail: ps->Class null\n"); return; }
    static SDK::UFunction* fn = nullptr;
    if (!fn) fn = ps->Class->GetFunction("AxPlayerState", "SetTeamIndex");
    if (!fn) { printf("[HalcyonA2][TCOL] PSTeam bail: SetTeamIndex fn not found\n"); return; }
    struct { int32_t NewTeamIndex; } parms{ teamIndex };
    const bool ok = SafeProcessEvent(ps, fn, &parms);
    printf("[HalcyonA2][TCOL] SetTeamIndex SENT ps=%s team=%d pe_ok=%d\n", ps->GetName().c_str(), teamIndex, ok);
}

// AVRPawn::Server_NotifyPlayerLeftArena_Implementation (sub @ 0x54962B0, a1=pawn). Fires when a
// player leaves the arena. Our fast-path color stamp keeps re-writing the arena team color every
// tick, so leaving never visually resets — we must drop the entity from g_colorTargets here (stop
// stamping) and reset its color/team to the hub default so it actually clears.
static constexpr uintptr_t NotifyLeftArena_RVA = HX::NotifyLeftArena_RVA;
using NotifyLeftArena_t = __int64(__fastcall*)(__int64, __int64);
static NotifyLeftArena_t NotifyLeftArena_Orig = nullptr;
static __int64 __fastcall NotifyLeftArena_Hook(__int64 pawn, __int64 a2)
{
    const __int64 r = NotifyLeftArena_Orig(pawn, a2);
    __try {
        void* entity = *reinterpret_cast<void**>(pawn + 0x840);
        if (entity)
        {
            for (int i = 0; i < g_colorTargetCount; ++i)          // stop stamping this entity
                if (g_colorTargets[i].entity == entity)
                { g_colorTargets[i] = g_colorTargets[--g_colorTargetCount]; break; }
            const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
            unsigned char def[0x14]; memset(def, 0xFF, 0x14);     // hub default = white / no team
            memcpy(reinterpret_cast<void*>(e + HX::Ent_ColorRep), def, HX::TeamColor_Size);  // VRPlayerRepData.CurrentTeamColor
            *reinterpret_cast<signed char*>(e + HX::Ent_TeamIdxRep) = -1;        // VRPlayerRepData.TeamIndex
            memcpy(reinterpret_cast<void*>(e + HX::Ent_ColorLocal), def, HX::TeamColor_Size);  // localData.CurrentTeamColor
            *reinterpret_cast<signed char*>(e + HX::Ent_TeamIdxLocal) = -1;        // localData.TeamIndex
        }
        *reinterpret_cast<signed char*>(pawn + 0x1CA0) = -1;      // pawn TeamIndex
        *reinterpret_cast<void**>(pawn + 0x1128) = nullptr;       // CurrentTicketManager backptr
        printf("[HalcyonA2][TCOL] player left arena -> color/team reset (pawn=%p)\n", reinterpret_cast<void*>(pawn));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// GOLF-CUP server-side detection probe. Does the server natively detect the ball sinking? If so we
// can synthesize the BallInCup event to the (running) conductor ourselves instead of relying on the
// client send that isn't arriving. AGolfCup::BallInCup_Impl@0x53986A0 (cup, Score) fires OnBallInCup;
// OnGoalBeginOverlap_Impl@0x53AD660 (cup, overlappedComp, ball=OtherActor) is the trigger overlap.
static void LogGolfBallInCup(__int64 cup, unsigned int score)
{
    auto* c = reinterpret_cast<SDK::UObject*>(cup);
    printf("[HalcyonA2][GOLFCUP] BallInCup cup=%s Score=%u\n", (c && c->Class) ? c->GetName().c_str() : "?", score);
}
static void LogGolfOverlap(__int64 cup, __int64 ball)
{
    auto* c = reinterpret_cast<SDK::UObject*>(cup);
    auto* b = reinterpret_cast<SDK::UObject*>(ball);
    printf("[HalcyonA2][GOLFCUP] Overlap cup=%s ball=%s\n",
           (c && c->Class) ? c->GetName().c_str() : "?", (b && b->Class) ? b->GetName().c_str() : "(non-ball)");
}
static constexpr uintptr_t GolfBallInCup_RVA = HX::GolfBallInCup_RVA;
using GolfBallInCup_t = __int64(__fastcall*)(__int64, unsigned int);
static GolfBallInCup_t GolfBallInCup_Orig = nullptr;
static __int64 __fastcall GolfBallInCup_Hook(__int64 cup, unsigned int score)
{
    __try { LogGolfBallInCup(cup, score); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return GolfBallInCup_Orig(cup, score);
}
static constexpr uintptr_t GolfOverlap_RVA = HX::GolfOverlap_RVA;
using GolfOverlap_t = void(__fastcall*)(__int64, __int64, __int64);
static GolfOverlap_t GolfOverlap_Orig = nullptr;
static void __fastcall GolfOverlap_Hook(__int64 cup, __int64 comp, __int64 ball)
{
    __try { LogGolfOverlap(cup, ball); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    GolfOverlap_Orig(cup, comp, ball);
}

// CRASH-STOPPER: FName->string resolver (sub_114E210 @ 0x114E210). On arena-leave the ball-sim
// teardown resolves an INVALID FName (id low-word 0xFFFF, null pool chunk) -> reads 0x1FFFE -> AV.
// SEH-wrap it: an invalid FName returns an empty FString instead of crashing. Zero cost on the normal
// path (x64 __try is table-based); only fires on the bad id. a2 = out FString {Data@0, Num/Max@8}.
static constexpr uintptr_t FNameResolve_RVA = HX::FNameResolve_RVA;
using FNameResolve_t = __int64*(__fastcall*)(unsigned int*, __int64*);
static FNameResolve_t FNameResolve_Orig = nullptr;
static __int64* __fastcall FNameResolve_Hook(unsigned int* a1, __int64* a2)
{
    __try { return FNameResolve_Orig(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { if (a2) { a2[0] = 0; a2[1] = 0; } return a2; }
}

// CRASH-STOPPER: Slate/text-run layout leaf (sub_1EC13E0 @ 0x1EC13E0). On arena-leave a UI text
// block (scoreboard/leaderboard) lays out a run whose FName key is INVALID (id low-word 0xFFFF,
// null pool chunk). The FName->entry lookup INLINED here (sub_1142D30) returns 0 + 2*0xFFFF =
// 0x1FFFE, and the next `movzx r10,byte[rax]` (0x1EC148C) AVs reading 0x1FFFE. The caller
// sub_1EC1BE0 is the recursive run-tree walker (the two 0x1EC1C5C frames). This inlined resolver
// is NOT sub_114E210, which is why the existing FName guard never caught it. Headless (-nullrhi)
// renders nothing, so a failed glyph layout is cosmetically irrelevant: swallow the AV and return
// 0 -> the walker's per-run loop just advances to the next run. Zero cost on the happy path
// (table-based x64 SEH). Narrowest possible wrap = the exact crash-site function only.
static constexpr uintptr_t TextLayoutLeaf_RVA = HX::TextLayoutLeaf_RVA;
using TextLayoutLeaf_t = __int64(__fastcall*)(__int64, __int64);
static TextLayoutLeaf_t TextLayoutLeaf_Orig = nullptr;
static __int64 __fastcall TextLayoutLeaf_Hook(__int64 a1, __int64 a2)
{
    __try { return TextLayoutLeaf_Orig(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// GOLF (and all A2 events) DIAGNOSTIC. sub_465F820(ctx, channel, payload) is the UNetEventsBridge
// server-side dispatch that BOTH the small-payload (TriggerEventOnServer_Param_Implementation) and the
// fractured path funnel into. payload a3 = {bytes@0, int len@8}. The bytes are a self-describing tagged
// tree (tag1=bool/4, tag3=number/8-byte double, tag5=string len-prefixed, tag6=table of key/value).
// Golf fires {EventType="BallInCup"|"OnPlayerStartHole", Cup/Score/PlayerID/HoleIndex as doubles}. Hook
// it, hexdump small payloads so we can see the REAL bytes and write the exact parser. Capped; temporary.
static constexpr uintptr_t EvtDispatch_RVA = HX::EvtDispatch_RVA;
using EvtDispatch_t = __int64(__fastcall*)(__int64, __int64, __int64);
static EvtDispatch_t EvtDispatch_Orig = nullptr;
static int g_golfDumpCount = 0;
// True if the byte buffer contains `needle` as either UTF-8 (stride 1) or UTF-16LE (stride 2) chars.
static bool BytesContainAscii(const uint8_t* p, int len, const char* needle)
{
    const int nl = (int)strlen(needle);
    for (int stride = 1; stride <= 2; ++stride)
        for (int i = 0; i + (nl - 1) * stride < len; ++i)
        {
            int k = 0;
            for (; k < nl; ++k) if (p[i + k * stride] != (uint8_t)needle[k]) break;
            if (k == nl) return true;
        }
    return false;
}
static __int64 __fastcall EvtDispatch_Hook(__int64 ctx, __int64 channel, __int64 payload)
{
    __try {
        const uint8_t* p = payload ? *reinterpret_cast<uint8_t**>(payload) : nullptr;
        const int plen  = payload ? *reinterpret_cast<int*>(payload + 8) : 0;
        // Golf-only filter: skip everything unless the payload mentions a golf event/key. Kills the
        // boot spam (this dispatch fires for ALL A2 events) and targets BallInCup/OnPlayerStartHole.
        const bool isGolf = p && plen > 0 &&
            (BytesContainAscii(p, plen, "Cup") || BytesContainAscii(p, plen, "Hole") ||
             BytesContainAscii(p, plen, "Golf") || BytesContainAscii(p, plen, "StartHole"));
        if (isGolf && plen < 512 && g_golfDumpCount < 120)
        {
            // ascii view first (cheap) so we can eyeball which events are golf; skip pure-spam later.
            char asc[513];
            const int n = plen < 512 ? plen : 512;
            for (int i = 0; i < n; ++i) asc[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
            asc[n] = 0;
            // channel name (FString {wchar*@0,int@8}); guard — may be a different struct.
            const wchar_t* ch = channel ? *reinterpret_cast<wchar_t**>(channel) : nullptr;
            ++g_golfDumpCount;
            printf("[HalcyonA2][EVT] chan='%ls' len=%d ascii=\"%s\"\n", ch ? ch : L"?", plen, asc);
            // hex of the first 96 bytes for the parser.
            char hex[96 * 3 + 1]; const int hn = plen < 96 ? plen : 96;
            for (int i = 0; i < hn; ++i) sprintf(hex + i * 3, "%02X ", p[i]);
            printf("[HalcyonA2][EVT] hex=%s\n", hex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EvtDispatch_Orig(ctx, channel, payload);
}

// ATicketManager::GiveAndCheckTicket impl (sub_46F6180). The arena-admission gate: gives
// the ticket (roster name) then verifies -> AddToVerifiedTicketHolders + SetColorAndTeamIndex.
// On our headless server players show on the roster but never reach VerifiedTicketHolders
// (the list the ball-sim rebuild + team-apply BOTH read) -> no team, no real sim seat (-2).
// Log every decision input + the verified-count promotion so we see WHICH bail fires:
//   team-full (teamSize>=max), CurrentTicket>RequiredTicket, whitelist, or the *(this+0x148)
//   world/slot gate that the admit branch requires non-null.
static constexpr uintptr_t GiveCheck_RVA = HX::GiveCheck_RVA;
using GiveCheck_t = char(__fastcall*)(uintptr_t, uint64_t*, int64_t, char, char);
static GiveCheck_t GiveCheck_Orig = nullptr;
static int g_gacLog = 0;
static char __fastcall GiveCheck_Hook(uintptr_t tm, uint64_t* pawnIface, int64_t teamIndex,
                                      char silent, char checkOverlap)
{
    int   vBefore = -1, tBefore = -1, ts = -1, mts = -1, ovr = 0;
    float cur = 0.f, req = 0.f;
    void* gate = nullptr; void* pawn = nullptr;
    const int team = static_cast<int>(teamIndex);
    __try {
        vBefore = *reinterpret_cast<int*>(tm + 0x390);   // VerifiedTicketHolders num
        tBefore = *reinterpret_cast<int*>(tm + 0x380);   // TicketHolders num
        cur     = *reinterpret_cast<float*>(tm + 0x36C);
        req     = *reinterpret_cast<float*>(tm + 0x370);
        ovr     = *reinterpret_cast<uint8_t*>(tm + 0x368);
        int* tsD = *reinterpret_cast<int**>(tm + 0x3E0); int tsN = *reinterpret_cast<int*>(tm + 0x3E8);
        int* mD  = *reinterpret_cast<int**>(tm + 0x3F0); int mN  = *reinterpret_cast<int*>(tm + 0x3F8);
        if (tsD && team >= 0 && team < tsN) ts  = tsD[team];
        if (mD  && team >= 0 && team < mN)  mts = mD[team];
        gate = *reinterpret_cast<void**>(tm + 0x148);    // the admit-branch world/slot gate
        pawn = pawnIface ? reinterpret_cast<void*>(pawnIface[0]) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    char r = GiveCheck_Orig(tm, pawnIface, teamIndex, silent, checkOverlap);

    // On a successful admit, do the two things the headless path leaves undone:
    // (1) stamp the pawn->manager backptr — GiveAndCheckTicket sets slot+ticket+team but
    //     leaves CurrentTicketManager@0x1128 null, and the seat-rebuild (sub_540E8A0)
    //     likely maps pawn->sim through it; a null => dumped to the -2 template.
    // (2) force a sim reconcile — reconcile (sub_540D970) only runs on VRPawn-count change,
    //     which does NOT fire on admission, so the freshly-verified holder never gets
    //     copied into SimulationsOutline.PlayerIndexes without a nudge.
    printf("[HalcyonA2][TCOL] GiveAndCheckTicket -> ret=%d pawn=%p team=%d (colors push %s)\n",
           (int)r, pawn, team, (r && pawn) ? "YES" : "NO");
    if (r && pawn)
    {
        __try { *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + 0x1128) =
                    reinterpret_cast<void*>(tm); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_lastReconcileCount = -1;   // != g_vrPawnCount => reconcile fires next tick

        // (3) team color/membership. P0 writes the REPLICATED per-player color+team (sticks + all
        //     clients see it); P1 sets the player-state team so the roster resolves it (was "None");
        //     PushTeamColorToClient stays for the instant owning-client local paint.
        PushReplicatedTeamColor(pawn, tm, team);   // P0 (authoritative, replicated)
        SetPlayerStateTeam(pawn, team);            // P1 (roster / per-player team)
        PushTeamColorToClient(pawn, tm, team);     // local paint (belt-and-suspenders)
    }

    int vAfter = -1;
    __try { vAfter = *reinterpret_cast<int*>(tm + 0x390); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_gacLog < 200)
    {
        printf("[HalcyonA2][GAC] tm=%p pawn=%p team=%d silent=%d ovr=%d cur=%.1f req=%.1f "
               "teamSize=%d/%d gate(0x148)=%p verified %d->%d ticketHolders=%d ret=%d\n",
               (void*)tm, pawn, team, (int)silent, ovr, cur, req, ts, mts, gate,
               vBefore, vAfter, tBefore, (int)r);
        ++g_gacLog;
    }
    return r;
}

static void SafeDumpBallStructure() { __try { DumpBallStructure(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Match start requires a disc to physically overlap the CenterDiscCylinder start
// trigger (a UPhysicalComponent whose Collider is set to "OverlapAll"), which fires
// OverlapBegin -> OnOverlapByDisc -> the Luau OnBallHitsStartTrigger. Our sim writes
// the ball transform raw each step, so UE never re-runs overlap detection and the
// disc slides through without tripping the trigger. Force the recheck by calling the
// engine's UPrimitiveComponent::UpdateOverlaps directly on each disc's root primitive
// (it recomputes overlaps and fires OverlapBegin/End) -> the already-armed trigger
// fires naturally.
//   sub_37A7960 = UPrimitiveComponent::UpdateOverlaps(this, const TOverlapArrayView* PendingOverlaps,
//                 bool bDoNotifyActors, const TOverlapArrayView* OverlapsAtEndLocation)
//   (identified by the "UpdateOverlaps" stat string it registers)
// UpdateOverlaps can null-deref on a half-constructed / unregistered component (a
// disc renderer being spawned in a ConstructionScript, a pooled collider with no
// world yet). Guard each call with SEH so a bad component is skipped, not fatal.
// SEH function must hold no C++ unwinding objects (MSVC C2712) — so this is isolated.
static char(*g_UpdateOverlaps)(void*, void*, unsigned char, void*) = nullptr;
static char SafeUpdateOverlaps(void* comp)
{
    __try { return g_UpdateOverlaps(comp, nullptr, /*bDoNotifyActors=*/1, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void PumpBallOverlaps()
{
    auto* primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
    if (!primCls)
        return;
    if (!g_UpdateOverlaps)
        g_UpdateOverlaps = reinterpret_cast<char(*)(void*, void*, unsigned char, void*)>(GetBase() + 0x37A7960);

    // CACHED disc/physical/goal lists, rebuilt ~1s. The old per-call full GObjects walk (120k+ objects
    // now that multiple gamemodes load) ran 10-20Hz ON THE GAME THREAD and starved actor replication ->
    // remote players teleported/snapped. DetectGoals hit + fixed the exact same thing; same pattern here.
    // Iterating the small cached lists (~70 discs + ~220 phys + ~32 goals) instead of 120k objects is a
    // ~300x cut. Stale entries (GC between rebuilds) are caught by the SafePumpBallOverlaps SEH wrapper.
    static SDK::UObject* discs[128]; static int nDisc = 0;
    static SDK::UObject* phys[512];  static int nPhys = 0;
    static SDK::UObject* goals[64];  static int nGoal = 0;
    static ULONGLONG lastRebuild = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastRebuild > 1000 || (nDisc == 0 && nPhys == 0 && nGoal == 0))
    {
        lastRebuild = now;
        nDisc = nPhys = nGoal = 0;
        auto* discCls = SDK::UObject::FindClassFast("DiscEntity");
        auto* physCls = SDK::UObject::FindClassFast("PhysicalComponent");
        auto* goalCls = SDK::UObject::FindClassFast("GoalComponent");
        const int32_t num = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (discCls && nDisc < 128 && o->IsA(discCls))      discs[nDisc++] = o;
            else if (physCls && nPhys < 512 && o->IsA(physCls)) phys[nPhys++]  = o;
            else if (goalCls && nGoal < 64 && o->IsA(goalCls))  goals[nGoal++] = o;
        }
    }

    // Disc side: re-check each ball's own root primitive AND its SphereComponent@0x4D0 (the real
    // collision body on ADiscEntity — the root may be a non-colliding scene component).
    for (int i = 0; i < nDisc; ++i)
    {
        auto* o = discs[i];
        auto* a = static_cast<SDK::AActor*>(o);
        auto* root = a->RootComponent;
        if (root && root->IsA(primCls))
            SafeUpdateOverlaps(root);
        void* sphere = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x4D0);
        if (sphere && static_cast<SDK::UObject*>(sphere)->IsA(primCls))
        {
            static_cast<SDK::UPrimitiveComponent*>(sphere)->SetGenerateOverlapEvents(true);
            SafeUpdateOverlaps(sphere);
        }
    }
    // Trigger side: force each PhysicalComponent's Collider (@0x4F0) to generate events + re-check so a
    // disc sitting inside a start trigger trips OnOverlapByDisc (both sides must generate overlap events).
    for (int i = 0; i < nPhys; ++i)
    {
        void* collider = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(phys[i]) + 0x4F0);
        if (collider && static_cast<SDK::UObject*>(collider)->IsA(primCls))
        {
            static_cast<SDK::UPrimitiveComponent*>(collider)->SetGenerateOverlapEvents(true);
            SafeUpdateOverlaps(collider);
        }
    }
    // Goal side: UGoalComponent's trigger is a UStaticMeshComponent @0x5A0 (not a UPhysicalComponent).
    for (int i = 0; i < nGoal; ++i)
    {
        void* collider = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(goals[i]) + 0x5A0);
        if (collider && static_cast<SDK::UObject*>(collider)->IsA(primCls))
        {
            static_cast<SDK::UPrimitiveComponent*>(collider)->SetGenerateOverlapEvents(true);
            SafeUpdateOverlaps(collider);
        }
    }
}

// The pump touches many game objects (SetGenerateOverlapEvents, UpdateOverlaps) that
// can be half-constructed while an arena is streaming in -> null deref. Wrap the whole
// thing in SEH (this wrapper holds no C++ unwinding objects, so __try is legal) so a
// transient bad object skips a frame instead of killing the process.
static void SafePumpBallOverlaps()
{
    __try { PumpBallOverlaps(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Goal-collider state: is UGoalComponent::Collider@0x5A0 a valid PrimitiveComponent, is the
// goal enabled (bGoalEnabled@0x5A9), and where is it vs the ball? If OverlapBegin never fires
// we need to know whether the collider is valid/enabled (setup) or the overlap just isn't
// registering (profile/binding).
static void DumpGoals()
{
    auto* goalCls = SDK::UObject::FindClassFast("GoalComponent");
    auto* primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
    if (!goalCls || !primCls)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
    int shown = 0;
    for (int32_t i = 0; i < num && shown < 8; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(goalCls))
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        void* col        = *reinterpret_cast<void**>(p + 0x5A0);
        const int  team  = *reinterpret_cast<int*>(p + 0x590);
        const int  gti   = *reinterpret_cast<int*>(p + 0x598);
        const int  en    = *reinterpret_cast<uint8_t*>(p + 0x5A9);
        const int  ig    = *reinterpret_cast<uint8_t*>(p + 0x5A8);
        const bool cPrim = col && static_cast<SDK::UObject*>(col)->IsA(primCls);
        SDK::FVector loc{};
        if (cPrim) loc = static_cast<SDK::USceneComponent*>(col)->K2_GetComponentLocation();
        printf("[HalcyonA2][GOALDBG] %s col=%p prim=%d enabled=%d inGoal=%d team=%d gti=%d pos=(%.0f,%.0f,%.0f)\n",
               o->GetName().c_str(), col, (int)cPrim, en, ig, team, gti,
               (double)loc.X, (double)loc.Y, (double)loc.Z);
        ++shown;
    }
}
static void SafeDumpGoals() { __try { DumpGoals(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Every goal ships with bGoalEnabled@0x5A9 == 0 on our server (the serverOnly Luau that
// calls EnableGoal(true) at match start never ran) -> UGoalComponent::OverlapBegin
// short-circuits and no goal is ever detected. Call the game's own EnableGoal(true) on each
// real goal (team@0x590 >= 0 filters out the -1 CDO/_GEN_VARIABLE templates). Re-run
// periodically so a goal that disables itself after scoring re-arms for the next round.
static void EnableGoals()
{
    auto* goalCls = SDK::UObject::FindClassFast("GoalComponent");
    if (!goalCls)
        return;
    static SDK::UFunction* fnEnable = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(goalCls))
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        if (*reinterpret_cast<int*>(p + 0x590) < 0)          // template / unassigned goal
            continue;
        if (*reinterpret_cast<uint8_t*>(p + 0x5A9) != 0)     // already enabled
            continue;
        if (!fnEnable) fnEnable = o->Class->GetFunction("GoalComponent", "EnableGoal");
        if (!fnEnable) return;
        struct { bool bEnable; } params{ true };
        o->ProcessEvent(fnEnable, &params);
        // Belt-and-suspenders: force the flag too, in case EnableGoal is a headless no-op.
        // Our scoring is geometric (DetectGoals), so all we need is this byte set for the gate.
        *reinterpret_cast<uint8_t*>(p + 0x5A9) = 1;
    }
}
static void SafeEnableGoals() { __try { EnableGoals(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// GEOMETRIC goal detection. The UE overlap chain (ball collider vs UGoalComponent::Collider
// -> OverlapBegin) never fires on our server even with goals enabled + overlaps pumped
// (collision profile / Luau-driven binding missing). So detect the ball entering a goal by
// distance and drive the game's own score path directly. SimulateGoalOnTeam0() runs the full
// native chain (IncrementScore + freeze ball + advance round via the GameStateManager that
// BP already listens to). Per-goal 3s cooldown prevents double-counting while the ball sits
// in. (Team-correct scoring is a follow-up; this proves the pipeline.)
struct FBoundsParams { SDK::USceneComponent* Component; SDK::FVector Origin; SDK::FVector BoxExtent; float SphereRadius; char Pad[4]; };

// Team-correct goal scoring, extracted from UGoalComponent::SimulateGoalOnTeam0. That function
// just fakes up an FGoalInfo and calls sub_5371710(goal, &info) @ RVA 0x5371710 — the real scorer,
// which resolves the arena's ScoreComponent, IncrementScore()s, freezes the ball, and advances the
// round via the GameStateManager the BP already listens to. SimulateGoalOnTeam0 hardcodes
// ScoringTeam=1 (ball in team-0's net -> team 1 scores) + sets goal+0x5A8=1 first. We set
// ScoringTeam from the goal actually hit so the correct team gets the point. Other fields mirror
// Simulate (Distance=100, GoalPoints=1, PlayerScoredID=-1, Timestamp=*(*(goal+0xA0)+0x6C0)); the
// rest are cosmetic stats.
// Fill an FString (16 bytes at outFStr) with the scorer's display name via the game's own
// GetPlayerName, which returns a GAME-ALLOCATED FString. This is REQUIRED for safety: the score
// component's FString ops (assign/realloc/free) must operate on a game-heap block — handing the
// game a pointer into our DLL's memory (a static buffer) trips FMallocBinned2 "realloc an
// unrecognized block" and crashes the process. The out FString leaks its buffer (SDK FString has no
// destructor), which is a tiny, per-goal leak — acceptable, and safe.
static void FillScorerNameFString(int playerIdx, void* outFStr)
{
    memset(outFStr, 0, 0x10);
    if (playerIdx < 0) return;
    static SDK::UClass* pawnCls = nullptr;
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
    if (!pawnCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pawnCls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        if (*reinterpret_cast<unsigned char*>(p + HX::VRPawn_PlayerIndex) != playerIdx) continue;
        auto* ps = *reinterpret_cast<SDK::UObject**>(p + 0x2B8);   // APawn.PlayerState
        if (!ps || !ps->Class) return;
        static SDK::UFunction* fn = nullptr;
        if (!fn) fn = ps->Class->GetFunction("PlayerState", "GetPlayerName");
        if (!fn) fn = ps->Class->GetFunction("AxPlayerState", "GetPlayerName");
        if (!fn) return;
        SafeProcessEvent(ps, fn, outFStr);   // GetPlayerName params = { FString ReturnValue@0 }
        return;
    }
}

// SEH-isolate the name resolution — it scans GObjects + ProcessEvents GetPlayerName, and it runs
// BEFORE the score call, so a fault here must NOT abort ScoreGoal (that would silently swallow the
// goal). On failure just leave the name empty; the score still lands.
static void SafeFillScorerName(int id, void* out) { __try { FillScorerNameFString(id, out); } __except (EXCEPTION_EXECUTE_HANDLER) { memset(out, 0, 0x10); } }

using GoalScoreFn = void(__fastcall*)(void* goal, SDK::FGoalInfo* info);
static bool g_goalUseOverlap = true;   // prefer driving the game's own OverlapBegin (FX + score)
static void ScoreGoal(void* goal, int scoringTeam, int scorerId, void* ball)
{
    const uintptr_t g = reinterpret_cast<uintptr_t>(goal);

    // PREFERRED PATH — drive the game's OWN UGoalComponent::OverlapBegin with the ball. Our direct
    // sub_5371710 call only publishes an EMPTY "PostGoalWithDisc" bridge event (conductor scores, but
    // clients get no goal data -> no FX). OverlapBegin builds the full payload (velocity/position/scorer/
    // bounces @goal+0x1160), fires the effect virtual + sound, and arms the timer that publishes the
    // populated event -> clients get the goal FX AND the conductor still scores off the same publish.
    // Guarded: OverlapBegin sets goal+0x480 = 257 (bInGoal|pending) when it runs the full path; if its
    // arena/disc guards reject our synthetic overlap it won't, so we fall back to the raw scorer.
    if (g_goalUseOverlap && ball)
    {
        auto* gc = static_cast<SDK::UObject*>(goal);
        void* collider = *reinterpret_cast<void**>(g + 0x5A0);   // UGoalComponent.Collider (== a1+1440 in OverlapBegin)
        static SDK::UFunction* fnOverlap = nullptr;
        if (!fnOverlap && gc->Class) fnOverlap = gc->Class->GetFunction("GoalComponent", "OverlapBegin");
        HxLog("[HalcyonA2][GOALFX] attempt fn=%p collider=%p ball=%p\n", (void*)fnOverlap, collider, ball);
        if (fnOverlap && collider)
        {
            *reinterpret_cast<uint16_t*>(g + 0x480) = 0;   // clear so we can detect a fresh "took"
            struct {
                void* Overlapped; void* OtherActor; void* OtherComp;
                int32_t OtherBodyIndex; bool FromSweep; char pad[3];
                unsigned char SweepResult[0x120];
            } parm{};
            parm.Overlapped = collider;
            parm.OtherActor = ball;
            gc->ProcessEvent(fnOverlap, &parm);
            if (*reinterpret_cast<uint16_t*>(g + 0x480) == 257)
            {
                HxLog("[HalcyonA2][GOALFX] OverlapBegin took -> FX + publish (goal=%p ball=%p)\n", goal, ball);
                return;   // OverlapBegin ran the full path -> it owns the FX + the (timer) publish
            }
            HxLog("[HalcyonA2][GOALFX] OverlapBegin rejected (guards) -> raw scorer fallback (no FX)\n");
        }
    }

    // FALLBACK — raw scorer (no FX). Only reached if OverlapBegin's guards rejected our overlap, so
    // scoring never regresses.
    *reinterpret_cast<uint8_t*>(g + 0x5A8) = 1;   // mark scored (Simulate sets this before scoring)

    double timestamp = 0.0;                        // FGoalInfo.Timestamp (cosmetic goal-time stat)
    if (void* tsrc = *reinterpret_cast<void**>(g + 0xA0))
        timestamp = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(tsrc) + 0x6C0);

    SDK::FGoalInfo info{};
    info.ScoringTeam      = scoringTeam;
    info.Distance         = 100.0f;
    info.GoalPoints       = 1;
    // Attribute the goal to the last player to hit the ball (ADiscEntity.RollbackRecentPlayerHit)
    // so "you scored a goal" quests credit the actual scorer + the scoreboard shows "Scored by".
    info.PlayerScoredTeam = scoringTeam;
    info.PlayerScoredID   = scorerId;
    info.Timestamp        = timestamp;
    // scorer NAME -> game-allocated FString (see FillScorerNameFString; static buffer here crashed).
    // SEH-isolated so name resolution can never prevent the score from landing.
    SafeFillScorerName(scorerId, reinterpret_cast<char*>(&info) + 0x40);

    reinterpret_cast<GoalScoreFn>(GetBase() + 0x5371710)(goal, &info);
}

static void DetectGoals()
{
    auto* goalCls = SDK::UObject::FindClassFast("GoalComponent");
    auto* ballCls = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!goalCls || !ballCls)
        return;
    static SDK::UFunction* fnBounds = nullptr;
    static SDK::UObject*    kslCDO  = nullptr;
    if (!kslCDO)  kslCDO = static_cast<SDK::UObject*>(SDK::UKismetSystemLibrary::GetDefaultObj());
    if (!fnBounds && kslCDO) fnBounds = kslCDO->Class->GetFunction("KismetSystemLibrary", "GetComponentBounds");
    if (!kslCDO || !fnBounds)
        return;
    // Edge-triggered debounce: a ball scores ONCE when it enters a goal, and only re-arms after it
    // has left every goal. (The old 3s time cooldown re-scored a ball that just sat frozen in the net.)
    static std::unordered_set<void*> ballInGoal;
    const double MARGIN = 40.0;   // small slack so the ball (radius) counts as "in"

    // CACHED goal + ball lists, rebuilt ~1Hz. A full GObjects walk (twice) + per-goal
    // GetComponentBounds every 100ms stuttered the game thread on a weak/contended VPS core, which
    // made the rollback sim burst-step and inflated MI. At 10Hz we now only read cached ball
    // positions vs cached goal AABBs (no walk, no ProcessEvent). Stale cache entries (GC) are caught
    // by the SafeDetectGoals SEH wrapper + refreshed on the next rebuild.
    static SDK::UObject* gObj[64]; static SDK::FVector gOrg[64]; static SDK::FVector gExt[64]; static int gN = 0;
    static SDK::UObject* bObj[256]; static int bN = 0;
    static ULONGLONG lastRebuild = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastRebuild > 1000 || gN == 0)
    {
        lastRebuild = now;
        gN = 0; bN = 0;
        const int32_t num = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (gN < 64 && o->IsA(goalCls))
            {
                const uintptr_t p = reinterpret_cast<uintptr_t>(o);
                if (*reinterpret_cast<int*>(p + 0x590) < 0)
                    continue;   // template / unassigned team only — cache ALL real goals regardless
                                // of enabled-state (a goal is briefly DISABLED right after a score;
                                // excluding it from the cache for up to 1s = "can't goal". We check
                                // bGoalEnabled LIVE at score time instead.)
                auto* col = *reinterpret_cast<SDK::USceneComponent**>(p + 0x5A0);
                if (!col) continue;
                FBoundsParams bp{}; bp.Component = col;
                kslCDO->ProcessEvent(fnBounds, &bp);
                gObj[gN] = o; gOrg[gN] = bp.Origin; gExt[gN] = bp.BoxExtent; ++gN;
            }
            else if (bN < 256 && o->IsA(ballCls))
            {
                bObj[bN++] = o;
            }
        }
        int gEnabled = 0;
        for (int g = 0; g < gN; ++g)
            if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(gObj[g]) + 0x5A9)) ++gEnabled;
        HxLog("[HalcyonA2][GOALDBG2] rebuilt cache: goals=%d enabled=%d balls=%d\n", gN, gEnabled, bN);
    }
    if (gN == 0)
        return;

    for (int b = 0; b < bN; ++b)
    {
        auto* o = bObj[b];
        auto* a = static_cast<SDK::AActor*>(o);
        if (!a || !a->RootComponent)
            continue;
        SDK::FVector bp = a->RootComponent->K2_GetComponentLocation();
        int hit = -1;
        for (int g = 0; g < gN; ++g)
        {
            // goal must be ENABLED right now (checked live, not at cache time)
            if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(gObj[g]) + 0x5A9) == 0) continue;
            // inside the goal's world AABB (+ margin)?
            if (fabs(bp.X - gOrg[g].X) > gExt[g].X + MARGIN) continue;
            if (fabs(bp.Y - gOrg[g].Y) > gExt[g].Y + MARGIN) continue;
            if (fabs(bp.Z - gOrg[g].Z) > gExt[g].Z + MARGIN) continue;
            hit = g; break;
        }
        void* ballKey = o;
        if (hit < 0) { ballInGoal.erase(ballKey); continue; }   // outside all goals -> re-arm this ball
        if (ballInGoal.count(ballKey)) continue;                // already scored on this entry
        ballInGoal.insert(ballKey);

        const int goalTeam    = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(gObj[hit]) + 0x590);
        const int scoringTeam = (goalTeam == 0) ? 1 : 0;        // ball in team T's net -> opponent scores
        const int scorer      = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(o) + 0x318);  // RollbackRecentPlayerHit
        ScoreGoal(gObj[hit], scoringTeam, scorer, o);   // pass the ball so OverlapBegin can build FX payload
        HxLog("[HalcyonA2][GOALHIT] %s in goal team=%d -> ScoringTeam=%d scorer(player)=%d\n",
              o->GetName().c_str(), goalTeam, scoringTeam, scorer);
    }
}
static void SafeDetectGoals() { __try { DetectGoals(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// GOLF SINK DETECTION (geometric, mirrors DetectGoals). The golf conductor (ServerCourseLogic.luau)
// RUNS and receives OnPlayerStartHole, but BallInCup never reaches it: the client's relay is gated on
// the native Cup.onBallInCup_Multicast, which never fires headless (the kinematically-replicated ball
// generates no server physics overlap in the cup's trigger volume). But the server DOES have the
// ball's transform (BP_GolfBallDisplay_C : BP_JakeBall_C -> RootComponent location updates; remotes see
// it move + sink). So detect the sink ourselves: ball RootComponent pos inside AGolfCup's
// GoalTriggerVolume(@0x2B8) AABB -> call the cup's reflected BallInCup(Score) -> broadcasts
// onBallInCup_Multicast -> client relays {BallInCup,...} -> conductor scores. Score (real stroke count)
// source is still unknown (native passes hardcoded 3; client-side counter) -> log candidate ball fields
// + fire a placeholder for now to PROVE the detect->fire->conductor pipeline; refine Score next.
static bool g_golfSinkDetect = true;
// Per-ball stroke tracking. NewHitEvent only fires on the FIRST hit server-side (confirmed: dribble
// ball works off one hit + a timer; stroke ball needs one per swing and freezes at 1; and the ball
// DOES move server-side — both VR + spectators see it). So count strokes ourselves: a golf stroke =
// the ball going from AT-REST -> suddenly MOVING (you only ever hit a stopped ball). Edge-detect that
// per ball at 10Hz off the same RootComponent position the sink detector reads.
struct GolfBallState {
    SDK::FVector last;         // motion-detect: last streamed pos
    bool         moving;
    int          strokes;
    ULONGLONG    seen;
    SDK::FVector lastHit;      // NewHitEvent: location of the last COUNTED strike
    bool         wasHit;       // NewHitEvent: first hit seen for this ball
    bool         hitDriven;    // NewHitEvent has fired for this ball -> motion counting backs off
};
static std::unordered_map<void*, GolfBallState> g_golfState;

// FName ComparisonIndex of "NewHitEvent", resolved once (0 = not yet resolved). Lets ProcessEvent_Hook
// recognize the event with a cheap integer compare instead of a per-call GetName() string alloc.
static int32_t g_nheIdx = 0;
// BP_GolfBallDisplay_C class (the stroke ball), resolved once. Used to guard the LastHitLocation poison
// so we only touch that field on balls that actually have it (dribble balls have a different layout).
static SDK::UClass* g_strokeBallCls = nullptr;
// BP_GolfBallDisplay_C property offsets (from the SDK dump):
//   StrokeCount       int32   @0x650 (Net, RepNotify -> OnRep_StrokeCount -> SetBallNumber)
//   LastHitLocation   FVector @0x658 (the anti-double-count anchor the game's gate measures against)
static constexpr uintptr_t GolfBall_LastHitLocation_Off = 0x658;

// The GAME's own StrokeCount++ lives in NewHitEvent, gated on
// Vector_Distance(GetActorLocation(), LastHitLocation) >= StrokeDistanceThreshold(100). Headless the
// ball actor's location is FROZEN, so that gate can never pass after the first hit -> count stuck at 1.
// NewHitEvent itself DOES fire per hit (server-auth, dispatched by FName via ProcessEvent), so we run
// the game's exact logic here but feed it the client-streamed position (g_ballStreamPos) instead of the
// frozen GetActorLocation(). This is authoritative per-hit — no motion/gap heuristics needed.
static void GolfHitEvent(SDK::UObject* ball)
{
    if (!ball) return;

    // Poison LastHitLocation to a far point so the GAME's own NewHitEvent gate
    // (Vector_Distance(GetActorLocation(), LastHitLocation) >= 100) PASSES — its GetActorLocation is
    // frozen headless, so without this the native StrokeCount++ never runs and the on-ball number
    // (SetBallNumber, driven by the replicated StrokeCount) stays stuck at 1. We run BEFORE the native
    // handler (ProcessEvent_Orig), so the game then increments StrokeCount itself and drives
    // SetBallNumber + push-replication + ReportStrokeCountToTee natively. Guarded to the stroke-ball
    // class so we never write 0x658 on a ball with a different layout. First hit (WasHit=false) skips
    // the gate anyway and overwrites LastHitLocation, so poisoning every time is harmless.
    if (g_strokeBallCls && ball->IsA(g_strokeBallCls))
    {
        SDK::FVector* lhl = reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(ball) + GolfBall_LastHitLocation_Off);
        lhl->X = 1.0e9; lhl->Y = 1.0e9; lhl->Z = 1.0e9;
    }

    auto sp = g_ballStreamPos.find(ball);
    SDK::FVector pos = (sp != g_ballStreamPos.end()) ? sp->second.pos : SDK::FVector{ 0, 0, 0 };
    GolfBallState& st = g_golfState[ball];   // default-constructs if absent
    st.hitDriven = true;                     // authoritative source is live -> motion path backs off
    if (!st.wasHit)
    {
        st.wasHit = true; st.lastHit = pos; st.strokes = 1;
        HxLog("[HalcyonA2][GOLFHIT] ball=%s FIRST stroke #1\n", ball->GetName().c_str());
        return;
    }
    const double dx = pos.X - st.lastHit.X, dy = pos.Y - st.lastHit.Y, dz = pos.Z - st.lastHit.Z;
    const double d = sqrt(dx*dx + dy*dy + dz*dz);
    if (d >= 100.0)   // StrokeDistanceThreshold — same anti-double-count gate the game uses
    {
        st.lastHit = pos; st.strokes++;
        HxLog("[HalcyonA2][GOLFHIT] ball=%s stroke #%d (dist=%.0f)\n", ball->GetName().c_str(), st.strokes, d);
    }
    else
        HxLog("[HalcyonA2][GOLFHIT] ball=%s ignored (dist=%.0f < 100)\n", ball->GetName().c_str(), d);
}
static void GolfSinkDetect()
{
    auto* cupCls  = SDK::UObject::FindClassFast("GolfCup");
    auto* ballCls = SDK::UObject::FindClassFast("BP_JakeBall_C");   // golf balls are subclasses
    if (!cupCls || !ballCls) return;                                // not a golf level -> nothing to do
    static SDK::UObject* kslCDO = nullptr; static SDK::UFunction* fnBounds = nullptr;
    if (!kslCDO) kslCDO = static_cast<SDK::UObject*>(SDK::UKismetSystemLibrary::GetDefaultObj());
    if (!fnBounds && kslCDO) fnBounds = kslCDO->Class->GetFunction("KismetSystemLibrary", "GetComponentBounds");
    if (!kslCDO || !fnBounds) return;

    static std::unordered_set<void*> ballInCup;   // edge-trigger: score once per entry, re-arm on exit
    const double MARGIN = 30.0;

    static SDK::UObject* cObj[64]; static SDK::FVector cOrg[64]; static SDK::FVector cExt[64];
    static SDK::UObject* cComp[64]; static int cN = 0;   // cComp = the cup's GolfCupComponent (networked BallInCup)
    static SDK::UObject* bObj[256]; static int bNg = 0;
    static ULONGLONG lastRb = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastRb > 1000 || cN == 0)
    {
        lastRb = now; cN = 0; bNg = 0;
        auto* compCls = SDK::UObject::FindClassFast("GolfCupComponent");
        SDK::UObject* compTmp[128]; int nComp = 0;
        const int32_t num = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (cN < 64 && o->IsA(cupCls))
            {
                auto* vol = *reinterpret_cast<SDK::USceneComponent**>(reinterpret_cast<uintptr_t>(o) + 0x2B8); // GoalTriggerVolume
                if (!vol) continue;
                FBoundsParams bp{}; bp.Component = vol;
                kslCDO->ProcessEvent(fnBounds, &bp);
                cObj[cN] = o; cOrg[cN] = bp.Origin; cExt[cN] = bp.BoxExtent; cComp[cN] = nullptr; ++cN;
            }
            else if (compCls && nComp < 128 && o->IsA(compCls))
                compTmp[nComp++] = o;
            else if (bNg < 256 && o->IsA(ballCls))
                bObj[bNg++] = o;
        }
        // Match each cup to its GolfCupComponent by walking the component's Outer chain up to the cup —
        // that component's BallInCup PUBLISHES over the bridge (networked); the cup actor's doesn't.
        int matched = 0;
        for (int c = 0; c < cN; ++c)
            for (int k = 0; k < nComp; ++k)
            {
                SDK::UObject* ow = compTmp[k]->Outer; int guard = 0;
                while (ow && guard++ < 6) { if (ow == cObj[c]) break; ow = ow->Outer; }
                if (ow == cObj[c]) { cComp[c] = compTmp[k]; ++matched; break; }
            }
        HxLog("[HalcyonA2][GOLFSINK] cache: cups=%d comps=%d balls=%d\n", cN, matched, bNg);

        // Resolve the "NewHitEvent" FName index once so ProcessEvent_Hook can count strokes off the
        // authoritative per-hit event (cheap integer compare, no per-call string work).
        if (!g_nheIdx && bNg > 0 && bObj[0]->Class)
        {
            SDK::UFunction* f = bObj[0]->Class->GetFunction("BP_GolfBallDisplay_C", "NewHitEvent");
            if (!f) f = bObj[0]->Class->GetFunction("BP_DribbleGolfBall_C", "NewHitEvent");
            if (!f) f = bObj[0]->Class->GetFunction("BP_JakeBall_C", "NewHitEvent");
            if (!f) f = bObj[0]->Class->GetFunction("DiscEntity", "NewHitEvent");
            if (f) { g_nheIdx = f->Name.ComparisonIndex; HxLog("[HalcyonA2][GOLFSINK] NewHitEvent FName idx=%d\n", g_nheIdx); }
        }
        if (!g_strokeBallCls)
            g_strokeBallCls = SDK::UObject::FindClassFast("BP_GolfBallDisplay_C");
    }
    if (cN == 0) return;

    const double MOVE_THRESH = 60.0;   // per-100ms displacement that counts as "hit into motion"
    const double REST_THRESH = 15.0;   // below this = at rest -> re-arm for the next stroke
    std::unordered_set<void*> live;

    for (int b = 0; b < bNg; ++b)
    {
        void* key = bObj[b];
        // Golf balls' RootComponent is frozen; use the client-streamed position instead. Only balls
        // being actively streamed (the player's live golf ball) have a recent entry -> this also
        // naturally filters the 71 cached balls down to the one in play.
        auto sp = g_ballStreamPos.find(key);
        if (sp == g_ballStreamPos.end() || (now - sp->second.seen) > 2000) continue;
        SDK::FVector p = sp->second.pos;
        live.insert(key);

        // --- stroke count ---
        // Primary source is the authoritative NewHitEvent (GolfHitEvent, driven from ProcessEvent_Hook).
        // The motion edge-detect below is a FALLBACK for balls that never see a NewHitEvent — once one
        // has (st.hitDriven), we stop motion-counting to avoid double-counting.
        auto it = g_golfState.find(key);
        if (it == g_golfState.end()) { g_golfState[key] = { p, false, 0, now, {0,0,0}, false, false }; continue; }   // baseline only
        GolfBallState& st = it->second;

        if (!st.hitDriven)
        {
            // Stream-gap re-arm: the client only streams the ball WHILE it's in motion, so a gap since we
            // last saw it means it came to REST between strokes. Re-arm + re-baseline without counting —
            // otherwise if the stream cuts out mid-decel (moving latched true), the next hit never counts.
            if (now - st.seen > 300)
            {
                st.moving = false; st.last = p; st.seen = now;
            }
            else
            {
                const double dx = p.X - st.last.X, dy = p.Y - st.last.Y, dz = p.Z - st.last.Z;
                const double disp = sqrt(dx*dx + dy*dy + dz*dz);
                if (!st.moving && disp > MOVE_THRESH)
                {
                    st.moving = true; st.strokes++;
                    HxLog("[HalcyonA2][GOLFSTROKE] ball=%s stroke #%d (disp=%.0f)\n", bObj[b]->GetName().c_str(), st.strokes, disp);
                }
                else if (disp < REST_THRESH) st.moving = false;   // rested -> arm for the next hit
                st.last = p; st.seen = now;
            }
        }
        else { st.last = p; st.seen = now; }   // keep motion baseline fresh but don't count

        // --- geometric cup overlap ---
        int hit = -1;
        for (int c = 0; c < cN; ++c)
        {
            if (fabs(p.X - cOrg[c].X) > cExt[c].X + MARGIN) continue;
            if (fabs(p.Y - cOrg[c].Y) > cExt[c].Y + MARGIN) continue;
            if (fabs(p.Z - cOrg[c].Z) > cExt[c].Z + MARGIN) continue;
            hit = c; break;
        }
        if (hit < 0) { ballInCup.erase(key); continue; }
        if (ballInCup.count(key)) continue;
        ballInCup.insert(key);

        const int score = st.strokes > 0 ? st.strokes : 1;   // our tracked stroke count
        auto* cup  = cObj[hit];
        auto* comp = cComp[hit];
        int firedWhat = 0;
        // The NETWORKED BallInCup lives on the GolfCupComponent (publishes over the UNetEventsBridge —
        // reaches clients + the ServerCourseLogic conductor). The cup actor's BallInCup is a local
        // multicast delegate that reaches nobody. Prefer the component; fall back to the actor.
        SDK::UFunction* fnComp = (g_golfSinkDetect && comp && comp->Class)
            ? comp->Class->GetFunction("GolfCupComponent", "BallInCup") : nullptr;
        if (fnComp)
        {
            struct { int32_t Score; int32_t _pad; void* Disc; } parm{ score, 0, bObj[b] };
            comp->ProcessEvent(fnComp, &parm);
            firedWhat = 2;
        }
        else
        {
            SDK::UFunction* fnBIC = (g_golfSinkDetect && cup->Class) ? cup->Class->GetFunction("GolfCup", "BallInCup") : nullptr;
            if (fnBIC) { struct { int32_t Score; } parm{ score }; cup->ProcessEvent(fnBIC, &parm); firedWhat = 1; }
        }
        HxLog("[HalcyonA2][GOLFSINK] SUNK ball=%s cup=%s comp=%p pos=(%.0f,%.0f,%.0f) strokes=%d fired=%d\n",
              bObj[b]->GetName().c_str(), cup->GetName().c_str(), (void*)comp, p.X, p.Y, p.Z, score, firedWhat);
    }

    // drop tracking for balls that are gone (GC / new hole) so counts don't leak across balls
    if (g_golfState.size() > live.size())
        for (auto it = g_golfState.begin(); it != g_golfState.end(); )
            (live.count(it->first)) ? (void)++it : (void)(it = g_golfState.erase(it));
}
static void SafeGolfSinkDetect() { __try { GolfSinkDetect(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Volleyfall "spleef" floor. Each tile is an ABP_FloorPanelB_LE_C. Its
// ReceiveActorBeginOverlap(disc) destroys the tile: Cast<DiscEntity>(OtherActor),
// 0.2s delay, then Mesh collision off + Mesh hidden + IsActive=false (Net, RepIndex
// 10, @0x2C0) MarkPropertyDirty + tileDestroyed.Broadcast(). ResetPanel restores it.
// That overlap is a physics touch between the disc (ball) and the panel, which never
// fires on our headless server (same reason jakeball goals / the golf cup don't).
// So detect the ball-in-panel overlap GEOMETRICALLY (exactly like DetectGoals: read the
// jakeball RootComponent world location vs each panel's cached AABB) and call the game's
// OWN ReceiveActorBeginOverlap with the real disc so its authentic destroy path (incl.
// the IsActive replication clients watch) runs. Volleyfall balls are jakeballs
// (JakeBallSpawner), whose RootComponent is live server-side (golf balls' isn't).
// ---------------------------------------------------------------------------
static bool g_volleyfall = true;
static void VolleyfallTick()
{
    if (!g_volleyfall) return;
    auto* panelCls = SDK::UObject::FindClassFast("BP_FloorPanelB_LE_C");
    if (!panelCls) return;   // not a volleyfall level -> nothing to do
    auto* ballCls  = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!ballCls) return;

    static SDK::UObject* kslCDO = nullptr; static SDK::UFunction* fnBounds = nullptr;
    if (!kslCDO) kslCDO = static_cast<SDK::UObject*>(SDK::UKismetSystemLibrary::GetDefaultObj());
    if (!fnBounds && kslCDO) fnBounds = kslCDO->Class->GetFunction("KismetSystemLibrary", "GetComponentBounds");
    if (!kslCDO || !fnBounds) return;

    auto* spawnCls = SDK::UObject::FindClassFast("BallSpawnerComponent");

    constexpr uintptr_t Panel_IsActive_Off = 0x2C0;   // bool IsActive (Net, RepIndex 10)
    constexpr uintptr_t Panel_Mesh_Off     = 0x2E0;   // UInstancedStaticMeshComponent* Mesh
    constexpr uintptr_t Spawner_Disc_Off   = 0x580;   // ADiscEntity* Disc (the spawner's current ball)
    const double MARGIN = 40.0;
    const double DROP   = 120.0;   // ball this far below the floor plane = fell through -> reset

    static SDK::UObject* pObj[128]; static SDK::FVector pOrg[128]; static SDK::FVector pExt[128]; static int pN = 0;
    static SDK::UObject* bObj[128]; static int bN = 0;
    static SDK::UObject* sObj[16]; static int sN = 0;   // BallSpawnerComponents
    static SDK::UFunction* fnOverlap = nullptr;
    static SDK::UFunction* fnReset = nullptr;           // BP_FloorPanelB_LE_C::ResetPanel
    static SDK::UFunction* fnResetBall = nullptr;       // BallSpawnerComponent::ResetBall
    static double floorZ = 0.0;
    static ULONGLONG lastRb = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastRb > 1000 || pN == 0)
    {
        lastRb = now; pN = 0; bN = 0; sN = 0;
        double zSum = 0.0;
        const int32_t num = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (pN < 128 && o->IsA(panelCls))
            {
                auto* mesh = *reinterpret_cast<SDK::USceneComponent**>(reinterpret_cast<uintptr_t>(o) + Panel_Mesh_Off);
                if (!mesh) continue;
                FBoundsParams bp{}; bp.Component = mesh;
                kslCDO->ProcessEvent(fnBounds, &bp);
                pObj[pN] = o; pOrg[pN] = bp.Origin; pExt[pN] = bp.BoxExtent; zSum += bp.Origin.Z; ++pN;
            }
            else if (bN < 128 && o->IsA(ballCls))
                bObj[bN++] = o;
            else if (spawnCls && sN < 16 && o->IsA(spawnCls))
                sObj[sN++] = o;
        }
        if (pN > 0) floorZ = zSum / pN;
        if (!fnOverlap && pN > 0 && pObj[0]->Class)
            fnOverlap = pObj[0]->Class->GetFunction("BP_FloorPanelB_LE_C", "ReceiveActorBeginOverlap");
        if (!fnReset && pN > 0 && pObj[0]->Class)
            fnReset = pObj[0]->Class->GetFunction("BP_FloorPanelB_LE_C", "ResetPanel");
        if (!fnResetBall && sN > 0 && sObj[0]->Class)
            fnResetBall = sObj[0]->Class->GetFunction("BallSpawnerComponent", "ResetBall");
        if (pN > 0)
            HxLog("[HalcyonA2][VOLLEY] cache: panels=%d balls=%d spawners=%d floorZ=%.0f fnOverlap=%d fnReset=%d fnResetBall=%d\n",
                  pN, bN, sN, floorZ, fnOverlap ? 1 : 0, fnReset ? 1 : 0, fnResetBall ? 1 : 0);
    }
    if (pN == 0 || !fnOverlap) return;

    // Latch: don't re-fire a panel's destroy during its 0.2s delay window (IsActive is still true
    // until the delay resolves). Once IsActive flips false (destroyed) or true again (reset) the
    // active-check below re-arms it.
    static std::unordered_set<void*> pending;

    for (int b = 0; b < bN; ++b)
    {
        auto* a = static_cast<SDK::AActor*>(bObj[b]);
        if (!a || !a->RootComponent) continue;
        SDK::FVector p = a->RootComponent->K2_GetComponentLocation();
        for (int c = 0; c < pN; ++c)
        {
            auto* panel = pObj[c];
            const bool active = *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(panel) + Panel_IsActive_Off);
            if (!active) { pending.erase(panel); continue; }   // broken (or mid-reset) -> nothing to do / re-arm
            if (pending.count(panel)) continue;                // destroy already in flight (0.2s delay)
            if (fabs(p.X - pOrg[c].X) > pExt[c].X + MARGIN) continue;
            if (fabs(p.Y - pOrg[c].Y) > pExt[c].Y + MARGIN) continue;
            if (fabs(p.Z - pOrg[c].Z) > pExt[c].Z + MARGIN) continue;
            // ball is on this active tile -> run the game's own overlap/destroy with the real disc
            struct { SDK::AActor* OtherActor; } parm{ a };
            panel->ProcessEvent(fnOverlap, &parm);
            pending.insert(panel);
            HxLog("[HalcyonA2][VOLLEY] break panel=%s (ball=%s) pos=(%.0f,%.0f,%.0f)\n",
                  panel->GetName().c_str(), bObj[b]->GetName().c_str(), p.X, p.Y, p.Z);
        }

        // --- reset: ball fell through the floor (below the panel plane) ---
        // The designed reset (Red Reset Trigger OnOverlapByDiscSimple -> ResetPanel on every tile +
        // BallReseter.luau respawning the ball) fires from the same server-side disc overlap that
        // never happens headless. Detect the fall geometrically and drive it: restore all panels +
        // reset the ball via its spawner. Latched per-ball so it fires once per fall.
        static std::unordered_set<void*> fell;
        if (p.Z < floorZ - DROP)
        {
            if (!fell.count(bObj[b]))
            {
                fell.insert(bObj[b]);
                if (fnReset)
                    for (int c = 0; c < pN; ++c) pObj[c]->ProcessEvent(fnReset, nullptr);   // restore floor
                // reset the ball via the spawner that owns it (match by Disc@0x580; else first spawner)
                SDK::UObject* spawner = nullptr;
                for (int s = 0; s < sN; ++s)
                    if (*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(sObj[s]) + Spawner_Disc_Off) == bObj[b]) { spawner = sObj[s]; break; }
                if (!spawner && sN > 0) spawner = sObj[0];
                if (spawner && fnResetBall) spawner->ProcessEvent(fnResetBall, nullptr);
                HxLog("[HalcyonA2][VOLLEY] RESET (ball=%s fell to z=%.0f, floorZ=%.0f) panels=%d spawner=%p\n",
                      bObj[b]->GetName().c_str(), p.Z, floorZ, pN, (void*)spawner);
            }
        }
        else fell.erase(bObj[b]);   // back above the floor -> re-arm
    }
}
static void SafeVolleyfallTick() { __try { VolleyfallTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Server dashboard AUTH — seed the api-key global, then run the native server login.
// The server login (sub_54AB570 -> sub_5339790 -> POST /users/log_in_with_key) sends
// x-api-key = qword_9BD4460, which is EMPTY on our hacked boot (nothing seeds it from the
// -DashboardApiKey cmdline), so the login carries NO key -> "No dashboard api key" forever and the
// authenticated chain (register + /v1/deployments station-config fetch -> boards) never proceeds.
// Cold-start chicken-egg: the login reads the token global, but the token global is only filled by
// a successful login (sub_5425100 stores the returned api_key there). So we seed it ourselves.
// The seed MUST be game-allocated: sub_5425100 later overwrites qword_9BD4460 via sub_FC0890 (which
// frees the old buffer), so a static literal would be freed and crash -> copy via sub_FBCD70 from a
// static view. (This build uses log_in_with_key; the /users/log_in_server route is in the backend
// openapi but that string isn't in the binary, so it's unused.) FString layout at 0x9BD4460:
// Data@0x9BD4460, Num@0x9BD4468, Max@0x9BD446C.
static const wchar_t kDashApiKey[] = L"halcyon-server-key";
using FStringAssign2Fn = void(__fastcall*)(void* dest, const void* src);   // sub_FBCD70 (FString copy)
using SessionLoginFn   = __int64(__fastcall*)(void* sessionSubsystem);     // sub_54AB570 (server login)
static bool      g_dashKeySeeded = false;
static void SeedDashboardApiKey()
{
    // Only seed while empty (Num==0) so we never clobber a real token stored post-login.
    if (*reinterpret_cast<int32_t*>(GetBase() + 0x9BD4468) != 0) { g_dashKeySeeded = true; return; }
    FStringView src{ kDashApiKey,
                     static_cast<int32_t>(sizeof(kDashApiKey) / sizeof(wchar_t)),
                     static_cast<int32_t>(sizeof(kDashApiKey) / sizeof(wchar_t)) };
    reinterpret_cast<FStringAssign2Fn>(GetBase() + 0xFBCD70)(
        reinterpret_cast<void*>(GetBase() + 0x9BD4460), &src);   // qword_9BD4460 = game-owned "halcyon-server-key"
    g_dashKeySeeded = true;
    printf("[HalcyonA2] seeded dashboard api key (qword_9BD4460 = halcyon-server-key)\n");
}
static void SafeSeedDashboardApiKey() { __try { SeedDashboardApiKey(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static bool      g_dashLoginDone = false;
static ULONGLONG g_lastDashLogin = 0;
static int       g_dashLoginTries = 0;
static void TriggerServerDashboardLogin()
{
    SeedDashboardApiKey();   // ensure the key global is populated before we log in
    auto* cls = SDK::UObject::FindClassFast("A2SessionSubsystem");
    if (!cls) return;
    SDK::UObject* ss = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(cls)) { ss = o; break; }
    }
    if (!ss) { printf("[HalcyonA2] server-login: A2SessionSubsystem not live yet\n"); return; }
    printf("[HalcyonA2] server-login: sub_54AB570(session=%p) with seeded key\n", ss);
    g_dashLoginDone = true;   // latch BEFORE the call: sub_54AB570 on this instance faults in our context
                              // (SEH-swallowed) so it never reached this line -> endless 3s retries. The
                              // native sub_540CC30 login already authenticates off our seeded key (log
                              // shows the token), so our explicit call is belt-and-suspenders only.
    reinterpret_cast<SessionLoginFn>(GetBase() + 0x54AB570)(ss);   // native login -> POST log_in_with_key (x-api-key seeded)
}
static void SafeTriggerServerDashboardLogin() { __try { TriggerServerDashboardLogin(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Station dashboard FULL NATIVE INIT. Root cause of everything dashboard-side being dead (no
// server_events, no periodic deployment fetch, no server_launched): sub_541D8A0 is the init that
// registers ALL the reporting delegates (heartbeat sub_54480B0 @client+184, server_events, config
// fetch, etc.) and fires "server_launched" -- but at boot it BAILED at its netmode gate
// (sub_4037D80/GetNetMode != 2) before registering anything, only running its first line
// `qword_9BD44F0 = a1`. Now that gate passes (we hook GetNetMode -> 1 and patched the cmp byte 2->1),
// we call it ourselves post-injection to run the whole init: it re-reads the -Dashboard* cmdline
// args, registers every reporting delegate, and fires server_launched -- the native cascade. One
// shot (it unregisters existing delegates at the slots before re-adding, so a single call is enough;
// don't loop it). client = qword_9BD44F0 (set at boot on sub_541D8A0's first line).
// Full native init sub_541D8A0(client): registers all reporting delegates + fires server_launched.
// It bailed with nothing registered when we called it. Gate B (netmode) is confirmed satisfied
// (GetNetMode hooked ->1, cmp byte patched 2->1), so it's gate A `(*(clientVtable+392))() == 0`
// (client's world/context null post-travel) OR a fault mid-registration. DIAGNOSE FIRST: replicate
// both gate checks and log them, so the next run pinpoints the failure instead of us guessing.
// vtable slot for gate A = 392/8 = 49.  GetNetMode = sub_4037D80 (RVA 0x4037D80).
using CtxVirtFn       = void*(__fastcall*)(void*);
using GetNetModeFn    = int(__fastcall*)(void*);
using DashboardInitFn = __int64(__fastcall*)(void* dashboardClient);   // sub_541D8A0 (full init)
static bool      g_deployFetchDone = false;
static ULONGLONG g_lastDeployFetch = 0;
static int       g_deployFetchTries = 0;
static void TriggerDeploymentFetch()
{
    uint64_t base = GetBase();
    void* client = *reinterpret_cast<void**>(base + HX::DashClientPtr_RVA);   // qword_9BD44F0 (dashboard client)
    if (!client) { printf("[HalcyonA2] dashboard-init: client (qword_9BD44F0) null, waiting\n"); return; }

    // Diagnose sub_541D8A0's two early gates before calling it.
    void** vtbl = *reinterpret_cast<void***>(client);
    void*  ctx  = reinterpret_cast<CtxVirtFn>(vtbl[49])(client);                                   // gate A: (*(v1+392))()
    int    nm   = ctx ? reinterpret_cast<GetNetModeFn>(base + HX::WorldGetNetMode_RVA)(ctx) : -999;              // gate B: sub_4037D80(ctx)
    printf("[HalcyonA2] dashboard-init: gateA ctx=%p  gateB netmode=%d (need ==1)\n", ctx, nm);

    printf("[HalcyonA2] dashboard-init: sub_541D8A0(client=%p) -> full native init\n", client);
    reinterpret_cast<DashboardInitFn>(base + HX::DashboardInit_RVA)(client);
    g_deployFetchDone = true;
}
static void SafeTriggerDeploymentFetch() { __try { TriggerDeploymentFetch(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Diagnostic: enumerate live module (gamemode) slots + confirm UGamemodesManager. A gamemode loads
// via a module slot whose DefaultGamemodePath (e.g. "deathrun", "arena_jakeball") resolves through
// ProjectMapping.locationsToProject -> ProjectID -> baked LevelDefinitions/{name}_a2level project
// (level.json objects + gamemode.luau). This dump tells us which slots exist, their gamemode path,
// and whether they're loaded (LoadedGameMode!=null) -- the template for spawning a "deathrun" slot.
// AModuleSlot offsets: bShouldLoadOnStart@0x2F8, SlotID@0x390(FString), VisibleName@0x3A0,
// DefaultGamemodePath@0x418(FString), LoadedGameMode@0x440(null=unloaded).
static bool      g_slotDumpDone = false;
static ULONGLONG g_lastSlotDump = 0;
static int       g_slotDumpTries = 0;
static void DumpModuleSlots()
{
    auto rdFStr = [](uintptr_t a) -> const wchar_t* { auto d = *reinterpret_cast<const wchar_t**>(a); return d ? d : L""; };
    auto* cls = SDK::UObject::FindClassFast("ModuleSlot");
    if (!cls) { printf("[HalcyonA2][SLOTS] ModuleSlot class not found yet\n"); return; }
    auto* gmCls = SDK::UObject::FindClassFast("GamemodesManager");
    void* gmMgr = nullptr;
    int count = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        if (gmCls && !gmMgr && o->IsA(gmCls)) gmMgr = o;
        if (!o->IsA(cls)) continue;
        uintptr_t p = reinterpret_cast<uintptr_t>(o);
        double loc[3] = {};   // K2_GetActorLocation -> FVector (world position of the slot)
        if (auto* gl = o->Class ? o->Class->GetFunction("Actor", "K2_GetActorLocation") : nullptr)
            SafeProcessEvent(o, gl, loc);
        printf("[HalcyonA2][SLOTS] %s (%s) SlotID='%ls' Path='%ls' loaded=%p pos=(%.0f, %.0f, %.0f)\n",
               o->GetName().c_str(),
               o->Class ? o->Class->GetName().c_str() : "?",
               rdFStr(p + 0x390), rdFStr(p + 0x418),
               *reinterpret_cast<void**>(p + 0x440),
               loc[0], loc[1], loc[2]);
        ++count;
    }
    printf("[HalcyonA2][SLOTS] total module slots = %d ; GamemodesManager = %p\n", count, gmMgr);
}
static void SafeDumpModuleSlots() { __try { DumpModuleSlots(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Find the Scraprun module-slot marker(s) at runtime and print their WORLD position. The scraprun
// slot was "removed for perf" leaving a bare AActor marker ("ModuleSlotLocationBeforeItWasRemoved-
// ForPerf") in the streamed PKR_Scraprun level; its engine-computed world transform is the target
// for -GamemodePos (no fragile nested-transform math). Match bare AActor (exact class "Actor") whose
// full name contains "Scraprun".
static void DumpScraprunMarkers()
{
    int n = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num && n < 40; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->Class) continue;
        if (o->Class->GetName() != "Actor") continue;          // bare AActor only
        std::string fn = o->GetFullName();
        if (fn.find("Scraprun") == std::string::npos && fn.find("scraprun") == std::string::npos) continue;
        double loc[3] = {};
        if (auto* gl = o->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(o, gl, loc);
        printf("[HalcyonA2][MARKER] %s pos=(%.0f, %.0f, %.0f)\n", fn.c_str(), loc[0], loc[1], loc[2]);
        ++n;
    }
    printf("[HalcyonA2][MARKER] bare-Actor-in-Scraprun count=%d\n", n);
}
static void SafeDumpScraprunMarkers() { __try { DumpScraprunMarkers(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Ground-truth: after a deathrun load, dump the WORLD positions of the actors it spawned, so we can
// see EXACTLY where the coord rewrite landed the course (vs. the corridor marker -33850,11775,4000).
// PRECISE isolation: g_gmObjBase is GObjects->Num() captured just before AddSlot, so any object at an
// index >= that is one this load created — no keyword guessing, no other gamemodes' actors.
static int32_t g_gmObjBase = 0;
static void DumpDeathrunActors()
{
    // deathrun2-UNIQUE class tokens (none appear in golf/jakeball/tackleball). Index-range alone is
    // useless here — the whole world (~28k objects) streams in after AddSlot.
    static const char* kTok[] = { "BP_Trap_", "Deathrun", "GameStateManager", "ScoreboardA", "Cube_Shield" };
    int n = 0, actors = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
    printf("[HalcyonA2][DRPOS] scanning %d objects for deathrun2 actors (corridor marker = -33850,11775,4000)\n", num);
    for (int32_t i = 0; i < num && n < 40; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->Class) continue;
        std::string cn = o->Class->GetName();
        bool hit = false; for (auto* t : kTok) if (cn.find(t) != std::string::npos) { hit = true; break; }
        if (!hit) continue;
        auto* locFn = o->Class->GetFunction("Actor", "K2_GetActorLocation");
        if (!locFn) continue;
        ++actors;
        double loc[3] = {};
        SafeProcessEvent(o, locFn, loc);
        printf("[HalcyonA2][DRPOS] %s (%s) pos=(%.0f, %.0f, %.0f)\n",
               o->GetName().c_str(), cn.c_str(), loc[0], loc[1], loc[2]);
        ++n;
    }
    printf("[HalcyonA2][DRPOS] deathrun actor total=%d (shown %d) | corridor marker = -33850,11775,4000\n", actors, n);
}
static void SafeDumpDeathrunActors() { __try { DumpDeathrunActors(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Load a gamemode into a module slot (OPT-IN via env GS_LOAD_GAMEMODE, e.g. "deathrun";
// GS_GAMEMODE_SLOT selects the target slot, default "PKR_Custom_Full" = the Parkour custom slot).
// UGamemodesManager::AddSlot(mgr, slot) async-loads the slot's DefaultGamemodePath when it's set
// (slot+0x418.Num > 1) and we're the server (GetNetMode != NM_Client): it kicks sub_46A7750 which
// resolves the path via ProjectMapping -> loads LevelDefinitions/{name}_a2level (level.json objects
// + gamemode.luau) -> binds to the slot -> replicates. The empty custom/UGC slots (PKR_Custom_Full,
// Station_Full_1, ...) are registered at boot with an EMPTY path so nothing loads; we set the path
// then re-call AddSlot to trigger the load. AModuleSlot: SlotID@0x390, DefaultGamemodePath@0x418.
static char      g_loadGmPath[64] = {};
static char      g_loadGmSlot[64] = "PKR_Custom_Full";
static bool      g_gmPosSet = false;
static double    g_gmPos[3] = {};   // -GamemodePos=X,Y,Z : world position to move the slot to before load
static char      g_snapMarker[96] = {};   // -SnapToMarker=<fullname substr> : copy that actor's full transform to the slot
static bool      g_gmLoadDone = false;
static ULONGLONG g_lastGmLoad = 0;
static int       g_gmLoadTries = 0;
static void SetSlotFString(void* slotObj, uint32_t off, const wchar_t* val)
{
    int wl = static_cast<int>(wcslen(val)) + 1;
    FStringView pv{ const_cast<wchar_t*>(val), wl, wl };
    reinterpret_cast<void(__fastcall*)(void*, const void*)>(GetBase() + 0xFBCD70)(
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slotObj) + off), &pv);
}

// ---------------------------------------------------------------------------
// PLACEMENT via the object-builder hook (the proper fix — see below).
//
// A loaded gamemode's placed prefabs store their transform as LOCAL (slot-space) coords in a
// REPLICATED netvar: sub_46D77A0(RVA 0x46D77A0) is the per-ObjectPrefab netvar builder, called once
// per object during the async load with a3 = the deserialized prefab. It copies a3's transform
// VERBATIM into UVector/URotator netvars (position@a3+48 = FVector f64 x/y/z@48/56/64; rotation@a3+72
// = FRotator f64 pitch/yaw/roll@72/80/88 — NO slot transform applied server-side). The CLIENT applies
// the slot's world transform when it spawns each prefab, i.e. worldPos = slotWorld o localPos. So to
// relocate the whole course into the Scraprun corridor without moving the (Static, non-replicating)
// slot, we pre-multiply every object's local transform by a constant delta D = slotWorld^-1 o marker-
// World before it's copied into the replicated netvar. Then the client computes
// slotWorld o (D o local) = markerWorld o local  ->  course lands at the marker. Nothing to move,
// no NetVarSystem wiring, and the rewritten value replicates exactly like the original did.
//
// D is captured in LoadGamemodeIntoSlot() (from the target slot's + marker's live transforms) and the
// hook applies it while g_objRewrite is set. Scale is assumed 1 on both slot and marker (ignored).
struct HQuat { double x, y, z, w; };
struct HVec  { double x, y, z; };
static bool   g_objRewrite = false;   // set true just before AddSlot; gates the sub_46D77A0 rewrite
static bool   g_rewriteRot = false;   // also rewrite rotation (full-transform mode via -SnapToMarker)
static HQuat  g_dRot   = { 0,0,0,1 }; // per-object rotation delta B, as quat (for object spin rewrite)
static HVec   g_dTrans = { 0,0,0 };   // per-object translation C
static int    g_objRewriteCount = 0;
static bool   g_useMatPos = false;    // position via matrix g_Bmat (bypass quat round-trip)
static double g_Bmat[3][3] = { {1,0,0},{0,1,0},{0,0,1} };   // per-object position rotation (B)
static double g_gmNudge[3] = {};      // -GmNudge=X,Y,Z : world-space nudge of the authored origin
static bool   g_forceRot = false;     // -GmRewriteRot : also rewrite object rotation (couples into pos)

// PKR_Custom_Full's client-map intercept T1: the client places a loaded object at world = A*fed + T1,
// where fed is the value we write into the position netvar (a3+48) and A = UERot(pkrActorRot)^T. T1 was
// measured by regressing 40 deathrun2 actor positions (residual < 10u) from a known-fed run. A fixed
// slot property. Placement: worldWant(local) = Rs*local + P (Rs = marker applied rot, P = marker loc +
// nudge); feed fed = B*local + C, B = A^-1*Rs, C = A^-1*(P - T1). (Earlier bug: used the raw-local
// intercept Tp = T1 + A*Dold here, which put C off by Dold -> course in space on the wrong side.)
static const HVec kPkrTp = { -33590.0, 23550.0, 40314.0 };   // = T1 (fed-intercept), NOT Tp

static HQuat HRotToQuat(double pitch, double yaw, double roll)   // UE FRotator::Quaternion()
{
    const double H = 3.14159265358979323846 / 360.0;   // deg -> half-radian
    double SP = sin(pitch*H), CP = cos(pitch*H), SY = sin(yaw*H), CY = cos(yaw*H), SR = sin(roll*H), CR = cos(roll*H);
    HQuat q;
    q.x = CR*SP*SY - SR*CP*CY;
    q.y = -CR*SP*CY - SR*CP*SY;
    q.z = CR*CP*SY - SR*SP*CY;
    q.w = CR*CP*CY + SR*SP*SY;
    return q;
}
static void HQuatToRot(HQuat q, double& pitch, double& yaw, double& roll)   // UE FQuat::Rotator()
{
    const double RAD2DEG = 180.0 / 3.14159265358979323846;
    const double kSingThresh = 0.4999995;
    double sing = q.z*q.x - q.w*q.y;
    double yawY = 2.0*(q.w*q.z + q.x*q.y);
    double yawX = 1.0 - 2.0*(q.y*q.y + q.z*q.z);
    if (sing < -kSingThresh)      { pitch = -90.0; yaw = atan2(yawY, yawX)*RAD2DEG; roll = -yaw - 2.0*atan2(q.x, q.w)*RAD2DEG; }
    else if (sing > kSingThresh)  { pitch =  90.0; yaw = atan2(yawY, yawX)*RAD2DEG; roll =  yaw - 2.0*atan2(q.x, q.w)*RAD2DEG; }
    else                   { pitch = asin(2.0*sing)*RAD2DEG; yaw = atan2(yawY, yawX)*RAD2DEG;
                             roll = atan2(-2.0*(q.w*q.x + q.y*q.z), 1.0 - 2.0*(q.x*q.x + q.y*q.y))*RAD2DEG; }
}
static HQuat HQuatMul(HQuat a, HQuat b)   // a * b (UE order)
{
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
static HQuat HQuatConj(HQuat q) { return { -q.x, -q.y, -q.z, q.w }; }
static HVec  HQuatRotate(HQuat q, HVec v)   // v' = q * v * q^-1  (UE FQuat::RotateVector)
{
    HVec u = { q.x, q.y, q.z };
    HVec t = { 2.0*(u.y*v.z - u.z*v.y), 2.0*(u.z*v.x - u.x*v.z), 2.0*(u.x*v.y - u.y*v.x) };   // 2*cross(u,v)
    return { v.x + q.w*t.x + (u.y*t.z - u.z*t.y),
             v.y + q.w*t.y + (u.z*t.x - u.x*t.z),
             v.z + q.w*t.z + (u.x*t.y - u.y*t.x) };
}

// --- 3x3 matrix helpers (mirror solve2.ps1 exactly; quaternion path had a handedness bug) ---
struct HMat { double m[3][3]; };
static HMat HUERot(double P, double Y, double R)   // UE FRotationMatrix (row-vector convention), degrees
{
    const double d = 3.14159265358979323846 / 180.0;
    double sp=sin(P*d),cp=cos(P*d),sy=sin(Y*d),cy=cos(Y*d),sr=sin(R*d),cr=cos(R*d);
    HMat o;
    o.m[0][0]=cp*cy;            o.m[0][1]=cp*sy;            o.m[0][2]=sp;
    o.m[1][0]=sr*sp*cy-cr*sy;   o.m[1][1]=sr*sp*sy+cr*cy;   o.m[1][2]=-sr*cp;
    o.m[2][0]=-(cr*sp*cy+sr*sy);o.m[2][1]=cy*sr-cr*sp*sy;   o.m[2][2]=cr*cp;
    return o;
}
static HMat HTranspose(HMat a){ HMat o; for(int i=0;i<3;i++)for(int j=0;j<3;j++)o.m[i][j]=a.m[j][i]; return o; }
static HMat HMatMul(HMat a, HMat b){ HMat o; for(int i=0;i<3;i++)for(int j=0;j<3;j++){double s=0;for(int k=0;k<3;k++)s+=a.m[i][k]*b.m[k][j];o.m[i][j]=s;} return o; }
static HVec HMatVec(HMat a, HVec v){ return { a.m[0][0]*v.x+a.m[0][1]*v.y+a.m[0][2]*v.z,
                                              a.m[1][0]*v.x+a.m[1][1]*v.y+a.m[1][2]*v.z,
                                              a.m[2][0]*v.x+a.m[2][1]*v.y+a.m[2][2]*v.z }; }
static HQuat HMatToQuat(HMat mm)   // column-vector rotation matrix -> quat (x,y,z,w)
{
    double(*m)[3] = mm.m; double t = m[0][0]+m[1][1]+m[2][2], s, x, y, z, w;
    if (t > 0)                                   { s=sqrt(t+1.0)*2; w=0.25*s; x=(m[2][1]-m[1][2])/s; y=(m[0][2]-m[2][0])/s; z=(m[1][0]-m[0][1])/s; }
    else if (m[0][0]>m[1][1] && m[0][0]>m[2][2]) { s=sqrt(1.0+m[0][0]-m[1][1]-m[2][2])*2; w=(m[2][1]-m[1][2])/s; x=0.25*s; y=(m[0][1]+m[1][0])/s; z=(m[0][2]+m[2][0])/s; }
    else if (m[1][1]>m[2][2])                    { s=sqrt(1.0+m[1][1]-m[0][0]-m[2][2])*2; w=(m[0][2]-m[2][0])/s; x=(m[0][1]+m[1][0])/s; y=0.25*s; z=(m[1][2]+m[2][1])/s; }
    else                                         { s=sqrt(1.0+m[2][2]-m[0][0]-m[1][1])*2; w=(m[1][0]-m[0][1])/s; x=(m[0][2]+m[2][0])/s; y=(m[1][2]+m[2][1])/s; z=0.25*s; }
    return { x, y, z, w };
}

// sub_46D77A0(a1=new obj, a2, a3=deserialized prefab). Rewrite a3's local transform in place before
// the original copies it into the replicated netvars. One-shot server loads exactly one gamemode, so
// leaving g_objRewrite latched after our AddSlot is safe (nothing else builds prefab objects after).
static __int64(__fastcall* ObjBuild_Orig)(__int64, __int64, __int64) = nullptr;
static __int64 __fastcall ObjBuild_Hook(__int64 a1, __int64 a2, __int64 a3)
{
    __try
    {
        if (g_objRewrite && a3)
        {
            auto* pos = reinterpret_cast<double*>(a3 + 48);   // FVector {x,y,z}
            auto* rot = reinterpret_cast<double*>(a3 + 72);   // FRotator {pitch,yaw,roll}
            HVec  oldPos = { pos[0], pos[1], pos[2] };
            HQuat oldRot = HRotToQuat(rot[0], rot[1], rot[2]);
            HVec  nPos;
            if (g_useMatPos)   // position via matrix B (mirrors the offline solver exactly)
                nPos = { g_Bmat[0][0]*oldPos.x + g_Bmat[0][1]*oldPos.y + g_Bmat[0][2]*oldPos.z,
                         g_Bmat[1][0]*oldPos.x + g_Bmat[1][1]*oldPos.y + g_Bmat[1][2]*oldPos.z,
                         g_Bmat[2][0]*oldPos.x + g_Bmat[2][1]*oldPos.y + g_Bmat[2][2]*oldPos.z };
            else
                nPos = HQuatRotate(g_dRot, oldPos);   // identity when g_dRot is unit -> exact oldPos
            nPos.x += g_dTrans.x; nPos.y += g_dTrans.y; nPos.z += g_dTrans.z;   // B*local + C
            pos[0] = nPos.x; pos[1] = nPos.y; pos[2] = nPos.z;
            if (g_rewriteRot)   // leave rotation alone in translation-only mode (avoids round-trip drift)
            {
                HQuat nRot = HQuatMul(g_dRot, oldRot);
                double np, ny, nr; HQuatToRot(nRot, np, ny, nr);
                rot[0] = np; rot[1] = ny; rot[2] = nr;
            }
            (void)oldRot;
            ++g_objRewriteCount;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ObjBuild_Orig(a1, a2, a3);
}

static void LoadGamemodeIntoSlot()
{
    uint64_t base = GetBase();
    auto* slotCls = SDK::UObject::FindClassFast("ModuleSlot");
    auto* gmCls   = SDK::UObject::FindClassFast("GamemodesManager");
    if (!slotCls || !gmCls) { printf("[HalcyonA2][GM] classes not ready\n"); return; }
    wchar_t wpath[64] = {}; size_t cvt = 0; mbstowcs_s(&cvt, wpath, g_loadGmPath, _TRUNCATE);
    wchar_t wantSlot[64] = {}; mbstowcs_s(&cvt, wantSlot, g_loadGmSlot, _TRUNCATE);

    // Find the GamemodesManager + the target (real, fully-initialized) slot.
    SDK::UObject* mgr = nullptr; SDK::UObject* slot = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        if (!mgr && o->IsA(gmCls)) mgr = o;
        if (!slot && o->IsA(slotCls))
        {
            auto* sid = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(o) + 0x390);
            if (sid && wcsstr(sid, wantSlot)) slot = o;
        }
    }
    if (!mgr || !slot) { printf("[HalcyonA2][GM] mgr=%p slot('%s')=%p not ready yet\n", mgr, g_loadGmSlot, slot); return; }
    auto* addFn = mgr->Class->GetFunction("GamemodesManager", "AddSlot");
    if (!addFn) { printf("[HalcyonA2][GM] AddSlot UFunction not found\n"); return; }

    // Target world transform for the course. -SnapToMarker uses a marker actor's full loc+rot;
    // -GamemodePos is loc-only (rotation kept = slot's, so the course isn't re-rotated). We DON'T move
    // the slot (it's Static, non-replicating — moving it never worked). Instead capture D = slotWorld^-1
    // o markerWorld and arm the sub_46D77A0 hook, which rewrites each object's local coords so the
    // client's slotWorld o (D o local) lands at markerWorld o local. Skip if neither target was given
    // (loads at the slot's baked spot, old behavior).
    double tgtLoc[3] = {}, tgtRot[3] = {}; bool haveLoc = false, haveRot = false;
    if (g_snapMarker[0])
    {
        SDK::UObject* marker = nullptr;
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject() || !o->Class || o->Class->GetName() != "Actor") continue;
            if (o->GetFullName().find(g_snapMarker) == std::string::npos) continue;
            marker = o; break;
        }
        if (!marker) { printf("[HalcyonA2][GM] snap-to-marker '%s' NOT FOUND (waiting)\n", g_snapMarker); return; }
        if (auto* gl = marker->Class->GetFunction("Actor", "K2_GetActorLocation")) { SafeProcessEvent(marker, gl, tgtLoc); haveLoc = true; }
        if (auto* gr = marker->Class->GetFunction("Actor", "K2_GetActorRotation")) { SafeProcessEvent(marker, gr, tgtRot); haveRot = true; }
        printf("[HalcyonA2][GM] marker loc=(%.0f,%.0f,%.0f) rot=(P%.1f,Y%.1f,R%.1f)\n",
               tgtLoc[0], tgtLoc[1], tgtLoc[2], tgtRot[0], tgtRot[1], tgtRot[2]);
        // The PKR_Scraprun level-instance transform isn't always applied when we read: if the marker
        // is still at origin, it's not positioned yet — bail and retry (don't latch, don't snap to 0,0,0).
        if (fabs(tgtLoc[0]) < 1.0 && fabs(tgtLoc[1]) < 1.0 && fabs(tgtLoc[2]) < 1.0)
        { printf("[HalcyonA2][GM] marker not positioned yet (0,0,0) -- waiting\n"); return; }
    }
    else if (g_gmPosSet) { tgtLoc[0] = g_gmPos[0]; tgtLoc[1] = g_gmPos[1]; tgtLoc[2] = g_gmPos[2]; haveLoc = true; }

    if (haveLoc)
    {
        double slotLoc[3] = {}, slotRot[3] = {};
        if (auto* gl = slot->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(slot, gl, slotLoc);
        if (auto* gr = slot->Class->GetFunction("Actor", "K2_GetActorRotation")) SafeProcessEvent(slot, gr, slotRot);
        g_objRewriteCount = 0;

        if (haveRot)
        {
            // FULL transform (via -SnapToMarker): place the course as the removed scraprun slot would,
            // i.e. worldWant = Rs*local + P (Rs = marker's applied rotation, P = marker loc + nudge).
            // Client applies world = A*fed + Tp, A = UERot(pkrActorRot)^T (confirmed exactly). So feed
            // fed = B*local + C, B = A^-1*Rs = UERot(pkrRot)*UERot(markerRot)^T, C = A^-1*(P - kPkrTp).
            // Matrices (not quaternions) so this mirrors solve2.ps1, which correctly predicted the
            // corridor; only the final B is converted to a quat for the hook.
            //
            // The actor rotations are read live for logging only — K2_GetActorRotation is FLAKY (returns
            // (0,0,0) on some load-timing paths, same late-stream issue the marker loc had), which throws
            // the whole transform off. PKR_Custom_Full and the scraprun marker are fixed, so use the
            // known-good constants for the actual math.
            const double pkrRot[3]    = { -60.0, 180.0, 180.0 };   // PKR_Custom_Full actor rotation (fixed)
            const double markerRotC[3]= { -90.0, 180.0, 180.0 };   // scraprun marker actor rotation (fixed)
            printf("[HalcyonA2][GM] live-read rot (diagnostic): slot=(P%.1f,Y%.1f,R%.1f) marker=(P%.1f,Y%.1f,R%.1f)\n",
                   slotRot[0],slotRot[1],slotRot[2], tgtRot[0],tgtRot[1],tgtRot[2]);
            HMat Ainv = HUERot(pkrRot[0], pkrRot[1], pkrRot[2]);            // A^-1 = UERot(pkrRot)
            HMat Rs   = HTranspose(HUERot(markerRotC[0], markerRotC[1], markerRotC[2])); // scraprun applied rotation
            HMat B    = HMatMul(Ainv, Rs);
            for (int i=0;i<3;i++) for (int j=0;j<3;j++) g_Bmat[i][j] = B.m[i][j];
            g_useMatPos = true;
            g_dRot = HMatToQuat(B);
            HVec P  = { tgtLoc[0]+g_gmNudge[0], tgtLoc[1]+g_gmNudge[1], tgtLoc[2]+g_gmNudge[2] };
            HVec dW = { P.x - kPkrTp.x, P.y - kPkrTp.y, P.z - kPkrTp.z };
            g_dTrans = HMatVec(Ainv, dW);                                    // C
            // The client's spawn COUPLES object rotation into position (rewriting a3+72 moves the actor),
            // so rewriting rotation breaks the clean world=A*fedPos+Tp mapping and flings the course off.
            // With rotation left alone, fedPos=B*local+C yields world = Rs*local + marker -> the course
            // LAYOUT lands correctly in the corridor (each mesh spun ~30deg off, PKR slot rot vs corridor).
            // Opt into rotation rewrite only for experiments via -GmRewriteRot.
            g_rewriteRot = g_forceRot;
            printf("[HalcyonA2][GM] ARMED full-transform (baked pkrRot=-60,180,180 markerRot=-90,180,180) "
                   "P=(%.0f,%.0f,%.0f) nudge=(%.0f,%.0f,%.0f) rewriteRot=%d | B=(%.4f,%.4f,%.4f,%.4f) C=(%.0f,%.0f,%.0f)\n",
                   P.x,P.y,P.z, g_gmNudge[0],g_gmNudge[1],g_gmNudge[2], g_rewriteRot?1:0,
                   g_dRot.x,g_dRot.y,g_dRot.z,g_dRot.w, g_dTrans.x,g_dTrans.y,g_dTrans.z);
        }
        else
        {
            // Loc-only (-GamemodePos): translation shift, no rotation change.
            g_dRot = { 0,0,0,1 };
            g_rewriteRot = false;
            g_dTrans = { tgtLoc[0]-slotLoc[0]+g_gmNudge[0], tgtLoc[1]-slotLoc[1]+g_gmNudge[1], tgtLoc[2]-slotLoc[2]+g_gmNudge[2] };
            printf("[HalcyonA2][GM] ARMED translation-only: D.trans=(%.0f,%.0f,%.0f)\n",
                   g_dTrans.x,g_dTrans.y,g_dTrans.z);
        }
        g_objRewrite = true;
    }

    SetSlotFString(slot, 0x418, wpath);   // DefaultGamemodePath
    void* parms = slot;
    printf("[HalcyonA2][GM] loading '%s' into slot '%ls' via AddSlot\n",
           g_loadGmPath, *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(slot) + 0x390));
    g_gmObjBase = SDK::UObject::GObjects->Num();   // snapshot: everything after this index is ours
    SafeProcessEvent(mgr, addFn, &parms);
    g_gmLoadDone = true;
    printf("[HalcyonA2][GM] AddSlot fired for gamemode '%s'\n", g_loadGmPath);
}
static void SafeLoadGamemodeIntoSlot() { __try { LoadGamemodeIntoSlot(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[HalcyonA2][GM] load faulted (SEH)\n"); } }

// ---------------------------------------------------------------------------
// SLOT-SPAWN placement (-SpawnSlotAtMarker) — the correct approach. Coordinate-rewrite is dead (the
// client transform isn't affine). Instead spawn a REAL AModuleSlot at the marker (Movable -> its
// transform replicates) and initialize it the way a level-placed slot is, using the reflected
// UFunctions from the SDK (SandboxEngine.ModuleSlot): PushNetVars() builds the netvar container that
// a raw SpawnActor slot lacks (the sub_465A320 null-table crash). Then AddSlot loads deathrun2 into
// it and the client places every object at slotTransform*local = marker*local = the corridor, natively.
static bool g_spawnSlot = false;
static double g_slotRotAdj[3] = {};   // -SlotRot=P,Y,R : extra rotation added to the marker rotation
static SDK::UObject* g_spawnedSlot = nullptr;   // the runtime slot, for the post-load netvar push
static bool g_pushedAfterLoad = false;
// After the async load binds LoadedGameMode, push the slot's + gamemode's netvars so the state
// replicates to clients (a runtime slot never gets the client-side auto-load a level slot does, so
// the client only learns the gamemode via these netvar pushes). Fired from the ticker once bound.
static void PushSpawnedSlotNetVars()
{
    if (!g_spawnedSlot || g_pushedAfterLoad) return;
    uintptr_t s = reinterpret_cast<uintptr_t>(g_spawnedSlot);
    void* lgm = *reinterpret_cast<void**>(s + 0x440);   // AModuleSlot::LoadedGameMode
    if (!lgm) { printf("[HalcyonA2][SPAWN] waiting for LoadedGameMode to bind...\n"); return; }
    g_pushedAfterLoad = true;
    if (auto* fn = g_spawnedSlot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(g_spawnedSlot, fn, nullptr);
    auto* lgmO = reinterpret_cast<SDK::UObject*>(lgm);
    if (lgmO->Class)
        if (auto* fn = lgmO->Class->GetFunction("LoadedGameMode", "PushNetVars")) SafeProcessEvent(lgmO, fn, nullptr);
    if (auto* fnu = g_spawnedSlot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(g_spawnedSlot, fnu, nullptr);
    printf("[HalcyonA2][SPAWN] pushed slot+gamemode netvars post-load (LoadedGameMode=%p)\n", lgm);
}
static void SafePushSpawnedSlotNetVars() { __try { PushSpawnedSlotNetVars(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SpawnSlotAndLoad()
{
    uint64_t base = GetBase();
    auto* world = SDK::UWorld::GetWorld();
    if (!world) { printf("[HalcyonA2][SPAWN] no world\n"); return; }
    auto* gmCls = SDK::UObject::FindClassFast("GamemodesManager");
    // Use the importance-volume variant (like real gamemode slots). Its ImportanceVolume drives
    // A2's mobile LOD/streaming: when a local pawn overlaps it the client calls
    // LoadedGameMode->EnterImportanceVolume() which makes the meshes render. Plain BP_GamemodeSlot_C
    // has no volume, so on Android objects load as collision-only (invisible). Same "defaultslot"
    // CDO default, so the client match still holds.
    auto* slotBPCls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    if (!slotBPCls) slotBPCls = SDK::UObject::FindClassFast("BP_GamemodeSlot_C");
    if (!gmCls || !slotBPCls) { printf("[HalcyonA2][SPAWN] classes not ready (gm=%p slotBP=%p)\n", gmCls, slotBPCls); return; }

    // GamemodesManager instance
    SDK::UObject* mgr = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    { auto* o = SDK::UObject::GObjects->GetByIndex(i); if (o && !o->IsDefaultObject() && o->IsA(gmCls)) { mgr = o; break; } }
    if (!mgr) { printf("[HalcyonA2][SPAWN] GamemodesManager not ready\n"); return; }

    // Marker transform (live, guarded non-zero — level-instance applies it late)
    double mLoc[3] = {}, mRot[3] = {};
    { SDK::UObject* marker = nullptr;
      for (int32_t i = 0; i < num; ++i)
      { auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->Class || o->Class->GetName() != "Actor") continue;
        if (o->GetFullName().find(g_snapMarker) == std::string::npos) continue; marker = o; break; }
      if (!marker) { printf("[HalcyonA2][SPAWN] marker '%s' not found (waiting)\n", g_snapMarker); return; }
      if (auto* gl = marker->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(marker, gl, mLoc);
      if (auto* gr = marker->Class->GetFunction("Actor", "K2_GetActorRotation")) SafeProcessEvent(marker, gr, mRot);
      if (fabs(mLoc[0]) < 1.0 && fabs(mLoc[1]) < 1.0 && fabs(mLoc[2]) < 1.0)
      { printf("[HalcyonA2][SPAWN] marker not positioned yet -- waiting\n"); return; } }
    printf("[HalcyonA2][SPAWN] marker loc=(%.0f,%.0f,%.0f) rot=(P%.1f,Y%.1f,R%.1f)\n",
           mLoc[0],mLoc[1],mLoc[2], mRot[0],mRot[1],mRot[2]);

    // Build the spawn transform at the marker. Apply -SlotRot as a WORLD-space quaternion delta
    // (pre-multiply) rather than adding Euler angles: the marker is pitched -90 so Euler yaw/roll are
    // gimbal-locked (they collapse to the same axis). As a quat delta, -SlotRot Yaw = world Z spin,
    // Roll = world X, Pitch = world Y — all distinct.
    HQuat qMarker = HRotToQuat(mRot[0], mRot[1], mRot[2]);
    HQuat qDelta  = HRotToQuat(g_slotRotAdj[0], g_slotRotAdj[1], g_slotRotAdj[2]);
    HQuat q       = HQuatMul(qMarker, qDelta);   // LOCAL delta: -SlotRot Yaw spins the course about its OWN up
    printf("[HalcyonA2][SPAWN] marker rot=(P%.1f,Y%.1f,R%.1f) + world adj=(P%.0f,Y%.0f,R%.0f) -> quat=(%.3f,%.3f,%.3f,%.3f)\n",
           mRot[0],mRot[1],mRot[2], g_slotRotAdj[0],g_slotRotAdj[1],g_slotRotAdj[2], q.x,q.y,q.z,q.w);
    SDK::FTransform xform{};
    xform.Translation.X = mLoc[0]+g_gmNudge[0]; xform.Translation.Y = mLoc[1]+g_gmNudge[1]; xform.Translation.Z = mLoc[2]+g_gmNudge[2];
    printf("[HalcyonA2][SPAWN] spawn loc=(%.0f,%.0f,%.0f) (marker + nudge %.0f,%.0f,%.0f)\n",
           xform.Translation.X, xform.Translation.Y, xform.Translation.Z, g_gmNudge[0],g_gmNudge[1],g_gmNudge[2]);
    xform.Rotation.X = q.x; xform.Rotation.Y = q.y; xform.Rotation.Z = q.z; xform.Rotation.W = q.w;
    xform.Scale3D.X = 1.0; xform.Scale3D.Y = 1.0; xform.Scale3D.Z = 1.0;

    auto* slot = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        world, slotBPCls, xform, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!slot) { printf("[HalcyonA2][SPAWN] BeginDeferredActorSpawnFromClass returned null\n"); return; }
    uintptr_t s = reinterpret_cast<uintptr_t>(slot);
    g_spawnedSlot = slot; g_pushedAfterLoad = false;

    // Leave SlotID at the CDO default "defaultslot". SlotID does NOT replicate, so the client's copy
    // of this slot keeps the CDO default; if we rename it server-side the client can't match it to the
    // gamemode and never spawns the course (confirmed via client census). Keeping "defaultslot" on the
    // server means the manager keys deathrun2 under "defaultslot" and the client's "defaultslot" slot
    // matches -> loads the course at the corridor, exactly like a baked slot does.

    // NOTE: bAllObjectsVisibleAtAllTimes@0x2FC and the ImportanceVolume box extent are CLIENT-LOCAL,
    // non-replicated — the Android client reads its own slot copy's CDO defaults, so setting them
    // server-side does nothing (same wall as SlotID). The LOD pop-in on the long course is fixed
    // instead via the replicated DefaultLODSettings netvar — see FixDeathrunLOD().

    SDK::UGameplayStatics::FinishSpawningActor(slot, xform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    printf("[HalcyonA2][SPAWN] spawned slot 0x%llX (class %s)\n",
           (unsigned long long)slot, slot->Class ? slot->Class->GetName().c_str() : "?");

    // A runtime-spawned actor doesn't replicate to clients by default (a level-placed slot does). The
    // client needs the slot to appear so it can place the gamemode objects relative to it. Force it:
    // SetReplicates(true) + bAlwaysRelevant + bReplicateMovement + huge net-cull distance.
    if (auto* sr = slot->Class->GetFunction("Actor", "SetReplicates"))
    { struct { bool b; } rp{ true }; SafeProcessEvent(slot, sr, &rp); }
    *reinterpret_cast<uint8_t*>(s + 0x60) |= 0x08 | 0x10;                 // bAlwaysRelevant | bReplicateMovement
    *reinterpret_cast<float*>(s + 0x170) = 1.0e12f;                        // NetCullDistanceSquared (never cull)
    if (auto* fnu = slot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(slot, fnu, nullptr);

    // Initialize like a level-placed slot: push its netvars (the container a fresh spawn otherwise
    // lacks -> the loader's null-deref). Do NOT call UpdateSlotIDFromPosition/GenerateUniqueSlotID:
    // those rename SlotID, which the client can't match (SlotID doesn't replicate).
    if (auto* fn = slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(slot, fn, nullptr);

    printf("[HalcyonA2][SPAWN] init done: SlotID='%ls' ModuleState=%p LoadedGameMode=%p\n",
           *reinterpret_cast<const wchar_t**>(s + 0x390),
           *reinterpret_cast<void**>(s + 0x300), *reinterpret_cast<void**>(s + 0x440));

    // Load deathrun2 into it.
    wchar_t wpath[64] = {}; size_t cvt = 0; mbstowcs_s(&cvt, wpath, g_loadGmPath, _TRUNCATE);
    SetSlotFString(slot, 0x418, wpath);   // DefaultGamemodePath
    auto* addFn = mgr->Class->GetFunction("GamemodesManager", "AddSlot");
    if (!addFn) { printf("[HalcyonA2][SPAWN] AddSlot not found\n"); return; }
    void* parms = slot;
    g_gmObjBase = SDK::UObject::GObjects->Num();
    printf("[HalcyonA2][SPAWN] AddSlot('%s') on fresh slot...\n", g_loadGmPath);
    SafeProcessEvent(mgr, addFn, &parms);
    g_gmLoadDone = true;
    double sLoc[3] = {};
    if (auto* gl = slot->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(slot, gl, sLoc);
    uintptr_t m = reinterpret_cast<uintptr_t>(mgr);
    int32_t mgrLoadedGm = *reinterpret_cast<int32_t*>(m + 0x2B8 + 8);   // GamemodesManager.loadedGamemodes.Num
    auto* eng = *reinterpret_cast<void**>(m + 0x60);                    // GamemodesManager.SandboxEngine
    int32_t engLoadedGm = eng ? *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(eng) + 0x118 + 8) : -1;
    printf("[HalcyonA2][SPAWN] AddSlot fired | slot worldpos=(%.0f,%.0f,%.0f) RemoteRole=%d bRepBits=0x%02X "
           "LoadedGameMode=%p | mgr.loadedGamemodes=%d eng.loadedGamemodes=%d\n",
           sLoc[0], sLoc[1], sLoc[2],
           *reinterpret_cast<uint8_t*>(s + 0x68), *reinterpret_cast<uint8_t*>(s + 0x60),
           *reinterpret_cast<void**>(s + 0x440), mgrLoadedGm, engLoadedGm);
}
static void SafeSpawnSlotAndLoad() { __try { SpawnSlotAndLoad(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[HalcyonA2][SPAWN] faulted (SEH)\n"); } }

// Deathrun's death balls (BP_SmallDeathBall/BP_BigDeathBall) are broken in the client's cooked APK
// (missing package dependency -> broken mesh/material) and CRASH the Android render thread (SIGSEGV)
// the instant one is drawn. We can't re-cook the client, so suppress them server-side: null out any
// BallSpawnerComponent whose DiscClass (@0x498) is a death ball so SpawnBall no-ops (the engine just
// warns "no class specified"), and destroy any death balls that already spawned. Deathrun then plays
// minus the death-ball traps. Runs periodically (spawners are (re)created on gamemode load).
static char g_ballClass[96] = {};   // -BallClass=<ClassName> : replacement for broken death balls (else suppress)
static int  g_deathBallsSuppressed = 0;
static void SuppressDeathBalls()
{
    auto* spawnerCls = SDK::UObject::FindClassFast("BallSpawnerComponent");
    if (!spawnerCls) return;
    // Optional replacement class (a ball the client CAN render, pulled from the OBB paks). If it
    // resolves we swap DiscClass to it (keeps the trap producing a real, renderable ball); otherwise
    // we null DiscClass so SpawnBall no-ops.
    SDK::UClass* repl = g_ballClass[0] ? SDK::UObject::FindClassFast(g_ballClass) : nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(spawnerCls)) continue;
        auto** discCls = reinterpret_cast<SDK::UClass**>(reinterpret_cast<uintptr_t>(o) + 0x498);   // DiscClass
        if (*discCls && (*discCls)->GetName().find("DeathBall") != std::string::npos)
        {
            printf("[HalcyonA2][NODB] spawner %s: DiscClass=%s -> %s\n",
                   o->GetName().c_str(), (*discCls)->GetName().c_str(), repl ? repl->GetName().c_str() : "(null/suppressed)");
            *discCls = repl;   // nullptr = suppress, else the safe replacement ball
            ++g_deathBallsSuppressed;
        }
    }
    // Destroy any broken death balls already in the world (they'd crash a client that renders them).
    auto destroyClass = [&](const char* cn)
    {
        auto* c = SDK::UObject::FindClassFast(cn);
        if (!c) return;
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject() || !o->IsA(c)) continue;
            if (auto* fn = o->Class->GetFunction("Actor", "K2_DestroyActor")) SafeProcessEvent(o, fn, nullptr);
        }
    };
    destroyClass("BP_SmallDeathBall_C");
    destroyClass("BP_BigDeathBall_C");
}
static void SafeSuppressDeathBalls() { __try { SuppressDeathBalls(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ANDROID LOD POP-IN FIX. deathrun2's project carries a replicated `DefaultLODSettings`
// (type NetworkedRawData in level.json settings) = the ONE server->client lever for object
// visibility (bAllObjectsVisibleAtAllTimes + the ImportanceVolume box are client-local and
// non-replicated, so setting them does nothing — that was the failed fix).
//
// Triangulated across all shipped levels, the blob is a serialized FPrefabLODSettings:
//   * open courses (club_golf, quests) = 37 bytes: 0x01 + zeros = NO importance-volume hide,
//     everything always visible.
//   * deathrun2 == arena_jakeball == TackleballTraining = 45 bytes: 0x01 + 28 zeros + four
//     trailing int32=1 = ShowImportanceVolume/HideOutsideImportanceVolume ON = hide objects
//     outside the box. Jakeball/tackleball fit their box so don't pop; deathrun's long corridor
//     exceeds it -> far traps hide = the pop-in.
//
// Fix = turn deathrun2's blob into the golf "no-hide" form by zeroing the four trailing int32=1.
// The value lives in a replicated TArray<uint8>; we can't reach it by a typed netvar API (no
// raw-data UNetVar class in the SDK), so locate it by its exact 45-byte signature (a TArray
// {ptr,Num=45,Max} whose buffer matches) among the loaded objects and patch it in place. The
// signature is exact enough that false positives are impossible.
static bool g_fixLod = true;          // -NoFixLOD disables
static int  g_lodPatched = 0;
static bool LODSigMatches(const uint8_t* b)
{
    // 0x01, then 28 zero bytes, then 01 00 00 00 x4
    if (b[0] != 0x01) return false;
    for (int i = 1; i <= 28; ++i) if (b[i] != 0x00) return false;
    for (int g = 0; g < 4; ++g)
    {
        const uint8_t* w = b + 29 + g * 4;
        if (w[0] != 0x01 || w[1] || w[2] || w[3]) return false;
    }
    return true;
}
// Raw, SEH-safe scan of one object's member region for the DefaultLODSettings TArray<uint8>
// {void* Data; int32 Num=45; int32 Max}. Patches every match in place (zeros the trailing flags)
// and returns how many it patched. No C++ objects here (SEH requires no unwinding).
static int ScanObjForLOD(uintptr_t base, uintptr_t* firstOff)
{
    int hits = 0;
    __try
    {
        for (uintptr_t off = 0; off <= 0x800; off += 8)
        {
            int32_t n = *reinterpret_cast<int32_t*>(base + off + 8);
            if (n != 45) continue;                                   // cheap pre-filter (in-object read)
            int32_t mx = *reinterpret_cast<int32_t*>(base + off + 12);
            if (mx < 45 || mx > 0x10000) continue;
            auto* data = *reinterpret_cast<uint8_t**>(base + off);
            uintptr_t dp = reinterpret_cast<uintptr_t>(data);
            if (dp < 0x10000 || dp > 0x7FFFFFFFFFFFULL || (dp & 1)) continue;
            if (!LODSigMatches(data)) continue;
            // Match: zero the four trailing int32=1 -> golf "no-hide" profile.
            DWORD oldp;
            VirtualProtect(data + 29, 16, PAGE_READWRITE, &oldp);
            for (int k = 29; k < 45; ++k) data[k] = 0;
            VirtualProtect(data + 29, 16, oldp, &oldp);
            if (hits == 0 && firstOff) *firstOff = off;
            ++hits;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* object smaller than scan window / bad read — skip */ }
    return hits;
}
static void FixDeathrunLOD()
{
    if (!g_fixLod) return;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o) continue;
        uintptr_t off = 0;
        int hits = ScanObjForLOD(reinterpret_cast<uintptr_t>(o), &off);   // raw scan/patch (SEH)
        if (hits > 0)
        {
            g_lodPatched += hits;   // logging (std::string) OUTSIDE the SEH scope
            printf("[HalcyonA2][LOD] patched DefaultLODSettings on %s (+0x%llX, %d) -> no-hide (all objects visible)\n",
                   o->GetName().c_str(), (unsigned long long)off, hits);
        }
    }
}
static void SafeFixDeathrunLOD() { __try { FixDeathrunLOD(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// The blob scan above only ever found a TRANSIENT copy (SacrificialUClass) — the live replicated
// value isn't a bare 45-byte TArray at runtime. The effective per-object LOD is the FPrefabLODSettings
// on each placed object: UPrefabComponent.Settings@0x258 -> LODSettings@0xF8 (and the shared
// prefabDefinition@0x278 -> Settings -> LODSettings). Settings is an InstancedReference/
// PersistentInstance subobject of the server-spawned (replicating) prefab actor, so patching it +
// ForceNetUpdate is the direct, testable lever. Force each to "always visible, no distance/volume
// hide" = the golf profile. FPrefabLODSettings (SandboxEngine_structs): VisibleNever@0,
// VisibleAlways@1, MaxDrawDistance@4, MinDrawDistance@8, VisibleWhenPlaying@0xC,
// ShowDistanceToGamemode@0x10, HideDistanceToGamemode@0x14, ShowImportanceVolume@0x18,
// HideOutsideImportanceVolume@0x19.
static int g_lodObjPatched = 0;
static void ApplyAlwaysVisible(uintptr_t lod)   // lod = &FPrefabLODSettings
{
    *reinterpret_cast<uint8_t*>(lod + 0x00) = 0;           // VisibleNever = false
    *reinterpret_cast<uint8_t*>(lod + 0x01) = 1;           // VisibleAlways = true
    *reinterpret_cast<float*>  (lod + 0x04) = 1.0e12f;     // MaxDrawDistance = effectively infinite
    *reinterpret_cast<float*>  (lod + 0x08) = 0.0f;        // MinDrawDistance = 0
    *reinterpret_cast<float*>  (lod + 0x10) = 1.0e12f;     // ShowDistanceToGamemode = always
    *reinterpret_cast<float*>  (lod + 0x14) = 1.0e12f;     // HideDistanceToGamemode = never
    *reinterpret_cast<uint8_t*>(lod + 0x18) = 0;           // ShowImportanceVolume = false
    *reinterpret_cast<uint8_t*>(lod + 0x19) = 0;           // HideOutsideImportanceVolume = false
}
static void ForceTrapLOD()
{
    if (!g_fixLod) return;
    auto* prefabCls = SDK::UObject::FindClassFast("PrefabComponent");
    if (!prefabCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(prefabCls)) continue;
        uintptr_t pc = reinterpret_cast<uintptr_t>(o);
        // per-instance Settings@0x258 -> LODSettings@0xF8
        if (auto* settings = *reinterpret_cast<void**>(pc + 0x258))
        { ApplyAlwaysVisible(reinterpret_cast<uintptr_t>(settings) + 0xF8); ++g_lodObjPatched; }
        // shared definition@0x278 -> Settings@0x38 -> LODSettings@0xF8
        if (auto* def = *reinterpret_cast<void**>(pc + 0x278))
            if (auto* dset = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(def) + 0x38))
                ApplyAlwaysVisible(reinterpret_cast<uintptr_t>(dset) + 0xF8);
        // re-replicate the owning prefab actor so the change goes out
        if (auto* actor = *reinterpret_cast<SDK::UObject**>(pc + 0x3E8))
            if (auto* fnu = actor->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(actor, fnu, nullptr);
    }
}
static void SafeForceTrapLOD() { __try { ForceTrapLOD(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ★ THE ACTUAL FIX. sub_46B5E70 (project deserializer) creates the replicated "DefaultLODSettings"
// as a RawData netvar (vtable off_7FE6540 @ RVA 0x7FE6540, allocated by sub_531F860(80) — NOT a
// UObject, so it never appeared in the GObjects scan). Layout: TArray<uint8> Data@0x38, Num@0x40.
// It calls sub_46A0DF0(netvar) once, right after the bytes are memcpy'd in — so hooking that gives
// us the netvar with its bytes in place, BEFORE it replicates. We filter for the DefaultLODSettings
// blob (vtable match + Num==45 + exact signature) and zero the trailing hide-flags -> golf "no-hide"
// (all objects always visible), so the client receives the no-hide profile from the first send.
using NetVarReg_t = __int64(__fastcall*)(void*);
static NetVarReg_t g_NetVarReg_Orig = nullptr;
static int g_lodNetvarPatched = 0;
static __int64 __fastcall NetVarReg_Hook(void* netvar)
{
    int didPatch = 0;
    if (g_fixLod && netvar)
    {
        __try
        {
            uintptr_t vt = *reinterpret_cast<uintptr_t*>(netvar);
            if (vt == GetBase() + 0x7FE6540)                                   // RawData netvar vtable
            {
                int num = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(netvar) + 0x40);
                auto* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(netvar) + 0x38);
                if (num == 45 && data && LODSigMatches(data))
                {
                    DWORD op; VirtualProtect(data + 29, 16, PAGE_READWRITE, &op);
                    for (int k = 29; k < 45; ++k) data[k] = 0;               // -> golf no-hide profile
                    VirtualProtect(data + 29, 16, op, &op);
                    didPatch = 1;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { didPatch = 0; }
    }
    if (didPatch) { ++g_lodNetvarPatched; printf("[HalcyonA2][LOD] rewrote DefaultLODSettings netvar -> no-hide (#%d)\n", g_lodNetvarPatched); }
    return g_NetVarReg_Orig(netvar);
}

// P1 — arena team colors. Each arena's TicketManager holds TeamColors[2] (Home/Away), which drives
// the arena decor (BP_GamemodeColorCoordinator reads TicketManager->TeamColors[i].Primary) AND the
// color admission stamps on a joining pawn. Offline the module slot's InitializeRandomColors() fills
// it from DT_CosmeticMaterialMetaData; on our headless server the TicketManager instead inits
// TeamColors to 2x the "NoPreference" gray (0xFF63697B = RGB 99,105,123) from the empty level.json
// TeamColors:{}, and calling InitializeRandomColors didn't stick (reset at TM init). So set the
// colors ourselves, directly on each TicketManager, using the exact DT palette (InitializeRandomColors
// draws rows 1..6 = these 6, skipping NoPreference[0] and Storm[7]). Deterministic 2-color pick per TM
// (stable across re-applies -> no flicker); re-apply whenever a TM shows empty/NoPreference so a late
// TM re-init can't leave it gray. FColor memory order is B,G,R,A.
struct HTeamColor { unsigned char pr, pg, pb, sr, sg, sb, logo; const wchar_t* name; };
static const HTeamColor kTeamPalette[6] = {
    {   0, 114, 255, 101,   0,  99, 1, L"Excelsior"    },
    { 216, 155,   0,  38,  38,  39, 2, L"RustRunners"  },
    { 221,  42,   0, 255, 202, 149, 3, L"BetelJuicers" },
    {   0,  31, 216, 199, 192,   0, 4, L"Crowns"       },
    {   0, 163,   0, 191, 186,   0, 5, L"Cobs"         },
    { 224,  73, 246,   0, 101, 134, 6, L"Cuttlefish"   },
};
static void FillFColor(void* fc, unsigned char r, unsigned char g, unsigned char b)
{
    auto* p = reinterpret_cast<unsigned char*>(fc);   // FColor = { B, G, R, A }
    p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
}
static bool SafeProcessEvent(SDK::UObject* o, SDK::UFunction* fn, void* parms)
{
    __try { o->ProcessEvent(fn, parms); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void InitTeamColors()
{
    auto* tmCls = SDK::UObject::FindClassFast("TicketManager");
    if (!tmCls) return;
    static SDK::UFunction* fnSet = nullptr;
    static SDK::UFunction* fnRep = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(tmCls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const int tcNum = *reinterpret_cast<int*>(p + 0x2F0);            // TeamColors.Num
        auto* tcData    = *reinterpret_cast<unsigned char**>(p + 0x2E8); // TeamColors.Data
        // already has real colors? (not empty, first entry isn't the NoPreference gray)
        const bool isDefault = tcNum < 2 || !tcData ||
            (tcData[2] == 99 && tcData[1] == 105 && tcData[0] == 123);
        if (!isDefault) continue;

        if (!fnSet) fnSet = o->Class->GetFunction("TicketManager", "SetTeamColors");
        if (!fnSet) return;

        // deterministic 2-of-6 pick keyed on the TM pointer (stable -> re-apply is idempotent)
        const size_t h  = reinterpret_cast<uintptr_t>(o) >> 4;
        const int    ia = static_cast<int>(h % 6);
        const int    ib = static_cast<int>((ia + 1 + (h / 6) % 5) % 6);
        const HTeamColor& A = kTeamPalette[ia];
        const HTeamColor& B = kTeamPalette[ib];

        struct { SDK::FTeamColor Home; SDK::FTeamColor Away; } parms{};
        FillFColor(&parms.Home.Primary,   A.pr, A.pg, A.pb);
        FillFColor(&parms.Home.Secondary, A.sr, A.sg, A.sb);
        parms.Home.TeamLogoIndex = A.logo;
        FillFColor(&parms.Away.Primary,   B.pr, B.pg, B.pb);
        FillFColor(&parms.Away.Secondary, B.sr, B.sg, B.sb);
        parms.Away.TeamLogoIndex = B.logo;
        // P2 — set the team NAME (FTeamColor.TeamName@0xC, FName). Without it the arena team-name
        // label + per-player roster resolve to "None" even with colors/logos set. Conv_StringToName
        // creates/resolves the FName (A2 FNames aren't encrypted).
        parms.Home.TeamName = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(A.name));
        parms.Away.TeamName = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(B.name));

        if (SafeProcessEvent(o, fnSet, &parms))
        {
            // Fire the RepNotify server-side so the local BP_GamemodeColorCoordinator repaints (the
            // server doesn't run its own OnReps; remote clients also get it via replication).
            if (!fnRep) fnRep = o->Class->GetFunction("TicketManager", "OnRep_TeamColors");
            if (fnRep) SafeProcessEvent(o, fnRep, nullptr);
            printf("[HalcyonA2][TEAMCOLOR] set %s home=%d away=%d\n", o->GetName().c_str(), ia, ib);
        }
    }
}
static void SafeInitTeamColors() { __try { InitTeamColors(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---- Tackleball/Driftball TRAINING: shooting & goalie practice ---------------
// Reimplements TackleballTraining_a2level/gamemode.luau (serverOnly -> never runs on our headless
// server), whose only job is: when a practice mode is enabled, spawn the next training ball at a
// random location and fire EventCompleteWithDiscArgument so the BP's server-side shooting/goalie
// logic (aim/velocity/scoring, all present + authority-gated) actually runs. Same "serverOnly luau
// conductor is missing" gap solved for jakeball goals. Kiosk = ABP_TackleballTraining_C:
//   ShootingTraining@0x350, GoalieTraining@0x358, Event@0x360, ActivePawn@0x430, ActiveBalls@0x438.
//   UShootingTrainingComponent.bIsEnabled@0x478, UGoalieTrainingComponent.bIsEnabled@0x498.
// Locations/rotations are the hardcoded arrays from gamemode.luau (FVector = 3 doubles).
struct TVec { double x, y, z; };
static const TVec kGoalieLocs[7] = {
    {2.998538,119.090794,207.935547}, {-281.232418,175.940410,207.935547},
    {-426.630364,289.948226,207.935547}, {-574.910145,478.843127,207.935547},
    {281.862065,176.994229,207.935547}, {427.270759,290.490197,207.935547},
    {576.825207,479.800985,207.935547},
};
static const TVec kShootLocs[7] = {
    {551.825222,1104.800985,157.935547}, {-550.999973,1104.801012,157.935547},
    {-648.174793,479.801015,132.935547}, {651.825207,479.800983,132.935547},
    {551.825220,1025.999960,81.560547}, {-551.825170,1025.999987,81.560547},
    {576.825207,479.800985,207.935547},
};
static const TVec kShootRots[7] = {
    {0,120,0}, {0,-60,0}, {0,-60,0}, {30,180,0}, {-30,180,0}, {0,-60,0}, {0,120,0},
};
static int g_trainSpawnCtr = 0;
static int g_trainDbg = 0;
static std::unordered_map<void*, ULONGLONG> g_trainLastSpawn;   // per-kiosk spawn cooldown
// DIAG: watch each spawned training ball for ~12s — did the native EventComplete handler fling it
// (GetDiscVelocity != 0) and does it actually MOVE server-side (RootComponent pos changes)?
//   vel==0 after spawn      -> the EventCompleteWithDisc handler never flung it (candidate A)
//   vel!=0 but pos static    -> flung but the free ball doesn't simulate server-side (candidate B, golf-class)
struct TrainWatch { ULONGLONG spawnedMs; SDK::FVector lastPos; };
static std::unordered_map<void*, TrainWatch> g_trainWatch;
static void TrainingTick()
{
    static SDK::UClass* kioskCls = nullptr;
    if (!kioskCls) kioskCls = SDK::UObject::FindClassFast("BP_TackleballTraining_C");
    if (!kioskCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
    const ULONGLONG now = GetTickCount64();

    // Report on watched balls (vel + moved) then drop stale ones.
    for (auto it = g_trainWatch.begin(); it != g_trainWatch.end(); )
    {
        auto* ballO = static_cast<SDK::UObject*>(it->first);
        auto* ballA = static_cast<SDK::AActor*>(it->first);
        SDK::FVector vel{}; SDK::FVector pos{};
        if (ballO->Class) { auto* fnV = ballO->Class->GetFunction("DiscEntity", "GetDiscVelocity");
                            if (fnV) SafeProcessEvent(ballO, fnV, &vel); }
        if (ballA->RootComponent) pos = ballA->RootComponent->K2_GetComponentLocation();
        const double dx = pos.X - it->second.lastPos.X, dy = pos.Y - it->second.lastPos.Y, dz = pos.Z - it->second.lastPos.Z;
        HxLog("[HalcyonA2][TRAINBALL] ball=%p vel=(%.0f,%.0f,%.0f) pos=(%.0f,%.0f,%.0f) moved=%.0f\n",
              it->first, vel.X, vel.Y, vel.Z, pos.X, pos.Y, pos.Z, sqrt(dx*dx+dy*dy+dz*dz));
        it->second.lastPos = pos;
        if (now - it->second.spawnedMs > 12000) it = g_trainWatch.erase(it); else ++it;
    }
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(kioskCls)) continue;
        const uintptr_t k = reinterpret_cast<uintptr_t>(o);
        void* shooting = *reinterpret_cast<void**>(k + 0x350);
        void* goalie   = *reinterpret_cast<void**>(k + 0x358);
        void* event    = *reinterpret_cast<void**>(k + 0x360);
        void* pawn     = *reinterpret_cast<void**>(k + 0x430);            // ActivePawn (may be client-only)
        const int ballsNum = *reinterpret_cast<int*>(k + 0x440);         // ActiveBalls.Num
        const bool sEnabled = shooting && *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(shooting) + 0x478);
        const bool gEnabled = goalie   && *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(goalie)   + 0x498);

        if (g_trainDbg < 80)
        {
            HxLog("[HalcyonA2][TRAIN] %s shoot=%d goalie=%d pawn=%p balls=%d\n",
                  o->GetName().c_str(), (int)sEnabled, (int)gEnabled, pawn, ballsNum);
            ++g_trainDbg;
        }

        if (!(sEnabled || gEnabled) || !event) continue;
        if (ballsNum > 0) continue;                       // ball in flight — wait for it to resolve
        if (now - g_trainLastSpawn[o] < 2000) continue;   // per-kiosk cooldown (avoid flooding)
        g_trainLastSpawn[o] = now;

        const int idx = (g_trainSpawnCtr++) % 7;
        const TVec loc = gEnabled ? kGoalieLocs[idx] : kShootLocs[idx];
        const TVec rot = gEnabled ? TVec{0,0,0}       : kShootRots[idx];
        auto* compO = reinterpret_cast<SDK::UObject*>(gEnabled ? goalie : shooting);
        if (!compO || !compO->Class) continue;

        // RequestTrainingBallAtLocation(FVector Location, FVector Rotation) -> ADiscEntity* (return
        // @0x30). Called via ProcessEvent — the SDK .cpp wrappers aren't linked in this project.
        static SDK::UFunction* fnShootReq  = nullptr;
        static SDK::UFunction* fnGoalieReq = nullptr;
        SDK::UFunction* fnReq = nullptr;
        if (gEnabled) { if (!fnGoalieReq) fnGoalieReq = compO->Class->GetFunction("GoalieTrainingComponent",  "RequestTrainingBallAtLocation"); fnReq = fnGoalieReq; }
        else          { if (!fnShootReq ) fnShootReq  = compO->Class->GetFunction("ShootingTrainingComponent", "RequestTrainingBallAtLocation"); fnReq = fnShootReq;  }
        if (!fnReq) continue;
        struct { double lx, ly, lz, rx, ry, rz; void* ret; } rp{};
        rp.lx = loc.x; rp.ly = loc.y; rp.lz = loc.z;
        rp.rx = rot.x; rp.ry = rot.y; rp.rz = rot.z;
        if (!SafeProcessEvent(compO, fnReq, &rp)) continue;
        void* ball = rp.ret;
        HxLog("[HalcyonA2][TRAIN] spawn %s idx=%d ball=%p on %s\n",
              gEnabled ? "goalie" : "shoot", idx, ball, o->GetName().c_str());
        if (!ball) continue;

        // Fire EventCompleteWithDiscArgument so the BP's server shooting/goalie logic runs.
        auto* evtO = reinterpret_cast<SDK::UObject*>(event);
        if (!evtO->Class) continue;
        static SDK::UFunction* fnEvtDone = nullptr;
        if (!fnEvtDone) fnEvtDone = evtO->Class->GetFunction("EventComponent", "EventCompleteWithDiscArgument");
        if (fnEvtDone) { struct { void* Disc; } ep{ ball }; SafeProcessEvent(evtO, fnEvtDone, &ep); }

        // Watch this ball to see if the native handler flings it + whether it moves server-side.
        SDK::FVector p0{};
        if (auto* ra = static_cast<SDK::AActor*>(ball)->RootComponent) p0 = ra->K2_GetComponentLocation();
        g_trainWatch[ball] = { now, p0 };
    }
}
static void SafeTrainingTick() { __try { TrainingTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// QUESTS — register each player with the server progression system. On our headless server a
// player's UA2PlayerQuestComponent never gets initialized (IsInitialized@0x140 stays 0) and is
// never inserted into UServerProgression.PlayerInstances, so quest completion/progress fails
// ("PlayerInstances does not contain Component") and the kiosk shows the placeholder subquest.
// Server_SetQuestProgressionAndInitializeQuests(FA2QuestProgression) = the register+init: it
// stores the progression, sets IsInitialized=1, and inserts the component into PlayerInstances
// (confirmed in sub_4680E50). It does NOT need a pre-loaded quest table to register. We call it
// with an EMPTY FA2QuestProgression (no saved progress) on each not-yet-initialized component.
// It's a NetServer RPC but our pawn has authority, so ProcessEvent runs the impl locally.
// The Server RPC wrapper (sub_46851C0) bails unless it can resolve the world + UServerProgression
// at call time, which is flaky on our server (real players stayed IsInitialized=0). So skip the
// RPC and call the register worker directly: sub_4680E50(UServerProgression, component,
// FA2QuestProgression*) unconditionally sets IsInitialized=1 and inserts the component into
// PlayerInstances@0x2B0. We resolve UServerProgression ourselves and pass an empty progression.
static void SafeRegisterQuest(void* sp, void* comp, void* prog)
{
    __try { reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(GetBase() + 0x4680E50)(sp, comp, prog); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// TEMP PROOF: seed the register with drychee's 3 real completed quests (decoded from the Mothership
// player_quests blob) instead of an empty progression, so registering doesn't CLOBBER the client's
// saved progress. Hardcoded for now — generalize to a per-player Mothership fetch once this proves
// the seed fixes the clobber + lights the kiosk. FA2QuestProgression{LastUpdate@0, Quests TArray@0x8,
// Version@0x18}; FA2QuestStorage(0x20){FGuid ID@0, uint8 Progress@0x10, int CompletedVersion@0x14,
// FDateTime CompletedTime@0x18}. UE struct-assign (the register's copy) deep-copies the TArray, so a
// static source buffer is safe + reusable across players.
static unsigned char g_seedQuests[3 * 0x20];
static unsigned char g_seedProg[0x20];
static bool g_seedBuilt = false;
static void ParseGuid32(const char* h, unsigned char* out16)
{
    auto hx = [](char c) -> unsigned { c |= 0x20; return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0; };
    for (int g = 0; g < 4; ++g)
    {
        unsigned v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 4) | hx(h[g * 8 + k]);
        *reinterpret_cast<unsigned*>(out16 + g * 4) = v;   // FGuid A,B,C,D as uint32
    }
}
static void BuildSeedProgression()
{
    static const char* kGuids[3] = {
        "7BB295444C8A86C232F1709A8FA6A34B",
        "418A44014B5FBF10071D49B7DA319A25",
        "59259FE944A3EEB3123D5EBA6DBEEB71",
    };
    memset(g_seedQuests, 0, sizeof(g_seedQuests));
    for (int i = 0; i < 3; ++i)
    {
        unsigned char* q = g_seedQuests + i * 0x20;
        ParseGuid32(kGuids[i], q + 0);               // FGuid ID
        q[0x10] = 255;                               // Progress
        *reinterpret_cast<int*>(q + 0x14) = 1;       // CompletedVersion
    }
    memset(g_seedProg, 0, sizeof(g_seedProg));
    *reinterpret_cast<void**>(g_seedProg + 0x8)  = g_seedQuests;   // Quests.Data
    *reinterpret_cast<int*>  (g_seedProg + 0x10) = 3;              // Quests.Num
    *reinterpret_cast<int*>  (g_seedProg + 0x14) = 3;              // Quests.Max
    *reinterpret_cast<int*>  (g_seedProg + 0x18) = 3;              // SavedQuestProgressionVersion
    g_seedBuilt = true;
}

// ---- REAL per-player progression, fetched from Mothership -------------------
// Feeding a player anything other than their real progression makes the client refuse to
// init the quest UI (it compares its own Mothership fetch against what the server feeds and
// bails on mismatch). So we fetch each joiner's real player_quests and register with THAT.
struct FetchedQuest { unsigned char guid[16]; unsigned char progress; int completedVersion; };

static std::mutex g_qMx;
static std::unordered_map<std::string, std::vector<FetchedQuest>> g_qReady;  // orgId -> quests
static std::unordered_set<std::string> g_qInflight;
static std::vector<std::string> g_qQueue;   // FIFO of ready orgIds not yet assigned to a component
                                            // (interim correlation; exact per-player mapping via the
                                            // FetchUserRoles caller is a TODO — fine for single-join)

static std::vector<FetchedQuest> ParseQuestsJson(const std::string& json)
{
    std::vector<FetchedQuest> out;
    size_t pos = 0;
    const std::string idKey = "\"iD\"";
    while (true)
    {
        size_t k = json.find(idKey, pos);
        if (k == std::string::npos) break;
        size_t colon = json.find(':', k + idKey.size());
        size_t s1 = (colon == std::string::npos) ? std::string::npos : json.find('"', colon);
        size_t s2 = (s1 == std::string::npos) ? std::string::npos : json.find('"', s1 + 1);
        if (s2 == std::string::npos) break;
        std::string idStr = json.substr(s1 + 1, s2 - s1 - 1);
        pos = s2 + 1;
        if (idStr.size() != 32) continue;
        FetchedQuest fq{};
        ParseGuid32(idStr.c_str(), fq.guid);
        fq.progress = (unsigned char)ExtractJsonInt(json, "progress", s2);
        fq.completedVersion = ExtractJsonInt(json, "completedVersion", s2);
        out.push_back(fq);
    }
    return out;
}

// Kick a one-shot background fetch for an org id (HTTP is blocking; never do it on the game thread).
static void KickQuestFetch(const std::string& orgId)
{
    if (orgId.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_qMx);
        if (g_qInflight.count(orgId) || g_qReady.count(orgId)) return;
        g_qInflight.insert(orgId);
    }
    std::thread([orgId]() {
        std::string json = FetchPlayerQuestsJson(orgId);
        std::vector<FetchedQuest> quests = ParseQuestsJson(json);
        {
            std::lock_guard<std::mutex> lk(g_qMx);
            g_qReady[orgId] = quests;
            g_qInflight.erase(orgId);
            g_qQueue.push_back(orgId);
        }
        HxLog("[HalcyonA2][QUEST] fetched %zu real quests for org %s\n", quests.size(), orgId.c_str());
    }).detach();
}

// Build an FA2QuestProgression in a reusable static buffer (rebuilt per call, so a move-assign in
// the register worker can't poison later registers).
static unsigned char g_realQBuf[4096 * 0x20];
static unsigned char g_realQProg[0x20];
static void* BuildProgFromFetched(const std::vector<FetchedQuest>& qs)
{
    int n = (int)qs.size();
    if (n > 4096) n = 4096;
    memset(g_realQBuf, 0, (size_t)n * 0x20);
    for (int i = 0; i < n; ++i)
    {
        unsigned char* q = g_realQBuf + (size_t)i * 0x20;
        memcpy(q + 0x00, qs[i].guid, 16);      // FGuid ID
        q[0x10] = qs[i].progress;              // Progress
        *reinterpret_cast<int*>(q + 0x14) = qs[i].completedVersion;
    }
    memset(g_realQProg, 0, sizeof(g_realQProg));
    *reinterpret_cast<void**>(g_realQProg + 0x08) = g_realQBuf;   // Quests.Data
    *reinterpret_cast<int*>  (g_realQProg + 0x10) = n;            // Num
    *reinterpret_cast<int*>  (g_realQProg + 0x14) = n;            // Max
    *reinterpret_cast<int*>  (g_realQProg + 0x18) = 3;            // SavedQuestProgressionVersion
    return g_realQProg;
}

// ===== Backend auth join-gate (Tier 1) =======================================================
// A connecting player must correspond to a live authed session in OUR backend, or we kick them.
// The backend (MothershipServer._activeSessions) marks a session active ONLY on a real client
// login (Quest attestation / RIFT); a pirate/standalone client that skipped our backend isn't
// active -> kicked. Design rules:
//   * FAIL-OPEN — any network/parse failure leaves the org "unknown" and never kicks (a backend
//     blip must not mass-kick legit players).
//   * REMOTE-ONLY — only controllers with a NetConnection and !bIsLocalPlayerController; never the
//     client-as-server's own host controller.
//   * LOG-ONLY until g_gateEnforce is set true, so we can watch the [GATE] lines confirm real
//     players resolve authorized=1 before any kick goes live.
// Kick primitive = sub_5333610(pc) (the controller's return-to-menu virtual; see IDA notes).
static bool g_gateEnforce = true;    // PROD: validated — legit players resolve authorized=1 within ~11s; a never-authed IP-join never populates org and is kicked after the grace window
static const ULONGLONG kGateGraceMs = 30000;   // a joining client gets this long to complete dashboard login + backend authorize before an empty org == kick (real logins land ~11s)
static ULONGLONG g_lastAuthGate = 0;
static std::mutex g_authMx;
static std::unordered_map<std::string, int> g_authState;   // org -> 0 unknown, 1 authorized, 2 unauthorized
static std::unordered_map<std::string, ULONGLONG> g_authCheckedAt;   // org -> last authorized-check completion (ms); lets a cached state==2 be re-checked (first-login race)
static std::unordered_set<std::string> g_authInflight;
static std::unordered_set<std::string> g_gateKicked;       // orgs already actioned (no repeat)
static std::unordered_map<void*, ULONGLONG> g_connFirstSeen;   // connection ptr -> first tick we saw it (grace clock)
static std::unordered_set<void*> g_connKicked;                 // connections already actioned (no repeat)

// Fire a one-shot background check of an org id against /v1/server/authorized (HTTP blocks; keep it
// off the game thread, same as KickQuestFetch). Leaves state 0 (unknown) on any failure = fail-open.
static void CheckAuthorized(const std::string& orgId)
{
    if (orgId.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_authMx);
        if (g_authInflight.count(orgId)) return;
        auto it = g_authState.find(orgId);
        if (it != g_authState.end() && it->second != 0) return;   // already resolved
        g_authInflight.insert(orgId);
    }
    std::thread([orgId]() {
        const std::wstring hdr = L"x-server-api-key: " + std::wstring(kServerApiKey) + L"\r\n";
        const std::wstring wid(orgId.begin(), orgId.end());   // org id is ASCII (Meta id / GUID)
        const std::wstring path = L"/v1/server/authorized?id=" + wid;
        DWORD code = 0;
        const std::string resp = HttpReq(kMotherHost, kMotherPort, L"GET", path.c_str(), "", hdr, &code);
        int state = 0;   // unknown -> fail-open, retried next pass
        if (code == 200)
        {
            if (resp.find("\"authorized\":true")  != std::string::npos ||
                resp.find("\"authorized\": true") != std::string::npos)  state = 1;
            else if (resp.find("\"authorized\":false")  != std::string::npos ||
                     resp.find("\"authorized\": false") != std::string::npos) state = 2;
        }
        {
            std::lock_guard<std::mutex> lk(g_authMx);
            g_authInflight.erase(orgId);
            if (state != 0) { g_authState[orgId] = state; g_authCheckedAt[orgId] = GetTickCount64(); }   // stamp so a cached 2 can be refreshed
        }
        HxLog("[HalcyonA2][GATE] authorized check org=%s http=%lu state=%d\n",
              orgId.c_str(), (unsigned long)code, state);
    }).detach();
}

// Turn up to N raw bytes into "aabbcc.." for identity-format discovery in the log.
static std::string HexBytes(const uint8_t* p, int n)
{
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (int i = 0; i < n && i < 64; ++i) { s.push_back(H[p[i] >> 4]); s.push_back(H[p[i] & 0xF]); }
    return s;
}

// Game-thread pass (~1s): enumerate the netdriver's ClientConnections — the AUTHORITATIVE list of
// connected clients (VR AND spectators, class-agnostic; conn+0x30 = PlayerController). Each remote
// client is gated on org@PC+0xA30, which is populated ONLY from the dashboard backend's validated
// login response (LoginWithKeyResponseHandler) — NOT the client's raw JWT — so it is not forgeable
// and a never-authed IP-join never gets one. Empirically (logs 2026-08-20): a legit Quest/RIFT player
// completes the dashboard login ~11s after connecting -> org populates -> /v1/server/authorized
// returns state=1; a client that opened the game, skipped auth, and joined by IP keeps org=='' the
// whole session (its dashboard login is rejected server-side). Decision, FAIL-CLOSED after a grace
// window:
//   org present + state==1 -> keep (authed)
//   org present + state==2 -> kick (id is not an authed backend session)
//   org empty  + age > kGateGraceMs -> kick (never authenticated through us — the IP-bypass case)
//   otherwise -> wait (within grace / backend still resolving) = fail-open, no false kicks.
// Kick primitive = sub_5333610(pc); once-per-connection via g_connKicked. Set g_gateDiscover to dump
// the raw connection id bytes for debugging.
static bool g_gateDiscover = false;  // optional per-connection id dump (org@0xA30 is the anchor; flip on only to debug)
// The dashboard/game player count comes from the EOS session we return, whose PublicPlayers list is
// only mutated by the CLIENT (join = POST, leave = DELETE). A HARD disconnect (quit/crash/network
// drop) never sends the leave, so ghosts accumulate and the count never drops. The AUTHORITY on who's
// actually connected is this gameserver's netdriver — ClientConnections.Num() drops the instant UE
// tears down a closed connection. So periodically report the real count to the backend, which
// reconciles the session's player count down to it. ~3-minute cadence (the count is not time-critical).
static void PlayerCountReportTick()
{
    if (g_deploymentId.empty()) return;   // not registered yet / manual launch without an id

    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->NetDriver) return;
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase) { imgBase = GetBase(); auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
                    auto* nth = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                    imgSize = nth->OptionalHeader.SizeOfImage; }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);
    if (vt < imgBase || vt >= imgBase + imgSize) return;          // not a real in-image netdriver yet
    int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8);    // ClientConnections.Num() (may be 0)
    if (numConns < 0) numConns = 0;

    // Fire-and-forget so a remote POST never stalls the game thread. Capture by value.
    const std::string dep  = g_deploymentId;
    const int         cnt  = numConns;
    std::thread([dep, cnt]() {
        std::string body = "{\"deployment_id\":\"" + dep + "\",\"player_count\":" + std::to_string(cnt) + "}";
        HttpPostLocal(kBackendHost, kBackendPort, L"/update_player_count", body);
    }).detach();
    printf("[HalcyonA2] player-count report: deployment=%s count=%d\n", g_deploymentId.c_str(), numConns);
}
static void SafePlayerCountReportTick() { __try { PlayerCountReportTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static void AuthGateTick()
{
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->NetDriver) return;
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    // same validity gate as TuneNetDriver: prove it's a real in-image UNetDriver before dereferencing.
    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase) { imgBase = GetBase(); auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
                    auto* nth = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                    imgSize = nth->OptionalHeader.SizeOfImage; }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);
    if (vt < imgBase || vt >= imgBase + imgSize) return;
    void** conns = *reinterpret_cast<void***>(nd + 0xD0);            // ClientConnections.Data
    const int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8); // ClientConnections.Num()
    if (!conns || numConns <= 0) return;

    const ULONGLONG nowMs = GetTickCount64();
    std::unordered_set<void*> live;   // connections seen this pass, for stale-entry cleanup

    // action a connection: kick (once) if enforcing, else log the would-kick. `why` names the reason.
    auto doKick = [&](void* conn, uintptr_t pc, const char* why, const std::string& org)
    {
        if (g_connKicked.count(conn)) return;   // already actioned this connection
        g_connKicked.insert(conn);
        if (g_gateEnforce)
        {
            HxLog("[HalcyonA2][GATE] KICK %s conn=%p pc=%p org='%s'\n", why, conn, (void*)pc, org.c_str());
            reinterpret_cast<void(__fastcall*)(void*)>(GetBase() + 0x5333610)(reinterpret_cast<void*>(pc));
        }
        else
            HxLog("[HalcyonA2][GATE] would-kick (log-only) %s conn=%p pc=%p org='%s'\n", why, conn, (void*)pc, org.c_str());
    };

    for (int32_t c = 0; c < numConns && c < 64; ++c)
    {
        const uintptr_t conn = reinterpret_cast<uintptr_t>(conns[c]);
        if (!conn) continue;
        const uintptr_t pc = *reinterpret_cast<uintptr_t*>(conn + 0x30);   // UNetConnection.PlayerController
        if (!pc) continue;
        if (*reinterpret_cast<bool*>(pc + 0x6C4)) continue;                // bIsLocalPlayerController -> host, never touch
        live.insert(reinterpret_cast<void*>(conn));

        // grace clock: first tick we saw this connection (covers VR + spectators identically).
        auto fs = g_connFirstSeen.find(reinterpret_cast<void*>(conn));
        if (fs == g_connFirstSeen.end()) fs = g_connFirstSeen.emplace(reinterpret_cast<void*>(conn), nowMs).first;
        const ULONGLONG ageMs = nowMs - fs->second;

        // org@PC+0xA30 is populated ONLY from the dashboard backend's validated login response — it is
        // not client-forgeable, and a never-authed IP-join never gets one. So it's the gate anchor.
        std::string org = FStringToNarrow(reinterpret_cast<void*>(pc + 0xA30));

        if (g_gateDiscover)   // optional raw-id dump (off by default; org@0xA30 is the real anchor)
        {
            uint8_t* rb   = *reinterpret_cast<uint8_t**>(conn + 0x160 + 0x20);    // PlayerId.ReplicationBytes.Data
            const int32_t rbn = *reinterpret_cast<int32_t*>(conn + 0x160 + 0x28); // .Num
            HxLog("[HalcyonA2][GATE-ID] conn=%p pc=%p org='%s' age=%llums replBytes(n=%d)=%s\n",
                  (void*)conn, (void*)pc, org.c_str(), (unsigned long long)ageMs, rbn,
                  (rb && rbn > 0) ? HexBytes(rb, rbn).c_str() : "<none>");
        }

        if (org.empty())
        {
            // legit clients populate org within ~11s of connecting (dashboard login round-trip). If it's
            // still empty past the grace window, this player never authenticated through us -> kick.
            if (ageMs > kGateGraceMs)
                doKick(reinterpret_cast<void*>(conn), pc, "no-auth-timeout", org);
            continue;                                            // otherwise still within grace -> wait
        }

        int state = 0;
        {
            std::lock_guard<std::mutex> lk(g_authMx);
            auto it = g_authState.find(org);
            if (it != g_authState.end()) state = it->second;
        }
        if (state == 1) continue;                                // authorized backend session -> keep
        if (state == 0) { CheckAuthorized(org); continue; }      // resolving -> fail-open, (re)check next pass
        // state == 2: NOT sticky. Mothership persists a first-login player's session slightly AFTER the
        // dashboard login that populated `org`, so our first check can race to `false`. Re-check on a
        // short TTL (so a now-authed player flips to state=1 and is kept) and only kick if STILL
        // unauthorized past the grace window — never on a single early false.
        {
            ULONGLONG lastChk = 0;
            { std::lock_guard<std::mutex> lk(g_authMx); auto ca = g_authCheckedAt.find(org); if (ca != g_authCheckedAt.end()) lastChk = ca->second; }
            if (nowMs - lastChk > 5000) CheckAuthorized(org);    // refresh the cached 2
        }
        if (ageMs > kGateGraceMs)
            doKick(reinterpret_cast<void*>(conn), pc, "unauthorized", org);   // still unauthorized after 30s + re-checks
    }

    // drop tracking for connections that have gone away (also lets a reused conn ptr re-arm cleanly).
    if (g_connFirstSeen.size() > live.size())
        for (auto it = g_connFirstSeen.begin(); it != g_connFirstSeen.end(); )
            if (!live.count(it->first)) { g_connKicked.erase(it->first); it = g_connFirstSeen.erase(it); }
            else ++it;
}
static void SafeAuthGateTick() { __try { AuthGateTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Hook A2Station__FetchUserRoles (RVA 0x541CB00): the game calls it for every joining player with
// a1 = that player's Oculus org-scoped id (FString). We use it to warm the quest fetch AND to kick
// off the backend authorized-check for the join gate.
using FetchRoles_t = __int64(__fastcall*)(__int64, __int64);
static FetchRoles_t FetchRoles_Orig = nullptr;
static __int64 __fastcall FetchRoles_Hook(__int64 a1, __int64 a2)
{
    std::string orgId = FStringToNarrow(reinterpret_cast<void*>(a1));
    if (!orgId.empty())
    {
        HxLog("[HalcyonA2][QUEST] roles fetch for org %s -> warming quest fetch\n", orgId.c_str());
        KickQuestFetch(orgId);
        CheckAuthorized(orgId);     // join-gate: resolve whether this player authed with our backend
    }
    return FetchRoles_Orig ? FetchRoles_Orig(a1, a2) : 0;
}
static constexpr uintptr_t FetchRoles_RVA = HX::FetchRoles_RVA;

static void InitPlayerQuests()
{
    static SDK::UClass* qcCls = nullptr;
    static SDK::UClass* spCls = nullptr;
    if (!qcCls) qcCls = SDK::UObject::FindClassFast("A2PlayerQuestComponent");
    if (!spCls) spCls = SDK::UObject::FindClassFast("ServerProgression");
    if (!qcCls || !spCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();

    // resolve the live UServerProgression subsystem
    void* sp = nullptr;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(spCls)) { sp = o; break; }
    }
    if (!sp) return;   // subsystem not up yet — retry next pass

    int seen = 0, registered = 0, skippedInit = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(qcCls)) continue;
        // skip template/placeholder components (owned by a class-default pawn)
        if (!o->Outer || o->Outer->IsDefaultObject()) continue;
        ++seen;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const unsigned char init0 = *reinterpret_cast<unsigned char*>(p + 0x140);
        if (init0) { ++skippedInit; continue; }   // already IsInitialized

        // Exact per-player correlation: read the org-scoped id straight off the player.
        //   component -> Outer(pawn) -> Controller(APawn+0x2D0) -> org id FString(VRPC+0xA30)
        // The org is stored there by AVRPlayerController::Server_LoginToStationDashboard -> sub_54AE2E0.
        // The local host's PC never gets an org stored, so it's naturally skipped (org == "").
        void* pawn = o->Outer;
        void* pc   = pawn ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + 0x2D0) : nullptr;
        std::string org = pc ? FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0xA30)) : "";
        HxLog("[HalcyonA2][QUEST] uninit comp %s pawn %s pc %s org '%s'\n",
              o->GetName().c_str(),
              pawn ? o->Outer->GetName().c_str() : "<null>",
              pc   ? reinterpret_cast<SDK::UObject*>(pc)->GetName().c_str() : "<null>",
              org.c_str());
        if (org.empty()) continue;   // dashboard login not applied yet (or local host) — wait/skip

        // Fetch this player's real progression once; register only when it's ready (feeding
        // wrong/empty data makes the client refuse to init the quest UI).
        std::vector<FetchedQuest> quests; bool ready = false;
        {
            std::lock_guard<std::mutex> lk(g_qMx);
            auto it = g_qReady.find(org);
            if (it != g_qReady.end()) { quests = it->second; ready = true; }
        }
        if (!ready) { KickQuestFetch(org); continue; }   // kick (once) then register next pass

        void* prog = BuildProgFromFetched(quests);
        SafeRegisterQuest(sp, o, prog);
        ++registered;
        HxLog("[HalcyonA2][QUEST] register %s (pc %s) with %zu REAL quests IsInitialized ->%d\n",
               o->GetName().c_str(),
               reinterpret_cast<SDK::UObject*>(pc)->GetName().c_str(), quests.size(),
               *reinterpret_cast<unsigned char*>(p + 0x140));
    }
    if (registered)
        HxLog("[HalcyonA2][QUEST] pass: %d qcomp(s), %d registered, %d already-init\n",
               seen, registered, skippedInit);
}
static void SafeInitPlayerQuests() { __try { InitPlayerQuests(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---- QUEST TRACE ----------------------------------------------------------
// Does the GAME itself call the quest-init/progress paths on join (just gated/failing), or never?
// Hook the impls + workers and log every call (with our own poking disabled, any hit = the game).
//   0x46851C0 UA2PlayerQuestComponent::Server_SetQuestProgressionAndInitializeQuests_Impl(comp, prog)
//   0x4685250 UA2PlayerQuestComponent::Server_SetQuests_Impl(comp, bundle, bool removing)
//   0x4680E50 register+init worker(sp, comp, prog)  [the guts of ...InitializeQuests]
//   0x4685C30 UServerProgression::SetProgress(sp, pawn, FGuid*, uint8)
using QInit_t = __int64(__fastcall*)(void*, void*);
using QSetQ_t = __int64(__fastcall*)(void*, void*, unsigned char);
using QReg_t  = __int64(__fastcall*)(void*, void*, void*);
using QSetP_t = __int64(__fastcall*)(void*, void*, void*, unsigned char);
static QInit_t QInit_Orig = nullptr;
static QSetQ_t QSetQ_Orig = nullptr;
static QReg_t  QReg_Orig  = nullptr;
static QSetP_t QSetP_Orig = nullptr;
static __int64 __fastcall QInit_Hook(void* comp, void* prog)
{
    printf("[HalcyonA2][QTRACE] Server_SetQuestProgressionAndInitializeQuests comp=%p prog=%p\n", comp, prog);
    return QInit_Orig(comp, prog);
}
static __int64 __fastcall QSetQ_Hook(void* comp, void* bundle, unsigned char rem)
{
    printf("[HalcyonA2][QTRACE] Server_SetQuests comp=%p bundle=%p removing=%d\n", comp, bundle, (int)rem);
    return QSetQ_Orig(comp, bundle, rem);
}
static __int64 __fastcall QReg_Hook(void* sp, void* comp, void* prog)
{
    printf("[HalcyonA2][QTRACE] RegisterWorker(4680E50) sp=%p comp=%p prog=%p\n", sp, comp, prog);
    return QReg_Orig(sp, comp, prog);
}
static __int64 __fastcall QSetP_Hook(void* sp, void* pawn, void* guid, unsigned char prog)
{
    printf("[HalcyonA2][QTRACE] ServerProgression::SetProgress sp=%p pawn=%p progress=%d\n", sp, pawn, (int)prog);
    return QSetP_Orig(sp, pawn, guid, prog);
}

// Texture-streaming thunk sub_54ABCC0(a1): this = *(a1+0x890); return (*this->vtable[0x600/8])(this).
// On -nullrhi that render/streaming resource is NULL => original null-derefs (the long-standing
// random crash). Guard it: skip when null, otherwise pass through.
using StreamThunk_t = __int64(__fastcall*)(__int64);
static StreamThunk_t StreamThunk_Orig = nullptr;
static __int64 __fastcall StreamThunk_Hook(__int64 a1)
{
    if (!a1 || *reinterpret_cast<void**>(a1 + 0x890) == nullptr) return 0;
    return StreamThunk_Orig(a1);
}

static void ProcessEvent_Hook(SDK::UObject* Context, SDK::UFunction* Function, void* Parms)
{
    // TEMP rollback-pipeline trace: the jakeball syncs via the rollback channel
    // (client Server_SubmitInputs/Server_HitProp -> server -> Client_SendServerSimResults
    // back to all clients). If our offline-wired BallSimManager never SENDS
    // Client_SendServerSimResults, clients never converge. Log the relevant RPCs (both
    // directions) so we can see exactly where the pipeline breaks. Capped; remove after.
    // NOTE: the per-ProcessEvent [GOAL] trace that lived here is DISABLED — it called
    // Function->GetName() (FName->string + heap alloc) on EVERY ProcessEvent (thousands/frame),
    // which taxed the game thread down to ~40fps with 185ms hitches. That made the rollback sim
    // burst-step and predict ~30 frames = the high MI + ball jumping. Never do per-PE string work.
    // (Goal scoring is geometric via DetectGoals; this trace isn't needed.)

    // Authoritative golf stroke count: NewHitEvent fires per hit (server-auth) but the game's own
    // StrokeCount++ is gated on the FROZEN GetActorLocation() so it sticks at 1. Recognize the event by
    // cached FName index (cheap integer compare — NEVER GetName() here) and run the count ourselves.
    if (g_nheIdx && Function && Function->Name.ComparisonIndex == g_nheIdx)
    {
        __try { GolfHitEvent(Context); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static thread_local bool inHook = false;
    if (!inHook)
    {
        inHook = true;

        // Keep the ball-sim results-send flag (mgr+0x412) pinned to 1 on EVERY ProcessEvent call —
        // the GAME advances the sim itself (our step loop is usually inert), and sub_543F2D0 only
        // emits Client_SendServerSimResults when this flag is set. Arming it only at ~90Hz can miss
        // game-driven steps -> clients get sparse confirmations -> they over-predict -> MI balloons
        // far past the real RTT (85ms ping was showing MI ~44). Pinning it here makes every step send.
        if (!g_quietSims && g_ballSimMgr)
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x412) = 1;

        // Populate the Vivox VOIP config on the live subsystem ASAP (before any client
        // requests voice tokens). One-shot, retries until the subsystem exists.
        if (!g_voipConfigDone)
            ApplyVoipConfigIfNeeded();

        // Station dashboard, step 1: seed the api key + run the native server login so the server
        // authenticates. Retry ~3s until the A2SessionSubsystem is live, then latch.
        if (g_trackerDone && !g_dashLoginDone && GetTickCount64() - g_lastDashLogin > 3000)
        {
            g_lastDashLogin = GetTickCount64();
            if (++g_dashLoginTries <= 10)
                SafeTriggerServerDashboardLogin();
        }

        // Station dashboard, step 2: fetch the deployment/station config ourselves (sub_54198E0 ->
        // /v1/deployments?include_station_config -> sub_5409770 applies -> boards). This uses the
        // cmdline -DashboardApiKey directly (sub_5413CC0) and does NOT depend on the login, so gate it
        // on the tracker being up (station loaded + netvar/HTTP plumbing ready), NOT on g_dashLoginDone
        // (which the login latch controls). Fire a few times ~3s apart, then latch.
        if (g_trackerDone && !g_deployFetchDone && GetTickCount64() - g_lastDeployFetch > 3000)
        {
            g_lastDeployFetch = GetTickCount64();
            SafeTriggerDeploymentFetch();
            if (++g_deployFetchTries >= 3) g_deployFetchDone = true;
        }

        // Diagnostic: dump module (gamemode) slots a few times ~5s apart (slots stream in with the
        // districts), then latch. Shows which gamemodes have slots + load state + confirms
        // GamemodesManager -- the template for spawning a "deathrun" slot.
        if (g_trackerDone && !g_slotDumpDone && GetTickCount64() - g_lastSlotDump > 5000)
        {
            g_lastSlotDump = GetTickCount64();
            SafeDumpModuleSlots();
            SafeDumpScraprunMarkers();
            if (++g_slotDumpTries >= 3) g_slotDumpDone = true;
        }

        // Opt-in (GS_LOAD_GAMEMODE): load the requested gamemode into its slot once the world +
        // slots are up. Retry ~5s apart until the manager + target slot are found, then latch.
        if (g_loadGmPath[0] && g_trackerDone && !g_gmLoadDone && GetTickCount64() - g_lastGmLoad > 5000)
        {
            g_lastGmLoad = GetTickCount64();
            if (g_spawnSlot) SafeSpawnSlotAndLoad();   // spawn a real slot at the marker + init it
            else             SafeLoadGamemodeIntoSlot();
            if (++g_gmLoadTries >= 30) g_gmLoadDone = true;   // wait out the marker's level-instance transform
        }
        // Spawn path: once the async load binds LoadedGameMode, push its netvars so clients render it.
        if (g_spawnSlot && g_gmLoadDone && !g_pushedAfterLoad && GetTickCount64() - g_lastGmLoad > 2000)
        {
            static ULONGLONG lastPush = 0;
            if (GetTickCount64() - lastPush > 2000) { lastPush = GetTickCount64(); SafePushSpawnedSlotNetVars(); }
        }
        // Suppress deathrun's death balls (they crash the Android render thread — broken client cook).
        if (g_spawnSlot && g_gmLoadDone)
        {
            static ULONGLONG lastNoDb = 0;
            if (GetTickCount64() - lastNoDb > 1000) { lastNoDb = GetTickCount64(); SafeSuppressDeathBalls(); }
        }
        // LOD/visibility fix is done at the source: the NetVarReg_Hook on sub_46A0DF0 rewrites the
        // replicated "DefaultLODSettings" netvar to golf's no-hide profile the moment it's created
        // during gamemode load (before it replicates). The per-object ForceTrapLOD/FixDeathrunLOD
        // passes proved ineffective (per-object Settings don't replicate; the raw blob only existed
        // on a transient) — no longer invoked.
        // CRITICAL: sub_46D77A0 builds EVERY gamemode's objects, so the rewrite must be armed ONLY for
        // deathrun2's load. Disarm as soon as the load settles (no new objects for 3s, or a 25s hard
        // cap) — otherwise later stream-ins of golf/jakeball/etc. get shifted by our delta too. Then
        // dump where deathrun2's actors actually landed (ground truth vs. the corridor marker).
        static int       g_lastRwCount = -1;
        static ULONGLONG g_lastRwChange = 0;
        static bool      g_rwDone = false;
        if (!g_rwDone && g_gmLoadDone && (g_objRewrite || g_spawnSlot))
        {
            ULONGLONG now = GetTickCount64();
            if (g_objRewriteCount != g_lastRwCount) { g_lastRwCount = g_objRewriteCount; g_lastRwChange = now; }
            // spawn path: no rewrite counter, just wait ~10s after load for actors to stream in.
            bool settled = g_objRewrite ? (g_objRewriteCount > 0 && now - g_lastRwChange > 3000)
                                        : (now - g_lastGmLoad > 10000);
            bool timeout = (now - g_lastGmLoad > 25000);
            if (settled || timeout)
            {
                if (g_objRewrite)
                {
                    g_objRewrite = false;   // DISARM before anything else can load through the hook
                    printf("[HalcyonA2][GM] placement hook rewrote %d object transform(s); DISARMED\n", g_objRewriteCount);
                }
                g_rwDone = true;
                SafeDumpDeathrunActors();
            }
        }

        // One-shot world setup: spawn the GamemodesTracker once Station_Prime + its
        // class are live.
        if (!g_trackerDone)
            SpawnGamemodesTrackerIfReady();

        // No ABallSimManager is spawned on our server, so the 70 balls never join a
        // sim. Spawn+wire one (once, latched).
        if (g_trackerDone && !g_ballSimDone)
            SpawnBallSimManagerIfNeeded();

        // Once the manager exists, reconcile/build sims from the player list ~1s so
        // connected VRPawns get a sim...
        if (g_ballSimMgr && GetTickCount64() - g_lastBallBuildTick > 1000)
        {
            g_lastBallBuildTick = GetTickCount64();
            SafeWireVRPawns();   // point every VRPawn@0x1A58 at our manager; updates g_vrPawnCount
            // Reconcile only on player-count change: continuous reconcile re-bases the sim
            // frame counter every second (MI ~= all inputs) and rubber-bands the ball to
            // spawn. Gating it gives MI=0 and no reset. (Hits register in NEITHER mode, so
            // reconcile is not the hit gate — the server-side disc->ball collision is.)
            if (g_vrPawnCount != g_lastReconcileCount)
            {
                g_lastReconcileCount = g_vrPawnCount;
                SafePumpBallSimBuild();
            }
        }
        // Step the sim ~90Hz (the only thing that moves the server ball actor).
        if (!g_quietSims && g_ballSimMgr && GetTickCount64() - g_lastBallStepTick >= 11)
        {
            g_lastBallStepTick = GetTickCount64();
            SafePumpBallSimStep();
        }
        // Re-run overlap detection on the discs (~20Hz) so a ball rolling into a
        // start trigger actually fires OnOverlapByDisc and the match begins.
        if (g_ballSimMgr && GetTickCount64() - g_lastBallOverlapTick >= 100)   // 10Hz (was 20Hz); cached lists
        {
            g_lastBallOverlapTick = GetTickCount64();
            SafePumpBallOverlaps();
        }
        // TEMP: census the ball-sim structure (jakeball vs heartball) a few times.
        if (g_ballSimMgr && g_ballDbg < 2 && GetTickCount64() - g_lastBallDbg > 1000)
        {
            g_lastBallDbg = GetTickCount64();
            SafeDumpBallStructure();
            ++g_ballDbg;
        }

        // Arena-admission dump ([ADM]/[SEAT]/[GOALDBG]) DISABLED — it scans all of GObjects + prints
        // ~30 console lines every 3s, which stutters the game thread (hurts rollback MI). Re-enable
        // (g_admDump<N) only when actively debugging admission.
        if (false && g_trackerDone && g_admDump < 40 && GetTickCount64() - g_lastAdmDump > 3000)
        {
            g_lastAdmDump = GetTickCount64();
            SafeDumpAdmissionState();
            SafeDumpSimSeats();
            if (g_admDump < 8) SafeDumpGoals();
            ++g_admDump;
        }

        // Keep goals armed (bGoalEnabled=0 on our server; re-arm after scoring) ~every 2s.
        if (g_trackerDone && GetTickCount64() - g_lastEnableGoals > 2000)
        {
            g_lastEnableGoals = GetTickCount64();
            SafeEnableGoals();
        }

        // NET-DRIVER TUNING DISABLED — proven a NO-OP: diagnostic showed NetServerMaxTickRate is
        // ALREADY 90 (curRate=90). The driver was never capped, so the non-arena snapping is NOT a
        // net-rate problem. A2 player pose rides the Mass "frequent data" system, not vanilla actor
        // replication -> the throttle is there. See a2-match-state-and-ball-physics.
        if (false && g_trackerDone && GetTickCount64() - g_lastNetTune > 2000)
        {
            g_lastNetTune = GetTickCount64();
            SafeTuneNetDriver();
        }

        // SNAP DIAGNOSIS probe — log each VRPawn's frequent-data server timestamp once/sec so we can
        // see if the pose stream's timestamps are advancing. Remove once the snap cause is found.
        if (g_trackerDone && GetTickCount64() - g_lastFreqProbe > 1000)
        {
            g_lastFreqProbe = GetTickCount64();
            SafeProbeFreqTimestamps();
        }

        // BALL-SYNC probe — log DiscEntity server positions that moved, ~5Hz, to see if hit balls
        // stream server-side or only jump (spawn->hit). Remove once the ball teleport cause is found.
        if (g_trackerDone && GetTickCount64() - g_lastBallPos > 200)
        {
            g_lastBallPos = GetTickCount64();
            SafeProbeBallPositions();
        }

        // SIM-WIRE probe — per-pawn disc-comp/ball/flag state + owned golf/heart balls, ~1s. Confirms
        // whether golf/heart balls are wired for reconcile (the sim fix). Read-only.
        if (g_trackerDone && GetTickCount64() - g_lastBallWire > 1000)
        {
            g_lastBallWire = GetTickCount64();
            SafeProbeBallWire();
        }

        // Populate each arena's TicketManager.TeamColors (2 random rows from
        // DT_CosmeticMaterialMetaData) via the module slot's own InitializeRandomColors — this
        // colors the arena decor AND makes admission color joining players (else all gray). Retry
        // ~2s; each slot is colored once, as its TicketManager comes online.
        if (g_trackerDone && GetTickCount64() - g_lastTeamColor > 2000)
        {
            g_lastTeamColor = GetTickCount64();
            SafeInitTeamColors();
        }

        // Register each player's quest component with the server progression system ~every 2s.
        // QTRACE confirmed: the game fires ServerProgression::SetProgress on its own but NEVER
        // the init RPCs (no orchestrator on our headless server), so SetProgress errors with
        // "PlayerInstances does not contain Component". We register the pawn ourselves; then the
        // game's own SetProgress completions land. (QTRACE hooks left in to watch the error vanish.)
        if (g_trackerDone && GetTickCount64() - g_lastQuestInit > 2000)
        {
            g_lastQuestInit = GetTickCount64();
            SafeInitPlayerQuests();
        }

        // Backend auth join-gate (~1s): kick remote players who didn't authenticate through our
        // backend. Log-only until g_gateEnforce is set. See AuthGateTick.
        if (g_trackerDone && GetTickCount64() - g_lastAuthGate > 1000)
        {
            g_lastAuthGate = GetTickCount64();
            SafeAuthGateTick();
            // Spectator client floods Server_ApplyData ~150/s, but the server only relays its RepData at
            // the pawn's net-update rate — pin it high + always-relevant so VR viewers get dense updates
            // (no interp on the spectator transform, so relay density is the only smoothing lever).
            SafeTuneSpectatorPawns();
        }

        // Report the real connected-player count to the backend every ~3 min so the EOS session's
        // count gets reconciled past client-missed leaves (hard disconnects). See PlayerCountReportTick.
        static ULONGLONG s_lastPlayerReport = 0;
        if (g_trackerDone && GetTickCount64() - s_lastPlayerReport > 180000)
        {
            s_lastPlayerReport = GetTickCount64();
            SafePlayerCountReportTick();
        }

        // Shooting/goalie practice conductor (Tackleball/Driftball). The serverOnly gamemode.luau
        // that spawns each training ball + kicks the BP shooting logic doesn't run headless, so we
        // do it ourselves. Was 500ms; a full GObjects walk 2Hz for a kiosk that only exists in the
        // training arena is wasted game-thread time in a jakeball match -> now 2s.
        if (g_trackerDone && GetTickCount64() - g_lastTraining > 2000)
        {
            g_lastTraining = GetTickCount64();
            SafeTrainingTick();
        }

        // Geometric goal detection -> drive the score (~10Hz).
        if (g_trackerDone && GetTickCount64() - g_lastDetectGoals > 100)
        {
            g_lastDetectGoals = GetTickCount64();
            SafeDetectGoals();
            SafeGolfSinkDetect();   // same 10Hz cadence: geometric golf-cup sink -> fire BallInCup
            SafeVolleyfallTick();   // geometric spleef-floor break -> fire the panel's ReceiveActorBeginOverlap
        }

        // Pump the physics-sync send so free-ball (heartball/grabbable) physics reaches
        // clients (~30Hz — the jakeball uses the rollback channel, not this; halved from
        // 66Hz to free game-thread time for the rollback step so it doesn't fall behind).
        if (!g_quietSims && g_trackerDone && GetTickCount64() - g_lastPhysTick >= 30)
        {
            g_lastPhysTick = GetTickCount64();
            SafePumpPhysicsSync();
        }

        // (SafeWatchBallOwnership diagnostic disabled — it walked GObjects + read positions
        // at 10Hz, stealing game-thread time from the rollback step. Re-enable if needed.)

        inHook = false;
    }
    ProcessEvent_Orig(Context, Function, Parms);
}

// UGameplayUtilityStatics::IsRunningSimulate(UWorld*) — the gate for A2's Mass
// physics processor (UA2PhysicsSyncProcessor). On our headless client-as-server it
// returns false, so the ball/disc Mass simulation never advances and everything
// floats. Native impl RVA (from IDA: "impl: 0x52DC906"). Force it true so the
// processor runs. Full replace — we never need the original result.
static constexpr uintptr_t IsRunningSimulate_RVA = HX::IsRunningSimulate_RVA;
static bool __fastcall IsRunningSimulate_Hook(void* /*World*/)
{
    return true;
}

// UA2NetworkUtilityBPFL::GetNetMode -> EA2NetMode. A2's OWN net-mode abstraction
// that gameplay/physics key off (EA2NetMode: Standalone=0, DedicatedServer=1,
// ListenServer=2, Client=3). Our client-as-server auto-listens, so this reports
// ListenServer/Standalone and every dedicated-server-gated path (incl. the Mass
// physics processor that moves the balls) is skipped. The exec thunk's core impl
// is sub_5473BC0 (from IDA). Force it to DedicatedServer(1).
static constexpr uintptr_t A2GetNetMode_RVA = HX::A2GetNetMode_RVA;
static char __fastcall A2GetNetMode_Hook(void* /*Context*/)
{
    return 1; // EA2NetMode::DedicatedServer
}

// UWorld::GetNetMode (found via UKismetSystemLibrary::IsDedicatedServer, which is
// `GetNetMode(World) == 1`). Force NM_DedicatedServer(1) so every net-mode query —
// including the ones during world init, before the net driver attaches (UE-174595)
// — reports a real dedicated server. Installed BEFORE open so it covers init.
static constexpr uintptr_t WorldGetNetMode_RVA = HX::WorldGetNetMode_RVA;
static char __fastcall WorldGetNetMode_Hook(void* /*World*/)
{
    return 1; // NM_DedicatedServer
}

// sub_3500950 — the BallSimManager's guarded GetWorld. It returns null for our hand-
// spawned manager (a world-resolution guard fails), so the ball sim can't determine
// net mode (logs "IsServer: 0") or resolve players into the sim -> it builds a local,
// non-authoritative sim on every machine -> the jakeball never converges between
// clients (each runs its own). Fix: when this returns null for a BallSimManager, hand
// back the real UWorld so GetNetMode (our hook) reports DedicatedServer and the sim goes
// authoritative. Gated on the null-return + class so it never affects other callers.
static constexpr uintptr_t BallSimGetWorld_RVA = HX::BallSimGetWorld_RVA;
using BallSimGetWorld_t = __int64 (__fastcall*)(void*);
static BallSimGetWorld_t BallSimGetWorld_Orig = nullptr;
static __int64 __fastcall BallSimGetWorld_Hook(void* a1)
{
    __int64 r = BallSimGetWorld_Orig(a1);
    if (r == 0 && a1)
    {
        static SDK::UClass* mgrCls = nullptr;
        if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
        if (mgrCls && static_cast<SDK::UObject*>(a1)->IsA(mgrCls))
            return reinterpret_cast<__int64>(SDK::UWorld::GetWorld());
    }
    return r;
}

// UGameplayStatics::GetPlayerPawn_Implementation (RVA 0x3D1C150). Reads the controller's Pawn at
// a1+0x2D8 with NO null-check on a1. After we drop the server's local player (LocalPlayers.Remove(0)),
// the local-player resolvers (e.g. sub_40329F0) return null, so client-side per-frame ticks that do
// GetPlayerPawn(localController) deref null+0x2D8 and crash. Every such caller checks the RESULT for
// null (e.g. sub_53B7340: `if (result != 0)`), so returning 0 on a null controller makes them cleanly
// skip — a targeted guard, not a NOP (remote players' pawns still resolve normally).
static constexpr uintptr_t GetPlayerPawn_RVA = HX::GetPlayerPawn_RVA;
using GetPlayerPawn_t = __int64(__fastcall*)(void*);
static GetPlayerPawn_t GetPlayerPawn_Orig = nullptr;
static __int64 __fastcall GetPlayerPawn_Hook(void* a1)
{
    if (!a1) return 0;   // no local player -> null controller; skip instead of deref-crashing
    return GetPlayerPawn_Orig(a1);
}

// UKismetSystemLibrary::PrintString — the real native impl (the body the
// execPrintString exec thunk calls after unpacking the FFrame:
// sub_3A21120(WCO, InString, bPrintToScreen, bPrintToLog, &TextColor, Duration, Key)).
// Hooking the impl here catches EVERY PrintString — including the EX_CallMath fast-path
// calls that never route through ProcessEvent — so PrintDebugString's "Gamemode Tests:
// ..." narration (which calls PrintString internally) lands here too. rdx = the second
// arg = const FString& InString, so read Data@0x00 / Num@0x08 straight off it.
static constexpr uintptr_t PrintString_RVA = HX::PrintString_RVA;
using PrintString_t = void(__fastcall*)(void*, void*, bool, bool, void*, float, void*);
static PrintString_t PrintString_Orig = nullptr;
static void __fastcall PrintString_Hook(void* wco, void* inString, bool toScreen,
                                        bool toLog, void* color, float duration, void* key)
{
    PrintCapturedString(inString);
    PrintString_Orig(wco, inString, toScreen, toLog, color, duration, key);
}

// A2's rollback ball sim gates authority on `GetNetMode() == NM_ListenServer(2)`
// (e.g. sub_540E8A0: the "IsServer" it logs is `GetNetMode == 2`). A2 has no listen
// servers — we run a dedicated server (net mode 1) — so those checks fail and the sim
// runs OFFLINE (non-authoritative), which is why the jakeball never converges between
// clients. GetNetMode is called out-of-line (`call 0x4037D80`/`0x5473BC0`) and the check
// is `cmp eax, 2` a few bytes later. Scan the BallSimManager code cluster for that shape
// and patch the imm `02` -> `01` so our dedicated server is recognized as the authority.
static void PatchBallSimNetModeChecks()
{
    const uintptr_t base = GetBase();
    const uintptr_t lo   = base + HX::BallSimLo_RVA;   // whole ball-sim + physics-helper cluster
    const uintptr_t hi   = base + HX::BallSimHi_RVA;   // (widened: was 0x540C000-0x5445000, missed collision/ingest helpers)
    const uintptr_t gnmA = base + HX::WorldGetNetMode_RVA;   // UWorld::GetNetMode
    const uintptr_t gnmB = base + HX::A2GetNetMode_RVA;   // UA2NetworkUtilityBPFL::GetNetMode
    int patched = 0;
    for (uintptr_t p = lo; p < hi - 8; ++p)
    {
        if (*reinterpret_cast<uint8_t*>(p) != 0xE8)   // call rel32
            continue;
        const int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
        const uintptr_t target = p + 5 + rel;
        if (target != gnmA && target != gnmB)
            continue;
        // within the next few instructions, find `cmp eax, 2` (83 F8 02) and patch to 01.
        for (uintptr_t q = p + 5; q < p + 5 + 20; ++q)
        {
            if (*reinterpret_cast<uint8_t*>(q) == 0x83 &&
                *reinterpret_cast<uint8_t*>(q + 1) == 0xF8 &&
                *reinterpret_cast<uint8_t*>(q + 2) == 0x02)
            {
                WriteByte(q + 2, 0x01);
                ++patched;
                break;
            }
        }
    }
    printf("[HalcyonA2] patched %d ball-sim net-mode checks (==2 -> ==1)\n", patched);
}

static void Main(HMODULE)
{
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    const uintptr_t base = GetBase();
    printf("[HalcyonA2] attached. base = 0x%llX\n", (unsigned long long)base);

    // Wait for a live world + game instance (we may be injected in the frontend).
    SDK::UWorld* world = nullptr;
    while (true)
    {
        world = SDK::UWorld::GetWorld();
        if (world && world->OwningGameInstance)
            break;
        Sleep(100);
    }
    printf("[HalcyonA2] world ready: 0x%llX\n", (unsigned long long)world);

    // 1. Flip into server mode BEFORE travel. If we flip after open, the new world
    //    runs its whole init (subsystem Initialize, physics-step callback arming,
    //    package export loading) while the process still looks like a client, so the
    //    server-authoritative paths (incl. the Mass physics plumbing) never arm.
    WriteByte(base + GIsClient_RVA, 0); // GIsClient = false
    WriteByte(base + GIsServer_RVA, 1); // GIsServer = true
    printf("[HalcyonA2] GIsClient=false, GIsServer=true (before open)\n");

    // Station Dashboard reporting gate: sub_541D8A0 (the reporting init that fetches
    // deployment/station config -> board netvars) bails unless GetNetMode() == 2 (NM_ListenServer).
    // We force GetNetMode to 1 (NM_DedicatedServer), so patch the compare `cmp eax, 2` (0541D8EF:
    // 83 F8 02) -> `cmp eax, 1` so our net mode passes the gate. Then, given the three
    // -Dashboard* cmdline args, the game natively registers + fetches config + reports.
    // sub_541D8A0 gate B: `cmp eax, 2 ; j..` on GetNetMode(ctx). Our GetNetMode hook returns garbage
    // for the ctx this init passes (`(*(clientVtable+392))()` isn't a plain UWorld), so comparing
    // against ANY constant fails and the init bails registering nothing. Neutralize the compare so it
    // ALWAYS passes: overwrite `cmp eax, 2` (83 F8 02 @ 0x541D8EF) with `xor eax,eax ; nop` (31 C0 90)
    // -> ZF=1 -> the following j.. takes the "equal" (pass) path into the init body. eax is reassigned
    // right after the gate, so clobbering it is safe.
    WriteByte(base + HX::DashboardGate_RVA + 0, 0x31);
    WriteByte(base + HX::DashboardGate_RVA + 1, 0xC0);
    WriteByte(base + HX::DashboardGate_RVA + 2, 0x90);
    printf("[HalcyonA2] neutralized dashboard-init net-mode gate (541D8EF: cmp eax,2 -> xor eax,eax;nop)\n");

    // Seed the server dashboard api-key global BEFORE the native login runs, so the server's own
    // sub_54AB570 -> log_in_with_key carries x-api-key = our key and authenticates. (Also re-seeded +
    // login re-triggered from the ticker as a fallback if the native login already ran.)
    SafeSeedDashboardApiKey();

    // Force the station-dashboard log categories to VeryVerbose(7) so the SILENT skip-branches print
    // (sub_5409770's config-apply gates only log at >=5). byte_9BD4450 = LogA2StationDashboard (config
    // apply + login-response/roles), byte_9BD4170 = LogA2SessionSubsystem (auth). The category's first
    // byte IS its runtime verbosity (that's what the `byte >= N` guards read).
    WriteByte(base + HX::LogA2StationDashboard_RVA, 7); // LogA2StationDashboard -> VeryVerbose
    WriteByte(base + HX::LogA2SessionSubsystem_RVA, 7); // LogA2SessionSubsystem  -> VeryVerbose
    printf("[HalcyonA2] forced LogA2StationDashboard + LogA2SessionSubsystem to VeryVerbose\n");

    // Opt-in gamemode load via CMDLINE (rides in through agent.cfg game_args like the -Dashboard*
    // args; env vars don't reach the game process under the allocator): -LoadGamemode=<path>
    // (e.g. deathrun) loads it into a module slot -GamemodeSlot=<id> (default PKR_Custom_Full) once
    // the world is up. Unset = no-op.
    {
        const wchar_t* cl = GetCommandLineW();
        auto grab = [cl](const wchar_t* key, char* out, size_t sz) {
            const wchar_t* p = wcsstr(cl, key);
            if (!p) return;
            p += wcslen(key);
            wchar_t quote = 0;
            if (*p == L'"' || *p == L'\'') { quote = *p; ++p; }   // tolerate a quoted value
            wchar_t val[96] = {}; int i = 0;
            while (*p && i < 95 && (quote ? (*p != quote) : (*p != L' '))) val[i++] = *p++;
            size_t cvt = 0; wcstombs_s(&cvt, out, sz, val, _TRUNCATE);
        };
        grab(L"-LoadGamemode=", g_loadGmPath, sizeof(g_loadGmPath));
        grab(L"-GamemodeSlot=", g_loadGmSlot, sizeof(g_loadGmSlot));
        char posbuf[96] = {};
        grab(L"-GamemodePos=", posbuf, sizeof(posbuf));   // "X,Y,Z"
        if (posbuf[0] && sscanf_s(posbuf, "%lf,%lf,%lf", &g_gmPos[0], &g_gmPos[1], &g_gmPos[2]) == 3)
            g_gmPosSet = true;
        grab(L"-SnapToMarker=", g_snapMarker, sizeof(g_snapMarker));   // fullname substr of a marker actor
        char nudgebuf[96] = {};
        grab(L"-GmNudge=", nudgebuf, sizeof(nudgebuf));   // "X,Y,Z" world-space nudge of the course origin
        if (nudgebuf[0]) sscanf_s(nudgebuf, "%lf,%lf,%lf", &g_gmNudge[0], &g_gmNudge[1], &g_gmNudge[2]);
        if (wcsstr(GetCommandLineW(), L"-GmRewriteRot")) g_forceRot = true;
        if (wcsstr(GetCommandLineW(), L"-SpawnSlotAtMarker")) g_spawnSlot = true;
        char rotbuf[96] = {};
        grab(L"-SlotRot=", rotbuf, sizeof(rotbuf));   // "P,Y,R" extra rotation on the spawned slot
        if (rotbuf[0]) sscanf_s(rotbuf, "%lf,%lf,%lf", &g_slotRotAdj[0], &g_slotRotAdj[1], &g_slotRotAdj[2]);
        grab(L"-BallClass=", g_ballClass, sizeof(g_ballClass));   // renderable ball to replace broken death balls
        if (wcsstr(GetCommandLineW(), L"-NoFixLOD")) g_fixLod = false;   // disable the DefaultLODSettings pop-in fix
        if (wcsstr(GetCommandLineW(), L"-QuietSims")) g_quietSims = true; // A/B: kill all our ball net sends (player-lag test)
    }
    if (g_quietSims) printf("[HalcyonA2] -QuietSims: ball sim step + results-send + physics-sync DISABLED (player-lag A/B)\n");
    if (g_snapMarker[0]) printf("[HalcyonA2] -SnapToMarker='%s'\n", g_snapMarker);
    if (g_gmNudge[0]||g_gmNudge[1]||g_gmNudge[2]) printf("[HalcyonA2] -GmNudge=(%.0f,%.0f,%.0f)\n", g_gmNudge[0],g_gmNudge[1],g_gmNudge[2]);
    if (g_loadGmPath[0])
        printf("[HalcyonA2] -LoadGamemode='%s' -> slot '%s'%s (will load after world up)\n",
               g_loadGmPath, g_loadGmSlot,
               g_gmPosSet ? " @ custom pos" : "");

    // Hook UWorld::GetNetMode -> DedicatedServer BEFORE open so world init sees a
    // real server (the physics-step arming happens during init).
    MH_Initialize();
    void* gnmAddr = reinterpret_cast<void*>(base + WorldGetNetMode_RVA);
    MH_CreateHook(gnmAddr, &WorldGetNetMode_Hook, nullptr);
    MH_EnableHook(gnmAddr);
    printf("[HalcyonA2] UWorld::GetNetMode hook @ 0x%llX (before open)\n", (unsigned long long)gnmAddr);

    // Netvar-register hook: rewrites the replicated "DefaultLODSettings" netvar to golf's no-hide
    // profile at creation, so deathrun course objects stop proximity-hiding on Android (§ LOD fix).
    if (g_fixLod)
    {
        void* nvAddr = reinterpret_cast<void*>(base + HX::NetVarReg_RVA);
        if (MH_CreateHook(nvAddr, &NetVarReg_Hook, reinterpret_cast<void**>(&g_NetVarReg_Orig)) == MH_OK)
        {
            MH_EnableHook(nvAddr);
            printf("[HalcyonA2] netvar-register hook @ 0x%llX (DefaultLODSettings no-hide)\n", (unsigned long long)nvAddr);
        }
    }

    // 2. Travel to the real map — now it initializes as a server.
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(
        world,
        SDK::FString(L"open /Game/A2/Maps/Station_Prime/Station_Prime_P.umap"),
        nullptr);
    printf("[HalcyonA2] issued: open Station_Prime_P\n");

    // NOTE: we do NOT remove the server's local player. Tried it (LocalPlayers.Remove(0)) as a fix for
    // the spawn-centered movement lag on the theory it was the streaming/relevancy origin — it made NO
    // difference to the lag and it crashed LoadMap: with no local player, post-load init (e.g. loading
    // BP_VolleyJakeball's CosmeticLoadout) walks the local player and derefs null+0x10 (sub_3EF1F60).
    // Not worth it. See a2-match-state-and-ball-physics.

    // Kill texture streaming — dead weight on our -nullrhi server, and its manager periodically
    // virtual-calls a NULL render resource (obj@this+0x890) => the long-standing random null-deref
    // crash (sub_54ABCC0, in the streaming-manager update fed by ClientAddTextureStreamingLoc).
    // No renderer == nothing to stream. (Belt-and-suspenders: sub_54ABCC0 is also null-guarded.)
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"r.TextureStreaming 0"), nullptr);
    printf("[HalcyonA2] issued: r.TextureStreaming 0\n");

    // Widen the GC purge window (CLAUDE.md §3). UE's default incremental pending-kill purge runs
    // often and stalls the game thread — on a weak/contended VPS core those stalls show up as the
    // [STEP] dt spikes during play (0.05-0.4s), which burst-step the rollback sim = residual MI +
    // ball jitter. Pushing the purge interval way out removes that periodic hitch (and the old
    // GC-after-purge crash window). Also raise the GC object headroom so it doesn't purge under load.
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"gc.TimeBetweenPurgingPendingKillObjects 9999"), nullptr);
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"gc.MaxObjectsNotConsideredByGC 1000000"), nullptr);
    printf("[HalcyonA2] issued: gc.TimeBetweenPurgingPendingKillObjects 9999 + MaxObjectsNotConsideredByGC\n");

    // 3. Register this server with the backend, then set the station-id FString to
    //    the unique id it hands back (no writer for this global exists in the client).
    //    Detect the ACTUAL bound game port first (UE starts at 7777 and auto-increments if taken),
    //    so multiple instances on this box each register their own port instead of all claiming
    //    7777. Poll ~12s while the net driver binds after the open above; fall back to 7777.
    int gamePort = 0;
    for (int i = 0; i < 120 && gamePort == 0; ++i) { gamePort = GetOurListenPort(); if (gamePort) break; Sleep(100); }
    if (gamePort == 0) { gamePort = 7777; printf("[HalcyonA2] listen port not detected in 12s; defaulting to 7777\n"); }
    // Instance index from the port offset: 7777 -> "HalcyonA2", 7778 -> "HalcyonA2_1", 7779 -> "_2"...
    const int  inst = gamePort - 7777;
    char nameStr[32];
    if (inst <= 0) strcpy_s(nameStr, "HalcyonA2");
    else           sprintf_s(nameStr, "HalcyonA2_%d", inst);
    printf("[HalcyonA2] detected game listen port = %d -> server_name = %s\n", gamePort, nameStr);
    char portStr[16]; sprintf_s(portStr, "%d", gamePort);

    // Deployment id chosen by the allocator/socket BEFORE launch and handed to the game as
    // -DashboardDeploymentId (the same arg the game's native config fetch reads). Register under it
    // so our DB row and the game's config GET agree — instead of the old fixed "halcyon" reused
    // everywhere. Absent (manual launch) -> register_server mints a fresh id as before.
    std::string deploymentId;
    {
        const wchar_t* cl = GetCommandLineW();
        const wchar_t* p = wcsstr(cl, L"-DashboardDeploymentId=");
        if (p)
        {
            p += wcslen(L"-DashboardDeploymentId=");
            wchar_t q = 0;
            if (*p == L'"' || *p == L'\'') { q = *p; ++p; }
            wchar_t val[128] = {}; int i = 0;
            while (*p && i < 127 && (q ? (*p != q) : (*p != L' '))) val[i++] = *p++;
            char buf[128] = {}; size_t cvt = 0; wcstombs_s(&cvt, buf, sizeof(buf), val, _TRUNCATE);
            deploymentId = buf;
        }
    }
    printf("[HalcyonA2] deployment id (from -DashboardDeploymentId) = %s\n",
           deploymentId.empty() ? "(none; backend will mint)" : deploymentId.c_str());
    g_deploymentIdW.assign(deploymentId.begin(), deploymentId.end());   // for per-server Vivox channel scoping

    // Register under this box's real address. Priority: explicit -RegisterIp=<ip> launch arg (ops
    // override), else the detected PUBLIC IP, else a last-resort fallback so registration still sends.
    std::string registerIp;
    {
        const wchar_t* cl = GetCommandLineW();
        const wchar_t* p = wcsstr(cl, L"-RegisterIp=");
        if (p)
        {
            p += wcslen(L"-RegisterIp=");
            wchar_t q = 0;
            if (*p == L'"' || *p == L'\'') { q = *p; ++p; }
            wchar_t val[64] = {}; int i = 0;
            while (*p && i < 63 && (q ? (*p != q) : (*p != L' '))) val[i++] = *p++;
            char buf[64] = {}; size_t cvt = 0; wcstombs_s(&cvt, buf, sizeof(buf), val, _TRUNCATE);
            registerIp = buf;
        }
    }
    if (registerIp.empty()) registerIp = GetPublicIp();
    if (registerIp.empty()) { registerIp = "34.239.141.19"; printf("[HalcyonA2] public IP detect FAILED -> fallback %s\n", registerIp.c_str()); }
    else printf("[HalcyonA2] register IP = %s\n", registerIp.c_str());

    std::string reqBody =
        std::string("{\"ip\":\"") + registerIp + "\",\"port\":\"" + portStr +
        "\",\"server_name\":\"" + nameStr + "\",\"max_players\":10";
    if (!deploymentId.empty()) reqBody += ",\"deployment_id\":\"" + deploymentId + "\"";
    reqBody += "}";
    const std::string resp = HttpPostLocal(kBackendHost, kBackendPort, L"/register_server", reqBody);
    printf("[HalcyonA2] register_server -> %s\n", resp.empty() ? "(no response)" : resp.c_str());

    // Remember the effective deployment id (ours, or the one the backend minted) so the
    // player-count heartbeat can key its updates to this server's EOS session.
    g_deploymentId = !deploymentId.empty() ? deploymentId : ExtractJsonString(resp, "deployment_id");

    const std::string sid = ExtractJsonString(resp, "station_id");
    if (sid.empty())
    {
        printf("[HalcyonA2] registration failed; using fallback station id\n");
        g_stationId = kStationFallback;
    }
    else
    {
        g_stationId.assign(sid.begin(), sid.end());
    }

    // CRASH FIX (2026-08-20): do NOT write GA2StationId (0x95579C0 Data / C8 Num / CC Max) at all.
    // It is a game FString that BOTH A2Station__FindRoleForStation / FetchUserRoles read AND the
    // native station-dashboard path writes: `GA2StationId = <parsed station_id>` is an FString
    // operator= -> realloc of the existing Data. We used to point Data at g_stationId's std::wstring
    // (CRT-heap) buffer, so that native realloc hit FMallocBinned2 "realloc an unrecognized block,
    // canary 0x44" and killed the process. Now that the net-mode gate is patched AND the backend
    // returns a real station_id, the native path populates GA2StationId itself with a game-heap
    // buffer it fully owns (alloc/realloc/free) — so we leave it untouched. Roles read GA2StationId,
    // so confirm they still resolve after deploy; if not, the native populate isn't firing.
    printf("[HalcyonA2] station id (registered) = %ls -> NOT touching GA2StationId; native dashboard populates it\n",
           g_stationId.c_str());

    // 4. Hook ProcessEvent (game thread) for one-shot world setup + the ball sim
    //    pumps + PrintString/PrintDebugString capture.
    void* peAddr = reinterpret_cast<void*>(base + SDK::Offsets::ProcessEvent);
    MH_CreateHook(peAddr, &ProcessEvent_Hook, reinterpret_cast<void**>(&ProcessEvent_Orig));

    // Force IsRunningSimulate=true so the Mass physics processor advances the balls.
    void* isRunAddr = reinterpret_cast<void*>(base + IsRunningSimulate_RVA);
    MH_STATUS s1 = MH_CreateHook(isRunAddr, &IsRunningSimulate_Hook, nullptr);
    printf("[HalcyonA2] IsRunningSimulate hook @ 0x%llX create=%s\n",
           (unsigned long long)isRunAddr, MH_StatusToString(s1));

    // Force A2's net mode to DedicatedServer so server-gated physics/logic runs.
    void* a2NetAddr = reinterpret_cast<void*>(base + A2GetNetMode_RVA);
    MH_STATUS s2 = MH_CreateHook(a2NetAddr, &A2GetNetMode_Hook, nullptr);
    printf("[HalcyonA2] A2 GetNetMode hook @ 0x%llX create=%s\n",
           (unsigned long long)a2NetAddr, MH_StatusToString(s2));

    // Echo every UKismetSystemLibrary::PrintString to our console.
    void* psAddr = reinterpret_cast<void*>(base + PrintString_RVA);
    MH_STATUS s3 = MH_CreateHook(psAddr, &PrintString_Hook, reinterpret_cast<void**>(&PrintString_Orig));
    printf("[HalcyonA2] PrintString hook @ 0x%llX create=%s\n",
           (unsigned long long)psAddr, MH_StatusToString(s3));

    // Vivox login-token "f" claim fix (RequestVivoxLoginToken impl + JWT assembler).
    void* reqAddr = reinterpret_cast<void*>(base + ReqLogin_RVA);
    MH_STATUS s4 = MH_CreateHook(reqAddr, &ReqLogin_Hook, reinterpret_cast<void**>(&ReqLogin_Orig));
    void* jwtAddr = reinterpret_cast<void*>(base + JwtBuild_RVA);
    MH_STATUS s5 = MH_CreateHook(jwtAddr, &JwtBuild_Hook, reinterpret_cast<void**>(&JwtBuild_Orig));
    printf("[HalcyonA2] VOIP login-token hooks: ReqLogin=%s JwtBuild=%s\n",
           MH_StatusToString(s4), MH_StatusToString(s5));

    // Vivox channel-join fix (join-token builder + ReceiveChannelJoinTokens sender).
    void* joinAddr = reinterpret_cast<void*>(base + JoinBuild_RVA);
    MH_STATUS s6 = MH_CreateHook(joinAddr, &JoinBuild_Hook, reinterpret_cast<void**>(&JoinBuild_Orig));
    void* sendAddr = reinterpret_cast<void*>(base + SendJoin_RVA);
    MH_STATUS s7 = MH_CreateHook(sendAddr, &SendJoin_Hook, reinterpret_cast<void**>(&SendJoin_Orig));
    printf("[HalcyonA2] VOIP channel-join hooks: JoinBuild=%s SendJoin=%s\n",
           MH_StatusToString(s6), MH_StatusToString(s7));

    // BallSimManager GetWorld fix — makes the sim go authoritative (IsServer:1).
    void* bgwAddr = reinterpret_cast<void*>(base + BallSimGetWorld_RVA);
    MH_STATUS s8 = MH_CreateHook(bgwAddr, &BallSimGetWorld_Hook, reinterpret_cast<void**>(&BallSimGetWorld_Orig));
    printf("[HalcyonA2] BallSim GetWorld hook @ 0x%llX create=%s\n",
           (unsigned long long)bgwAddr, MH_StatusToString(s8));

    // Make the ball sim treat our dedicated server (net mode 1) as the authority.
    PatchBallSimNetModeChecks();

    // Force-accept client ball-state streams (bypass the owningActor gate) so hits land.
    void* spAddr = reinterpret_cast<void*>(base + SendPhysImpl_RVA);
    MH_STATUS s9 = MH_CreateHook(spAddr, &SendPhys_Hook, reinterpret_cast<void**>(&SendPhys_Orig));
    printf("[HalcyonA2] SendPhysicsPropData hook @ 0x%llX create=%s\n",
           (unsigned long long)spAddr, MH_StatusToString(s9));

    // Log whether client inputs are ADDED to the sim or SKIPPED (frame gate / player miss).
    void* inAddr = reinterpret_cast<void*>(base + IngestInput_RVA);
    MH_STATUS s10 = MH_CreateHook(inAddr, &IngestInput_Hook, reinterpret_cast<void**>(&IngestInput_Orig));
    printf("[HalcyonA2] input-ingest hook @ 0x%llX create=%s\n",
           (unsigned long long)inAddr, MH_StatusToString(s10));

    // Read the server's computed missedInputs/missedCaught (the client HUD MI/MIB) each send.
    void* srAddr = reinterpret_cast<void*>(base + SendResults_RVA);
    MH_STATUS sSR = MH_CreateHook(srAddr, &SendResults_Hook, reinterpret_cast<void**>(&SendResults_Orig));
    printf("[HalcyonA2] SendServerSimResults hook @ 0x%llX create=%s\n",
           (unsigned long long)srAddr, MH_StatusToString(sSR));

    // DIAGNOSTIC: measure who advances the sim frame (step vs ingest).
    void* stAddr = reinterpret_cast<void*>(base + StepSim_RVA);
    MH_STATUS sST = MH_CreateHook(stAddr, &StepSim_Hook, reinterpret_cast<void**>(&StepSim_Orig));
    printf("[HalcyonA2] StepSim probe hook @ 0x%llX create=%s\n",
           (unsigned long long)stAddr, MH_StatusToString(sST));

    // Inject the hit velocity into the sim's ball state each integrate.
    void* siAddr = reinterpret_cast<void*>(base + SimIntegrate_RVA);
    MH_STATUS s11 = MH_CreateHook(siAddr, &SimIntegrate_Hook, reinterpret_cast<void**>(&SimIntegrate_Orig));
    printf("[HalcyonA2] sim-integrate hook @ 0x%llX create=%s\n",
           (unsigned long long)siAddr, MH_StatusToString(s11));

    // Trace ATicketManager::GiveAndCheckTicket (arena admission) to find the verify-bail
    // that keeps players out of VerifiedTicketHolders (no team, no sim seat).
    void* gacAddr = reinterpret_cast<void*>(base + GiveCheck_RVA);
    MH_STATUS s12 = MH_CreateHook(gacAddr, &GiveCheck_Hook, reinterpret_cast<void**>(&GiveCheck_Orig));
    printf("[HalcyonA2] GiveAndCheckTicket hook @ 0x%llX create=%s\n",
           (unsigned long long)gacAddr, MH_StatusToString(s12));

    // Leave-arena reset: drop the player from the color stamp + clear color/team (else our fast-path
    // stamp keeps re-painting the arena color forever after they leave).
    void* nlaAddr = reinterpret_cast<void*>(base + NotifyLeftArena_RVA);
    MH_STATUS s13 = MH_CreateHook(nlaAddr, &NotifyLeftArena_Hook, reinterpret_cast<void**>(&NotifyLeftArena_Orig));
    printf("[HalcyonA2] Server_NotifyPlayerLeftArena hook @ 0x%llX create=%s\n",
           (unsigned long long)nlaAddr, MH_StatusToString(s13));

    // GOLF/event dispatch diagnostic — hexdump incoming A2 events to reverse the golf payload format.
    void* evtAddr = reinterpret_cast<void*>(base + EvtDispatch_RVA);
    MH_STATUS s14 = MH_CreateHook(evtAddr, &EvtDispatch_Hook, reinterpret_cast<void**>(&EvtDispatch_Orig));
    printf("[HalcyonA2] UNetEventsBridge dispatch hook @ 0x%llX create=%s\n",
           (unsigned long long)evtAddr, MH_StatusToString(s14));

    // Crash-stopper: guard FName->string against invalid FNames (arena-leave sim teardown AV).
    void* fnrAddr = reinterpret_cast<void*>(base + FNameResolve_RVA);
    MH_STATUS s15 = MH_CreateHook(fnrAddr, &FNameResolve_Hook, reinterpret_cast<void**>(&FNameResolve_Orig));
    printf("[HalcyonA2] FName-resolve guard hook @ 0x%llX create=%s\n",
           (unsigned long long)fnrAddr, MH_StatusToString(s15));

    // Crash-stopper: guard the Slate text-run layout leaf against invalid-FName AV (arena-leave 0x1FFFE).
    void* tllAddr = reinterpret_cast<void*>(base + TextLayoutLeaf_RVA);
    MH_STATUS s15b = MH_CreateHook(tllAddr, &TextLayoutLeaf_Hook, reinterpret_cast<void**>(&TextLayoutLeaf_Orig));
    printf("[HalcyonA2] text-layout leaf guard hook @ 0x%llX create=%s\n",
           (unsigned long long)tllAddr, MH_StatusToString(s15b));

    // Golf-cup server-side detection probe (does the sink fire on the server?).
    void* gbicAddr = reinterpret_cast<void*>(base + GolfBallInCup_RVA);
    MH_STATUS s16 = MH_CreateHook(gbicAddr, &GolfBallInCup_Hook, reinterpret_cast<void**>(&GolfBallInCup_Orig));
    void* govAddr = reinterpret_cast<void*>(base + GolfOverlap_RVA);
    MH_STATUS s17 = MH_CreateHook(govAddr, &GolfOverlap_Hook, reinterpret_cast<void**>(&GolfOverlap_Orig));
    printf("[HalcyonA2] GolfCup hooks BallInCup=%s Overlap=%s\n", MH_StatusToString(s16), MH_StatusToString(s17));

    // Gamemode placement: sub_46D77A0 = per-ObjectPrefab netvar builder. When armed (LoadGamemode +
    // -SnapToMarker/-GamemodePos), rewrites each object's local transform so the loaded course lands at
    // the marker instead of the slot's baked spot. See the block above LoadGamemodeIntoSlot.
    void* objBuildAddr = reinterpret_cast<void*>(base + HX::ObjBuild_RVA);
    MH_STATUS sOB = MH_CreateHook(objBuildAddr, &ObjBuild_Hook, reinterpret_cast<void**>(&ObjBuild_Orig));
    printf("[HalcyonA2] gamemode object-builder hook @ 0x%llX create=%s\n",
           (unsigned long long)objBuildAddr, MH_StatusToString(sOB));

    // QUEST TRACE hooks — see if the game drives quest init/progress itself.
    MH_STATUS qa = MH_CreateHook(reinterpret_cast<void*>(base + HX::QInit_RVA), &QInit_Hook, reinterpret_cast<void**>(&QInit_Orig));
    MH_STATUS qb = MH_CreateHook(reinterpret_cast<void*>(base + HX::QSetQ_RVA), &QSetQ_Hook, reinterpret_cast<void**>(&QSetQ_Orig));
    MH_STATUS qc = MH_CreateHook(reinterpret_cast<void*>(base + HX::QReg_RVA), &QReg_Hook,  reinterpret_cast<void**>(&QReg_Orig));
    MH_STATUS qd = MH_CreateHook(reinterpret_cast<void*>(base + HX::QSetP_RVA), &QSetP_Hook, reinterpret_cast<void**>(&QSetP_Orig));
    printf("[HalcyonA2] QTRACE hooks: Init=%s SetQuests=%s RegWorker=%s SetProgress=%s\n",
           MH_StatusToString(qa), MH_StatusToString(qb), MH_StatusToString(qc), MH_StatusToString(qd));

    // Null-guard the texture-streaming thunk that random-crashes on -nullrhi.
    MH_STATUS qs = MH_CreateHook(reinterpret_cast<void*>(base + HX::StreamThunk_RVA), &StreamThunk_Hook, reinterpret_cast<void**>(&StreamThunk_Orig));
    printf("[HalcyonA2] texture-streaming null-guard hook: %s\n", MH_StatusToString(qs));

    // Quests: capture each joiner's org-scoped id from the roles fetch to warm their Mothership
    // quest fetch (A2Station__FetchUserRoles, a1 = org id FString).
    void* frAddr = reinterpret_cast<void*>(base + FetchRoles_RVA);
    MH_STATUS sFetchRoles = MH_CreateHook(frAddr, &FetchRoles_Hook, reinterpret_cast<void**>(&FetchRoles_Orig));
    printf("[HalcyonA2] FetchUserRoles hook @ 0x%llX create=%s\n",
           (unsigned long long)frAddr, MH_StatusToString(sFetchRoles));

    // Null-guard GetPlayerPawn so client ticks that resolve the (removed) local player skip instead
    // of dereferencing a null controller — the first fault after LocalPlayers.Remove(0).
    void* gppAddr = reinterpret_cast<void*>(base + GetPlayerPawn_RVA);
    MH_STATUS sGpp = MH_CreateHook(gppAddr, &GetPlayerPawn_Hook, reinterpret_cast<void**>(&GetPlayerPawn_Orig));
    printf("[HalcyonA2] GetPlayerPawn null-guard hook @ 0x%llX create=%s\n",
           (unsigned long long)gppAddr, MH_StatusToString(sGpp));

    // Silence AVRPawn::GetLocalPlayer_Implementation's per-pawn "GetLocalPlayer -- Controller was
    // invalid" LogTemp::Error spam. With the local player removed, every pawn whose controller resolves
    // null hits that branch each frame; the RETURN (0) is correct — only the FMsg::Logf floods the log
    // (real game-thread format+I/O cost). NOP the 5-byte `call FMsg__Logf` @0x547380A; behavior is
    // otherwise identical (still returns 0), the log just doesn't fire. Nothing else in the func changes.
    for (int i = 0; i < 5; ++i) WriteByte(base + HX::LogfNop_RVA + i, 0x90);
    printf("[HalcyonA2] silenced GetLocalPlayer 'Controller was invalid' spam (nop'd call @0x547380A)\n");

    MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
    printf("[HalcyonA2] EnableHook(ALL)=%s | ProcessEvent @ 0x%llX\n",
           MH_StatusToString(es), (unsigned long long)peAddr);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Never do work in DllMain (loader lock). Spin up a worker thread.
        std::thread(Main, hModule).detach();
    }
    return TRUE;
}
