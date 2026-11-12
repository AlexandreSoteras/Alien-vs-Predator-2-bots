// avp2bots: client data of Master Server Patch 2.4 clients (see MspClient.cpp).
#ifndef AVP2BOTS_MSPCLIENT_H
#define AVP2BOTS_MSPCLIENT_H

#include "ltbasedefs.h"

// Unscrambles an MSP 2.4 client's data in place (once) and logs its name and GUID; other clients' data is left
// as it is. Called first thing in CGameServerShell::OnClientEnterWorld (hook mspclient).
void Msp_DecodeClientData(HCLIENT hClient, void* pClientData, uint32 clientDataLen);

// Reads the MSP 2.4 scrambling constant from lithserver.rez (once; later calls return the first result) and logs
// the outcome. Called at every world start (BotMgr_PostStartWorld) and before the first decode.
bool Msp_LoadKey();

// A player name the server app can show: printable ASCII, at most 24 characters, else "NoName".
void Msp_SanitizeName(char* name);

#endif
