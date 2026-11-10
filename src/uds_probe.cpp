#include "uds_probe.hpp"

#include <cstring>

namespace azp {

namespace {
Handle g_uds = 0;
bool g_cfg_ready = false;

// UTF-16 (solo piano base) -> UTF-8: i nomi giapponesi/accentati si vedono con il font di sistema; le coppie surrogate diventano '?'.
void Utf16ToAscii(const u16* in, int n, char* out, int out_size) {
    int o = 0;
    for (int i = 0; i < n && in[i]; ++i) {
        const u16 c = in[i];
        if (c < 0x80) {
            if (o + 1 >= out_size) break;
            out[o++] = static_cast<char>(c);
        } else if (c < 0x800) {
            if (o + 2 >= out_size) break;
            out[o++] = static_cast<char>(0xC0 | (c >> 6));
            out[o++] = static_cast<char>(0x80 | (c & 0x3F));
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            if (o + 1 >= out_size) break;
            out[o++] = '?';
        } else {
            if (o + 3 >= out_size) break;
            out[o++] = static_cast<char>(0xE0 | (c >> 12));
            out[o++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out[o++] = static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}
} // namespace

bool UdsOpen() {
    if (g_uds) {
        return true;
    }
    Handle h = 0;
    if (R_FAILED(srvGetServiceHandle(&h, "nwm::UDS"))) {
        return false;
    }
    g_uds = h;
    return true;
}

void UdsClose() {
    if (g_uds) {
        svcCloseHandle(g_uds);
        g_uds = 0;
    }
}

bool UdsRead(UdsSnapshot* out) {
    *out = UdsSnapshot();
    if (!g_uds) {
        return false;
    }
    out->service_ok = true;
    for (u16 id = 1; id <= 8; ++id) {
        u32* cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(0x000D, 1, 0); // GetNodeInformation(u16 network_node_id)
        cmd[1] = id;
        if (R_FAILED(svcSendSyncRequest(g_uds))) {
            return true;
        }
        const Result rc = static_cast<Result>(cmd[1]);
        if (R_FAILED(rc)) {
            if (id == 1) {
                out->first_error = rc; // 0xD8... NotInitialized / NotFound: il gioco non ha ancora una partita
            }
            continue;
        }
        // NodeInfo (40 byte): u64 friend_code_seed, u16 username[10], 4 byte, u16 network_node_id, 6 byte
        u8 raw[40];
        std::memcpy(raw, &cmd[2], sizeof(raw));
        UdsNode& n = out->nodes[out->count++];
        std::memcpy(&n.seed, raw, 8);
        u16 name[10];
        std::memcpy(name, raw + 8, 20);
        Utf16ToAscii(name, 10, n.name, sizeof(n.name));
        std::memcpy(&n.id, raw + 32, 2);
    }
    return true;
}

bool UdsMyName(char* out, int size) {
    if (!g_cfg_ready) {
        if (R_FAILED(cfguInit())) {
            return false;
        }
        g_cfg_ready = true;
    }
    u16 block[0x1C / 2] = {};
    if (R_FAILED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, block))) { // blocco "nome utente"
        return false;
    }
    Utf16ToAscii(block, 10, out, size);
    return true;
}

} // namespace azp
