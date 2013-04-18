// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/showmsg.h"
#include "../common/ers.h"
#include "../common/random.h"
#include "../common/socket.h"
#include "../common/strlib.h"
#include "../common/utils.h"

#include "map.h"
#include "path.h"
#include "pc.h"
#include "status.h"
#include "skill.h"
#include "homunculus.h"
#include "mercenary.h"
#include "elemental.h"
#include "mob.h"
#include "itemdb.h"
#include "clif.h"
#include "pet.h"
#include "guild.h"
#include "party.h"
#include "battle.h"
#include "battleground.h"
#include "chrif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int attr_fix_table[4][ELE_MAX][ELE_MAX];

struct Battle_Config battle_config;
static struct eri *delay_damage_ers; //For battle delay damage structures.

int battle_getcurrentskill(struct block_list *bl) {	//Returns the current/last skill in use by this bl.
	struct unit_data *ud;

	if( bl->type == BL_SKILL ) {
		struct skill_unit * su = (struct skill_unit*)bl;
		return su->group?su->group->skill_id:0;
	}
	
	ud = unit_bl2ud(bl);
	
	return ud?ud->skill_id:0;
}

/*==========================================
 * Get random targetting enemy
 *------------------------------------------*/
static int battle_gettargeted_sub(struct block_list *bl, va_list ap) {
	struct block_list **bl_list;
	struct unit_data *ud;
	int target_id;
	int *c;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	target_id = va_arg(ap, int);

	if (bl->id == target_id)
		return 0;
		
	if (*c >= 24)
		return 0;

	if ( !(ud = unit_bl2ud(bl)) )
		return 0;

	if (ud->target == target_id || ud->skilltarget == target_id) {
		bl_list[(*c)++] = bl;
		return 1;
	}
	
	return 0;
}

struct block_list* battle_gettargeted(struct block_list *target) {
	struct block_list *bl_list[24];
	int c = 0;
	nullpo_retr(NULL, target);

	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinrange(battle_gettargeted_sub, target, AREA_SIZE, BL_CHAR, bl_list, &c, target->id);
	if ( c == 0 )
		return NULL;
	if ( c > 24 )
		c = 24;
	return bl_list[rnd()%c];
}


//Returns the id of the current targetted character of the passed bl. [Skotlex]
int battle_gettarget(struct block_list* bl) {

	switch (bl->type) {
		case BL_PC:  return ((struct map_session_data*)bl)->ud.target;
		case BL_MOB: return ((struct mob_data*)bl)->target_id;
		case BL_PET: return ((struct pet_data*)bl)->target_id;
		case BL_HOM: return ((struct homun_data*)bl)->ud.target;
		case BL_MER: return ((struct mercenary_data*)bl)->ud.target;
		case BL_ELEM: return ((struct elemental_data*)bl)->ud.target;
	}
	
	return 0;
}

static int battle_getenemy_sub(struct block_list *bl, va_list ap) {
	struct block_list **bl_list;
	struct block_list *target;
	int *c;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	target = va_arg(ap, struct block_list *);

	if (bl->id == target->id)
		return 0;
		
	if (*c >= 24)
		return 0;
		
	if (status_isdead(bl))
		return 0;
		
	if (battle_check_target(target, bl, BCT_ENEMY) > 0) {
		bl_list[(*c)++] = bl;
		return 1;
	}
	
	return 0;
}

// Picks a random enemy of the given type (BL_PC, BL_CHAR, etc) within the range given. [Skotlex]
struct block_list* battle_getenemy(struct block_list *target, int type, int range) {
	struct block_list *bl_list[24];
	int c = 0;
	
	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinrange(battle_getenemy_sub, target, range, type, bl_list, &c, target);
	
	if ( c == 0 )
		return NULL;
		
	if ( c > 24 )
		c = 24;
		
	return bl_list[rnd()%c];
}
static int battle_getenemyarea_sub(struct block_list *bl, va_list ap) {
	struct block_list **bl_list, *src;
	int *c, ignore_id;

	bl_list = va_arg(ap, struct block_list **);
	c = va_arg(ap, int *);
	src = va_arg(ap, struct block_list *);
	ignore_id = va_arg(ap, int);

	if( bl->id == src->id || bl->id == ignore_id )
		return 0; // Ignores Caster and a possible pre-target
		
	if( *c >= 23 )
		return 0;
		
	if( status_isdead(bl) )
		return 0;
		
	if( battle_check_target(src, bl, BCT_ENEMY) > 0 ) { // Is Enemy!...
		bl_list[(*c)++] = bl;
		return 1;
	}
	
	return 0;
}

// Pick a random enemy
struct block_list* battle_getenemyarea(struct block_list *src, int x, int y, int range, int type, int ignore_id) {
	struct block_list *bl_list[24];
	int c = 0;
	
	memset(bl_list, 0, sizeof(bl_list));
	map_foreachinarea(battle_getenemyarea_sub, src->m, x - range, y - range, x + range, y + range, type, bl_list, &c, src, ignore_id);
	
	if( c == 0 )
		return NULL;
	if( c >= 24 )
		c = 23;
		
	return bl_list[rnd()%c];
}

// Dammage delayed info
struct delay_damage {
	int src_id;
	int target_id;
	int damage;
	int delay;
	unsigned short distance;
	uint16 skill_lv;
	uint16 skill_id;
	enum damage_lv dmg_lv;
	unsigned short attack_type;
	bool additional_effects;
};

int battle_delay_damage_sub(int tid, unsigned int tick, int id, intptr_t data) {
	struct delay_damage *dat = (struct delay_damage *)data;
	
	if ( dat ) {
		struct block_list* src;
		struct block_list* target = map_id2bl(dat->target_id);
		
		if( !target || status_isdead(target) ) {/* nothing we can do */
			ers_free(delay_damage_ers, dat);
			return 0;
		}
			
		src = map_id2bl(dat->src_id);
		
		if( src && target->m == src->m &&
			(target->type != BL_PC || ((TBL_PC*)target)->invincible_timer == INVALID_TIMER) &&
			check_distance_bl(src, target, dat->distance) ) //Check to see if you haven't teleported. [Skotlex]
		{
			map_freeblock_lock();
			status_fix_damage(src, target, dat->damage, dat->delay);
			if( dat->attack_type && !status_isdead(target) && dat->additional_effects )
				skill_additional_effect(src,target,dat->skill_id,dat->skill_lv,dat->attack_type,dat->dmg_lv,tick);
			if( dat->dmg_lv > ATK_BLOCK && dat->attack_type )
				skill_counter_additional_effect(src,target,dat->skill_id,dat->skill_lv,dat->attack_type,tick);
			map_freeblock_unlock();
		} else if( !src && dat->skill_id == CR_REFLECTSHIELD ) {
			/**
			 * it was monster reflected damage, and the monster died, we pass the damage to the character as expected
			 **/
			map_freeblock_lock();
			status_fix_damage(target, target, dat->damage, dat->delay);
			map_freeblock_unlock();
		}
	}
	ers_free(delay_damage_ers, dat);
	return 0;
}

int battle_delay_damage(unsigned int tick, int amotion, struct block_list *src, struct block_list *target, int attack_type, uint16 skill_id, uint16 skill_lv, int damage, enum damage_lv dmg_lv, int ddelay, bool additional_effects)
{
	struct delay_damage *dat;
	struct status_change *sc;
	nullpo_ret(src);
	nullpo_ret(target);

	sc = status_get_sc(target);

	if( sc && sc->data[SC_DEVOTION] && damage > 0 && skill_id != PA_PRESSURE && skill_id != CR_REFLECTSHIELD )
		damage = 0;

	if ( !battle_config.delay_battle_damage || amotion <= 1 ) {
		map_freeblock_lock();
		status_fix_damage(src, target, damage, ddelay); // We have to seperate here between reflect damage and others [icescope]
		if( attack_type && !status_isdead(target) && additional_effects )
			skill_additional_effect(src, target, skill_id, skill_lv, attack_type, dmg_lv, gettick());
		if( dmg_lv > ATK_BLOCK && attack_type )
			skill_counter_additional_effect(src, target, skill_id, skill_lv, attack_type, gettick());
		map_freeblock_unlock();
		return 0;
	}
	dat = ers_alloc(delay_damage_ers, struct delay_damage);
	dat->src_id = src->id;
	dat->target_id = target->id;
	dat->skill_id = skill_id;
	dat->skill_lv = skill_lv;
	dat->attack_type = attack_type;
	dat->damage = damage;
	dat->dmg_lv = dmg_lv;
	dat->delay = ddelay;
	dat->distance = distance_bl(src, target)+10; //Attack should connect regardless unless you teleported.
	dat->additional_effects = additional_effects;
	if (src->type != BL_PC && amotion > 1000)
		amotion = 1000; //Aegis places a damage-delay cap of 1 sec to non player attacks. [Skotlex]

	add_timer(tick+amotion, battle_delay_damage_sub, 0, (intptr_t)dat);

	return 0;
}
int battle_attr_ratio(int atk_elem,int def_type, int def_lv)
{

	if (atk_elem < 0 || atk_elem >= ELE_MAX)
		return 100;

	if (def_type < 0 || def_type > ELE_MAX || def_lv < 1 || def_lv > 4)
		return 100;

	return attr_fix_table[def_lv-1][atk_elem][def_type];
}

/*==========================================
 * Does attribute fix modifiers.
 * Added passing of the chars so that the status changes can affect it. [Skotlex]
 * Note: Passing src/target == NULL is perfectly valid, it skips SC_ checks.
 *------------------------------------------*/
int battle_attr_fix(struct block_list *src, struct block_list *target, int damage,int atk_elem,int def_type, int def_lv)
{
	struct status_change *sc=NULL, *tsc=NULL;
	int ratio;

	if (src) sc = status_get_sc(src);
	if (target) tsc = status_get_sc(target);
	
	if (atk_elem < 0 || atk_elem >= ELE_MAX)
		atk_elem = rnd()%ELE_MAX;

	if (def_type < 0 || def_type > ELE_MAX ||
		def_lv < 1 || def_lv > 4) {
		ShowError("battle_attr_fix: unknown attr type: atk=%d def_type=%d def_lv=%d\n",atk_elem,def_type,def_lv);
		return damage;
	}

	ratio = attr_fix_table[def_lv-1][atk_elem][def_type];
	if (sc && sc->count) {
		if(sc->data[SC_VOLCANO] && atk_elem == ELE_FIRE)
			ratio += enchant_eff[sc->data[SC_VOLCANO]->val1-1];
		if(sc->data[SC_VIOLENTGALE] && atk_elem == ELE_WIND)
			ratio += enchant_eff[sc->data[SC_VIOLENTGALE]->val1-1];
		if(sc->data[SC_DELUGE] && atk_elem == ELE_WATER)
			ratio += enchant_eff[sc->data[SC_DELUGE]->val1-1];
	}
	if( target && target->type == BL_SKILL ) {
		if( atk_elem == ELE_FIRE && battle_getcurrentskill(target) == GN_WALLOFTHORN ) {
			struct skill_unit *su = (struct skill_unit*)target;
			struct skill_unit_group *sg;
			struct block_list *src;

			if( !su || !su->alive || (sg = su->group) == NULL || !sg || sg->val3 == -1 ||
			   (src = map_id2bl(sg->src_id)) == NULL || status_isdead(src) )
				return 0;

			if( sg->unit_id != UNT_FIREWALL ) {
				int x,y;
				x = sg->val3 >> 16;
				y = sg->val3 & 0xffff;
				skill_unitsetting(src,su->group->skill_id,su->group->skill_lv,x,y,1);
				sg->val3 = -1;
				sg->limit = DIFF_TICK(gettick(),sg->tick)+300;
			}
		}
	}
	if( tsc && tsc->count ) { //since an atk can only have one type let's optimise this a bit
		switch( atk_elem ) {
			case ELE_FIRE:
				if( tsc->data[SC_SPIDERWEB] ) {
					tsc->data[SC_SPIDERWEB]->val1 = 0; // free to move now
						if( tsc->data[SC_SPIDERWEB]->val2-- > 0 )
							damage <<= 1; // double damage
						if( tsc->data[SC_SPIDERWEB]->val2 == 0 )
							status_change_end(target, SC_SPIDERWEB, INVALID_TIMER);
				}
				if( tsc->data[SC_THORNSTRAP] )
					status_change_end(target, SC_THORNSTRAP, INVALID_TIMER);
				if( tsc->data[SC_CRYSTALIZE] )
					status_change_end(target, SC_CRYSTALIZE, INVALID_TIMER);
				if( tsc->data[SC_EARTH_INSIGNIA] ) damage += damage / 2;
				if( tsc->data[SC_ASH] ) damage += damage / 2; //150%
				break;
			case ELE_HOLY:
				if( tsc->data[SC_ORATIO] ) ratio += tsc->data[SC_ORATIO]->val1 * 2;
				break;
			case ELE_POISON:
				if( tsc->data[SC_VENOMIMPRESS] ) ratio += tsc->data[SC_VENOMIMPRESS]->val2;
				break;
			case ELE_WIND:
				if( tsc->data[SC_CRYSTALIZE] ) damage += damage / 2;
				if( tsc->data[SC_WATER_INSIGNIA] ) damage += damage / 2;
				break;
			case ELE_WATER:
				if( tsc->data[SC_FIRE_INSIGNIA] ) damage += damage / 2;
				break;
			case ELE_EARTH:
				if( tsc->data[SC_WIND_INSIGNIA] ) damage += damage / 2;
				break;
		}
	} //end tsc check
	if( src && src->type == BL_PC ) {
		struct map_session_data *sd = BL_CAST(BL_PC, src);
		int s;

		ARR_FIND(1, 6, s, sd->talisman[s] > 0);

		if( s < 5 && atk_elem == s )
			ratio += sd->talisman[s] * 15; // +15% custom value
	}
	if( target && target->type == BL_PC ) {
		struct map_session_data *tsd = BL_CAST(BL_PC, target);
		int t;

		ARR_FIND(1, 6, t, tsd->talisman[t] > 0);

		if( t < 5 && atk_elem == t )
			damage -= damage * ( tsd->talisman[t] * 15 ) / 100; // -15% custom value
	}
	return damage*ratio/100;
}

/*==========================================
 * Calculates card bonuses damage adjustments.
 *------------------------------------------*/
int battle_calc_cardfix(int attack_type, struct block_list *src, struct block_list *target, int nk, int s_ele, int s_ele_, int damage, int left, int flag){
	struct map_session_data *sd, *tsd;
	short cardfix = 1000, t_class, s_class, s_race2, t_race2;
	struct status_data *sstatus, *tstatus;
	int i;

	if( !damage )
		return 0;

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);
	t_class = status_get_class(target);
	s_class = status_get_class(src);
	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);
	s_race2 = status_get_race2(src);

	switch( attack_type ) {
		case BF_MAGIC:
			if( sd && !(nk&NK_NO_CARDFIX_ATK) ) {
				cardfix=cardfix*(100+sd->magic_addrace[tstatus->race])/100;
				if( !(nk&NK_NO_ELEFIX) )
					cardfix=cardfix*(100+sd->magic_addele[tstatus->def_ele])/100;
				cardfix=cardfix*(100+sd->magic_addsize[tstatus->size])/100;
				cardfix=cardfix*(100+sd->magic_addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
				cardfix=cardfix*(100+sd->magic_atk_ele[s_ele])/100;
				for( i=0; i< ARRAYLENGTH(sd->add_mdmg) && sd->add_mdmg[i].rate;i++ ) {
					if( sd->add_mdmg[i].class_ == t_class ) {
						cardfix=cardfix*(100+sd->add_mdmg[i].rate)/100;
						break;
					}
				}
				if( cardfix != 1000 )
					damage = damage * cardfix / 1000;
			}

			if( tsd && !(nk&NK_NO_CARDFIX_DEF) ) { // Target cards.
				if( !(nk&NK_NO_ELEFIX) ) {
					int ele_fix = tsd->subele[s_ele];
					for( i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++ ) {
						if( tsd->subele2[i].ele != s_ele ) continue;
						if( !(tsd->subele2[i].flag&flag&BF_WEAPONMASK &&
							 tsd->subele2[i].flag&flag&BF_RANGEMASK &&
							 tsd->subele2[i].flag&flag&BF_SKILLMASK) )
							continue;
						ele_fix += tsd->subele2[i].rate;
					}
					cardfix=cardfix*(100-ele_fix)/100;
				}
				cardfix=cardfix*(100-tsd->subsize[sstatus->size])/100;
				cardfix=cardfix*(100-tsd->subrace2[s_race2])/100;
				cardfix=cardfix*(100-tsd->subrace[sstatus->race])/100;
				cardfix=cardfix*(100-tsd->subrace[is_boss(src)?RC_BOSS:RC_NONBOSS])/100;
				if( sstatus->race != RC_DEMIHUMAN )
					cardfix=cardfix*(100-tsd->subrace[RC_NONDEMIHUMAN])/100;

				for( i=0; i < ARRAYLENGTH(tsd->add_mdef) && tsd->add_mdef[i].rate; i++ ) {
					if( tsd->add_mdef[i].class_ == s_class ) {
						cardfix=cardfix*(100-tsd->add_mdef[i].rate)/100;
						break;
					}
				}
				//It was discovered that ranged defense also counts vs magic! [Skotlex]
				if( flag&BF_SHORT )
					cardfix = cardfix * ( 100 - tsd->bonus.near_attack_def_rate ) / 100;
				else
					cardfix = cardfix * ( 100 - tsd->bonus.long_attack_def_rate ) / 100;

				cardfix = cardfix * ( 100 - tsd->bonus.magic_def_rate ) / 100;

				if( tsd->sc.data[SC_MDEF_RATE] )
					cardfix = cardfix * ( 100 - tsd->sc.data[SC_MDEF_RATE]->val1 ) / 100;

				if( cardfix != 1000 )
					damage = damage * cardfix / 1000;
			}
			break;
		case BF_WEAPON:
			t_race2 = status_get_race2(target);
			if( sd && !(nk&NK_NO_CARDFIX_ATK) && (left&2) ) { //Attacker cards should be checked
				short cardfix_ = 1000;
				if( sd->state.arrow_atk ) {
					cardfix=cardfix*(100+sd->right_weapon.addrace[tstatus->race]+sd->arrow_addrace[tstatus->race])/100;
					if( !(nk&NK_NO_ELEFIX) ) {
						int ele_fix = sd->right_weapon.addele[tstatus->def_ele] + sd->arrow_addele[tstatus->def_ele];
						for( i = 0; ARRAYLENGTH(sd->right_weapon.addele2) > i && sd->right_weapon.addele2[i].rate != 0; i++ ) {
							if( sd->right_weapon.addele2[i].ele != tstatus->def_ele ) continue;
							if( !(sd->right_weapon.addele2[i].flag&flag&BF_WEAPONMASK &&
								 sd->right_weapon.addele2[i].flag&flag&BF_RANGEMASK &&
								 sd->right_weapon.addele2[i].flag&flag&BF_SKILLMASK) )
									continue;
							ele_fix += sd->right_weapon.addele2[i].rate;
						}
						cardfix=cardfix*(100+ele_fix)/100;
					}
					cardfix=cardfix*(100+sd->right_weapon.addsize[tstatus->size]+sd->arrow_addsize[tstatus->size])/100;
					cardfix=cardfix*(100+sd->right_weapon.addrace2[t_race2])/100;
					cardfix=cardfix*(100+sd->right_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS]+sd->arrow_addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
					if( tstatus->race != RC_DEMIHUMAN )
						cardfix=cardfix*(100+sd->right_weapon.addrace[RC_NONDEMIHUMAN]+sd->arrow_addrace[RC_NONDEMIHUMAN])/100;
				} else { // Melee attack
					if( !battle_config.left_cardfix_to_right ) {
						cardfix=cardfix*(100+sd->right_weapon.addrace[tstatus->race])/100;
						if( !(nk&NK_NO_ELEFIX) ) {
							int ele_fix = sd->right_weapon.addele[tstatus->def_ele];
							for( i = 0; ARRAYLENGTH(sd->right_weapon.addele2) > i && sd->right_weapon.addele2[i].rate != 0; i++ ) {
								if( sd->right_weapon.addele2[i].ele != tstatus->def_ele ) continue;
								if( !(sd->right_weapon.addele2[i].flag&flag&BF_WEAPONMASK &&
									 sd->right_weapon.addele2[i].flag&flag&BF_RANGEMASK &&
									 sd->right_weapon.addele2[i].flag&flag&BF_SKILLMASK) )
										continue;
								ele_fix += sd->right_weapon.addele2[i].rate;
							}
							cardfix=cardfix*(100+ele_fix)/100;
						}
						cardfix=cardfix*(100+sd->right_weapon.addsize[tstatus->size])/100;
						cardfix=cardfix*(100+sd->right_weapon.addrace2[t_race2])/100;
						cardfix=cardfix*(100+sd->right_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
						if( tstatus->race != RC_DEMIHUMAN )
							cardfix=cardfix*(100+sd->right_weapon.addrace[RC_NONDEMIHUMAN])/100;

						if( left&1 ) {
							cardfix_=cardfix_*(100+sd->left_weapon.addrace[tstatus->race])/100;
							if( !(nk&NK_NO_ELEFIX) )	{
								int ele_fix_lh = sd->left_weapon.addele[tstatus->def_ele];
								for( i = 0; ARRAYLENGTH(sd->left_weapon.addele2) > i && sd->left_weapon.addele2[i].rate != 0; i++ ) {
									if( sd->left_weapon.addele2[i].ele != tstatus->def_ele ) continue;
									if( !(sd->left_weapon.addele2[i].flag&flag&BF_WEAPONMASK &&
										 sd->left_weapon.addele2[i].flag&flag&BF_RANGEMASK &&
										 sd->left_weapon.addele2[i].flag&flag&BF_SKILLMASK) )
											continue;
									ele_fix_lh += sd->left_weapon.addele2[i].rate;
								}
								cardfix=cardfix*(100+ele_fix_lh)/100;
							}
							cardfix_=cardfix_*(100+sd->left_weapon.addsize[tstatus->size])/100;
							cardfix_=cardfix_*(100+sd->left_weapon.addrace2[t_race2])/100;
							cardfix_=cardfix_*(100+sd->left_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
							if( tstatus->race != RC_DEMIHUMAN )
								cardfix_=cardfix_*(100+sd->left_weapon.addrace[RC_NONDEMIHUMAN])/100;
						}
					} else {
						int ele_fix = sd->right_weapon.addele[tstatus->def_ele] + sd->left_weapon.addele[tstatus->def_ele];
						for( i = 0; ARRAYLENGTH(sd->right_weapon.addele2) > i && sd->right_weapon.addele2[i].rate != 0; i++ ) {
							if( sd->right_weapon.addele2[i].ele != tstatus->def_ele ) continue;
							if( !(sd->right_weapon.addele2[i].flag&flag&BF_WEAPONMASK &&
								 sd->right_weapon.addele2[i].flag&flag&BF_RANGEMASK &&
								 sd->right_weapon.addele2[i].flag&flag&BF_SKILLMASK) )
									continue;
							ele_fix += sd->right_weapon.addele2[i].rate;
						}
						for( i = 0; ARRAYLENGTH(sd->left_weapon.addele2) > i && sd->left_weapon.addele2[i].rate != 0; i++ ) {
							if( sd->left_weapon.addele2[i].ele != tstatus->def_ele ) continue;
							if( !(sd->left_weapon.addele2[i].flag&flag&BF_WEAPONMASK &&
								 sd->left_weapon.addele2[i].flag&flag&BF_RANGEMASK &&
								 sd->left_weapon.addele2[i].flag&flag&BF_SKILLMASK) )
									continue;
							ele_fix += sd->left_weapon.addele2[i].rate;
						}

						cardfix=cardfix*(100+sd->right_weapon.addrace[tstatus->race]+sd->left_weapon.addrace[tstatus->race])/100;
						cardfix=cardfix*(100+ele_fix)/100;
						cardfix=cardfix*(100+sd->right_weapon.addsize[tstatus->size]+sd->left_weapon.addsize[tstatus->size])/100;
						cardfix=cardfix*(100+sd->right_weapon.addrace2[t_race2]+sd->left_weapon.addrace2[t_race2])/100;
						cardfix=cardfix*(100+sd->right_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS]+sd->left_weapon.addrace[is_boss(target)?RC_BOSS:RC_NONBOSS])/100;
						if( tstatus->race != RC_DEMIHUMAN )
							cardfix=cardfix*(100+sd->right_weapon.addrace[RC_NONDEMIHUMAN]+sd->left_weapon.addrace[RC_NONDEMIHUMAN])/100;
					}
				}
				for( i = 0; i < ARRAYLENGTH(sd->right_weapon.add_dmg) && sd->right_weapon.add_dmg[i].rate; i++ ) {
					if( sd->right_weapon.add_dmg[i].class_ == t_class ) {
						cardfix=cardfix*(100+sd->right_weapon.add_dmg[i].rate)/100;
						break;
					}
				}

				if( left&1 ) {
					for( i = 0; i < ARRAYLENGTH(sd->left_weapon.add_dmg) && sd->left_weapon.add_dmg[i].rate; i++ ) {
						if( sd->left_weapon.add_dmg[i].class_ == t_class ) {
							cardfix_=cardfix_*(100+sd->left_weapon.add_dmg[i].rate)/100;
							break;
						}
					}
				}

				if( flag&BF_LONG )
					cardfix = cardfix * ( 100 + sd->bonus.long_attack_atk_rate ) / 100;
#ifdef RENEWAL_EDP
				if( sd->sc.data[SC_EDP] ) {
					cardfix = cardfix * (100 + sd->sc.data[SC_EDP]->val1 * 60 ) / 100;
					cardfix_ = cardfix_ * (100 + sd->sc.data[SC_EDP]->val1 * 60 ) / 100;
				}
#endif
				if( sd->sc.data[SC_ZENKAI] && sstatus->rhw.ele == sd->sc.data[SC_ZENKAI]->val2 ) {
					cardfix += sd->sc.data[SC_ZENKAI]->val1 * 20;
					cardfix_ += sd->sc.data[SC_ZENKAI]->val1 * 20;
				}

				if( (left&1) && cardfix_ != 1000 )
					damage = damage * cardfix_ / 1000;
				else if( cardfix != 1000 )
					damage = damage * cardfix / 1000;

			} else if( tsd && !(nk&NK_NO_CARDFIX_DEF) && !(left&2) ){ //Target cards should be checked
				if( !(nk&NK_NO_ELEFIX) ) {
					int ele_fix = tsd->subele[s_ele];
					for (i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++) {
						if(tsd->subele2[i].ele != s_ele) continue;
						if(!(tsd->subele2[i].flag&flag&BF_WEAPONMASK &&
							 tsd->subele2[i].flag&flag&BF_RANGEMASK &&
							 tsd->subele2[i].flag&flag&BF_SKILLMASK))
							continue;
						ele_fix += tsd->subele2[i].rate;
					}
					cardfix=cardfix*(100-ele_fix)/100;
					if( left&1 && s_ele_ != s_ele ) {
						int ele_fix_lh = tsd->subele[s_ele_];
						for (i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++) {
							if(tsd->subele2[i].ele != s_ele_) continue;
							if(!(tsd->subele2[i].flag&flag&BF_WEAPONMASK &&
								 tsd->subele2[i].flag&flag&BF_RANGEMASK &&
								 tsd->subele2[i].flag&flag&BF_SKILLMASK))
								continue;
							ele_fix_lh += tsd->subele2[i].rate;
						}
						cardfix=cardfix*(100-ele_fix_lh)/100;
					}
				}
				cardfix=cardfix*(100-tsd->subsize[sstatus->size])/100;
				cardfix=cardfix*(100-tsd->subrace2[s_race2])/100;
				cardfix=cardfix*(100-tsd->subrace[sstatus->race])/100;
				cardfix=cardfix*(100-tsd->subrace[is_boss(src)?RC_BOSS:RC_NONBOSS])/100;
				if( sstatus->race != RC_DEMIHUMAN )
					cardfix=cardfix*(100-tsd->subrace[RC_NONDEMIHUMAN])/100;

				for( i = 0; i < ARRAYLENGTH(tsd->add_def) && tsd->add_def[i].rate;i++ ) {
					if( tsd->add_def[i].class_ == s_class ) {
						cardfix=cardfix*(100-tsd->add_def[i].rate)/100;
						break;
					}
				}

				if( flag&BF_SHORT )
					cardfix = cardfix * ( 100 - tsd->bonus.near_attack_def_rate ) / 100;
				else // BF_LONG (there's no other choice)
					cardfix = cardfix * ( 100 - tsd->bonus.long_attack_def_rate ) / 100;

				if( tsd->sc.data[SC_DEF_RATE] )
					cardfix = cardfix * ( 100 - tsd->sc.data[SC_DEF_RATE]->val1 ) / 100;

				if( cardfix != 1000 )
					damage = damage * cardfix / 1000;
			}
			break;
		case BF_MISC:
			if( tsd && !(nk&NK_NO_CARDFIX_DEF) ){
			// misc damage reduction from equipment
				if (!(nk&NK_NO_ELEFIX)) {
					int ele_fix = tsd->subele[s_ele];
					for (i = 0; ARRAYLENGTH(tsd->subele2) > i && tsd->subele2[i].rate != 0; i++) {
						if(tsd->subele2[i].ele != s_ele) continue;
						if(!(tsd->subele2[i].flag&flag&BF_WEAPONMASK &&
							 tsd->subele2[i].flag&flag&BF_RANGEMASK &&
							 tsd->subele2[i].flag&flag&BF_SKILLMASK))
							continue;
						ele_fix += tsd->subele2[i].rate;
					}
					cardfix=cardfix*(100-ele_fix)/100;
				}
				cardfix=cardfix*(100-tsd->subsize[sstatus->size])/100;
				cardfix=cardfix*(100-tsd->subrace2[s_race2])/100;
				cardfix=cardfix*(100-tsd->subrace[sstatus->race])/100;
				cardfix=cardfix*(100-tsd->subrace[is_boss(src)?RC_BOSS:RC_NONBOSS])/100;
				if( sstatus->race != RC_DEMIHUMAN )
					cardfix=cardfix*(100-tsd->subrace[RC_NONDEMIHUMAN])/100;

				cardfix = cardfix * ( 100 - tsd->bonus.misc_def_rate ) / 100;
				if( flag&BF_SHORT )
					cardfix = cardfix * ( 100 - tsd->bonus.near_attack_def_rate ) / 100;
				else	// BF_LONG (there's no other choice)
					cardfix = cardfix * ( 100 - tsd->bonus.long_attack_def_rate ) / 100;

				if (cardfix != 10000)
					damage = damage * cardfix / 1000;
			}
			break;
	}

	return damage;
}

/*==========================================
 * Check dammage trough status.
 * ATK may be MISS, BLOCKED FAIL, reduc, increase, end status...
 * After this we apply bg/gvg reduction
 *------------------------------------------*/
int battle_calc_damage(struct block_list *src,struct block_list *bl,struct Damage *d,int damage,uint16 skill_id,uint16 skill_lv)
{
	struct map_session_data *sd = NULL;
	struct status_change *sc;
	struct status_change_entry *sce;
	int div_ = d->div_, flag = d->flag;

	nullpo_ret(bl);

	if( !damage )
		return 0;
	if( battle_config.ksprotection && mob_ksprotected(src, bl) )
		return 0;

	if (bl->type == BL_PC) {
		sd=(struct map_session_data *)bl;
		//Special no damage states
		if(flag&BF_WEAPON && sd->special_state.no_weapon_damage)
			damage -= damage * sd->special_state.no_weapon_damage / 100;

		if(flag&BF_MAGIC && sd->special_state.no_magic_damage)
			damage -= damage * sd->special_state.no_magic_damage / 100;

		if(flag&BF_MISC && sd->special_state.no_misc_damage)
			damage -= damage * sd->special_state.no_misc_damage / 100;

		if(!damage) return 0;
	}

	sc = status_get_sc(bl);

	if( sc && sc->data[SC_INVINCIBLE] && !sc->data[SC_INVINCIBLEOFF] )
		return 1;

	if( skill_id == PA_PRESSURE )
		return damage; //This skill bypass everything else.

	if( sc && sc->count ) {
		//First, sc_*'s that reduce damage to 0.
		if( sc->data[SC_BASILICA] && !(status_get_mode(src)&MD_BOSS) ) {
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}
		// Gravitation and Pressure do damage without removing the effect
		if( sc->data[SC_WHITEIMPRISON] && skill_id != HW_GRAVITATION ) {
			if( skill_id == MG_NAPALMBEAT ||
				skill_id == MG_SOULSTRIKE ||
				skill_id == WL_SOULEXPANSION ||
				(skill_id && skill_get_ele(skill_id, skill_lv) == ELE_GHOST) ||
				(!skill_id && (status_get_status_data(src))->rhw.ele == ELE_GHOST)
					) {
				status_change_end(bl,SC_WHITEIMPRISON,INVALID_TIMER); // Those skills do damage and removes effect
			} else {
				d->dmg_lv = ATK_BLOCK;
				return 0;
			}
		}

		if( sc->data[SC_ZEPHYR] && (((flag&(BF_SHORT|BF_LONG)) == BF_LONG) ||
			((flag&BF_MAGIC) && skill_id && !(skill_get_inf(skill_id)&INF_GROUND_SKILL))) ) {
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}

		if( sc->data[SC_SAFETYWALL] && (flag&(BF_SHORT|BF_MAGIC)) == BF_SHORT ) {
			struct skill_unit_group* group = skill_id2group(sc->data[SC_SAFETYWALL]->val3);
			if (group) {
				//in RE, SW possesses a lifetime equal to group val2, (3x caster hp, or homon formula)
#ifdef RENEWAL
				d->dmg_lv = ATK_BLOCK;
				if ( ( group->val2 - damage) > 0 ) {
					group->val2 -= damage;
				} else
					skill_delunitgroup(group);
				return 0;
#else
				if (--group->val2<=0)
					skill_delunitgroup(group);
				d->dmg_lv = ATK_BLOCK;
				return 0;
#endif
			}
			status_change_end(bl, SC_SAFETYWALL, INVALID_TIMER);
		}

		if( ( sc->data[SC_PNEUMA] && (flag&(BF_MAGIC|BF_LONG)) == BF_LONG ) || sc->data[SC__MANHOLE] ) {
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}
		if( sc->data[SC_NEUTRALBARRIER] && (flag&(BF_MAGIC|BF_LONG)) == BF_LONG && skill_id != CR_ACIDDEMONSTRATION ) {
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}
		if( sc->data[SC__MAELSTROM] && (flag&BF_MAGIC) && skill_id && (skill_get_inf(skill_id)&INF_GROUND_SKILL) ) {
			// Unofficial Absorbtion Value
			int sp = (( sc->data[SC__MAELSTROM]->val1 * 10 ) + (sd ? sd->status.job_level / 5 : 0)) / 2;
			status_heal(bl,0,sp,3);
			d->dmg_lv = ATK_BLOCK;
			return 0;
		}
		if( sc->data[SC_WEAPONBLOCKING] && flag&(BF_SHORT|BF_WEAPON) && rnd()%100 < sc->data[SC_WEAPONBLOCKING]->val2 ) {
			clif_skill_nodamage(bl,src,GC_WEAPONBLOCKING,1,1);
			d->dmg_lv = ATK_BLOCK;
			sc_start2(src,bl,SC_COMBO,100,GC_WEAPONBLOCKING,src->id,2000);
			return 0;
		}
		if( (sce = sc->data[SC_AUTOGUARD]) && flag&BF_WEAPON && !(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK) && rnd()%100 < sce->val2 ) {
			int delay;
			clif_skill_nodamage(bl,bl,CR_AUTOGUARD,sce->val1,1);
			// different delay depending on skill level [celest]
			if (sce->val1 <= 5)
				delay = 300;
			else if (sce->val1 > 5 && sce->val1 <= 9)
				delay = 200;
			else
				delay = 100;
			unit_set_walkdelay(bl, gettick(), delay, 1);

			if(sc->data[SC_SHRINK] && rnd()%100<5*sce->val1)
				skill_blown(bl,src,skill_get_blewcount(CR_SHRINK,1),-1,0);
			return 0;
		}

		if( (sce = sc->data[SC_MILLENNIUMSHIELD]) && sce->val2 > 0 && damage > 0 ) {
			clif_skill_nodamage(bl, bl, RK_MILLENNIUMSHIELD, 1, 1);
			sce->val3 -= damage; // absorb damage
			d->dmg_lv = ATK_BLOCK;
			// There is a chance to be stuned when one shield is broken.
			sc_start(src,bl,SC_STUN,15,0,skill_get_time2(RK_MILLENNIUMSHIELD,sce->val1));
			if( sce->val3 <= 0 ) { // Shield Down
				sce->val2--;
				if( sce->val2 > 0 ) {
					if( sd )
						clif_millenniumshield(sd,sce->val2);
						sce->val3 = 1000; // Next Shield
				} else
					status_change_end(bl,SC_MILLENNIUMSHIELD,INVALID_TIMER); // All shields down
			}
			return 0;
		}

		if( (sce = sc->data[SC_PARRYING]) && flag&BF_WEAPON && skill_id != WS_CARTTERMINATION && rnd()%100 < sce->val2 ) {
			// attack blocked by Parrying
			clif_skill_nodamage(bl, bl, LK_PARRYING, sce->val1,1);
			return 0;
		}
		
		if(sc->data[SC_DODGE] && ( !sc->opt1 || sc->opt1 == OPT1_BURNING ) &&
			(flag&BF_LONG || sc->data[SC_SPURT])
			&& rnd()%100 < 20) {
			if (sd && pc_issit(sd)) pc_setstand(sd); //Stand it to dodge.
			clif_skill_nodamage(bl,bl,TK_DODGE,1,1);
			if (!sc->data[SC_COMBO])
				sc_start4(src, bl, SC_COMBO, 100, TK_JUMPKICK, src->id, 1, 0, 2000);
			return 0;
		}

		if(sc->data[SC_HERMODE] && flag&BF_MAGIC)
			return 0;

		if(sc->data[SC_TATAMIGAESHI] && (flag&(BF_MAGIC|BF_LONG)) == BF_LONG)
			return 0;

		if((sce = sc->data[SC_KAUPE]) && rnd()%100 < sce->val2) {
			//Kaupe blocks damage (skill or otherwise) from players, mobs, homuns, mercenaries.
			clif_specialeffect(bl, 462, AREA);
			//Shouldn't end until Breaker's non-weapon part connects.
			if (skill_id != ASC_BREAKER || !(flag&BF_WEAPON))
				if (--(sce->val3) <= 0) //We make it work like Safety Wall, even though it only blocks 1 time.
					status_change_end(bl, SC_KAUPE, INVALID_TIMER);
			return 0;
		}
		
		if( sc->data[SC_KAITE] && (flag&BF_SHORT) ) {
			damage <<= 2; //400% damage receive
		}

		if( flag&BF_MAGIC && (sce=sc->data[SC_PRESTIGE]) && rnd()%100 < sce->val2) {
			clif_specialeffect(bl, 462, AREA); // Still need confirm it.
			return 0;
		}

		if (((sce = sc->data[SC_UTSUSEMI]) || sc->data[SC_BUNSINJYUTSU])
		&& flag&BF_WEAPON && !(skill_get_nk(skill_id)&NK_NO_CARDFIX_ATK)) {
			
			skill_additional_effect (src, bl, skill_id, skill_lv, flag, ATK_BLOCK, gettick() );
			if( !status_isdead(src) )
				skill_counter_additional_effect( src, bl, skill_id, skill_lv, flag, gettick() );
			if (sce) {
				clif_specialeffect(bl, 462, AREA);
				skill_blown(src,bl,sce->val3,-1,0);
			}
			//Both need to be consumed if they are active.
			if (sce && --(sce->val2) <= 0)
				status_change_end(bl, SC_UTSUSEMI, INVALID_TIMER);
			if ((sce=sc->data[SC_BUNSINJYUTSU]) && --(sce->val2) <= 0)
				status_change_end(bl, SC_BUNSINJYUTSU, INVALID_TIMER);
						
			return 0;
		}

		//Now damage increasing effects
		if( sc->data[SC_AETERNA] && skill_id != PF_SOULBURN ) {
			if( src->type != BL_MER || skill_id == 0 )
				damage <<= 1; // Lex Aeterna only doubles damage of regular attacks from mercenaries

			if( skill_id != ASC_BREAKER || !(flag&BF_WEAPON) )
				status_change_end(bl, SC_AETERNA, INVALID_TIMER); //Shouldn't end until Breaker's non-weapon part connects.
		}

#ifdef RENEWAL
		if( sc->data[SC_RAID] ) {
			damage += damage * 20 / 100;

			if (--sc->data[SC_RAID]->val1 == 0)
				status_change_end(bl, SC_RAID, INVALID_TIMER);
		}
#endif

		if( damage ) {
			struct map_session_data *tsd = BL_CAST(BL_PC, src);
			if( sc->data[SC_DEEPSLEEP] ) {
				damage += damage / 2; // 1.5 times more damage while in Deep Sleep.
				status_change_end(bl,SC_DEEPSLEEP,INVALID_TIMER);
			}
			if( tsd && sd && sc->data[SC_CRYSTALIZE] && flag&BF_WEAPON ) {
				switch(tsd->status.weapon) {
					case W_MACE:
					case W_2HMACE:
					case W_1HAXE:
					case W_2HAXE:
						damage += damage / 2;
						break;
					case W_MUSICAL:
					case W_WHIP:
						if(!sd->state.arrow_atk)
							break;
					case W_BOW:
					case W_REVOLVER:
					case W_RIFLE:
					case W_GATLING:
					case W_SHOTGUN:
					case W_GRENADE:
					case W_DAGGER:
					case W_1HSWORD:
					case W_2HSWORD:
						damage -= damage / 2;
						break;
				}
			}
			if( sc->data[SC_VOICEOFSIREN] )
				status_change_end(bl,SC_VOICEOFSIREN,INVALID_TIMER);
		}
#ifndef RENEWAL
		//Finally damage reductions....
		if( sc->data[SC_ASSUMPTIO] ) {
			if( map_flag_vs(bl->m) )
				damage = damage*2/3; //Receive 66% damage
			else
				damage >>= 1; //Receive 50% damage
		}
#endif

		if(sc->data[SC_DEFENDER] &&
			(flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
			damage = damage * ( 100 - sc->data[SC_DEFENDER]->val2 ) / 100;

		if(sc->data[SC_ADJUSTMENT] &&
			(flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
			damage -= damage * 20 / 100;

		if(sc->data[SC_FOGWALL] && skill_id != RK_DRAGONBREATH) {
			if(flag&BF_SKILL) //25% reduction
				damage -= damage * 25 / 100;
			else if ((flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON))
				damage >>= 2; //75% reduction
		}

		if( sc->data[SC_SMOKEPOWDER] ) {
			if( (flag&(BF_SHORT|BF_WEAPON)) == (BF_SHORT|BF_WEAPON) )
				damage -= damage * 15 / 100; //15% reduction to physical melee attacks
			else if( (flag&(BF_LONG|BF_WEAPON)) == (BF_LONG|BF_WEAPON) )
				damage -= damage * 50 / 100; //50% reduction to physical ranged attacks
		}

		// Compressed code, fixed by map.h [Epoque]
		if (src->type == BL_MOB) {
			int i;
			if (sc->data[SC_MANU_DEF])
				for (i=0;ARRAYLENGTH(mob_manuk)>i;i++)
					if (mob_manuk[i]==((TBL_MOB*)src)->class_) {
						damage -= damage * sc->data[SC_MANU_DEF]->val1 / 100;
						break;
					}
			if (sc->data[SC_SPL_DEF])
				for (i=0;ARRAYLENGTH(mob_splendide)>i;i++)
					if (mob_splendide[i]==((TBL_MOB*)src)->class_) {
						damage -= damage * sc->data[SC_SPL_DEF]->val1 / 100;
						break;
					}
		}

		if((sce=sc->data[SC_ARMOR]) && //NPC_DEFENDER
			sce->val3&flag && sce->val4&flag)
			damage -= damage * sc->data[SC_ARMOR]->val2 / 100;

#ifdef RENEWAL
		if(sc->data[SC_ENERGYCOAT] && (flag&BF_WEAPON || flag&BF_MAGIC) && skill_id != WS_CARTTERMINATION)
#else
		if(sc->data[SC_ENERGYCOAT] && flag&BF_WEAPON && skill_id != WS_CARTTERMINATION)
#endif
		{
			struct status_data *status = status_get_status_data(bl);
			int per = 100*status->sp / status->max_sp -1; //100% should be counted as the 80~99% interval
			per /=20; //Uses 20% SP intervals.
			//SP Cost: 1% + 0.5% per every 20% SP
			if (!status_charge(bl, 0, (10+5*per)*status->max_sp/1000))
				status_change_end(bl, SC_ENERGYCOAT, INVALID_TIMER);
			//Reduction: 6% + 6% every 20%
			damage -= damage * (6 * (1+per)) / 100;
		}
		if(sc->data[SC_GRANITIC_ARMOR]) {
			damage -= damage * sc->data[SC_GRANITIC_ARMOR]->val2 / 100;
		}
		if(sc->data[SC_PAIN_KILLER]) {
			damage -= sc->data[SC_PAIN_KILLER]->val3;
			damage = max(0,damage);
		}
		if((sce=sc->data[SC_MAGMA_FLOW]) && (rnd()%100 <= sce->val2) ) {
			skill_castend_damage_id(bl,src,MH_MAGMA_FLOW,sce->val1,gettick(),0);
		}
		if( (sce = sc->data[SC_STONEHARDSKIN]) && flag&BF_WEAPON && damage > 0 ) {
			sce->val2 -= damage;
			if( src->type == BL_PC ) {
				TBL_PC *ssd = BL_CAST(BL_PC, src);
				if (ssd && ssd->status.weapon != W_BOW)
					skill_break_equip(src, src, EQP_WEAPON, 3000, BCT_SELF);
			}
					skill_break_equip(src, src, EQP_WEAPON, 3000, BCT_SELF);
			// 30% chance to reduce monster's ATK by 25% for 10 seconds.
			if( src->type == BL_MOB )
				sc_start(src, src, SC_STRIPWEAPON, 30, 0, skill_get_time2(RK_STONEHARDSKIN, sce->val1));
			if( sce->val2 <= 0 )
				status_change_end(bl, SC_STONEHARDSKIN, INVALID_TIMER);
		}
/**
 * In renewal steel body reduces all incoming damage by 1/10
 **/
#ifdef RENEWAL
		if( sc->data[SC_STEELBODY] ) {
			damage = damage > 10 ? damage / 10 : 1;
		}
#endif

		//Finally added to remove the status of immobile when aimedbolt is used. [Jobbie]
		if( skill_id == RA_AIMEDBOLT && (sc->data[SC_BITE] || sc->data[SC_ANKLE] || sc->data[SC_ELECTRICSHOCKER]) ) {
			status_change_end(bl, SC_BITE, INVALID_TIMER);
			status_change_end(bl, SC_ANKLE, INVALID_TIMER);
			status_change_end(bl, SC_ELECTRICSHOCKER, INVALID_TIMER);
		}

		//Finally Kyrie because it may, or not, reduce damage to 0.
		if((sce = sc->data[SC_KYRIE]) && damage > 0){
			sce->val2-=damage;
			if(flag&BF_WEAPON || skill_id == TF_THROWSTONE){
				if(sce->val2>=0)
					damage=0;
				else
				  	damage=-sce->val2;
			}
			if((--sce->val3)<=0 || (sce->val2<=0) || skill_id == AL_HOLYLIGHT)
				status_change_end(bl, SC_KYRIE, INVALID_TIMER);
		}
		
		if( sc->data[SC_MEIKYOUSISUI] && rnd()%100 < 40 ) // custom value
			damage = 0;

		if (!damage) return 0;

		if( (sce = sc->data[SC_LIGHTNINGWALK]) && flag&BF_LONG && rnd()%100 < sce->val1 ) {
			int dx[8]={0,-1,-1,-1,0,1,1,1};
			int dy[8]={1,1,0,-1,-1,-1,0,1};
			uint8 dir = map_calc_dir(bl, src->x, src->y);
			if( unit_movepos(bl, src->x-dx[dir], src->y-dy[dir], 1, 1) ) {
				clif_slide(bl,src->x-dx[dir],src->y-dy[dir]);
				unit_setdir(bl, dir);
			}
			d->dmg_lv = ATK_DEF;
			status_change_end(bl, SC_LIGHTNINGWALK, INVALID_TIMER);
			return 0;
		}

		//Probably not the most correct place, but it'll do here
		//(since battle_drain is strictly for players currently)
		if( (sce = sc->data[SC_BLOODLUST]) && flag&BF_WEAPON && damage > 0 &&
			rnd()%100 < sce->val3 )
			status_heal(src, damage*sce->val4/100, 0, 3);

		if( sd && (sce = sc->data[SC_FORCEOFVANGUARD]) && flag&BF_WEAPON && rnd()%100 < sce->val2 )
			pc_addspiritball(sd, skill_get_time(LG_FORCEOFVANGUARD, sce->val1), sce->val3);

		if( sd && (sce = sc->data[SC_GT_ENERGYGAIN]) && flag&BF_WEAPON && rnd()%100 < sce->val3 ) {
			int spheremax = 0;
			if ( sc->data[SC_RAISINGDRAGON] )
				spheremax = 5 + sc->data[SC_RAISINGDRAGON]->val1;
			else
				spheremax = 5;
			pc_addspiritball(sd, skill_get_time2(SR_GENTLETOUCH_ENERGYGAIN, sce->val1), spheremax);
		}

		if( sc->data[SC_STYLE_CHANGE] ) {
			TBL_HOM *hd = BL_CAST(BL_HOM,bl); //when being hit
			if ( hd && (rnd()%100<(status_get_lv(bl)/2)) ) hom_addspiritball(hd, 10); //add a sphere
		}

		if( sc->data[SC__DEADLYINFECT] && flag&BF_SHORT && damage > 0 && rnd()%100 < 30 + 10 * sc->data[SC__DEADLYINFECT]->val1 )
			status_change_spread(bl, src); // Deadly infect attacked side

		if( sc && sc->data[SC__SHADOWFORM] ) {
			struct block_list *s_bl = map_id2bl(sc->data[SC__SHADOWFORM]->val2);
			if( !s_bl || s_bl->m != bl->m ) { // If the shadow form target is not present remove the sc.
				status_change_end(bl, SC__SHADOWFORM, INVALID_TIMER);
			} else if( status_isdead(s_bl) || !battle_check_target(src,s_bl,BCT_ENEMY)) { // If the shadow form target is dead or not your enemy remove the sc in both.
				status_change_end(bl, SC__SHADOWFORM, INVALID_TIMER);
				if( s_bl->type == BL_PC )
					((TBL_PC*)s_bl)->shadowform_id = 0;
			} else {
				if( (--sc->data[SC__SHADOWFORM]->val3) < 0 ) { // If you have exceded max hits supported, remove the sc in both.
					status_change_end(bl, SC__SHADOWFORM, INVALID_TIMER);
					if( s_bl->type == BL_PC )
						((TBL_PC*)s_bl)->shadowform_id = 0;
				} else {
					status_damage(bl, s_bl, damage, 0, clif_damage(s_bl, s_bl, gettick(), 500, 500, damage, -1, 0, 0), 0);
					return ATK_NONE;
				}
			}
		}

	}

	//SC effects from caster side.
	sc = status_get_sc(src);

	if( sc && sc->count ) {
		if( sc->data[SC_INVINCIBLE] && !sc->data[SC_INVINCIBLEOFF] )
			damage += damage * 75 / 100;
		// [Epoque]
		if (bl->type == BL_MOB) {
			int i;

			if( ((sce=sc->data[SC_MANU_ATK]) && (flag&BF_WEAPON)) ||
				 ((sce=sc->data[SC_MANU_MATK]) && (flag&BF_MAGIC))
				)
				for( i=0;ARRAYLENGTH(mob_manuk)>i;i++ )
					if( ((TBL_MOB*)bl)->class_==mob_manuk[i] ) {
						damage += damage * sce->val1 / 100;
						break;
					}
			if( ((sce=sc->data[SC_SPL_ATK]) && (flag&BF_WEAPON)) ||
				 ((sce=sc->data[SC_SPL_MATK]) && (flag&BF_MAGIC))
				)
				for( i=0;ARRAYLENGTH(mob_splendide)>i;i++ )
					if( ((TBL_MOB*)bl)->class_==mob_splendide[i] ) {
						damage += damage * sce->val1 / 100;
						break;
					}
		}
		if( sc->data[SC_POISONINGWEAPON] && skill_id != GC_VENOMPRESSURE && (flag&BF_WEAPON) && damage > 0 && rnd()%100 < sc->data[SC_POISONINGWEAPON]->val3 )
			sc_start(src,bl,sc->data[SC_POISONINGWEAPON]->val2,100,sc->data[SC_POISONINGWEAPON]->val1,skill_get_time2(GC_POISONINGWEAPON, 1));
		if( sc->data[SC__DEADLYINFECT] && flag&BF_SHORT && damage > 0 && rnd()%100 < 30 + 10 * sc->data[SC__DEADLYINFECT]->val1 )
			status_change_spread(src, bl);
		if( sc->data[SC_STYLE_CHANGE] ) {
			TBL_HOM *hd = BL_CAST(BL_HOM,src); //when attacking
			if ( hd && (rnd()%100<(20+status_get_lv(bl)/5)) ) hom_addspiritball(hd, 10);
		}
	}

	if (battle_config.pk_mode && sd && bl->type == BL_PC && damage && map[bl->m].flag.pvp) {
		if (flag & BF_SKILL) { //Skills get a different reduction than non-skills. [Skotlex]
			if (flag&BF_WEAPON)
				damage = damage * battle_config.pk_weapon_damage_rate / 100;
			if (flag&BF_MAGIC)
				damage = damage * battle_config.pk_magic_damage_rate / 100;
			if (flag&BF_MISC)
				damage = damage * battle_config.pk_misc_damage_rate / 100;
		} else { //Normal attacks get reductions based on range.
			if (flag & BF_SHORT)
				damage = damage * battle_config.pk_short_damage_rate / 100;
			if (flag & BF_LONG)
				damage = damage * battle_config.pk_long_damage_rate / 100;
		}
		if (!damage) damage  = 1;
	}

	if (battle_config.skill_min_damage && damage > 0 && damage < div_) {
		if ((flag&BF_WEAPON && battle_config.skill_min_damage&1)
			|| (flag&BF_MAGIC && battle_config.skill_min_damage&2)
			|| (flag&BF_MISC && battle_config.skill_min_damage&4)
		)
			damage = div_;
	}

	if (bl->type == BL_MOB && !status_isdead(bl) && src != bl) {
	  if (damage > 0)
			mobskill_event((TBL_MOB*)bl,src,gettick(),flag);
	  if (skill_id)
			mobskill_event((TBL_MOB*)bl,src,gettick(),MSC_SKILLUSED|(skill_id<<16));
	}
	if( sd ) {
		if( pc_ismadogear(sd) && rnd()%100 < 50 ) {
			short element = skill_get_ele(skill_id, skill_lv);
			if( !skill_id || element == -1 ) { //Take weapon's element
				struct status_data *sstatus = NULL;
				if( src->type == BL_PC && ((TBL_PC*)src)->bonus.arrow_ele )
					element = ((TBL_PC*)src)->bonus.arrow_ele;
				else if( (sstatus = status_get_status_data(src)) ) {
					element = sstatus->rhw.ele;
				}
			}
			else if( element == -2 ) //Use enchantment's element
				element = status_get_attack_sc_element(src,status_get_sc(src));
			else if( element == -3 ) //Use random element
				element = rnd()%ELE_MAX;
			if( element == ELE_FIRE || element == ELE_WATER )
				pc_overheat(sd,element == ELE_FIRE ? 1 : -1);
		}
	}

	return damage;
}

/*==========================================
 * Calculates BG related damage adjustments.
 *------------------------------------------*/
int battle_calc_bg_damage(struct block_list *src, struct block_list *bl, int damage, int div_, uint16 skill_id, uint16 skill_lv, int flag)
{
	if( !damage )
		return 0;

	if( bl->type == BL_MOB ) {
		struct mob_data* md = BL_CAST(BL_MOB, bl);
		if( map[bl->m].flag.battleground && (md->class_ == MOBID_BLUE_CRYST || md->class_ == MOBID_PINK_CRYST) && flag&BF_SKILL )
			return 0; // Crystal cannot receive skill damage on battlegrounds
	}

	switch( skill_id ) {
		//Skills with no damage reduction.
		case PA_PRESSURE:
		case HW_GRAVITATION:
		case NJ_ZENYNAGE:
			break;
		default:
			if( flag&BF_SKILL ) { //Skills get a different reduction than non-skills. [Skotlex]
				if( flag&BF_WEAPON )
					damage = damage * battle_config.bg_weapon_damage_rate / 100;
				if( flag&BF_MAGIC )
					damage = damage * battle_config.bg_magic_damage_rate / 100;
				if(	flag&BF_MISC )
					damage = damage * battle_config.bg_misc_damage_rate / 100;
			} else { //Normal attacks get reductions based on range.
				if( flag&BF_SHORT )
					damage = damage * battle_config.bg_short_damage_rate / 100;
				if( flag&BF_LONG )
					damage = damage * battle_config.bg_long_damage_rate / 100;
			}

			if( !damage )
				damage = 1;
	}

	return damage;
}

/*==========================================
 * Calculates GVG related damage adjustments.
 *------------------------------------------*/
int battle_calc_gvg_damage(struct block_list *src,struct block_list *bl,int damage,int div_,uint16 skill_id,uint16 skill_lv,int flag)
{
	struct mob_data* md = BL_CAST(BL_MOB, bl);
	int class_ = status_get_class(bl);

	if (!damage) //No reductions to make.
		return 0;

	if(md && md->guardian_data) {
		if(class_ == MOBID_EMPERIUM && flag&BF_SKILL) {
			//Skill immunity.
			switch (skill_id) {
#ifndef RENEWAL
				case MO_TRIPLEATTACK:
#endif
				case HW_GRAVITATION:
				case KO_MAKIBISHI:
					break;
				default:
					return 0;
			}
		}
		if(src->type != BL_MOB) {
			struct guild *g = src->type == BL_PC ? ((TBL_PC *)src)->guild : guild_search(status_get_guild_id(src));
			if (class_ == MOBID_EMPERIUM && (!g || guild_checkskill(g,GD_APPROVAL) <= 0 ))
				return 0;
				
			if (g && battle_config.guild_max_castles && guild_checkcastles(g)>=battle_config.guild_max_castles)
				return 0; // [MouseJstr]
		}
	}

	switch (skill_id) {
		//Skills with no damage reduction.
		case PA_PRESSURE:
		case HW_GRAVITATION:
		case NJ_ZENYNAGE:
			break;
		default:
			/* Uncomment if you want god-mode Emperiums at 100 defense. [Kisuka]
			if (md && md->guardian_data) {
				damage -= damage * (md->guardian_data->castle->defense/100) * battle_config.castle_defense_rate/100;
			}
			*/
			if (flag & BF_SKILL) { //Skills get a different reduction than non-skills. [Skotlex]
				if (flag&BF_WEAPON)
					damage = damage * battle_config.gvg_weapon_damage_rate / 100;
				if (flag&BF_MAGIC)
					damage = damage * battle_config.gvg_magic_damage_rate / 100;
				if (flag&BF_MISC)
					damage = damage * battle_config.gvg_misc_damage_rate / 100;
			} else { //Normal attacks get reductions based on range.
				if (flag & BF_SHORT)
					damage = damage * battle_config.gvg_short_damage_rate / 100;
				if (flag & BF_LONG)
					damage = damage * battle_config.gvg_long_damage_rate / 100;
			}

			if(!damage)
				damage = 1;
	}
	return damage;
}

/*==========================================
 * HP/SP drain calculation
 *------------------------------------------*/
static int battle_calc_drain(int damage, int rate, int per)
{
	int diff = 0;

	if (per && rnd()%1000 < rate) {
		diff = (damage * per) / 100;
		if (diff == 0) {
			if (per > 0)
				diff = 1;
			else
				diff = -1;
		}
	}
	return diff;
}

/*==========================================
 * Passif skill dammages increases
 *------------------------------------------*/
int battle_addmastery(struct map_session_data *sd,struct block_list *target,int dmg,int type)
{
	int damage,skill;
	struct status_data *status = status_get_status_data(target);
	int weapon;
	damage = dmg;

	nullpo_ret(sd);

	if((skill = pc_checkskill(sd,AL_DEMONBANE)) > 0 &&
		target->type == BL_MOB && //This bonus doesnt work against players.
		(battle_check_undead(status->race,status->def_ele) || status->race==RC_DEMON) )
		damage += (skill*(int)(3+(sd->status.base_level+1)*0.05));	// submitted by orn
		//damage += (skill * 3);
	if( (skill = pc_checkskill(sd, RA_RANGERMAIN)) > 0 && (status->race == RC_BRUTE || status->race == RC_PLANT || status->race == RC_FISH) )
		damage += (skill * 5);
	if( (skill = pc_checkskill(sd,NC_RESEARCHFE)) > 0 && (status->def_ele == ELE_FIRE || status->def_ele == ELE_EARTH) )
		damage += (skill * 10);
	if( pc_ismadogear(sd) )
		damage += 15 * pc_checkskill(sd, NC_MADOLICENCE);

	if((skill = pc_checkskill(sd,HT_BEASTBANE)) > 0 && (status->race==RC_BRUTE || status->race==RC_INSECT) ) {
		damage += (skill * 4);
		if (sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_HUNTER)
			damage += sd->status.str;
	}

	if(type == 0)
		weapon = sd->weapontype1;
	else
		weapon = sd->weapontype2;
	switch(weapon)
	{
		case W_1HSWORD:
#ifdef RENEWAL
				if((skill = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
					damage += (skill * 3);
#endif
		case W_DAGGER:
			if((skill = pc_checkskill(sd,SM_SWORD)) > 0)
				damage += (skill * 4);
			if((skill = pc_checkskill(sd,GN_TRAINING_SWORD)) > 0)
				damage += skill * 10;
			break;
		case W_2HSWORD:
#ifdef RENEWAL
				if((skill = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
					damage += (skill * 3);
#endif
			if((skill = pc_checkskill(sd,SM_TWOHAND)) > 0)
				damage += (skill * 4);
			break;
		case W_1HSPEAR:
		case W_2HSPEAR:
			if((skill = pc_checkskill(sd,KN_SPEARMASTERY)) > 0) {
				if(!pc_isriding(sd) || !pc_isridingdragon(sd))
					damage += (skill * 4);
				else
					damage += (skill * 5);
				// Increase damage by level of KN_SPEARMASTERY * 10
				if(pc_checkskill(sd,RK_DRAGONTRAINING) > 0)
					damage += (skill * 10);
			}
			break;
		case W_1HAXE:
		case W_2HAXE:
			if((skill = pc_checkskill(sd,AM_AXEMASTERY)) > 0)
				damage += (skill * 3);
			if((skill = pc_checkskill(sd,NC_TRAININGAXE)) > 0)
				damage += (skill * 5);
			break;
		case W_MACE:
		case W_2HMACE:
			if((skill = pc_checkskill(sd,PR_MACEMASTERY)) > 0)
				damage += (skill * 3);
			if((skill = pc_checkskill(sd,NC_TRAININGAXE)) > 0)
				damage += (skill * 4);
			break;
		case W_FIST:
			if((skill = pc_checkskill(sd,TK_RUN)) > 0)
				damage += (skill * 10);
			// No break, fallthrough to Knuckles
		case W_KNUCKLE:
			if((skill = pc_checkskill(sd,MO_IRONHAND)) > 0)
				damage += (skill * 3);
			break;
		case W_MUSICAL:
			if((skill = pc_checkskill(sd,BA_MUSICALLESSON)) > 0)
				damage += (skill * 3);
			break;
		case W_WHIP:
			if((skill = pc_checkskill(sd,DC_DANCINGLESSON)) > 0)
				damage += (skill * 3);
			break;
		case W_BOOK:
			if((skill = pc_checkskill(sd,SA_ADVANCEDBOOK)) > 0)
				damage += (skill * 3);
			break;
		case W_KATAR:
			if((skill = pc_checkskill(sd,AS_KATAR)) > 0)
				damage += (skill * 3);
			break;
	}

	return damage;
}
/*==========================================
 * Calculates the standard damage of a normal attack assuming it hits,
 * it calculates nothing extra fancy, is needed for magnum break's WATK_ELEMENT bonus. [Skotlex]
 *------------------------------------------
 * Pass damage2 as NULL to not calc it.
 * Flag values:
 * &1: Critical hit
 * &2: Arrow attack
 * &4: Skill is Magic Crasher
 * &8: Skip target size adjustment (Extremity Fist?)
 *&16: Arrow attack but BOW, REVOLVER, RIFLE, SHOTGUN, GATLING or GRENADE type weapon not equipped (i.e. shuriken, kunai and venom knives not affected by DEX)
 */
static int battle_calc_base_damage(struct status_data *status, struct weapon_atk *wa, struct status_change *sc, unsigned short t_size, struct map_session_data *sd, int flag)
{
	unsigned int atkmin=0, atkmax=0;
	short type = 0;
	int damage = 0;

	if (!sd) { //Mobs/Pets
		if(flag&4) {
			atkmin = status->matk_min;
			atkmax = status->matk_max;
		} else {
			atkmin = wa->atk;
			atkmax = wa->atk2;
		}
		if (atkmin > atkmax)
			atkmin = atkmax;
	} else { //PCs
		atkmax = wa->atk;
		type = (wa == &status->lhw)?EQI_HAND_L:EQI_HAND_R;

		if (!(flag&1) || (flag&2)) { //Normal attacks
			atkmin = status->dex;
			
			if (sd->equip_index[type] >= 0 && sd->inventory_data[sd->equip_index[type]])
				atkmin = atkmin*(80 + sd->inventory_data[sd->equip_index[type]]->wlv*20)/100;

			if (atkmin > atkmax)
				atkmin = atkmax;
			
			if(flag&2 && !(flag&16)) { //Bows
				atkmin = atkmin*atkmax/100;
				if (atkmin > atkmax)
					atkmax = atkmin;
			}
		}
	}

	if (sc && sc->data[SC_MAXIMIZEPOWER])
		atkmin = atkmax;

	//Weapon Damage calculation
	if (!(flag&1))
		damage = (atkmax>atkmin? rnd()%(atkmax-atkmin):0)+atkmin;
	else
		damage = atkmax;

	if (sd) {
		//rodatazone says the range is 0~arrow_atk-1 for non crit
		if (flag&2 && sd->bonus.arrow_atk)
			damage += ( (flag&1) ? sd->bonus.arrow_atk : rnd()%sd->bonus.arrow_atk );

		//SizeFix only for players
		if (!(sd->special_state.no_sizefix || (flag&8)))
			damage = damage * ( type == EQI_HAND_L ? sd->left_weapon.atkmods[t_size] : sd->right_weapon.atkmods[t_size] ) / 100;
	}

	//Finally, add baseatk
	if(flag&4)
		damage += status->matk_min;
	else
		damage += status->batk;

	//rodatazone says that Overrefine bonuses are part of baseatk
	//Here we also apply the weapon_atk_rate bonus so it is correctly applied on left/right hands.
	if(sd) {
		if (type == EQI_HAND_L) {
			if(sd->left_weapon.overrefine)
				damage += rnd()%sd->left_weapon.overrefine+1;
			if (sd->weapon_atk_rate[sd->weapontype2])
				damage += damage * sd->weapon_atk_rate[sd->weapontype2] / 100;
		} else { //Right hand
			if(sd->right_weapon.overrefine)
				damage += rnd()%sd->right_weapon.overrefine+1;
			if (sd->weapon_atk_rate[sd->weapontype1])
				damage += damage * sd->weapon_atk_rate[sd->weapontype1] / 100;
		}
	}
	return damage;
}

/*==========================================
 * Consumes ammo for the given skill.
 *------------------------------------------*/
void battle_consume_ammo(TBL_PC*sd, int skill, int lv)
{
	int qty=1;
	if (!battle_config.arrow_decrement)
		return;
	
	if (skill) {
		qty = skill_get_ammo_qty(skill, lv);
		if (!qty) qty = 1;
	}

	if(sd->equip_index[EQI_AMMO]>=0) //Qty check should have been done in skill_check_condition
		pc_delitem(sd,sd->equip_index[EQI_AMMO],qty,0,1,LOG_TYPE_CONSUME);

	sd->state.arrow_atk = 0;
}

static int battle_range_type(
	struct block_list *src, struct block_list *target,
	uint16 skill_id, uint16 skill_lv)
{	//Skill Range Criteria
	if (battle_config.skillrange_by_distance &&
		(src->type&battle_config.skillrange_by_distance)
	) { //based on distance between src/target [Skotlex]
		if (check_distance_bl(src, target, 5))
			return BF_SHORT;
		return BF_LONG;
	}
	//based on used skill's range
	if (skill_get_range2(src, skill_id, skill_lv) < 5)
		return BF_SHORT;
	return BF_LONG;
}
static inline int battle_adjust_skill_damage(int m, unsigned short skill_id) {

	if( map[m].skill_count ) {
		int i;
		ARR_FIND(0, map[m].skill_count, i, map[m].skills[i]->skill_id == skill_id );

		if( i < map[m].skill_count ) {
			return map[m].skills[i]->modifier;
		}
	}

	return 0;
}

static int battle_blewcount_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i;
	if (!sd->skillblown[0].id)
		return 0;
	//Apply the bonus blewcount. [Skotlex]
	for (i = 0; i < ARRAYLENGTH(sd->skillblown) && sd->skillblown[i].id; i++) {
		if (sd->skillblown[i].id == skill_id)
			return sd->skillblown[i].val;
	}
	return 0;
}

struct Damage battle_calc_magic_attack(struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv,int mflag);
struct Damage battle_calc_misc_attack(struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv,int mflag);

//For quick div adjustment.
#define damage_div_fix(dmg, div) { if (div > 1) (dmg)*=div; else if (div < 0) (div)*=-1; }
/*==========================================
 * battle_calc_weapon_attack (by Skotlex)
 *------------------------------------------*/
static struct Damage battle_calc_weapon_attack(struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv,int wflag)
{
	unsigned int skillratio = 100;	//Skill dmg modifiers.
	short skill = 0;
	short s_ele, s_ele_, t_class;
	int i, nk;
	bool n_ele = false; // non-elemental
	int chorusbonus = 0; //Chorus bonus value for chorus skills. Bonus remains 0 unless 3 or more Minstrel's/Wanderer's are in the party.

	struct map_session_data *sd, *tsd;
	struct Damage wd;
	struct status_change *sc = status_get_sc(src);
	struct status_change *tsc = status_get_sc(target);
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct {
		unsigned hit : 1; //the attack Hit? (not a miss)
		unsigned cri : 1;		//Critical hit
		unsigned idef : 1;	//Ignore defense
		unsigned idef2 : 1;	//Ignore defense (left weapon)
		unsigned pdef : 2;	//Pierces defense (Investigate/Ice Pick)
		unsigned pdef2 : 2;	//1: Use def+def2/100, 2: Use def+def2/50
		unsigned infdef : 1;	//Infinite defense (plants)
		unsigned arrow : 1;	//Attack is arrow-based
		unsigned rh : 1;		//Attack considers right hand (wd.damage)
		unsigned lh : 1;		//Attack considers left hand (wd.damage2)
		unsigned weapon : 1; //It's a weapon attack (consider VVS, and all that)
	}	flag;

	memset(&wd,0,sizeof(wd));
	memset(&flag,0,sizeof(flag));

	if(src == NULL || target == NULL) {
		nullpo_info(NLP_MARK);
		return wd;
	}
	//Initial flag
	flag.rh = 1;
	flag.weapon = 1;
	flag.infdef = (tstatus->mode&MD_PLANT && skill_id != RA_CLUSTERBOMB
#ifdef RENEWAL
		&& skill_id != HT_FREEZINGTRAP
#endif
		?1:0);
	if (target->type == BL_SKILL) {
		TBL_SKILL *su = (TBL_SKILL*)target;
		if( su->group && (su->group->skill_id == WM_REVERBERATION || su->group->skill_id == WM_POEMOFNETHERWORLD) )
			flag.infdef = 1;
 	}

	//Initial Values
	wd.type = 0; //Normal attack
	wd.div_ = skill_id?skill_get_num(skill_id,skill_lv):1;
	wd.amotion = (skill_id && skill_get_inf(skill_id)&INF_GROUND_SKILL)?0:sstatus->amotion; //Amotion should be 0 for ground skills.
	if(skill_id == KN_AUTOCOUNTER)
		wd.amotion >>= 1;
	wd.dmotion = tstatus->dmotion;
	wd.blewcount = skill_get_blewcount(skill_id,skill_lv);
	wd.flag = BF_WEAPON; //Initial Flag
	wd.flag |= (skill_id||wflag)?BF_SKILL:BF_NORMAL; // Baphomet card's splash damage is counted as a skill. [Inkfish]
	wd.dmg_lv = ATK_DEF;	//This assumption simplifies the assignation later
	nk = skill_get_nk(skill_id);
	if( !skill_id && wflag ) //If flag, this is splash damage from Baphomet Card and it always hits.
		nk |= NK_NO_CARDFIX_ATK|NK_IGNORE_FLEE;
	flag.hit = nk&NK_IGNORE_FLEE?1:0;
	flag.idef = flag.idef2 = nk&NK_IGNORE_DEF?1:0;

	if (sc && !sc->count)
		sc = NULL; //Skip checking as there are no status changes active.
	if (tsc && !tsc->count)
		tsc = NULL; //Skip checking as there are no status changes active.

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);
	
	// Minstrel/Wanderer number check for chorus skills.
	// Bonus remains 0 unless 3 or more Minstrel's/Wanderer's are in the party.
	if( sd && sd->status.party_id && party_foreachsamemap(party_sub_count_chorus, sd, 0) > 7)
		chorusbonus = 5;//Maximum effect possiable from 7 or more Minstrel's/Wanderer's
	else if( sd && sd->status.party_id && party_foreachsamemap(party_sub_count_chorus, sd, 0) > 2)
		chorusbonus = party_foreachsamemap(party_sub_count_chorus, sd, 0) - 2;//Effect bonus from additional Minstrel's/Wanderer's if not above the max possiable.

	if(sd)
		wd.blewcount += battle_blewcount_bonus(sd, skill_id);

	//Set miscellaneous data that needs be filled regardless of hit/miss
	if(
		(sd && sd->state.arrow_atk) ||
		(!sd && ((skill_id && skill_get_ammotype(skill_id)) || sstatus->rhw.range>3))
	)
		flag.arrow = 1;
	
	if(skill_id) {
		wd.flag |= battle_range_type(src, target, skill_id, skill_lv);
		switch(skill_id) {
			case MH_SONIC_CRAW: {
					TBL_HOM *hd = BL_CAST(BL_HOM,src);
					wd.div_ = hd->homunculus.spiritball;
				}
				break;
			case MO_FINGEROFFENSIVE:
				if(sd) {
					if (battle_config.finger_offensive_type)
						wd.div_ = 1;
					else
						wd.div_ = sd->spiritball_old;
				}
				break;
			case HT_PHANTASMIC:
				//Since these do not consume ammo, they need to be explicitly set as arrow attacks.
				flag.arrow = 1;
				break;
#ifndef RENEWAL
			case PA_SHIELDCHAIN:
			case CR_SHIELDBOOMERANG:
#endif
			case LG_SHIELDPRESS:
			case LG_EARTHDRIVE:
				flag.weapon = 0;
				break;

			case KN_PIERCE:
			case ML_PIERCE:
				wd.div_= (wd.div_>0?tstatus->size+1:-(tstatus->size+1));
				break;

			case TF_DOUBLE: //For NPC used skill.
			case GS_CHAINACTION:
				wd.type = 0x08;
				break;
				
			case GS_GROUNDDRIFT:
			case KN_SPEARSTAB:
			case KN_BOWLINGBASH:
			case MS_BOWLINGBASH:
			case MO_BALKYOUNG:
			case TK_TURNKICK:
				wd.blewcount = 0;
				break;

			case KN_AUTOCOUNTER:
				wd.flag = (wd.flag&~BF_SKILLMASK)|BF_NORMAL;
				break;

			case NPC_CRITICALSLASH:
			case LG_PINPOINTATTACK:
				flag.cri = 1; //Always critical skill.
				break;

			case LK_SPIRALPIERCE:
				if (!sd) wd.flag=(wd.flag&~(BF_RANGEMASK|BF_WEAPONMASK))|BF_LONG|BF_MISC;
				break;
		}
	} else //Range for normal attacks.
		wd.flag |= flag.arrow?BF_LONG:BF_SHORT;
	
	if ( (!skill_id || skill_id == PA_SACRIFICE) && tstatus->flee2 && rnd()%1000 < tstatus->flee2 ) { //Check for Lucky Dodge
		wd.type = 0x0b;
		wd.dmg_lv = ATK_LUCKY;
		if (wd.div_ < 0) wd.div_ *= -1;
		return wd;
	}

	t_class = status_get_class(target);
	s_ele = s_ele_ = skill_get_ele(skill_id, skill_lv);
	if( !skill_id || s_ele == -1 ) { //Take weapon's element
		s_ele = sstatus->rhw.ele;
		s_ele_ = sstatus->lhw.ele;
		if( sd ){ //Summoning 10 talisman will endow your weapon.
			ARR_FIND(1, 6, i, sd->talisman[i] >= 10);
			if( i < 5 ) s_ele = s_ele_ = i;
		}
		if( flag.arrow && sd && sd->bonus.arrow_ele )
			s_ele = sd->bonus.arrow_ele;
		if( battle_config.attack_attr_none&src->type )
			n_ele = true; //Weapon's element is "not elemental"
	}
	else if( s_ele == -2 ) //Use enchantment's element
		s_ele = s_ele_ = status_get_attack_sc_element(src,sc);
	else if( s_ele == -3 ) //Use random element
		s_ele = s_ele_ = rnd()%ELE_MAX;
	switch( skill_id ) {
		case GS_GROUNDDRIFT:
			s_ele = s_ele_ = wflag; //element comes in flag.
			break;
		case LK_SPIRALPIERCE:
			if (!sd) n_ele = false; //forced neutral for monsters
			break;
	}
	
	if (!(nk & NK_NO_ELEFIX) && !n_ele)
		if (src->type == BL_HOM)
		n_ele = true; //skill is "not elemental"
	if (sc && sc->data[SC_GOLDENE_FERSE] && ((!skill_id && (rnd() % 100 < sc->data[SC_GOLDENE_FERSE]->val4)) || skill_id == MH_STAHL_HORN)) {
		s_ele = s_ele_ = ELE_HOLY;
		n_ele = false;
	}

	if(!skill_id) { //Skills ALWAYS use ONLY your right-hand weapon (tested on Aegis 10.2)
		if (sd && sd->weapontype1 == 0 && sd->weapontype2 > 0) {
			flag.rh = 0;
			flag.lh = 1;
		}
		if (sstatus->lhw.atk)
			flag.lh = 1;
	}

	//Check for double attack.
	if( sd && !skill_id ) {
		short dachance = 0;//Success chance of double attacking. If player is in fear breeze status and generated number is within fear breeze's range, this will be ignored.
		short hitnumber = 0;//Used for setting how many hits will hit.
		short gendetect[] = { 12, 12, 21, 27, 30 };//If generated number is outside this value while in fear breeze status, it will check if their's a chance for double attacking.
		short generate = rnd()%100 + 1;//Generates a random number between 1 - 100 which is then used to determine if fear breeze or double attacking will happen.

		// First we go through a number of checks to see if their's any chance of double attacking a target. Only the highest success chance is taken.
		if ( sd->bonus.double_rate > 0 && sd->weapontype1 != W_FIST )
			dachance = sd->bonus.double_rate;

		if ( sc && sc->data[SC_KAGEMUSYA] && sc->data[SC_KAGEMUSYA]->val3 > dachance && sd->weapontype1 != W_FIST )
			dachance = sc->data[SC_KAGEMUSYA]->val3;

		if ( 5 * pc_checkskill(sd,TF_DOUBLE) > dachance && sd->weapontype1 == W_DAGGER )
			dachance = 5 * pc_checkskill(sd,TF_DOUBLE);

		if ( 5 * pc_checkskill(sd,GS_CHAINACTION) > dachance && sd->weapontype1 == W_REVOLVER )
			dachance = 5 * pc_checkskill(sd,GS_CHAINACTION);

		// This checks if the generated value is within fear breeze's success chance range for the level used as set by gendetect.
		if ( sc && sc->data[SC_FEARBREEZE] && generate <= gendetect[sc->data[SC_FEARBREEZE]->val1 - 1] && sd->weapontype1 == W_BOW ) {
				if ( generate >= 1 && generate <= 12 )//12% chance to deal 2 hits.
					hitnumber = 2;
				else if ( generate >= 13 && generate <= 21 )//9% chance to deal 3 hits.
					hitnumber = 3;
				else if ( generate >= 22 && generate <= 27 )//6% chance to deal 4 hits.
					hitnumber = 4;
				else if ( generate >= 28 && generate <= 30 )//3% chance to deal 5 hits.
					hitnumber = 5;
		}
		// If the generated value is higher then Fear Breeze's success chance range, but not higher then the player's double attack success chance,
		// then allow a double attack to happen.
		else if ( generate < dachance )
			hitnumber = 2;

		if ( hitnumber > 1 ) {//Needed to allow critical attacks to hit when not hitting more then once.
			wd.div_ = hitnumber;
			wd.type = 0x08;
		}
	}

	//Check for critical
	if( !flag.cri && !(wd.type&0x08) && sstatus->cri &&
		(!skill_id ||
		skill_id == KN_AUTOCOUNTER ||
		skill_id == SN_SHARPSHOOTING || skill_id == MA_SHARPSHOOTING ||
		skill_id == NJ_KIRIKAGE))
	{
		short cri = sstatus->cri;
		if (sd) {
			cri+= sd->critaddrace[tstatus->race];
			if(flag.arrow)
				cri += sd->bonus.arrow_cri;
		}
		if( sc && sc->data[SC_CAMOUFLAGE] )
			cri += 10 * (10-sc->data[SC_CAMOUFLAGE]->val4);

		//The official equation is *2, but that only applies when sd's do critical.
		//Therefore, we use the old value 3 on cases when an sd gets attacked by a mob
		cri -= tstatus->luk*(!sd&&tsd?3:2);

		if( tsc && tsc->data[SC_SLEEP] ) {
			cri <<= 1;
		}
		switch (skill_id) {
			case KN_AUTOCOUNTER:
				if(battle_config.auto_counter_type &&
					(battle_config.auto_counter_type&src->type))
					flag.cri = 1;
				else
					cri <<= 1;
				break;
			case SN_SHARPSHOOTING:
			case MA_SHARPSHOOTING:
				cri += 200;
				break;
			case NJ_KIRIKAGE:
				cri += 250 + 50*skill_lv;
				break;
		}
		if(tsd && tsd->bonus.critical_def)
			cri = cri * ( 100 - tsd->bonus.critical_def ) / 100;
		if (rnd()%1000 < cri)
			flag.cri = 1;
	}
	if (flag.cri) {
		wd.type = 0x0a;
#ifdef RENEWAL
		flag.hit = 1;
#else
		flag.idef = flag.idef2 = flag.hit = 1;
#endif
	} else {	//Check for Perfect Hit
		if(sd && sd->bonus.perfect_hit > 0 && rnd()%100 < sd->bonus.perfect_hit)
			flag.hit = 1;
		if (sc && sc->data[SC_FUSION]) {
			flag.hit = 1; //SG_FUSION always hit [Komurka]
			flag.idef = flag.idef2 = 1; //def ignore [Komurka]
		}
		if( !flag.hit )
			switch(skill_id) {
				case AS_SPLASHER:
					if( !wflag ) // Always hits the one exploding.
						flag.hit = 1;
					break;
				case CR_SHIELDBOOMERANG:
					if( sc && sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_CRUSADER )
						flag.hit = 1;
					break;
			}
		if (tsc && !flag.hit && tsc->opt1 && tsc->opt1 != OPT1_STONEWAIT && tsc->opt1 != OPT1_BURNING)
			flag.hit = 1;
	}

	if (!flag.hit) { //Hit/Flee calculation
		short
			flee = tstatus->flee,
#ifdef RENEWAL
				hitrate = 0; //Default hitrate
#else
				hitrate = 80; //Default hitrate
#endif

		if(battle_config.agi_penalty_type && battle_config.agi_penalty_target&target->type) {
			unsigned char attacker_count; //256 max targets should be a sane max
			attacker_count = unit_counttargeted(target);
			if(attacker_count >= battle_config.agi_penalty_count) {
				if (battle_config.agi_penalty_type == 1)
					flee = (flee * (100 - (attacker_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num))/100;
				else //asume type 2: absolute reduction
					flee -= (attacker_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num;
				if(flee < 1) flee = 1;
			}
		}

		hitrate += sstatus->hit - flee;

		if(wd.flag&BF_LONG && !skill_id && //Fogwall's hit penalty is only for normal ranged attacks.
			tsc && tsc->data[SC_FOGWALL])
			hitrate -= 50;

		if(sd && flag.arrow)
			hitrate += sd->bonus.arrow_hit;
#ifdef RENEWAL
		if( sd ) //in Renewal hit bonus from Vultures Eye is not anymore shown in status window
			hitrate += pc_checkskill(sd,AC_VULTURE);
#endif
		if(skill_id)
			switch(skill_id) { //Hit skill modifiers
				//It is proven that bonus is applied on final hitrate, not hit.
				case SM_BASH:
				case MS_BASH:
					hitrate += hitrate * 5 * skill_lv / 100;
					break;
				case MS_MAGNUM:
				case SM_MAGNUM:
					hitrate += hitrate * 10 * skill_lv / 100;
					break;
				case KN_AUTOCOUNTER:
				case PA_SHIELDCHAIN:
				case NPC_WATERATTACK:
				case NPC_GROUNDATTACK:
				case NPC_FIREATTACK:
				case NPC_WINDATTACK:
				case NPC_POISONATTACK:
				case NPC_HOLYATTACK:
				case NPC_DARKNESSATTACK:
				case NPC_UNDEADATTACK:
				case NPC_TELEKINESISATTACK:
				case NPC_BLEEDING:
					hitrate += hitrate * 20 / 100;
					break;
				case KN_PIERCE:
				case ML_PIERCE:
					hitrate += hitrate * 5 * skill_lv / 100;
					break;
				case AS_SONICBLOW:
					if( sd && pc_checkskill(sd,AS_SONICACCEL) > 0 )
						hitrate += hitrate * 50 / 100;
					break;
				case MC_CARTREVOLUTION:
				case GN_CART_TORNADO:
				case GN_CARTCANNON:
					if( sd && pc_checkskill(sd, GN_REMODELING_CART) > 0 )
						hitrate += 4 * pc_checkskill(sd, GN_REMODELING_CART);
					break;
				case GC_VENOMPRESSURE:
					hitrate += 10 + 4 * skill_lv;
					break;
				case SC_FATALMENACE:
						hitrate -= 35 - 5 * skill_lv;
					break;
				case LG_BANISHINGPOINT:
						hitrate += 3 * skill_lv;
					break;
			}

		if( sd ) {
			// Weaponry Research hidden bonus
			if ((skill = pc_checkskill(sd,BS_WEAPONRESEARCH)) > 0)
				hitrate += hitrate * ( 2 * skill ) / 100;
			
			if( (sd->status.weapon == W_DAGGER || sd->status.weapon == W_1HSWORD) &&
			   (skill = pc_checkskill(sd, GN_TRAINING_SWORD)) > 0 )
				hitrate += 3 * skill;
		}
		
		hitrate = cap_value(hitrate, battle_config.min_hitrate, battle_config.max_hitrate);

		if(rnd()%100 >= hitrate)
			wd.dmg_lv = ATK_FLEE;
		else
			flag.hit = 1;
	}	//End hit/miss calculation

	if (flag.hit && !flag.infdef) { //No need to do the math for plants
		//Hitting attack

//Assuming that 99% of the cases we will not need to check for the flag.rh... we don't.
//ATK_RATE scales the damage. 100 = no change. 50 is halved, 200 is doubled, etc
#define ATK_RATE( a ) { wd.damage = wd.damage*(a)/100; if(flag.lh) wd.damage2 = wd.damage2*(a)/100; }
#define ATK_RATE2( a , b ) { wd.damage = wd.damage*(a)/100; if(flag.lh) wd.damage2 = wd.damage2*(b)/100; }
#define ATK_RATER(a){ wd.damage = wd.damage*(a)/100; }
#define ATK_RATEL(a){ wd.damage2 = wd.damage2*(a)/100; }
//Adds dmg%. 100 = +100% (double) damage. 10 = +10% damage
#define ATK_ADDRATE( a ) { wd.damage += wd.damage*(a)/100; if(flag.lh) wd.damage2 += wd.damage2*(a)/100; }
#define ATK_ADDRATE2( a , b ) { wd.damage += wd.damage*(a)/100; if(flag.lh) wd.damage2 += wd.damage2*(b)/100; }
//Adds an absolute value to damage. 100 = +100 damage
#define ATK_ADD( a ) { wd.damage += a; if (flag.lh) wd.damage2 += a; }
#define ATK_ADD2( a , b ) { wd.damage += a; if (flag.lh) wd.damage2 += b; }

		switch (skill_id) { //Calc base damage according to skill
			case PA_SACRIFICE:
				wd.damage = sstatus->max_hp * 9 / 100;
				wd.damage2 = 0;
				break;
#ifndef RENEWAL
			case NJ_ISSEN:
				wd.damage = 40 * sstatus->str + skill_lv * (sstatus->hp / 10 + 35);
				wd.damage2 = 0;
				break;
			case LK_SPIRALPIERCE:
			case ML_SPIRALPIERCE:
				if (sd) {
					short index = sd->equip_index[EQI_HAND_R];

					if (index >= 0 &&
						sd->inventory_data[index] &&
						sd->inventory_data[index]->type == IT_WEAPON)
						wd.damage = sd->inventory_data[index]->weight * 8 / 100; //80% of weight

					ATK_ADDRATE(50 * skill_lv); //Skill modifier applies to weight only.
				} else {
					wd.damage = battle_calc_base_damage(sstatus, &sstatus->rhw, sc, tstatus->size, sd, 0); //Monsters have no weight and use ATK instead
				}

				ATK_ADDRATE(50 * skill_lv); //Skill modifier applies to weight only.
				i = sstatus->str/10;
				i *= i;
				ATK_ADD(i); //Add str bonus.
				switch (tstatus->size) { //Size-fix. Is this modified by weapon perfection?
					case SZ_SMALL: //Small: 125%
						ATK_RATE(125);
						break;
					//case SZ_MEDIUM: //Medium: 100%
					case SZ_BIG: //Large: 75%
						ATK_RATE(75);
						break;
				}
				break;
#endif
			case CR_SHIELDBOOMERANG:
			case PA_SHIELDCHAIN:
#ifdef RENEWAL
				wd.damage = (2 * sstatus->batk);
#else
				wd.damage = sstatus->batk;
#endif
				if (sd) {
					short index = sd->equip_index[EQI_HAND_L];

					if (index >= 0 &&
						sd->inventory_data[index] &&
						sd->inventory_data[index]->type == IT_ARMOR)
						ATK_ADD(sd->inventory_data[index]->weight/10);
				} else
					ATK_ADD(sstatus->rhw.atk2); //Else use Atk2
				break;
#ifdef RENEWAL
			case ASC_BREAKER:
				wd.damage = ((2 * sstatus->batk) + sstatus->rhw.atk) * ((skill_lv / 2) + 3);
				wd.damage2 = sstatus->lhw.atk * ((skill_lv / 2) + 3);
				break;
#endif
			case KO_HAPPOKUNAI:
				if (sd) {
					short index = sd->equip_index[EQI_AMMO];
					wd.damage = 0;
					if (index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_AMMO)
						ATK_ADD((3 * ((2 * sstatus->batk) + sstatus->rhw.atk + sd->inventory_data[index]->atk)) * (skill_lv + 5) / 5);
				} else
					ATK_ADD(5000);
				break;
			case HFLI_SBR44: //[orn]
				if (src->type == BL_HOM) {
					wd.damage = ((TBL_HOM*)src)->homunculus.intimacy ;
					break;
				}
			default: {
				i = (flag.cri?1:0)|
					(flag.arrow?2:0)|
					(skill_id == HW_MAGICCRASHER?4:0)|
					(!skill_id && sc && sc->data[SC_CHANGE]?4:0)|
					(skill_id == MO_EXTREMITYFIST?8:0)|
					(sc && sc->data[SC_WEAPONPERFECTION]?8:0);
				if (flag.arrow && sd)
				switch(sd->status.weapon) {
					case W_BOW:
					case W_REVOLVER:
					case W_GATLING:
					case W_SHOTGUN:
					case W_GRENADE:
						break;
					default:
						i |= 16; // for ex. shuriken must not be influenced by DEX
				}
				wd.damage = battle_calc_base_damage(sstatus, &sstatus->rhw, sc, tstatus->size, sd, i);
				if (flag.lh)
					wd.damage2 = battle_calc_base_damage(sstatus, &sstatus->lhw, sc, tstatus->size, sd, i);

				if (nk&NK_SPLASHSPLIT) { // Divide ATK among targets
					if (wflag > 0)
						wd.damage /= wflag;
					else
						ShowError("0 enemies targeted by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
				}

				//Add any bonuses that modify the base baseatk+watk (pre-skills)
				if (sd) {
					if (sd->bonus.atk_rate)
						ATK_ADDRATE(sd->bonus.atk_rate);

					if (flag.cri && sd->bonus.crit_atk_rate)
						ATK_ADDRATE(sd->bonus.crit_atk_rate);

					if (sd->status.party_id && (skill=pc_checkskill(sd,TK_POWER)) > 0) {
						if( (i = party_foreachsamemap(party_sub_count, sd, 0)) > 1 ) //Exclude the player himself [Inkfish]
							ATK_ADDRATE(2*skill*i);
					}
				}
				break;
			} //End default case
		} //End switch(skill_id)

		//Skill damage modifiers that stack linearly
		if(sc && skill_id != PA_SACRIFICE) {
			if(sc->data[SC_OVERTHRUST])
				skillratio += sc->data[SC_OVERTHRUST]->val3;
			if(sc->data[SC_MAXOVERTHRUST])
				skillratio += sc->data[SC_MAXOVERTHRUST]->val2;
			if(sc->data[SC_BERSERK] || sc->data[SC_SATURDAYNIGHTFEVER] || sc->data[SC__BLOODYLUST])
				skillratio += 100;
#ifdef RENEWAL
			if(sc && sc->data[SC_TRUESIGHT])
				skillratio += 2*sc->data[SC_TRUESIGHT]->val1;
#endif
		}
		if( !skill_id ) {
			ATK_RATE(skillratio);
		} else {
			switch( skill_id ) {
				case SM_BASH:
				case MS_BASH:
					skillratio += 30*skill_lv;
					break;
				case SM_MAGNUM:
				case MS_MAGNUM:
					skillratio += 20*skill_lv;
					break;
				case MC_MAMMONITE:
					skillratio += 50*skill_lv;
					break;
				case HT_POWER:
					skillratio += -50+8*sstatus->str;
					break;
				case AC_DOUBLE:
				case MA_DOUBLE:
					skillratio += 10*(skill_lv-1);
					break;
				case AC_SHOWER:
				case MA_SHOWER:
#ifdef RENEWAL
						skillratio += 50+10*skill_lv;
#else
						skillratio += -25+5*skill_lv;
#endif
					break;
				case AC_CHARGEARROW:
				case MA_CHARGEARROW:
					skillratio += 50;
					break;
#ifndef RENEWAL
				case HT_FREEZINGTRAP:
				case MA_FREEZINGTRAP:
					skillratio += -50+10*skill_lv;
					break;
#endif
				case KN_PIERCE:
				case ML_PIERCE:
					skillratio += 10*skill_lv;
					break;
				case MER_CRASH:
					skillratio += 10*skill_lv;
					break;
				case KN_SPEARSTAB:
					skillratio += 15*skill_lv;
					break;
				case KN_SPEARBOOMERANG:
					skillratio += 50*skill_lv;
					break;
				case KN_BRANDISHSPEAR:
				case ML_BRANDISH:
					{
						int ratio = 100+20*skill_lv;
						skillratio += ratio-100;
						if(skill_lv>3 && wflag==1) skillratio += ratio/2;
						if(skill_lv>6 && wflag==1) skillratio += ratio/4;
						if(skill_lv>9 && wflag==1) skillratio += ratio/8;
						if(skill_lv>6 && wflag==2) skillratio += ratio/2;
						if(skill_lv>9 && wflag==2) skillratio += ratio/4;
						if(skill_lv>9 && wflag==3) skillratio += ratio/2;
						break;
					}
				case KN_BOWLINGBASH:
				case MS_BOWLINGBASH:
					skillratio+= 40*skill_lv;
					break;
				case AS_GRIMTOOTH:
					skillratio += 20*skill_lv;
					break;
				case AS_POISONREACT:
					skillratio += 30*skill_lv;
					break;
				case AS_SONICBLOW:
					skillratio += -50+5*skill_lv;
					break;
				case TF_SPRINKLESAND:
					skillratio += 30;
					break;
				case MC_CARTREVOLUTION:
					skillratio += 50;
					if(sd && sd->cart_weight)
						skillratio += 100*sd->cart_weight/sd->cart_weight_max; // +1% every 1% weight
					else if (!sd)
						skillratio += 100; //Max damage for non players.
					break;
				case NPC_PIERCINGATT:
					skillratio += -25; //75% base damage
					break;
				case NPC_COMBOATTACK:
					skillratio += 25*skill_lv;
					break;
				case NPC_RANDOMATTACK:
				case NPC_WATERATTACK:
				case NPC_GROUNDATTACK:
				case NPC_FIREATTACK:
				case NPC_WINDATTACK:
				case NPC_POISONATTACK:
				case NPC_HOLYATTACK:
				case NPC_DARKNESSATTACK:
				case NPC_UNDEADATTACK:
				case NPC_TELEKINESISATTACK:
				case NPC_BLOODDRAIN:
				case NPC_ACIDBREATH:
				case NPC_DARKNESSBREATH:
				case NPC_FIREBREATH:
				case NPC_ICEBREATH:
				case NPC_THUNDERBREATH:
				case NPC_HELLJUDGEMENT:
				case NPC_PULSESTRIKE:
					skillratio += 100*(skill_lv-1);
					break;
				case RG_BACKSTAP:
					if(sd && sd->status.weapon == W_BOW && battle_config.backstab_bow_penalty)
						skillratio += (200+40*skill_lv)/2;
					else
						skillratio += 200+40*skill_lv;
					break;
				case RG_RAID:
					skillratio += 40*skill_lv;
					break;
				case RG_INTIMIDATE:
					skillratio += 30*skill_lv;
					break;
				case CR_SHIELDCHARGE:
					skillratio += 20*skill_lv;
					break;
				case CR_SHIELDBOOMERANG:
					skillratio += 30*skill_lv;
					break;
				case NPC_DARKCROSS:
				case CR_HOLYCROSS:
					{
						int ratio = 35*skill_lv;
#ifdef RENEWAL
							if(sd && sd->status.weapon == W_2HSPEAR)
								ratio *= 2;
#endif
						skillratio += ratio;
						break;
					}
				case AM_DEMONSTRATION:
					skillratio += 20*skill_lv;
					break;
				case AM_ACIDTERROR:
#ifdef RENEWAL
					skillratio += 100*skill_lv;
					if(tstatus->mode&MD_BOSS)
						skillratio >>= 1;
#else
					skillratio += 40*skill_lv;
#endif
					break;
				case MO_FINGEROFFENSIVE:
					skillratio += 50*skill_lv;
					break;
				case MO_INVESTIGATE:
					skillratio += 100+150*skill_lv;
					flag.pdef = flag.pdef2 = 2;
					break;
				case MO_EXTREMITYFIST:
					skillratio += -100+100*(8+sstatus->sp/10);
					skillratio = min(500000,skillratio); //We stop at roughly 50k SP for overflow protection
					break;
				case MO_TRIPLEATTACK:
					skillratio += 20*skill_lv;
					break;
				case MO_CHAINCOMBO:
					skillratio += 50+50*skill_lv;
					break;
				case MO_COMBOFINISH:
					skillratio += 140+60*skill_lv;
					break;
				case BA_MUSICALSTRIKE:
				case DC_THROWARROW:
					skillratio += 25+25*skill_lv;
					break;
				case CH_TIGERFIST:
					skillratio += 100*skill_lv-60;
					break;
				case CH_CHAINCRUSH:
					skillratio += 300+100*skill_lv;
					break;
				case CH_PALMSTRIKE:
					skillratio += 100+100*skill_lv;
					break;
				case LK_HEADCRUSH:
					skillratio += 40*skill_lv;
					break;
				case LK_JOINTBEAT:
					i = 10*skill_lv-50;
					// Although not clear, it's being assumed that the 2x damage is only for the break neck ailment.
					if (wflag&BREAK_NECK) i*=2;
					skillratio += i;
					break;
#ifdef RENEWAL
				case LK_SPIRALPIERCE:
				case ML_SPIRALPIERCE:
					{ // Formula: Floor[Floor(Weapon Weight/2)*skill level + ATK ]*(100%+50%*s.lvl) * 5 multi-hits
						short index = sd?sd->equip_index[EQI_HAND_R]:0;
						int weight = 0;

						if (sd && index >= 0 &&
							sd->inventory_data[index] &&
							sd->inventory_data[index]->type == IT_WEAPON)
								weight = sd->inventory_data[index]->weight/20;
						ATK_ADD(weight * skill_lv)
						skillratio += 50*skill_lv;
					}
					break;
#endif
				case ASC_METEORASSAULT:
					skillratio += 40*skill_lv-60;
					break;
				case SN_SHARPSHOOTING:
				case MA_SHARPSHOOTING:
					skillratio += 100+50*skill_lv;
					break;
				case CG_ARROWVULCAN:
					skillratio += 100+100*skill_lv;
					break;
				case AS_SPLASHER:
					skillratio += 400+50*skill_lv;
					if (sd)
						skillratio += 20*pc_checkskill(sd,AS_POISONREACT);
					break;
#ifndef RENEWAL
				case ASC_BREAKER:
					skillratio += 100*skill_lv-100;
					break;
#endif
				case PA_SACRIFICE:
					skillratio += 10*skill_lv-10;
					break;
				case PA_SHIELDCHAIN:
					skillratio += 30*skill_lv;
					break;
				case WS_CARTTERMINATION:
					i = 10*(16-skill_lv);
					if (i < 1) i = 1;
					//Preserve damage ratio when max cart weight is changed.
					if (sd && sd->cart_weight)
						skillratio += sd->cart_weight/i*80000/battle_config.max_cart_weight-100;
					else if (!sd)
						skillratio += 80000/i-100;
					break;
				case TK_DOWNKICK:
					skillratio += 60+20*skill_lv;
					break;
				case TK_STORMKICK:
					skillratio += 60+20*skill_lv;
					break;
				case TK_TURNKICK:
					skillratio += 90+30*skill_lv;
					break;
				case TK_COUNTER:
					skillratio += 90+30*skill_lv;
					break;
				case TK_JUMPKICK:
					skillratio += -70+10*skill_lv;
					if (sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == skill_id)
						skillratio += 10*status_get_lv(src)/3; //Tumble bonus
					if (wflag) {
						skillratio += 10*status_get_lv(src)/3; //Running bonus (TODO: What is the real bonus?)
						if( sc && sc->data[SC_SPURT] )  // Spurt bonus
							skillratio *= 2;
					}
					break;
				case GS_TRIPLEACTION:
					skillratio += 50*skill_lv;
					break;
				case GS_BULLSEYE:
					//Only works well against brute/demihumans non bosses.
					if((tstatus->race == RC_BRUTE || tstatus->race == RC_DEMIHUMAN)
						&& !(tstatus->mode&MD_BOSS))
						skillratio += 400;
					break;
				case GS_TRACKING:
					skillratio += 100*(skill_lv+1);
					break;
				case GS_PIERCINGSHOT:
					skillratio += 20*skill_lv;
					break;
				case GS_RAPIDSHOWER:
					skillratio += 10*skill_lv;
					break;
				case GS_DESPERADO:
					skillratio += 50*(skill_lv-1);
					break;
				case GS_DUST:
					skillratio += 50*skill_lv;
					break;
				case GS_FULLBUSTER:
					skillratio += 100*(skill_lv+2);
					break;
				case GS_SPREADATTACK:
#ifdef RENEWAL
					skillratio += 20*(skill_lv);
#else
					skillratio += 20*(skill_lv-1);
#endif
					break;
#ifdef RENEWAL
				case NJ_ISSEN:
					skillratio += 100*(skill_lv-1);
					break;
#endif
				case NJ_HUUMA:
					skillratio += 50+150*skill_lv;
					break;
				case NJ_TATAMIGAESHI:
#ifdef RENEWAL
					ATK_RATE(200);
#endif
					skillratio += 10*skill_lv;
					break;
				case NJ_KASUMIKIRI:
					skillratio += 10*skill_lv;
					break;
				case NJ_KIRIKAGE:
					skillratio += 100*(skill_lv-1);
					break;
				case KN_CHARGEATK: {
						//+100% every 3 cells of distance but hard-limited to 500%.
						unsigned int k = wflag/3;
						if( k < 2 ) k = 0;
						else if( k > 1 && k < 3 ) k = 1;
						else if( k > 2 && k < 4 ) k = 2;
						else if( k > 3 && k < 5 ) k = 3;
						else k = 4;
						skillratio += 100*k;
					}
					break;
				case HT_PHANTASMIC:
					skillratio += 50;
					break;
				case MO_BALKYOUNG:
					skillratio += 200;
					break;
				case HFLI_MOON: //[orn]
					skillratio += 10+110*skill_lv;
					break;
				case HFLI_SBR44: //[orn]
					skillratio += 100*(skill_lv-1);
					break;
				case NPC_VAMPIRE_GIFT:
					skillratio += ((skill_lv-1)%5+1)*100;
					break;
				case RK_SONICWAVE:
					// ATK = {((Skill Level + 5) x 100) x (1 + [(Caster Base Level - 100) / 200])} %
					skillratio = (skill_lv + 5) * 100;
					skillratio = skillratio * (100 + (status_get_lv(src) - 100) / 2) / 100;
					break;
				case RK_HUNDREDSPEAR:
					skillratio += 500 + (80 * skill_lv);
					if( sd ) {
						short index = sd->equip_index[EQI_HAND_R];
						if( index >= 0 && sd->inventory_data[index] 
							&& sd->inventory_data[index]->type == IT_WEAPON )
							skillratio += max(10000 - sd->inventory_data[index]->weight, 0) / 10;
						skillratio += 50 * pc_checkskill(sd,LK_SPIRALPIERCE);
					} // (1 + [(Caster Base Level - 100) / 200])
					skillratio = skillratio * (100 + (status_get_lv(src) - 100) / 2) / 100;
					break;
				case RK_WINDCUTTER:
					skillratio = (skill_lv + 2) * 50;
					RE_LVL_DMOD(100);
					break;
				case RK_IGNITIONBREAK: {
						// 3x3 cell Damage = ATK [{(Skill Level x 300) x (1 + [(Caster Base Level - 100) / 100])}] %
						// 7x7 cell Damage = ATK [{(Skill Level x 250) x (1 + [(Caster Base Level - 100) / 100])}] %
						// 11x11 cell Damage = ATK [{(Skill Level x 200) x (1 + [(Caster Base Level - 100) / 100])}] %
						int dmg = 300; // Base maximum damage at less than 3 cells.
						i = distance_bl(src,target);
						if( i > 3 && i <= 7 )
							dmg -= 50; // Greater than 3 cells, less than 8. (250 damage)
						else if( i > 7 && i <= 11 )
							dmg -= 100; // Greather than 7 cells, less than 12. (200 damage)
						dmg = (skill_lv * dmg) * (1 + (status_get_lv(src) - 100) / 100);
						// Elemental check, +(Skill Level x 100)% damage if your element is fire.
						if( sstatus->rhw.ele  == ELE_FIRE )
							dmg += skill_lv * 100;
						skillratio = dmg;
					}
					break;
				case RK_CRUSHSTRIKE:
					if( sd ) {
						//ATK [{Weapon Level * (Weapon Upgrade Level + 6) * 100} + (Weapon ATK) + (Weapon Weight)]%
						short index = sd->equip_index[EQI_HAND_R];
						if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON )
							skillratio += sd->inventory_data[index]->weight/10 + sstatus->rhw.atk +
								100 * sd->inventory_data[index]->wlv * (sd->status.inventory[index].refine + 6);
					}
					break;
				case RK_STORMBLAST:
					// ATK = [{Rune Mastery Skill Level + (Caster INT / 8)} x 100] %
					skillratio = ((sd ? pc_checkskill(sd,RK_RUNEMASTERY) : 0) + (sstatus->int_ / 8)) * 100;
					break;
				case RK_PHANTOMTHRUST:
					// ATK = [{(Skill Level x 50) + (Spear Master Level x 10)} x Caster Base Level / 150] %
					skillratio = 50 * skill_lv + (sd ? pc_checkskill(sd,KN_SPEARMASTERY) * 10 : 0);
					RE_LVL_DMOD(150);
					break;
				case GC_CROSSIMPACT:
					skillratio += 900 + 100 * skill_lv;
					RE_LVL_DMOD(120);
					break;
				case GC_COUNTERSLASH:
					//ATK [{(Skill Level x 100) + 300} x Caster's Base Level / 120]% + ATK [(AGI x 2) + (Caster's Job Level x 4)]%
					skillratio += 200 + (100 * skill_lv);
					RE_LVL_DMOD(120);
					skillratio += sstatus->agi * 2 + (sd ? sd->status.job_level * 4 : 0);
					break;
				case GC_PHANTOMMENACE:
					skillratio += 200;
					break;
				case GC_ROLLINGCUTTER:
					skillratio += -50 + 50 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case GC_CROSSRIPPERSLASHER:
					skillratio += 300 + 80 * skill_lv;
					RE_LVL_DMOD(100);
					if( sc && sc->data[SC_ROLLINGCUTTER] )
						skillratio += sc->data[SC_ROLLINGCUTTER]->val1 * sstatus->agi;
					break;
				case AB_DUPLELIGHT_MELEE:
					skillratio += 10 * skill_lv;
					break;
				case RA_ARROWSTORM:
					skillratio += 900 + 80 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case RA_AIMEDBOLT:
					skillratio += 400 + 50 * skill_lv;
					RE_LVL_DMOD(100);
					if( tsc && (tsc->data[SC_BITE] || tsc->data[SC_ANKLE] || tsc->data[SC_ELECTRICSHOCKER]) )
						wd.div_ = tstatus->size + 2 + ( (rnd()%100 < 50-tstatus->size*10) ? 1 : 0 );
					break;
				case RA_CLUSTERBOMB:
					skillratio += 100 + 100 * skill_lv;
					break;
				case RA_WUGDASH:// ATK 300%
					skillratio += 200;
					break;
				case RA_WUGSTRIKE:
					skillratio = 200 * skill_lv;
					break;
				case RA_WUGBITE:
					skillratio += 300 + 200 * skill_lv;
					if ( skill_lv == 5 ) skillratio += 100;
					break;
				case RA_SENSITIVEKEEN:
					skillratio += 50 * skill_lv;
					break;
				case NC_BOOSTKNUCKLE:
					skillratio += 100 + 100 * skill_lv + sstatus->dex;
					RE_LVL_DMOD(100);
					break;
				case NC_PILEBUNKER:
					skillratio += 200 + 100 * skill_lv + sstatus->str;
					RE_LVL_DMOD(100);
					break;
				case NC_VULCANARM:
					skillratio = 70 * skill_lv + sstatus->dex;
					RE_LVL_DMOD(100);
					break;
				case NC_FLAMELAUNCHER:
				case NC_COLDSLOWER:
					skillratio += 200 + 300 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case NC_ARMSCANNON:
					switch( tstatus->size ) {
						case SZ_SMALL: skillratio += 200 + 400 * skill_lv; break;// Small
						case SZ_MEDIUM: skillratio += 200 + 350 * skill_lv; break;// Medium
						case SZ_BIG: skillratio += 200 + 300 * skill_lv; break;// Large
					}
					RE_LVL_DMOD(100);
					//NOTE: Their's some other factors that affects damage, but not sure how exactly. Will recheck one day. [Rytech]
					break;
				case NC_AXEBOOMERANG:
					skillratio += 150 + 50 * skill_lv;
					if( sd ) {
						short index = sd->equip_index[EQI_HAND_R];
						if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_WEAPON )
						skillratio += sd->inventory_data[index]->weight / 10; // Weight is divided by 10 since 10 weight in coding make 1 whole actural weight. [Rytech]
					}
					RE_LVL_DMOD(100);
					break;
				case NC_POWERSWING:
					skillratio += 200 + 100 * skill_lv + sstatus->str + sstatus->dex;
					RE_LVL_DMOD(100);
					break;
				case NC_AXETORNADO:
					skillratio += 100 + 100 * skill_lv + sstatus->vit;
					RE_LVL_DMOD(100);
					if( sstatus->rhw.ele == ELE_WIND )
						skillratio += skillratio * 25 / 100;
					i = distance_bl(src,target);
					if( i > 5 ) skillratio = skillratio * 75 / 100;
					break;
				case SC_FATALMENACE:
					skillratio += 100 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case SC_TRIANGLESHOT:
					skillratio += 200 + (skill_lv - 1) * sstatus->agi / 2;
					RE_LVL_DMOD(120);
					break;
				case SC_FEINTBOMB:
					skillratio = (1 + skill_lv) * sstatus->dex / 2 * (sd ? sd->status.job_level / 10 : 1);
					RE_LVL_DMOD(120);
					break;
				case LG_CANNONSPEAR:// Stimated formula. Still need confirm it.
					skillratio = (50  + sstatus->str) * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case LG_BANISHINGPOINT:
					skillratio = (50 * skill_lv) + (sd ? pc_checkskill(sd,SM_BASH) * 30 : 0);
					RE_LVL_DMOD(100);
					break;
				case LG_SHIELDPRESS:
					skillratio = 150 * skill_lv + sstatus->str;
					if( sd ) {
						short index = sd->equip_index[EQI_HAND_L];
						if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR )
						skillratio += sd->inventory_data[index]->weight / 10;
					}
					RE_LVL_DMOD(100);
					break;
				case LG_PINPOINTATTACK:
					skillratio = 100 * skill_lv + 5 * sstatus->agi;
					RE_LVL_DMOD(120);
					break;
				case LG_RAGEBURST:
					if( sd && sd->spiritball_old )
						skillratio = 200 * sd->spiritball_old + (sstatus->max_hp - sstatus->hp) / 100;
					else
						skillratio = 3000 + (sstatus->max_hp - sstatus->hp) / 100;
					RE_LVL_DMOD(100);
					break;
				case LG_SHIELDSPELL: // [(Caster Base Level x 4) + (Shield DEF x 10) + (Caster VIT x 2)] %
					if( sd && skill_lv == 1 ) {
						struct item_data *shield_data = sd->inventory_data[sd->equip_index[EQI_HAND_L]];
						skillratio = status_get_lv(src) * 4 + status_get_vit(src) * 2;
						if( shield_data )
							skillratio += shield_data->def * 10;
					} else
						skillratio = 0;
					break;
				case LG_OVERBRAND:
					skillratio = 200 * skill_lv + (sd ? pc_checkskill(sd,CR_SPEARQUICKEN) * 50 : 0);
					RE_LVL_DMOD(100);
					break;
				case LG_OVERBRAND_BRANDISH:
					skillratio = 100 * skill_lv + (sstatus->str + sstatus->dex);
					RE_LVL_DMOD(100);
					break;
				case LG_OVERBRAND_PLUSATK:
					skillratio = 100 * skill_lv + rnd_value(1, 100);
					break;
				case LG_MOONSLASHER:
					skillratio = 120 * skill_lv + (sd ? pc_checkskill(sd,LG_OVERBRAND) * 80 : 0);
					RE_LVL_DMOD(100);
					break;
				case LG_RAYOFGENESIS:
					skillratio += 200 + 300 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case LG_EARTHDRIVE:
					if( sd ) {
						short index = sd->equip_index[EQI_HAND_L];
						if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR )
						skillratio = (1 + skill_lv) * sd->inventory_data[index]->weight / 10;
					}
					RE_LVL_DMOD(100);
					break;
				case LG_HESPERUSLIT:
					skillratio = 120 * skill_lv;
					if( sc && sc->data[SC_INSPIRATION] )
						skillratio += 600;
					RE_LVL_DMOD(100);
					break;
				case SR_DRAGONCOMBO:
					skillratio += 40 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case SR_SKYNETBLOW:
					//ATK [{(Skill Level x 80) + (Caster AGI)} x Caster Base Level / 100] %
					skillratio = 80 * skill_lv + sstatus->agi;
					if( sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_DRAGONCOMBO )
						//ATK [{(Skill Level x 100) + (Caster AGI) + 150} x Caster Base Level / 100] %
						skillratio = 100 * skill_lv + sstatus->agi + 150;
					RE_LVL_DMOD(100);
					break;
				case SR_EARTHSHAKER:
					if( tsc && (tsc->data[SC_HIDING] || tsc->data[SC_CLOAKING] || // [(Skill Level x 150) x (Caster Base Level / 100) + (Caster INT x 3)] %
						tsc->data[SC_CHASEWALK] || tsc->data[SC_CLOAKINGEXCEED] || tsc->data[SC__INVISIBILITY]) ) {
						skillratio = 150 * skill_lv;
						RE_LVL_DMOD(100);
						skillratio += sstatus->int_ * 3;
					} else { //[(Skill Level x 50) x (Caster Base Level / 100) + (Caster INT x 2)] %
						skillratio += 50 * (skill_lv-2);
						RE_LVL_DMOD(100);
						skillratio += sstatus->int_ * 2;
					}
					break;
				case SR_FALLENEMPIRE: // ATK [(Skill Level x 150 + 100) x Caster Base Level / 150] %
					skillratio += 150 *skill_lv;
					RE_LVL_DMOD(150);
					break;
				case SR_TIGERCANNON: { // ATK [((Caster consumed HP + SP) / 4) x Caster Base Level / 100] %
						int hp = sstatus->max_hp * (10 + 2 * skill_lv) / 100,
							sp = sstatus->max_sp * (5 + skill_lv) / 100;
						skillratio = (hp+sp) / 4;
						if( sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE ) // ATK [((Caster consumed HP + SP) / 2) x Caster Base Level / 100] %
							skillratio = (hp+sp) / 2;
						RE_LVL_DMOD(100);
					}
					break;
				case SR_RAMPAGEBLASTER:
					skillratio += 20 * skill_lv * (sd ? sd->spiritball_old : 1) - 100;
					if( sc && sc->data[SC_EXPLOSIONSPIRITS] ) { 
						skillratio += sc->data[SC_EXPLOSIONSPIRITS]->val1 * 20;
						RE_LVL_DMOD(120);
					} else
						RE_LVL_DMOD(150);
					break;
				case SR_KNUCKLEARROW:
					if( wflag&4 ) {  // ATK [(Skill Level x 150) + (1000 x Target current weight / Maximum weight) + (Target Base Level x 5) x (Caster Base Level / 150)] %
						skillratio = 150 * skill_lv + status_get_lv(target) * 5 * (status_get_lv(src) / 100) ;
						if( tsd && tsd->weight )
							skillratio += 100 * (tsd->weight / tsd->max_weight);
					} else // ATK [(Skill Level x 100 + 500) x Caster Base Level / 100] %
						skillratio += 400 + (100 * skill_lv);
					RE_LVL_DMOD(100);
					break;
				case SR_WINDMILL: // ATK [(Caster Base Level + Caster DEX) x Caster Base Level / 100] %
					skillratio = status_get_lv(src) + sstatus->dex;
					RE_LVL_DMOD(100);
					break;
				case SR_GATEOFHELL:
					if( sc && sc->data[SC_COMBO]
						&& sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE )
						skillratio += 800 * skill_lv -100;
					else
						skillratio += 500 * skill_lv -100;
					RE_LVL_DMOD(100);
					break;
				case SR_GENTLETOUCH_QUIET:
					skillratio += 100 * skill_lv - 100 + sstatus->dex;
					RE_LVL_DMOD(100);
					break;
				case SR_HOWLINGOFLION:
					skillratio += 300 * skill_lv - 100;
					RE_LVL_DMOD(150);
					break;
				case SR_RIDEINLIGHTNING: // ATK [{(Skill Level x 200) + Additional Damage} x Caster Base Level / 100] %
					if( (sstatus->rhw.ele) == ELE_WIND || (sstatus->lhw.ele) == ELE_WIND )
						skillratio += skill_lv * 50;
					skillratio += -100 + 200 * skill_lv;
					RE_LVL_DMOD(100);
					break;
				case WM_REVERBERATION_MELEE:
					//ATK [{(Skill Level x 100) + 300} x Caster Base Level / 100]
					skillratio += 200 + 100 * pc_checkskill(sd, WM_REVERBERATION);
					RE_LVL_DMOD(100);
					break;
				case WM_SEVERE_RAINSTORM_MELEE:
					//ATK [{(Caster DEX + AGI) x (Skill Level / 5)} x Caster Base Level / 100] %
					skillratio = (sstatus->dex + sstatus->agi) * (skill_lv / 5);
					RE_LVL_DMOD(100);
					break;
				case WM_GREAT_ECHO:
					skillratio += 300 + 200 * skill_lv;
					//Chorus bonus dont count the first 2 Minstrel's/Wanderer's and only increases when their's 3 or more. [Rytech]
					if ( chorusbonus == 1 )
					skillratio += 100;
					else if ( chorusbonus == 2 )
					skillratio += 200;
					else if ( chorusbonus == 3 )
					skillratio += 400;
					else if ( chorusbonus == 4 )
					skillratio += 800;
					else if ( chorusbonus == 5 )
					skillratio += 1600;
					break;
				case WM_SOUND_OF_DESTRUCTION:
					skillratio += 400;
					break;
				case GN_CART_TORNADO:
					// ATK [( Skill Level x 50 ) + ( Cart Weight / ( 150 - Caster Base STR ))] + ( Cart Remodeling Skill Level x 50 )] %
					skillratio = 50 * skill_lv;
					if( sd && sd->cart_weight)
						 skillratio += sd->cart_weight / 10 / max(150-sstatus->str,1) + pc_checkskill(sd, GN_REMODELING_CART) * 50;
					break;
				case GN_CARTCANNON:
					// ATK [{( Cart Remodeling Skill Level x 50 ) x ( INT / 40 )} + ( Cart Cannon Skill Level x 60 )] %
					skillratio = 60 * skill_lv; 
					if( sd ) skillratio += pc_checkskill(sd, GN_REMODELING_CART) * 50 * (sstatus->int_ / 40);
					break;
				case GN_SPORE_EXPLOSION:
					skillratio = ( 100 * skill_lv ) + ( 200 + sstatus->int_ );
					RE_LVL_DMOD(100);
					break;
				case GN_WALLOFTHORN:
					skillratio += 10 * skill_lv;
					break;
				case GN_CRAZYWEED_ATK:
					skillratio += 400 + 100 * skill_lv;
					break;
				case GN_SLINGITEM_RANGEMELEEATK:
					if( sd ) {
						switch( sd->itemid ) {
							case 13260: // Apple Bomob
								skillratio = sstatus->str + sstatus->dex + 300;
								break;
							case 13262: // Melon Bomb
								skillratio = sstatus->str + sstatus->dex + 500;
								break;
							case 13261: // Coconut Bomb
							case 13263: // Pinapple Bomb
							case 13264: // Banana Bomb
								skillratio = sstatus->str + sstatus->dex + 800;
								break;
							case 13265: // Black Lump
								skillratio = (sstatus->str + sstatus->agi + sstatus->dex) / 3;
								break;
							case 13266: // Hard Black Lump
								skillratio = (sstatus->str + sstatus->agi + sstatus->dex) / 2;
								break;
							case 13267: // Extremely Hard Black Lump
								skillratio = sstatus->str + sstatus->agi + sstatus->dex;
								break;
						}
					}
					break;
				case SO_VARETYR_SPEAR:
					//ATK [{( Striking Level x 50 ) + ( Varetyr Spear Skill Level x 50 )} x Caster Base Level / 100 ] %
					skillratio = 50 * skill_lv + ( sd ? pc_checkskill(sd, SO_STRIKING) * 50 : 0 );
					RE_LVL_DMOD(100);
					if( sc && sc->data[SC_BLAST_OPTION] )
						skillratio += (sd ? sd->status.job_level * 5 : 0);
					break;
				// Physical Elemantal Spirits Attack Skills
				case EL_CIRCLE_OF_FIRE:
				case EL_FIRE_BOMB_ATK:
				case EL_STONE_RAIN:
					skillratio += 200;
					break;
				case EL_FIRE_WAVE_ATK:
					skillratio += 500;
					break;
				case EL_TIDAL_WEAPON:
					skillratio += 1400;
					break;
				case EL_WIND_SLASH:
					skillratio += 100;
					break;
				case EL_HURRICANE:
					skillratio += 600;
					break;
				case EL_TYPOON_MIS:
				case EL_WATER_SCREW_ATK:
					skillratio += 900;
					break;
				case EL_STONE_HAMMER:
					skillratio += 400;
					break;
				case EL_ROCK_CRUSHER:
					skillratio += 700;
					break;
				case KO_JYUMONJIKIRI:
					skillratio += -100 + 150 * skill_lv;
					RE_LVL_DMOD(120);
					if( tsc && tsc->data[SC_JYUMONJIKIRI] )
						skillratio += skill_lv * status_get_lv(src);
					break;
				case KO_HUUMARANKA:
					skillratio += -100 + 150 * skill_lv + sstatus->agi + sstatus->dex + (sd ? pc_checkskill(sd,NJ_HUUMA) * 100 : 0);
					break;
				case KO_SETSUDAN:
					skillratio += 100 * (skill_lv-1);
					RE_LVL_DMOD(100);
					break;
				case KO_BAKURETSU:
					skillratio = (sd ? pc_checkskill(sd,NJ_TOBIDOUGU) : 1) * (50 + sstatus->dex / 4) * skill_lv * 4 / 10;
					RE_LVL_DMOD(120);
					skillratio += 10 * (sd ? sd->status.job_level : 1);
					break;
				case KO_MAKIBISHI:
					skillratio = 20 * skill_lv;
					break;
				case MH_NEEDLE_OF_PARALYZE:
					skillratio += 600 + 100 * skill_lv;
					break;
				case MH_STAHL_HORN:
					skillratio += 400 + 100 * skill_lv * status_get_lv(src);
					skillratio = skillratio / 100; //@TODO uv1 factor need to be confirmed
					break;
				case MH_LAVA_SLIDE:
					skillratio += -100 + 70 * skill_lv;
					break;
				case MH_SONIC_CRAW:
					skillratio += -100 + 40 * skill_lv * status_get_lv(src);
					skillratio = skillratio / 100; //@TODO uv1 factor need to be confirmed
					break;
				case MH_SILVERVEIN_RUSH:
					skillratio += -100 + (150 * skill_lv * status_get_lv(src)) / 100;
					break;
				case MH_MIDNIGHT_FRENZY:
					skillratio += -100 + (300 * skill_lv * status_get_lv(src)) / 150;
					break;
				case MH_TINDER_BREAKER:
					skillratio += -100 + (100 * skill_lv + status_get_str(src));
					skillratio = (skillratio * status_get_lv(src)) / 120;
					break;
				case MH_CBC:
					skillratio += 300 * skill_lv + 4 * status_get_lv(src);
					break;
				case MH_MAGMA_FLOW:
					skillratio += -100 + 100 * skill_lv + 3 * status_get_lv(src);
					skillratio = (skillratio * status_get_lv(src)) / 120;
					break;
			}

			ATK_RATE(skillratio);

			//Constant/misc additions from skills
			switch (skill_id) {
				case MO_EXTREMITYFIST:
					ATK_ADD(250 + 150*skill_lv);
					break;
				case TK_DOWNKICK:
				case TK_STORMKICK:
				case TK_TURNKICK:
				case TK_COUNTER:
				case TK_JUMPKICK:
					//TK_RUN kick damage bonus.
					if(sd && sd->weapontype1 == W_FIST && sd->weapontype2 == W_FIST)
						ATK_ADD(10*pc_checkskill(sd, TK_RUN));
					break;
				case GS_MAGICALBULLET:
					if(sstatus->matk_max > sstatus->matk_min) {
						ATK_ADD(sstatus->matk_min+rnd()%(sstatus->matk_max-sstatus->matk_min));
					} else {
						ATK_ADD(sstatus->matk_min);
					}
					break;
				case NJ_SYURIKEN:
					ATK_ADD(4*skill_lv);
					break;
#ifdef RENEWAL
				case NJ_ISSEN:
					// Damage = (current HP + atk * skill_lv) - (sdef+edef)
					ATK_ADD(sstatus->hp);
					wd.damage2 = 0; // needs more info if this really 0 for dual weilding KG/OB. [malufett]
					if(sc && sc->data[SC_BUNSINJYUTSU] && (i = sc->data[SC_BUNSINJYUTSU]->val2) > 0) {
						wd.div_ = -(i + 2); // mirror image number of hits + 2
						ATK_ADDRATE(20 + i*20); // (20 + 20 * mirror image) %
					}
					break;
#endif
				case HT_FREEZINGTRAP:
					if(sd)
						ATK_ADD(40*pc_checkskill(sd, RA_RESEARCHTRAP));
					break;
				case RA_WUGDASH: //(Caster Current Weight x 10 / 8)
					if(sd && sd->weight)
						ATK_ADD(sd->weight/8);
				case RA_WUGSTRIKE:
				case RA_WUGBITE:
					if(sd)
						ATK_ADD(30*pc_checkskill(sd, RA_TOOTHOFWUG));
					if( sc && sc->data[SC_DANCEWITHWUG] )
						ATK_ADDRATE(skill_lv*10*(2 + chorusbonus));
					break;
				case LG_SHIELDPRESS:
					if(sd) {
						int damagevalue = 0;
						short index = sd->equip_index[EQI_HAND_L];
						if(index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR)
							damagevalue = sstatus->vit * sd->status.inventory[index].refine;
						ATK_ADD(damagevalue);
					}
					break;
				case SR_GATEOFHELL:
					ATK_ADD (sstatus->max_hp - sstatus->hp);
					if(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE) {
						ATK_ADD((sstatus->max_sp*(1 + skill_lv*2/10)) + 40*status_get_lv(src));
					} else {
						ATK_ADD((sstatus->sp*(1 + skill_lv*2/10)) + 10*status_get_lv(src));
					}
					break;
				case SR_TIGERCANNON: // (Tiger Cannon skill level x 240) + (Target Base Level x 40)
					ATK_ADD(skill_lv*240 + status_get_lv(target)*40);
					if(sc && sc->data[SC_COMBO] && sc->data[SC_COMBO]->val1 == SR_FALLENEMPIRE)
						// (Tiger Cannon skill level x 500) + (Target Base Level x 40)
						ATK_ADD(skill_lv*500 + status_get_lv(target)*40);
					break;
				case SR_FALLENEMPIRE:// [(Target Size value + Skill Level - 1) x Caster STR] + [(Target current weight x Caster DEX / 120)]
					ATK_ADD(((tstatus->size + 1)*2 + skill_lv - 1)*sstatus->str);
					if(tsd && tsd->weight) {
						ATK_ADD((tsd->weight/10)*sstatus->dex/120);
					} else {
						ATK_ADD(status_get_lv(target)*50); //mobs
					}
					break;
				case KO_SETSUDAN:
					if(tsc && tsc->data[SC_SPIRIT]) {
						ATK_ADDRATE(200*tsc->data[SC_SPIRIT]->val1); // +200%.
						status_change_end(target,SC_SPIRIT,INVALID_TIMER);
					}
					break;
				case KO_KAIHOU:
					if(sd) {
						ARR_FIND(1, 6, i, sd->talisman[i] > 0);
						if(i < 5) {
							s_ele = i;
							ATK_ADDRATE(200*sd->talisman[i]); // +200%.
							pc_del_talisman(sd,sd->talisman[i],i);
						}
					}
					break;
			}
		}
		//Div fix.
		damage_div_fix(wd.damage, wd.div_);

		//The following are applied on top of current damage and are stackable.
		if (sc) {
#ifndef RENEWAL
			if (sc->data[SC_TRUESIGHT])
				ATK_ADDRATE(2*sc->data[SC_TRUESIGHT]->val1);
#endif
			if (sc->data[SC_GLOOMYDAY_SK] && (
				skill_id == LK_SPIRALPIERCE || skill_id == KN_BRANDISHSPEAR ||
				skill_id == CR_SHIELDBOOMERANG || skill_id == PA_SHIELDCHAIN ||
				skill_id == RK_HUNDREDSPEAR || skill_id == LG_SHIELDPRESS))
					ATK_ADDRATE(sc->data[SC_GLOOMYDAY_SK]->val2);

			if (sc && sc->data[SC_SPIRIT]) {
				if (skill_id == AS_SONICBLOW && sc->data[SC_SPIRIT]->val2 == SL_ASSASIN) {
					ATK_ADDRATE(map_flag_gvg(src->m)?25:100); //+25% dmg on woe/+100% dmg on nonwoe
				} else if (skill_id == CR_SHIELDBOOMERANG && (sc->data[SC_SPIRIT]->val2 == SL_CRUSADER)) {
					ATK_ADDRATE(100);
				}
			}

			if (sc->data[SC_EDP]) {
				switch (skill_id) {
					case AS_SPLASHER:
					case AS_VENOMKNIFE:
#ifndef RENEWAL_EDP
					case ASC_BREAKER:
					case ASC_METEORASSAULT:
#endif
						break;
#ifdef RENEWAL_EDP
					case AS_SONICBLOW:
					case AS_GRIMTOOTH:
					case ASC_BREAKER:
					case GC_COUNTERSLASH:
					case GC_CROSSIMPACT:
						ATK_RATE(50);
#endif
					default:
						ATK_ADDRATE(sc->data[SC_EDP]->val3);
				}
			}
			if (sc->data[SC_STYLE_CHANGE]) {
				TBL_HOM *hd = BL_CAST(BL_HOM,src);
				if (hd) ATK_ADD(hd->homunculus.spiritball * 3);
			}
		}

		switch (skill_id) {
			case AS_SONICBLOW:
				if(sd && pc_checkskill(sd,AS_SONICACCEL)>0)
					ATK_ADDRATE(10);
				break;
			case NC_AXETORNADO:
				if( (sstatus->rhw.ele) == ELE_WIND || (sstatus->lhw.ele) == ELE_WIND )
					ATK_ADDRATE(50);
				break;
		}

		if( (i = battle_adjust_skill_damage(src->m,skill_id)) )
			ATK_RATE(i);

		if( sd ) {
			if (skill_id && (i = pc_skillatk_bonus(sd, skill_id)))
				ATK_ADDRATE(i);

			if( skill_id != PA_SACRIFICE && skill_id != MO_INVESTIGATE && skill_id != CR_GRANDCROSS && skill_id != NPC_GRANDDARKNESS &&
			    skill_id != PA_SHIELDCHAIN && skill_id != KO_HAPPOKUNAI && !flag.cri )
			{ //Elemental/Racial adjustments
				if( sd->right_weapon.def_ratio_atk_ele & (1<<tstatus->def_ele) ||
					sd->right_weapon.def_ratio_atk_race & (1<<tstatus->race) ||
					sd->right_weapon.def_ratio_atk_race & (1<<(is_boss(target)?RC_BOSS:RC_NONBOSS))
				)
					flag.pdef = 1;

				if( sd->left_weapon.def_ratio_atk_ele & (1<<tstatus->def_ele) ||
					sd->left_weapon.def_ratio_atk_race & (1<<tstatus->race) ||
					sd->left_weapon.def_ratio_atk_race & (1<<(is_boss(target)?RC_BOSS:RC_NONBOSS))
				)
				{ //Pass effect onto right hand if configured so. [Skotlex]
					if (battle_config.left_cardfix_to_right && flag.rh)
						flag.pdef = 1;
					else
						flag.pdef2 = 1;
				}
			}

			if (skill_id != CR_GRANDCROSS && skill_id != NPC_GRANDDARKNESS) {
				//Ignore Defense?
				if (!flag.idef && (
					sd->right_weapon.ignore_def_ele & (1<<tstatus->def_ele) ||
					sd->right_weapon.ignore_def_race & (1<<tstatus->race) ||
					sd->right_weapon.ignore_def_race & (is_boss(target)?1<<RC_BOSS:1<<RC_NONBOSS)
				))
					flag.idef = 1;

				if (!flag.idef2 && (
					sd->left_weapon.ignore_def_ele & (1<<tstatus->def_ele) ||
					sd->left_weapon.ignore_def_race & (1<<tstatus->race) ||
					sd->left_weapon.ignore_def_race & (is_boss(target)?1<<RC_BOSS:1<<RC_NONBOSS)
				)) {
						if(battle_config.left_cardfix_to_right && flag.rh) //Move effect to right hand. [Skotlex]
							flag.idef = 1;
						else
							flag.idef2 = 1;
				}
			}
		}

		if( !flag.idef || !flag.idef2 ) {
			//Defense reduction
			short vit_def;
			defType def1 = status_get_def(target); //Don't use tstatus->def1 due to skill timer reductions.
			short def2 = tstatus->def2;
#ifdef RENEWAL
			if( tsc && tsc->data[SC_ASSUMPTIO] )
				def1 <<= 1; // only eDEF is doubled
#endif
			if( sd ) {
				int type;
				i = sd->ignore_def[is_boss(target)?RC_BOSS:RC_NONBOSS];
				i += sd->ignore_def[tstatus->race];
				ARR_FIND(1, 6, type, sd->talisman[type] > 0);
				if( i ) {
					if( i > 100 ) i = 100;
					def1 -= def1 * i / 100;
					def2 -= def2 * i / 100;
				}
				if( type == 2 ) {
					def1 += def1 * (5 * sd->talisman[type]) / 100; //+5% eDEF
				}
			}

			if( sc && sc->data[SC_EXPIATIO] ) {
				i = 5 * sc->data[SC_EXPIATIO]->val1; // 5% per level
				def1 -= def1 * i / 100;
				def2 -= def2 * i / 100;
			}

			if( tsc && tsc->data[SC_GT_REVITALIZE] && tsc->data[SC_GT_REVITALIZE]->val2 )
				def2 += tsc->data[SC_GT_REVITALIZE]->val4;

			if( tsc && tsc->data[SC_CAMOUFLAGE] ) {
				i = 5 * (10-tsc->data[SC_CAMOUFLAGE]->val4);
				def1 -= def1 * i / 100;
				def2 -= def2 * i / 100;
			}

			if( battle_config.vit_penalty_type && battle_config.vit_penalty_target&target->type ) {
				unsigned char target_count; //256 max targets should be a sane max
				target_count = unit_counttargeted(target);
				if( target_count >= battle_config.vit_penalty_count ) {
					if( battle_config.vit_penalty_type == 1 ) {
						if( !tsc || !tsc->data[SC_STEELBODY] )
							def1 = (def1 * (100 - (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num))/100;
						def2 = (def2 * (100 - (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num))/100;
					} else { //Assume type 2
						if( !tsc || !tsc->data[SC_STEELBODY] )
							def1 -= (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num;
						def2 -= (target_count - (battle_config.vit_penalty_count - 1))*battle_config.vit_penalty_num;
					}
				}
#ifdef RENEWAL
				if( skill_id == AM_ACIDTERROR ) def2 = 0; //Acid Terror ignores only status defense. [FatalEror]
#else
				if( skill_id == AM_ACIDTERROR ) def1 = 0; //Acid Terror ignores only armor defense. [Skotlex]
#endif
				if( def2 < 1 ) def2 = 1;
			}
			//Vitality reduction from rodatazone: http://rodatazone.simgaming.net/mechanics/substats.php#def
			if( tsd ) {
				//Sd vit-eq
#ifndef RENEWAL
				//[VIT*0.5] + rnd([VIT*0.3], max([VIT*0.3],[VIT^2/150]-1))
				vit_def = def2*(def2-15)/150;
				vit_def = def2/2 + (vit_def>0?rnd()%vit_def:0);
#else
				vit_def = def2;
#endif
				if( (battle_check_undead(sstatus->race,sstatus->def_ele) || sstatus->race==RC_DEMON) && //This bonus already doesnt work vs players
					src->type == BL_MOB && (skill=pc_checkskill(tsd,AL_DP)) > 0 )
					vit_def += skill*(int)(3 +(tsd->status.base_level+1)*0.04);   // submitted by orn
				if( src->type == BL_MOB && (skill=pc_checkskill(tsd,RA_RANGERMAIN))>0 &&
					(sstatus->race == RC_BRUTE || sstatus->race == RC_FISH || sstatus->race == RC_PLANT) )
					vit_def += skill*5;
#ifdef RENEWAL
				if( skill == NJ_ISSEN ) {
					//TODO: do better implementation if other skills(same func) are found [malufett]
					vit_def += def1;
					def1 = 0;
				}
#endif
			} else {
				//Mob-Pet vit-eq
#ifndef RENEWAL
				//VIT + rnd(0,[VIT/20]^2-1)
				vit_def = (def2/20)*(def2/20);
				vit_def = def2 + (vit_def>0?rnd()%vit_def:0);
#else
				vit_def = def2;
#endif
			}

			if( battle_config.weapon_defense_type ) {
				vit_def += def1*battle_config.weapon_defense_type;
				def1 = 0;
			}

#ifdef RENEWAL
			/**
			* RE DEF Reduction
			* Damage = Attack * (4000+eDEF)/(4000+eDEF) - sDEF
			* Pierce defence gains 1 atk per def/2
			**/

			/* being hit by a gazillion units, you hit the jackpot and got -400
			which creates a division by 0 and subsequently crashes */
			if( def1 == -400 )
				def1 = -399;

			ATK_ADD2(
				flag.pdef ?(def1/2):0,
				flag.pdef2?(def1/2):0
			);
			if( !flag.idef && !flag.pdef )
				wd.damage = wd.damage * (4000+def1) / (4000+10*def1) - vit_def;
			if( flag.lh && !flag.idef2 && !flag.pdef2 )
				wd.damage2 = wd.damage2 * (4000+def1) / (4000+10*def1) - vit_def;
#else
				if (def1 > 100) def1 = 100;
				ATK_RATE2(
					flag.idef ?100:(flag.pdef ? flag.pdef*(def1+vit_def) : (100-def1)),
			 		flag.idef2?100:(flag.pdef2? flag.pdef2*(def1+vit_def) : (100-def1))
				);
				ATK_ADD2(
					flag.idef ||flag.pdef ?0:-vit_def,
					flag.idef2||flag.pdef2?0:-vit_def
				);
#endif
		}

		//Post skill/vit reduction damage increases
		if( sc ) {
			//SC skill damages
			if(sc->data[SC_AURABLADE]
#ifndef RENEWAL
					&& skill_id != LK_SPIRALPIERCE && skill_id != ML_SPIRALPIERCE
#endif
			) {
				int lv = sc->data[SC_AURABLADE]->val1;
#ifdef RENEWAL
				lv *= ((skill_id == LK_SPIRALPIERCE || skill_id == ML_SPIRALPIERCE)?wd.div_:1); // +100 per hit in lv 5
#endif
				ATK_ADD(20*lv);
			}

			if(sc->data[SC_GN_CARTBOOST])
				ATK_ADD(10*sc->data[SC_GN_CARTBOOST]->val1);

			if(sc->data[SC_GT_CHANGE] && sc->data[SC_GT_CHANGE]->val2) {
				struct block_list *bl;
				// ATK increase: ATK [{(Caster DEX / 4) + (Caster STR / 2)} x Skill Level / 5]
				if( (bl = map_id2bl(sc->data[SC_GT_CHANGE]->val2)) )
					ATK_ADD( ( status_get_dex(bl)/4 + status_get_str(bl)/2 ) * sc->data[SC_GT_CHANGE]->val1 / 5 );
			}

			if(sc->data[SC_CAMOUFLAGE])
				ATK_ADD(30 * (10-sc->data[SC_CAMOUFLAGE]->val4) );
		}

		if( sd ) {
			ARR_FIND(1, 6, i, sd->talisman[i] > 0);
			if( i == 2 ) {
				ATK_ADDRATE(15 * sd->talisman[i]); //+15% Weapon Attack
			}
		}

		//Refine bonus
		if( sd && flag.weapon && skill_id != MO_INVESTIGATE && skill_id != MO_EXTREMITYFIST ) {
			// Counts refine bonus multiple times
			if( skill_id == MO_FINGEROFFENSIVE ) {
				ATK_ADD2(wd.div_*sstatus->rhw.atk2, wd.div_*sstatus->lhw.atk2);
			} else {
				ATK_ADD2(sstatus->rhw.atk2, sstatus->lhw.atk2);
			}
		}

		//Set to min of 1
		if (flag.rh && wd.damage < 1) wd.damage = 1;
		if (flag.lh && wd.damage2 < 1) wd.damage2 = 1;

		if (sd && flag.weapon &&
			skill_id != MO_INVESTIGATE &&
		  	skill_id != MO_EXTREMITYFIST &&
		  	skill_id != CR_GRANDCROSS) {
			//Add mastery damage
			if(skill_id != ASC_BREAKER && sd->status.weapon == W_KATAR &&
				(skill = pc_checkskill(sd,ASC_KATAR)) > 0) {
				//Adv Katar Mastery is does not applies to ASC_BREAKER,
				//but other masteries DO apply >_>
				ATK_ADDRATE(10+2*skill);
			}

			wd.damage = battle_addmastery(sd,target,wd.damage,0);
			if (flag.lh)
				wd.damage2 = battle_addmastery(sd,target,wd.damage2,1);

			if (sc && sc->data[SC_MIRACLE]) i = 2; //Star anger
			else
				ARR_FIND(0, MAX_PC_FEELHATE, i, t_class == sd->hate_mob[i]);
			if (i < MAX_PC_FEELHATE && (skill = pc_checkskill(sd,sg_info[i].anger_id))) {
				skillratio = sd->status.base_level + sstatus->dex + sstatus->luk;
				if (i == 2) skillratio += sstatus->str; //Star Anger
				if (skill<4)
					skillratio /= 12-3*skill;
				ATK_ADDRATE(skillratio);
			}
			if (skill_id == NJ_SYURIKEN && (skill = pc_checkskill(sd,NJ_TOBIDOUGU)) > 0)
				ATK_ADD(3*skill);
			if (skill_id == NJ_KUNAI)
				ATK_ADD(60);
		} //Here ends flag.hit section, the rest of the function applies to both hitting and missing attacks
	} else if (wd.div_ < 0) //Since the attack missed...
		wd.div_ *= -1;

	if (sd && (skill=pc_checkskill(sd,BS_WEAPONRESEARCH)) > 0)
		ATK_ADD(skill*2);

	if (skill_id == TF_POISON)
		ATK_ADD(15*skill_lv);

	if( !(nk&NK_NO_ELEFIX) && !n_ele ) {
		//Elemental attribute fix
		if( wd.damage > 0 ) {
			wd.damage = battle_attr_fix(src,target,wd.damage,s_ele,tstatus->def_ele, tstatus->ele_lv);
			if( skill_id == MC_CARTREVOLUTION ) //Cart Revolution applies the element fix once more with neutral element
				wd.damage = battle_attr_fix(src,target,wd.damage,ELE_NEUTRAL,tstatus->def_ele, tstatus->ele_lv);
			if( skill_id == GS_GROUNDDRIFT ) //Additional 50*lv Neutral damage.
				wd.damage += battle_attr_fix(src,target,50*skill_lv,ELE_NEUTRAL,tstatus->def_ele, tstatus->ele_lv);
		}
		if( flag.lh && wd.damage2 > 0 )
			wd.damage2 = battle_attr_fix(src,target,wd.damage2,s_ele_,tstatus->def_ele, tstatus->ele_lv);
		if( sc && sc->data[SC_WATK_ELEMENT] ) {
			// Descriptions indicate this means adding a percent of a normal attack in another element. [Skotlex]
			int damage = battle_calc_base_damage(sstatus, &sstatus->rhw, sc, tstatus->size, sd, (flag.arrow?2:0)) * sc->data[SC_WATK_ELEMENT]->val2 / 100;
			wd.damage += battle_attr_fix(src, target, damage, sc->data[SC_WATK_ELEMENT]->val1, tstatus->def_ele, tstatus->ele_lv);

			if( flag.lh ) {
				damage = battle_calc_base_damage(sstatus, &sstatus->lhw, sc, tstatus->size, sd, (flag.arrow?2:0)) * sc->data[SC_WATK_ELEMENT]->val2 / 100;
				wd.damage2 += battle_attr_fix(src, target, damage, sc->data[SC_WATK_ELEMENT]->val1, tstatus->def_ele, tstatus->ele_lv);
			}
		}
#ifdef RENEWAL
		/**
		 * In RE Shield Bommerang takes weapon element only for damage calculation,
		 * - resist calculation is always against neutral
		 **/
		if ( skill_id == CR_SHIELDBOOMERANG )
			s_ele = s_ele_ = ELE_NEUTRAL;
#endif
	}

	if( skill_id == CR_GRANDCROSS || skill_id == NPC_GRANDDARKNESS )
		return wd; //Enough, rest is not needed.

	if( sd ) {
		if( skill_id != CR_SHIELDBOOMERANG ) //Only Shield boomerang doesn't takes the Star Crumbs bonus.
			ATK_ADD2(wd.div_*sd->right_weapon.star, wd.div_*sd->left_weapon.star);
		if( skill_id == MO_FINGEROFFENSIVE ) { //The finger offensive spheres on moment of attack do count. [Skotlex]
			ATK_ADD(wd.div_*sd->spiritball_old*3);
		} else {
			ATK_ADD(wd.div_*sd->spiritball*3);
		}

		//Card Fix for attacker (sd), 2 is added to the "left" flag meaning "attacker cards only"
		wd.damage = battle_calc_cardfix(BF_WEAPON, src, target, nk, s_ele, s_ele_, wd.damage, 2, wd.flag);
		if( flag.lh )
			wd.damage2 = battle_calc_cardfix(BF_WEAPON, src, target, nk, s_ele, s_ele_, wd.damage2, 3, wd.flag);
#ifdef RENEWAL
		if( flag.cri )
			ATK_ADDRATE(sd->bonus.crit_atk_rate>=100?sd->bonus.crit_atk_rate-60:40);
#endif
		if( skill_id == CR_SHIELDBOOMERANG || skill_id == PA_SHIELDCHAIN ) {
			//Refine bonus applies after cards and elements.
			short index = sd->equip_index[EQI_HAND_L];
			if( index >= 0 && sd->inventory_data[index] && sd->inventory_data[index]->type == IT_ARMOR )
				ATK_ADD(10*sd->status.inventory[index].refine);
		}
	}

	if( !sd && tsd ) //Card Fix for target (tsd), 2 is not added to the "left" flag meaning "target cards only"
		wd.damage = battle_calc_cardfix(BF_WEAPON, src, target, nk, s_ele, s_ele_, wd.damage, flag.lh, wd.flag);

	if( flag.infdef ) { //Plants receive 1 damage when hit
		short class_ = status_get_class(target);
		if( flag.hit || wd.damage > 0 )
			wd.damage = wd.div_; // In some cases, right hand no need to have a weapon to increase damage
		if( flag.lh && (flag.hit || wd.damage2 > 0) )
			wd.damage2 = wd.div_;
		if( flag.hit && class_ == MOBID_EMPERIUM ) {
			if(wd.damage2 > 0) {
				wd.damage2 = battle_attr_fix(src,target,wd.damage2,s_ele_,tstatus->def_ele, tstatus->ele_lv);
				wd.damage2 = battle_calc_gvg_damage(src,target,wd.damage2,wd.div_,skill_id,skill_lv,wd.flag);
			} else if(wd.damage > 0) {
				wd.damage = battle_attr_fix(src,target,wd.damage,s_ele_,tstatus->def_ele, tstatus->ele_lv);
				wd.damage = battle_calc_gvg_damage(src,target,wd.damage,wd.div_,skill_id,skill_lv,wd.flag);
			}
			return wd;
		}
		if( !(battle_config.skill_min_damage&1) )
			//Do not return if you are supposed to deal greater damage to plants than 1. [Skotlex]
			return wd;
	}

	if (sd) {
		if (!flag.rh && flag.lh) { //Move lh damage to the rh
			wd.damage = wd.damage2;
			wd.damage2 = 0;
			flag.rh = 1;
			flag.lh = 0;
		} else if (flag.rh && flag.lh) { //Dual-wield
			if (wd.damage) {
				if ((skill = pc_checkskill(sd,AS_RIGHT)))
					ATK_RATER(50 + (skill * 10))
				else if ((skill = pc_checkskill(sd,KO_RIGHT)))
					ATK_RATER(70 + (skill * 10))
				if(wd.damage < 1) wd.damage = 1;
			}
			if (wd.damage2) {
				if ((skill = pc_checkskill(sd,AS_LEFT)))
					ATK_RATEL(30 + (skill * 10))
				else if ((skill = pc_checkskill(sd,KO_LEFT)))
					ATK_RATEL(50 + (skill * 10))
				if (wd.damage2 < 1) wd.damage2 = 1;
			}
		} else if (sd->status.weapon == W_KATAR && !skill_id) {
			//Katars (offhand damage only applies to normal attacks, tested on Aegis 10.2)
			skill = pc_checkskill(sd,TF_DOUBLE);
			wd.damage2 = wd.damage * (1 + (skill * 2))/100;

			if (wd.damage && !wd.damage2) wd.damage2 = 1;
			flag.lh = 1;
		}
	}

	if(!flag.rh && wd.damage)
		wd.damage = 0;

	if(!flag.lh && wd.damage2)
		wd.damage2 = 0;

	if(wd.damage + wd.damage2) { //There is a total damage value
		if(!wd.damage2) {
			wd.damage = battle_calc_damage(src,target,&wd,wd.damage,skill_id,skill_lv);
			if(map_flag_gvg2(target->m))
				wd.damage=battle_calc_gvg_damage(src,target,wd.damage,wd.div_,skill_id,skill_lv,wd.flag);
			else if(map[target->m].flag.battleground)
				wd.damage=battle_calc_bg_damage(src,target,wd.damage,wd.div_,skill_id,skill_lv,wd.flag);
		} else if(!wd.damage) {
			wd.damage2 = battle_calc_damage(src,target,&wd,wd.damage2,skill_id,skill_lv);
			if(map_flag_gvg2(target->m))
				wd.damage2 = battle_calc_gvg_damage(src,target,wd.damage2,wd.div_,skill_id,skill_lv,wd.flag);
			else if(map[target->m].flag.battleground)
				wd.damage = battle_calc_bg_damage(src,target,wd.damage2,wd.div_,skill_id,skill_lv,wd.flag);
		} else {
			int d1 = wd.damage + wd.damage2, d2 = wd.damage2;
			wd.damage = battle_calc_damage(src,target,&wd,d1,skill_id,skill_lv);
			if(map_flag_gvg2(target->m))
				wd.damage = battle_calc_gvg_damage(src,target,wd.damage,wd.div_,skill_id,skill_lv,wd.flag);
			else if(map[target->m].flag.battleground)
				wd.damage = battle_calc_bg_damage(src,target,wd.damage,wd.div_,skill_id,skill_lv,wd.flag);
			wd.damage2 = d2 * 100 / d1 * wd.damage / 100;
			if(wd.damage > 1 && wd.damage2 < 1) wd.damage2 = 1;
			wd.damage -= wd.damage2;
		}
	}

#ifdef RENEWAL
	if(skill_id == AM_ACIDTERROR) {
		struct Damage ad = battle_calc_magic_attack(src, target, skill_id, skill_lv, wflag);
		wd.damage += ad.damage;
	}
#endif

	//Reject Sword bugreport:4493 by Daegaladh
	if(wd.damage && tsc && tsc->data[SC_REJECTSWORD] &&
		(src->type!=BL_PC || (
			((TBL_PC *)src)->weapontype1 == W_DAGGER ||
			((TBL_PC *)src)->weapontype1 == W_1HSWORD ||
			((TBL_PC *)src)->status.weapon == W_2HSWORD
		)) &&
		rnd()%100 < tsc->data[SC_REJECTSWORD]->val2
		) {
		ATK_RATER(50)
		status_fix_damage(target,src,wd.damage,clif_damage(target,src,gettick(),0,0,wd.damage,0,0,0));
		clif_skill_nodamage(target,target,ST_REJECTSWORD,tsc->data[SC_REJECTSWORD]->val1,1);
		if( --(tsc->data[SC_REJECTSWORD]->val3) <= 0 )
			status_change_end(target, SC_REJECTSWORD, INVALID_TIMER);
	}

	if( sc ) {
		//SG_FUSION hp penalty [Komurka]
		if( sc->data[SC_FUSION] ) {
			int hp= sstatus->max_hp;
			if( sd && tsd ) {
				hp = 8*hp/100;
				if ((sstatus->hp*100) <= (sstatus->max_hp*20))
					hp = sstatus->hp;
			} else
				hp = 2*hp/100; //2% hp loss per hit
			status_zap(src, hp, 0);
		}
		/**
		 * affecting non-skills
		 **/
		if( !skill_id ) {
			if( sc->data[SC_ENCHANTBLADE] && sd && ( (flag.rh && sd->weapontype1) || (flag.lh && sd->weapontype2) ) ) {
				//[( ( Skill Lv x 20 ) + 100 ) x ( casterBaseLevel / 150 )] + casterInt
				ATK_ADD( ( sc->data[SC_ENCHANTBLADE]->val1*20+100 )*status_get_lv(src)/150 + status_get_int(src) );
			}
		}
		status_change_end(src,SC_CAMOUFLAGE, INVALID_TIMER);
	}

	if( skill_id == LG_RAYOFGENESIS ) {
		struct Damage ad = battle_calc_magic_attack(src, target, skill_id, skill_lv, wflag);
		wd.damage += ad.damage;
	} else if( skill_id == ASC_BREAKER ) {
		struct Damage md =
#ifdef RENEWAL
		battle_calc_magic_attack(src, target, skill_id, skill_lv, wflag)
#else
		battle_calc_misc_attack(src, target, skill_id, skill_lv, wflag)
#endif
		;
		wd.damage += md.damage;
	}

	return wd;
}

/*==========================================
 * battle_calc_magic_attack [DracoRPG]
 *------------------------------------------*/
struct Damage battle_calc_magic_attack(struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv,int mflag)
{
	int i, nk;
	short s_ele = 0;
	unsigned int skillratio = 100;	//Skill dmg modifiers.

	TBL_PC *sd;
//	TBL_PC *tsd;
	struct status_change *sc, *tsc;
	struct Damage ad;
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);
	struct {
		unsigned imdef : 1;
		unsigned infdef : 1;
	} flag;

	memset(&ad,0,sizeof(ad));
	memset(&flag,0,sizeof(flag));

	if(src==NULL || target==NULL) {
		nullpo_info(NLP_MARK);
		return ad;
	}
	//Initial Values
	ad.damage = 1;
	ad.div_ = skill_get_num(skill_id,skill_lv);
	ad.amotion = skill_get_inf(skill_id)&INF_GROUND_SKILL?0:sstatus->amotion; //Amotion should be 0 for ground skills.
	ad.dmotion = tstatus->dmotion;
	ad.blewcount = skill_get_blewcount(skill_id,skill_lv);
	ad.flag = BF_MAGIC|BF_SKILL;
	ad.dmg_lv = ATK_DEF;
	nk = skill_get_nk(skill_id);
	flag.imdef = nk&NK_IGNORE_DEF?1:0;

	sd = BL_CAST(BL_PC, src);
//	tsd = BL_CAST(BL_PC, target);
	sc = status_get_sc(src);
	tsc = status_get_sc(target);

	//Initialize variables that will be used afterwards
	s_ele = skill_get_ele(skill_id,skill_lv);

	if (s_ele == -1) { // pl=-1 : the skill takes the weapon's element
		s_ele = sstatus->rhw.ele;
		if( sd ) { //Summoning 10 talisman will endow your weapon
			ARR_FIND(1, 6, i, sd->talisman[i] >= 10);
			if( i < 5 ) s_ele = i;
		}
	} else if (s_ele == -2) //Use status element
		s_ele = status_get_attack_sc_element(src,status_get_sc(src));
	else if( s_ele == -3 ) //Use random element
		s_ele = rnd()%ELE_MAX;

	if( skill_id == SO_PSYCHIC_WAVE ) {
		if( sc && sc->count ) {
			if( sc->data[SC_HEATER_OPTION] ) s_ele = sc->data[SC_HEATER_OPTION]->val4;
			else if( sc->data[SC_COOLER_OPTION] ) s_ele = sc->data[SC_COOLER_OPTION]->val4;
			else if( sc->data[SC_BLAST_OPTION] ) s_ele = sc->data[SC_BLAST_OPTION]->val3;
			else if( sc->data[SC_CURSED_SOIL_OPTION] ) s_ele = sc->data[SC_CURSED_SOIL_OPTION]->val4;
		}
	}

	//Set miscellaneous data that needs be filled
	if(sd) {
		sd->state.arrow_atk = 0;
		ad.blewcount += battle_blewcount_bonus(sd, skill_id);
	}

	//Skill Range Criteria
	ad.flag |= battle_range_type(src, target, skill_id, skill_lv);
	flag.infdef=(tstatus->mode&MD_PLANT?1:0);
	if( target->type == BL_SKILL) {
		TBL_SKILL *su = (TBL_SKILL*)target;
		if( su->group && (su->group->skill_id == WM_REVERBERATION || su->group->skill_id == WM_POEMOFNETHERWORLD) )
			flag.infdef = 1;
	}

	switch(skill_id) {
		case MG_FIREWALL:
		case NJ_KAENSIN:
			ad.dmotion = 0; //No flinch animation.
			if ( tstatus->def_ele == ELE_FIRE || battle_check_undead(tstatus->race, tstatus->def_ele) )
				ad.blewcount = 0; //No knockback
			break;
		case PR_SANCTUARY:
			ad.dmotion = 0; //No flinch animation.
			break;
	}

	if (!flag.infdef) { //No need to do the math for plants
#ifdef RENEWAL
	ad.damage = 0; //reinitialize..
#endif
//MATK_RATE scales the damage. 100 = no change. 50 is halved, 200 is doubled, etc
#define MATK_RATE( a ) { ad.damage = ad.damage*(a)/100; }
//Adds dmg%. 100 = +100% (double) damage. 10 = +10% damage
#define MATK_ADDRATE( a ) { ad.damage += ad.damage*(a)/100; }
//Adds an absolute value to damage. 100 = +100 damage
#define MATK_ADD( a ) { ad.damage += a; }

		switch (skill_id) { //Calc base damage according to skill
			case AL_HEAL:
			case PR_BENEDICTIO:
			case PR_SANCTUARY:
			case AB_HIGHNESSHEAL:
				ad.damage = skill_calc_heal(src, target, (skill_id == AB_HIGHNESSHEAL)?AL_HEAL:skill_id, (skill_id == AB_HIGHNESSHEAL)?10:skill_lv, false);
				if( skill_id == AB_HIGHNESSHEAL )
					ad.damage = ad.damage * ( 17 + 3 * skill_lv ) / 10;
				break;
			case PR_ASPERSIO:
				ad.damage = 40;
				break;
			case ALL_RESURRECTION:
			case PR_TURNUNDEAD:
				//Undead check is on skill_castend_damageid code.
#ifdef RENEWAL
				i = 10*skill_lv + sstatus->luk + sstatus->int_ + status_get_lv(src)
				  	+ 300 - 300*tstatus->hp/tstatus->max_hp;
#else
				i = 20*skill_lv + sstatus->luk + sstatus->int_ + status_get_lv(src)
				  	+ 200 - 200*tstatus->hp/tstatus->max_hp;
#endif
				if(i > 700) i = 700;
				if(rnd()%1000 < i && !(tstatus->mode&MD_BOSS))
					ad.damage = tstatus->hp;
				else {
#ifdef RENEWAL
					if (sstatus->matk_max > sstatus->matk_min) {
						MATK_ADD(sstatus->matk_min + rnd()%(sstatus->matk_max - sstatus->matk_min));
					} else {
						MATK_ADD(sstatus->matk_min);
					}
					MATK_RATE(skill_lv);
#else
					ad.damage = status_get_lv(src) + sstatus->int_ + skill_lv * 10;
#endif
				}
				break;
#ifdef RENEWAL
			case ASC_BREAKER:
				ad.damage = (sstatus->matk_max + sstatus->matk_min) * ((skill_lv / 2) + 3);
				break;
#endif
			case PF_SOULBURN:
				ad.damage = tstatus->sp * 2;
				break;
			case AB_RENOVATIO:
				ad.damage = status_get_lv(src) * 10 + sstatus->int_;
				break;
			default: {
				if (sstatus->matk_max > sstatus->matk_min) {
					MATK_ADD(sstatus->matk_min + rnd()%(sstatus->matk_max - sstatus->matk_min));
				} else {
					MATK_ADD(sstatus->matk_min);
				}
				if(nk&NK_SPLASHSPLIT) { // Divide MATK in case of multiple targets skill
					if(mflag>0)
						ad.damage /= mflag;
					else
						ShowError("0 enemies targeted by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
				}

				switch(skill_id) {
					case MG_NAPALMBEAT:
						skillratio += skill_lv*10-30;
						break;
					case MG_FIREBALL:
#ifdef RENEWAL
						skillratio += 20*skill_lv;
#else
						skillratio += skill_lv*10-30;
#endif
						break;
					case MG_SOULSTRIKE:
						if (battle_check_undead(tstatus->race,tstatus->def_ele))
							skillratio += 5*skill_lv;
						break;
					case MG_FIREWALL:
						skillratio -= 50;
						break;
					case MG_FIREBOLT:
					case MG_COLDBOLT:
					case MG_LIGHTNINGBOLT:
						if ( sc && sc->data[SC_SPELLFIST] && mflag&BF_SHORT )  {
							// val4 = used bolt level, val2 = used spellfist level. [Rytech]
							skillratio += (sc->data[SC_SPELLFIST]->val4 * 100) + (sc->data[SC_SPELLFIST]->val2 * 50) - 100;
							ad.div_ = 1; // ad mods, to make it work similar to regular hits [Xazax]
							ad.flag = BF_WEAPON|BF_SHORT;
							ad.type = 0;
						}
						break;
					case MG_THUNDERSTORM:
						 //In Renewal Thunder Storm boost is 100% (in pre-re, 80%)
#ifndef RENEWAL
							skillratio -= 20;
#endif
						break;
					case MG_FROSTDIVER:
						skillratio += 10*skill_lv;
						break;
					case AL_HOLYLIGHT:
						skillratio += 25;
						if (sd && sd->sc.data[SC_SPIRIT] && sd->sc.data[SC_SPIRIT]->val2 == SL_PRIEST)
							skillratio *= 5; //Does 5x damage include bonuses from other skills?
						break;
					case AL_RUWACH:
						skillratio += 45;
						break;
					case WZ_FROSTNOVA:
						skillratio += (100+skill_lv*10)*2/3-100;
						break;
					case WZ_FIREPILLAR:
						if (skill_lv > 10)
							skillratio += 100;
						else
							skillratio -= 80;
						break;
					case WZ_SIGHTRASHER:
						skillratio += 20*skill_lv;
						break;
					case WZ_WATERBALL:
						skillratio += 30*skill_lv;
						break;
					case WZ_STORMGUST:
						skillratio += 40*skill_lv;
						break;
					case HW_NAPALMVULCAN:
						skillratio += 10*skill_lv-30;
						break;
					case SL_STIN:
						skillratio += (tstatus->size!=SZ_SMALL?-99:10*skill_lv); //target size must be small (0) for full damage.
						break;
					case SL_STUN:
						skillratio += (tstatus->size!=SZ_BIG?5*skill_lv:-99); //Full damage is dealt on small/medium targets
						break;
					case SL_SMA:
						skillratio += -60 + status_get_lv(src); //Base damage is 40% + lv%
						break;
					case NJ_KOUENKA:
						skillratio -= 10;
						break;
					case NJ_KAENSIN:
						skillratio -= 50;
						break;
					case NJ_BAKUENRYU:
						skillratio += 50*(skill_lv-1);
						break;
					case NJ_HYOUSYOURAKU:
						skillratio += 50*skill_lv;
						break;
					case NJ_RAIGEKISAI:
						skillratio += 60 + 40*skill_lv;
						break;
					case NJ_KAMAITACHI:
					case NPC_ENERGYDRAIN:
						skillratio += 100*skill_lv;
						break;
					case NPC_EARTHQUAKE:
						skillratio += 100 +100*skill_lv +100*(skill_lv/2);
						break;
#ifdef RENEWAL
					case WZ_HEAVENDRIVE:
					case WZ_METEOR:
						skillratio += 25;
						break;
					case WZ_VERMILION: {
    						int interval = 0, per = interval, ratio = per;
    						while( (per++) < skill_lv ) {
     							ratio += interval;
     							if(per%3==0) interval += 20;
    						}
							if( skill_lv > 9 )
								ratio -= 10;
							skillratio += ratio;
						}
						break;
					case NJ_HUUJIN:
						skillratio += 50;
						break;
#else
					case WZ_VERMILION:
						skillratio += 20 * skill_lv - 20;
						break;
#endif
					case AB_JUDEX:
						skillratio += 200 + 20 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case AB_ADORAMUS:
						skillratio += 400 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case AB_DUPLELIGHT_MAGIC:
						skillratio += 100 + 20 * skill_lv;
						break;
					case WL_SOULEXPANSION:
						skillratio = (skill_lv + 4 ) * 100 + sstatus->int_;
						RE_LVL_DMOD(100);
						if( tsc && tsc->data[SC_WHITEIMPRISON] )
							skillratio <<= 1;
						break;
					case WL_FROSTMISTY:
						skillratio += 100 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case WL_JACKFROST:
						if( tsc && tsc->data[SC_FREEZING] ) {
							skillratio += 900 + 300 * skill_lv;
							RE_LVL_DMOD(100);
						} else {
							skillratio += 400 + 100 * skill_lv;
							RE_LVL_DMOD(150);
						}
						break;
					case WL_DRAINLIFE:
						skillratio = 200 * skill_lv + sstatus->int_;
						RE_LVL_DMOD(100);
						break;
					case WL_CRIMSONROCK:
						skillratio += 1200 + 300 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case WL_HELLINFERNO:
						skillratio = 300 * skill_lv;
						RE_LVL_DMOD(100);
						// Shadow: MATK [{( Skill Level x 300 ) x ( Caster Base Level / 100 ) x 4/5 }] %
						// Fire : MATK [{( Skill Level x 300 ) x ( Caster Base Level / 100 ) /5 }] %
						if( mflag&ELE_DARK ){ skillratio *= 4; s_ele = ELE_DARK; }
						skillratio /= 5;
						break;
					case WL_COMET: {
							struct status_change * sc = status_get_sc(src);
							if( sc )
								i = distance_xy(target->x, target->y, sc->comet_x, sc->comet_y);
							else
								i = 8;
							if( i < 4 ) skillratio = 2500 + 500 * skill_lv;
							else
							if( i < 6 ) skillratio = 2000 + 500 * skill_lv;
							else
							if( i < 8 ) skillratio = 1500 + 500 * skill_lv;
							else
							skillratio = 1000 + 500 * skill_lv;
						}
						break;
					case WL_CHAINLIGHTNING_ATK:
						skillratio += 400 + 100 * skill_lv + 500;
						RE_LVL_DMOD(100);
						break;
					case WL_EARTHSTRAIN:
						skillratio += 1900 + 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case WL_TETRAVORTEX_FIRE:
					case WL_TETRAVORTEX_WATER:
					case WL_TETRAVORTEX_WIND:
					case WL_TETRAVORTEX_GROUND:
						skillratio += 400 + 500 * skill_lv;
						break;
					case WL_SUMMON_ATK_FIRE:
					case WL_SUMMON_ATK_WATER:
					case WL_SUMMON_ATK_WIND:
					case WL_SUMMON_ATK_GROUND:
						skillratio = (1 + skill_lv) / 2 * (status_get_lv(src) + ( sd ? sd->status.job_level : 0 ));
						RE_LVL_DMOD(100);
						break;
					case LG_RAYOFGENESIS: {
							int16 lv = skill_lv;
							int bandingBonus = 0;
							if( sc && sc->data[SC_BANDING] )
								bandingBonus = 200 * (sd ? skill_check_pc_partner(sd,skill_id,&lv,skill_get_splash(skill_id,skill_lv),0) : 1);
							skillratio = ((300 * skill_lv) + bandingBonus) * (sd ? sd->status.job_level / 25 : 1);
						}
						break;
					case LG_SHIELDSPELL: // [(Caster Base Level x 4) + (Shield MDEF x 100) + (Caster INT x 2)] %
						if( sd && skill_lv == 2 ) {
							s_ele = ELE_HOLY;
							skillratio = status_get_lv(src) * 4 + sd->bonus.shieldmdef * 100 + status_get_int(src) * 2;
						} else
							skillratio = 0;
						break;
					case WM_METALICSOUND:
						skillratio = 120 * skill_lv + 60 * pc_checkskill(sd, WM_LESSON);
						RE_LVL_DMOD(100);
						if( tsc && (tsc->data[SC_SLEEP] || tsc->data[SC_DEEPSLEEP]) )
							skillratio += skillratio / 2;
						break;
					case WM_REVERBERATION_MAGIC:
						// MATK [{(Skill Level x 100) + 100} x Caster Base Level / 100] %
						skillratio += 100 * skill_lv;
						RE_LVL_DMOD(100);
						break;
					case SO_FIREWALK:
						skillratio = 60 * skill_lv;
						RE_LVL_DMOD(100);
						if( sc && sc->data[SC_HEATER_OPTION] )
							skillratio += sc->data[SC_HEATER_OPTION]->val3 / 2;
						break;
					case SO_ELECTRICWALK:
						skillratio = 60 * skill_lv;
						RE_LVL_DMOD(100);
						if( sc && sc->data[SC_BLAST_OPTION] )
							skillratio += sc->data[SC_BLAST_OPTION]->val2 / 2;
						break;
					case SO_EARTHGRAVE:
						skillratio = sstatus->int_ * skill_lv + ( sd ? pc_checkskill(sd, SA_SEISMICWEAPON) * 200 : 0 );
						RE_LVL_DMOD(100);
						if( sc && sc->data[SC_CURSED_SOIL_OPTION] )
							skillratio += sc->data[SC_CURSED_SOIL_OPTION]->val3 * 5;
						break;
					case SO_DIAMONDDUST:
						skillratio = sstatus->int_ * skill_lv + ( sd ? pc_checkskill(sd, SA_FROSTWEAPON) * 200 : 0 );
						RE_LVL_DMOD(100);
						if( sc && sc->data[SC_COOLER_OPTION] )
							skillratio += sc->data[SC_COOLER_OPTION]->val3 * 5;
						break;
					case SO_POISON_BUSTER:
						skillratio += 900 + 300 * skill_lv;
						RE_LVL_DMOD(120);
						if( sc && sc->data[SC_CURSED_SOIL_OPTION] )
							skillratio += sc->data[SC_CURSED_SOIL_OPTION]->val3 * 5;
						break;
					case SO_PSYCHIC_WAVE:
						skillratio = 70 * skill_lv + 3 * sstatus->int_;
						RE_LVL_DMOD(100);
						if( sc )
							if( sc->data[SC_HEATER_OPTION] || sc->data[SC_COOLER_OPTION] ||
								sc->data[SC_BLAST_OPTION] || sc->data[SC_CURSED_SOIL_OPTION] )
								skillratio += 20;
						break;
					case SO_CLOUD_KILL:
						skillratio = 40 * skill_lv;
						RE_LVL_DMOD(100);
						if( sc && sc->data[SC_CURSED_SOIL_OPTION] )
							skillratio += sc->data[SC_CURSED_SOIL_OPTION]->val3;
						break;
					case SO_VARETYR_SPEAR: //MATK [{( Endow Tornado skill level x 50 ) + ( Caster INT x Varetyr Spear Skill level )} x Caster Base Level / 100 ] %
						skillratio = status_get_int(src) * skill_lv + ( sd ? pc_checkskill(sd, SA_LIGHTNINGLOADER) * 50 : 0 );
						RE_LVL_DMOD(100);
						if( sc && sc->data[SC_BLAST_OPTION] )
							skillratio += sc->data[SC_BLAST_OPTION]->val2 * 5;
						break;
					case GN_DEMONIC_FIRE:
						if ( skill_lv > 20 ) { // Fire Expansion Level 2
							skillratio += 10 + 20 * (skill_lv - 20) + 10 * sstatus->int_;
						} else if ( skill_lv > 10 ) { // Fire Expansion Level 1
							skillratio += 10 + 20 * (skill_lv - 10) + sstatus->int_;
							RE_LVL_DMOD(100);
						} else // Normal Demonic Fire Damage
							skillratio += 10 + 20 * skill_lv;
						break;
					// Magical Elemental Spirits Attack Skills
					case EL_FIRE_MANTLE:
					case EL_WATER_SCREW:
						skillratio += 900;
						break;
					case EL_FIRE_ARROW:
					case EL_ROCK_CRUSHER_ATK:
						skillratio += 200;
						break;
					case EL_FIRE_BOMB:
					case EL_ICE_NEEDLE:
					case EL_HURRICANE_ATK:
						skillratio += 400;
						break;
					case EL_FIRE_WAVE:
					case EL_TYPOON_MIS_ATK:
						skillratio += 1100;
						break;
					case MH_ERASER_CUTTER:
						if(skill_lv%2) skillratio += 400; //600:800:1000
						else skillratio += 700; //1000:1200
						skillratio += 100 * skill_lv;
						break;
					case MH_XENO_SLASHER:
						if(skill_lv%2) skillratio += 350 + 50 * skill_lv; //500:600:700
						else skillratio += 400 + 100 * skill_lv; //700:900
						break;
					case MH_HEILIGE_STANGE:
						skillratio += 400 + 250 * skill_lv;
						skillratio = (skillratio * status_get_lv(src)) / 150;
						break;
					case MH_POISON_MIST:
						skillratio += -100 + 40 * skill_lv * status_get_lv(src) / 100;
						break;
				}

				MATK_RATE(skillratio);

				//Constant/misc additions from skills
				if (skill_id == WZ_FIREPILLAR)
					MATK_ADD(50);
			}
		}
#ifdef RENEWAL
	ad.damage = battle_calc_cardfix(BF_MAGIC, src, target, nk, s_ele, 0, ad.damage, 0, ad.flag);
#endif
		if (sd) {
			//Damage bonuses
			if ((i = pc_skillatk_bonus(sd, skill_id)))
				ad.damage += ad.damage * i / 100;

			if( (i = battle_adjust_skill_damage(src->m,skill_id)) )
				MATK_RATE(i);

			//Ignore Magic Defense?
			if (!flag.imdef && (
				sd->bonus.ignore_mdef_ele & ( 1 << tstatus->def_ele ) ||
				sd->bonus.ignore_mdef_race & ( 1 << tstatus->race ) ||
				sd->bonus.ignore_mdef_race & ( is_boss(target) ? 1 << RC_BOSS : 1 << RC_NONBOSS )
			))
				flag.imdef = 1;
		}

		if (!flag.imdef) {
			defType mdef = tstatus->mdef;
			int mdef2 = tstatus->mdef2;
#ifdef RENEWAL
			if (tsc && tsc->data[SC_ASSUMPTIO])
				mdef <<= 1; // only eMDEF is doubled
#endif
			if (sd) {
				i = sd->ignore_mdef[is_boss(target)?RC_BOSS:RC_NONBOSS];
				i += sd->ignore_mdef[tstatus->race];
				if (i) {
					if (i > 100) i = 100;
					mdef -= mdef * i / 100;
					//mdef2 -= mdef2* i / 100;
				}
			}
#ifdef RENEWAL
			/**
			 * RE MDEF Reduction
			 * Damage = Magic Attack * (1000+eMDEF)/(1000+eMDEF) - sMDEF
			 **/
			ad.damage = ad.damage * (1000 + mdef) / (1000 + mdef * 10) - mdef2;
#else
			if (battle_config.magic_defense_type)
				ad.damage = ad.damage - mdef*battle_config.magic_defense_type - mdef2;
			else
				ad.damage = ad.damage * (100-mdef)/100 - mdef2;
#endif
		}
		
		if (skill_id == NPC_EARTHQUAKE) {
			//Adds atk2 to the damage, should be influenced by number of hits and skill-ratio, but not mdef reductions. [Skotlex]
			//Also divide the extra bonuses from atk2 based on the number in range [Kevin]
			if (mflag > 0)
				ad.damage += (sstatus->rhw.atk2*skillratio/100)/mflag;
			else
				ShowError("Zero range by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
		}

		if (ad.damage < 1)
			ad.damage = 1;
		else if (sc) { //only applies when hit
			// TODO: there is another factor that contribute with the damage and need to be formulated. [malufett]
			switch (skill_id) {
				case MG_LIGHTNINGBOLT:
				case MG_THUNDERSTORM:
				case MG_FIREBOLT:
				case MG_FIREWALL:
				case MG_COLDBOLT:
				case MG_FROSTDIVER:
				case WZ_EARTHSPIKE:
				case WZ_HEAVENDRIVE:
					if(sc->data[SC_GUST_OPTION] || sc->data[SC_PETROLOGY_OPTION] 
						|| sc->data[SC_PYROTECHNIC_OPTION] || sc->data[SC_AQUAPLAY_OPTION])
						ad.damage += (6 + sstatus->int_ / 4) + max(sstatus->dex - 10, 0) / 30;
					break;
			}
		}

		if (!(nk&NK_NO_ELEFIX))
			ad.damage = battle_attr_fix(src, target, ad.damage, s_ele, tstatus->def_ele, tstatus->ele_lv);

		if( skill_id == CR_GRANDCROSS || skill_id == NPC_GRANDDARKNESS ) {
			//Apply the physical part of the skill's damage. [Skotlex]
			struct Damage wd = battle_calc_weapon_attack(src,target,skill_id,skill_lv,mflag);
			ad.damage = battle_attr_fix(src, target, wd.damage + ad.damage, s_ele, tstatus->def_ele, tstatus->ele_lv) * (100 + 40*skill_lv)/100;
			if( src == target ) {
				if( src->type == BL_PC )
					ad.damage = ad.damage / 2;
				else
					ad.damage = 0;
			}
		}
#ifndef RENEWAL
	ad.damage = battle_calc_cardfix(BF_MAGIC, src, target, nk, s_ele, 0, ad.damage, 0, ad.flag);
#endif
	}

	damage_div_fix(ad.damage, ad.div_);

	if (flag.infdef && ad.damage)
		ad.damage = ad.damage>0?1:-1;

	ad.damage = battle_calc_damage(src,target,&ad,ad.damage,skill_id,skill_lv);
	if( map_flag_gvg2(target->m) )
		ad.damage = battle_calc_gvg_damage(src,target,ad.damage,ad.div_,skill_id,skill_lv,ad.flag);
	else if( map[target->m].flag.battleground )
		ad.damage = battle_calc_bg_damage(src,target,ad.damage,ad.div_,skill_id,skill_lv,ad.flag);

	switch( skill_id ) { /* post-calc modifiers */
		case SO_VARETYR_SPEAR: { // Physical damage.
			struct Damage wd = battle_calc_weapon_attack(src,target,skill_id,skill_lv,mflag);
			if (!flag.infdef && ad.damage > 1)
				ad.damage += wd.damage;
			break;
		}
		//case HM_ERASER_CUTTER:
	}

	return ad;
}

/*==========================================
 * Calculate Misc dammage for skill_id
 *------------------------------------------*/
struct Damage battle_calc_misc_attack(struct block_list *src,struct block_list *target,uint16 skill_id,uint16 skill_lv,int mflag)
{
	int skill;
	short i, nk;
	short s_ele;
	int chorusbonus = 0; //Chorus bonus value for chorus skills. Bonus remains 0 unless 3 or more Minstrel's/Wanderer's are in the party.

	struct map_session_data *sd, *tsd;
	struct Damage md; //DO NOT CONFUSE with md of mob_data!
	struct status_data *sstatus = status_get_status_data(src);
	struct status_data *tstatus = status_get_status_data(target);

	memset(&md,0,sizeof(md));

	if( src == NULL || target == NULL ) {
		nullpo_info(NLP_MARK);
		return md;
	}

	//Some initial values
	md.amotion = skill_get_inf(skill_id)&INF_GROUND_SKILL?0:sstatus->amotion;
	md.dmotion = tstatus->dmotion;
	md.div_ = skill_get_num( skill_id,skill_lv );
	md.blewcount = skill_get_blewcount(skill_id,skill_lv);
	md.dmg_lv = ATK_DEF;
	md.flag = BF_MISC|BF_SKILL;

	nk = skill_get_nk(skill_id);

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);

	// Minstrel/Wanderer number check for chorus skills.
	// Bonus remains 0 unless 3 or more Minstrel's/Wanderer's are in the party.
	if( sd && sd->status.party_id && party_foreachsamemap(party_sub_count_chorus, sd, 0) > 7)
		chorusbonus = 5;//Maximum effect possiable from 7 or more Minstrel's/Wanderer's
	else if( sd && sd->status.party_id && party_foreachsamemap(party_sub_count_chorus, sd, 0) > 2)
		//Effect bonus from additional Minstrel's/Wanderer's if not above the max possiable.
		chorusbonus = party_foreachsamemap(party_sub_count_chorus, sd, 0) - 2;

	if (sd) {
		sd->state.arrow_atk = 0;
		md.blewcount += battle_blewcount_bonus(sd, skill_id);
	}

	s_ele = skill_get_ele(skill_id, skill_lv);
	if (s_ele < 0 && s_ele != -3) //Attack that takes weapon's element for misc attacks? Make it neutral [Skotlex]
		s_ele = ELE_NEUTRAL;
	else if (s_ele == -3) //Use random element
		s_ele = rnd()%ELE_MAX;

	//Skill Range Criteria
	md.flag |= battle_range_type(src, target, skill_id, skill_lv);

	switch( skill_id ) {
#ifdef RENEWAL
		case HT_LANDMINE:
		case MA_LANDMINE:
		case HT_BLASTMINE:
		case HT_CLAYMORETRAP:
			md.damage = skill_lv * sstatus->dex * (3+status_get_lv(src)/100) * (1+sstatus->int_/35);
			md.damage += md.damage * (rnd()%20-10) / 100;
			md.damage += (sd ? pc_checkskill(sd,RA_RESEARCHTRAP) * 40 : 0);
			break;
#else
		case HT_LANDMINE:
		case MA_LANDMINE:
			md.damage = skill_lv*(sstatus->dex+75)*(100+sstatus->int_)/100;
			break;
		case HT_BLASTMINE:
			md.damage = skill_lv*(sstatus->dex/2+50)*(100+sstatus->int_)/100;
			break;
		case HT_CLAYMORETRAP:
			md.damage = skill_lv*(sstatus->dex/2+75)*(100+sstatus->int_)/100;
			break;
#endif
		case HT_BLITZBEAT:
		case SN_FALCONASSAULT:
			//Blitz-beat Damage.
			if(!sd || (skill = pc_checkskill(sd,HT_STEELCROW)) <= 0)
				skill = 0;
			md.damage = (sstatus->dex/10+sstatus->int_/2+skill*3+40)*2;
			if(mflag > 1) //Autocasted Blitz.
				nk |= NK_SPLASHSPLIT;

			if (skill_id == SN_FALCONASSAULT) {
				//Div fix of Blitzbeat
				skill = skill_get_num(HT_BLITZBEAT, 5);
				damage_div_fix(md.damage, skill);

				//Falcon Assault Modifier
				md.damage = md.damage*(150+70*skill_lv)/100;
			}
			break;
		case TF_THROWSTONE:
			md.damage = 50;
			break;
		case BA_DISSONANCE:
			md.damage = 30+skill_lv*10;
			if (sd)
				md.damage += 3*pc_checkskill(sd,BA_MUSICALLESSON);
			break;
		case NPC_SELFDESTRUCTION:
			md.damage = sstatus->hp;
			break;
		case NPC_SMOKING:
			md.damage = 3;
			break;
		case NPC_DARKBREATH:
			md.damage = 500 + (skill_lv-1)*1000 + rnd()%1000;
			if(md.damage > 9999) md.damage = 9999;
			break;
		case PA_PRESSURE:
			md.damage = 500+300*skill_lv;
			break;
		case PA_GOSPEL:
			md.damage = 1+rnd()%9999;
			break;
		case CR_ACIDDEMONSTRATION:
#ifdef RENEWAL
			{
				short atk, matk, size_mod, bonus;
				atk  = (2 * sstatus->batk) + sstatus->rhw.atk;
				matk = sstatus->matk_max + sstatus->matk_min;
				size_mod  = sd ? sd->right_weapon.atkmods[tstatus->size] : 100;
				bonus = sd ? sd->bonus.long_attack_atk_rate : 0; // Long ATK Bonus. Likes : Archer Skeleton Card

				if ((atk != 0 && size_mod != 0) || matk != 0)
					md.damage = (int)(((7 * atk * tstatus->vit) * size_mod / 100 + (7 * matk * tstatus->vit)) / 100);
				else
					md.damage = 0;

				if (tsd || is_boss(target))
					md.damage >>= 1;
				if (md.damage < 0)
					md.damage = 0;
				if (md.damage > INT_MAX>>1)
					md.damage = INT_MAX>>1; //Overflow prevention
				if (sd && bonus != 0)
					md.damage += md.damage * bonus / 100;
			}
#else
			if (tstatus->vit + sstatus->int_) //crash fix
				md.damage = (int)(7 * tstatus->vit * sstatus->int_ * sstatus->int_ / (10 * (tstatus->vit + sstatus->int_)));
			else
				md.damage = 0;
			if (tsd) md.damage >>= 1;
			if (md.damage < 0 || md.damage > INT_MAX>>1)
			//Overflow prevention, will anyone whine if I cap it to a few billion?
			//Not capped to INT_MAX to give some room for further damage increase.
				md.damage = INT_MAX>>1;
#endif
			break;
		case NJ_ZENYNAGE:
		case KO_MUCHANAGE:
				md.damage = skill_get_zeny(skill_id, skill_lv);
				if (!md.damage) md.damage = 2;
				md.damage = (skill_id == NJ_ZENYNAGE ? rnd()%md.damage + md.damage : md.damage * rnd_value(50,100)) / (skill_id == NJ_ZENYNAGE ? 1 : 100);
				if (sd) {
					if (skill_id == KO_MUCHANAGE && (pc_checkskill(sd,NJ_TOBIDOUGU) == 0))
						md.damage = md.damage / 2;
				}
				if (is_boss(target))
					md.damage = md.damage / (skill_id == NJ_ZENYNAGE ? 3 : 2);
				else if (tsd) // need confirmation for KO_MUCHANAGE
					md.damage = md.damage / 2;
			break;
		case GS_FLING:
			md.damage = (sd ? sd->status.job_level : status_get_lv(src));
			break;
		case HVAN_EXPLOSION: //[orn]
			md.damage = sstatus->max_hp * (50 + 50 * skill_lv) / 100;
			break;
#ifndef RENEWAL
		case ASC_BREAKER:
			md.damage = 500 + rnd()%500 + 5 * skill_lv * sstatus->int_;
			nk |= NK_IGNORE_FLEE|NK_NO_ELEFIX; //These two are not properties of the weapon based part.
			break;
#endif
		case HW_GRAVITATION:
			md.damage = 200 + 200 * skill_lv;
			md.dmotion = 0; //No flinch animation.
			break;
		case NPC_EVILLAND:
			md.damage = skill_calc_heal(src,target,skill_id,skill_lv,false);
			break;
		case RK_DRAGONBREATH:
			md.damage = ((sstatus->hp / 50) + (status_get_max_sp(src) / 4)) * skill_lv;
			RE_LVL_MDMOD(150);
			if (sd)
				md.damage = md.damage * (100 + 5 * (pc_checkskill(sd,RK_DRAGONTRAINING) - 1)) / 100;
			md.flag |= BF_LONG|BF_WEAPON;
			break;
		case RA_CLUSTERBOMB:
		case RA_FIRINGTRAP:
		case RA_ICEBOUNDTRAP:
			md.damage = skill_lv * sstatus->dex + sstatus->int_ * 5 ;
			RE_LVL_TMDMOD();
			if (sd) {
				int researchskill_lv = pc_checkskill(sd,RA_RESEARCHTRAP);
				if(researchskill_lv)
					md.damage = md.damage * 20 * researchskill_lv / (skill_id == RA_CLUSTERBOMB ? 50 : 100);
				else
					md.damage = 0;
			} else
				md.damage = md.damage * 200 / (skill_id == RA_CLUSTERBOMB ? 50 : 100);
			break;
		case WM_SOUND_OF_DESTRUCTION:
			md.damage = 1000 * skill_lv + sstatus->int_ * pc_checkskill(sd,WM_LESSON);
			md.damage += md.damage * ( 10 * chorusbonus ) / 100;
			break;
		case NC_SELFDESTRUCTION: {
				short totaldef = tstatus->def2 + (short)status_get_def(target);
				md.damage = (skill_lv + 1) * ((sd ? pc_checkskill(sd,NC_MAINFRAME) : 0) + 8) * (status_get_sp(src) + sstatus->vit);
				RE_LVL_MDMOD(100);
				md.damage += status_get_hp(src) - totaldef;
			}
			break;
		case GN_THORNS_TRAP:
			md.damage = 100 + 200 * skill_lv + sstatus->int_;
			break;
		case GN_HELLS_PLANT_ATK:
			//[{( Hell Plant Skill Level x Caster Base Level ) x 10 } + {( Caster INT x 7 ) / 2 } x { 18 + ( Caster Job Level / 4 )] x ( 5 / ( 10 - Summon Flora Skill Level ))
			md.damage = ( skill_lv * status_get_lv(src) * 10 ) + ( sstatus->int_ * 7 / 2 ) * ( 18 + (sd ? sd->status.job_level : 0) / 4 ) * ( 5 / (10 - (sd ? pc_checkskill(sd,AM_CANNIBALIZE) : 0)) );
			break;
	}

	if (nk&NK_SPLASHSPLIT) { // Divide ATK among targets
		if(mflag>0)
			md.damage/= mflag;
		else
			ShowError("0 enemies targeted by %d:%s, divide per 0 avoided!\n", skill_id, skill_get_name(skill_id));
	}

	damage_div_fix(md.damage, md.div_);

	if (!(nk&NK_IGNORE_FLEE)) {
		struct status_change *sc = status_get_sc(target);
		i = 0; //Temp for "hit or no hit"
		if(sc && sc->opt1 && sc->opt1 != OPT1_STONEWAIT && sc->opt1 != OPT1_BURNING)
			i = 1;
		else {
			short
				flee = tstatus->flee,
#ifdef RENEWAL
				hitrate = 0; //Default hitrate
#else
				hitrate = 80; //Default hitrate
#endif

			if(battle_config.agi_penalty_type && battle_config.agi_penalty_target&target->type) {
				unsigned char attacker_count; //256 max targets should be a sane max
				attacker_count = unit_counttargeted(target);
				if(attacker_count >= battle_config.agi_penalty_count) {
					if(battle_config.agi_penalty_type == 1)
						flee = (flee * (100 - (attacker_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num))/100;
					else //asume type 2: absolute reduction
						flee -= (attacker_count - (battle_config.agi_penalty_count - 1))*battle_config.agi_penalty_num;
					if(flee < 1) flee = 1;
				}
			}

			hitrate += sstatus->hit - flee;
#ifdef RENEWAL
			if( sd ) //in Renewal hit bonus from Vultures Eye is not anymore shown in status window
				hitrate += pc_checkskill(sd,AC_VULTURE);
#endif
			hitrate = cap_value(hitrate, battle_config.min_hitrate, battle_config.max_hitrate);

			if(rnd()%100 < hitrate)
				i = 1;
		}
		if (!i) {
			md.damage = 0;
			md.dmg_lv = ATK_FLEE;
		}
	}

	md.damage = battle_calc_cardfix(BF_MISC, src, target, nk, s_ele, 0, md.damage, 0, md.flag);

	if( sd && (i = pc_skillatk_bonus(sd, skill_id)) )
		md.damage += md.damage * i / 100;

	if( (i = battle_adjust_skill_damage(src->m,skill_id)) )
		md.damage = md.damage * i / 100;

	if( md.damage < 0 )
		md.damage = 0;
	else if(md.damage && tstatus->mode&MD_PLANT) {
		switch(skill_id) {
			case HT_LANDMINE:
			case MA_LANDMINE:
			case HT_BLASTMINE:
			case HT_CLAYMORETRAP:
			case RA_CLUSTERBOMB:
#ifdef RENEWAL
			break;
#endif
			default:
				md.damage = 1;
		}
	} else if( target->type == BL_SKILL ) {
		TBL_SKILL *su = (TBL_SKILL*)target;
		if( su->group && (su->group->skill_id == WM_REVERBERATION || su->group->skill_id == WM_POEMOFNETHERWORLD) )
			md.damage = 1;
	}

	if(!(nk&NK_NO_ELEFIX))
		md.damage = battle_attr_fix(src, target, md.damage, s_ele, tstatus->def_ele, tstatus->ele_lv);

	md.damage = battle_calc_damage(src,target,&md,md.damage,skill_id,skill_lv);
	if( map_flag_gvg2(target->m) )
		md.damage = battle_calc_gvg_damage(src,target,md.damage,md.div_,skill_id,skill_lv,md.flag);
	else if( map[target->m].flag.battleground )
		md.damage = battle_calc_bg_damage(src,target,md.damage,md.div_,skill_id,skill_lv,md.flag);

	switch( skill_id ) {
		case RA_FIRINGTRAP:
 		case RA_ICEBOUNDTRAP:
			if( md.damage == 1 ) break;
		case RA_CLUSTERBOMB: {
				struct Damage wd;
				wd = battle_calc_weapon_attack(src,target,skill_id,skill_lv,mflag);
				md.damage += wd.damage;
			}
			break;
		case NJ_ZENYNAGE:
			if( sd ) {
				if ( md.damage > sd->status.zeny )
					md.damage = sd->status.zeny;
				pc_payzeny(sd, md.damage,LOG_TYPE_STEAL,NULL);
			}
		break;
	}

	return md;
}
/*==========================================
 * Battle main entry, from skill_attack
 *------------------------------------------*/
struct Damage battle_calc_attack(int attack_type,struct block_list *bl,struct block_list *target,uint16 skill_id,uint16 skill_lv,int count)
{
	struct Damage d;

	switch(attack_type) {
		case BF_WEAPON: d = battle_calc_weapon_attack(bl,target,skill_id,skill_lv,count); break;
		case BF_MAGIC:  d = battle_calc_magic_attack(bl,target,skill_id,skill_lv,count);  break;
		case BF_MISC:   d = battle_calc_misc_attack(bl,target,skill_id,skill_lv,count);   break;
		default:
			ShowError("battle_calc_attack: unknown attack type! %d\n",attack_type);
			memset(&d,0,sizeof(d));
			break;
	}
	if( d.damage + d.damage2 < 1 ) { //Miss/Absorbed
		//Weapon attacks should go through to cause additional effects.
		if (d.dmg_lv == ATK_DEF /*&& attack_type&(BF_MAGIC|BF_MISC)*/) // Isn't it that additional effects don't apply if miss?
			d.dmg_lv = ATK_MISS;
		d.dmotion = 0;
	} else // Some skills like Weaponry Research will cause damage even if attack is dodged
		d.dmg_lv = ATK_DEF;
	return d;
}

//Calculates BF_WEAPON returned damage.
int battle_calc_return_damage(struct block_list* bl, struct block_list *src, int *dmg, int flag, uint16 skill_id) {
	struct map_session_data* sd;
	int rdamage = 0, damage = *dmg, max_damage = status_get_max_hp(bl);
	struct status_change *sc;
	struct status_change *ssc = status_get_sc(src);

	sd = BL_CAST(BL_PC, bl);
	sc = status_get_sc(bl);
	
	if( sc && sc->data[SC_CRESCENTELBOW] && !is_boss(src) && rnd()%100 < sc->data[SC_CRESCENTELBOW]->val2 ) {
		//ATK [{(Target HP / 100) x Skill Level} x Caster Base Level / 125] % + [Received damage x {1 + (Skill Level x 0.2)}]
		int ratio = (status_get_hp(src) / 100) * sc->data[SC_CRESCENTELBOW]->val1 * status_get_lv(bl) / 125;
		if (ratio > 5000) ratio = 5000; // Maximum of 5000% ATK
		rdamage = rdamage * ratio / 100 + (*dmg) * (10 + sc->data[SC_CRESCENTELBOW]->val1 * 20 / 10) / 10;
		skill_blown(bl, src, skill_get_blewcount(SR_CRESCENTELBOW_AUTOSPELL, sc->data[SC_CRESCENTELBOW]->val1), unit_getdir(src), 0);
		clif_skill_damage(bl, src, gettick(), status_get_amotion(src), 0, rdamage,
			1, SR_CRESCENTELBOW_AUTOSPELL, sc->data[SC_CRESCENTELBOW]->val1, 6); // This is how official does
		clif_damage(src, bl, gettick(), status_get_amotion(src)+1000, 0, rdamage/10, 1, 0, 0);
		status_damage(src, bl, status_damage(bl, src, rdamage, 0, 0, 1)/10, 0, 0, 1);
		status_change_end(bl, SC_CRESCENTELBOW, INVALID_TIMER);
		return 0; // Just put here to minimize redundancy
	} else if (flag & BF_SHORT) { //Bounces back part of the damage.
		if ( sd && sd->bonus.short_weapon_damage_return ) {
			rdamage += damage * sd->bonus.short_weapon_damage_return / 100;
			if(rdamage < 1) rdamage = 1;
		}
		if( sc && sc->count ) {
			if ( sc->data[SC_REFLECTSHIELD] && skill_id != WS_CARTTERMINATION ) {
				rdamage += damage * sc->data[SC_REFLECTSHIELD]->val2 / 100;
				rdamage = cap_value(rdamage,1,max_damage);
			}
			if(sc->data[SC_DEATHBOUND] && skill_id != WS_CARTTERMINATION && !(src->type == BL_MOB && is_boss(src)) ) {
				uint8 dir = map_calc_dir(bl,src->x,src->y), t_dir = unit_getdir(bl);

				if( distance_bl(src,bl) <= 0 || !map_check_dir(dir,t_dir) ) {
					int rd1 = 0;
					rd1 = min(damage,status_get_max_hp(bl)) * sc->data[SC_DEATHBOUND]->val2 / 100; // Amplify damage.
					*dmg = rd1 * 30 / 100; // Player receives 30% of the amplified damage.
					clif_skill_damage(src,bl,gettick(), status_get_amotion(src), 0, -30000, 1, RK_DEATHBOUND, sc->data[SC_DEATHBOUND]->val1,6);
					status_change_end(bl,SC_DEATHBOUND,INVALID_TIMER);
					rdamage += rd1 * 70 / 100; // Target receives 70% of the amplified damage. [Rytech]
				}
			}
			if( sc && sc->data[SC_REFLECTDAMAGE] && rnd()%100 < 30 + 10 * sc->data[SC_REFLECTDAMAGE]->val1) {
				max_damage = max_damage * status_get_lv(bl) / 100;
				rdamage += (*dmg) * sc->data[SC_REFLECTDAMAGE]->val2 / 100;
				rdamage = cap_value(rdamage,1,max_damage);
				if ((--sc->data[SC_REFLECTDAMAGE]->val3) <= 0)
					status_change_end(bl,SC_REFLECTDAMAGE,INVALID_TIMER);
			}
			if( sc && sc->data[SC_SHIELDSPELL_DEF] && sc->data[SC_SHIELDSPELL_DEF]->val1 == 2 ) {
				rdamage += (*dmg) * sc->data[SC_SHIELDSPELL_DEF]->val2 / 100;
				rdamage = cap_value(rdamage,1,max_damage);
			}
			if( ssc && ssc->data[SC_INSPIRATION] ) {
				rdamage += (*dmg) / 100;
				rdamage = cap_value(rdamage,1,max_damage);
			}
		}
	} else {
		if (sd && sd->bonus.long_weapon_damage_return) {
			rdamage += damage * sd->bonus.long_weapon_damage_return / 100;
			if (rdamage < 1) rdamage = 1;
		}
	}

	if( sc && sc->data[SC_KYOMU] ) // Nullify reflecting ability
		rdamage = 0;

	return rdamage;
}

void battle_drain(TBL_PC *sd, struct block_list *tbl, int rdamage, int ldamage, int race, int boss)
{
	struct weapon_data *wd;
	int type, thp = 0, tsp = 0, rhp = 0, rsp = 0, hp, sp, i, *damage;
	for (i = 0; i < 4; i++) {
		//First two iterations: Right hand
		if (i < 2) { wd = &sd->right_weapon; damage = &rdamage; }
		else { wd = &sd->left_weapon; damage = &ldamage; }
		if (*damage <= 0) continue;
		//First and Third iterations: race, other two boss/nonboss state
		if (i == 0 || i == 2)
			type = race;
		else
			type = boss?RC_BOSS:RC_NONBOSS;

		hp = wd->hp_drain[type].value;
		if (wd->hp_drain[type].rate)
			hp += battle_calc_drain(*damage, wd->hp_drain[type].rate, wd->hp_drain[type].per);

		sp = wd->sp_drain[type].value;
		if (wd->sp_drain[type].rate)
			sp += battle_calc_drain(*damage, wd->sp_drain[type].rate, wd->sp_drain[type].per);

		if (hp) {
			if (wd->hp_drain[type].type)
				rhp += hp;
			thp += hp;
		}
		if (sp) {
			if (wd->sp_drain[type].type)
				rsp += sp;
			tsp += sp;
		}
	}

	if (sd->bonus.sp_vanish_rate && rnd()%1000 < sd->bonus.sp_vanish_rate)
		status_percent_damage(&sd->bl, tbl, 0, (unsigned char)sd->bonus.sp_vanish_per, false);

	if( sd->sp_gain_race_attack[race] )
		tsp += sd->sp_gain_race_attack[race];
	if( sd->hp_gain_race_attack[race] )
		thp += sd->hp_gain_race_attack[race];

	if (!thp && !tsp) return;

	status_heal(&sd->bl, thp, tsp, battle_config.show_hp_sp_drain?3:1);

	if (rhp || rsp)
		status_zap(tbl, rhp, rsp);
}
// Deals the same damage to targets in area. [pakpil]
int battle_damage_area( struct block_list *bl, va_list ap) {
	unsigned int tick;
	int amotion, dmotion, damage;
	struct block_list *src;

	nullpo_ret(bl);

	tick=va_arg(ap, unsigned int);
	src=va_arg(ap,struct block_list *);
	amotion=va_arg(ap,int);
	dmotion=va_arg(ap,int);
	damage=va_arg(ap,int);

	if( bl->type == BL_MOB && ((TBL_MOB*)bl)->class_ == MOBID_EMPERIUM )
		return 0;
	if( bl != src && battle_check_target(src, bl, BCT_ENEMY) > 0 ) {
		map_freeblock_lock();
		if( src->type == BL_PC )
			battle_drain((TBL_PC*)src, bl, damage, damage, status_get_race(bl), is_boss(bl));
		if( amotion )
			battle_delay_damage(tick, amotion, src, bl, 0, CR_REFLECTSHIELD, 0, damage, ATK_DEF, 0, true);
		else
			status_fix_damage(src ,bl, damage, 0);
		clif_damage(bl, bl, tick, amotion, dmotion, damage, 1, ATK_BLOCK, 0);
		if( !(src && src->type == BL_PC && ((TBL_PC*)src)->state.autocast) )
			skill_additional_effect(src, bl, CR_REFLECTSHIELD, 1, BF_WEAPON|BF_SHORT|BF_NORMAL, ATK_DEF, tick);
		map_freeblock_unlock();
	}
	
	return 0;
}
/*==========================================
 * Do a basic physical attack (call trough unit_attack_timer)
 *------------------------------------------*/
enum damage_lv battle_weapon_attack(struct block_list* src, struct block_list* target, unsigned int tick, int flag) {
	struct map_session_data *sd = NULL, *tsd = NULL;
	struct status_data *sstatus, *tstatus;
	struct status_change *sc, *tsc;
	int damage, rdamage = 0, rdelay = 0;
	int skillv;
	struct Damage wd;

	nullpo_retr(ATK_NONE, src);
	nullpo_retr(ATK_NONE, target);

	if (src->prev == NULL || target->prev == NULL)
		return ATK_NONE;

	sd = BL_CAST(BL_PC, src);
	tsd = BL_CAST(BL_PC, target);

	sstatus = status_get_status_data(src);
	tstatus = status_get_status_data(target);

	sc = status_get_sc(src);
	tsc = status_get_sc(target);

	if (sc && !sc->count) //Avoid sc checks when there's none to check for. [Skotlex]
		sc = NULL;
	if (tsc && !tsc->count)
		tsc = NULL;
	
	if (sd) {
		sd->state.arrow_atk = (sd->status.weapon == W_BOW || (sd->status.weapon >= W_REVOLVER && sd->status.weapon <= W_GRENADE));
		if (sd->state.arrow_atk) {
			int index = sd->equip_index[EQI_AMMO];
			if (index<0) {
				clif_arrow_fail(sd,0);
				return ATK_NONE;
			}
			//Ammo check by Ishizu-chan
			if (sd->inventory_data[index])
			switch (sd->status.weapon) {
				case W_BOW:
					if (sd->inventory_data[index]->look != A_ARROW) {
						clif_arrow_fail(sd,0);
						return ATK_NONE;
					}
					break;
				case W_REVOLVER:
				case W_RIFLE:
				case W_GATLING:
				case W_SHOTGUN:
					if (sd->inventory_data[index]->look != A_BULLET) {
						clif_arrow_fail(sd,0);
						return ATK_NONE;
					}
					break;
				case W_GRENADE:
					if (sd->inventory_data[index]->look != A_GRENADE) {
						clif_arrow_fail(sd,0);
						return ATK_NONE;
					}
					break;
			}
		}
	}
	if (sc && sc->count) {
		if (sc->data[SC_CLOAKING] && !(sc->data[SC_CLOAKING]->val4 & 2))
			status_change_end(src, SC_CLOAKING, INVALID_TIMER);
		else if (sc->data[SC_CLOAKINGEXCEED] && !(sc->data[SC_CLOAKINGEXCEED]->val4 & 2))
			status_change_end(src, SC_CLOAKINGEXCEED, INVALID_TIMER);
	}
	if( tsc && tsc->data[SC_AUTOCOUNTER] && status_check_skilluse(target, src, KN_AUTOCOUNTER, 1) ) {
		uint8 dir = map_calc_dir(target,src->x,src->y);
		int t_dir = unit_getdir(target);
		int dist = distance_bl(src, target);
		if(dist <= 0 || (!map_check_dir(dir,t_dir) && dist <= tstatus->rhw.range+1)) {
			uint16 skill_lv = tsc->data[SC_AUTOCOUNTER]->val1;
			clif_skillcastcancel(target); //Remove the casting bar. [Skotlex]
			clif_damage(src, target, tick, sstatus->amotion, 1, 0, 1, 0, 0); //Display MISS.
			status_change_end(target, SC_AUTOCOUNTER, INVALID_TIMER);
			skill_attack(BF_WEAPON,target,target,src,KN_AUTOCOUNTER,skill_lv,tick,0);
			return ATK_BLOCK;
		}
	}

	if( tsc && tsc->data[SC_BLADESTOP_WAIT] && !is_boss(src) && (src->type == BL_PC || tsd == NULL || distance_bl(src, target) <= (tsd->status.weapon == W_FIST ? 1 : 2)) )
	{
		uint16 skill_lv = tsc->data[SC_BLADESTOP_WAIT]->val1;
		int duration = skill_get_time2(MO_BLADESTOP,skill_lv);
		status_change_end(target, SC_BLADESTOP_WAIT, INVALID_TIMER);
		if(sc_start4(src, src, SC_BLADESTOP, 100, sd ? pc_checkskill(sd, MO_BLADESTOP) : 0, 0, 0, target->id, duration)) {
			//Target locked.
			clif_damage(src, target, tick, sstatus->amotion, 1, 0, 1, 0, 0); //Display MISS.
			clif_bladestop(target, src->id, 1);
			sc_start4(src, target, SC_BLADESTOP, 100, skill_lv, 0, 0, src->id, duration);
			return ATK_BLOCK;
		}
	}

	if (sd && (skillv = pc_checkskill(sd,MO_TRIPLEATTACK)) > 0) {
		int triple_rate= 30 - skillv; //Base Rate
		if (sc && sc->data[SC_SKILLRATE_UP] && sc->data[SC_SKILLRATE_UP]->val1 == MO_TRIPLEATTACK) {
			triple_rate+= triple_rate*(sc->data[SC_SKILLRATE_UP]->val2)/100;
			status_change_end(src, SC_SKILLRATE_UP, INVALID_TIMER);
		}
		if (rnd()%100 < triple_rate) {
			if( skill_attack(BF_WEAPON,src,src,target,MO_TRIPLEATTACK,skillv,tick,0) )
				return ATK_DEF;
			return ATK_MISS;
		}
	}

	if (sc) {
		if (sc->data[SC_SACRIFICE]) {
			uint16 skill_lv = sc->data[SC_SACRIFICE]->val1;
			damage_lv ret_val;

			if (--sc->data[SC_SACRIFICE]->val2 <= 0)
				status_change_end(src,SC_SACRIFICE,INVALID_TIMER);

			/**
			 * We need to calculate the DMG before the hp reduction, because it can kill the source.
			 * For futher information: bugreport:4950
			 **/
			ret_val = (damage_lv)skill_attack(BF_WEAPON,src,src,target,PA_SACRIFICE,skill_lv,tick,0);

			status_zap(src, sstatus->max_hp*9/100, 0); //Damage to self is always 9%
			if (ret_val == ATK_NONE)
				return ATK_MISS;
			return ret_val;
		}
		if (sc->data[SC_MAGICALATTACK]) {
			if (skill_attack(BF_MAGIC,src,src,target,NPC_MAGICALATTACK,sc->data[SC_MAGICALATTACK]->val1,tick,0))
				return ATK_DEF;
			return ATK_MISS;
		}
		if (sc->data[SC_GT_ENERGYGAIN] && sc->data[SC_GT_ENERGYGAIN]->val2) {
			int spheremax = 0;
			if (sc->data[SC_RAISINGDRAGON])
				spheremax = 5 + sc->data[SC_RAISINGDRAGON]->val1;
			else
				spheremax = 5;
			if (sd && rnd()%100 < sc->data[SC_GT_ENERGYGAIN]->val3)
				pc_addspiritball(sd,skill_get_time2(SR_GENTLETOUCH_ENERGYGAIN,sc->data[SC_GT_ENERGYGAIN]->val1),spheremax);
		}
		if (sc && sc->data[SC_CRUSHSTRIKE]) {
			uint16 skill_lv = sc->data[SC_CRUSHSTRIKE]->val1;
			status_change_end(src,SC_CRUSHSTRIKE,INVALID_TIMER);
			if (skill_attack(BF_WEAPON,src,src,target,RK_CRUSHSTRIKE,skill_lv,tick,0))
				return ATK_DEF;
			return ATK_MISS;
		}
	}

	if( tsc && tsc->data[SC_GT_ENERGYGAIN] && tsc->data[SC_GT_ENERGYGAIN]->val2 ) {
		int spheremax = 0;
		if( tsc->data[SC_RAISINGDRAGON] )
			spheremax = 5 + sc->data[SC_RAISINGDRAGON]->val1;
		else
			spheremax = 5;
		if( tsd && rnd()%100 < tsc->data[SC_GT_ENERGYGAIN]->val3 )
			pc_addspiritball(tsd,skill_get_time2(SR_GENTLETOUCH_ENERGYGAIN,tsc->data[SC_GT_ENERGYGAIN]->val1),spheremax);
	}

	if( tsc && tsc->data[SC_KAAHI] && tsc->data[SC_KAAHI]->val4 == INVALID_TIMER && tstatus->hp < tstatus->max_hp )
		tsc->data[SC_KAAHI]->val4 = add_timer(tick + skill_get_time2(SL_KAAHI,tsc->data[SC_KAAHI]->val1), kaahi_heal_timer, target->id, SC_KAAHI); //Activate heal.

	wd = battle_calc_attack(BF_WEAPON, src, target, 0, 0, flag);

	if( sc && sc->count ) {
		if (sc->data[SC_EXEEDBREAK]) {
			ATK_RATER(sc->data[SC_EXEEDBREAK]->val1)
			status_change_end(src, SC_EXEEDBREAK, INVALID_TIMER);
		}
		if( sc->data[SC_SPELLFIST] ) {
			if( --(sc->data[SC_SPELLFIST]->val1) >= 0 ) {
				struct Damage ad = battle_calc_attack(BF_MAGIC,src,target,sc->data[SC_SPELLFIST]->val3,sc->data[SC_SPELLFIST]->val4,flag|BF_SHORT);
				wd.damage = ad.damage;
			} else
				status_change_end(src,SC_SPELLFIST,INVALID_TIMER);
		}
		if( sc->data[SC_GIANTGROWTH] && (wd.flag&BF_SHORT) && rnd()%100 < sc->data[SC_GIANTGROWTH]->val2 )
			wd.damage *= 3; // Triple Damage

		if( sd && sc->data[SC_FEARBREEZE] && sc->data[SC_FEARBREEZE]->val4 > 0 && sd->status.inventory[sd->equip_index[EQI_AMMO]].amount >= sc->data[SC_FEARBREEZE]->val4 && battle_config.arrow_decrement){
			pc_delitem(sd,sd->equip_index[EQI_AMMO],sc->data[SC_FEARBREEZE]->val4,0,1,LOG_TYPE_CONSUME);
			sc->data[SC_FEARBREEZE]->val4 = 0;
		}
	}
	if (sd && sd->state.arrow_atk) //Consume arrow.
		battle_consume_ammo(sd, 0, 0);

	damage = wd.damage + wd.damage2;

	if( damage > 0 && src != target ) {
		if( sc && sc->data[SC_DUPLELIGHT] && (wd.flag&BF_SHORT) && rnd()%100 <= 10+2*sc->data[SC_DUPLELIGHT]->val1 ) {
			// Activates it only from melee damage
			uint16 skill_id;
			if( rnd()%2 == 1 )
				skill_id = AB_DUPLELIGHT_MELEE;
			else
				skill_id = AB_DUPLELIGHT_MAGIC;
			skill_attack(skill_get_type(skill_id), src, src, target, skill_id, sc->data[SC_DUPLELIGHT]->val1, tick, SD_LEVEL);
		}

		rdamage = battle_calc_return_damage(target,src, &damage, wd.flag, 0);
		if( rdamage > 0 ) {
			if( tsc && tsc->data[SC_REFLECTDAMAGE] ) {
				if( src != target ) // Don't reflect your own damage (Grand Cross)
					map_foreachinshootrange(battle_damage_area,target,skill_get_splash(LG_REFLECTDAMAGE,1),BL_CHAR,tick,target,wd.amotion,wd.dmotion,rdamage,tstatus->race);
			} else {
				rdelay = clif_damage(src,src,tick,wd.amotion,sstatus->dmotion,rdamage,1,4,0);
				//Use Reflect Shield to signal this kind of skill trigger. [Skotlex]
				skill_additional_effect(target,src,CR_REFLECTSHIELD,1,BF_WEAPON|BF_SHORT|BF_NORMAL,ATK_DEF,tick);
			}
		}
	}

	wd.dmotion = clif_damage(src, target, tick, wd.amotion, wd.dmotion, wd.damage, wd.div_ , wd.type, wd.damage2);

	if (sd && sd->bonus.splash_range > 0 && damage > 0)
		skill_castend_damage_id(src, target, 0, 1, tick, 0);
	if ( target->type == BL_SKILL && damage > 0 ){
		TBL_SKILL *su = (TBL_SKILL*)target;
		if( su->group && su->group->skill_id == HT_BLASTMINE)
			skill_blown(src, target, 3, -1, 0);
	}

	map_freeblock_lock();

	battle_delay_damage(tick, wd.amotion, src, target, wd.flag, 0, 0, damage, wd.dmg_lv, wd.dmotion, true);

	if( tsc ) {
		if( tsc->data[SC_DEVOTION] ) {
			struct status_change_entry *sce = tsc->data[SC_DEVOTION];
			struct block_list *d_bl = map_id2bl(sce->val1);

			if( d_bl && (
				(d_bl->type == BL_MER && ((TBL_MER*)d_bl)->master && ((TBL_MER*)d_bl)->master->bl.id == target->id) ||
				(d_bl->type == BL_PC && ((TBL_PC*)d_bl)->devotion[sce->val2] == target->id)
				) && check_distance_bl(target, d_bl, sce->val3) )
			{
				clif_damage(d_bl, d_bl, gettick(), 0, 0, damage, 0, 0, 0);
				status_fix_damage(NULL, d_bl, damage, 0);
			} else
				status_change_end(target, SC_DEVOTION, INVALID_TIMER);
		} else if( tsc->data[SC_CIRCLE_OF_FIRE_OPTION] && (wd.flag&BF_SHORT) && target->type == BL_PC ) {
			struct elemental_data *ed = ((TBL_PC*)target)->ed;
			if( ed ) {
				clif_skill_damage(&ed->bl, target, tick, status_get_amotion(src), 0, -30000, 1, EL_CIRCLE_OF_FIRE, tsc->data[SC_CIRCLE_OF_FIRE_OPTION]->val1, 6);
				skill_attack(BF_WEAPON, &ed->bl, &ed->bl, src, EL_CIRCLE_OF_FIRE, tsc->data[SC_CIRCLE_OF_FIRE_OPTION]->val1, tick, wd.flag);
			}
		} else if( tsc->data[SC_WATER_SCREEN_OPTION] && tsc->data[SC_WATER_SCREEN_OPTION]->val1 ) {
			struct block_list *e_bl = map_id2bl(tsc->data[SC_WATER_SCREEN_OPTION]->val1);
			if( e_bl && !status_isdead(e_bl) ) {
				clif_damage(e_bl, e_bl, tick, wd.amotion, wd.dmotion, damage, wd.div_, wd.type, wd.damage2);
				status_damage(target, e_bl, damage, 0, 0, 0);
				// Just show damage in target.
				clif_damage(src, target, tick, wd.amotion, wd.dmotion, damage, wd.div_, wd.type, wd.damage2);
				map_freeblock_unlock();
				return ATK_BLOCK;
			}
		}
	}

	if (sc && sc->data[SC_AUTOSPELL] && rnd()%100 < sc->data[SC_AUTOSPELL]->val4) {
		int sp = 0;
		uint16 skill_id = sc->data[SC_AUTOSPELL]->val2;
		uint16 skill_lv = sc->data[SC_AUTOSPELL]->val3;
		int i = rnd()%100;
		if (sc->data[SC_SPIRIT] && sc->data[SC_SPIRIT]->val2 == SL_SAGE)
			i = 0; //Max chance, no skilllv reduction. [Skotlex]
		if (i >= 50) skill_lv -= 2;
		else if (i >= 15) skill_lv--;
		if (skill_lv < 1) skill_lv = 1;
		sp = skill_get_sp(skill_id,skill_lv) * 2 / 3;

		if (status_charge(src, 0, sp)) {
			switch (skill_get_casttype(skill_id)) {
				case CAST_GROUND:
					skill_castend_pos2(src, target->x, target->y, skill_id, skill_lv, tick, flag);
					break;
				case CAST_NODAMAGE:
					skill_castend_nodamage_id(src, target, skill_id, skill_lv, tick, flag);
					break;
				case CAST_DAMAGE:
					skill_castend_damage_id(src, target, skill_id, skill_lv, tick, flag);
					break;
			}
		}
	}
	if (sd) {
		if( wd.flag&BF_SHORT && sc && sc->data[SC__AUTOSHADOWSPELL] && rnd()%100 < sc->data[SC__AUTOSHADOWSPELL]->val3 &&
			sd->status.skill[sc->data[SC__AUTOSHADOWSPELL]->val1].id != 0 && sd->status.skill[sc->data[SC__AUTOSHADOWSPELL]->val1].flag == SKILL_FLAG_PLAGIARIZED )
		{
			int r_skill = sd->status.skill[sc->data[SC__AUTOSHADOWSPELL]->val1].id,
				r_lv = sc->data[SC__AUTOSHADOWSPELL]->val2, type;

				if( (type = skill_get_casttype(r_skill)) == CAST_GROUND ) {
					int maxcount = 0;

					if( !(BL_PC&battle_config.skill_reiteration) &&
						skill_get_unit_flag(r_skill)&UF_NOREITERATION )
							type = -1;

					if( BL_PC&battle_config.skill_nofootset &&
						skill_get_unit_flag(r_skill)&UF_NOFOOTSET )
							type = -1;

					if( BL_PC&battle_config.land_skill_limit &&
						(maxcount = skill_get_maxcount(r_skill, r_lv)) > 0
					  ) {
						int v;
						for(v=0;v<MAX_SKILLUNITGROUP && sd->ud.skillunit[v] && maxcount;v++) {
							if(sd->ud.skillunit[v]->skill_id == r_skill)
								maxcount--;
						}
						if( maxcount == 0 )
							type = -1;
					}

					if( type != CAST_GROUND ) {
							clif_skill_fail(sd,r_skill,USESKILL_FAIL_LEVEL,0);
							map_freeblock_unlock();
							return wd.dmg_lv;
					}
				}

				sd->state.autocast = 1;
				skill_consume_requirement(sd,r_skill,r_lv,3);
				switch( type ) {
					case CAST_GROUND:
						skill_castend_pos2(src, target->x, target->y, r_skill, r_lv, tick, flag);
						break;
					case CAST_DAMAGE:
						skill_castend_damage_id(src, target, r_skill, r_lv, tick, flag);
						break;
					case CAST_NODAMAGE:
						skill_castend_nodamage_id(src, target, r_skill, r_lv, tick, flag);
						break;
				}
				sd->state.autocast = 0;

				sd->ud.canact_tick = tick + skill_delayfix(src, r_skill, r_lv);
				clif_status_change(src, SI_ACTIONDELAY, 1, skill_delayfix(src, r_skill, r_lv), 0, 0, 1);
		}

		if (wd.flag & BF_WEAPON && src != target && damage > 0) {
			if (battle_config.left_cardfix_to_right)
				battle_drain(sd, target, wd.damage, wd.damage, tstatus->race, is_boss(target));
			else
				battle_drain(sd, target, wd.damage, wd.damage2, tstatus->race, is_boss(target));
		}
	}
	if (rdamage > 0 && !(tsc && tsc->data[SC_REFLECTDAMAGE])) { //By sending attack type "none" skill_additional_effect won't be invoked. [Skotlex]
		if(tsd && src != target)
			battle_drain(tsd, src, rdamage, rdamage, sstatus->race, is_boss(src));
		battle_delay_damage(tick, wd.amotion, target, src, 0, CR_REFLECTSHIELD, 0, rdamage, ATK_DEF, rdelay, true);
	}

	if (tsc) {
		if (tsc->data[SC_POISONREACT] &&
			(rnd()%100 < tsc->data[SC_POISONREACT]->val3
			|| sstatus->def_ele == ELE_POISON) &&
//			check_distance_bl(src, target, tstatus->rhw.range+1) && Doesn't checks range! o.O;
			status_check_skilluse(target, src, TF_POISON, 0)
		) {	//Poison React
			struct status_change_entry *sce = tsc->data[SC_POISONREACT];
			if (sstatus->def_ele == ELE_POISON) {
				sce->val2 = 0;
				skill_attack(BF_WEAPON,target,target,src,AS_POISONREACT,sce->val1,tick,0);
			} else {
				skill_attack(BF_WEAPON,target,target,src,TF_POISON, 5, tick, 0);
				--sce->val2;
			}
			if (sce->val2 <= 0)
				status_change_end(target, SC_POISONREACT, INVALID_TIMER);
		}
	}
	map_freeblock_unlock();
	return wd.dmg_lv;
}

int battle_check_undead(int race,int element)
{
	if(battle_config.undead_detect_type == 0) {
		if(element == ELE_UNDEAD)
			return 1;
	} else if(battle_config.undead_detect_type == 1) {
		if(race == RC_UNDEAD)
			return 1;
	} else {
		if(element == ELE_UNDEAD || race == RC_UNDEAD)
			return 1;
	}
	return 0;
}

//Returns the upmost level master starting with the given object
struct block_list* battle_get_master(struct block_list *src)
{
	struct block_list *prev; //Used for infinite loop check (master of yourself?)
	do {
		prev = src;
		switch (src->type) {
			case BL_PET:
				if (((TBL_PET*)src)->msd)
					src = (struct block_list*)((TBL_PET*)src)->msd;
				break;
			case BL_MOB:
				if (((TBL_MOB*)src)->master_id)
					src = map_id2bl(((TBL_MOB*)src)->master_id);
				break;
			case BL_HOM:
				if (((TBL_HOM*)src)->master)
					src = (struct block_list*)((TBL_HOM*)src)->master;
				break;
			case BL_MER:
				if (((TBL_MER*)src)->master)
					src = (struct block_list*)((TBL_MER*)src)->master;
				break;
			case BL_ELEM:
				if (((TBL_ELEM*)src)->master)
					src = (struct block_list*)((TBL_ELEM*)src)->master;
				break;
			case BL_SKILL:
				if (((TBL_SKILL*)src)->group && ((TBL_SKILL*)src)->group->src_id)
					src = map_id2bl(((TBL_SKILL*)src)->group->src_id);
				break;
		}
	} while (src && src != prev);
	return prev;
}

/*==========================================
 * Checks the state between two targets (rewritten by Skotlex)
 * (enemy, friend, party, guild, etc)
 * See battle.h for possible values/combinations
 * to be used here (BCT_* constants)
 * Return value is:
 * 1: flag holds true (is enemy, party, etc)
 * -1: flag fails
 * 0: Invalid target (non-targetable ever)
 *------------------------------------------*/
int battle_check_target( struct block_list *src, struct block_list *target, int flag)
{
	int16 m; //map
	int state = 0; //Initial state none
	int strip_enemy = 1; //Flag which marks whether to remove the BCT_ENEMY status if it's also friend/ally.
	struct block_list *s_bl = src, *t_bl = target;

	nullpo_ret(src);
	nullpo_ret(target);

	m = target->m;

	//t_bl/s_bl hold the 'master' of the attack, while src/target are the actual
	//objects involved.
	if( (t_bl = battle_get_master(target)) == NULL )
		t_bl = target;

	if( (s_bl = battle_get_master(src)) == NULL )
		s_bl = src;
		
	if ( s_bl->type == BL_PC ) {
		switch( t_bl->type ) {
			case BL_MOB: // Source => PC, Target => MOB
				if ( pc_has_permission((TBL_PC*)s_bl, PC_PERM_DISABLE_PVM) )
					return 0;
				break;
			case BL_PC:
				if (pc_has_permission((TBL_PC*)s_bl, PC_PERM_DISABLE_PVP))
					return 0;
				break;
			default:/* anything else goes */
				break;
		}
	}

	switch( target->type ) { // Checks on actual target
		case BL_PC: {
				struct status_change* sc = status_get_sc(src);
				if (((TBL_PC*)target)->invincible_timer != INVALID_TIMER || pc_isinvisible((TBL_PC*)target))
					return -1; //Cannot be targeted yet.
				if( sc && sc->count ) {
					if( sc->data[SC_VOICEOFSIREN] && sc->data[SC_VOICEOFSIREN]->val2 == target->id )
						return -1;
				}
			}
			break;
		case BL_MOB:
			if(((((TBL_MOB*)target)->special_state.ai == 2 || //Marine Spheres
				(((TBL_MOB*)target)->special_state.ai == 3 && battle_config.summon_flora&1)) && //Floras
				s_bl->type == BL_PC && src->type != BL_MOB) || (((TBL_MOB*)target)->special_state.ai == 4 && t_bl->id != s_bl->id)) //Zanzoe
			{	//Targettable by players
				state |= BCT_ENEMY;
				strip_enemy = 0;
			}
			break;
		case BL_SKILL: {
			TBL_SKILL *su = (TBL_SKILL*)target;
			if( !su->group )
				return 0;
			if( skill_get_inf2(su->group->skill_id)&INF2_TRAP ) { //Only a few skills can target traps...
				switch( battle_getcurrentskill(src) ) {
					case RK_DRAGONBREATH:// it can only hit traps in pvp/gvg maps
						if( !map[m].flag.pvp && !map[m].flag.gvg )
						break;
					case 0://you can hit them without skills
					case MA_REMOVETRAP:
					case HT_REMOVETRAP:
					case AC_SHOWER:
					case MA_SHOWER:
					case WZ_SIGHTRASHER:
					case WZ_SIGHTBLASTER:
					case SM_MAGNUM:
					case MS_MAGNUM:
					case RA_DETONATOR:
					case RA_SENSITIVEKEEN:
					case GN_CRAZYWEED_ATK:
					case RK_STORMBLAST:
					case SR_RAMPAGEBLASTER:
					case NC_COLDSLOWER:
					case NC_SELFDESTRUCTION:
#ifdef RENEWAL
					case KN_BOWLINGBASH:
					case KN_SPEARSTAB:
					case LK_SPIRALPIERCE:
					case ML_SPIRALPIERCE:
					case MO_FINGEROFFENSIVE:
					case MO_INVESTIGATE:
					case MO_TRIPLEATTACK:
					case MO_EXTREMITYFIST:
					case CR_HOLYCROSS:
					case ASC_METEORASSAULT:
					case RG_RAID:
					case MC_CARTREVOLUTION:
#endif
						state |= BCT_ENEMY;
						strip_enemy = 0;
						break;
					default:
						if(su->group->skill_id == WM_REVERBERATION || su->group->skill_id == WM_POEMOFNETHERWORLD) {
							state |= BCT_ENEMY;
							strip_enemy = 0;
						} else
							return 0;
				}
			} else if (su->group->skill_id==WZ_ICEWALL ||
					   su->group->skill_id == GN_WALLOFTHORN) {
				state |= BCT_ENEMY;
				strip_enemy = 0;
			} else	//Excepting traps and icewall, you should not be able to target skills.
				return 0;
		}
			break;
		//Valid targets with no special checks here.
		case BL_MER:
		case BL_HOM:
		case BL_ELEM:
			break;
		//All else not specified is an invalid target.
		default:
			return 0;
	} //end switch actual target

	switch( t_bl->type ) { //Checks on target master
		case BL_PC: {
			struct map_session_data *sd;
			if( t_bl == s_bl ) break;
			sd = BL_CAST(BL_PC, t_bl);

			if( sd->state.monster_ignore && flag&BCT_ENEMY )
				return 0; // Global inminuty only to Attacks
			if( sd->status.karma && s_bl->type == BL_PC && ((TBL_PC*)s_bl)->status.karma )
				state |= BCT_ENEMY; // Characters with bad karma may fight amongst them
			if( sd->state.killable ) {
				state |= BCT_ENEMY; // Everything can kill it
				strip_enemy = 0;
			}
			break;
		}
		case BL_MOB: {
			struct mob_data *md = BL_CAST(BL_MOB, t_bl);

			if( !((agit_flag || agit2_flag) && map[m].flag.gvg_castle) && md->guardian_data && md->guardian_data->guild_id )
				return 0; // Disable guardians/emperiums owned by Guilds on non-woe times.
			break;
		}
		default: break; //other type doesn't have slave yet
	} //end switch master target

	switch( src->type ) { //Checks on actual src type
		case BL_PET:
			if (t_bl->type != BL_MOB && flag&BCT_ENEMY)
				return 0; //Pet may not attack non-mobs.
			if (t_bl->type == BL_MOB && ((TBL_MOB*)t_bl)->guardian_data && flag&BCT_ENEMY)
				return 0; //pet may not attack Guardians/Emperium
			break;
		case BL_SKILL: {
				struct skill_unit *su = (struct skill_unit *)src;
				if (!su->group)
					return 0;

				if (su->group->src_id == target->id) {
					int inf2 = skill_get_inf2(su->group->skill_id);
					if (inf2&INF2_NO_TARGET_SELF)
						return -1;
					if (inf2&INF2_TARGET_SELF)
						return 1;
				}
			}
			break;
		case BL_MER:
			if (t_bl->type == BL_MOB && ((TBL_MOB*)t_bl)->class_ == MOBID_EMPERIUM && flag&BCT_ENEMY)
				return 0; //mercenary may not attack Emperium
			break;
	} //end switch actual src

	switch( s_bl->type ) { //Checks on source master
		case BL_PC: {
			struct map_session_data *sd = BL_CAST(BL_PC, s_bl);
			if( s_bl != t_bl ) {
				if( sd->state.killer ) {
					state |= BCT_ENEMY; // Can kill anything
					strip_enemy = 0;
				} else if( sd->duel_group && !((!battle_config.duel_allow_pvp && map[m].flag.pvp) || (!battle_config.duel_allow_gvg && map_flag_gvg(m))) )
		  		{
					if( t_bl->type == BL_PC && (sd->duel_group == ((TBL_PC*)t_bl)->duel_group) )
						return (BCT_ENEMY&flag)?1:-1; // Duel targets can ONLY be your enemy, nothing else.
					else
						return 0; // You can't target anything out of your duel
				}
			}
			if( map_flag_gvg(m) && !sd->status.guild_id && t_bl->type == BL_MOB && ((TBL_MOB*)t_bl)->class_ == MOBID_EMPERIUM )
				return 0; //If you don't belong to a guild, can't target emperium.
			if( t_bl->type != BL_PC )
				state |= BCT_ENEMY; //Natural enemy.
			break;
		}
		case BL_MOB: {
			struct mob_data *md = BL_CAST(BL_MOB, s_bl);
			if( !((agit_flag || agit2_flag) && map[m].flag.gvg_castle) && md->guardian_data && md->guardian_data->guild_id )
				return 0; // Disable guardians/emperium owned by Guilds on non-woe times.

			if( !md->special_state.ai ) { //Normal mobs
				if(
					( target->type == BL_MOB && t_bl->type == BL_PC && ( ((TBL_MOB*)target)->special_state.ai != 4 && ((TBL_MOB*)target)->special_state.ai != 1 ) ) ||
					( t_bl->type == BL_MOB && !((TBL_MOB*)t_bl)->special_state.ai )
				  )
					state |= BCT_PARTY; //Normal mobs with no ai are friends.
				else
					state |= BCT_ENEMY; //However, all else are enemies.
			} else {
				if( t_bl->type == BL_MOB && !((TBL_MOB*)t_bl)->special_state.ai )
					state |= BCT_ENEMY; //Natural enemy for AI mobs are normal mobs.
			}
			break;
		}
		default:
		//Need some sort of default behaviour for unhandled types.
			if (t_bl->type != s_bl->type)
				state |= BCT_ENEMY;
			break;
	} //end switch on src master

	if( (flag&BCT_ALL) == BCT_ALL ) { //All actually stands for all attackable chars
		if( target->type&BL_CHAR )
			return 1;
		else
			return -1;
	}
	if( flag == BCT_NOONE ) //Why would someone use this? no clue.
		return -1;

	if( t_bl == s_bl ) { //No need for further testing.
		state |= BCT_SELF|BCT_PARTY|BCT_GUILD;
		if( state&BCT_ENEMY && strip_enemy )
			state&=~BCT_ENEMY;
		return (flag&state)?1:-1;
	}

	if( map_flag_vs(m) ) { //Check rivalry settings.
		int sbg_id = 0, tbg_id = 0;
		if( map[m].flag.battleground ) {
			sbg_id = bg_team_get_id(s_bl);
			tbg_id = bg_team_get_id(t_bl);
		}
		if( flag&(BCT_PARTY|BCT_ENEMY) ) {
			int s_party = status_get_party_id(s_bl);
			if( s_party && s_party == status_get_party_id(t_bl) && !(map[m].flag.pvp && map[m].flag.pvp_noparty) && !(map_flag_gvg(m) && map[m].flag.gvg_noparty) && (!map[m].flag.battleground || sbg_id == tbg_id) )
				state |= BCT_PARTY;
			else
				state |= BCT_ENEMY;
		}
		if( flag&(BCT_GUILD|BCT_ENEMY) ) {
			int s_guild = status_get_guild_id(s_bl);
			int t_guild = status_get_guild_id(t_bl);
			if( !(map[m].flag.pvp && map[m].flag.pvp_noguild) && s_guild && t_guild && (s_guild == t_guild || guild_isallied(s_guild, t_guild)) && (!map[m].flag.battleground || sbg_id == tbg_id) )
				state |= BCT_GUILD;
			else
				state |= BCT_ENEMY;
		}
		if( state&BCT_ENEMY && map[m].flag.battleground && sbg_id && sbg_id == tbg_id )
			state &= ~BCT_ENEMY;

		if( state&BCT_ENEMY && battle_config.pk_mode && !map_flag_gvg(m) && s_bl->type == BL_PC && t_bl->type == BL_PC ) {
			// Prevent novice engagement on pk_mode (feature by Valaris)
			TBL_PC *sd = (TBL_PC*)s_bl, *sd2 = (TBL_PC*)t_bl;
			if (
				(sd->class_&MAPID_UPPERMASK) == MAPID_NOVICE ||
				(sd2->class_&MAPID_UPPERMASK) == MAPID_NOVICE ||
				(int)sd->status.base_level < battle_config.pk_min_level ||
			  	(int)sd2->status.base_level < battle_config.pk_min_level ||
				(battle_config.pk_level_range && abs((int)sd->status.base_level - (int)sd2->status.base_level) > battle_config.pk_level_range)
			)
				state &= ~BCT_ENEMY;
		}
	}//end map_flag_vs chk rivality
	else { //Non pvp/gvg, check party/guild settings.
		if( flag&BCT_PARTY || state&BCT_ENEMY ) {
			int s_party = status_get_party_id(s_bl);
			if(s_party && s_party == status_get_party_id(t_bl))
				state |= BCT_PARTY;
		}
		if( flag&BCT_GUILD || state&BCT_ENEMY ) {
			int s_guild = status_get_guild_id(s_bl);
			int t_guild = status_get_guild_id(t_bl);
			if(s_guild && t_guild && (s_guild == t_guild || guild_isallied(s_guild, t_guild)))
				state |= BCT_GUILD;
		}
	} //end non pvp/gvg chk rivality

	if( !state ) //If not an enemy, nor a guild, nor party, nor yourself, it's neutral.
		state = BCT_NEUTRAL;
	//Alliance state takes precedence over enemy one.
	else if( state&BCT_ENEMY && strip_enemy && state&(BCT_SELF|BCT_PARTY|BCT_GUILD) )
		state&=~BCT_ENEMY;

	return (flag&state)?1:-1;
}
/*==========================================
 * Check if can attack from this range
 * Basic check then calling path_search for obstacle etc..
 *------------------------------------------*/
bool battle_check_range(struct block_list *src, struct block_list *bl, int range)
{
	int d;
	nullpo_retr(false, src);
	nullpo_retr(false, bl);

	if( src->m != bl->m )
		return false;

#ifndef CIRCULAR_AREA
	if( src->type == BL_PC ) { // Range for players' attacks and skills should always have a circular check. [Angezerus]
		int dx = src->x - bl->x, dy = src->y - bl->y;
		if( !check_distance(dx, dy, range) )
			return false;
	} else
#endif
	if( !check_distance_bl(src, bl, range) )
		return false;

	if( (d = distance_bl(src, bl)) < 2 )
		return true;  // No need for path checking.

	if( d > AREA_SIZE )
		return false; // Avoid targetting objects beyond your range of sight.

	return path_search_long(NULL,src->m,src->x,src->y,bl->x,bl->y,CELL_CHKWALL);
}

static const struct _battle_data {
	const char* str;
	int* val;
	int defval;
	int min;
	int max;
} battle_data[] = {
	{ "warp_point_debug",                   &battle_config.warp_point_debug,                0,      0,      1,              },
	{ "enable_critical",                    &battle_config.enable_critical,                 BL_PC,  BL_NUL, BL_ALL,         },
	{ "mob_critical_rate",                  &battle_config.mob_critical_rate,               100,    0,      INT_MAX,        },
	{ "critical_rate",                      &battle_config.critical_rate,                   100,    0,      INT_MAX,        },
	{ "enable_baseatk",                     &battle_config.enable_baseatk,                  BL_PC|BL_HOM, BL_NUL, BL_ALL,   },
	{ "enable_perfect_flee",                &battle_config.enable_perfect_flee,             BL_PC|BL_PET, BL_NUL, BL_ALL,   },
	{ "casting_rate",                       &battle_config.cast_rate,                       100,    0,      INT_MAX,        },
	{ "delay_rate",                         &battle_config.delay_rate,                      100,    0,      INT_MAX,        },
	{ "delay_dependon_dex",                 &battle_config.delay_dependon_dex,              0,      0,      1,              },
	{ "delay_dependon_agi",                 &battle_config.delay_dependon_agi,              0,      0,      1,              },
	{ "skill_delay_attack_enable",          &battle_config.sdelay_attack_enable,            0,      0,      1,              },
	{ "left_cardfix_to_right",              &battle_config.left_cardfix_to_right,           0,      0,      1,              },
	{ "skill_add_range",                    &battle_config.skill_add_range,                 0,      0,      INT_MAX,        },
	{ "skill_out_range_consume",            &battle_config.skill_out_range_consume,         1,      0,      1,              },
	{ "skillrange_by_distance",             &battle_config.skillrange_by_distance,          ~BL_PC, BL_NUL, BL_ALL,         },
	{ "skillrange_from_weapon",             &battle_config.use_weapon_skill_range,          ~BL_PC, BL_NUL, BL_ALL,         },
	{ "player_damage_delay_rate",           &battle_config.pc_damage_delay_rate,            100,    0,      INT_MAX,        },
	{ "defunit_not_enemy",                  &battle_config.defnotenemy,                     0,      0,      1,              },
	{ "gvg_traps_target_all",               &battle_config.vs_traps_bctall,                 BL_PC,  BL_NUL, BL_ALL,         },
	{ "traps_setting",                      &battle_config.traps_setting,                   0,      0,      1,              },
	{ "summon_flora_setting",               &battle_config.summon_flora,                    1|2,    0,      1|2,            },
	{ "clear_skills_on_death",              &battle_config.clear_unit_ondeath,              BL_NUL, BL_NUL, BL_ALL,         },
	{ "clear_skills_on_warp",               &battle_config.clear_unit_onwarp,               BL_ALL, BL_NUL, BL_ALL,         },
	{ "random_monster_checklv",             &battle_config.random_monster_checklv,          0,      0,      1,              },
	{ "attribute_recover",                  &battle_config.attr_recover,                    1,      0,      1,              },
	{ "flooritem_lifetime",                 &battle_config.flooritem_lifetime,              60000,  1000,   INT_MAX,        },
	{ "item_auto_get",                      &battle_config.item_auto_get,                   0,      0,      1,              },
	{ "item_first_get_time",                &battle_config.item_first_get_time,             3000,   0,      INT_MAX,        },
	{ "item_second_get_time",               &battle_config.item_second_get_time,            1000,   0,      INT_MAX,        },
	{ "item_third_get_time",                &battle_config.item_third_get_time,             1000,   0,      INT_MAX,        },
	{ "mvp_item_first_get_time",            &battle_config.mvp_item_first_get_time,         10000,  0,      INT_MAX,        },
	{ "mvp_item_second_get_time",           &battle_config.mvp_item_second_get_time,        10000,  0,      INT_MAX,        },
	{ "mvp_item_third_get_time",            &battle_config.mvp_item_third_get_time,         2000,   0,      INT_MAX,        },
	{ "drop_rate0item",                     &battle_config.drop_rate0item,                  0,      0,      1,              },
	{ "base_exp_rate",                      &battle_config.base_exp_rate,                   100,    0,      INT_MAX,        },
	{ "job_exp_rate",                       &battle_config.job_exp_rate,                    100,    0,      INT_MAX,        },
	{ "pvp_exp",                            &battle_config.pvp_exp,                         1,      0,      1,              },
	{ "death_penalty_type",                 &battle_config.death_penalty_type,              0,      0,      2,              },
	{ "death_penalty_base",                 &battle_config.death_penalty_base,              0,      0,      INT_MAX,        },
	{ "death_penalty_job",                  &battle_config.death_penalty_job,               0,      0,      INT_MAX,        },
	{ "zeny_penalty",                       &battle_config.zeny_penalty,                    0,      0,      INT_MAX,        },
	{ "hp_rate",                            &battle_config.hp_rate,                         100,    1,      INT_MAX,        },
	{ "sp_rate",                            &battle_config.sp_rate,                         100,    1,      INT_MAX,        },
	{ "restart_hp_rate",                    &battle_config.restart_hp_rate,                 0,      0,      100,            },
	{ "restart_sp_rate",                    &battle_config.restart_sp_rate,                 0,      0,      100,            },
	{ "guild_aura",                         &battle_config.guild_aura,                      31,     0,      31,             },
	{ "mvp_hp_rate",                        &battle_config.mvp_hp_rate,                     100,    1,      INT_MAX,        },
	{ "mvp_exp_rate",                       &battle_config.mvp_exp_rate,                    100,    0,      INT_MAX,        },
	{ "monster_hp_rate",                    &battle_config.monster_hp_rate,                 100,    1,      INT_MAX,        },
	{ "monster_max_aspd",                   &battle_config.monster_max_aspd,                199,    100,    199,            },
	{ "view_range_rate",                    &battle_config.view_range_rate,                 100,    0,      INT_MAX,        },
	{ "chase_range_rate",                   &battle_config.chase_range_rate,                100,    0,      INT_MAX,        },
	{ "gtb_sc_immunity",                    &battle_config.gtb_sc_immunity,                 50,     0,      INT_MAX,        },
	{ "guild_max_castles",                  &battle_config.guild_max_castles,               0,      0,      INT_MAX,        },
	{ "guild_skill_relog_delay",            &battle_config.guild_skill_relog_delay,         0,      0,      1,              },
	{ "emergency_call",                     &battle_config.emergency_call,                  11,     0,      31,             },
	{ "atcommand_spawn_quantity_limit",     &battle_config.atc_spawn_quantity_limit,        100,    0,      INT_MAX,        },
	{ "atcommand_slave_clone_limit",        &battle_config.atc_slave_clone_limit,           25,     0,      INT_MAX,        },
	{ "partial_name_scan",                  &battle_config.partial_name_scan,               0,      0,      1,              },
	{ "player_skillfree",                   &battle_config.skillfree,                       0,      0,      1,              },
	{ "player_skillup_limit",               &battle_config.skillup_limit,                   1,      0,      1,              },
	{ "weapon_produce_rate",                &battle_config.wp_rate,                         100,    0,      INT_MAX,        },
	{ "potion_produce_rate",                &battle_config.pp_rate,                         100,    0,      INT_MAX,        },
	{ "monster_active_enable",              &battle_config.monster_active_enable,           1,      0,      1,              },
	{ "monster_damage_delay_rate",          &battle_config.monster_damage_delay_rate,       100,    0,      INT_MAX,        },
	{ "monster_loot_type",                  &battle_config.monster_loot_type,               0,      0,      1,              },
//	{ "mob_skill_use",                      &battle_config.mob_skill_use,                   1,      0,      1,              }, //Deprecated
	{ "mob_skill_rate",                     &battle_config.mob_skill_rate,                  100,    0,      INT_MAX,        },
	{ "mob_skill_delay",                    &battle_config.mob_skill_delay,                 100,    0,      INT_MAX,        },
	{ "mob_count_rate",                     &battle_config.mob_count_rate,                  100,    0,      INT_MAX,        },
	{ "mob_spawn_delay",                    &battle_config.mob_spawn_delay,                 100,    0,      INT_MAX,        },
	{ "plant_spawn_delay",                  &battle_config.plant_spawn_delay,               100,    0,      INT_MAX,        },
	{ "boss_spawn_delay",                   &battle_config.boss_spawn_delay,                100,    0,      INT_MAX,        },
	{ "no_spawn_on_player",                 &battle_config.no_spawn_on_player,              0,      0,      100,            },
	{ "force_random_spawn",                 &battle_config.force_random_spawn,              0,      0,      1,              },
	{ "slaves_inherit_mode",                &battle_config.slaves_inherit_mode,             2,      0,      3,              },
	{ "slaves_inherit_speed",               &battle_config.slaves_inherit_speed,            3,      0,      3,              },
	{ "summons_trigger_autospells",         &battle_config.summons_trigger_autospells,      1,      0,      1,              },
	{ "pc_damage_walk_delay_rate",          &battle_config.pc_walk_delay_rate,              20,     0,      INT_MAX,        },
	{ "damage_walk_delay_rate",             &battle_config.walk_delay_rate,                 100,    0,      INT_MAX,        },
	{ "multihit_delay",                     &battle_config.multihit_delay,                  80,     0,      INT_MAX,        },
	{ "quest_skill_learn",                  &battle_config.quest_skill_learn,               0,      0,      1,              },
	{ "quest_skill_reset",                  &battle_config.quest_skill_reset,               0,      0,      1,              },
	{ "basic_skill_check",                  &battle_config.basic_skill_check,               1,      0,      1,              },
	{ "guild_emperium_check",               &battle_config.guild_emperium_check,            1,      0,      1,              },
	{ "guild_exp_limit",                    &battle_config.guild_exp_limit,                 50,     0,      99,             },
	{ "player_invincible_time",             &battle_config.pc_invincible_time,              5000,   0,      INT_MAX,        },
	{ "pet_catch_rate",                     &battle_config.pet_catch_rate,                  100,    0,      INT_MAX,        },
	{ "pet_rename",                         &battle_config.pet_rename,                      0,      0,      1,              },
	{ "pet_friendly_rate",                  &battle_config.pet_friendly_rate,               100,    0,      INT_MAX,        },
	{ "pet_hungry_delay_rate",              &battle_config.pet_hungry_delay_rate,           100,    10,     INT_MAX,        },
	{ "pet_hungry_friendly_decrease",       &battle_config.pet_hungry_friendly_decrease,    5,      0,      INT_MAX,        },
	{ "pet_status_support",                 &battle_config.pet_status_support,              0,      0,      1,              },
	{ "pet_attack_support",                 &battle_config.pet_attack_support,              0,      0,      1,              },
	{ "pet_damage_support",                 &battle_config.pet_damage_support,              0,      0,      1,              },
	{ "pet_support_min_friendly",           &battle_config.pet_support_min_friendly,        900,    0,      950,            },
	{ "pet_equip_min_friendly",             &battle_config.pet_equip_min_friendly,          900,    0,      950,            },
	{ "pet_support_rate",                   &battle_config.pet_support_rate,                100,    0,      INT_MAX,        },
	{ "pet_attack_exp_to_master",           &battle_config.pet_attack_exp_to_master,        0,      0,      1,              },
	{ "pet_attack_exp_rate",                &battle_config.pet_attack_exp_rate,             100,    0,      INT_MAX,        },
	{ "pet_lv_rate",                        &battle_config.pet_lv_rate,                     0,      0,      INT_MAX,        },
	{ "pet_max_stats",                      &battle_config.pet_max_stats,                   99,     0,      INT_MAX,        },
	{ "pet_max_atk1",                       &battle_config.pet_max_atk1,                    750,    0,      INT_MAX,        },
	{ "pet_max_atk2",                       &battle_config.pet_max_atk2,                    1000,   0,      INT_MAX,        },
	{ "pet_disable_in_gvg",                 &battle_config.pet_no_gvg,                      0,      0,      1,              },
	{ "skill_min_damage",                   &battle_config.skill_min_damage,                2|4,    0,      1|2|4,          },
	{ "finger_offensive_type",              &battle_config.finger_offensive_type,           0,      0,      1,              },
	{ "heal_exp",                           &battle_config.heal_exp,                        0,      0,      INT_MAX,        },
	{ "resurrection_exp",                   &battle_config.resurrection_exp,                0,      0,      INT_MAX,        },
	{ "shop_exp",                           &battle_config.shop_exp,                        0,      0,      INT_MAX,        },
	{ "max_heal_lv",                        &battle_config.max_heal_lv,                     11,     1,      INT_MAX,        },
	{ "max_heal",                           &battle_config.max_heal,                        9999,   0,      INT_MAX,        },
	{ "combo_delay_rate",                   &battle_config.combo_delay_rate,                100,    0,      INT_MAX,        },
	{ "item_check",                         &battle_config.item_check,                      0,      0,      1,              },
	{ "item_use_interval",                  &battle_config.item_use_interval,               100,    0,      INT_MAX,        },
	{ "cashfood_use_interval",              &battle_config.cashfood_use_interval,           60000,  0,      INT_MAX,        },
	{ "wedding_modifydisplay",              &battle_config.wedding_modifydisplay,           0,      0,      1,              },
	{ "wedding_ignorepalette",              &battle_config.wedding_ignorepalette,           0,      0,      1,              },
	{ "xmas_ignorepalette",                 &battle_config.xmas_ignorepalette,              0,      0,      1,              },
	{ "summer_ignorepalette",               &battle_config.summer_ignorepalette,            0,      0,      1,              },
	{ "natural_healhp_interval",            &battle_config.natural_healhp_interval,         6000,   NATURAL_HEAL_INTERVAL, INT_MAX, },
	{ "natural_healsp_interval",            &battle_config.natural_healsp_interval,         8000,   NATURAL_HEAL_INTERVAL, INT_MAX, },
	{ "natural_heal_skill_interval",        &battle_config.natural_heal_skill_interval,     10000,  NATURAL_HEAL_INTERVAL, INT_MAX, },
	{ "natural_heal_weight_rate",           &battle_config.natural_heal_weight_rate,        50,     50,     101             },
	{ "arrow_decrement",                    &battle_config.arrow_decrement,                 1,      0,      2,              },
	{ "max_aspd",                           &battle_config.max_aspd,                        190,    100,    199,            },
	{ "max_third_aspd",                     &battle_config.max_third_aspd,                  193,    100,    199,            },
	{ "max_walk_speed",                     &battle_config.max_walk_speed,                  300,    100,    100*DEFAULT_WALK_SPEED, },
	{ "max_lv",                             &battle_config.max_lv,                          99,     0,      MAX_LEVEL,      },
	{ "aura_lv",                            &battle_config.aura_lv,                         99,     0,      INT_MAX,        },
	{ "max_hp",                             &battle_config.max_hp,                          32500,  100,    1000000000,     },
	{ "max_sp",                             &battle_config.max_sp,                          32500,  100,    1000000000,     },
	{ "max_cart_weight",                    &battle_config.max_cart_weight,                 8000,   100,    1000000,        },
	{ "max_parameter",                      &battle_config.max_parameter,                   99,     10,     10000,          },
	{ "max_baby_parameter",                 &battle_config.max_baby_parameter,              80,     10,     10000,          },
	{ "max_def",                            &battle_config.max_def,                         99,     0,      INT_MAX,        },
	{ "over_def_bonus",                     &battle_config.over_def_bonus,                  0,      0,      1000,           },
	{ "skill_log",                          &battle_config.skill_log,                       BL_NUL, BL_NUL, BL_ALL,         },
	{ "battle_log",                         &battle_config.battle_log,                      0,      0,      1,              },
	{ "etc_log",                            &battle_config.etc_log,                         1,      0,      1,              },
	{ "save_clothcolor",                    &battle_config.save_clothcolor,                 1,      0,      1,              },
	{ "undead_detect_type",                 &battle_config.undead_detect_type,              0,      0,      2,              },
	{ "auto_counter_type",                  &battle_config.auto_counter_type,               BL_ALL, BL_NUL, BL_ALL,         },
	{ "min_hitrate",                        &battle_config.min_hitrate,                     5,      0,      100,            },
	{ "max_hitrate",                        &battle_config.max_hitrate,                     100,    0,      100,            },
	{ "agi_penalty_target",                 &battle_config.agi_penalty_target,              BL_PC,  BL_NUL, BL_ALL,         },
	{ "agi_penalty_type",                   &battle_config.agi_penalty_type,                1,      0,      2,              },
	{ "agi_penalty_count",                  &battle_config.agi_penalty_count,               3,      2,      INT_MAX,        },
	{ "agi_penalty_num",                    &battle_config.agi_penalty_num,                 10,     0,      INT_MAX,        },
	{ "vit_penalty_target",                 &battle_config.vit_penalty_target,              BL_PC,  BL_NUL, BL_ALL,         },
	{ "vit_penalty_type",                   &battle_config.vit_penalty_type,                1,      0,      2,              },
	{ "vit_penalty_count",                  &battle_config.vit_penalty_count,               3,      2,      INT_MAX,        },
	{ "vit_penalty_num",                    &battle_config.vit_penalty_num,                 5,      0,      INT_MAX,        },
	{ "weapon_defense_type",                &battle_config.weapon_defense_type,             0,      0,      INT_MAX,        },
	{ "magic_defense_type",                 &battle_config.magic_defense_type,              0,      0,      INT_MAX,        },
	{ "skill_reiteration",                  &battle_config.skill_reiteration,               BL_NUL, BL_NUL, BL_ALL,         },
	{ "skill_nofootset",                    &battle_config.skill_nofootset,                 BL_PC,  BL_NUL, BL_ALL,         },
	{ "player_cloak_check_type",            &battle_config.pc_cloak_check_type,             1,      0,      1|2|4,          },
	{ "monster_cloak_check_type",           &battle_config.monster_cloak_check_type,        4,      0,      1|2|4,          },
	{ "sense_type",                         &battle_config.estimation_type,                 1|2,    0,      1|2,            },
	{ "gvg_short_attack_damage_rate",       &battle_config.gvg_short_damage_rate,           80,     0,      INT_MAX,        },
	{ "gvg_long_attack_damage_rate",        &battle_config.gvg_long_damage_rate,            80,     0,      INT_MAX,        },
	{ "gvg_weapon_attack_damage_rate",      &battle_config.gvg_weapon_damage_rate,          60,     0,      INT_MAX,        },
	{ "gvg_magic_attack_damage_rate",       &battle_config.gvg_magic_damage_rate,           60,     0,      INT_MAX,        },
	{ "gvg_misc_attack_damage_rate",        &battle_config.gvg_misc_damage_rate,            60,     0,      INT_MAX,        },
	{ "gvg_flee_penalty",                   &battle_config.gvg_flee_penalty,                20,     0,      INT_MAX,        },
	{ "pk_short_attack_damage_rate",        &battle_config.pk_short_damage_rate,            80,     0,      INT_MAX,        },
	{ "pk_long_attack_damage_rate",         &battle_config.pk_long_damage_rate,             70,     0,      INT_MAX,        },
	{ "pk_weapon_attack_damage_rate",       &battle_config.pk_weapon_damage_rate,           60,     0,      INT_MAX,        },
	{ "pk_magic_attack_damage_rate",        &battle_config.pk_magic_damage_rate,            60,     0,      INT_MAX,        },
	{ "pk_misc_attack_damage_rate",         &battle_config.pk_misc_damage_rate,             60,     0,      INT_MAX,        },
	{ "mob_changetarget_byskill",           &battle_config.mob_changetarget_byskill,        0,      0,      1,              },
	{ "attack_direction_change",            &battle_config.attack_direction_change,         BL_ALL, BL_NUL, BL_ALL,         },
	{ "land_skill_limit",                   &battle_config.land_skill_limit,                BL_ALL, BL_NUL, BL_ALL,         },
	{ "monster_class_change_full_recover",  &battle_config.monster_class_change_recover,    1,      0,      1,              },
	{ "produce_item_name_input",            &battle_config.produce_item_name_input,         0x1|0x2, 0,     0x9F,           },
	{ "display_skill_fail",                 &battle_config.display_skill_fail,              2,      0,      1|2|4|8,        },
	{ "chat_warpportal",                    &battle_config.chat_warpportal,                 0,      0,      1,              },
	{ "mob_warp",                           &battle_config.mob_warp,                        0,      0,      1|2|4,          },
	{ "dead_branch_active",                 &battle_config.dead_branch_active,              1,      0,      1,              },
	{ "vending_max_value",                  &battle_config.vending_max_value,               10000000, 1,    MAX_ZENY,       },
	{ "vending_over_max",                   &battle_config.vending_over_max,                1,      0,      1,              },
	{ "show_steal_in_same_party",           &battle_config.show_steal_in_same_party,        0,      0,      1,              },
	{ "party_hp_mode",                      &battle_config.party_hp_mode,                   0,      0,      1,              },
	{ "show_party_share_picker",            &battle_config.party_show_share_picker,         1,      0,      1,              },
	{ "show_picker.item_type",              &battle_config.show_picker_item_type,           112,    0,      INT_MAX,        },
	{ "party_update_interval",              &battle_config.party_update_interval,           1000,   100,    INT_MAX,        },
	{ "party_item_share_type",              &battle_config.party_share_type,                0,      0,      1|2|3,          },
	{ "attack_attr_none",                   &battle_config.attack_attr_none,                ~BL_PC, BL_NUL, BL_ALL,         },
	{ "gx_allhit",                          &battle_config.gx_allhit,                       0,      0,      1,              },
	{ "gx_disptype",                        &battle_config.gx_disptype,                     1,      0,      1,              },
	{ "devotion_level_difference",          &battle_config.devotion_level_difference,       10,     0,      INT_MAX,        },
	{ "player_skill_partner_check",         &battle_config.player_skill_partner_check,      1,      0,      1,              },
	{ "invite_request_check",               &battle_config.invite_request_check,            1,      0,      1,              },
	{ "skill_removetrap_type",              &battle_config.skill_removetrap_type,           0,      0,      1,              },
	{ "disp_experience",                    &battle_config.disp_experience,                 0,      0,      1,              },
	{ "disp_zeny",                          &battle_config.disp_zeny,                       0,      0,      1,              },
	{ "castle_defense_rate",                &battle_config.castle_defense_rate,             100,    0,      100,            },
	{ "bone_drop",                          &battle_config.bone_drop,                       0,      0,      2,              },
	{ "buyer_name",                         &battle_config.buyer_name,                      1,      0,      1,              },
	{ "skill_wall_check",                   &battle_config.skill_wall_check,                1,      0,      1,              },
	{ "cell_stack_limit",                   &battle_config.cell_stack_limit,                1,      1,      255,            },
	{ "dancing_weaponswitch_fix",           &battle_config.dancing_weaponswitch_fix,        1,      0,      1,              },
	
	// eAthena additions
	{ "item_logarithmic_drops",             &battle_config.logarithmic_drops,               0,      0,      1,              },
	{ "item_drop_common_min",               &battle_config.item_drop_common_min,            1,      1,      10000,          },
	{ "item_drop_common_max",               &battle_config.item_drop_common_max,            10000,  1,      10000,          },
	{ "item_drop_equip_min",                &battle_config.item_drop_equip_min,             1,      1,      10000,          },
	{ "item_drop_equip_max",                &battle_config.item_drop_equip_max,             10000,  1,      10000,          },
	{ "item_drop_card_min",                 &battle_config.item_drop_card_min,              1,      1,      10000,          },
	{ "item_drop_card_max",                 &battle_config.item_drop_card_max,              10000,  1,      10000,          },
	{ "item_drop_mvp_min",                  &battle_config.item_drop_mvp_min,               1,      1,      10000,          },
	{ "item_drop_mvp_max",                  &battle_config.item_drop_mvp_max,               10000,  1,      10000,          },
	{ "item_drop_heal_min",                 &battle_config.item_drop_heal_min,              1,      1,      10000,          },
	{ "item_drop_heal_max",                 &battle_config.item_drop_heal_max,              10000,  1,      10000,          },
	{ "item_drop_use_min",                  &battle_config.item_drop_use_min,               1,      1,      10000,          },
	{ "item_drop_use_max",                  &battle_config.item_drop_use_max,               10000,  1,      10000,          },
	{ "item_drop_add_min",                  &battle_config.item_drop_adddrop_min,           1,      1,      10000,          },
	{ "item_drop_add_max",                  &battle_config.item_drop_adddrop_max,           10000,  1,      10000,          },
	{ "item_drop_treasure_min",             &battle_config.item_drop_treasure_min,          1,      1,      10000,          },
	{ "item_drop_treasure_max",             &battle_config.item_drop_treasure_max,          10000,  1,      10000,          },
	{ "item_rate_mvp",                      &battle_config.item_rate_mvp,                   100,    0,      1000000,        },
	{ "item_rate_common",                   &battle_config.item_rate_common,                100,    0,      1000000,        },
	{ "item_rate_common_boss",              &battle_config.item_rate_common_boss,           100,    0,      1000000,        },
	{ "item_rate_equip",                    &battle_config.item_rate_equip,                 100,    0,      1000000,        },
	{ "item_rate_equip_boss",               &battle_config.item_rate_equip_boss,            100,    0,      1000000,        },
	{ "item_rate_card",                     &battle_config.item_rate_card,                  100,    0,      1000000,        },
	{ "item_rate_card_boss",                &battle_config.item_rate_card_boss,             100,    0,      1000000,        },
	{ "item_rate_heal",                     &battle_config.item_rate_heal,                  100,    0,      1000000,        },
	{ "item_rate_heal_boss",                &battle_config.item_rate_heal_boss,             100,    0,      1000000,        },
	{ "item_rate_use",                      &battle_config.item_rate_use,                   100,    0,      1000000,        },
	{ "item_rate_use_boss",                 &battle_config.item_rate_use_boss,              100,    0,      1000000,        },
	{ "item_rate_adddrop",                  &battle_config.item_rate_adddrop,               100,    0,      1000000,        },
	{ "item_rate_treasure",                 &battle_config.item_rate_treasure,              100,    0,      1000000,        },
	{ "prevent_logout",                     &battle_config.prevent_logout,                  10000,  0,      60000,          },
	{ "alchemist_summon_reward",            &battle_config.alchemist_summon_reward,         1,      0,      2,              },
	{ "drops_by_luk",                       &battle_config.drops_by_luk,                    0,      0,      INT_MAX,        },
	{ "drops_by_luk2",                      &battle_config.drops_by_luk2,                   0,      0,      INT_MAX,        },
	{ "equip_natural_break_rate",           &battle_config.equip_natural_break_rate,        0,      0,      INT_MAX,        },
	{ "equip_self_break_rate",              &battle_config.equip_self_break_rate,           100,    0,      INT_MAX,        },
	{ "equip_skill_break_rate",             &battle_config.equip_skill_break_rate,          100,    0,      INT_MAX,        },
	{ "pk_mode",                            &battle_config.pk_mode,                         0,      0,      2,              },
	{ "pk_level_range",                     &battle_config.pk_level_range,                  0,      0,      INT_MAX,        },
	{ "manner_system",                      &battle_config.manner_system,                   0xFFF,  0,      0xFFF,          },
	{ "pet_equip_required",                 &battle_config.pet_equip_required,              0,      0,      1,              },
	{ "multi_level_up",                     &battle_config.multi_level_up,                  0,      0,      1,              },
	{ "max_exp_gain_rate",                  &battle_config.max_exp_gain_rate,               0,      0,      INT_MAX,        },
	{ "backstab_bow_penalty",               &battle_config.backstab_bow_penalty,            0,      0,      1,              },
	{ "night_at_start",                     &battle_config.night_at_start,                  0,      0,      1,              },
	{ "show_mob_info",                      &battle_config.show_mob_info,                   0,      0,      1|2|4,          },
	{ "ban_hack_trade",                     &battle_config.ban_hack_trade,                  0,      0,      INT_MAX,        },
	{ "packet_ver_flag",                    &battle_config.packet_ver_flag,                 0xFFFFFF,0x0000,INT_MAX,        },
	{ "min_hair_style",                     &battle_config.min_hair_style,                  0,      0,      INT_MAX,        },
	{ "max_hair_style",                     &battle_config.max_hair_style,                  23,     0,      INT_MAX,        },
	{ "min_hair_color",                     &battle_config.min_hair_color,                  0,      0,      INT_MAX,        },
	{ "max_hair_color",                     &battle_config.max_hair_color,                  9,      0,      INT_MAX,        },
	{ "min_cloth_color",                    &battle_config.min_cloth_color,                 0,      0,      INT_MAX,        },
	{ "max_cloth_color",                    &battle_config.max_cloth_color,                 4,      0,      INT_MAX,        },
	{ "pet_hair_style",                     &battle_config.pet_hair_style,                  100,    0,      INT_MAX,        },
	{ "castrate_dex_scale",                 &battle_config.castrate_dex_scale,              150,    1,      INT_MAX,        },
	{ "vcast_stat_scale",                   &battle_config.vcast_stat_scale,                530,    1,      INT_MAX,        },
	{ "area_size",                          &battle_config.area_size,                       14,     0,      INT_MAX,        },
	{ "zeny_from_mobs",                     &battle_config.zeny_from_mobs,                  0,      0,      1,              },
	{ "mobs_level_up",                      &battle_config.mobs_level_up,                   0,      0,      1,              },
	{ "mobs_level_up_exp_rate",             &battle_config.mobs_level_up_exp_rate,          1,      1,      INT_MAX,        },
	{ "pk_min_level",                       &battle_config.pk_min_level,                    55,     1,      INT_MAX,        },
	{ "skill_steal_max_tries",              &battle_config.skill_steal_max_tries,           0,      0,      UCHAR_MAX,      },
	{ "motd_type",                          &battle_config.motd_type,                       0,      0,      1,              },
	{ "finding_ore_rate",                   &battle_config.finding_ore_rate,                100,    0,      INT_MAX,        },
	{ "exp_calc_type",                      &battle_config.exp_calc_type,                   0,      0,      1,              },
	{ "exp_bonus_attacker",                 &battle_config.exp_bonus_attacker,              25,     0,      INT_MAX,        },
	{ "exp_bonus_max_attacker",             &battle_config.exp_bonus_max_attacker,          12,     2,      INT_MAX,        },
	{ "min_skill_delay_limit",              &battle_config.min_skill_delay_limit,           100,    10,     INT_MAX,        },
	{ "default_walk_delay",                 &battle_config.default_walk_delay,              300,    0,      INT_MAX,        },
	{ "no_skill_delay",                     &battle_config.no_skill_delay,                  BL_MOB, BL_NUL, BL_ALL,         },
	{ "attack_walk_delay",                  &battle_config.attack_walk_delay,               BL_ALL, BL_NUL, BL_ALL,         },
	{ "require_glory_guild",                &battle_config.require_glory_guild,             0,      0,      1,              },
	{ "idle_no_share",                      &battle_config.idle_no_share,                   0,      0,      INT_MAX,        },
	{ "party_even_share_bonus",             &battle_config.party_even_share_bonus,          0,      0,      INT_MAX,        },
	{ "delay_battle_damage",                &battle_config.delay_battle_damage,             1,      0,      1,              },
	{ "hide_woe_damage",                    &battle_config.hide_woe_damage,                 0,      0,      1,              },
	{ "display_version",                    &battle_config.display_version,                 1,      0,      1,              },
	{ "display_hallucination",              &battle_config.display_hallucination,           1,      0,      1,              },
	{ "use_statpoint_table",                &battle_config.use_statpoint_table,             1,      0,      1,              },
	{ "ignore_items_gender",                &battle_config.ignore_items_gender,             1,      0,      1,              },
	{ "copyskill_restrict",                 &battle_config.copyskill_restrict,              2,      0,      2,              },
	{ "berserk_cancels_buffs",              &battle_config.berserk_cancels_buffs,           0,      0,      1,              },
	{ "debuff_on_logout",                   &battle_config.debuff_on_logout,                1|2,    0,      1|2,            },
	{ "monster_ai",                         &battle_config.mob_ai,                          0x000,  0x000,  0x77F,          },
	{ "hom_setting",                        &battle_config.hom_setting,                     0xFFFF, 0x0000, 0xFFFF,         },
	{ "dynamic_mobs",                       &battle_config.dynamic_mobs,                    1,      0,      1,              },
	{ "mob_remove_damaged",                 &battle_config.mob_remove_damaged,              1,      0,      1,              },
	{ "show_hp_sp_drain",                   &battle_config.show_hp_sp_drain,                0,      0,      1,              },
	{ "show_hp_sp_gain",                    &battle_config.show_hp_sp_gain,                 1,      0,      1,              },
	{ "mob_npc_event_type",                 &battle_config.mob_npc_event_type,              1,      0,      1,              },
	{ "character_size",                     &battle_config.character_size,                  1|2,    0,      1|2,            },
	{ "mob_max_skilllvl",                   &battle_config.mob_max_skilllvl,                MAX_SKILL_LEVEL, 1, MAX_SKILL_LEVEL, },
	{ "retaliate_to_master",                &battle_config.retaliate_to_master,             1,      0,      1,              },
	{ "rare_drop_announce",                 &battle_config.rare_drop_announce,              0,      0,      10000,          },
	{ "duel_allow_pvp",                     &battle_config.duel_allow_pvp,                  0,      0,      1,              },
	{ "duel_allow_gvg",                     &battle_config.duel_allow_gvg,                  0,      0,      1,              },
	{ "duel_allow_teleport",                &battle_config.duel_allow_teleport,             0,      0,      1,              },
	{ "duel_autoleave_when_die",            &battle_config.duel_autoleave_when_die,         1,      0,      1,              },
	{ "duel_time_interval",                 &battle_config.duel_time_interval,              60,     0,      INT_MAX,        },
	{ "duel_only_on_same_map",              &battle_config.duel_only_on_same_map,           0,      0,      1,              },
	{ "skip_teleport_lv1_menu",             &battle_config.skip_teleport_lv1_menu,          0,      0,      1,              },
	{ "allow_skill_without_day",            &battle_config.allow_skill_without_day,         0,      0,      1,              },
	{ "allow_es_magic_player",              &battle_config.allow_es_magic_pc,               0,      0,      1,              },
	{ "skill_caster_check",                 &battle_config.skill_caster_check,              1,      0,      1,              },
	{ "status_cast_cancel",                 &battle_config.sc_castcancel,                   BL_NUL, BL_NUL, BL_ALL,         },
	{ "pc_status_def_rate",                 &battle_config.pc_sc_def_rate,                  100,    0,      INT_MAX,        },
	{ "mob_status_def_rate",                &battle_config.mob_sc_def_rate,                 100,    0,      INT_MAX,        },
	{ "pc_max_status_def",                  &battle_config.pc_max_sc_def,                   100,    0,      INT_MAX,        },
	{ "mob_max_status_def",                 &battle_config.mob_max_sc_def,                  100,    0,      INT_MAX,        },
	{ "sg_miracle_skill_ratio",             &battle_config.sg_miracle_skill_ratio,          1,      0,      10000,          },
	{ "sg_angel_skill_ratio",               &battle_config.sg_angel_skill_ratio,            10,     0,      10000,          },
	{ "autospell_stacking",                 &battle_config.autospell_stacking,              0,      0,      1,              },
	{ "override_mob_names",                 &battle_config.override_mob_names,              0,      0,      2,              },
	{ "min_chat_delay",                     &battle_config.min_chat_delay,                  0,      0,      INT_MAX,        },
	{ "friend_auto_add",                    &battle_config.friend_auto_add,                 1,      0,      1,              },
	{ "hom_rename",                         &battle_config.hom_rename,                      0,      0,      1,              },
	{ "homunculus_show_growth",             &battle_config.homunculus_show_growth,          0,      0,      1,              },
	{ "homunculus_friendly_rate",           &battle_config.homunculus_friendly_rate,        100,    0,      INT_MAX,        },
	{ "vending_tax",                        &battle_config.vending_tax,                     0,      0,      10000,          },
	{ "day_duration",                       &battle_config.day_duration,                    0,      0,      INT_MAX,        },
	{ "night_duration",                     &battle_config.night_duration,                  0,      0,      INT_MAX,        },
	{ "mob_remove_delay",                   &battle_config.mob_remove_delay,                60000,  1000,   INT_MAX,        },
	{ "mob_active_time",                    &battle_config.mob_active_time,                 0,      0,      INT_MAX,        },
	{ "boss_active_time",                   &battle_config.boss_active_time,                0,      0,      INT_MAX,        },
	{ "sg_miracle_skill_duration",          &battle_config.sg_miracle_skill_duration,       3600000, 0,     INT_MAX,        },
	{ "hvan_explosion_intimate",            &battle_config.hvan_explosion_intimate,         45000,  0,      100000,         },
	{ "quest_exp_rate",                     &battle_config.quest_exp_rate,                  100,    0,      INT_MAX,        },
	{ "at_mapflag",                         &battle_config.autotrade_mapflag,               0,      0,      1,              },
	{ "at_timeout",                         &battle_config.at_timeout,                      0,      0,      INT_MAX,        },
	{ "homunculus_autoloot",                &battle_config.homunculus_autoloot,             0,      0,      1,              },
	{ "idle_no_autoloot",                   &battle_config.idle_no_autoloot,                0,      0,      INT_MAX,        },
	{ "max_guild_alliance",                 &battle_config.max_guild_alliance,              3,      0,      3,              },
	{ "ksprotection",                       &battle_config.ksprotection,                    5000,   0,      INT_MAX,        },
	{ "auction_feeperhour",                 &battle_config.auction_feeperhour,              12000,  0,      INT_MAX,        },
	{ "auction_maximumprice",               &battle_config.auction_maximumprice,            500000000, 0,   MAX_ZENY,       },
	{ "homunculus_auto_vapor",              &battle_config.homunculus_auto_vapor,           1,      0,      1,              },
	{ "display_status_timers",              &battle_config.display_status_timers,           1,      0,      1,              },
	{ "skill_add_heal_rate",                &battle_config.skill_add_heal_rate,             7,      0,      INT_MAX,        },
	{ "eq_single_target_reflectable",       &battle_config.eq_single_target_reflectable,    1,      0,      1,              },
	{ "invincible.nodamage",                &battle_config.invincible_nodamage,             0,      0,      1,              },
	{ "mob_slave_keep_target",              &battle_config.mob_slave_keep_target,           0,      0,      1,              },
	{ "autospell_check_range",              &battle_config.autospell_check_range,           0,      0,      1,              },
	{ "client_reshuffle_dice",              &battle_config.client_reshuffle_dice,           0,      0,      1,              },
	{ "client_sort_storage",                &battle_config.client_sort_storage,             0,      0,      1,              },
	{ "feature.buying_store",               &battle_config.feature_buying_store,            1,      0,      1,              },
	{ "feature.search_stores",              &battle_config.feature_search_stores,           1,      0,      1,              },
	{ "searchstore_querydelay",             &battle_config.searchstore_querydelay,         10,      0,      INT_MAX,        },
	{ "searchstore_maxresults",             &battle_config.searchstore_maxresults,         30,      1,      INT_MAX,        },
	{ "display_party_name",                 &battle_config.display_party_name,              0,      0,      1,              },
	{ "cashshop_show_points",               &battle_config.cashshop_show_points,            0,      0,      1,              },
	{ "mail_show_status",                   &battle_config.mail_show_status,                0,      0,      2,              },
	{ "client_limit_unit_lv",               &battle_config.client_limit_unit_lv,            0,      0,      BL_ALL,         },
	// BattleGround Settings
	{ "bg_update_interval",                 &battle_config.bg_update_interval,              1000,   100,    INT_MAX,        },
	{ "bg_short_attack_damage_rate",        &battle_config.bg_short_damage_rate,            80,     0,      INT_MAX,        },
	{ "bg_long_attack_damage_rate",         &battle_config.bg_long_damage_rate,             80,     0,      INT_MAX,        },
	{ "bg_weapon_attack_damage_rate",       &battle_config.bg_weapon_damage_rate,           60,     0,      INT_MAX,        },
	{ "bg_magic_attack_damage_rate",        &battle_config.bg_magic_damage_rate,            60,     0,      INT_MAX,        },
	{ "bg_misc_attack_damage_rate",         &battle_config.bg_misc_damage_rate,             60,     0,      INT_MAX,        },
	{ "bg_flee_penalty",                    &battle_config.bg_flee_penalty,                 20,     0,      INT_MAX,        },
	/**
	 * rAthena
	 **/
	{ "max_third_parameter",                &battle_config.max_third_parameter,             120,    10,     10000,          },
	{ "max_baby_third_parameter",           &battle_config.max_baby_third_parameter,        108,    10,     10000,          },
	{ "atcommand_max_stat_bypass",          &battle_config.atcommand_max_stat_bypass,       0,      0,      100,            },
	{ "skill_amotion_leniency",             &battle_config.skill_amotion_leniency,          90,     0,      300,            },
	{ "mvp_tomb_enabled",                   &battle_config.mvp_tomb_enabled,                1,      0,      1,              },
	{ "feature.atcommand_suggestions",      &battle_config.atcommand_suggestions_enabled,   0,      0,      1,              },
	{ "min_npc_vendchat_distance",          &battle_config.min_npc_vendchat_distance,       3,      0,      100,            },
	{ "atcommand_mobinfo_type",             &battle_config.atcommand_mobinfo_type,          0,      0,      1,              },
	{ "homunculus_max_level",               &battle_config.hom_max_level,                   99,     0,      MAX_LEVEL,      },
	{ "homunculus_S_max_level",             &battle_config.hom_S_max_level,                 150,    0,      MAX_LEVEL,      },
	{ "mob_size_influence",                 &battle_config.mob_size_influence,              0,      0,      1,              },
	{ "hanbok_ignorepalette",               &battle_config.hanbok_ignorepalette,            0,      0,      1,              },
	/**
	* Hercules
	**/
	{ "skill_trap_type",                    &battle_config.skill_trap_type,                 0,      0,      1,              },
	{ "item_restricted_consumption_type",   &battle_config.item_restricted_consumption_type,1,      0,      1,              },
	{ "max_walk_path",                      &battle_config.max_walk_path,                  17,      1,      MAX_WALKPATH,   },
	{ "item_enabled_npc",                   &battle_config.item_enabled_npc,                1,      0,      1,              },
};
#ifndef STATS_OPT_OUT
/**
 * rAthena anonymous statistic usage report -- packet is built here, and sent to char server to report.
 **/
void rAthena_report(char* date, char *time_c) {
	int i, rev = 0, bd_size = ARRAYLENGTH(battle_data);
	unsigned int config = 0;
	const char* rev_str;
	char timestring[25];
	time_t curtime;
	char* buf;

	enum config_table {
		C_CIRCULAR_AREA         = 0x0001,
		C_CELLNOSTACK           = 0x0002,
		C_BETA_THREAD_TEST      = 0x0004,
		C_SCRIPT_CALLFUNC_CHECK = 0x0008,
		C_OFFICIAL_WALKPATH     = 0x0010,
		C_RENEWAL               = 0x0020,
		C_RENEWAL_CAST          = 0x0040,
		C_RENEWAL_DROP          = 0x0080,
		C_RENEWAL_EXP           = 0x0100,
		C_RENEWAL_LVDMG         = 0x0200,
		C_RENEWAL_EDP           = 0x0400,
		C_RENEWAL_ASPD          = 0x0800,
		C_SECURE_NPCTIMEOUT     = 0x1000,
		C_SQL_DBS               = 0x2000,
		C_SQL_LOGS              = 0x4000,
	};
		
	if( (rev_str = get_svn_revision()) != 0 )
		rev = atoi(rev_str);
	
	/* we get the current time */
	time(&curtime);
	strftime(timestring, 24, "%Y-%m-%d %H:%M:%S", localtime(&curtime));
	
	
#ifdef CIRCULAR_AREA
	config |= C_CIRCULAR_AREA;
#endif
	
#ifdef CELL_NOSTACK
	config |= C_CELLNOSTACK;
#endif
	
#ifdef BETA_THREAD_TEST
	config |= C_BETA_THREAD_TEST;
#endif

#ifdef SCRIPT_CALLFUNC_CHECK
	config |= C_SCRIPT_CALLFUNC_CHECK;
#endif

#ifdef OFFICIAL_WALKPATH
	config |= C_OFFICIAL_WALKPATH;
#endif

#ifdef RENEWAL
	config |= C_RENEWAL;
#endif
	
#ifdef RENEWAL_CAST
	config |= C_RENEWAL_CAST;
#endif

#ifdef RENEWAL_DROP
	config |= C_RENEWAL_DROP;
#endif

#ifdef RENEWAL_EXP
	config |= C_RENEWAL_EXP;
#endif
	
#ifdef RENEWAL_LVDMG
	config |= C_RENEWAL_LVDMG;
#endif

#ifdef RENEWAL_EDP
	config |= C_RENEWAL_EDP;
#endif
	
#ifdef RENEWAL_ASPD
	config |= C_RENEWAL_ASPD;
#endif
	
#ifdef SECURE_NPCTIMEOUT
	config |= C_SECURE_NPCTIMEOUT;
#endif

	/* non-define part */
	if( db_use_sqldbs )
		config |= C_SQL_DBS;
	
	if( log_config.sql_logs )
		config |= C_SQL_LOGS;
	
#define BFLAG_LENGTH 35
	
	CREATE(buf, char, 6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( bd_size * ( BFLAG_LENGTH + 4 ) ) + 1 );
	
	/* build packet */

	WBUFW(buf,0) = 0x3000;
	WBUFW(buf,2) = 6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( bd_size * ( BFLAG_LENGTH + 4 ) );
	WBUFW(buf,4) = 0x9c;

	safestrncpy((char*)WBUFP(buf,6), date, 12);
	safestrncpy((char*)WBUFP(buf,6 + 12), time_c, 9);
	safestrncpy((char*)WBUFP(buf,6 + 12 + 9), timestring, 24);
	
	WBUFL(buf,6 + 12 + 9 + 24)         = rev;
	WBUFL(buf,6 + 12 + 9 + 24 + 4)     = map_getusers();
	
	WBUFL(buf,6 + 12 + 9 + 24 + 4 + 4) = config;
	WBUFL(buf,6 + 12 + 9 + 24 + 4 + 4 + 4) = bd_size;
	
	for( i = 0; i < bd_size; i++ ) {
		safestrncpy((char*)WBUFP(buf,6 + 12 + 9+ 24  + 4 + 4 + 4 + 4 + ( i * ( BFLAG_LENGTH + 4 ) ) ), battle_data[i].str, 35);
		WBUFL(buf,6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + BFLAG_LENGTH + ( i * ( BFLAG_LENGTH + 4 )  )  ) = *battle_data[i].val;
	}
	
	chrif_send_report(buf,  6 + 12 + 9 + 24 + 4 + 4 + 4 + 4 + ( bd_size * ( BFLAG_LENGTH + 4 ) ) );
	
	aFree(buf);
	
#undef BFLAG_LENGTH
}
static int rAthena_report_timer(int tid, unsigned int tick, int id, intptr_t data) {
	if( chrif_isconnected() ) {/* char server relays it, so it must be online. */
		rAthena_report(__DATE__,__TIME__);
	}
	return 0;
}
#endif

int battle_set_value(const char* w1, const char* w2)
{
	int val = config_switch(w2);

	int i;
	ARR_FIND(0, ARRAYLENGTH(battle_data), i, strcmpi(w1, battle_data[i].str) == 0);
	if (i == ARRAYLENGTH(battle_data))
		return 0; // not found

	if (val < battle_data[i].min || val > battle_data[i].max) {
		ShowWarning("Value for setting '%s': %s is invalid (min:%i max:%i)! Defaulting to %i...\n", w1, w2, battle_data[i].min, battle_data[i].max, battle_data[i].defval);
		val = battle_data[i].defval;
	}

	*battle_data[i].val = val;
	return 1;
}

int battle_get_value(const char* w1)
{
	int i;
	ARR_FIND(0, ARRAYLENGTH(battle_data), i, strcmpi(w1, battle_data[i].str) == 0);
	if (i == ARRAYLENGTH(battle_data))
		return 0; // not found
	else
		return *battle_data[i].val;
}

void battle_set_defaults()
{
	int i;
	for (i = 0; i < ARRAYLENGTH(battle_data); i++)
		*battle_data[i].val = battle_data[i].defval;
}

void battle_adjust_conf()
{
	battle_config.monster_max_aspd = 2000 - battle_config.monster_max_aspd*10;
	battle_config.max_aspd = 2000 - battle_config.max_aspd*10;
	battle_config.max_third_aspd = 2000 - battle_config.max_third_aspd*10;
	battle_config.max_walk_speed = 100*DEFAULT_WALK_SPEED/battle_config.max_walk_speed;
	battle_config.max_cart_weight *= 10;
	
	if(battle_config.max_def > 100 && !battle_config.weapon_defense_type) // added by [Skotlex]
		battle_config.max_def = 100;

	if(battle_config.min_hitrate > battle_config.max_hitrate)
		battle_config.min_hitrate = battle_config.max_hitrate;
		
	if(battle_config.pet_max_atk1 > battle_config.pet_max_atk2) //Skotlex
		battle_config.pet_max_atk1 = battle_config.pet_max_atk2;
	
	if (battle_config.day_duration && battle_config.day_duration < 60000) // added by [Yor]
		battle_config.day_duration = 60000;
	if (battle_config.night_duration && battle_config.night_duration < 60000) // added by [Yor]
		battle_config.night_duration = 60000;

#if PACKETVER < 20100427
	if( battle_config.feature_buying_store ) {
		ShowWarning("conf/battle/feature.conf buying_store is enabled but it requires PACKETVER 2010-04-27 or newer, disabling...\n");
		battle_config.feature_buying_store = 0;
	}
#endif

#if PACKETVER < 20100803
	if( battle_config.feature_search_stores ) {
		ShowWarning("conf/battle/feature.conf search_stores is enabled but it requires PACKETVER 2010-08-03 or newer, disabling...\n");
		battle_config.feature_search_stores = 0;
	}
#endif

#ifndef CELL_NOSTACK
	if (battle_config.cell_stack_limit != 1)
		ShowWarning("Battle setting 'cell_stack_limit' takes no effect as this server was compiled without Cell Stack Limit support.\n");
#endif
}

int battle_config_read(const char* cfgName)
{
	FILE* fp;
	static int count = 0;

	if (count == 0)
		battle_set_defaults();

	count++;

	fp = fopen(cfgName,"r");
	if (fp == NULL)
		ShowError("File not found: %s\n", cfgName);
	else {
		char line[1024], w1[1024], w2[1024];
		while(fgets(line, sizeof(line), fp)) {
			if (line[0] == '/' && line[1] == '/')
				continue;
			if (sscanf(line, "%1023[^:]:%1023s", w1, w2) != 2)
				continue;
			if (strcmpi(w1, "import") == 0)
				battle_config_read(w2);
			else if
				(battle_set_value(w1, w2) == 0)
				ShowWarning("Unknown setting '%s' in file %s\n", w1, cfgName);
		}

		fclose(fp);
	}

	count--;

	if (count == 0)
		battle_adjust_conf();

	return 0;
}

void do_init_battle(void)
{
	delay_damage_ers = ers_new(sizeof(struct delay_damage),"battle.c::delay_damage_ers",ERS_OPT_CLEAR);
	add_timer_func_list(battle_delay_damage_sub, "battle_delay_damage_sub");
	
#ifndef STATS_OPT_OUT
	add_timer_func_list(rAthena_report_timer, "rAthena_report_timer");
	add_timer_interval(gettick()+30000, rAthena_report_timer, 0, 0, 60000 * 30);
#endif

}

void do_final_battle(void)
{
	ers_destroy(delay_damage_ers);
}