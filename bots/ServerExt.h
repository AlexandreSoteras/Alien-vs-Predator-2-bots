// avp2bots server extension points: the hooks a private server extension can implement (events, bot names,
// per-bot spawn settings, a spectator client that never spawns). The public build links no-op versions
// (bots/ServerExtStub.cpp); a server's own extension replaces them. With the stub nothing changes.
#ifndef AVP2BOTS_SERVEREXT_H
#define AVP2BOTS_SERVEREXT_H

#include "ltbasedefs.h"

// Every server frame (top of BotMgr_Update).
void ServerExt_Update();

// Scoreboard name of a bot (species 0 marine, 1 alien, 2 predator; slot 1..n), or LTNULL for the default.
const char* ServerExt_BotName(int species, int slot);

// Called by BotMgr when a bot spawns; true = multiply its health and armor by the scales and, when
// *forcedClass is set, use that class. C linkage: BotMgr's weak default (/alternatename) works on the C name.
extern "C" bool ServerExt_BotSpawnOverride(int species, int slot, float* healthScale, float* armorScale, const char** forcedClass);

// Called by BotMgr when a bot spawns; true = this bot's skill, 0..1 (accuracy and sight speed; 0.5 = normal),
// instead of bots.cfg. C linkage, like BotSpawnOverride.
extern "C" bool ServerExt_BotSkill(int species, int slot, float* skill);

// MultiplayerMgr hooks (hooks srv*): a client added (after its unique name), left the world (at every world
// change too), renamed, chatting.
void ServerExt_ClientAdded(HCLIENT hClient, uint32 nClientID, const char* szName);
void ServerExt_ClientRemoved(HCLIENT hClient, uint32 nClientID, const char* szName);
// CGameServerShell::OnRemoveClient (hook srvdisconnect): the client really left the server.
void ServerExt_ClientDisconnected(HCLIENT hClient);
void ServerExt_ClientRenamed(uint32 nClientID, const char* szOld, const char* szNew);
void ServerExt_Chat(uint32 nClientID, const char* szName, const char* szText);

// A world has started (MultiplayerMgr::ServerAppPostStartWorld).
void ServerExt_WorldStarted();

// A character died (CDestructible::HandleDestruction). nDamageType: the DamageType of the last damage.
void ServerExt_Death(HOBJECT hVictim, HOBJECT hDamager, int nDamageType);

// True for a client that must never spawn and stays an observer (MultiplayerMgr::ValidateRespawn).
bool ServerExt_IsCameraClient(HCLIENT hClient);

// BotMgr read-only accessors (BotMgr.h)
int BotMgr_BotInfo(HOBJECT hObj, const char** display, int* species, int* slot);
int BotMgr_BotCount();
HOBJECT BotMgr_BotObject(int i);

#endif
