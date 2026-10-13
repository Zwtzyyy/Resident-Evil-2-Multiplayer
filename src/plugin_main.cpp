// re2r_mp.dll - our RE2R co-op networking plugin.
//
// Architecture (see docs/ARCHITECTURE.md):
//   * The gameplay logic lives in Lua (ported from the reference mod's readable source).
//   * This plugin owns the socket and a shared RAM block. It publishes the block address to
//     reframework\data\re2r_mp_net.txt; the Lua reads/writes the block by address, exactly the
//     pattern the reference helper plugins use. No Lua<->C++ binding machinery required yet.
//   * Transport: custom UDP. Host-authoritative. LAN discovery + direct IP for the internet.
//
// MVP scope: two peers exchange a per-frame player snapshot (position, rotation, motion state,
// health, flags) and a liveness/handshake. World sync (enemies/doors/items) comes later.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cwchar>

#include "reframework/API.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr uint32_t kMagic = 0x32504D52; // "RMP2"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kDefaultPort = 27099;
constexpr uint16_t kDiscoveryPort = 27100;

enum PacketType : uint8_t {
    PKT_HELLO = 1,
    PKT_HELLO_ACK = 2,
    PKT_STATE = 3,
    PKT_BYE = 4,
    PKT_DISCOVER = 5,
    PKT_DISCOVER_ACK = 6,
    PKT_DOOR = 7,
    PKT_ITEM = 8,
};

enum NetState : uint32_t {
    NET_IDLE = 0,
    NET_HOSTING = 1,
    NET_CONNECTING = 2,
    NET_CONNECTED = 3,
    NET_ERROR = 4,
};

enum NetCmd : uint32_t {
    CMD_NONE = 0,
    CMD_HOST = 1,
    CMD_JOIN = 2,
    CMD_DISCONNECT = 3,
    CMD_SCAN = 4,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t type;
    uint8_t flags;
    uint32_t seq;
};

struct PlayerSnapshot {
    float pos[3];
    float rot[4];       // quaternion (x, y, z, w)
    uint32_t motion;    // motion-FSM state hash / id
    uint32_t flags;     // bitfield (aiming, downed, sprinting, ...)
    uint32_t health;    // scaled int
    float aim[3];       // world point under the local player's crosshair (who this machine aims at)
    uint32_t aim_valid; // 1 when a crosshair point is available
};

// Canonical input, exactly the mod's virtual-pad state: buttons is the via.hid-style bitfield
// the game's getGamePadButton masks expect, axes are signed left/right sticks (-1..1) then the
// two trigger levels (0..1). Written by Lua on the sending machine, consumed by Lua (virtual
// pad) on the receiving one, so both peers agree on button identity regardless of controller.
#pragma pack(push, 1)
struct PlayerInput {
    uint32_t buttons;
    float axes[6];      // lx, ly, rx, ry, lt, rt
};
#pragma pack(pop)

struct StatePacket {
    PacketHeader h;
    PlayerSnapshot s;
    PlayerInput in;
};

struct DoorPacket {
    PacketHeader h;
    uint32_t state;  // 2 = OPEN
    char name[64];   // Deterministic per-map door id (GameObject name).
};

struct ItemPacket {
    PacketHeader h;
    float pos[3];    // to disambiguate items that share a prefab/GameObject name
    char name[64];   // world item id (GameObject name)
};
#pragma pack(pop)

// ---- Shared RAM block (published to the Lua by address) -----------------------------------
struct NetBlock {
    // control: Lua -> plugin
    volatile uint32_t cmd;
    volatile uint32_t port;
    volatile uint32_t scan_ports; // unused for now
    char ip[64];

    // status: plugin -> Lua
    volatile uint32_t state;
    volatile int32_t error;
    volatile uint32_t peer;
    volatile uint32_t tick;
    char status[128];

    // local snapshot: Lua writes, plugin sends (plain POD; torn reads are tolerated for MVP)
    PlayerSnapshot local;
    PlayerInput local_input;

    // remote snapshot: plugin writes, Lua reads
    volatile uint32_t remote_valid;
    uint32_t remote_seq;
    PlayerSnapshot remote;
    volatile uint32_t remote_input_valid;
    uint32_t remote_input_seq;
    PlayerInput remote_input;

    // door events: Lua queues (writes name/state then sets out_valid=1); plugin ships them
    volatile uint32_t door_out_valid;
    volatile uint32_t door_out_state;
    char door_out_name[64];

    // door events: plugin serves incoming into these; Lua reads + clears in_valid
    volatile uint32_t door_in_valid;
    uint32_t door_in_seq;
    volatile uint32_t door_in_state;
    char door_in_name[64];

    // world-item events (a pickup removed an item): Lua queues (out), plugin serves (in)
    volatile uint32_t item_out_valid;
    char item_out_name[64];
    float item_out_pos[3];
    volatile uint32_t item_in_valid;
    uint32_t item_in_seq;
    char item_in_name[64];
    float item_in_pos[3];

    // LAN scan results: plugin writes
    volatile uint32_t found_count;
    char found[8][64]; // "ip:port"
} g_block{};

volatile LONG g_run = 0;
HANDLE g_thread = nullptr;
const REFrameworkPluginFunctions* g_fn = nullptr;

void log_info(const char* msg) { if (g_fn && g_fn->log_info) g_fn->log_info("[re2r_mp] %s", msg); }

void set_status(const char* s)
{
    strncpy_s((char*)g_block.status, sizeof(g_block.status), s, _TRUNCATE);
}

void game_root(wchar_t* out, size_t cap)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&game_root, &self);
    out[0] = L'\0';
    if (self == nullptr || GetModuleFileNameW(self, out, (DWORD)cap) == 0)
        return;
    wchar_t* p = wcsstr(out, L"\\reframework\\");
    if (p != nullptr) { *(p + 1) = L'\0'; return; }
    int cuts = 3;
    while (cuts-- > 0) {
        wchar_t* last = wcsrchr(out, L'\\');
        if (last == nullptr || last == out) break;
        *last = L'\0';
    }
    size_t n = wcslen(out);
    if (n + 1 < cap) { out[n] = L'\\'; out[n + 1] = L'\0'; }
}

void publish_block_address()
{
    wchar_t base[1024];
    game_root(base, 1024);
    wchar_t path[1152];
    swprintf(path, 1152, L"%lsreframework\\data\\re2r_mp_net.txt", base);
    FILE* f = _wfopen(path, L"wb");
    if (f == nullptr)
        return;
    const NetBlock* b = &g_block;
    fprintf(f, "addr %llu\n", (unsigned long long)(uintptr_t)&g_block);
    fprintf(f, "version %u\n", (unsigned)kVersion);
    // Field offsets so the Lua never has to hardcode the C struct layout.
    fprintf(f, "off_cmd %u\n", (unsigned)offsetof(NetBlock, cmd));
    fprintf(f, "off_port %u\n", (unsigned)offsetof(NetBlock, port));
    fprintf(f, "off_ip %u\n", (unsigned)offsetof(NetBlock, ip));
    fprintf(f, "off_state %u\n", (unsigned)offsetof(NetBlock, state));
    fprintf(f, "off_error %u\n", (unsigned)offsetof(NetBlock, error));
    fprintf(f, "off_peer %u\n", (unsigned)offsetof(NetBlock, peer));
    fprintf(f, "off_tick %u\n", (unsigned)offsetof(NetBlock, tick));
    fprintf(f, "off_status %u\n", (unsigned)offsetof(NetBlock, status));
    fprintf(f, "off_local %u\n", (unsigned)offsetof(NetBlock, local));
    fprintf(f, "off_local_pos %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, pos)));
    fprintf(f, "off_local_rot %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, rot)));
    fprintf(f, "off_local_motion %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, motion)));
    fprintf(f, "off_local_flags %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, flags)));
    fprintf(f, "off_local_health %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, health)));
    fprintf(f, "off_local_aim %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, aim)));
    fprintf(f, "off_local_aim_valid %u\n", (unsigned)(offsetof(NetBlock, local) + offsetof(PlayerSnapshot, aim_valid)));
    fprintf(f, "off_local_input %u\n", (unsigned)offsetof(NetBlock, local_input));
    fprintf(f, "off_local_input_buttons %u\n", (unsigned)(offsetof(NetBlock, local_input) + offsetof(PlayerInput, buttons)));
    fprintf(f, "off_local_input_axes %u\n", (unsigned)(offsetof(NetBlock, local_input) + offsetof(PlayerInput, axes)));
    fprintf(f, "off_remote %u\n", (unsigned)offsetof(NetBlock, remote));
    fprintf(f, "off_remote_valid %u\n", (unsigned)offsetof(NetBlock, remote_valid));
    fprintf(f, "off_remote_seq %u\n", (unsigned)offsetof(NetBlock, remote_seq));
    fprintf(f, "off_remote_input %u\n", (unsigned)offsetof(NetBlock, remote_input));
    fprintf(f, "off_remote_input_buttons %u\n", (unsigned)(offsetof(NetBlock, remote_input) + offsetof(PlayerInput, buttons)));
    fprintf(f, "off_remote_input_axes %u\n", (unsigned)(offsetof(NetBlock, remote_input) + offsetof(PlayerInput, axes)));
    fprintf(f, "off_remote_input_valid %u\n", (unsigned)offsetof(NetBlock, remote_input_valid));
    fprintf(f, "off_remote_aim %u\n", (unsigned)(offsetof(NetBlock, remote) + offsetof(PlayerSnapshot, aim)));
    fprintf(f, "off_remote_aim_valid %u\n", (unsigned)(offsetof(NetBlock, remote) + offsetof(PlayerSnapshot, aim_valid)));
    fprintf(f, "off_door_out_valid %u\n", (unsigned)offsetof(NetBlock, door_out_valid));
    fprintf(f, "off_door_out_state %u\n", (unsigned)offsetof(NetBlock, door_out_state));
    fprintf(f, "off_door_out_name %u\n", (unsigned)offsetof(NetBlock, door_out_name));
    fprintf(f, "off_door_in_valid %u\n", (unsigned)offsetof(NetBlock, door_in_valid));
    fprintf(f, "off_door_in_seq %u\n", (unsigned)offsetof(NetBlock, door_in_seq));
    fprintf(f, "off_door_in_state %u\n", (unsigned)offsetof(NetBlock, door_in_state));
    fprintf(f, "off_door_in_name %u\n", (unsigned)offsetof(NetBlock, door_in_name));
    fprintf(f, "off_item_out_valid %u\n", (unsigned)offsetof(NetBlock, item_out_valid));
    fprintf(f, "off_item_out_name %u\n", (unsigned)offsetof(NetBlock, item_out_name));
    fprintf(f, "off_item_out_pos %u\n", (unsigned)offsetof(NetBlock, item_out_pos));
    fprintf(f, "off_item_in_valid %u\n", (unsigned)offsetof(NetBlock, item_in_valid));
    fprintf(f, "off_item_in_seq %u\n", (unsigned)offsetof(NetBlock, item_in_seq));
    fprintf(f, "off_item_in_name %u\n", (unsigned)offsetof(NetBlock, item_in_name));
    fprintf(f, "off_item_in_pos %u\n", (unsigned)offsetof(NetBlock, item_in_pos));
    fprintf(f, "off_found_count %u\n", (unsigned)offsetof(NetBlock, found_count));
    fprintf(f, "off_found %u\n", (unsigned)offsetof(NetBlock, found));
    (void)b;
    fclose(f);
}

SOCKET make_socket(uint16_t bind_port, bool reuse)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind_port != 0) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(bind_port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    (void)reuse;
    return s;
}

uint32_t next_seq = 1;
PacketHeader make_header(uint8_t type)
{
    PacketHeader h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.type = type;
    h.seq = next_seq++;
    return h;
}

// ---------------------------------------------------------------------------------------------
struct HostSession {
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in peer{};
    bool has_peer = false;
};
struct ClientSession {
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in host{};
    bool connected = false;
    ULONGLONG last_hello = 0;
};

void send_state(SOCKET sock, const sockaddr_in* to)
{
    StatePacket p{};
    p.h = make_header(PKT_STATE);
    p.s = g_block.local;
    p.in = g_block.local_input;
    sendto(sock, (const char*)&p, sizeof(p), 0, (const sockaddr*)to, sizeof(*to));
}

void store_remote(const StatePacket& p)
{
    g_block.remote = p.s;
    g_block.remote_seq = p.h.seq;
    g_block.remote_valid = 1;
    g_block.remote_input = p.in;
    g_block.remote_input_seq = p.h.seq;
    g_block.remote_input_valid = 1;
}

void send_door(SOCKET sock, const sockaddr_in* to)
{
    if (g_block.door_out_valid != 1)
        return;
    DoorPacket p{};
    p.h = make_header(PKT_DOOR);
    p.state = g_block.door_out_state;
    strncpy_s(p.name, sizeof(p.name), g_block.door_out_name, _TRUNCATE);
    g_block.door_out_valid = 0;
    sendto(sock, (const char*)&p, sizeof(p), 0, (const sockaddr*)to, sizeof(*to));
}

void recv_door(const void* buf)
{
    DoorPacket* p = (DoorPacket*)buf;
    g_block.door_in_state = p->state;
    strncpy_s(g_block.door_in_name, sizeof(g_block.door_in_name), p->name, _TRUNCATE);
    g_block.door_in_seq = p->h.seq;
    g_block.door_in_valid = 1;
}

void send_item(SOCKET sock, const sockaddr_in* to)
{
    if (g_block.item_out_valid != 1)
        return;
    ItemPacket p{};
    p.h = make_header(PKT_ITEM);
    strncpy_s(p.name, sizeof(p.name), g_block.item_out_name, _TRUNCATE);
    p.pos[0] = g_block.item_out_pos[0];
    p.pos[1] = g_block.item_out_pos[1];
    p.pos[2] = g_block.item_out_pos[2];
    g_block.item_out_valid = 0;
    sendto(sock, (const char*)&p, sizeof(p), 0, (const sockaddr*)to, sizeof(*to));
}

void recv_item(const void* buf)
{
    ItemPacket* p = (ItemPacket*)buf;
    strncpy_s(g_block.item_in_name, sizeof(g_block.item_in_name), p->name, _TRUNCATE);
    g_block.item_in_pos[0] = p->pos[0];
    g_block.item_in_pos[1] = p->pos[1];
    g_block.item_in_pos[2] = p->pos[2];
    g_block.item_in_seq = p->h.seq;
    g_block.item_in_valid = 1;
}

void do_host()
{
    HostSession hs;
    hs.sock = make_socket((uint16_t)g_block.port, true);
    if (hs.sock == INVALID_SOCKET) {
        g_block.state = NET_ERROR;
        g_block.error = WSAGetLastError();
        set_status("host: bind failed");
        return;
    }
    g_block.state = NET_HOSTING;
    set_status("listening for a peer");
    log_info("hosting");

    char buf[512];
    ULONGLONG last_send = 0;
    while (InterlockedCompareExchange(&g_run, 1, 1) == 1 && g_block.cmd != CMD_DISCONNECT) {
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(hs.sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n >= (int)sizeof(PacketHeader)) {
            PacketHeader* h = (PacketHeader*)buf;
            if (h->magic == kMagic && h->version == kVersion) {
                if (h->type == PKT_HELLO) {
                    hs.peer = from; hs.has_peer = true;
                    PacketHeader ack = make_header(PKT_HELLO_ACK);
                    sendto(hs.sock, (const char*)&ack, sizeof(ack), 0, (sockaddr*)&from, sizeof(from));
                    g_block.state = NET_CONNECTED; g_block.peer = 1;
                    set_status("peer connected");
                } else if (h->type == PKT_STATE && n >= (int)sizeof(StatePacket)) {
                    store_remote(*(StatePacket*)buf);
                } else if (h->type == PKT_DOOR && n >= (int)sizeof(DoorPacket)) {
                    recv_door(buf);
                } else if (h->type == PKT_ITEM && n >= (int)sizeof(ItemPacket)) {
                    recv_item(buf);
                }
            }
        }
        ULONGLONG now = GetTickCount64();
        if (hs.has_peer) {
            send_door(hs.sock, &hs.peer);
            send_item(hs.sock, &hs.peer);
            if (now - last_send >= 16) {
                last_send = now;
                send_state(hs.sock, &hs.peer);
            }
        }
        ++g_block.tick;
        Sleep(3);
    }
    if (hs.has_peer) {
        PacketHeader bye = make_header(PKT_BYE);
        sendto(hs.sock, (const char*)&bye, sizeof(bye), 0, (sockaddr*)&hs.peer, sizeof(hs.peer));
    }
    closesocket(hs.sock);
    g_block.state = NET_IDLE; g_block.peer = 0; g_block.remote_valid = 0; g_block.remote_input_valid = 0;
    g_block.door_out_valid = 0; g_block.door_in_valid = 0; g_block.item_out_valid = 0; g_block.item_in_valid = 0;
    set_status("idle");
}

void do_join(const char* ip, uint16_t port)
{
    ClientSession cs;
    cs.sock = make_socket(0, false);
    if (cs.sock == INVALID_SOCKET) {
        g_block.state = NET_ERROR; g_block.error = WSAGetLastError();
        set_status("join: socket failed");
        return;
    }
    cs.host.sin_family = AF_INET;
    cs.host.sin_port = htons(port);
    inet_pton(AF_INET, ip, &cs.host.sin_addr);
    g_block.state = NET_CONNECTING;
    set_status("connecting");

    char buf[512];
    ULONGLONG last_send = 0;
    while (InterlockedCompareExchange(&g_run, 1, 1) == 1 && g_block.cmd != CMD_DISCONNECT) {
        ULONGLONG now = GetTickCount64();
        if (now - last_send >= (cs.connected ? 16 : 250)) {
            last_send = now;
            if (!cs.connected) {
                PacketHeader hello = make_header(PKT_HELLO);
                sendto(cs.sock, (const char*)&hello, sizeof(hello), 0, (sockaddr*)&cs.host, sizeof(cs.host));
            } else {
                send_state(cs.sock, &cs.host);
            }
        }
        if (cs.connected)
            send_door(cs.sock, &cs.host);
        if (cs.connected)
            send_item(cs.sock, &cs.host);
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(cs.sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n >= (int)sizeof(PacketHeader)) {
            PacketHeader* h = (PacketHeader*)buf;
            if (h->magic == kMagic && h->version == kVersion) {
                if (h->type == PKT_HELLO_ACK && !cs.connected) {
                    cs.connected = true;
                    g_block.state = NET_CONNECTED; g_block.peer = 1;
                    set_status("connected");
                } else if (h->type == PKT_STATE && n >= (int)sizeof(StatePacket)) {
                    store_remote(*(StatePacket*)buf);
                } else if (h->type == PKT_DOOR && n >= (int)sizeof(DoorPacket)) {
                    recv_door(buf);
                } else if (h->type == PKT_ITEM && n >= (int)sizeof(ItemPacket)) {
                    recv_item(buf);
                } else if (h->type == PKT_BYE) {
                    break;
                }
            }
        }
        ++g_block.tick;
        Sleep(3);
    }
    closesocket(cs.sock);
    g_block.state = NET_IDLE; g_block.peer = 0; g_block.remote_valid = 0; g_block.remote_input_valid = 0;
    g_block.door_out_valid = 0; g_block.door_in_valid = 0; g_block.item_out_valid = 0; g_block.item_in_valid = 0;
    set_status("idle");
}

void do_scan()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    sockaddr_in bc{};
    bc.sin_family = AF_INET;
    bc.sin_port = htons(kDiscoveryPort);
    bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    PacketHeader q = make_header(PKT_DISCOVER);
    sendto(s, (const char*)&q, sizeof(q), 0, (sockaddr*)&bc, sizeof(bc));

    g_block.found_count = 0;
    DWORD start = GetTickCount();
    char buf[512];
    while (GetTickCount() - start < 400) {
        DWORD tv = 20;
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval t{(long)(tv / 1000), (long)((tv % 1000) * 1000)};
        if (select(0, &rd, nullptr, nullptr, &t) > 0) {
            sockaddr_in from{}; int fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n >= (int)sizeof(PacketHeader)) {
                PacketHeader* h = (PacketHeader*)buf;
                if (h->magic == kMagic && h->type == PKT_DISCOVER_ACK) {
                    if (g_block.found_count < 8) {
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                        snprintf((char*)g_block.found[g_block.found_count], 64, "%s:%u", ip, ntohs(from.sin_port));
                        ++g_block.found_count;
                    }
                }
            }
        }
    }
    closesocket(s);
    g_block.cmd = CMD_NONE;
}

DWORD WINAPI worker(LPVOID)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        g_block.state = NET_ERROR; g_block.error = WSAGetLastError();
        set_status("WSAStartup failed");
        return 0;
    }
    publish_block_address();
    log_info("net thread started");

    while (InterlockedCompareExchange(&g_run, 1, 1) == 1) {
        uint32_t cmd = g_block.cmd;
        if (cmd == CMD_HOST) {
            g_block.cmd = CMD_NONE;
            if (g_block.port == 0) g_block.port = kDefaultPort;
            do_host();
        } else if (cmd == CMD_JOIN) {
            g_block.cmd = CMD_NONE;
            uint16_t port = g_block.port ? (uint16_t)g_block.port : kDefaultPort;
            do_join((const char*)g_block.ip, port);
        } else if (cmd == CMD_SCAN) {
            g_block.cmd = CMD_NONE;
            do_scan();
        }
        Sleep(10);
    }
    WSACleanup();
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version)
{
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param)
{
    g_fn = param->functions;
    g_block.port = kDefaultPort;
    set_status("idle");
    InterlockedExchange(&g_run, 1);
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    log_info("re2r_mp plugin initialized");
    return true;
}
