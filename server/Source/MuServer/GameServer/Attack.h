#pragma once

#include "ProtocolDefines.h"
#include "User.h"

// Alas vanilla: porcentaje de daño que suman (Inc) y que absorben (Dec) =
// Base + PorNivel * nivel del ala.  También viajan en el catálogo (F3:FE) para
// el tooltip del cliente.
constexpr int WING1_INC_DAMAGE_BASE = 12;
constexpr int WING1_INC_DAMAGE_LEVEL = 2;
constexpr int WING1_DEC_DAMAGE_BASE = 12;
constexpr int WING1_DEC_DAMAGE_LEVEL = 2;
constexpr int WING2_INC_DAMAGE_BASE = 32;
constexpr int WING2_INC_DAMAGE_LEVEL = 1;
constexpr int WING2_DEC_DAMAGE_BASE = 25;
constexpr int WING2_DEC_DAMAGE_LEVEL = 2;

// Vida máxima que suma el Guardian Angel (también en el catálogo F3:FE).
constexpr int ANGEL_ADD_LIFE = 50;

//**********************************************//
//************ Client -> GameServer ************//
//**********************************************//

struct PMSG_ATTACK_RECV
{
	PBMSG_HEAD header; // C1:[PROTOCOL_CODE2]
	BYTE index[2];
	BYTE action;
	BYTE dir;
};

//**********************************************//
//**********************************************//
//**********************************************//

class CAttack
{
public:

	CAttack();

	~CAttack();

	bool Attack(LPOBJ lpObj, LPOBJ lpTarget, CSkill* lpSkill, bool send, int damage, int count);

	bool DecreaseArrow(LPOBJ lpObj);

	void WingSprite(LPOBJ lpObj, LPOBJ lpTarget, int* damage);

	void HelperSprite(LPOBJ lpObj, LPOBJ lpTarget, int* damage);

	void DamageSprite(LPOBJ lpObj, int damage);

	void WeaponDurabilityDown(LPOBJ lpObj, LPOBJ lpTarget);

	void ArmorDurabilityDown(LPOBJ lpObj, LPOBJ lpTarget);

	bool CheckPlayerTarget(LPOBJ lpObj, LPOBJ lpTarget);

	void MissSend(LPOBJ lpObj, LPOBJ lpTarget, CSkill* lpSkill, int send, int count);

	bool MissCheck(LPOBJ lpObj, LPOBJ lpTarget, CSkill* lpSkill, int send, int count, BYTE* miss);

	bool ApplySkillEffect(LPOBJ lpObj, LPOBJ lpTarget, int skill, int damage);

	int GetTargetDefense(LPOBJ lpObj, LPOBJ lpTarget, WORD* effect);

	int GetAttackDamage(LPOBJ lpObj, LPOBJ lpTarget, CSkill* lpSkill, WORD* effect, int TargetDefense);

	int GetAttackDamageWizard(LPOBJ lpObj, LPOBJ lpTarget, CSkill* lpSkill, WORD* effect, int TargetDefense);

	void GetPreviewDefense(LPOBJ lpObj, DWORD* defense);

	void GetPreviewPhysiDamage(LPOBJ lpObj, DWORD* DamageMin, DWORD* DamageMax);

	void GetPreviewMagicDamage(LPOBJ lpObj, DWORD* DamageMin, DWORD* DamageMax, DWORD* DamageRate);

	void CGAttackRecv(PMSG_ATTACK_RECV* lpMsg, int aIndex);
};

extern CAttack gAttack;
