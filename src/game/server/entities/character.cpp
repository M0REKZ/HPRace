/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <string.h>
#include <new>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemodes/race.h>
#include <game/server/gamemodes/hprace.h>
#include <game/mapitems.h>

#include "character.h"
#include "laser.h"
#include "projectile.h"

//input count
struct CInputCount
{
	int m_Presses;
	int m_Releases;
};

CInputCount CountInput(int Prev, int Cur)
{
	CInputCount c = {0, 0};
	Prev &= INPUT_STATE_MASK;
	Cur &= INPUT_STATE_MASK;
	int i = Prev;

	while(i != Cur)
	{
		i = (i+1)&INPUT_STATE_MASK;
		if(i&1)
			c.m_Presses++;
		else
			c.m_Releases++;
	}

	return c;
}


MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld)
: CEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER)
{
	m_ProximityRadius = ms_PhysSize;
	m_Health = 0;
	m_Armor = 0;
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_EmoteStop = -1;
	m_LastAction = -1;

	if(!GameServer()->m_pController->IsRace())
		m_ActiveWeapon = WEAPON_GUN;
	else
		m_ActiveWeapon = WEAPON_HAMMER;

	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;

	race_state = RACE_NONE;
	time = 0.0f;
	starttime = 0.0f;
	refreshtime = 0.0f;
	udeadbro = false;

	m_pPlayer = pPlayer;
	m_Pos = Pos;

	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, GameServer()->Collision());
	m_Core.m_Pos = m_Pos;
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));

	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);

	return true;
}

void CCharacter::Destroy()
{
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	m_Alive = false;
}

void CCharacter::SetWeapon(int W)
{
	if(W == m_ActiveWeapon)
		return;

	m_LastWeapon = m_ActiveWeapon;
	m_QueuedWeapon = -1;
	m_ActiveWeapon = W;
	GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH);

	if(m_ActiveWeapon < 0 || m_ActiveWeapon >= NUM_WEAPONS)
		m_ActiveWeapon = 0;
}

bool CCharacter::IsGrounded()
{
	if(GameServer()->Collision()->CheckPoint(m_Pos.x+m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	if(GameServer()->Collision()->CheckPoint(m_Pos.x-m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	return false;
}


void CCharacter::HandleNinja()
{
	if(m_ActiveWeapon != WEAPON_NINJA)
		return;

	if ((Server()->Tick() - m_Ninja.m_ActivationTick) > (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000))
	{
		// time's up, return
		m_aWeapons[WEAPON_NINJA].m_Got = false;
		m_ActiveWeapon = m_LastWeapon;

		SetWeapon(m_ActiveWeapon);
		return;
	}

	// force ninja Weapon
	SetWeapon(WEAPON_NINJA);

	m_Ninja.m_CurrentMoveTime--;

	if (m_Ninja.m_CurrentMoveTime == 0)
	{
		// reset velocity
		m_Core.m_Vel = m_Ninja.m_ActivationDir*m_Ninja.m_OldVelAmount;
	}

	if (m_Ninja.m_CurrentMoveTime > 0)
	{
		// Set velocity
		m_Core.m_Vel = m_Ninja.m_ActivationDir * g_pData->m_Weapons.m_Ninja.m_Velocity;
		vec2 OldPos = m_Pos;
		GameServer()->Collision()->MoveBox(&m_Core.m_Pos, &m_Core.m_Vel, vec2(m_ProximityRadius, m_ProximityRadius), 0.f);

		// reset velocity so the client doesn't predict stuff
		m_Core.m_Vel = vec2(0.f, 0.f);

		// check if we Hit anything along the way
		{
			CCharacter *aEnts[MAX_CLIENTS];
			vec2 Dir = m_Pos - OldPos;
			float Radius = m_ProximityRadius * 2.0f;
			vec2 Center = OldPos + Dir * 0.5f;
			int Num = GameServer()->m_World.FindEntities(Center, Radius, (CEntity**)aEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				if (aEnts[i] == this)
					continue;

				// make sure we haven't Hit this object before
				bool bAlreadyHit = false;
				for (int j = 0; j < m_NumObjectsHit; j++)
				{
					if (m_apHitObjects[j] == aEnts[i])
						bAlreadyHit = true;
				}
				if (bAlreadyHit)
					continue;

				// check so we are sufficiently close
				if (distance(aEnts[i]->m_Pos, m_Pos) > (m_ProximityRadius * 2.0f))
					continue;

				// Hit a player, give him damage and stuffs...
				GameServer()->CreateSound(aEnts[i]->m_Pos, SOUND_NINJA_HIT);
				// set his velocity to fast upward (for now)
				if(m_NumObjectsHit < 10)
					m_apHitObjects[m_NumObjectsHit++] = aEnts[i];

				aEnts[i]->TakeDamage(vec2(0, -10.0f), g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage, m_pPlayer->GetCID(), WEAPON_NINJA);
			}
		}

		return;
	}

	return;
}


void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(m_ReloadTimer != 0 || m_QueuedWeapon == -1 || m_aWeapons[WEAPON_NINJA].m_Got)
		return;

	if(GameServer()->m_pController->IsHPRace())
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon+1)%NUM_WEAPONS;
			if(m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon-1)<0?NUM_WEAPONS-1:WantedWeapon-1;
			if(m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon-1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_ActiveWeapon && m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer != 0)
		return;

	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_ActiveWeapon == WEAPON_GRENADE || m_ActiveWeapon == WEAPON_SHOTGUN || m_ActiveWeapon == WEAPON_RIFLE)
		FullAuto = true;


	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire&1) && m_aWeapons[m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire)
		return;

	// check for ammo
	if(!m_aWeapons[m_ActiveWeapon].m_Ammo)
	{
		// 125ms is a magical limit of how fast a human can click
		m_ReloadTimer = 125 * Server()->TickSpeed() / 1000;
		GameServer()->CreateSound(m_Pos, SOUND_WEAPON_NOAMMO);
		return;
	}

	vec2 ProjStartPos = m_Pos+Direction*m_ProximityRadius*0.75f;

	switch(m_ActiveWeapon)
	{
		case WEAPON_HAMMER:
		{
			// reset objects Hit
			m_NumObjectsHit = 0;
			if(m_pPlayer->GetPartner() && GameServer()->m_pController->IsHPRace())
			{
				GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE, CmaskOne(m_pPlayer->GetPartner()->GetCID()));
				GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE, CmaskOne(m_pPlayer->GetCID()));
			}
			else if(!GameServer()->m_pController->IsHPRace())
				GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE);

			CCharacter *apEnts[MAX_CLIENTS];
			int Hits = 0;
			int Num = -1;
			if(GameServer()->m_pController->IsHPRace())
				Num = GameServer()->m_World.FindEntities(ProjStartPos, m_ProximityRadius, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
			else if(!GameServer()->m_pController->IsRace())
				Num = GameServer()->m_World.FindEntities(ProjStartPos, m_ProximityRadius*0.5f, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				CCharacter *pTarget = apEnts[i];

				if(pTarget == this)
					continue;
				if(!GameServer()->m_pController->IsHPRace() && GameServer()->Collision()->IntersectLine(ProjStartPos, pTarget->m_Pos, NULL, NULL))
					continue;
				if(GameServer()->m_pController->IsHPRace() && (!m_pPlayer->GetPartner() || pTarget->m_pPlayer->GetCID() != m_pPlayer->GetPartner()->GetCID()))
					continue;

				// set his velocity to fast upward (for now)
				if(!GameServer()->m_pController->IsHPRace())
				{
					if(length(pTarget->m_Pos-ProjStartPos) > 0.0f)
						GameServer()->CreateHammerHit(pTarget->m_Pos-normalize(pTarget->m_Pos-ProjStartPos)*m_ProximityRadius*0.5f);
					else
						GameServer()->CreateHammerHit(ProjStartPos);
				}
				else if(m_pPlayer->GetPartner())
				{
					GameServer()->CreateSound(m_Pos, SOUND_HAMMER_HIT, CmaskOne(m_pPlayer->GetPartner()->GetCID()));
					GameServer()->CreateSound(m_Pos, SOUND_HAMMER_HIT, CmaskOne(m_pPlayer->GetCID()));
				}

				vec2 Dir;
				if (length(pTarget->m_Pos - m_Pos) > 0.0f)
				{
					if(!GameServer()->m_pController->IsHPRace())
						Dir = normalize(pTarget->m_Pos - m_Pos);
					else
						Dir = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));
				}
				else
					Dir = vec2(0.f, -1.f);

				if(!GameServer()->m_pController->IsHPRace())
					pTarget->TakeDamage(vec2(0.f, -1.0f) + normalize(Dir + vec2(0.0f, -1.1f)) * 10.0f, g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage, m_pPlayer->GetCID(), m_ActiveWeapon);
				else
					pTarget->TakeDamage(vec2(0.f, -1.0f) + Dir * 10.0f*g_Config.m_SvHammerPower, g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage, m_pPlayer->GetCID(), m_ActiveWeapon);
				Hits++;
			}

			// if we Hit anything, we have to wait for the reload
			if(Hits)
				m_ReloadTimer = Server()->TickSpeed()/3;

		} break;

		case WEAPON_GUN:
		{
			CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GUN,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GunLifetime),
				1, 0, 0, -1, WEAPON_GUN);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			pProj->FillInfo(&p);

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);

			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_GUN_FIRE);
		} break;

		case WEAPON_SHOTGUN:
		{
			int ShotSpread = 2;

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(ShotSpread*2+1);

			for(int i = -ShotSpread; i <= ShotSpread; ++i)
			{
				float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
				float a = GetAngle(Direction);
				a += Spreading[i+2];
				float v = 1-(absolute(i)/(float)ShotSpread);
				float Speed = mix((float)GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
				CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_SHOTGUN,
					m_pPlayer->GetCID(),
					ProjStartPos,
					vec2(cosf(a), sinf(a))*Speed,
					(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_ShotgunLifetime),
					1, 0, 0, -1, WEAPON_SHOTGUN);

				// pack the Projectile and send it to the client Directly
				CNetObj_Projectile p;
				pProj->FillInfo(&p);

				for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
					Msg.AddInt(((int *)&p)[i]);
			}

			Server()->SendMsg(&Msg, 0,m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_SHOTGUN_FIRE);
		} break;

		case WEAPON_GRENADE:
		{
			CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GRENADE,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GrenadeLifetime),
				1, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			pProj->FillInfo(&p);

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);
			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_FIRE);
		} break;

		case WEAPON_RIFLE:
		{
			new CLaser(GameWorld(), m_Pos, Direction, GameServer()->Tuning()->m_LaserReach, m_pPlayer->GetCID());
			GameServer()->CreateSound(m_Pos, SOUND_RIFLE_FIRE);
		} break;

		case WEAPON_NINJA:
		{
			// reset Hit objects
			m_NumObjectsHit = 0;

			m_Ninja.m_ActivationDir = Direction;
			m_Ninja.m_CurrentMoveTime = g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000;
			m_Ninja.m_OldVelAmount = length(m_Core.m_Vel);

			GameServer()->CreateSound(m_Pos, SOUND_NINJA_FIRE);
		} break;

	}

	m_AttackTick = Server()->Tick();

	if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0 && (!GameServer()->m_pController->IsRace() || !g_Config.m_SvInfiniteAmmo)) // -1 == unlimited
		m_aWeapons[m_ActiveWeapon].m_Ammo--;

	if(!m_ReloadTimer)
		m_ReloadTimer = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Firedelay * Server()->TickSpeed() / 1000;
}

void CCharacter::HandleWeapons()
{
	//ninja
	HandleNinja();

	// check reload timer
	if(m_ReloadTimer)
	{
		m_ReloadTimer--;
		return;
	}

	// fire Weapon, if wanted
	FireWeapon();

	// ammo regen
	int AmmoRegenTime = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Ammoregentime;
	if(AmmoRegenTime)
	{
		// If equipped and not active, regen ammo?
		if (m_ReloadTimer <= 0)
		{
			if (m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart < 0)
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = Server()->Tick();

			if ((Server()->Tick() - m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart) >= AmmoRegenTime * Server()->TickSpeed() / 1000)
			{
				// Add some ammo
				m_aWeapons[m_ActiveWeapon].m_Ammo = min(m_aWeapons[m_ActiveWeapon].m_Ammo + 1, 10);
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
			}
		}
		else
		{
			m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
		}
	}

	return;
}

bool CCharacter::GiveWeapon(int Weapon, int Ammo)
{
	if(m_aWeapons[Weapon].m_Ammo < g_pData->m_Weapons.m_aId[Weapon].m_Maxammo || !m_aWeapons[Weapon].m_Got)
	{
		m_aWeapons[Weapon].m_Got = true;
		m_aWeapons[Weapon].m_Ammo = min(g_pData->m_Weapons.m_aId[Weapon].m_Maxammo, Ammo);
		return true;
	}
	return false;
}

void CCharacter::GiveNinja()
{
	m_Ninja.m_ActivationTick = Server()->Tick();
	m_aWeapons[WEAPON_NINJA].m_Got = true;
	m_aWeapons[WEAPON_NINJA].m_Ammo = -1;
	if (m_ActiveWeapon != WEAPON_NINJA)
		m_LastWeapon = m_ActiveWeapon;
	m_ActiveWeapon = WEAPON_NINJA;

	GameServer()->CreateSound(m_Pos, SOUND_PICKUP_NINJA);
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_Input, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));
	m_NumInputs++;

	// or are not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));

	if(m_NumInputs > 2 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	m_Input.m_Hook = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire&1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

void CCharacter::HPRaceTick()
{
	m_Core.m_Vel.x = clamp((float)m_Core.m_Vel.x, (float)-500.0f, (float)500.0f); // fix the super fly bug
	m_Core.m_Vel.y = clamp((float)m_Core.m_Vel.y, (float)-500.0f, (float)500.0f);

	if(!m_pPlayer->GetPartner())
		return;

	// just prevent teleport hook bug
	int z = GameServer()->Collision()->IsTeleport(m_Pos.x, m_Pos.y);
	if(g_Config.m_SvTeleport && z)
	{
		for(int i = 0;i<MAX_CLIENTS;i++)
		{
			if(GameServer()->m_apPlayers[i] && i != m_pPlayer->GetCID() && GameServer()->m_apPlayers[i]->GetCharacter() 
				&& GameServer()->m_apPlayers[i]->GetCharacter()->m_Core.m_HookedPlayer == m_pPlayer->GetCID())
			{
				GameServer()->GetPlayerChar(i)->m_Core.m_HookedPlayer = -1;
				GameServer()->GetPlayerChar(i)->m_Core.m_HookState = HOOK_RETRACTED;
				GameServer()->GetPlayerChar(i)->m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
				GameServer()->GetPlayerChar(i)->m_Core.m_HookState = HOOK_RETRACTED;
				GameServer()->GetPlayerChar(i)->m_Core.m_HookPos = m_Core.m_Pos;
			}
		}
		m_Core.m_HookedPlayer = -1;
		m_Core.m_HookState = HOOK_RETRACTED;
		m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
		m_Core.m_HookPos = m_Core.m_Pos;
	}

	CCharacterCore *p = 0;
	if(m_pPlayer->GetPartnerChar())
		p = &m_pPlayer->GetPartnerChar()->m_Core;

	/*if(p)
	{
		float d = distance(m_Core.m_Pos+m_Core.m_Vel, p->m_Pos);
		vec2 Dir = normalize(m_Core.m_Pos+m_Core.m_Vel - p->m_Pos);
		if((d < ms_PhysSize*1.25f || distance(m_Core.m_Pos, p->m_Pos) < ms_PhysSize*1.25f) && d > 0.0f)
		{
			float a = (ms_PhysSize*1.45f - d);
			float Velocity = 0.5f;

			// make sure that we don't add excess force by checking the
			// Direction against the current m_Velocity
			vec2 VelDir = normalize(m_Core.m_Vel);
			if (length(VelDir) > 0.0001)
				Velocity = 1-(dot(VelDir, Dir)+1)/2;

			m_Core.m_Vel += Dir*a*(Velocity*0.75f);
			m_Core.m_Vel *= 0.85f;
		}

		// handle hook influence
		if(m_Core.m_HookedPlayer == m_pPlayer->GetPartner()->GetCID())
		{
			if(d > ms_PhysSize) // TODO: fix tweakable variable
			{
				float accel = GameServer()->Tuning()->m_HookDragAccel * (d/GameServer()->Tuning()->m_HookLength);
				float drag_speed = GameServer()->Tuning()->m_HookDragSpeed;

				// add force to the hooked m_pPlayer
				p->m_Vel.x = SaturatedAdd(-drag_speed, drag_speed, p->m_Vel.x, accel*Dir.x*1.5f);
				p->m_Vel.y = SaturatedAdd(-drag_speed, drag_speed, p->m_Vel.y, accel*Dir.y*1.5f);

				// add a little bit force to the guy who has the grip
				m_Core.m_Vel.x = SaturatedAdd(-drag_speed, drag_speed, m_Core.m_Vel.x, -accel*Dir.x*0.25f);
				m_Core.m_Vel.y = SaturatedAdd(-drag_speed, drag_speed, m_Core.m_Vel.y, -accel*Dir.y*0.25f);
			}
		}
		else if(m_Core.m_HookedPlayer >= 0)
		{
			vec2 Dir = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

			float angle = GetAngle(Dir);

			if(distance(m_Core.m_Pos,vec2(m_Core.m_HookPos.x+30.0f*cos(angle),m_Core.m_HookPos.y+30.0f*sin(angle))) <= GameServer()->Tuning()->m_HookLength)
			{
				m_Core.m_HookState = HOOK_FLYING;
				if(GameServer()->GetPlayerChar(m_Core.m_HookedPlayer))
					m_Core.m_HookPos += GameServer()->GetPlayerChar(m_Core.m_HookedPlayer)->m_Core.m_Vel+Dir*10.0f;
				m_Core.m_HookPos.x += 30.0f*cos(angle);
				m_Core.m_HookPos.y += 30.0f*sin(angle);
			}
			else
			{
				m_Core.m_HookState = HOOK_RETRACTED;
				m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
				m_Core.m_HookState = HOOK_IDLE;
			}
			m_Core.m_HookedPlayer = -1;
		}
	}*/

	// race
	char buftime[128];
	float f_time = (float)(Server()->Tick()-starttime)/((float)Server()->TickSpeed());
	z = GameServer()->Collision()->IsCheckpoint(m_Pos.x, m_Pos.y);
	CGameControllerHPRace *hp = (CGameControllerHPRace*)GameServer()->m_pController;

	if(race_state == RACE_STARTED)
	{
		this->time = f_time;
		hp->SetHPTeamScore(m_pPlayer->hprace_team, f_time);
	}

	if(race_state == RACE_STARTED && Server()->Tick()-refreshtime >= Server()->TickSpeed() && m_pPlayer->hprace_team>-1)
	{
		int int_time = (int)f_time;
		str_format(buftime, sizeof(buftime), "Current time: %d min %d sec", int_time/60, int_time%60);
		GameServer()->SendBroadcast(buftime, m_pPlayer->GetCID());
		refreshtime = Server()->Tick();
	}

	if(GameServer()->Collision()->GetIndex(m_Pos.x, m_Pos.y) == TILE_BEGIN)
	{
		starttime = Server()->Tick();
		refreshtime = Server()->Tick();
		race_state = RACE_STARTED;
	}
	if(race_state == RACE_STARTED && m_pPlayer->GetPartnerChar() && m_pPlayer->GetPartnerChar()->race_state == RACE_STARTED 
		&& m_pPlayer->GetPartnerChar()->time > this->time)
	{
		this->time = m_pPlayer->GetPartnerChar()->time;
		refreshtime = m_pPlayer->GetPartnerChar()->refreshtime;
		starttime = m_pPlayer->GetPartnerChar()->starttime;
	}
	else if(GameServer()->Collision()->GetIndex(m_Pos.x, m_Pos.y) == TILE_END && race_state == RACE_STARTED)
	{
		char buf[128];
		str_format(buf, sizeof(buf), "%s & %s finished in: %d minute(s) %5.3f second(s)", Server()->ClientName(m_pPlayer->GetCID()),
			Server()->ClientName(m_pPlayer->GetPartner()->GetCID()), (int)f_time/60, f_time-((int)f_time/60*60));
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, buf);

		HPRACE_TEAM_SCORE *PScore = ((CGameControllerHPRace*)GameServer()->m_pController)->score.search_team(Server()->ClientName(m_pPlayer->GetCID()), Server()->ClientName(m_pPlayer->GetPartner()->GetCID()), 0);
		if(PScore && f_time - PScore->score < 0)
		{
			str_format(buf, sizeof(buf), "New record: %5.3f second(s) better", f_time - PScore->score);
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, buf);
		}

		int ttime = 0-(int)f_time;
		race_state = RACE_FINISHED;

		if(m_pPlayer->m_Score < ttime)
		{
			m_pPlayer->m_Score = ttime;
			m_pPlayer->GetPartner()->m_Score = ttime;
		}

		if(strncmp(Server()->ClientName(m_pPlayer->GetCID()), "nameless tee", 12) != 0 &&
			strncmp(Server()->ClientName(m_pPlayer->GetPartner()->GetCID()), "nameless tee", 12) != 0)
			((CGameControllerHPRace*)GameServer()->m_pController)->score.parseTeam(hp->GetHPTeam(m_pPlayer->hprace_team));

		if(m_pPlayer->GetPartnerChar())
		{
			m_pPlayer->GetPartnerChar()->race_state = RACE_FINISHED;
			m_pPlayer->GetPartner()->KillCharacter(-1);
		}
		udeadbro = true;
	}

	z = GameServer()->Collision()->IsTeleport(m_Pos.x, m_Pos.y);
	if(g_Config.m_SvTeleport && z)
	{
		m_Core.m_HookedPlayer = -1;
		m_Core.m_HookState = HOOK_RETRACTED;
		m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
		m_Core.m_Pos = GameServer()->Collision()->Teleport(z);
		m_Core.m_HookPos = m_Core.m_Pos;
		if(g_Config.m_SvStrip)
		{
			m_ActiveWeapon = WEAPON_HAMMER;
			m_LastWeapon = WEAPON_HAMMER;
			m_aWeapons[0].m_Got = true;
			for(int i = 1; i < 5; i++)
				m_aWeapons[i].m_Got = false;
		}
	}
}

void CCharacter::Tick()
{
	if(m_pPlayer->m_ForceBalanced)
	{
		char Buf[128];
		str_format(Buf, sizeof(Buf), "You were moved to %s due to team balancing", GameServer()->m_pController->GetTeamName(m_pPlayer->GetTeam()));
		GameServer()->SendBroadcast(Buf, m_pPlayer->GetCID());

		m_pPlayer->m_ForceBalanced = false;
	}

	m_Core.m_Input = m_Input;

	if(GameServer()->m_pController->IsHPRace())
	{
		if(m_pPlayer->GetPartner())
			GameServer()->m_World.m_Core.m_Tuning.m_PlayerCollision = m_pPlayer->GetPartner()->GetCID()+2;
		else
			GameServer()->m_World.m_Core.m_Tuning.m_PlayerCollision = 1;
	}
	m_Core.Tick(true);
	if(GameServer()->m_pController->IsHPRace())
		GameServer()->m_World.m_Core.m_Tuning.m_PlayerCollision = 1;

	int z = GameServer()->Collision()->IsCheckpoint(m_Pos.x, m_Pos.y);

	if(GameServer()->m_pController->IsHPRace())
	{
		HPRaceTick();
		if(udeadbro)
		{
			m_pPlayer->KillCharacter(-1);
			return;
		}
	}
	else
	{
		// race
		char buftime[128];
		float time = (float)(Server()->Tick()-starttime)/((float)Server()->TickSpeed());

		if(z && race_state == RACE_STARTED)
		{
			cp_active = z;
			cp_current[z] = time;
			cp_tick = Server()->Tick() + Server()->TickSpeed()*2;
		}
		if(race_state == RACE_STARTED && Server()->Tick()-refreshtime >= Server()->TickSpeed())
		{
			int int_time = (int)time;
			str_format(buftime, sizeof(buftime), "Current time: %d min %d sec", int_time/60, int_time%60);

			if(cp_active && cp_tick > Server()->Tick())
			{
				PLAYER_SCORE *PScore = ((CGameControllerRace*)GameServer()->m_pController)->score.search_score(m_pPlayer->GetCID(), 0, 0);
				if(PScore && PScore->cp_time[cp_active] != 0)
				{
					char tmp[128];
					float diff = cp_current[cp_active] - PScore->cp_time[cp_active];
					str_format(tmp, sizeof(tmp), "\nCheckpoint | Diff : %s%5.3f", (diff >= 0)?"+":"", diff);
					strcat(buftime, tmp);
				}
			}

			GameServer()->SendBroadcast(buftime, m_pPlayer->GetCID());
			refreshtime = Server()->Tick();
		}

		if(g_Config.m_SvRegen > 0 && (Server()->Tick()%g_Config.m_SvRegen) == 0 && GameServer()->m_pController->IsRace())
		{
			if(m_Health < 10) 
				m_Health++;
			else if(m_Armor < 10)
				m_Armor++;
		}

		if(GameServer()->Collision()->GetIndex(m_Pos.x, m_Pos.y) == TILE_BEGIN && GameServer()->m_pController->IsRace() && (!m_aWeapons[WEAPON_GRENADE].m_Got || race_state == RACE_NONE))
		{
			starttime = Server()->Tick();
			refreshtime = Server()->Tick();
			race_state = RACE_STARTED;
		}
		else if(GameServer()->Collision()->GetIndex(m_Pos.x, m_Pos.y) == TILE_END && race_state == RACE_STARTED)
		{
			char buf[128];
			str_format(buf, sizeof(buf), "%s finished in: %d minute(s) %5.3f second(s)", Server()->ClientName(m_pPlayer->GetCID()), (int)time/60, time-((int)time/60*60));
			GameServer()->SendChat(-1,CGameContext::CHAT_ALL, buf);
			
			PLAYER_SCORE *PScore = ((CGameControllerRace*)GameServer()->m_pController)->score.search_score(m_pPlayer->GetCID(), 0, 0);
			if(PScore && time - PScore->score < 0)
			{
				str_format(buf, sizeof(buf), "New record: %5.3f second(s) better", time - PScore->score);
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, buf);
			}
			
			int ttime = 0-(int)time;
			race_state = RACE_FINISHED;
			
			if(m_pPlayer->m_Score < ttime)
				m_pPlayer->m_Score = ttime;
			if(strncmp(Server()->ClientName(m_pPlayer->GetCID()), "nameless tee", 12) != 0)
				((CGameControllerRace*)GameServer()->m_pController)->score.parsePlayer(m_pPlayer->GetCID(), (float)time, cp_current);
		}
		z = GameServer()->Collision()->IsTeleport(m_Pos.x, m_Pos.y);
		if(g_Config.m_SvTeleport && z && GameServer()->m_pController->IsRace())
		{
			m_Core.m_HookedPlayer = -1;
			m_Core.m_HookState = HOOK_RETRACTED;
			m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
			m_Core.m_Pos = GameServer()->Collision()->Teleport(z);
			m_Core.m_HookPos = m_Core.m_Pos;
			if(g_Config.m_SvStrip)
			{
				m_ActiveWeapon = WEAPON_HAMMER;
				m_LastWeapon = WEAPON_HAMMER;
				m_aWeapons[0].m_Got = true;
				for(int i = 1; i < 5; i++)
					m_aWeapons[i].m_Got = false;
			}
		}
	}

	z = GameServer()->Collision()->IsCheckpoint(m_Pos.x, m_Pos.y);
	if(GameServer()->Collision()->GetIndex(m_Core.m_Pos.x, m_Core.m_Pos.y) == TILE_BOOST && GameServer()->m_pController->IsRace())
	{
		if(m_Core.m_Vel.x >= 0)
			m_Core.m_Vel.x = m_Core.m_Vel.x*(((float)g_Config.m_SvSpeedupMult)/10.0)+g_Config.m_SvSpeedupAdd;
		else 
			m_Core.m_Vel.x = m_Core.m_Vel.x*(((float)g_Config.m_SvSpeedupMult)/10.0)-g_Config.m_SvSpeedupAdd;
	}
	else if(GameServer()->Collision()->GetIndex(m_Core.m_Pos.x, m_Core.m_Pos.y) == TILE_BOOSTR && GameServer()->m_pController->IsRace())
	{
		if(m_Core.m_Vel.x >= 0)
			m_Core.m_Vel.x = m_Core.m_Vel.x*(((float)g_Config.m_SvSpeedupMult)/10.0)+g_Config.m_SvSpeedupAdd;
		else 
			m_Core.m_Vel.x = g_Config.m_SvSpeedupAdd;
	}
	else if(GameServer()->Collision()->GetIndex(m_Core.m_Pos.x, m_Core.m_Pos.y) == TILE_BOOSTL && GameServer()->m_pController->IsRace())
	{
		if(m_Core.m_Vel.x <= 0)
			m_Core.m_Vel.x = m_Core.m_Vel.x*(((float)g_Config.m_SvSpeedupMult)/10.0)-g_Config.m_SvSpeedupAdd;
		else
			m_Core.m_Vel.x = 0-g_Config.m_SvSpeedupAdd;
	}
	else if(GameServer()->Collision()->GetIndex(m_Core.m_Pos.x, m_Core.m_Pos.y) == TILE_JUMPER && GameServer()->m_pController->IsRace())
		m_Core.m_Vel.y -= g_Config.m_SvJumperAdd;

	// handle death-tiles and leaving GameServer()layer
	if(GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameLayerClipped(m_Pos))
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}

	// handle Weapons
	HandleWeapons();

	// Previnput
	m_PrevInput = m_Input;
	return;
}

void CCharacter::TickDefered()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, GameServer()->Collision());
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	//lastsentCore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));

	if(GameServer()->m_pController->IsHPRace())
	{
		if(m_pPlayer->GetPartner())
			GameServer()->m_World.m_Core.m_Tuning.m_PlayerCollision = m_pPlayer->GetPartner()->GetCID()+2;
		else
			GameServer()->m_World.m_Core.m_Tuning.m_PlayerCollision = 1;
	}
	m_Core.Move();
	if(GameServer()->m_pController->IsHPRace())
		GameServer()->m_World.m_Core.m_Tuning.m_PlayerCollision = 1;
	bool StuckAfterMove = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Core.Quantize();
	bool StuckAfterQuant = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		}StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	int EvapEnts = m_Core.m_TriggeredEvents;
	int Mask = CmaskAllExceptOne(m_pPlayer->GetCID());

	if(!GameServer()->m_pController->IsHPRace())
	{
		if(EvapEnts&COREEVENT_GROUND_JUMP) GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, Mask);
		if(EvapEnts&COREEVENT_HOOK_ATTACH_PLAYER) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskAll());
		if(EvapEnts&COREEVENT_HOOK_ATTACH_GROUND) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, Mask);
		if(EvapEnts&COREEVENT_HOOK_HIT_NOHOOK) GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, Mask);
	}
	else if(m_pPlayer->GetPartnerChar()) // allow hear just partner's sound
	{
		int pid = m_pPlayer->GetPartner()->GetCID();
		if(EvapEnts&COREEVENT_GROUND_JUMP) GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, CmaskOne(pid));
		if(EvapEnts&COREEVENT_HOOK_ATTACH_PLAYER && m_Core.m_HookedPlayer == pid) 
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskOne(pid));
		if(EvapEnts&COREEVENT_HOOK_ATTACH_GROUND) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, CmaskOne(pid));
		if(EvapEnts&COREEVENT_HOOK_HIT_NOHOOK) GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, CmaskOne(pid));
		if(EvapEnts&COREEVENT_HOOK_ATTACH_PLAYER && m_Core.m_HookedPlayer == pid)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskOne(m_pPlayer->GetCID()));
	}


	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}

	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		//reckoningm_Core.write(&predicted);
		m_Core.Write(&Current);
		m_ReckoningCore = m_Core;
		m_ReckoningCore.m_Vel += m_Core.m_Vel;
		m_ReckoningCore.m_Vel.y += GameServer()->Tuning()->m_Gravity;
		m_ReckoningCore.Write(&Predicted);

		// only allow dead reackoning for a top of 3 seconds
		if(m_ReckoningTick+Server()->TickSpeed()*3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}
	}
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10)
		return false;
	m_Health = clamp(m_Health+Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = clamp(m_Armor+Amount, 0, 10);
	return true;
}

void CCharacter::Die(int Killer, int Weapon)
{
	// we got to wait 0.5 secs before respawning
	m_pPlayer->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()/2;
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	// a nice sound
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE);

	// this is for auto respawn after 3 secs
	m_pPlayer->m_DieTick = Server()->Tick();

	m_Alive = false;
	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon)
{
	m_Core.m_Vel += Force;

	if(GameServer()->m_pController->IsFriendlyFire(m_pPlayer->GetCID(), From) && !g_Config.m_SvTeamdamage)
		return false;
	if(GameServer()->m_pController->IsHPRace())
		return false;

	// m_pPlayer only inflicts half damage on self
	if(From == m_pPlayer->GetCID())
		Dmg = max(1, Dmg/2);
 
	if(((From == m_pPlayer->GetCID() && !g_Config.m_SvRocketJumpDamage) || From != m_pPlayer->GetCID()) && GameServer()->m_pController->IsRace())
		Dmg = 0;

	m_DamageTaken++;

	// create healthmod indicator
	if(Server()->Tick() < m_DamageTakenTick+25)
	{
		// make sure that the damage indicators doesn't group together
		GameServer()->CreateDamageInd(m_Pos, m_DamageTaken*0.25f, Dmg);
	}
	else
	{
		m_DamageTaken = 0;
		GameServer()->CreateDamageInd(m_Pos, 0, Dmg);
	}

	if(Dmg)
	{
		if(m_Armor)
		{
			if(Dmg > 1)
			{
				m_Health--;
				Dmg--;
			}

			if(Dmg > m_Armor)
			{
				Dmg -= m_Armor;
				m_Armor = 0;
			}
			else
			{
				m_Armor -= Dmg;
				Dmg = 0;
			}
		}

		m_Health -= Dmg;
	}

	m_DamageTakenTick = Server()->Tick();

	// do damage Hit sound
	if(From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
	{
		int Mask = CmaskOne(From);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS && GameServer()->m_apPlayers[i]->m_SpectatorID == From)
				Mask |= CmaskOne(i);
		}
		GameServer()->CreateSound(GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, Mask);
	}

	// check for death
	if(m_Health <= 0)
	{
		Die(From, Weapon);

		// set attacker's face to happy (taunt!)
		if (From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
		{
			CCharacter *pChr = GameServer()->m_apPlayers[From]->GetCharacter();
			if (pChr)
			{
				pChr->m_EmoteType = EMOTE_HAPPY;
				pChr->m_EmoteStop = Server()->Tick() + Server()->TickSpeed();
			}
		}

		return false;
	}

	if (Dmg > 2)
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG);
	else
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);

	m_EmoteType = EMOTE_PAIN;
	m_EmoteStop = Server()->Tick() + 500 * Server()->TickSpeed() / 1000;

	return true;
}

void CCharacter::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
 	
	if(GameServer()->m_apPlayers[SnappingClient] && GameServer()->m_apPlayers[SnappingClient]->GetPartner()
		&& SnappingClient != m_pPlayer->GetCID() && m_pPlayer->GetCID() != GameServer()->m_apPlayers[SnappingClient]->GetPartner()->GetCID())
		return;

	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, m_pPlayer->GetCID(), sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;

	// write down the m_Core
	if(!m_ReckoningTick || GameServer()->m_World.m_Paused)
	{
		// no dead reckoning when paused because the client doesn't know
		// how far to perform the reckoning
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter);
	}
	else
	{
		pCharacter->m_Tick = m_ReckoningTick;
		m_SendCore.Write(pCharacter);
	}

	// set emote
	if (m_EmoteStop < Server()->Tick())
	{
		m_EmoteType = EMOTE_NORMAL;
		m_EmoteStop = -1;
	}

	pCharacter->m_Emote = m_EmoteType;

	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;

	pCharacter->m_Weapon = m_ActiveWeapon;
	pCharacter->m_AttackTick = m_AttackTick;

	pCharacter->m_Direction = m_Input.m_Direction;

	if(m_pPlayer->GetCID() == SnappingClient || SnappingClient == -1 ||
		(!g_Config.m_SvStrictSpectateMode && m_pPlayer->GetCID() == GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID))
	{
		pCharacter->m_Health = m_Health;
		pCharacter->m_Armor = m_Armor;
		if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0)
			pCharacter->m_AmmoCount = m_aWeapons[m_ActiveWeapon].m_Ammo;
	}

	if(pCharacter->m_Emote == EMOTE_NORMAL)
	{
		if(250 - ((Server()->Tick() - m_LastAction)%(250)) < 5)
			pCharacter->m_Emote = EMOTE_BLINK;
	}

	pCharacter->m_PlayerFlags = GetPlayer()->m_PlayerFlags;
}
