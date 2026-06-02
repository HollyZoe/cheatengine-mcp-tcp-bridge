/*
 * ce_mcp_tcp.dll - Native TCP Bridge for Cheat Engine MCP
 *
 * Loaded by CE Lua via package.loadlib(). Provides TCP server functionality
 * without requiring Winsock FFI, PEB walking, or executeCodeLocal.
 *
 * Lua functions registered:
 *   mcp_tcp_start(port, bind_addr)  -> {ok=bool, port=int, err=string}
 *   mcp_tcp_stop()                  -> {ok=bool}
 *   mcp_tcp_poll()                  -> json_string or nil
 *   mcp_tcp_respond(json_string)    -> {ok=bool, err=string}
 *   mcp_tcp_status()                -> {listening=bool, connected=bool, port=int}
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

/* ---------- Lua API runtime binding ---------- */

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State *L);
typedef long long lua_Integer;

/* Function pointers resolved at runtime from CE's Lua DLL */
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

#define LUA_TNONE     (-1)
#define LUA_TNIL       0
#define LUA_TBOOLEAN   1
#define LUA_TSTRING    4

static int resolve_lua_api(void) {
    HMODULE mod = NULL;
    const char* names[] = {
        "lua54.dll", "lua53.dll", "lua5.4.dll", "lua5.3.dll", NULL
    };

    for (int i = 0; names[i]; i++) {
        mod = GetModuleHandleA(names[i]);
        if (mod) break;
    }
    if (!mod) mod = GetModuleHandleA(NULL);
    if (!mod) return 0;

    pL_pushstring   = (void*)GetProcAddress(mod, "lua_pushstring");
    pL_pushinteger  = (void*)GetProcAddress(mod, "lua_pushinteger");
    pL_pushnil      = (void*)GetProcAddress(mod, "lua_pushnil");
    pL_pushboolean  = (void*)GetProcAddress(mod, "lua_pushboolean");
    pL_tolstring    = (void*)GetProcAddress(mod, "lua_tolstring");
    pL_tointegerx   = (void*)GetProcAddress(mod, "lua_tointegerx");
    pL_gettop       = (void*)GetProcAddress(mod, "lua_gettop");
    pL_settop       = (void*)GetProcAddress(mod, "lua_settop");
    pL_setglobal    = (void*)GetProcAddress(mod, "lua_setglobal");
    pL_pushcclosure = (void*)GetProcAddress(mod, "lua_pushcclosure");
    pL_isstring     = (void*)GetProcAddress(mod, "lua_isstring");
    pL_isnumber     = (void*)GetProcAddress(mod, "lua_isnumber");
    pL_createtable  = (void*)GetProcAddress(mod, "lua_createtable");
    pL_setfield     = (void*)GetProcAddress(mod, "lua_setfield");
    pL_getglobal    = (void*)GetProcAddress(mod, "lua_getglobal");
    pL_pcallk       = (void*)GetProcAddress(mod, "lua_pcallk");
    pL_error        = (void*)GetProcAddress(mod, "lua_error");

    if (!pL_pushstring || !pL_pushinteger || !pL_pushnil ||
        !pL_pushboolean || !pL_tolstring || !pL_tointegerx ||
        !pL_gettop || !pL_settop || !pL_setglobal ||
        !pL_pushcclosure || !pL_createtable || !pL_setfield) {
        return 0;
    }

    lua_api_ready = 1;
    return 1;
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

/* ---------- TCP Server ---------- */

#define MAX_CMD_SIZE   (4 * 1024 * 1024)   /* 4 MB max command */
#define MAX_RESP_SIZE  (4 * 1024 * 1024)
#define MAX_PORT_RANGE 10
#define SELECT_TIMEOUT_SEC 1

typedef struct {
    /* Server state */
    volatile int running;
    volatile int listening;
    volatile int connected;
    int listen_port;

    /* Sockets */
    SOCKET listen_sock;
    SOCKET client_sock;

    /* Thread */
    HANDLE thread;
    DWORD  thread_id;

    /* Command queue (single-slot: one command at a time) */
    CRITICAL_SECTION cs;
    char  *cmd_buf;
    int    cmd_len;
    volatile int cmd_ready;

    /* Response slot */
    char  *resp_buf;
    int    resp_len;
    volatile int resp_ready;
    HANDLE resp_event;

    /* Config */
    char bind_addr[64];
    int  base_port;
    int  max_port;
} TcpBridge;

static TcpBridge g_bridge = {0};
static int g_wsa_init = 0;

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
        tv.tv_usec = 200000; /* 200ms select timeout */

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

    if (tcp_recv_exact(s, buf, len, 600000) != len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static DWORD WINAPI tcp_server_thread(LPVOID param) {
    TcpBridge *br = (TcpBridge*)param;

    /* Create listening socket */
    br->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (br->listen_sock == INVALID_SOCKET) {
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
        closesocket(br->listen_sock);
        br->listen_sock = INVALID_SOCKET;
        br->running = 0;
        return 2;
    }

    if (listen(br->listen_sock, 1) != 0) {
        closesocket(br->listen_sock);
        br->listen_sock = INVALID_SOCKET;
        br->running = 0;
        return 3;
    }

    br->listening = 1;

    /* Main accept loop */
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

        /* Disable Nagle, enable keepalive */
        int one = 1;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        setsockopt(cs, SOL_SOCKET, SO_KEEPALIVE, (char*)&one, sizeof(one));

        br->client_sock = cs;
        br->connected = 1;

        /* Recv loop for this client */
        while (br->running && br->connected) {
            int cmd_len = 0;
            char *cmd = tcp_recv_frame(cs, &cmd_len);
            if (!cmd) {
                br->connected = 0;
                break;
            }

            /* Queue command for Lua to pick up */
            EnterCriticalSection(&br->cs);
            if (br->cmd_buf) free(br->cmd_buf);
            br->cmd_buf = cmd;
            br->cmd_len = cmd_len;
            br->cmd_ready = 1;
            LeaveCriticalSection(&br->cs);

            /* Wait for response from Lua (up to 120 seconds) */
            ResetEvent(br->resp_event);
            DWORD wait = WaitForSingleObject(br->resp_event, 120000);
            if (wait != WAIT_OBJECT_0 || !br->resp_ready) {
                /* Timeout or shutdown — send error response */
                const char *err = "{\"error\":\"timeout waiting for command handler\"}";
                tcp_send_frame(cs, err, (int)strlen(err));
                continue;
            }

            /* Send response */
            EnterCriticalSection(&br->cs);
            if (br->resp_buf && br->resp_len > 0) {
                tcp_send_frame(cs, br->resp_buf, br->resp_len);
                free(br->resp_buf);
                br->resp_buf = NULL;
                br->resp_len = 0;
            }
            br->resp_ready = 0;
            LeaveCriticalSection(&br->cs);
        }

        /* Client disconnected */
        closesocket(cs);
        br->client_sock = INVALID_SOCKET;
        br->connected = 0;
    }

    /* Cleanup */
    if (br->listen_sock != INVALID_SOCKET) {
        closesocket(br->listen_sock);
        br->listen_sock = INVALID_SOCKET;
    }
    br->listening = 0;
    br->running = 0;
    return 0;
}

/* ---------- Lua-callable functions ---------- */

/* mcp_tcp_start(port, bind_addr) -> {ok, port, err} */
static int l_mcp_tcp_start(lua_State *L) {
    if (g_bridge.running) {
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 0);
        lua_setstrfield(L, -1, "err", "server already running");
        return 1;
    }

    if (!wsa_startup()) {
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 0);
        lua_setstrfield(L, -1, "err", "WSAStartup failed");
        return 1;
    }

    int port = 17171;
    const char *bind = "0.0.0.0";

    if (lua_nargs(L) >= 1 && pL_isnumber && pL_isnumber(L, 1))
        port = (int)lua_getint(L, 1);
    if (lua_nargs(L) >= 2 && pL_isstring && pL_isstring(L, 2))
        bind = lua_getstr(L, 2);

    memset(&g_bridge, 0, sizeof(g_bridge));
    InitializeCriticalSection(&g_bridge.cs);
    g_bridge.resp_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_bridge.listen_sock = INVALID_SOCKET;
    g_bridge.client_sock = INVALID_SOCKET;
    g_bridge.base_port = port;
    g_bridge.max_port = port + MAX_PORT_RANGE - 1;
    strncpy(g_bridge.bind_addr, bind, sizeof(g_bridge.bind_addr) - 1);
    g_bridge.running = 1;

    g_bridge.thread = CreateThread(NULL, 0, tcp_server_thread, &g_bridge, 0, &g_bridge.thread_id);
    if (!g_bridge.thread) {
        g_bridge.running = 0;
        DeleteCriticalSection(&g_bridge.cs);
        CloseHandle(g_bridge.resp_event);
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 0);
        lua_setstrfield(L, -1, "err", "CreateThread failed");
        return 1;
    }

    /* Wait briefly for the server to start listening */
    for (int i = 0; i < 50 && g_bridge.running && !g_bridge.listening; i++)
        Sleep(100);

    lua_newtable(L);
    lua_setboolfield(L, -1, "ok", g_bridge.listening ? 1 : 0);
    lua_setintfield(L, -1, "port", g_bridge.listen_port);
    if (!g_bridge.listening)
        lua_setstrfield(L, -1, "err", "failed to bind port");
    return 1;
}

/* mcp_tcp_stop() -> {ok} */
static int l_mcp_tcp_stop(lua_State *L) {
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

    SetEvent(g_bridge.resp_event);

    if (g_bridge.thread) {
        WaitForSingleObject(g_bridge.thread, 5000);
        CloseHandle(g_bridge.thread);
        g_bridge.thread = NULL;
    }

    EnterCriticalSection(&g_bridge.cs);
    if (g_bridge.cmd_buf) { free(g_bridge.cmd_buf); g_bridge.cmd_buf = NULL; }
    if (g_bridge.resp_buf) { free(g_bridge.resp_buf); g_bridge.resp_buf = NULL; }
    g_bridge.cmd_ready = 0;
    g_bridge.resp_ready = 0;
    LeaveCriticalSection(&g_bridge.cs);

    DeleteCriticalSection(&g_bridge.cs);
    CloseHandle(g_bridge.resp_event);
    g_bridge.resp_event = NULL;
    g_bridge.listening = 0;
    g_bridge.connected = 0;

    lua_newtable(L);
    lua_setboolfield(L, -1, "ok", 1);
    return 1;
}

/* mcp_tcp_poll() -> json_string or nil */
static int l_mcp_tcp_poll(lua_State *L) {
    if (!g_bridge.cmd_ready) {
        lua_pushnothing(L);
        return 1;
    }

    EnterCriticalSection(&g_bridge.cs);
    if (g_bridge.cmd_ready && g_bridge.cmd_buf) {
        pL_pushstring(L, g_bridge.cmd_buf);
        free(g_bridge.cmd_buf);
        g_bridge.cmd_buf = NULL;
        g_bridge.cmd_len = 0;
        g_bridge.cmd_ready = 0;
    } else {
        lua_pushnothing(L);
    }
    LeaveCriticalSection(&g_bridge.cs);

    return 1;
}

/* mcp_tcp_respond(json_string) -> {ok, err} */
static int l_mcp_tcp_respond(lua_State *L) {
    if (lua_nargs(L) < 1 || !pL_isstring || !pL_isstring(L, 1)) {
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 0);
        lua_setstrfield(L, -1, "err", "expected string argument");
        return 1;
    }

    size_t len = 0;
    const char *data = pL_tolstring(L, 1, &len);
    if (!data || len == 0) {
        lua_newtable(L);
        lua_setboolfield(L, -1, "ok", 0);
        lua_setstrfield(L, -1, "err", "empty response");
        return 1;
    }

    EnterCriticalSection(&g_bridge.cs);
    if (g_bridge.resp_buf) free(g_bridge.resp_buf);
    g_bridge.resp_buf = (char*)malloc(len + 1);
    if (g_bridge.resp_buf) {
        memcpy(g_bridge.resp_buf, data, len);
        g_bridge.resp_buf[len] = '\0';
        g_bridge.resp_len = (int)len;
        g_bridge.resp_ready = 1;
    }
    LeaveCriticalSection(&g_bridge.cs);

    SetEvent(g_bridge.resp_event);

    lua_newtable(L);
    lua_setboolfield(L, -1, "ok", 1);
    return 1;
}

/* mcp_tcp_status() -> {listening, connected, port} */
static int l_mcp_tcp_status(lua_State *L) {
    lua_newtable(L);
    lua_setboolfield(L, -1, "listening", g_bridge.listening);
    lua_setboolfield(L, -1, "connected", g_bridge.connected);
    lua_setintfield(L, -1, "port", g_bridge.listen_port);
    lua_setboolfield(L, -1, "running", g_bridge.running);
    return 1;
}

/* ---------- DLL Entry Point ---------- */

__declspec(dllexport) int luaopen_ce_mcp_tcp(lua_State *L) {
    if (!lua_api_ready && !resolve_lua_api()) {
        /* Can't register functions without Lua API */
        return 0;
    }

    lua_register_func(L, "mcp_tcp_start",   l_mcp_tcp_start);
    lua_register_func(L, "mcp_tcp_stop",    l_mcp_tcp_stop);
    lua_register_func(L, "mcp_tcp_poll",    l_mcp_tcp_poll);
    lua_register_func(L, "mcp_tcp_respond", l_mcp_tcp_respond);
    lua_register_func(L, "mcp_tcp_status",  l_mcp_tcp_status);

    /* Return version info */
    lua_newtable(L);
    lua_setstrfield(L, -1, "version", "1.0.0");
    lua_setstrfield(L, -1, "transport", "native_tcp");
    return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    (void)hModule; (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        if (g_bridge.running) {
            g_bridge.running = 0;
            if (g_bridge.client_sock != INVALID_SOCKET)
                closesocket(g_bridge.client_sock);
            if (g_bridge.listen_sock != INVALID_SOCKET)
                closesocket(g_bridge.listen_sock);
            if (g_bridge.resp_event) SetEvent(g_bridge.resp_event);
        }
    }
    return TRUE;
}
