#include "i_net.h"
#include "basics.h"
#include "engineerrors.h"
#include "printf.h"
#include "version.h"
#include "widgets/netstartwindow.h"

#include <steam/steam_api.h>

FARG(host, "Multiplayer", "Designates the machine as the host for a multiplayer game.", "x",
     "This machine will function as a host for a multiplayer game with x players (including this"
     " machine). It will wait for other machines to connect using the -join. parameter and then"
     " start the game when everyone is connected.");
FARG(join, "Multiplayer", "Connects to a multiplayer host.", "host's IP address[:host's port]",
     "Connect to a host for a multiplayer game.");
FARG(dup, "Multiplayer", "Send less player movement commands over the network.", "x",
     "Causes " GAMENAME " to transmit fewer player movement commands across the network. Valid"
     " values range from 1–9. For example, -dup 2 would cause " GAMENAME " to send half as many"
     " movements as normal.");
FARG(port, "Multiplayer", "Specifies an alternative IP port for a network game.", "x",
     "Specifies an alternate IP port for this machine to use during a network game. By default,"
     " port 5029 is used.");
FARG(password, "", "", "", "");

struct FConnection
{
	// EConnectionStatus Status = CSTAT_NONE;
	// sockaddr_in Address = {};
	uint64_t SteamID = 0u;
	uint64_t InfoAck = 0u;
	bool bHasGameInfo = false;

	void Clear()
	{
		// Status = CSTAT_NONE;
		// Address = {};
		InfoAck = 0u;
		bHasGameInfo = false;
	}
};

bool         netgame        = false;
bool         multiplayer    = false;
int          consoleplayer  = 0;
int          Net_Arbitrator = 0;
FClientStack NetworkClients = {};

uint8_t TicDup                = 1u;
int     MaxClients            = 1;
int     RemoteClient          = -1;
size_t  NetBufferLength       = 0u;
uint8_t NetBuffer[MAX_MSGLEN] = {};

static bool        bGameStarted          = false;
static FConnection Connected[MAXPLAYERS] = {};

// Game-specific API
size_t             Net_SetEngineInfo(uint8_t *&stream);
FVerificationError Net_VerifyEngine(uint8_t *&stream, size_t &offset);
void               Net_SetupUserInfo();
const char        *Net_GetClientName(int client, unsigned int charLimit);
void               Net_SetUserInfo(int client, TArrayView<uint8_t> &stream);
void               Net_ReadUserInfo(int client, TArrayView<uint8_t> &stream);
void               Net_ReadGameInfo(TArrayView<uint8_t> &stream);
void               Net_SetGameInfo(TArrayView<uint8_t> &stream);

enum EConnectionFlags : unsigned int
{
	CFL_NONE          = 0,
	CFL_CONSOLEPLAYER = 1,
	CFL_HOST          = 1 << 1,
};

enum EConnectionStatus
{
	CSTAT_NONE,       // Guest isn't connected
	CSTAT_CONNECTING, // Guest is trying to connect
	CSTAT_WAITING,    // Guest is waiting for game info
	CSTAT_READY,      // Guest is ready to start the game
};

static void I_NetClientConnected(int client, unsigned int charLimit = 0u)
{
	Printf("NetLobby:: Client '%s' connected.\n", Net_GetClientName(client, 0u));

	const char  *name  = Net_GetClientName(client, charLimit);
	unsigned int flags = CFL_NONE;
	if (client == 0)
		flags |= CFL_HOST;
	if (client == consoleplayer)
		flags |= CFL_CONSOLEPLAYER;

	NetStartWindow::NetConnect(client, name, flags, CSTAT_READY);
}

static bool I_NetLoop(bool (*loopCallback)(void *), void *data)
{
	return NetStartWindow::NetLoop(loopCallback, data);
}

static bool Host_CheckForConnections(void *connected)
{
	// Ensure everyone is ready before we start the game.
	// Note, player data is handled by other callbacks
	return false;
}

static void HostGame(int arg)
{
	if (arg >= Args->NumArgs() || !(MaxClients = atoi(Args->GetArg(arg))))
	{ // No player count specified, assume 2
		MaxClients = 2u;
	}

	if ((unsigned)MaxClients > MAXPLAYERS)
		I_FatalError("Cannot host a game with %u players. The limit is currently %lu", MaxClients, MAXPLAYERS);

	NetworkClients += 0;
	Net_SetupUserInfo();

	// If only 1 player, don't bother starting the network
	if (MaxClients == 1)
	{
		TicDup      = 1u;
		multiplayer = true;

		bGameStarted = true;
		return;
	}

	// Originally StartNetwork
	netgame     = true;
	multiplayer = true;

	// Start Steam Lobby. The below should be a callback
	SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, MaxClients);

	Printf("NetLobby:: Waiting for players...\n");
	NetStartWindow::NetInit("Waiting for players...", true);
	NetStartWindow::NetProgress(0u, MaxClients);
	I_NetClientConnected(0u, 16u);

	// Wait for all players to join.
	int connectedPlayers = 1;
	if (!I_NetLoop(Host_CheckForConnections, (void *)&connectedPlayers))
	{
		// TODO: Close Lobby
		throw CExitEvent(0);
	}

	bGameStarted = true;
}

static bool JoinGame(int arg)
{


}

bool I_InitNetwork()
{
	const char *v = Args->CheckValue(FArg_dup);
	if (v != nullptr)
		TicDup = clamp<int>(atoi(v), 1, MAXTICDUP);

	int arg = -1;
	if ((arg = Args->CheckParm(FArg_host)))
	{
		HostGame(arg + 1);
		return true;
		// if (!HostGame(arg + 1))
		// return false;
	}
	else if ((arg = Args->CheckParm(FArg_join)))
	{
		if (!JoinGame(arg + 1))
			return false;
	}
	else
	{
		// single player game
		TicDup = 1;
		NetworkClients += 0;
		Net_SetupUserInfo();
	}

	bGameStarted = true;
	return true;
}

void CloseNetwork()
{
}

void I_ClearClient(unsigned long client_id)
{
}

void HandleIncomingConnection()
{
}

void I_NetCmd(ENetCommand cmd)
{
	if (cmd == CMD_SEND)
	{
		if (RemoteClient >= 0)
		{
		}
	}
	else if (cmd == CMD_GET)
	{
	}
}

// Print a network-related message to the console. This doesn't print to the window so should
// not be used for that and is mainly for logging.
static void I_NetLog(const char* text, ...)
{
	// todo: use better abstraction once everything is migrated to in-game start screens.
#if defined _WIN32 || defined __APPLE__
	va_list ap;
	va_start(ap, text);
	VPrintf(PRINT_HIGH, text, ap);
	Printf("\n");
	va_end(ap);
#else
	FString str;
	va_list argptr;

	va_start(argptr, text);
	str.VFormat(text, argptr);
	va_end(argptr);
	fprintf(stderr, "\r%-40s\n", str.GetChars());
#endif
}

class CallbackHandler
{
  private:
	STEAM_CALLBACK(CallbackHandler, OnCreateLobby, LobbyCreated_t);
	STEAM_CALLBACK(CallbackHandler, OnLobbyChatUpdate, LobbyChatUpdate_t);
};

void CallbackHandler::OnCreateLobby(LobbyCreated_t *cb)
{
	Printf("Started a steam lobby with id: %d.\n", cb->m_ulSteamIDLobby);
}

static void AddClientConnection(uint64_t steamID, int client)
{
	// Connected[client].Status = CSTAT_CONNECTING;
	Connected[client].SteamID = steamID;
	NetworkClients += client;
	Printf("Client %u joined the lobby", client);
	// NetStartWindow::NetUpdate(client, Connected[client].Status);

	// Make sure any ready clients are marked as needing the new client's info.
	// for (int i = 1; i < MaxClients; ++i)
	// {
	// 	if (Connected[i].Status == CSTAT_READY)
	// 	{
	// 		Connected[i].Status = CSTAT_WAITING;
	// 		NetStartWindow::NetUpdate(i, Connected[i].Status);
	// 	}
	// }
}


void CallbackHandler::OnLobbyChatUpdate(LobbyChatUpdate_t *cb)
{
	if (cb->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered)
	{
		// Unsure of meaning
		if (RemoteClient >= 0)
			return;

		int free = 1;
		for (; free < MaxClients; ++free)
		{
			// if (Connected[free].Status == CSTAT_NONE)
			// 	break;
		}

		const int connectedPlayers = SteamMatchmaking()->GetNumLobbyMembers(cb->m_ulSteamIDLobby);

		AddClientConnection(cb->m_ulSteamIDUserChanged, free);
		NetStartWindow::NetProgress(connectedPlayers, MaxClients);
	}
}