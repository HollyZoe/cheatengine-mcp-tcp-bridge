/*
 * ce_mcp_tcp.dll - Native TCP Bridge for Cheat Engine MCP
 *
 * Two operating modes:
 *   1. Native Lua API mode: resolves lua_pushstring etc. and registers global
 *      functions (mcp_tcp_start/stop/poll/respond/status).
 *   2. File IPC mode (fallback): when Lua API cannot be resolved, the DLL
 *      starts TCP itself and exchanges commands/responses via temp files.
 *      Lua polls %TEMP%\ce_mcp\cmd.txt and writes resp.txt.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

/* ---------- Debug Console ---------- */

static HANDLE g_console = NULL;
static FILE  *g_logfp   = NULL;
static HMODULE g_self_module = NULL;

static void dbg_init(void) {
    if (g_console) return;
    AllocConsole();
    SetConsoleTitleA("[MCP] ce_mcp_tcp.dll - Debug Console");
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    freopen_s(&g_logfp, "CONOUT$", "w", stdout);
}

static void dbg_log(const char *fmt, ...) {
    if (!g_console) dbg_init();
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n > 0) {
        buf[n] = '\n';
        buf[n + 1] = '\0';
        DWORD written;
        WriteConsoleA(g_console, buf, n + 1, &written, NULL);
    }
}

/* ---------- Lua API runtime binding ---------- */

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State *L);
typedef long long lua_Integer;

static const char* (*pL_pushstring)(lua_State*, const char*);
static void        (*pL_pushinteger)(lua_State*, lua_Integer);
static void        (*pL_pushnil)(lua_State*);
static void        (*pL_pushboolean)(lua_State*, int);
static const char* (*pL_tolstring)(lua_State*, int, size_t*);
static lua_Integer (*pL_tointegerx)(lua_State*, int, int*);
static int         (*pL_gettop)(lua_State*);
static void        (*pL_settop)(lua_State*, int);
static void        (*pL_setglobal)(lua_State*, const char*);
static void        (*pL_pushcclosure)(lua_State*, lua_CFunction, int);
static int         (*pL_isstring)(lua_State*, int);
static int         (*pL_isnumber)(lua_State*, int);
static void        (*pL_createtable)(lua_State*, int, int);
static void        (*pL_setfield)(lua_State*, int, const char*);
static int         (*pL_getglobal)(lua_State*, const char*);
static int         (*pL_pcallk)(lua_State*, int, int, int, long long, void*);
static int         (*pL_error)(lua_State*);

static int lua_api_ready = 0;

/* Thread-safe Lua execution state */
static lua_State *g_lua_state = NULL;
static CRITICAL_SECTION g_lua_cs;
static volatile int g_lua_cs_init = 0;
static volatile int g_threaded_mode = 0;  /* 1 = execute in TCP thread via pcall */

typedef struct {
    const char *name;
    void       **ptr;
} LuaApiEntry;

static int try_resolve_from_module(HMODULE mod, const char *modName, LuaApiEntry *entries, int count) {
    int found = 0;
    for (int i = 0; i < count; i++) {
        void *addr = (void*)GetProcAddress(mod, entries[i].name);
        if (addr) {
            *(entries[i].ptr) = addr;
            found++;
        }
    }
    if (found > 0)
        dbg_log("[MCP-DLL]   %s => %d/%d functions", modName, found, count);
    return found;
}

static int resolve_lua_api(void) {
    LuaApiEntry entries[] = {
        { "lua_pushstring",  (void**)&pL_pushstring   },
        { "lua_pushinteger", (void**)&pL_pushinteger  },
        { "lua_pushnil",     (void**)&pL_pushnil      },
        { "lua_pushboolean", (void**)&pL_pushboolean  },
        { "lua_tolstring",   (void**)&pL_tolstring    },
        { "lua_tointegerx",  (void**)&pL_tointegerx   },
        { "lua_gettop",      (void**)&pL_gettop       },
        { "lua_settop",      (void**)&pL_settop       },
        { "lua_setglobal",   (void**)&pL_setglobal    },
        { "lua_pushcclosure",(void**)&pL_pushcclosure },
        { "lua_isstring",    (void**)&pL_isstring     },
        { "lua_isnumber",    (void**)&pL_isnumber     },
        { "lua_createtable", (void**)&pL_createtable  },
        { "lua_setfield",    (void**)&pL_setfield     },
        { "lua_getglobal",   (void**)&pL_getglobal    },
        { "lua_pcallk",      (void**)&pL_pcallk       },
        { "lua_error",       (void**)&pL_error        },
    };
    int total = sizeof(entries) / sizeof(entries[0]);
    int best_count = 0;
    char best_name[260] = {0};

    dbg_log("[MCP-DLL] Resolving Lua API (%d functions)...", total);

    const char* known[] = {
        "lua54.dll", "lua53.dll", "lua5.4.dll", "lua5.3.dll",
        "lua54-64.dll", "lua53-64.dll", "lua5.4-64.dll", "lua5.3-64.dll",
        "lua53-32.dll", "lua54-32.dll",
        NULL
    };

    /* Phase 1: well-known Lua DLL names */
    char selfDir[MAX_PATH] = {0};
    if (g_self_module) {
        GetModuleFileNameA(g_self_module, selfDir, MAX_PATH);
        char *sep = strrchr(selfDir, '\\');
        if (sep) *(sep + 1) = '\0'; else selfDir[0] = '\0';
    }

    for (int i = 0; known[i]; i++) {
        HMODULE mod = GetModuleHandleA(known[i]);
        if (!mod) {
            /* Try full path from DLL's directory first */
            if (selfDir[0]) {
                char fullp[MAX_PATH];
                _snprintf(fullp, MAX_PATH, "%s%s", selfDir, known[i]);
                mod = LoadLibraryA(fullp);
            }
        }
        if (!mod) mod = LoadLibraryA(known[i]);
        if (!mod) continue;
        dbg_log("[MCP-DLL] Found module: %s", known[i]);
        int n = try_resolve_from_module(mod, known[i], entries, total);
        if (n == total) { lua_api_ready = 1; return 1; }
        if (n > best_count) { best_count = n; strncpy(best_name, known[i], 259); }
    }

    /* Phase 2: main executable */
    {
        HMODULE mainExe = GetModuleHandleA(NULL);
        if (mainExe) {
            int n = try_resolve_from_module(mainExe, "(main exe)", entries, total);
            if (n == total) { lua_api_ready = 1; return 1; }
            if (n > best_count) { best_count = n; strncpy(best_name, "(main exe)", 259); }
        }
    }

    /* Phase 3: enumerate every loaded module */
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me;
            me.dwSize = sizeof(me);
            int moduleCount = 0;
            if (Module32First(snap, &me)) {
                do {
                    moduleCount++;
                    HMODULE mod = me.hModule;
                    if (!mod) continue;
                    if (!GetProcAddress(mod, "lua_pushstring")) continue;
                    int n = try_resolve_from_module(mod, me.szModule, entries, total);
                    if (n == total) { CloseHandle(snap); lua_api_ready = 1; return 1; }
                    if (n > best_count) { best_count = n; strncpy(best_name, me.szModule, 259); }
                } while (Module32Next(snap, &me));
            }
            dbg_log("[MCP-DLL] Enumerated %d modules, best: %d/%d", moduleCount, best_count, total);
            CloseHandle(snap);
        }
    }

    /* Phase 4: scan OUR DLL's directory for lua*.dll (DLL is placed in CE dir) */
    {
        char dllPath[MAX_PATH] = {0};
        GetModuleFileNameA(g_self_module, dllPath, MAX_PATH);
        char *lastSep = strrchr(dllPath, '\\');
        if (!lastSep) lastSep = strrchr(dllPath, '/');
        if (lastSep) {
            char exeDir[MAX_PATH];
            int dirLen = (int)(lastSep - dllPath);
            strncpy(exeDir, dllPath, dirLen);
            exeDir[dirLen] = '\0';
            dbg_log("[MCP-DLL] DLL dir (CE dir): %s", exeDir);

            char searchPattern[MAX_PATH];
            _snprintf(searchPattern, MAX_PATH, "%s\\lua*.dll", exeDir);

            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(searchPattern, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    char fullPath[MAX_PATH];
                    _snprintf(fullPath, MAX_PATH, "%s\\%s", exeDir, fd.cFileName);
                    dbg_log("[MCP-DLL] Found lua DLL: %s", fd.cFileName);

                    HMODULE mod = GetModuleHandleA(fd.cFileName);
                    if (!mod) mod = LoadLibraryA(fullPath);
                    if (mod) {
                        int n = try_resolve_from_module(mod, fd.cFileName, entries, total);
                        if (n == total) {
                            dbg_log("[MCP-DLL] All Lua API resolved from: %s", fd.cFileName);
                            FindClose(hFind);
                            lua_api_ready = 1;
                            return 1;
                        }
                        if (n > best_count) { best_count = n; strncpy(best_name, fd.cFileName, 259); }
                    } else {
                        dbg_log("[MCP-DLL]   LoadLibrary failed for %s (err=%lu)", fd.cFileName, GetLastError());
                    }
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            } else {
                dbg_log("[MCP-DLL] No lua*.dll found in CE dir");
            }

            /* Also try the main exe by full path (some Delphi apps export from exe) */
            {
                HMODULE mainMod = GetModuleHandleA(NULL);
                if (mainMod) {
                    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)mainMod;
                    if (dos->e_magic == 0x5A4D) {
                        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)((char*)mainMod + dos->e_lfanew);
                        DWORD expRVA = nt->OptionalHeader.DataDirectory[0].VirtualAddress;
                        if (expRVA) {
                            IMAGE_EXPORT_DIRECTORY *exp = (IMAGE_EXPORT_DIRECTORY*)((char*)mainMod + expRVA);
                            DWORD *names_arr = (DWORD*)((char*)mainMod + exp->AddressOfNames);
                            int luaCount = 0;
                            for (DWORD i = 0; i < exp->NumberOfNames && i < 10000; i++) {
                                const char *nm = (const char*)mainMod + names_arr[i];
                                if (nm[0]=='l' && nm[1]=='u' && nm[2]=='a' && nm[3]=='_') {
                                    luaCount++;
                                    if (luaCount <= 5) dbg_log("[MCP-DLL]   exe export: %s", nm);
                                }
                            }
                            dbg_log("[MCP-DLL] Main exe: %d lua_* exports (%lu total)", luaCount, exp->NumberOfNames);
                        } else {
                            dbg_log("[MCP-DLL] Main exe: no export directory");
                        }
                    }
                }
            }
        }
    }

    /* Partial match with critical functions is acceptable */
    if (pL_pushstring && pL_pushinteger && pL_pushnil &&
        pL_pushboolean && pL_tolstring && pL_tointegerx &&
        pL_gettop && pL_settop && pL_setglobal &&
        pL_pushcclosure && pL_createtable && pL_setfield) {
        dbg_log("[MCP-DLL] Core Lua API resolved (some optional missing)");
        lua_api_ready = 1;
        return 1;
    }

    dbg_log("[MCP-DLL] Lua API resolution FAILED (%d/%d from %s)",
            best_count, total, best_count > 0 ? best_name : "none");
    return 0;
}

/* Convenience wrappers */
static void lua_pushstr(lua_State *L, const char *s) { pL_pushstring(L, s); }
static void lua_pushint(lua_State *L, lua_Integer v) { pL_pushinteger(L, v); }
static void lua_pushbool(lua_State *L, int b) { pL_pushboolean(L, b); }
static void lua_pushnothing(lua_State *L) { pL_pushnil(L); }
static const char* lua_getstr(lua_State *L, int idx) { return pL_tolstring(L, idx, NULL); }
static lua_Integer lua_getint(lua_State *L, int idx) { return pL_tointegerx(L, idx, NULL); }
static int lua_nargs(lua_State *L) { return pL_gettop(L); }

static void lua_newtable(lua_State *L) { pL_createtable(L, 0, 4); }
static void lua_setstrfield(lua_State *L, int idx, const char *k, const char *v) {
    lua_pushstr(L, v);
    pL_setfield(L, idx < 0 ? idx - 1 : idx, k);
}
static void lua_setintfield(lua_State *L, int idx, const char *k, lua_Integer v) {
    lua_pushint(L, v);
    pL_setfield(L, idx < 0 ? idx - 1 : idx, k);
}
static void lua_setboolfield(lua_State *L, int idx, const char *k, int v) {
    lua_pushbool(L, v);
    pL_setfield(L, idx < 0 ? idx - 1 : idx, k);
}
static void lua_register_func(lua_State *L, const char *name, lua_CFunction f) {
    pL_pushcclosure(L, f, 0);
    pL_setglobal(L, name);
}

/* ---------- JSON method extractor ---------- */

static const char* extract_json_method(const char *json, char *out, int outLen) {
    const char *key = "\"method\"";
    const char *p = strstr(json, key);
    if (!p) { out[0] = '\0'; return out; }
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') { out[0] = '\0'; return out; }
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outLen - 1) out[i++] = *p++;
    out[i] = '\0';
    return out;
}

/* ---------- Threaded Lua Execution ---------- */

static char* execute_lua_handler(const char *cmd_json, int cmd_len, int *out_resp_len) {
    if (!g_lua_state || !g_threaded_mode || !pL_getglobal || !pL_pcallk) {
        *out_resp_len = 0;
        return NULL;
    }

    char *result = NULL;
    *out_resp_len = 0;

    EnterCriticalSection(&g_lua_cs);

    pL_getglobal(g_lua_state, "MCP_NativeExecute");
    pL_pushstring(g_lua_state, cmd_json);
    int err = pL_pcallk(g_lua_state, 1, 1, 0, 0, NULL);

    if (err == 0) {
        size_t rlen = 0;
        const char *resp = pL_tolstring(g_lua_state, -1, &rlen);
        if (resp && rlen > 0) {
            result = (char*)malloc(rlen + 1);
            if (result) {
                memcpy(result, resp, rlen);
                result[rlen] = '\0';
                *out_resp_len = (int)rlen;
            }
        }
    } else {
        const char *errmsg = pL_tolstring(g_lua_state, -1, NULL);
        dbg_log("[MCP-DLL] Lua pcall error: %s", errmsg ? errmsg : "unknown");
        const char *err_json = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32603,\"message\":\"Lua execution error\"},\"id\":null}";
        int elen = (int)strlen(err_json);
        result = (char*)malloc(elen + 1);
        if (result) {
            memcpy(result, err_json, elen + 1);
            *out_resp_len = elen;
        }
    }

    pL_settop(g_lua_state, 0);
    LeaveCriticalSection(&g_lua_cs);

    return result;
}

/* ---------- TCP Server ---------- */

#define MAX_CMD_SIZE   (4 * 1024 * 1024)
#define MAX_RESP_SIZE  (4 * 1024 * 1024)
#define MAX_PORT_RANGE 10
#define SELECT_TIMEOUT_SEC 1

typedef struct {
    volatile int running;
    volatile int listening;
    volatile int connected;
    int listen_port;

    SOCKET listen_sock;
    SOCKET client_sock;

    HANDLE thread;
    DWORD  thread_id;

    char bind_addr[64];
    int  base_port;
    int  max_port;
} TcpBridge;

static TcpBridge g_bridge = {0};
static int g_wsa_init = 0;
static volatile int g_bridge_initialized = 0;

static int wsa_startup(void) {
    if (g_wsa_init) return 1;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    g_wsa_init = 1;
    return 1;
}

static int tcp_send_frame(SOCKET s, const char *data, int len) {
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(len & 0xFF);
    hdr[1] = (unsigned char)((len >> 8) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 24) & 0xFF);

    int sent = 0;
    while (sent < 4) {
        int n = send(s, (char*)hdr + sent, 4 - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static int tcp_recv_exact(SOCKET s, char *buf, int len, int timeout_ms) {
    int total = 0;
    DWORD start = GetTickCount();
    while (total < len) {
        fd_set rd;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        int sel = select(0, &rd, NULL, NULL, &tv);
        if (sel < 0) return -1;
        if (sel == 0) {
            if (!g_bridge.running) return -1;
            if (timeout_ms > 0 && (int)(GetTickCount() - start) > timeout_ms)
                return -1;
            continue;
        }
        int n = recv(s, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return total;
}

static char* tcp_recv_frame(SOCKET s, int *out_len) {
    unsigned char hdr[4];
    if (tcp_recv_exact(s, (char*)hdr, 4, 600000) != 4) return NULL;
    int len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
    if (len <= 0 || len > MAX_CMD_SIZE) return NULL;
    char *buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    if (tcp_recv_exact(s, buf, len, 600000) != len) { free(buf); return NULL; }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static DWORD WINAPI tcp_server_thread(LPVOID param) {
    TcpBridge *br = (TcpBridge*)param;
    dbg_log("[MCP-DLL] TCP server thread started");

    br->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (br->listen_sock == INVALID_SOCKET) {
        dbg_log("[MCP-DLL] ERROR: socket() failed, err %d", WSAGetLastError());
        br->running = 0;
        return 1;
    }

    int optval = 1;
    setsockopt(br->listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(br->bind_addr);

    int bound = 0;
    for (int p = br->base_port; p <= br->max_port; p++) {
        addr.sin_port = htons((u_short)p);
        if (bind(br->listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            br->listen_port = p;
            bound = 1;
            break;
        }
    }
    if (!bound) {
        dbg_log("[MCP-DLL] ERROR: bind failed on ports %d-%d", br->base_port, br->max_port);
        closesocket(br->listen_sock);
        br->listen_sock = INVALID_SOCKET;
        br->running = 0;
        return 2;
    }

    if (listen(br->listen_sock, 1) != 0) {
        dbg_log("[MCP-DLL] ERROR: listen() failed");
        closesocket(br->listen_sock);
        br->listen_sock = INVALID_SOCKET;
        br->running = 0;
        return 3;
    }

    dbg_log("[MCP-DLL] Listening on %s:%d (THREADED mode)",
            br->bind_addr, br->listen_port);
    br->listening = 1;

    while (br->running) {
        fd_set rd;
        struct timeval tv;
        tv.tv_sec = SELECT_TIMEOUT_SEC;
        tv.tv_usec = 0;
        FD_ZERO(&rd);
        FD_SET(br->listen_sock, &rd);

        int sel = select(0, &rd, NULL, NULL, &tv);
        if (sel <= 0) continue;

        SOCKET cs = accept(br->listen_sock, NULL, NULL);
        if (cs == INVALID_SOCKET) continue;

        int one = 1;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        setsockopt(cs, SOL_SOCKET, SO_KEEPALIVE, (char*)&one, sizeof(one));

        br->client_sock = cs;
        br->connected = 1;
        dbg_log("[MCP-DLL] Client connected");

        while (br->running && br->connected) {
            int cmd_len = 0;
            char *cmd = tcp_recv_frame(cs, &cmd_len);
            if (!cmd) { br->connected = 0; break; }

            {
                char method[128];
                extract_json_method(cmd, method, sizeof(method));
                dbg_log("[MCP-DLL] CMD: %s", method[0] ? method : "(unknown)");

                int resp_len = 0;
                char *resp = execute_lua_handler(cmd, cmd_len, &resp_len);
                free(cmd);

                if (resp && resp_len > 0) {
                    dbg_log("[MCP-DLL] RSP: %s -> OK (%d bytes)", method[0] ? method : "(unknown)", resp_len);
                    tcp_send_frame(cs, resp, resp_len);
                    free(resp);
                } else {
                    dbg_log("[MCP-DLL] RSP: %s -> ERROR (no result)", method[0] ? method : "(unknown)");
                    const char *err = "{\"error\":\"Lua handler returned no result\"}";
                    tcp_send_frame(cs, err, (int)strlen(err));
                }
            }
        }

        dbg_log("[MCP-DLL] Client disconnected");
        closesocket(cs);
        br->client_sock = INVALID_SOCKET;
        br->connected = 0;
    }

    if (br->listen_sock != INVALID_SOCKET) {
        closesocket(br->listen_sock);
        br->listen_sock = INVALID_SOCKET;
    }
    br->listening = 0;
    br->running = 0;
    return 0;
}

/* Start TCP server (shared by both modes) */
static int start_tcp_server(int base_port, const char *bind_addr) {
    if (!wsa_startup()) {
        dbg_log("[MCP-DLL] ERROR: WSAStartup failed");
        return 0;
    }

    memset(&g_bridge, 0, sizeof(g_bridge));
    g_bridge.listen_sock = INVALID_SOCKET;
    g_bridge.client_sock = INVALID_SOCKET;
    g_bridge_initialized = 1;
    g_bridge.base_port = base_port;
    g_bridge.max_port = base_port + MAX_PORT_RANGE - 1;
    strncpy(g_bridge.bind_addr, bind_addr, sizeof(g_bridge.bind_addr) - 1);
    g_bridge.running = 1;

    g_bridge.thread = CreateThread(NULL, 0, tcp_server_thread, &g_bridge, 0, &g_bridge.thread_id);
    if (!g_bridge.thread) {
        g_bridge.running = 0;
        return 0;
    }

    for (int i = 0; i < 50 && g_bridge.running && !g_bridge.listening; i++)
        Sleep(100);

    return g_bridge.listening;
}

/* ---------- Lua-callable functions (native mode only) ---------- */

static int l_mcp_tcp_start(lua_State *L) {
    dbg_log("[MCP-DLL] mcp_tcp_start called");
    if (g_bridge.running) {
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 0);
        lua_setstrfield(L, -1, "err", "server already running");
        return 1;
    }

    int port = 17171;
    const char *bind = "0.0.0.0";
    if (lua_nargs(L) >= 1 && pL_isnumber && pL_isnumber(L, 1))
        port = (int)lua_getint(L, 1);
    if (lua_nargs(L) >= 2 && pL_isstring && pL_isstring(L, 2))
        bind = lua_getstr(L, 2);

    int ok = start_tcp_server(port, bind);

    lua_newtable(L);
    lua_setboolfield(L, -1, "ok", ok ? 1 : 0);
    lua_setintfield(L, -1, "port", g_bridge.listen_port);
    if (!ok)
        lua_setstrfield(L, -1, "err", "failed to bind port");
    return 1;
}

static int l_mcp_tcp_stop(lua_State *L) {
    if (!g_bridge_initialized) {
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 1);
        return 1;
    }

    dbg_log("[MCP-DLL] Stopping server...");
    g_bridge.running = 0;

    if (g_bridge.client_sock != INVALID_SOCKET) {
        shutdown(g_bridge.client_sock, SD_BOTH);
        closesocket(g_bridge.client_sock);
        g_bridge.client_sock = INVALID_SOCKET;
    }
    if (g_bridge.listen_sock != INVALID_SOCKET) {
        closesocket(g_bridge.listen_sock);
        g_bridge.listen_sock = INVALID_SOCKET;
    }
    if (g_bridge.thread) {
        WaitForSingleObject(g_bridge.thread, 5000);
        CloseHandle(g_bridge.thread);
        g_bridge.thread = NULL;
    }

    g_bridge.listening = 0;
    g_bridge.connected = 0;
    g_bridge_initialized = 0;

    g_threaded_mode = 0;
    g_lua_state = NULL;
    if (g_lua_cs_init) {
        DeleteCriticalSection(&g_lua_cs);
        g_lua_cs_init = 0;
    }

    dbg_log("[MCP-DLL] Server stopped");

    lua_newtable(L);
    lua_setboolfield(L, -1, "ok", 1);
    return 1;
}

static int l_mcp_tcp_set_threaded(lua_State *L) {
    int enable = 1;
    if (lua_nargs(L) >= 1 && pL_isnumber && pL_isnumber(L, 1))
        enable = (int)lua_getint(L, 1);

    if (enable) {
        if (!g_lua_cs_init) {
            InitializeCriticalSection(&g_lua_cs);
            g_lua_cs_init = 1;
        }
        g_lua_state = L;
        g_threaded_mode = 1;
        dbg_log("[MCP-DLL] Threaded mode ENABLED (Lua state: %p)", (void*)L);
    } else {
        g_threaded_mode = 0;
        g_lua_state = NULL;
        dbg_log("[MCP-DLL] Threaded mode DISABLED (legacy poll mode)");
    }

    lua_newtable(L);
    lua_setboolfield(L, -1, "ok", 1);
    lua_setboolfield(L, -1, "threaded", g_threaded_mode);
    return 1;
}

static int l_mcp_tcp_status(lua_State *L) {
    lua_newtable(L);
    lua_setboolfield(L, -1, "listening", g_bridge.listening);
    lua_setboolfield(L, -1, "connected", g_bridge.connected);
    lua_setintfield(L, -1, "port", g_bridge.listen_port);
    lua_setboolfield(L, -1, "running", g_bridge.running);
    lua_setboolfield(L, -1, "threaded", g_threaded_mode);
    return 1;
}

/* ---------- DLL Entry Point ---------- */

__declspec(dllexport) int luaopen_ce_mcp_tcp(lua_State *L) {
    dbg_log("[MCP-DLL] luaopen_ce_mcp_tcp called");

    if (!lua_api_ready && !resolve_lua_api()) {
        dbg_log("[MCP-DLL] FATAL: cannot resolve Lua API");
        dbg_log("[MCP-DLL] This CE build has Lua statically linked without exports.");
        dbg_log("[MCP-DLL] Check the log above for diagnostic details.");
        return 0;
    }

    /* ---- NATIVE LUA API MODE ---- */
    lua_register_func(L, "mcp_tcp_start",         l_mcp_tcp_start);
    lua_register_func(L, "mcp_tcp_stop",          l_mcp_tcp_stop);
    lua_register_func(L, "mcp_tcp_status",        l_mcp_tcp_status);
    lua_register_func(L, "mcp_tcp_set_threaded",  l_mcp_tcp_set_threaded);

    dbg_log("[MCP-DLL] Native mode: 4 Lua functions registered (threaded only)");

    lua_newtable(L);
    lua_setstrfield(L, -1, "version", "3.0.0");
    lua_setstrfield(L, -1, "transport", "native_tcp_threaded");
    return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self_module = hModule;
        dbg_init();
        dbg_log("[MCP-DLL] ce_mcp_tcp.dll loaded (v3.0.0 threaded)");
    }
    if (reason == DLL_PROCESS_DETACH) {
        dbg_log("[MCP-DLL] DLL unloading...");
        if (g_bridge.running) {
            g_bridge.running = 0;
            if (g_bridge.client_sock != INVALID_SOCKET) closesocket(g_bridge.client_sock);
            if (g_bridge.listen_sock != INVALID_SOCKET) closesocket(g_bridge.listen_sock);
        }
        if (g_logfp) fclose(g_logfp);
        FreeConsole();
    }
    return TRUE;
}
