/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <new>
#include <base/math.h>
#include <engine/shared/config.h>
#include <engine/map.h>
#include <engine/console.h>
#include "gamecontext.h"
#include <game/version.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include "gamemodes/dm.h"
#include "gamemodes/tdm.h"
#include "gamemodes/ctf.h"
#include "gamemodes/mod.h"
#include "gamemodes/race.h"
#include "gamemodes/hprace.h"

enum
{
	RESET,
	NO_RESET
};

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_apPlayers[i] = 0;

	m_pController = 0;
	m_VoteCloseTime = 0;
	m_pVoteOptionFirst = 0;
	m_pVoteOptionLast = 0;
	m_NumVoteOptions = 0;

	if(Resetting==NO_RESET)
		m_pVoteOptionHeap = new CHeap();
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		delete m_apPlayers[i];
	if(!m_Resetting)
		delete m_pVoteOptionHeap;
}

void CGameContext::Clear()
{
	CHeap *pVoteOptionHeap = m_pVoteOptionHeap;
	CVoteOptionServer *pVoteOptionFirst = m_pVoteOptionFirst;
	CVoteOptionServer *pVoteOptionLast = m_pVoteOptionLast;
	int NumVoteOptions = m_NumVoteOptions;
	CTuningParams Tuning = m_Tuning;

	m_Resetting = true;
	this->~CGameContext();
	mem_zero(this, sizeof(*this));
	new (this) CGameContext(RESET);

	m_pVoteOptionHeap = pVoteOptionHeap;
	m_pVoteOptionFirst = pVoteOptionFirst;
	m_pVoteOptionLast = pVoteOptionLast;
	m_NumVoteOptions = NumVoteOptions;
	m_Tuning = Tuning;
}


class CCharacter *CGameContext::GetPlayerChar(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return 0;
	return m_apPlayers[ClientID]->GetCharacter();
}

void CGameContext::CreateDamageInd(vec2 Pos, float Angle, int Amount)
{
	float a = 3 * 3.14159f / 2 + Angle;
	//float a = get_angle(dir);
	float s = a-pi/3;
	float e = a+pi/3;
	for(int i = 0; i < Amount; i++)
	{
		float f = mix(s, e, float(i+1)/float(Amount+2));
		CNetEvent_DamageInd *pEvent = (CNetEvent_DamageInd *)m_Events.Create(NETEVENTTYPE_DAMAGEIND, sizeof(CNetEvent_DamageInd));
		if(pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_Angle = (int)(f*256.0f);
		}
	}
}

void CGameContext::CreateHammerHit(vec2 Pos)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *)m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
}


void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	if (!NoDamage)
	{
		// deal damage
		CCharacter *apEnts[MAX_CLIENTS];
		float Radius = 135.0f;
		float InnerRadius = 48.0f;
		int Num = m_World.FindEntities(Pos, Radius, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; i++)
		{
			vec2 Diff = apEnts[i]->m_Pos - Pos;
			vec2 ForceDir(0,1);
			float l = length(Diff);
			if(l)
				ForceDir = normalize(Diff);
			l = 1-clamp((l-InnerRadius)/(Radius-InnerRadius), 0.0f, 1.0f);
			float Dmg = 6 * l;
			if((int)Dmg)
				apEnts[i]->TakeDamage(ForceDir*Dmg*2, (int)Dmg, Owner, Weapon);
		}
	}
}

/*
void create_smoke(vec2 Pos)
{
	// create the event
	EV_EXPLOSION *pEvent = (EV_EXPLOSION *)events.create(EVENT_SMOKE, sizeof(EV_EXPLOSION));
	if(pEvent)
	{
		pEvent->x = (int)Pos.x;
		pEvent->y = (int)Pos.y;
	}
}*/

void CGameContext::CreatePlayerSpawn(vec2 Pos)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *)m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn));
	if(ev)
	{
		ev->m_X = (int)Pos.x;
		ev->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientID)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *)m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound, int Mask)
{
	if (Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

void CGameContext::CreateSoundGlobal(int Sound, int Target)
{
	if (Sound < 0)
		return;

	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundID = Sound;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, Target);
}


void CGameContext::SendChatTarget(int To, const char *pText, int From, int Team)
{
	CNetMsg_Sv_Chat Msg;
	Msg.m_Team = Team;
	Msg.m_ClientID = From;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
}


void CGameContext::SendChat(int ChatterClientID, int Team, const char *pText)
{
	char aBuf[256];
	if(ChatterClientID >= 0 && ChatterClientID < MAX_CLIENTS)
		str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", ChatterClientID, Team, Server()->ClientName(ChatterClientID), pText);
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", pText);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, Team!=CHAT_ALL?"teamchat":"chat", aBuf);

	if(Team == CHAT_ALL)
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 0;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = pText;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
	}
	else
	{
		CNetMsg_Sv_Chat Msg;
		Msg.m_Team = 1;
		Msg.m_ClientID = ChatterClientID;
		Msg.m_pMessage = pText;

		// pack one for the recording only
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NOSEND, -1);

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() == Team)
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, i);
		}
	}
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}


void CGameContext::SendBroadcast(const char *pText, int ClientID)
{
	CNetMsg_Sv_Broadcast Msg;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

//
void CGameContext::StartVote(const char *pDesc, const char *pCommand, const char *pReason)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	// reset votes
	m_VoteEnforce = VOTE_ENFORCE_UNKNOWN;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->m_Vote = 0;
			m_apPlayers[i]->m_VotePos = 0;
		}
	}

	// start vote
	m_VoteCloseTime = time_get() + time_freq()*25;
	str_copy(m_aVoteDescription, pDesc, sizeof(m_aVoteDescription));
	str_copy(m_aVoteCommand, pCommand, sizeof(m_aVoteCommand));
	str_copy(m_aVoteReason, pReason, sizeof(m_aVoteReason));
	SendVoteSet(-1);
	m_VoteUpdate = true;
}


void CGameContext::EndVote()
{
	m_VoteCloseTime = 0;
	SendVoteSet(-1);
}

void CGameContext::SendVoteSet(int ClientID)
{
	CNetMsg_Sv_VoteSet Msg;
	if(m_VoteCloseTime)
	{
		Msg.m_Timeout = (m_VoteCloseTime-time_get())/time_freq();
		Msg.m_pDescription = m_aVoteDescription;
		Msg.m_pReason = m_aVoteReason;
	}
	else
	{
		Msg.m_Timeout = 0;
		Msg.m_pDescription = "";
		Msg.m_pReason = "";
	}
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendVoteStatus(int ClientID, int Total, int Yes, int No)
{
	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes+No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);

}

void CGameContext::AbortVoteKickOnDisconnect(int ClientID)
{
	if(m_VoteCloseTime && ((!str_comp_num(m_aVoteCommand, "kick ", 5) && str_toint(&m_aVoteCommand[5]) == ClientID) ||
		(!str_comp_num(m_aVoteCommand, "set_team ", 9) && str_toint(&m_aVoteCommand[9]) == ClientID)))
		m_VoteCloseTime = -1;
}


void CGameContext::CheckPureTuning()
{
	// might not be created yet during start up
	if(!m_pController)
		return;

	if(	str_comp(m_pController->m_pGameType, "DM")==0 ||
		str_comp(m_pController->m_pGameType, "TDM")==0 ||
		str_comp(m_pController->m_pGameType, "CTF")==0)
	{
		CTuningParams p;
		if(mem_comp(&p, &m_Tuning, sizeof(p)) != 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "resetting tuning due to pure server");
			m_Tuning = p;
		}
	}
}

void CGameContext::SendTuningParams(int ClientID)
{
	CheckPureTuning();

	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = (int *)&m_Tuning;
	for(unsigned i = 0; i < sizeof(m_Tuning)/sizeof(int); i++)
		Msg.AddInt(pParams[i]);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::OnTick()
{
	// check tuning
	CheckPureTuning();

	// copy tuning
	m_World.m_Core.m_Tuning = m_Tuning;
	m_World.Tick();

	//if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();
		}
	}

	// update voting
	if(m_VoteCloseTime)
	{
		// abort the kick-vote on player-leave
		if(m_VoteCloseTime == -1)
		{
			SendChat(-1, CGameContext::CHAT_ALL, "Vote aborted");
			EndVote();
		}
		else
		{
			int Total = 0, Yes = 0, No = 0;
			if(m_VoteUpdate)
			{
				// count votes
				char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}};
				for(int i = 0; i < MAX_CLIENTS; i++)
					if(m_apPlayers[i])
						Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
				bool aVoteChecked[MAX_CLIENTS] = {0};
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!m_apPlayers[i] || m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS || aVoteChecked[i])	// don't count in votes by spectators
						continue;

					int ActVote = m_apPlayers[i]->m_Vote;
					int ActVotePos = m_apPlayers[i]->m_VotePos;

					// check for more m_apPlayers with the same ip (only use the vote of the one who voted first)
					for(int j = i+1; j < MAX_CLIENTS; ++j)
					{
						if(!m_apPlayers[j] || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]))
							continue;

						aVoteChecked[j] = true;
						if(m_apPlayers[j]->m_Vote && (!ActVote || ActVotePos > m_apPlayers[j]->m_VotePos))
						{
							ActVote = m_apPlayers[j]->m_Vote;
							ActVotePos = m_apPlayers[j]->m_VotePos;
						}
					}

					Total++;
					if(ActVote > 0)
						Yes++;
					else if(ActVote < 0)
						No++;
				}

				if(Yes >= Total/2+1)
					m_VoteEnforce = VOTE_ENFORCE_YES;
				else if(No >= (Total+1)/2)
					m_VoteEnforce = VOTE_ENFORCE_NO;
			}

			if(m_VoteEnforce == VOTE_ENFORCE_YES)
			{
				Console()->ExecuteLine(m_aVoteCommand);
				EndVote();
				SendChat(-1, CGameContext::CHAT_ALL, "Vote passed");

				if(m_apPlayers[m_VoteCreator])
					m_apPlayers[m_VoteCreator]->m_LastVoteCall = 0;
			}
			else if(m_VoteEnforce == VOTE_ENFORCE_NO || time_get() > m_VoteCloseTime)
			{
				EndVote();
				SendChat(-1, CGameContext::CHAT_ALL, "Vote failed");
			}
			else if(m_VoteUpdate)
			{
				m_VoteUpdate = false;
				SendVoteStatus(-1, Total, Yes, No);
			}
		}
	}


#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies ; i++)
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i&1)?-1:1;
			m_apPlayers[MAX_CLIENTS-i-1]->OnPredictedInput(&Input);
		}
	}
#endif
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
		m_apPlayers[ClientID]->OnPredictedInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientEnter(int ClientID)
{
	//world.insert_entity(&m_apPlayers[ClientID]);
	m_apPlayers[ClientID]->Respawn();

	//race
	if(m_pController->IsRace())
		m_apPlayers[ClientID]->m_Score = -9999;
	else
		m_apPlayers[ClientID]->m_Score = 0;

	if(m_pController->IsRace())
		((CGameControllerRace*)m_pController)->score.initPlayer(ClientID);

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "'%s' entered and joined the %s", Server()->ClientName(ClientID), m_pController->GetTeamName(m_apPlayers[ClientID]->GetTeam()));
	SendChat(-1, CGameContext::CHAT_ALL, aBuf);

	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' team=%d", ClientID, Server()->ClientName(ClientID), m_apPlayers[ClientID]->GetTeam());
	Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	m_VoteUpdate = true;

	if(m_pController->IsHPRace())
	{
		SendChatTarget(ClientID, "HpRace mod 2.0 by LordSkelethom.");
		SendChatTarget(ClientID, "type /help to learn how to join.");
		SendBroadcast("type /help to learn how to join.", ClientID);
	}
}

void CGameContext::OnClientConnected(int ClientID)
{
	// Check which team the player should be on
	const int StartTeam = g_Config.m_SvTournamentMode ? TEAM_SPECTATORS : m_pController->GetAutoTeam(ClientID);

	m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, StartTeam);
	//m_apPlayers[ClientID].init(ClientID);
	//m_apPlayers[ClientID].ClientID = ClientID;

	(void)m_pController->CheckTeamBalance();

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		if(ClientID >= MAX_CLIENTS-g_Config.m_DbgDummies)
			return;
	}
#endif

	// send active vote
	if(m_VoteCloseTime)
		SendVoteSet(ClientID);

	// send motd
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = g_Config.m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::OnClientDrop(int ClientID, const char *pReason)
{
	AbortVoteKickOnDisconnect(ClientID);
	m_apPlayers[ClientID]->OnDisconnect(pReason);
	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = 0;

	(void)m_pController->CheckTeamBalance();
	m_VoteUpdate = true;

	// update spectator modes
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->m_SpectatorID == ClientID)
			m_apPlayers[i]->m_SpectatorID = SPEC_FREEVIEW;
	}
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	void *pRawMsg = m_NetObjHandler.SecureUnpackMsg(MsgID, pUnpacker);
	CPlayer *pPlayer = m_apPlayers[ClientID];
	CPlayer *p = pPlayer;

	if(!pRawMsg)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "dropped weird message '%s' (%d), failed on '%s'", m_NetObjHandler.GetMsgName(MsgID), MsgID, m_NetObjHandler.FailedMsgOn());
		Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		return;
	}

	if(MsgID == NETMSGTYPE_CL_SAY)
	{
		CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *)pRawMsg;
		int Team = pMsg->m_Team;
		if(Team)
			Team = pPlayer->GetTeam();
		else
			Team = CGameContext::CHAT_ALL;

		// check for invalid chars
		unsigned char *pMessage = (unsigned char *)pMsg->m_pMessage;
		while (*pMessage)
		{
			if(*pMessage < 32)
				*pMessage = ' ';
			pMessage++;
		}

		if(g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat+Server()->TickSpeed() > Server()->Tick())
		{
			if(!str_comp(pMsg->m_pMessage, "/rank") && m_pController->IsRace())
				m_apPlayers[ClientID]->m_LastChat = Server()->Tick() + Server()->TickSpeed()*10;
			else 
				m_apPlayers[ClientID]->m_LastChat = Server()->Tick();
		}
		else
		{
			m_apPlayers[ClientID]->m_LastChat = Server()->Tick();

			if(!str_comp(pMsg->m_pMessage, "/info"))
			{
				char buf[256];
				str_format(buf, sizeof(buf), "HpRace mod by LordSkelethom (say /mods). (C)Rajh & LordSkelethom (2.0)", Server()->ClientName(ClientID));
				SendChatTarget(ClientID, buf);
			}
			else if(!str_comp(pMsg->m_pMessage, "/mods"))
			{
				char buf[128];
				str_format(buf, sizeof(buf), "Mod used: Race mod (%s)", Server()->ClientName(ClientID));
				SendChatTarget(ClientID, buf);
			}
			else if(!str_comp_num(pMsg->m_pMessage, "/top5", 5) && m_pController->IsRace())
			{
				const char *pt = pMsg->m_pMessage;
				int number = 0;
				pt += 6;
				while(*pt && *pt >= '0' && *pt <= '9')
				{
					number = number*10+(*pt-'0');
					pt++;
				}
				if(!m_pController->IsHPRace())
				{
					if(number)
						((CGameControllerRace*)m_pController)->score.top5_draw(ClientID, number);
					else
						((CGameControllerRace*)m_pController)->score.top5_draw(ClientID, 0);
				}
				else
				{
					if(number)
						((CGameControllerHPRace*)m_pController)->score.top5_draw(ClientID, number);
					else
						((CGameControllerHPRace*)m_pController)->score.top5_draw(ClientID, 0);
				}
			}
			else if(!str_comp_num(pMsg->m_pMessage, "/rank", 5) && m_pController->IsRace())
			{
				char buf[512];
				const char *name = pMsg->m_pMessage;
				name += 6;
				int pos;
				
				if(!m_pController->IsHPRace())
				{
					PLAYER_SCORE *pscore;
					if(!str_comp(pMsg->m_pMessage, "/rank"))
						pscore = ((CGameControllerRace*)m_pController)->score.search_score(ClientID, 1, &pos);
					else
						pscore = ((CGameControllerRace*)m_pController)->score.search_name(name, &pos, true);
					
					if(pscore && pos > -1)
					{
						float time = pscore->score;
						str_format(buf, sizeof(buf), "%d. %s Time: %d minute(s) %5.3f second(s)", pos, pscore->name, (int)time/60, time-((int)time/60*60));
						if(str_comp(pMsg->m_pMessage, "/rank"))
						{
							char client_name[128];
							str_format(client_name, sizeof(client_name), " (%s)", Server()->ClientName(ClientID));
							char rofl[256];
							str_format(rofl, sizeof(rofl), "%s%s", buf, client_name);
							str_copy(buf, rofl, sizeof(buf));
						}
						SendChat(-1, CGameContext::CHAT_ALL, buf);
						m_apPlayers[ClientID]->m_LastChat = Server()->Tick() + Server()->TickSpeed()*3;
						return;
					}
					else if(pos == -1)
						str_format(buf, sizeof(buf), "Several m_apPlayers were found.");
					else
						str_format(buf, sizeof(buf), "%s is not ranked", str_comp(pMsg->m_pMessage, "/rank")?name:Server()->ClientName(ClientID));
				}
				else
				{
					HPRACE_TEAM_SCORE *pscore;
					if(!str_comp(pMsg->m_pMessage, "/rank"))
						pscore = ((CGameControllerHPRace*)m_pController)->score.search_score(ClientID, 1, &pos);
					else
						pscore = ((CGameControllerHPRace*)m_pController)->score.search_name(name, &pos, true);
					
					if(pscore)
					{
						float time = pscore->score;
						str_format(buf, sizeof(buf), "%d. %s & %s - Time: %d minute(s) %5.3f second(s)", pos, pscore->name, pscore->name2
							, (int) time/60, time-((int)time/60*60));
						SendChat(-1, CGameContext::CHAT_ALL, buf);
						if(str_comp(pMsg->m_pMessage, "/rank"))
							SendChatTarget(ClientID, "If it is not the right player, be more precise.");
						m_apPlayers[ClientID]->m_LastChat = Server()->Tick() + Server()->TickSpeed()*3;
						return;
					}
					else
						str_format(buf, sizeof(buf), "%s is not ranked", str_comp(pMsg->m_pMessage, "/rank")?name:Server()->ClientName(ClientID));
				}
				
				SendChatTarget(ClientID, buf);
			}
			else if(!str_comp(pMsg->m_pMessage, "/cmdlist"))
			{
				SendChatTarget(ClientID, "---Command List---");
				SendChatTarget(ClientID, "\"/info\" information about the mod");
				SendChatTarget(ClientID, "\"/mods\" shows the used mods");
				SendChatTarget(ClientID, "\"/rank\" shows your rank");
				SendChatTarget(ClientID, "\"/rank NAME\" shows the rank of a specific player");
				SendChatTarget(ClientID, "\"/top5 X\" shows the top 5");
			}
			else if(!str_comp(pMsg->m_pMessage, "/help"))
			{
				SendChatTarget(ClientID, "--- How to get a partner ---");
				SendChatTarget(ClientID, "- type /with partofnameyouwant");
				SendChatTarget(ClientID, "- example :");
				SendChatTarget(ClientID, "- /with lord will send a msg to a player with 'lord' in his name.");
				SendChatTarget(ClientID, "----------------------------");
			}
			else if(!str_comp(pMsg->m_pMessage, "/without")  && m_pController->IsHPRace())
			{
				p->DeletePartner();
				p->KillCharacter(-1);
			}
			else if(!str_comp_num(pMsg->m_pMessage, "/with", 5) && m_pController->IsHPRace())
			{
				char buf[512];
				const char *name = pMsg->m_pMessage;
				name += 6;
				int num = 0;
				int id = -1;
				
				for(int i=0;i<MAX_CLIENTS;i++)
				{
					if(m_apPlayers[i] && !m_apPlayers[i]->GetPartner() && i!=ClientID && str_find_nocase(Server()->ClientName(i),name))
					{
						id = i;
						num++;
					}
				}
				if(p->GetPartner())
					str_format(buf, sizeof(buf), "You have already a partner (%s).",Server()->ClientName(p->GetPartner()->GetCID()));
				else if(num>1)
					str_format(buf, sizeof(buf), "Several m_apPlayers were found, be more precise.");
				else if(num==0)
					str_format(buf, sizeof(buf), "No player matches.");
				else if(!m_apPlayers[id]->GetPartner())
				{
					m_apPlayers[id]->asked = ClientID;
					char buf2[512];
					str_format(buf2, sizeof(buf2), "Do you want to be %s's partner ?",Server()->ClientName(ClientID));
					SendChatTarget(id,buf2);
					SendChatTarget(id, "Say /yes or /no.");
					str_format(buf, sizeof(buf), "%s have been asked.",Server()->ClientName(id));
				}
				else
					str_format(buf, sizeof(buf), "You can't choose this player.");
				SendChatTarget(ClientID, buf);
				return;
			}
			else if(!str_comp(pMsg->m_pMessage, "/yes")  && m_pController->IsHPRace())
			{
				if(p->asked<0 || !m_apPlayers[p->asked])
					return;

				else if(p->GetPartner())
				{
					char buf[256];
					str_format(buf, sizeof(buf), "%s chose another partner.",Server()->ClientName(p->asked));
					SendChatTarget(ClientID, buf);
				}
				else
				{
					p->SetPartner(p->asked);
					m_apPlayers[p->asked]->SetPartner(ClientID);
					((CGameControllerHPRace*)m_pController)->CreateHPTeam(ClientID, p->asked);
				}
			}
			else if(!str_comp(pMsg->m_pMessage, "/no")  && m_pController->IsHPRace())
			{
				if(p->asked<0 || !m_apPlayers[p->asked])
					return;
				char buf[256];
				str_format(buf, sizeof(buf), "%s refused to be your partner.",Server()->ClientName(ClientID));
				SendChatTarget(p->asked, buf);
				str_format(buf, sizeof(buf), "You refused to be %s's partner.",Server()->ClientName(p->asked));
				SendChatTarget(ClientID, buf);
				p->asked = -1;
			}
			else if(!str_comp_num(pMsg->m_pMessage, "/", 1))
			{
				SendChatTarget(ClientID, "Wrong command.");
				SendChatTarget(ClientID, "Say \"/cmdlist\" for list of command available.");
			}
			else if(p->GetPartner() && Team!=CGameContext::CHAT_ALL && p->GetTeam() > -1)
			{
				SendChatTarget(p->GetPartner()->GetCID(), pMsg->m_pMessage, ClientID, 1);
				SendChatTarget(ClientID, pMsg->m_pMessage, ClientID, 1);
			}
			else if(!p->GetPartner() && Team!=CGameContext::CHAT_ALL && p->GetTeam()>-1)
			{
				for(int i=0;i<MAX_CLIENTS;i++)
				{
					if(m_apPlayers[i] && m_apPlayers[i]->hprace_team<0)
						SendChatTarget(i, pMsg->m_pMessage, ClientID, 1);
				}
			}
			else
				SendChat(ClientID, Team, pMsg->m_pMessage);
		}
	}
	else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
	{
		if(g_Config.m_SvSpamprotection && pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry+Server()->TickSpeed()*3 > Server()->Tick())
			return;

		int64 Now = Server()->Tick();
		pPlayer->m_LastVoteTry = Now;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
		{
			SendChatTarget(ClientID, "Spectators aren't allowed to start a vote.");
			return;
		}

		if(m_VoteCloseTime)
		{
			SendChatTarget(ClientID, "Wait for current vote to end before calling a new one.");
			return;
		}

		int Timeleft = pPlayer->m_LastVoteCall + Server()->TickSpeed()*60 - Now;
		if(pPlayer->m_LastVoteCall && Timeleft > 0)
		{
			char aChatmsg[512] = {0};
			str_format(aChatmsg, sizeof(aChatmsg), "You must wait %d seconds before making another vote", (Timeleft/Server()->TickSpeed())+1);
			SendChatTarget(ClientID, aChatmsg);
			return;
		}

		char aChatmsg[512] = {0};
		char aDesc[VOTE_DESC_LENGTH] = {0};
		char aCmd[VOTE_CMD_LENGTH] = {0};
		CNetMsg_Cl_CallVote *pMsg = (CNetMsg_Cl_CallVote *)pRawMsg;
		const char *pReason = pMsg->m_Reason[0] ? pMsg->m_Reason : "No reason given";

		if(str_comp_nocase(pMsg->m_Type, "option") == 0)
		{
			CVoteOptionServer *pOption = m_pVoteOptionFirst;
			while(pOption)
			{
				if(str_comp_nocase(pMsg->m_Value, pOption->m_aDescription) == 0)
				{
					str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s' (%s)", Server()->ClientName(ClientID),
								pOption->m_aDescription, pReason);
					str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);
					str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
					break;
				}

				pOption = pOption->m_pNext;
			}

			if(!pOption)
			{
				str_format(aChatmsg, sizeof(aChatmsg), "'%s' isn't an option on this server", pMsg->m_Value);
				SendChatTarget(ClientID, aChatmsg);
				return;
			}
		}
		else if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
		{
			if(!g_Config.m_SvVoteKick)
			{
				SendChatTarget(ClientID, "Server does not allow voting to kick m_apPlayers");
				return;
			}

			if(g_Config.m_SvVoteKickMin)
			{
				int PlayerNum = 0;
				for(int i = 0; i < MAX_CLIENTS; ++i)
					if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
						++PlayerNum;

				if(PlayerNum < g_Config.m_SvVoteKickMin)
				{
					str_format(aChatmsg, sizeof(aChatmsg), "Kick voting requires %d m_apPlayers on the server", g_Config.m_SvVoteKickMin);
					SendChatTarget(ClientID, aChatmsg);
					return;
				}
			}

			int KickID = str_toint(pMsg->m_Value);
			if(KickID < 0 || KickID >= MAX_CLIENTS || !m_apPlayers[KickID])
			{
				SendChatTarget(ClientID, "Invalid client id to kick");
				return;
			}
			if(KickID == ClientID)
			{
				SendChatTarget(ClientID, "You can't kick yourself");
				return;
			}
			if(Server()->IsAuthed(KickID))
			{
				SendChatTarget(ClientID, "You can't kick admins");
				char aBufKick[128];
				str_format(aBufKick, sizeof(aBufKick), "'%s' called for vote to kick you", Server()->ClientName(ClientID));
				SendChatTarget(KickID, aBufKick);
				return;
			}

			str_format(aChatmsg, sizeof(aChatmsg), "'%s' called for vote to kick '%s' (%s)", Server()->ClientName(ClientID), Server()->ClientName(KickID), pReason);
			str_format(aDesc, sizeof(aDesc), "Kick '%s'", Server()->ClientName(KickID));
			if (!g_Config.m_SvVoteKickBantime)
				str_format(aCmd, sizeof(aCmd), "kick %d Kicked by vote", KickID);
			else
			{
				char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
				Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
				str_format(aCmd, sizeof(aCmd), "ban %s %d Banned by vote", aAddrStr, g_Config.m_SvVoteKickBantime);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aCmd);
			}
		}
		else if(str_comp_nocase(pMsg->m_Type, "spectate") == 0)
		{
			if(!g_Config.m_SvVoteSpectate)
			{
				SendChatTarget(ClientID, "Server does not allow voting to move m_apPlayers to spectators");
				return;
			}

			int SpectateID = str_toint(pMsg->m_Value);
			if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !m_apPlayers[SpectateID] || m_apPlayers[SpectateID]->GetTeam() == TEAM_SPECTATORS)
			{
				SendChatTarget(ClientID, "Invalid client id to move");
				return;
			}
			if(SpectateID == ClientID)
			{
				SendChatTarget(ClientID, "You can't move yourself");
				return;
			}

			str_format(aChatmsg, sizeof(aChatmsg), "'%s' called for vote to move '%s' to spectators (%s)", Server()->ClientName(ClientID), Server()->ClientName(SpectateID), pReason);
			str_format(aDesc, sizeof(aDesc), "move '%s' to spectators", Server()->ClientName(SpectateID));
			str_format(aCmd, sizeof(aCmd), "set_team %d -1 %d", SpectateID, g_Config.m_SvVoteSpectateRejoindelay);
		}

		if(aCmd[0])
		{
			SendChat(-1, CGameContext::CHAT_ALL, aChatmsg);
			StartVote(aDesc, aCmd, pReason);
			pPlayer->m_Vote = 1;
			pPlayer->m_VotePos = m_VotePos = 1;
			m_VoteCreator = ClientID;
			pPlayer->m_LastVoteCall = Now;
		}
	}
	else if(MsgID == NETMSGTYPE_CL_VOTE)
	{
		if(!m_VoteCloseTime)
			return;

		if(pPlayer->m_Vote == 0)
		{
			CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
			if(!pMsg->m_Vote)
				return;

			pPlayer->m_Vote = pMsg->m_Vote;
			pPlayer->m_VotePos = ++m_VotePos;
			m_VoteUpdate = true;
		}
	}
	else if (MsgID == NETMSGTYPE_CL_SETTEAM && !m_World.m_Paused)
	{
		CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *)pRawMsg;

		if(pPlayer->GetTeam() == pMsg->m_Team || (g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam+Server()->TickSpeed()*3 > Server()->Tick()))
			return;

		if(pPlayer->m_TeamChangeTick > Server()->Tick())
		{
			pPlayer->m_LastSetTeam = Server()->Tick();
			int TimeLeft = (pPlayer->m_TeamChangeTick - Server()->Tick())/Server()->TickSpeed();
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "Time to wait before changing team: %02d:%02d", TimeLeft/60, TimeLeft%60);
			SendBroadcast(aBuf, ClientID);
			return;
		}

		// Switch team on given client and kill/respawn him
		if(m_pController->CanJoinTeam(pMsg->m_Team, ClientID))
		{
			if(m_pController->CanChangeTeam(pPlayer, pMsg->m_Team))
			{
				pPlayer->m_LastSetTeam = Server()->Tick();
				if(pPlayer->GetTeam() == TEAM_SPECTATORS || pMsg->m_Team == TEAM_SPECTATORS)
					m_VoteUpdate = true;
				pPlayer->SetTeam(pMsg->m_Team);
				(void)m_pController->CheckTeamBalance();
				pPlayer->m_TeamChangeTick = Server()->Tick();
			}
			else
				SendBroadcast("Teams must be balanced, please join other team", ClientID);
		}
		else
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "Only %d active m_apPlayers are allowed", g_Config.m_SvMaxClients-g_Config.m_SvSpectatorSlots);
			SendBroadcast(aBuf, ClientID);
		}
	}
	else if (MsgID == NETMSGTYPE_CL_SETSPECTATORMODE && !m_World.m_Paused)
	{
		CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *)pRawMsg;

		if(pPlayer->GetTeam() != TEAM_SPECTATORS || pPlayer->m_SpectatorID == pMsg->m_SpectatorID || ClientID == pMsg->m_SpectatorID ||
			(g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode+Server()->TickSpeed()*3 > Server()->Tick()))
			return;

		pPlayer->m_LastSetSpectatorMode = Server()->Tick();
		if(pMsg->m_SpectatorID != SPEC_FREEVIEW && (!m_apPlayers[pMsg->m_SpectatorID] || m_apPlayers[pMsg->m_SpectatorID]->GetTeam() == TEAM_SPECTATORS))
			SendChatTarget(ClientID, "Invalid spectator id used");
		else
			pPlayer->m_SpectatorID = pMsg->m_SpectatorID;
	}
	else if (MsgID == NETMSGTYPE_CL_STARTINFO)
	{
		if(pPlayer->m_IsReady)
			return;

		CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *)pRawMsg;
		pPlayer->m_LastChangeInfo = Server()->Tick();

		// set start infos
		Server()->SetClientName(ClientID, pMsg->m_pName);
		Server()->SetClientClan(ClientID, pMsg->m_pClan);
		Server()->SetClientCountry(ClientID, pMsg->m_Country);
		str_copy(pPlayer->m_TeeInfos.m_SkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_SkinName));
		pPlayer->m_TeeInfos.m_UseCustomColor = pMsg->m_UseCustomColor;
		pPlayer->m_TeeInfos.m_ColorBody = pMsg->m_ColorBody;
		pPlayer->m_TeeInfos.m_ColorFeet = pMsg->m_ColorFeet;
		m_pController->OnPlayerInfoChange(pPlayer);

		// send vote options
		CNetMsg_Sv_VoteClearOptions ClearMsg;
		Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);

		CNetMsg_Sv_VoteOptionListAdd OptionMsg;
		int NumOptions = 0;
		OptionMsg.m_pDescription0 = "";
		OptionMsg.m_pDescription1 = "";
		OptionMsg.m_pDescription2 = "";
		OptionMsg.m_pDescription3 = "";
		OptionMsg.m_pDescription4 = "";
		OptionMsg.m_pDescription5 = "";
		OptionMsg.m_pDescription6 = "";
		OptionMsg.m_pDescription7 = "";
		OptionMsg.m_pDescription8 = "";
		OptionMsg.m_pDescription9 = "";
		OptionMsg.m_pDescription10 = "";
		OptionMsg.m_pDescription11 = "";
		OptionMsg.m_pDescription12 = "";
		OptionMsg.m_pDescription13 = "";
		OptionMsg.m_pDescription14 = "";
		CVoteOptionServer *pCurrent = m_pVoteOptionFirst;
		while(pCurrent)
		{
			switch(NumOptions++)
			{
			case 0: OptionMsg.m_pDescription0 = pCurrent->m_aDescription; break;
			case 1: OptionMsg.m_pDescription1 = pCurrent->m_aDescription; break;
			case 2: OptionMsg.m_pDescription2 = pCurrent->m_aDescription; break;
			case 3: OptionMsg.m_pDescription3 = pCurrent->m_aDescription; break;
			case 4: OptionMsg.m_pDescription4 = pCurrent->m_aDescription; break;
			case 5: OptionMsg.m_pDescription5 = pCurrent->m_aDescription; break;
			case 6: OptionMsg.m_pDescription6 = pCurrent->m_aDescription; break;
			case 7: OptionMsg.m_pDescription7 = pCurrent->m_aDescription; break;
			case 8: OptionMsg.m_pDescription8 = pCurrent->m_aDescription; break;
			case 9: OptionMsg.m_pDescription9 = pCurrent->m_aDescription; break;
			case 10: OptionMsg.m_pDescription10 = pCurrent->m_aDescription; break;
			case 11: OptionMsg.m_pDescription11 = pCurrent->m_aDescription; break;
			case 12: OptionMsg.m_pDescription12 = pCurrent->m_aDescription; break;
			case 13: OptionMsg.m_pDescription13 = pCurrent->m_aDescription; break;
			case 14:
				{
					OptionMsg.m_pDescription14 = pCurrent->m_aDescription;
					OptionMsg.m_NumOptions = NumOptions;
					Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, ClientID);
					OptionMsg = CNetMsg_Sv_VoteOptionListAdd();
					NumOptions = 0;
					OptionMsg.m_pDescription1 = "";
					OptionMsg.m_pDescription2 = "";
					OptionMsg.m_pDescription3 = "";
					OptionMsg.m_pDescription4 = "";
					OptionMsg.m_pDescription5 = "";
					OptionMsg.m_pDescription6 = "";
					OptionMsg.m_pDescription7 = "";
					OptionMsg.m_pDescription8 = "";
					OptionMsg.m_pDescription9 = "";
					OptionMsg.m_pDescription10 = "";
					OptionMsg.m_pDescription11 = "";
					OptionMsg.m_pDescription12 = "";
					OptionMsg.m_pDescription13 = "";
					OptionMsg.m_pDescription14 = "";
				}
			}
			pCurrent = pCurrent->m_pNext;
		}
		if(NumOptions > 0)
		{
			OptionMsg.m_NumOptions = NumOptions;
			Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, ClientID);
			NumOptions = 0;
		}

		// send tuning parameters to client
		SendTuningParams(ClientID);

		// client is ready to enter
		pPlayer->m_IsReady = true;
		CNetMsg_Sv_ReadyToEnter m;
		Server()->SendPackMsg(&m, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID);
	}
	else if (MsgID == NETMSGTYPE_CL_CHANGEINFO)
	{
		if(g_Config.m_SvSpamprotection && pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo+Server()->TickSpeed()*5 > Server()->Tick())
			return;

		CNetMsg_Cl_ChangeInfo *pMsg = (CNetMsg_Cl_ChangeInfo *)pRawMsg;
		pPlayer->m_LastChangeInfo = Server()->Tick();

		// set infos
		char aOldName[MAX_NAME_LENGTH];
		str_copy(aOldName, Server()->ClientName(ClientID), sizeof(aOldName));
		Server()->SetClientName(ClientID, pMsg->m_pName);
		if(str_comp(aOldName, Server()->ClientName(ClientID)) != 0)
		{
			char aChatText[256];
			str_format(aChatText, sizeof(aChatText), "'%s' changed name to '%s'", aOldName, Server()->ClientName(ClientID));
			SendChat(-1, CGameContext::CHAT_ALL, aChatText);
		}
		Server()->SetClientClan(ClientID, pMsg->m_pClan);
		Server()->SetClientCountry(ClientID, pMsg->m_Country);
		str_copy(pPlayer->m_TeeInfos.m_SkinName, pMsg->m_pSkin, sizeof(pPlayer->m_TeeInfos.m_SkinName));
		pPlayer->m_TeeInfos.m_UseCustomColor = pMsg->m_UseCustomColor;
		pPlayer->m_TeeInfos.m_ColorBody = pMsg->m_ColorBody;
		pPlayer->m_TeeInfos.m_ColorFeet = pMsg->m_ColorFeet;
		m_pController->OnPlayerInfoChange(pPlayer);
	}
	else if (MsgID == NETMSGTYPE_CL_EMOTICON && !m_World.m_Paused)
	{
		CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *)pRawMsg;

		if(g_Config.m_SvSpamprotection && pPlayer->m_LastEmote && pPlayer->m_LastEmote+Server()->TickSpeed()*3 > Server()->Tick())
			return;

		pPlayer->m_LastEmote = Server()->Tick();

		SendEmoticon(ClientID, pMsg->m_Emoticon);
	}
	else if (MsgID == NETMSGTYPE_CL_KILL && !m_World.m_Paused)
	{
		if(pPlayer->m_LastKill && pPlayer->m_LastKill+Server()->TickSpeed()*3 > Server()->Tick())
			return;

		if(p->GetPartnerChar())
		{
			p->GetPartner()->m_LastKill = Server()->Tick();
			p->GetPartner()->KillCharacter(WEAPON_SELF);
			p->GetPartner()->m_RespawnTick = Server()->Tick();
		}
 		
		pPlayer->m_LastKill = Server()->Tick();
		pPlayer->KillCharacter(WEAPON_SELF);
		if(m_pController->IsRace())
			p->m_RespawnTick = Server()->Tick();
	}
}

void CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);

	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		pSelf->SendTuningParams(-1);
	}
	else
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CTuningParams TuningParams;
	*pSelf->Tuning() = TuningParams;
	pSelf->SendTuningParams(-1);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
}

void CGameContext::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float v;
		pSelf->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->m_apNames[i], v);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

void CGameContext::ConChangeMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->ChangeMap(pResult->NumArguments() ? pResult->GetString(0) : "");
}

void CGameContext::ConRestart(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(pResult->NumArguments())
		pSelf->m_pController->DoWarmup(pResult->GetInteger(0));
	else
		pSelf->m_pController->StartRound();
}

void CGameContext::ConBroadcast(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendBroadcast(pResult->GetString(0), -1);
}

void CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendChat(-1, CGameContext::CHAT_ALL, pResult->GetString(0));
}

void CGameContext::ConSetTeam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int ClientID = clamp(pResult->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	int Team = clamp(pResult->GetInteger(1), -1, 1);
	int Delay = 0;
	if(pResult->NumArguments() > 2)
		Delay = pResult->GetInteger(2);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "moved client %d to team %d", ClientID, Team);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	if(!pSelf->m_apPlayers[ClientID])
		return;

	pSelf->m_apPlayers[ClientID]->m_TeamChangeTick = pSelf->Server()->Tick()+pSelf->Server()->TickSpeed()*Delay*60;
	pSelf->m_apPlayers[ClientID]->SetTeam(Team);
	(void)pSelf->m_pController->CheckTeamBalance();
}

void CGameContext::ConSetTeamAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Team = clamp(pResult->GetInteger(0), -1, 1);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "moved all clients to team %d", Team);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	for(int i = 0; i < MAX_CLIENTS; ++i)
		if(pSelf->m_apPlayers[i])
			pSelf->m_apPlayers[i]->SetTeam(Team);

	(void)pSelf->m_pController->CheckTeamBalance();
}

void CGameContext::con_kill_pl(IConsole::IResult *result, void *user_data)
{
	CGameContext *pSelf = (CGameContext *)user_data;
	int cid = clamp(result->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	if(!pSelf->m_apPlayers[cid] || !pSelf->m_pController->IsRace())
		return;
	
	pSelf->m_apPlayers[cid]->KillCharacter(WEAPON_GAME);
	char buf[512];
	str_format(buf, sizeof(buf), "%s Killed by admin", pSelf->Server()->ClientName(cid));
	pSelf->SendChat(-1, CGameContext::CHAT_ALL, buf);
}

void CGameContext::con_teleport(IConsole::IResult *result, void *user_data)
{
	CGameContext *pSelf = (CGameContext *)user_data;
	int cid1 = clamp(result->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	int cid2 = clamp(result->GetInteger(1), 0, (int)MAX_CLIENTS-1);
	if(pSelf->m_apPlayers[cid1] && pSelf->m_apPlayers[cid2] && pSelf->m_pController->IsRace())
	{
		CCharacter* chr = pSelf->m_apPlayers[cid1]->GetCharacter();
		CCharacter* par = pSelf->m_apPlayers[cid1]->GetPartnerChar();
		if(chr)
		{
			chr->m_Core.m_Pos = pSelf->m_apPlayers[cid2]->m_ViewPos;
			chr->race_state = RACE_FINISHED;
		}
		if(par)
			par->race_state = RACE_FINISHED;
	}
}

void CGameContext::con_teleport_to(IConsole::IResult *result, void *user_data)
{
	CGameContext *pSelf = (CGameContext *)user_data;
	int cid = clamp(result->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	if(pSelf->m_apPlayers[cid] && pSelf->m_pController->IsRace())
	{
		CCharacter* chr = pSelf->m_apPlayers[cid]->GetCharacter();
		CCharacter* par = pSelf->m_apPlayers[cid]->GetPartnerChar();
		if(chr)
		{
			chr->m_Core.m_Pos.x = result->GetInteger(1);
			chr->m_Core.m_Pos.y = result->GetInteger(2);
			chr->race_state = RACE_FINISHED;
		}
		if(par)
			par->race_state = RACE_FINISHED;
	}
}

void CGameContext::con_get_pos(IConsole::IResult *result, void *user_data)
{
	CGameContext *pSelf = (CGameContext *)user_data;
	int cid = clamp(result->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	if(pSelf->m_apPlayers[cid])
		dbg_msg("Tele", "%s pos: %d @ %d", pSelf->Server()->ClientName(cid), (int)pSelf->m_apPlayers[cid]->m_ViewPos.x, (int)pSelf->m_apPlayers[cid]->m_ViewPos.y);
}

void CGameContext::ConAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);
	const char *pCommand = pResult->GetString(1);

	if(pSelf->m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return;
	}

	// check for valid option
	if(!pSelf->Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}
	while(*pDescription && *pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	++pSelf->m_NumVoteOptions;
	int Len = str_length(pCommand);

	pOption = (CVoteOptionServer *)pSelf->m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
	pOption->m_pNext = 0;
	pOption->m_pPrev = pSelf->m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	pSelf->m_pVoteOptionLast = pOption;
	if(!pSelf->m_pVoteOptionFirst)
		pSelf->m_pVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len+1);
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "added option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	// inform clients about added option
	CNetMsg_Sv_VoteOptionAdd OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	pSelf->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);
}

void CGameContext::ConRemoveVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);

	// check for valid option
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
			break;
		pOption = pOption->m_pNext;
	}
	if(!pOption)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "option '%s' does not exist", pDescription);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// inform clients about removed option
	CNetMsg_Sv_VoteOptionRemove OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	pSelf->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);

	// TODO: improve this
	// remove the option
	--pSelf->m_NumVoteOptions;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "removed option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	CHeap *pVoteOptionHeap = new CHeap();
	CVoteOptionServer *pVoteOptionFirst = 0;
	CVoteOptionServer *pVoteOptionLast = 0;
	int NumVoteOptions = pSelf->m_NumVoteOptions;
	for(CVoteOptionServer *pSrc = pSelf->m_pVoteOptionFirst; pSrc; pSrc = pSrc->m_pNext)
	{
		if(pSrc == pOption)
			continue;

		// copy option
		int Len = str_length(pSrc->m_aCommand);
		CVoteOptionServer *pDst = (CVoteOptionServer *)pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
		pDst->m_pNext = 0;
		pDst->m_pPrev = pVoteOptionLast;
		if(pDst->m_pPrev)
			pDst->m_pPrev->m_pNext = pDst;
		pVoteOptionLast = pDst;
		if(!pVoteOptionFirst)
			pVoteOptionFirst = pDst;

		str_copy(pDst->m_aDescription, pSrc->m_aDescription, sizeof(pDst->m_aDescription));
		mem_copy(pDst->m_aCommand, pSrc->m_aCommand, Len+1);
	}

	// clean up
	delete pSelf->m_pVoteOptionHeap;
	pSelf->m_pVoteOptionHeap = pVoteOptionHeap;
	pSelf->m_pVoteOptionFirst = pVoteOptionFirst;
	pSelf->m_pVoteOptionLast = pVoteOptionLast;
	pSelf->m_NumVoteOptions = NumVoteOptions;
}

void CGameContext::ConForceVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pType = pResult->GetString(0);
	const char *pValue = pResult->GetString(1);
	const char *pReason = pResult->NumArguments() > 2 && pResult->GetString(2)[0] ? pResult->GetString(2) : "No reason given";
	char aBuf[128] = {0};

	if(str_comp_nocase(pType, "option") == 0)
	{
		CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
		while(pOption)
		{
			if(str_comp_nocase(pValue, pOption->m_aDescription) == 0)
			{
				str_format(aBuf, sizeof(aBuf), "admin forced server option '%s' (%s)", pValue, pReason);
				pSelf->SendChatTarget(-1, aBuf);
				pSelf->Console()->ExecuteLine(pOption->m_aCommand);
				break;
			}

			pOption = pOption->m_pNext;
		}

		if(!pOption)
		{
			str_format(aBuf, sizeof(aBuf), "'%s' isn't an option on this server", pValue);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
	}
	else if(str_comp_nocase(pType, "kick") == 0)
	{
		int KickID = str_toint(pValue);
		if(KickID < 0 || KickID >= MAX_CLIENTS || !pSelf->m_apPlayers[KickID])
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid client id to kick");
			return;
		}

		if (!g_Config.m_SvVoteKickBantime)
		{
			str_format(aBuf, sizeof(aBuf), "kick %d %s", KickID, pReason);
			pSelf->Console()->ExecuteLine(aBuf);
		}
		else
		{
			char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
			pSelf->Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
			str_format(aBuf, sizeof(aBuf), "ban %s %d %s", aAddrStr, g_Config.m_SvVoteKickBantime, pReason);
			pSelf->Console()->ExecuteLine(aBuf);
		}
	}
	else if(str_comp_nocase(pType, "spectate") == 0)
	{
		int SpectateID = str_toint(pValue);
		if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !pSelf->m_apPlayers[SpectateID] || pSelf->m_apPlayers[SpectateID]->GetTeam() == TEAM_SPECTATORS)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "Invalid client id to move");
			return;
		}

		str_format(aBuf, sizeof(aBuf), "admin moved '%s' to spectator (%s)", pSelf->Server()->ClientName(SpectateID), pReason);
		pSelf->SendChatTarget(-1, aBuf);
		str_format(aBuf, sizeof(aBuf), "set_team %d -1 %d", SpectateID, g_Config.m_SvVoteSpectateRejoindelay);
		pSelf->Console()->ExecuteLine(aBuf);
	}
}

void CGameContext::ConClearVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "cleared votes");
	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	pSelf->Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);
	pSelf->m_pVoteOptionHeap->Reset();
	pSelf->m_pVoteOptionFirst = 0;
	pSelf->m_pVoteOptionLast = 0;
	pSelf->m_NumVoteOptions = 0;
}

void CGameContext::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(str_comp_nocase(pResult->GetString(0), "yes") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_YES;
	else if(str_comp_nocase(pResult->GetString(0), "no") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_NO;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "admin forced vote %s", pResult->GetString(0));
	pSelf->SendChatTarget(-1, aBuf);
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pResult->GetString(0));
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CNetMsg_Sv_Motd Msg;
		Msg.m_pMessage = g_Config.m_SvMotd;
		CGameContext *pSelf = (CGameContext *)pUserData;
		for(int i = 0; i < MAX_CLIENTS; ++i)
			if(pSelf->m_apPlayers[i])
				pSelf->Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
	}
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	Console()->Register("tune", "si", CFGFLAG_SERVER, ConTuneParam, this, "Tune variable to value");
	Console()->Register("tune_reset", "", CFGFLAG_SERVER, ConTuneReset, this, "Reset tuning");
	Console()->Register("tune_dump", "", CFGFLAG_SERVER, ConTuneDump, this, "Dump tuning");

	Console()->Register("change_map", "?r", CFGFLAG_SERVER|CFGFLAG_STORE, ConChangeMap, this, "Change map");
	Console()->Register("restart", "?i", CFGFLAG_SERVER|CFGFLAG_STORE, ConRestart, this, "Restart in x seconds");
	Console()->Register("broadcast", "r", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("say", "r", CFGFLAG_SERVER, ConSay, this, "Say in chat");
	Console()->Register("set_team", "ii?i", CFGFLAG_SERVER, ConSetTeam, this, "Set team of player to team");
	Console()->Register("set_team_all", "i", CFGFLAG_SERVER, ConSetTeamAll, this, "Set team of all m_apPlayers to team");

	Console()->Register("add_vote", "sr", CFGFLAG_SERVER, ConAddVote, this, "Add a voting option");
	Console()->Register("remove_vote", "s", CFGFLAG_SERVER, ConRemoveVote, this, "remove a voting option");
	Console()->Register("force_vote", "ss?r", CFGFLAG_SERVER, ConForceVote, this, "Force a voting option");
	Console()->Register("clear_votes", "", CFGFLAG_SERVER, ConClearVotes, this, "Clears the voting options");
	Console()->Register("vote", "r", CFGFLAG_SERVER, ConVote, this, "Force a vote to yes/no");

	Console()->Register("teleport", "ii", CFGFLAG_SERVER, con_teleport, this, "");
	Console()->Register("teleport_to", "iii", CFGFLAG_SERVER, con_teleport_to, this, "");
	Console()->Register("get_pos", "i", CFGFLAG_SERVER, con_get_pos, this, "");
	Console()->Register("kill_pl", "i", CFGFLAG_SERVER, con_kill_pl, this, "");

	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);
}

void CGameContext::OnInit(/*class IKernel *pKernel*/)
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	//if(!data) // only load once
		//data = load_data_from_memory(internal_data);

	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	m_Layers.Init(Kernel());
	m_Collision.Init(&m_Layers);

	// reset everything here
	//world = new GAMEWORLD;
	//m_apPlayers = new CPlayer[MAX_CLIENTS];

	// select gametype
	if(str_comp(g_Config.m_SvGametype, "mod") == 0)
		m_pController = new CGameControllerMOD(this);
	else if(str_comp(g_Config.m_SvGametype, "ctf") == 0)
		m_pController = new CGameControllerCTF(this);
	else if(str_comp(g_Config.m_SvGametype, "tdm") == 0)
		m_pController = new CGameControllerTDM(this);
	else if(str_comp(g_Config.m_SvGametype, "race") == 0)
		m_pController = new CGameControllerRace(this);
	else if(str_comp(g_Config.m_SvGametype, "hprace") == 0)
		m_pController = new CGameControllerHPRace(this);
	else
		m_pController = new CGameControllerDM(this);

	// setup m_Core world
	//for(int i = 0; i < MAX_CLIENTS; i++)
	//	m_apPlayers[i].m_Core.world = &world.m_Core;

	// create all entities from the game layer
	CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	CTile *pTiles = (CTile *)Kernel()->RequestInterface<IMap>()->GetData(pTileMap->m_Data);




	/*
	num_spawn_points[0] = 0;
	num_spawn_points[1] = 0;
	num_spawn_points[2] = 0;
	*/

	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			int Index = pTiles[y*pTileMap->m_Width+x].m_Index;

			if(Index >= ENTITY_OFFSET)
			{
				vec2 Pos(x*32.0f+16.0f, y*32.0f+16.0f);
				m_pController->OnEntity(Index-ENTITY_OFFSET, Pos);
			}
		}
	}

	//world.insert_entity(Controller);

#ifdef CONF_DEBUG
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies ; i++)
		{
			OnClientConnected(MAX_CLIENTS-i-1);
		}
	}
#endif
}

void CGameContext::OnShutdown()
{
	delete m_pController;
	m_pController = 0;
	Clear();
}

void CGameContext::OnSnap(int ClientID)
{
	m_World.Snap(ClientID);
	m_pController->Snap(ClientID);
	m_Events.Snap(ClientID);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
			m_apPlayers[i]->Snap(ClientID);
	}
}
void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_Events.Clear();
}

bool CGameContext::IsClientReady(int ClientID)
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReady ? true : false;
}

bool CGameContext::IsClientPlayer(int ClientID)
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS ? false : true;
}

const char *CGameContext::GameType() { return m_pController && m_pController->m_pGameType ? m_pController->m_pGameType : ""; }
const char *CGameContext::Version() { return GAME_VERSION; }
const char *CGameContext::NetVersion() { return GAME_NETVERSION; }

IGameServer *CreateGameServer() { return new CGameContext; }
