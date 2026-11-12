// avp2bots: client data of Master Server Patch 2.4 clients.
//
// The MSP 2.4 client (modified CSHELL.DLL) does not send the 68-byte MPClientData of the 2002 code: it sends
// 0x88 bytes, the first 0x68 of them scrambled, followed by a 32-byte key chosen per connection. The MSP server
// code (LITHSERVER.REZ) unscrambles them before the game code reads them. Our server code is built from the
// 2002 source and read the scrambled bytes as they were: a garbage player name, and a name without a line end
// froze AvP2Serv.exe (its CServerDlg::OnShellUpdate loop reads the names with getline(.., 30) and never checks
// the stream), so most joins hung the server (found 2026-11-11).
//
// Layout once unscrambled: 0x00 the 2002 MPClientData (name[32], password[16], class, sets[4], ignore taunts,
// server control, auto team switched), 0x44 u32 flags, 0x48 GUID (24 upper-case hex characters: the hardware
// ID of the MSP's guidlog.txt and admin system), 0x60 and 0x64 two u32 of unknown use.
//
// The scrambling also uses a 32-byte constant of the MSP. It is not in this file: Msp_LoadKey reads it, once, from
// the MSP's own server code (lithserver.rez, next to AvP2Serv.exe on every MSP 2.4 install), where the MSP decoder
// builds it on its stack with 32 one-byte stores; a hash checks the bytes found. Without it (no lithserver.rez,
// or another MSP version), MSP 2.4 players get the name "NoName" and the default class, and the server still runs.

#include "stdafx.h"
#include "MultiplayerMgrDefs.h"
#include "MspClient.h"
#include "BotMgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
const uint32 kMspDataLen = 0x88;   // what an MSP 2.4 client sends
const uint32 kMspScrambled = 0x68; // scrambled part, followed by the 32-byte key
const unsigned long long kMspStaticHash = 0xc9a4390075b768e0ULL; // FNV-1a 64 of the MSP constant
uint8 g_mspStatic[32];
int g_mspKeyState = 0; // 0 not looked for yet, 1 loaded, -1 not found

unsigned long long Fnv1a64(const uint8* p, size_t n)
{
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// 32 consecutive "mov byte ptr [esp+d+k], imm8" (C6 44 24 d+k imm8) whose 32 values have the right hash.
bool FindMspStatic(const uint8* d, size_t n, long* where)
{
    for (size_t i = 0; i + 32 * 5 <= n; ++i)
    {
        if (d[i] != 0xC6 || d[i + 1] != 0x44 || d[i + 2] != 0x24)
            continue;
        bool ok = true;
        for (int k = 1; k < 32 && ok; ++k)
        {
            const uint8* q = d + i + 5 * k;
            ok = q[0] == 0xC6 && q[1] == 0x44 && q[2] == 0x24 && q[3] == (uint8)(d[i + 3] + k);
        }
        if (!ok)
            continue;
        uint8 cand[32];
        for (int k = 0; k < 32; ++k)
            cand[k] = d[i + 5 * k + 4];
        if (Fnv1a64(cand, 32) != kMspStaticHash)
            continue;
        memcpy(g_mspStatic, cand, 32);
        *where = (long)i;
        return true;
    }
    return false;
}

bool KeyIsZero(const uint8* key)
{
    for (int i = 0; i < 32; ++i)
        if (key[i])
            return false;
    return true;
}

// The MSP server's checks once the data is readable.
void Sanitize(MPClientData* p)
{
    char* name = p->m_szName;
    name[MAX_MP_CLIENT_NAME_LENGTH - 1] = '\0';
    Msp_SanitizeName(name);
    if (p->m_nCharacterClass >= 4)
        p->m_nCharacterClass = 0;
    if ((uint32)p->m_bIgnoreTaunts > 1)
        p->m_bIgnoreTaunts = LTFALSE;
    p->m_bAutoTeamSwitched = LTFALSE;
}

// Unscrambles one copy in place, once: the key is zeroed afterwards (decoding with a zero key would scramble it).
bool DecodeOne(uint8* d)
{
    uint8* key = d + kMspScrambled;
    if (KeyIsZero(key))
        return false;
    if (Msp_LoadKey())
    {
        for (uint32 i = 0; i < kMspScrambled; ++i)
        {
            uint8 b = (uint8)(d[i] ^ g_mspStatic[i % 32]);
            b = (uint8)(b + 0x80);
            d[i] = (uint8)(b ^ key[i % 32]);
        }
    }
    else
    {
        // unreadable without the constant: defaults rather than garbage (no GUID, no password)
        memset(d, 0, kMspScrambled);
        strcpy((char*)d, "NoName");
    }
    memset(key, 0, 32);
    Sanitize((MPClientData*)d);
    return true;
}
} // namespace

bool Msp_LoadKey()
{
    if (g_mspKeyState)
        return g_mspKeyState > 0;
    g_mspKeyState = -1;
    static const char* const kFiles[] = {"lithserver.rez", "LITHSERVER.REZ"};
    for (int f = 0; f < 2; ++f)
    {
        FILE* fp = fopen(kFiles[f], "rb");
        if (!fp)
            continue;
        long size = 0;
        if (fseek(fp, 0, SEEK_END) == 0)
            size = ftell(fp);
        uint8* data = (size > 0 && size < 64L * 1048576L) ? (uint8*)malloc((size_t)size) : 0;
        bool read = data && fseek(fp, 0, SEEK_SET) == 0 && fread(data, 1, (size_t)size, fp) == (size_t)size;
        fclose(fp);
        long where = -1;
        bool found = read && FindMspStatic(data, (size_t)size, &where);
        free(data);
        if (found)
        {
            g_mspKeyState = 1;
            BotMgr_Log("MSPKEY read from %s (offset %ld): MSP 2.4 players' names and GUIDs are readable", kFiles[f],
                       where);
            return true;
        }
    }
    BotMgr_Log("MSPKEY not found: no lithserver.rez of the Master Server Patch 2.4 next to AvP2Serv.exe (or another "
               "version); MSP 2.4 players will be called NoName");
    return false;
}

void Msp_SanitizeName(char* name)
{
    // printable ASCII only (a line end or a control character in a name froze AvP2Serv.exe), at most 24
    // characters, no leading or trailing spaces; otherwise "NoName", as the MSP server does
    if (!name)
        return;
    name[24] = '\0';
    size_t len = strlen(name);
    while (len > 0 && name[len - 1] == ' ')
        name[--len] = '\0';
    size_t start = 0;
    while (name[start] == ' ')
        ++start;
    if (start)
        memmove(name, name + start, len - start + 1);
    bool ok = name[0] != '\0';
    for (const char* c = name; *c; ++c)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e)
            ok = false;
    if (!ok)
        strcpy(name, "NoName");
}

void Msp_DecodeClientData(HCLIENT hClient, void* pClientData, uint32 clientDataLen)
{
    // the data passed to OnClientEnterWorld, and the engine's stored copy if it is a different block
    uint8* done = 0;
    if (pClientData && clientDataLen == kMspDataLen && DecodeOne((uint8*)pClientData))
        done = (uint8*)pClientData;
    void* pStored = 0;
    uint32 nStored = 0;
    if (hClient && g_pLTServer->GetClientData(hClient, pStored, nStored) == LT_OK && pStored &&
        pStored != pClientData && nStored == kMspDataLen && DecodeOne((uint8*)pStored))
        done = (uint8*)pStored;
    if (!done)
        return;
    const MPClientData* p = (const MPClientData*)done;
    uint32 flags;
    memcpy(&flags, done + 0x44, 4);
    char guid[25];
    memcpy(guid, done + 0x48, 24);
    guid[24] = '\0';
    for (int i = 0; i < 24; ++i)
        if (!strchr("0123456789ABCDEF", guid[i]) || !guid[i])
            guid[i] = '?';
    BotMgr_Log("CLIENT id=%d name=%s class=%d guid=%s flags=%08lX%s", (int)g_pLTServer->GetClientID(hClient),
               p->m_szName, (int)p->m_nCharacterClass, guid, (unsigned long)flags,
               (flags & 0xF) ? " (the MSP server rejects such a client)" : "");
}
