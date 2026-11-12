// avp2bots: the server extension points (ServerExt.h) with nothing behind them.
//
// The game code and BotMgr call these functions at a few moments (a client joins, leaves, is renamed or chats; a
// world starts; a character dies; every server frame). A server that wants its own add-ons (an event log for a
// chat bridge, a spectator camera, fixed bot names...) builds its own ServerEvents.cpp instead of this file:
// server/CMakeLists.txt compiles ServerEvents.cpp and BotCamera.cpp when they exist, this file otherwise.
// ServerExt_BotSpawnOverride has its own default in BotMgr.cpp.

#include "stdafx.h"
#include "ServerExt.h"

void ServerExt_Update()
{
}

const char* ServerExt_BotName(int, int)
{
    return LTNULL;
}

void ServerExt_ClientAdded(HCLIENT, uint32, const char*)
{
}

void ServerExt_ClientRemoved(HCLIENT, uint32, const char*)
{
}

void ServerExt_ClientDisconnected(HCLIENT)
{
}

void ServerExt_ClientRenamed(uint32, const char*, const char*)
{
}

void ServerExt_Chat(uint32, const char*, const char*)
{
}

void ServerExt_WorldStarted()
{
}

void ServerExt_Death(HOBJECT, HOBJECT, int)
{
}

bool ServerExt_IsCameraClient(HCLIENT)
{
    return false;
}
