// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

//#define DEBUG_DISP
//#define DEBUG_DISASM
//#define DEBUG_RUN
//#define DEBUG_HASH
//#define DEBUG_DUMP_STACK

#include "../common/cbasetypes.h"
#include "../common/malloc.h"
#include "../common/md5calc.h"
#include "../common/nullpo.h"
#include "../common/random.h"
#include "../common/showmsg.h"
#include "../common/socket.h"   // usage: getcharip
#include "../common/strlib.h"
#include "../common/timer.h"
#include "../common/utils.h"

#include "map.h"
#include "path.h"
#include "clif.h"
#include "chrif.h"
#include "itemdb.h"
#include "pc.h"
#include "status.h"
#include "storage.h"
#include "mob.h"
#include "npc.h"
#include "pet.h"
#include "mapreg.h"
#include "homunculus.h"
#include "instance.h"
#include "mercenary.h"
#include "intif.h"
#include "skill.h"
#include "status.h"
#include "chat.h"
#include "battle.h"
#include "battleground.h"
#include "party.h"
#include "guild.h"
#include "atcommand.h"
#include "log.h"
#include "unit.h"
#include "pet.h"
#include "mail.h"
#include "script.h"
#include "quest.h"
#include "elemental.h"
#include "../config/core.h"

#ifdef PCRE_SUPPORT
	#include "../../3rdparty/pcre/include/pcre.h" // preg_match
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef WIN32
	#include <sys/time.h>
#endif
#include <time.h>
#include <setjmp.h>
#include <errno.h>

#ifdef BETA_THREAD_TEST
	#include "../common/atomic.h"
	#include "../common/spinlock.h"
	#include "../common/thread.h"
	#include "../common/mutex.h"
#endif


///////////////////////////////////////////////////////////////////////////////
// @TODO: Possible enhancements: [FlavioJS]
// - 'callfunc' supporting labels in the current npc "::LabelName"
// - 'callfunc' supporting labels in other npcs "NpcName::LabelName"
// - 'function FuncName;' function declarations reverting to global functions
//   if local label isn't found
// - join callfunc and callsub's functionality
// - remove dynamic allocation in add_word()
// - remove GETVALUE / SETVALUE
// - clean up the set_reg / set_val / setd_sub mess
// - detect invalid label references at parse-time

//
// struct script_state* st;
//

/// Returns the script_data at the target index
#define script_getdata(st,i) ( &((st)->stack->stack_data[(st)->start + (i)]) )
/// Returns if the stack contains data at the target index
#define script_hasdata(st,i) ( (st)->end > (st)->start + (i) )
/// Returns the index of the last data in the stack
#define script_lastdata(st) ( (st)->end - (st)->start - 1 )
/// Pushes an int into the stack
#define script_pushint(st,val) push_val((st)->stack, C_INT, (val))
/// Pushes a string into the stack (script engine frees it automatically)
#define script_pushstr(st,val) push_str((st)->stack, C_STR, (val))
/// Pushes a copy of a string into the stack
#define script_pushstrcopy(st,val) push_str((st)->stack, C_STR, aStrdup(val))
/// Pushes a constant string into the stack (must never change or be freed)
#define script_pushconststr(st,val) push_str((st)->stack, C_CONSTSTR, (val))
/// Pushes a nil into the stack
#define script_pushnil(st) push_val((st)->stack, C_NOP, 0)
/// Pushes a copy of the data in the target index
#define script_pushcopy(st,i) push_copy((st)->stack, (st)->start + (i))

#define script_isstring(st,i) data_isstring(script_getdata(st,i))
#define script_isint(st,i) data_isint(script_getdata(st,i))

#define script_getnum(st,val) conv_num(st, script_getdata(st,val))
#define script_getstr(st,val) conv_str(st, script_getdata(st,val))
#define script_getref(st,val) ( script_getdata(st,val)->ref )
// Returns name of currently running function
#define script_getfuncname(st) ( st->funcname )

// NOTE: "top" functions/defines use indexes relative to the top of the stack
//       -1 is the index of the data at the top

/// Returns the script_data at the target index relative to the top of the stack
#define script_getdatatop(st,i) ( &((st)->stack->stack_data[(st)->stack->sp + (i)]) )
/// Pushes a copy of the data in the target index relative to the top of the stack
#define script_pushcopytop(st,i) push_copy((st)->stack, (st)->stack->sp + (i))
/// Removes the range of values [start,end[ relative to the top of the stack
#define script_removetop(st,start,end) ( pop_stack((st), ((st)->stack->sp + (start)), (st)->stack->sp + (end)) )

//
// struct script_data* data;
//

/// Returns if the script data is a string
#define data_isstring(data) ( (data)->type == C_STR || (data)->type == C_CONSTSTR )
/// Returns if the script data is an int
#define data_isint(data) ( (data)->type == C_INT )
/// Returns if the script data is a reference
#define data_isreference(data) ( (data)->type == C_NAME )
/// Returns if the script data is a label
#define data_islabel(data) ( (data)->type == C_POS )
/// Returns if the script data is an internal script function label
#define data_isfunclabel(data) ( (data)->type == C_USERFUNC_POS )

/// Returns if this is a reference to a constant
#define reference_toconstant(data) ( str_data[reference_getid(data)].type == C_INT )
/// Returns if this a reference to a param
#define reference_toparam(data) ( str_data[reference_getid(data)].type == C_PARAM )
/// Returns if this a reference to a variable
//@TODO: Confirm it's C_NAME [FlavioJS]
#define reference_tovariable(data) ( str_data[reference_getid(data)].type == C_NAME )
/// Returns the unique id of the reference (id and index)
#define reference_getuid(data) ( (data)->u.num )
/// Returns the id of the reference
#define reference_getid(data) ( (int32)(reference_getuid(data) & 0x00ffffff) )
/// Returns the array index of the reference
#define reference_getindex(data) ( (int32)(((uint32)(reference_getuid(data) & 0xff000000)) >> 24) )
/// Returns the name of the reference
#define reference_getname(data) ( str_buf + str_data[reference_getid(data)].str )
/// Returns the linked list of uid-value pairs of the reference (can be NULL)
#define reference_getref(data) ( (data)->ref )
/// Returns the value of the constant
#define reference_getconstant(data) ( str_data[reference_getid(data)].val )
/// Returns the type of param
#define reference_getparamtype(data) ( str_data[reference_getid(data)].val )

/// Composes the uid of a reference from the id and the index
#define reference_uid(id,idx) ( (int32)((((uint32)(id)) & 0x00ffffff) | (((uint32)(idx)) << 24)) )

#define not_server_variable(prefix) ( (prefix) != '$' && (prefix) != '.' && (prefix) != '\'')
#define not_array_variable(prefix) ( (prefix) != '$' && (prefix) != '@' && (prefix) != '.' && (prefix) != '\'' )
#define is_string_variable(name) ( (name)[strlen(name) - 1] == '$' )

#define FETCH(n, t) \
		if( script_hasdata(st,n) ) \
			(t) = script_getnum(st,n);

#define SCRIPT_MAX_ARRAYSIZE 128 /// Maximum amount of elements in script arrays
#define SCRIPT_CMD_SUCCESS 0 /// When a buildin cmd was correctly done
#define SCRIPT_CMD_FAILURE 1 /// When an errors appear in cmd, show_debug will follow

#define SCRIPT_BLOCK_SIZE 512
enum { LABEL_NEXTLINE = 1, LABEL_START };

/// Temporary buffer for passing around compiled bytecode
/// @see add_scriptb, set_label, parse_script
static unsigned char* script_buf = NULL;
static int script_pos = 0, script_size = 0;

static inline int GETVALUE(const unsigned char* buf, int i)
{
	return (int)MakeDWord(MakeWord(buf[i], buf[i + 1]), MakeWord(buf[i + 2], 0));
}
static inline void SETVALUE(unsigned char* buf, int i, int n)
{
	buf[i]   = GetByte(n, 0);
	buf[i + 1] = GetByte(n, 1);
	buf[i + 2] = GetByte(n, 2);
}

// String buffer structures.
// str_data stores string information
static struct str_data_struct {
	enum c_op type;
	int str;
	int backpatch;
	int label;
	int (*func)(struct script_state *st);
	int val;
	int next;
} *str_data = NULL;
static int str_data_size = 0; // size of the data
static int str_num = LABEL_START; // next id to be assigned

// str_buf holds the strings themselves
static char *str_buf;
static int str_size = 0; // size of the buffer
static int str_pos = 0; // next position to be assigned


// Using a prime number for SCRIPT_HASH_SIZE should give better distributions
#define SCRIPT_HASH_SIZE 1021
int str_hash[SCRIPT_HASH_SIZE];
// Specifies which string hashing method to use
//#define SCRIPT_HASH_DJB2
//#define SCRIPT_HASH_SDBM
#define SCRIPT_HASH_ELF

static DBMap* scriptlabel_db = NULL; // const char* label_name -> int script_pos
static DBMap* userfunc_db = NULL; // const char* func_name -> struct script_code*
static int parse_options = 0;
DBMap* script_get_label_db(void){ return scriptlabel_db; }
DBMap* script_get_userfunc_db(void){ return userfunc_db; }

// important buildin function references for usage in scripts
static int buildin_set_ref = 0;
static int buildin_callsub_ref = 0;
static int buildin_callfunc_ref = 0;
static int buildin_getelementofarray_ref = 0;

// Caches compiled autoscript item code.
// NOTE: This is not cleared when reloading itemdb.
static DBMap* autobonus_db = NULL; // char* script -> char* bytecode

struct Script_Config script_config = {
	1, // warn_func_mismatch_argtypes
	1, 65535, 2048, //warn_func_mismatch_paramnum/check_cmdcount/check_gotocount
	0, INT_MAX, // input_min_value/input_max_value
	"OnPCDieEvent", //die_event_name
	"OnPCKillEvent", //kill_pc_event_name
	"OnNPCKillEvent", //kill_mob_event_name
	"OnPCLoginEvent", //login_event_name
	"OnPCLogoutEvent", //logout_event_name
	"OnPCLoadMapEvent", //loadmap_event_name
	"OnPCBaseLvUpEvent", //baselvup_event_name
	"OnPCJobLvUpEvent", //joblvup_event_name
	"OnPCStatCalcEvent", //stat_calc_event_name
	"OnTouch_",	//ontouch_name (runs on first visible char to enter area, picks another char if the first char leaves)
	"OnTouch",	//ontouch2_name (run whenever a char walks into the OnTouch area)
};

static jmp_buf     error_jump;
static char*       error_msg;
static const char* error_pos;
static int         error_report; // if the error should produce output
// Used by disp_warning_message
static const char* parser_current_src;
static const char* parser_current_file;
static int         parser_current_line;

// for advanced scripting support ( nested if, switch, while, for, do-while, function, etc )
// [Eoe / jA 1080, 1081, 1094, 1164]
enum curly_type {
	TYPE_NULL = 0,
	TYPE_IF,
	TYPE_SWITCH,
	TYPE_WHILE,
	TYPE_FOR,
	TYPE_DO,
	TYPE_USERFUNC,
	TYPE_ARGLIST // function argument list
};

enum e_arglist
{
	ARGLIST_UNDEFINED = 0,
	ARGLIST_NO_PAREN  = 1,
	ARGLIST_PAREN     = 2,
};

static struct {
	struct {
		enum curly_type type;
		int index;
		int count;
		int flag;
		struct linkdb_node *case_label;
	} curly[256];		// Information right parenthesis
	int curly_count;	// The number of right brackets
	int index;			// Number of the syntax used in the script
} syntax;

const char* parse_curly_close(const char* p);
const char* parse_syntax_close(const char* p);
const char* parse_syntax_close_sub(const char* p,int* flag);
const char* parse_syntax(const char* p);
static int parse_syntax_for_flag = 0;

extern short current_equip_item_index; //For New CARDS Scripts. It contains Inventory Index of the EQUIP_SCRIPT caller item. [Lupus]
extern unsigned int current_equip_combo_pos;

int potion_flag = 0; //For use on Alchemist improved potions/Potion Pitcher. [Skotlex]
int potion_hp = 0, potion_per_hp = 0, potion_sp = 0, potion_per_sp = 0;
int potion_target = 0;


c_op get_com(unsigned char *script,int *pos);
int get_num(unsigned char *script,int *pos);

typedef struct script_function {
	int (*func)(struct script_state *st);
	const char *name;
	const char *arg;
} script_function;

extern script_function buildin_func[];

static struct linkdb_node* sleep_db;// int oid -> struct script_state*

#ifdef BETA_THREAD_TEST
/**
 * MySQL Query Slave
 */
static SPIN_LOCK queryThreadLock;
static rAthread queryThread = NULL;
static ramutex	queryThreadMutex = NULL;
static racond	queryThreadCond = NULL;
static volatile int32 queryThreadTerminate = 0;

struct queryThreadEntry {
	bool ok;
	bool type; /* Main db or log db? */
	struct script_state *st;
};

/* Ladies and Gentleman the Manager! */
struct {
	struct queryThreadEntry **entry; /* Array of structs */
	int count;
	int timer; /* Used to receive processed entries */
} queryThreadData;
#endif

/*==========================================
 * (Only those needed) local declaration prototype
 *------------------------------------------*/
const char* parse_subexpr(const char* p,int limit);
int run_func(struct script_state *st);
int script_instancegetid(struct script_state *st);

enum {
	MF_NOMEMO, //0
	MF_NOTELEPORT,
	MF_NOSAVE,
	MF_NOBRANCH,
	MF_NOPENALTY,
	MF_NOZENYPENALTY,
	MF_PVP,
	MF_PVP_NOPARTY,
	MF_PVP_NOGUILD,
	MF_GVG,
	MF_GVG_NOPARTY, //10
	MF_NOTRADE,
	MF_NOSKILL,
	MF_NOWARP,
	MF_PARTYLOCK,
	MF_NOICEWALL,
	MF_SNOW,
	MF_FOG,
	MF_SAKURA,
	MF_LEAVES,
	/**
	 * No longer available, keeping here just in case it's back someday. [Ind]
	 */
	//MF_RAIN, //20
	//21 free
	MF_NOGO = 22,
	MF_CLOUDS,
	MF_CLOUDS2,
	MF_FIREWORKS,
	MF_GVG_CASTLE,
	MF_GVG_DUNGEON,
	MF_NIGHTENABLED,
	MF_NOBASEEXP,
	MF_NOJOBEXP, //30
	MF_NOMOBLOOT,
	MF_NOMVPLOOT,
	MF_NORETURN,
	MF_NOWARPTO,
	MF_NIGHTMAREDROP,
	MF_RESTRICTED,
	MF_NOCOMMAND,
	MF_NODROP,
	MF_JEXP,
	MF_BEXP, //40
	MF_NOVENDING,
	MF_LOADEVENT,
	MF_NOCHAT,
	MF_NOEXPPENALTY,
	MF_GUILDLOCK,
	MF_TOWN,
	MF_AUTOTRADE,
	MF_ALLOWKS,
	MF_MONSTER_NOTELEPORT,
	MF_PVP_NOCALCRANK, //50
	MF_BATTLEGROUND,
	MF_RESET,
	MF_CHANNELAUTOJOIN,
	MF_NOUSECART,
	MF_NOITEMCONSUMPTION,
	MF_SUMSTARTMIRACLE,
	MF_NOMINEEFFECT,
	MF_NOLOCKON,
	MF_NOTOMB,
	MF_SKILL_DAMAGE, //60
	MF_NOCASHSHOP,
	MF_NOBANKING
};

const char* script_op2name(int op)
{
#define RETURN_OP_NAME(type) case type: return #type
	switch( op ) {
		RETURN_OP_NAME(C_NOP);
		RETURN_OP_NAME(C_POS);
		RETURN_OP_NAME(C_INT);
		RETURN_OP_NAME(C_PARAM);
		RETURN_OP_NAME(C_FUNC);
		RETURN_OP_NAME(C_STR);
		RETURN_OP_NAME(C_CONSTSTR);
		RETURN_OP_NAME(C_ARG);
		RETURN_OP_NAME(C_NAME);
		RETURN_OP_NAME(C_EOL);
		RETURN_OP_NAME(C_RETINFO);
		RETURN_OP_NAME(C_USERFUNC);
		RETURN_OP_NAME(C_USERFUNC_POS);

		//Operators
		RETURN_OP_NAME(C_OP3);
		RETURN_OP_NAME(C_LOR);
		RETURN_OP_NAME(C_LAND);
		RETURN_OP_NAME(C_LE);
		RETURN_OP_NAME(C_LT);
		RETURN_OP_NAME(C_GE);
		RETURN_OP_NAME(C_GT);
		RETURN_OP_NAME(C_EQ);
		RETURN_OP_NAME(C_NE);
		RETURN_OP_NAME(C_XOR);
		RETURN_OP_NAME(C_OR);
		RETURN_OP_NAME(C_AND);
		RETURN_OP_NAME(C_ADD);
		RETURN_OP_NAME(C_SUB);
		RETURN_OP_NAME(C_MUL);
		RETURN_OP_NAME(C_DIV);
		RETURN_OP_NAME(C_MOD);
		RETURN_OP_NAME(C_NEG);
		RETURN_OP_NAME(C_LNOT);
		RETURN_OP_NAME(C_NOT);
		RETURN_OP_NAME(C_R_SHIFT);
		RETURN_OP_NAME(C_L_SHIFT);

		default:
			ShowDebug("script_op2name: unexpected op=%d\n", op);
			return "???";
		}
#undef RETURN_OP_NAME
}

#ifdef DEBUG_DUMP_STACK
static void script_dump_stack(struct script_state* st)
{
	int i;
	ShowMessage("\tstart = %d\n", st->start);
	ShowMessage("\tend   = %d\n", st->end);
	ShowMessage("\tdefsp = %d\n", st->stack->defsp);
	ShowMessage("\tsp    = %d\n", st->stack->sp);
	for( i = 0; i < st->stack->sp; ++i ) {
		struct script_data* data = &st->stack->stack_data[i];
		ShowMessage("\t[%d] %s", i, script_op2name(data->type));
		switch( data->type ) {
			case C_INT:
			case C_POS:
				ShowMessage(" %d\n", data->u.num);
				break;

			case C_STR:
			case C_CONSTSTR:
				ShowMessage(" \"%s\"\n", data->u.str);
				break;

			case C_NAME:
				ShowMessage(" \"%s\" (id=%d ref=%p subtype=%s)\n", reference_getname(data), data->u.num, data->ref, script_op2name(str_data[data->u.num].type));
				break;

			case C_RETINFO: {
					struct script_retinfo* ri = data->u.ri;
					ShowMessage(" %p {var_function=%p, script=%p, pos=%d, nargs=%d, defsp=%d}\n", ri, ri->var_function, ri->script, ri->pos, ri->nargs, ri->defsp);
				}
				break;
			default:
				ShowMessage("\n");
				break;
		}
	}
}
#endif

/// Reports on the console the src of a script error.
static void script_reportsrc(struct script_state *st)
{
	struct block_list* bl;

	if( st->oid == 0 )
		return; //Can't report source.

	bl = map_id2bl(st->oid);
	if( bl == NULL )
		return;

	switch( bl->type ) {
		case BL_NPC:
			if( bl->m >= 0 )
				ShowDebug("Source (NPC): %s at %s (%d,%d)\n", ((struct npc_data *)bl)->name, map[bl->m].name, bl->x, bl->y);
			else
				ShowDebug("Source (NPC): %s (invisible/not on a map)\n", ((struct npc_data *)bl)->name);
			break;
		default:
			if( bl->m >= 0 )
				ShowDebug("Source (Non-NPC type %d): name %s at %s (%d,%d)\n", bl->type, status_get_name(bl), map[bl->m].name, bl->x, bl->y);
			else
				ShowDebug("Source (Non-NPC type %d): name %s (invisible/not on a map)\n", bl->type, status_get_name(bl));
			break;
	}
}

/// Reports on the console information about the script data.
static void script_reportdata(struct script_data* data)
{
	if( data == NULL )
		return;
	switch( data->type ) {
		case C_NOP: //No value
			ShowDebug("Data: nothing (nil)\n");
			break;
		case C_INT: //Number
			ShowDebug("Data: number value=%d\n", data->u.num);
			break;
		case C_STR:
		case C_CONSTSTR:// string
			if( data->u.str )
				ShowDebug("Data: string value=\"%s\"\n", data->u.str);
			else
				ShowDebug("Data: string value=NULL\n");
			break;
		case C_NAME: //Reference
			if( reference_tovariable(data) ) { //Variable
				const char* name = reference_getname(data);
				if( not_array_variable(*name) )
					ShowDebug("Data: variable name='%s'\n", name);
				else
					ShowDebug("Data: variable name='%s' index=%d\n", name, reference_getindex(data));
			} else if( reference_toconstant(data) ) //Constant
				ShowDebug("Data: constant name='%s' value=%d\n", reference_getname(data), reference_getconstant(data));
			else if( reference_toparam(data) ) { //Param
				ShowDebug("Data: param name='%s' type=%d\n", reference_getname(data), reference_getparamtype(data));
			} else { //???
				ShowDebug("Data: reference name='%s' type=%s\n", reference_getname(data), script_op2name(data->type));
				ShowDebug("Please report this!!! - str_data.type=%s\n", script_op2name(str_data[reference_getid(data)].type));
			}
			break;
		case C_POS: //Label
			ShowDebug("Data: label pos=%d\n", data->u.num);
			break;
		default:
			ShowDebug("Data: %s\n", script_op2name(data->type));
			break;
	}
}


/// Reports on the console information about the current built-in function.
static void script_reportfunc(struct script_state* st)
{
	int params, id;
	struct script_data* data;

	if( !script_hasdata(st,0) ) //No stack
		return;

	data = script_getdata(st,0);

	if( !data_isreference(data) || str_data[reference_getid(data)].type != C_FUNC ) {
		//Script currently not executing a built-in function or corrupt stack
		return;
	}

	id = reference_getid(data);
	params = script_lastdata(st) - 1;

	if( params > 0 ) {
		int i;

		ShowDebug("Function: %s (%d parameter%s):\n", get_str(id), params, (params == 1) ? "" : "s");
		for( i = 2; i <= script_lastdata(st); i++ )
			script_reportdata(script_getdata(st,i));
	} else
		ShowDebug("Function: %s (no parameters)\n", get_str(id));
}

/*==========================================
 * Output error message
 *------------------------------------------*/
static void disp_error_message2(const char *mes,const char *pos,int report)
{
	error_msg = aStrdup(mes);
	error_pos = pos;
	error_report = report;
	longjmp(error_jump, 1);
}
#define disp_error_message(mes,pos) disp_error_message2(mes,pos,1)

static void disp_warning_message(const char *mes, const char *pos) {
	script_warning(parser_current_src,parser_current_file,parser_current_line,mes,pos);
}

/// Checks event parameter validity
static void check_event(struct script_state *st, const char *evt)
{
	if( evt && evt[0] && !stristr(evt, "::On") ) {
		ShowWarning("NPC event parameter deprecated! Please use 'NPCNAME::OnEVENT' instead of '%s'.\n", evt);
		script_reportsrc(st);
	}
}

/*==========================================
 * Hashes the input string
 *------------------------------------------*/
static unsigned int calc_hash(const char* p)
{
	unsigned int h;

#if defined(SCRIPT_HASH_DJB2)
	h = 5381;
	while( *p ) // hash*33 + c
		h = ( h << 5 ) + h + ((unsigned char)TOLOWER(*p++));
#elif defined(SCRIPT_HASH_SDBM)
	h = 0;
	while( *p ) // hash*65599 + c
		h = ( h << 6 ) + ( h << 16 ) - h + ((unsigned char)TOLOWER(*p++));
#elif defined(SCRIPT_HASH_ELF) // UNIX ELF hash
	h = 0;
	while( *p ){
		unsigned int g;
		h = ( h << 4 ) + ((unsigned char)TOLOWER(*p++));
		g = h & 0xF0000000;
		if( g )
		{
			h ^= g >> 24;
			h &= ~g;
		}
	}
#else // athena hash
	h = 0;
	while( *p )
		h = ( h << 1 ) + ( h >> 3 ) + ( h >> 5 ) + ( h >> 8 ) + (unsigned char)TOLOWER(*p++);
#endif

	return h % SCRIPT_HASH_SIZE;
}


/*==========================================
 * str_data manipulation functions
 *------------------------------------------*/

/// Looks up string using the provided id.
const char* get_str(int id)
{
	Assert( id >= LABEL_START && id < str_size );
	return str_buf+str_data[id].str;
}

/// Returns the uid of the string, or -1.
static int search_str(const char* p)
{
	int i;

	for( i = str_hash[calc_hash(p)]; i != 0; i = str_data[i].next )
		if( strcasecmp(get_str(i),p) == 0 )
			return i;

	return -1;
}

/// Stores a copy of the string and returns its id.
/// If an identical string is already present, returns its id instead.
int add_str(const char* p)
{
	int h;
	int len;

	h = calc_hash(p);

	if( str_hash[h] == 0 ) // Empty bucket, add new node here
		str_hash[h] = str_num;
	else { // Scan for end of list, or occurence of identical string
		int i;

		for( i = str_hash[h]; ; i = str_data[i].next ) {
			if( strcasecmp(get_str(i),p) == 0 )
				return i; // String already in list
			if( str_data[i].next == 0 )
				break; // Reached the end
		}

		// Append node to end of list
		str_data[i].next = str_num;
	}

	// Grow list if neccessary
	if( str_num >= str_data_size ) {
		str_data_size += 128;
		RECREATE(str_data,struct str_data_struct,str_data_size);
		memset(str_data + (str_data_size - 128), '\0', 128);
	}

	len = (int)strlen(p);

	// Grow string buffer if neccessary
	while( str_pos + len + 1 >= str_size ) {
		str_size += 256;
		RECREATE(str_buf,char,str_size);
		memset(str_buf + (str_size - 256), '\0', 256);
	}

	safestrncpy(str_buf + str_pos, p, len + 1);
	str_data[str_num].type = C_NOP;
	str_data[str_num].str = str_pos;
	str_data[str_num].next = 0;
	str_data[str_num].func = NULL;
	str_data[str_num].backpatch = -1;
	str_data[str_num].label = -1;
	str_pos += len + 1;

	return str_num++;
}


/// Appends 1 byte to the script buffer.
static void add_scriptb(int a)
{
	if( script_pos+1 >= script_size )
	{
		script_size += SCRIPT_BLOCK_SIZE;
		RECREATE(script_buf,unsigned char,script_size);
	}
	script_buf[script_pos++] = (uint8)(a);
}

/// Appends a c_op value to the script buffer.
/// The value is variable-length encoded into 8-bit blocks.
/// The encoding scheme is ( 01?????? )* 00??????, LSB first.
/// All blocks but the last hold 7 bits of data, topmost bit is always 1 (carries).
static void add_scriptc(int a)
{
	while( a >= 0x40 )
	{
		add_scriptb((a&0x3f)|0x40);
		a = (a - 0x40) >> 6;
	}

	add_scriptb(a);
}

/// Appends an integer value to the script buffer.
/// The value is variable-length encoded into 8-bit blocks.
/// The encoding scheme is ( 11?????? )* 10??????, LSB first.
/// All blocks but the last hold 7 bits of data, topmost bit is always 1 (carries).
static void add_scripti(int a)
{
	while( a >= 0x40 )
	{
		add_scriptb((a&0x3f)|0xc0);
		a = (a - 0x40) >> 6;
	}
	add_scriptb(a|0x80);
}

/// Appends a str_data object (label/function/variable/integer) to the script buffer.

///
/// @param l The id of the str_data entry
// Maximum up to 16M
static void add_scriptl(int l)
{
	int backpatch = str_data[l].backpatch;

	switch(str_data[l].type){
	case C_POS:
	case C_USERFUNC_POS:
		add_scriptc(C_POS);
		add_scriptb(str_data[l].label);
		add_scriptb(str_data[l].label>>8);
		add_scriptb(str_data[l].label>>16);
		break;
	case C_NOP:
	case C_USERFUNC:
		// Embedded data backpatch there is a possibility of label
		add_scriptc(C_NAME);
		str_data[l].backpatch = script_pos;
		add_scriptb(backpatch);
		add_scriptb(backpatch>>8);
		add_scriptb(backpatch>>16);
		break;
	case C_INT:
		add_scripti(abs(str_data[l].val));
		if( str_data[l].val < 0 ) //Notice that this is negative, from jA (Rayce)
			add_scriptc(C_NEG);
		break;
	default: // assume C_NAME
		add_scriptc(C_NAME);
		add_scriptb(l);
		add_scriptb(l>>8);
		add_scriptb(l>>16);
		break;
	}
}

/*==========================================
 * Resolve the label
 *------------------------------------------*/
void set_label(int l,int pos, const char* script_pos)
{
	int i;

	//Prevent overwriting constants values, parameters and built-in functions [Skotlex]
	if(str_data[l].type == C_INT || str_data[l].type == C_PARAM || str_data[l].type == C_FUNC) {
		disp_error_message("set_label: invalid label name",script_pos);
		return;
	}
	if(str_data[l].label != -1) {
		disp_error_message("set_label: dup label ",script_pos);
		return;
	}
	str_data[l].type = (str_data[l].type == C_USERFUNC ? C_USERFUNC_POS : C_POS);
	str_data[l].label = pos;
	for(i = str_data[l].backpatch; i >= 0 && i != 0x00ffffff;) {
		int next = GETVALUE(script_buf,i);

		script_buf[i - 1] = (str_data[l].type == C_USERFUNC ? C_USERFUNC_POS : C_POS);
		SETVALUE(script_buf,i,pos);
		i = next;
	}
}

/// Skips spaces and/or comments.
const char* skip_space(const char* p)
{
	if( p == NULL )
		return NULL;
	for(;;) {
		while( ISSPACE(*p) )
			++p;
		if( *p == '/' && p[1] == '/' ) { //Line comment
			while(*p && *p!='\n')
				++p;
		} else if( *p == '/' && p[1] == '*' ) { //Block comment
			p += 2;
			for(;;) {
				if( *p == '\0' ) {
					disp_warning_message("script:script->skip_space: end of file while parsing block comment. expected "CL_BOLD"*/"CL_NORM, p);
					return p;
				}
				if( *p == '*' && p[1] == '/' ) { //End of block comment
					p += 2;
					break;
				}
				++p;
			}
		} else
			break;
	}
	return p;
}

/// Skips a word.
/// A word consists of undercores and/or alphanumeric characters,
/// and valid variable prefixes/postfixes.
static
const char* skip_word(const char* p)
{
	// prefix
	switch( *p ) {
		case '@':// temporary char variable
			++p; break;
		case '#':// account variable
			p += ( p[1] == '#' ? 2 : 1 ); break;
		case '\'':// instance variable
			++p; break;
		case '.':// npc variable
			p += ( p[1] == '@' ? 2 : 1 ); break;
		case '$':// global variable
			p += ( p[1] == '@' ? 2 : 1 ); break;
	}

	while( ISALNUM(*p) || *p == '_' )
		++p;

	// postfix
	if( *p == '$' )// string
		p++;

	return p;
}

/// Adds a word to str_data.
/// @see skip_word
/// @see add_str
static
int add_word(const char* p)
{
	char* word;
	int len;
	int i;

	// Check for a word
	len = skip_word(p) - p;
	if( len == 0 )
		disp_error_message("script:add_word: invalid word. A word consists of undercores and/or alphanumeric characters, and valid variable prefixes/postfixes.", p);

	// Duplicate the word
	word = (char*)aMalloc(len+1);
	memcpy(word, p, len);
	word[len] = 0;
	
	// add the word
	i = add_str(word);
	aFree(word);
	return i;
}

/// Parses a function call.
/// The argument list can have parenthesis or not.
/// The number of arguments is checked.
static const char* parse_callfunc(const char* p, int require_paren, int is_custom)
{
	const char* p2;
	const char* arg = NULL;
	int func;

	func = add_word(p);
	if( str_data[func].type == C_FUNC ) {
		// Buildin function
		add_scriptl(func);
		add_scriptc(C_ARG);
		arg = buildin_func[str_data[func].val].arg;
	} else if( str_data[func].type == C_USERFUNC || str_data[func].type == C_USERFUNC_POS ) {
		// Script defined function
		add_scriptl(buildin_callsub_ref);
		add_scriptc(C_ARG);
		add_scriptl(func);
		arg = buildin_func[str_data[buildin_callsub_ref].val].arg;
		if( *arg == 0 )
			disp_error_message("parse_callfunc: callsub has no arguments, please review its definition",p);
		if( *arg != '*' )
			++arg; // Count func as argument
	} else {
#ifdef SCRIPT_CALLFUNC_CHECK
		const char* name = get_str(func);
		if( !is_custom && strdb_get(userfunc_db, name) == NULL ) {
#endif
			disp_error_message("parse_line: expect command, missing function name or calling undeclared function",p);
#ifdef SCRIPT_CALLFUNC_CHECK
		} else {;
			add_scriptl(buildin_callfunc_ref);
			add_scriptc(C_ARG);
			add_scriptc(C_STR);
			while( *name )
				add_scriptb(*name++);
			add_scriptb(0);
			arg = buildin_func[str_data[buildin_callfunc_ref].val].arg;
			if( *arg != '*' )
				++arg;
		}
#endif
	}

	p = skip_word(p);
	p = skip_space(p);
	syntax.curly[syntax.curly_count].type = TYPE_ARGLIST;
	syntax.curly[syntax.curly_count].count = 0;
	if( *p == ';' ) { // <func name> ';'
		syntax.curly[syntax.curly_count].flag = ARGLIST_NO_PAREN;
	} else if( *p == '(' && *(p2 = skip_space(p + 1)) == ')' ) { // <func name> '(' ')'
		syntax.curly[syntax.curly_count].flag = ARGLIST_PAREN;
		p = p2;
	/*
	} else if( 0 && require_paren && *p != '(' ) { // <func name>
		syntax.curly[syntax.curly_count].flag = ARGLIST_NO_PAREN;
	*/
	} else { // <func name> <arg list>
		if( require_paren ) {
			if( *p != '(' )
				disp_error_message("need '('",p);
			++p; // Skip '('
			syntax.curly[syntax.curly_count].flag = ARGLIST_PAREN;
		} else if( *p == '(' )
			syntax.curly[syntax.curly_count].flag = ARGLIST_UNDEFINED;
		else
			syntax.curly[syntax.curly_count].flag = ARGLIST_NO_PAREN;
		++syntax.curly_count;
		while( *arg ) {
			p2 = parse_subexpr(p,-1);
			if( p == p2 )
				break; // Not an argument
			if( *arg != '*' )
				++arg; // Next argument
			p = skip_space(p2);
			if( *arg == 0 || *p != ',' )
				break; // No more arguments
			++p; // Skip comma
		}
		--syntax.curly_count;
	}
	if( *arg && *arg != '?' && *arg != '*' )
		disp_error_message2("parse_callfunc: not enough arguments, expected ','", p, script_config.warn_func_mismatch_paramnum);
	if( syntax.curly[syntax.curly_count].type != TYPE_ARGLIST )
		disp_error_message("parse_callfunc: DEBUG last curly is not an argument list",p);
	if( syntax.curly[syntax.curly_count].flag == ARGLIST_PAREN ){
		if( *p != ')' )
			disp_error_message("parse_callfunc: expected ')' to close argument list",p);
		++p;
	}
	add_scriptc(C_FUNC);
	return p;
}

/// Processes end of logical script line.
/// @param first When true, only fix up scheduling data is initialized
/// @param p Script position for error reporting in set_label
static void parse_nextline(bool first, const char* p)
{
	if( !first )
	{
		add_scriptc(C_EOL);  // mark end of line for stack cleanup
		set_label(LABEL_NEXTLINE, script_pos, p);  // fix up '-' labels
	}

	// initialize data for new '-' label fix up scheduling
	str_data[LABEL_NEXTLINE].type      = C_NOP;
	str_data[LABEL_NEXTLINE].backpatch = -1;
	str_data[LABEL_NEXTLINE].label     = -1;
}

/// Parse a variable assignment using the direct equals operator
/// @param p script position where the function should run from
/// @return NULL if not a variable assignment, the new position otherwise
const char* parse_variable(const char* p) {
	int word;
	c_op type = C_NOP;
	const char *p2 = NULL;
	const char *var = p;
			
	// Skip the variable where applicable
	p = skip_word(p);
	p = skip_space(p);

	if( p == NULL ) // End of the line or invalid buffer
		return NULL;

	if( *p == '[' ) { // Array variable so process the array as appropriate
		int i, j;

		for( p2 = p, i = 0, j = 1; p; ++ i ) {
			if( *p++ == ']' && --(j) == 0 )
				break;
			if( *p == '[' )
				++j;
		}

		if( !(p = skip_space(p)) ) // End of line or invalid characters remaining
			disp_error_message("Missing right expression or closing bracket for variable.", p);
	}

	if( type == C_NOP &&
	!( (p[0] == '=' && p[1] != '=' && (type = C_EQ)) // =
	|| (p[0] == '+' && p[1] == '=' && (type = C_ADD)) // +=
	|| (p[0] == '-' && p[1] == '=' && (type = C_SUB)) // -=
	|| (p[0] == '^' && p[1] == '=' && (type = C_XOR)) // ^=
	|| (p[0] == '|' && p[1] == '=' && (type = C_OR )) // |=
	|| (p[0] == '&' && p[1] == '=' && (type = C_AND)) // &=
	|| (p[0] == '*' && p[1] == '=' && (type = C_MUL)) // *=
	|| (p[0] == '/' && p[1] == '=' && (type = C_DIV)) // /=
	|| (p[0] == '%' && p[1] == '=' && (type = C_MOD)) // %=
	|| (p[0] == '~' && p[1] == '=' && (type = C_NOT)) // ~=
	|| (p[0] == '+' && p[1] == '+' && (type = C_ADD_PP)) // ++
	|| (p[0] == '-' && p[1] == '-' && (type = C_SUB_PP)) // --
	|| (p[0] == '<' && p[1] == '<' && p[2] == '=' && (type = C_L_SHIFT)) // <<=
	|| (p[0] == '>' && p[1] == '>' && p[2] == '=' && (type = C_R_SHIFT)) // >>=
	) )
		return NULL; // Failed to find a matching operator combination so invalid

	switch( type ) {
		case C_EQ: // Incremental modifier
			p = skip_space(&p[1]);
			break;

		case C_L_SHIFT:
		case C_R_SHIFT: // Left or right shift modifier
			p = skip_space(&p[3]);
			break;

		default: // Normal incremental command
			p = skip_space(&p[2]);
			break;
	}

	if( p == NULL ) // End of line or invalid buffer
		return NULL;

	// Push the set function onto the stack
	add_scriptl(buildin_set_ref);
	add_scriptc(C_ARG);

	// Always append parenthesis to avoid errors
	syntax.curly[syntax.curly_count].type = TYPE_ARGLIST;
	syntax.curly[syntax.curly_count].count = 0;
	syntax.curly[syntax.curly_count].flag = ARGLIST_PAREN;

	// Increment the total curly count for the position in the script
	++syntax.curly_count;
	
	// Parse the variable currently being modified
	word = add_word(var);

	// Cannot assign a variable which exists as a function or label
	if( str_data[word].type == C_FUNC || str_data[word].type == C_USERFUNC || str_data[word].type == C_USERFUNC_POS )
		disp_error_message("Cannot modify a variable which has the same name as a function or label.", p);

	if( p2 ) { // Process the variable index
		const char* p3 = NULL;

		// Push the getelementofarray method into the stack
		add_scriptl(buildin_getelementofarray_ref);
		add_scriptc(C_ARG);
		add_scriptl(word);
			
		// Process the sub-expression for this assignment
		p3 = parse_subexpr(p2 + 1, 1);
		p3 = skip_space(p3);

		if( *p3 != ']' ) // Closing parenthesis is required for this script
			disp_error_message("Missing closing ']' parenthesis for the variable assignment.", p3);

		// Push the closing function stack operator onto the stack
		add_scriptc(C_FUNC);
		p3 ++;
	} else // Simply push the variable or value onto the stack
		add_scriptl(word);

	if( type != C_EQ )
		add_scriptc(C_REF);
	
	if( type == C_ADD_PP || type == C_SUB_PP ) { // Incremental operator for the method
		add_scripti(1);
		add_scriptc(type == C_ADD_PP ? C_ADD : C_SUB);
	} else { // Process the value as an expression
		p = parse_subexpr(p, -1);

		if( type != C_EQ )
			add_scriptc(type); // Push the type of modifier onto the stack
	}

	// Decrement the curly count for the position within the script
	--syntax.curly_count;
	
	// Close the script by appending the function operator
	add_scriptc(C_FUNC);
		
	// Push the buffer from the method
	return p;
}

/**
 * Checks whether the gives string is a number literal
 *
 * Mainly necessary to differentiate between number literals and NPC name
 * constants, since several of those start with a digit.
 *
 * All this does is to check if the string begins with an optional + or - sign,
 * followed by a hexadecimal or decimal number literal literal and is NOT
 * followed by a underscore or letter.
 *
 * @author : Hercules.ws
 * @param p Pointer to the string to check
 * @return Whether the string is a number literal
 */
bool is_number(const char *p) {
	const char *np;

	if (!p)
		return false;
	if (*p == '-' || *p == '+')
		p++;
	np = p;
	if (*p == '0' && p[1] == 'x') {
		p += 2;
		np = p;
		// Hexadecimal
		while (ISXDIGIT(*np))
			np++;
	} else {
		// Decimal
		while (ISDIGIT(*np))
			np++;
	}
	if (p != np && *np != '_' && !ISALPHA(*np)) // At least one digit, and next isn't a letter or _
		return true;
	return false;
}

/*==========================================
 * Analysis section
 *------------------------------------------*/
const char* parse_simpleexpr(const char *p)
{
	long long i;
	p = skip_space(p);

	if(*p == ';' || *p == ',')
		disp_error_message("parse_simpleexpr: unexpected end of expression",p);
	if(*p == '(') {
		if((i = syntax.curly_count - 1) >= 0 && syntax.curly[i].type == TYPE_ARGLIST)
			++syntax.curly[i].count;
		p = parse_subexpr(p + 1,-1);
		p = skip_space(p);
		if((i = syntax.curly_count - 1) >= 0 && syntax.curly[i].type == TYPE_ARGLIST &&
			syntax.curly[i].flag == ARGLIST_UNDEFINED && --syntax.curly[i].count == 0
		) {
			if(*p == ',') {
				syntax.curly[i].flag = ARGLIST_PAREN;
				return p;
			} else
				syntax.curly[i].flag = ARGLIST_NO_PAREN;
		}
		if(*p != ')')
			disp_error_message("parse_simpleexpr: unmatched ')'",p);
		++p;
	} else if(is_number(p)) {
		char *np;

		while(*p == '0' && ISDIGIT(p[1])) p++;
		i = strtoll(p,&np,0);
		if(i < INT_MIN) {
			i = INT_MIN;
			disp_warning_message("parse_simpleexpr: underflow detected, capping value to INT_MIN",p);
		} else if( i > INT_MAX ) {
			i = INT_MAX;
			disp_warning_message("parse_simpleexpr: overflow detected, capping value to INT_MAX",p);
		}
		add_scripti((int)i);
		p = np;
	} else if(*p == '"') {
		add_scriptc(C_STR);
		p++;
		while(*p && *p != '"') {
			if((unsigned char)p[-1] <= 0x7e && *p == '\\') {
				char buf[8];
				size_t len = skip_escaped_c(p) - p;
				size_t n = sv_unescape_c(buf, p, len);
				if(n != 1)
					ShowDebug("parse_simpleexpr: unexpected length %d after unescape (\"%.*s\" -> %.*s)\n",(int)n,(int)len,p,(int)n,buf);
				p += len;
				add_scriptb(*buf);
				continue;
			} else if(*p == '\n')
				disp_error_message("parse_simpleexpr: unexpected newline @ string",p);
			add_scriptb(*p++);
		}
		if(!*p)
			disp_error_message("parse_simpleexpr: unexpected eof @ string",p);
		add_scriptb(0);
		p++;	//'"'
	} else {
		int l;
		const char* pv;

		//Label , register , function etc
		if(skip_word(p) == p)
			disp_error_message("parse_simpleexpr: unexpected character",p);

		l = add_word(p);
		if(str_data[l].type == C_FUNC || str_data[l].type == C_USERFUNC || str_data[l].type == C_USERFUNC_POS)
			return parse_callfunc(p,1,0);
#ifdef SCRIPT_CALLFUNC_CHECK
		else {
			const char* name = get_str(l);
			if(strdb_get(userfunc_db,name) != NULL) {
				return parse_callfunc(p,1,1);
			}
		}
#endif

		if((pv = parse_variable(p))) { //Successfully processed a variable assignment
			return pv;
		}

		p = skip_word(p);
		if(*p == '[') {
			//Array(name[i] => getelementofarray(name,i))
			add_scriptl(buildin_getelementofarray_ref);
			add_scriptc(C_ARG);
			add_scriptl(l);

			p = parse_subexpr(p + 1,-1);
			p = skip_space(p);
			if(*p != ']')
				disp_error_message("parse_simpleexpr: unmatched ']'",p);
			++p;
			add_scriptc(C_FUNC);
		} else
			add_scriptl(l);

	}

	return p;
}

/*==========================================
 * Analysis of the expression
 *------------------------------------------*/
const char* parse_subexpr(const char* p,int limit)
{
	int op, opl, len;

	p = skip_space(p);
	if( *p == '-' ) {
		const char* tmpp = skip_space(p + 1);

		if( *tmpp == ';' || *tmpp == ',' ) {
			add_scriptl(LABEL_NEXTLINE);
			p++;
			return p;
		}
	}

	if( (op = C_NEG, *p == '-') || (op = C_LNOT, *p == '!') || (op = C_NOT, *p == '~') ) {
		p = parse_subexpr(p + 1, 10);
		add_scriptc(op);
	} else
		p = parse_simpleexpr(p);
	p = skip_space(p);
	while((
			(op = C_OP3, opl = 0, len = 1, *p == '?') ||
			(op = C_ADD, opl = 8, len = 1, *p == '+') ||
			(op = C_SUB, opl = 8, len = 1, *p == '-') ||
			(op = C_MUL, opl = 9, len = 1, *p == '*') ||
			(op = C_DIV, opl = 9, len = 1, *p == '/') ||
			(op = C_MOD, opl = 9, len = 1, *p == '%') ||
			(op = C_LAND, opl = 2, len = 2, *p == '&' && p[1] == '&') ||
			(op = C_AND, opl = 6, len = 1, *p == '&') ||
			(op = C_LOR, opl = 1, len = 2, *p == '|' && p[1] == '|') ||
			(op = C_OR, opl = 5, len = 1, *p == '|') ||
			(op = C_XOR, opl = 4, len = 1, *p == '^') ||
			(op = C_EQ, opl = 3, len = 2, *p == '=' && p[1] == '=') ||
			(op = C_NE, opl = 3, len = 2, *p == '!' && p[1] == '=') ||
			(op = C_R_SHIFT, opl = 7, len = 2, *p == '>' && p[1] == '>') ||
			(op = C_GE, opl = 3, len = 2, *p == '>' && p[1] == '=') ||
			(op = C_GT, opl = 3, len = 1, *p == '>') ||
			(op = C_L_SHIFT, opl = 7, len = 2, *p == '<' && p[1] == '<') ||
			(op = C_LE, opl = 3, len = 2, *p == '<' && p[1] == '=') ||
			(op = C_LT, opl = 3, len = 1, *p == '<')) && opl > limit) {
		p += len;
		if( op == C_OP3 ) {
			p = parse_subexpr(p, -1);
			p = skip_space(p);
			if( *(p++) != ':' )
				disp_error_message("parse_subexpr: expected ':'", p - 1);
			p = parse_subexpr(p, -1);
		} else
			p = parse_subexpr(p, opl);

		add_scriptc(op);
		p = skip_space(p);
	}

	return p; /* Return first untreated operator */
}

/*==========================================
 * Evaluation of the expression
 *------------------------------------------*/
const char* parse_expr(const char *p)
{
	switch(*p){
	case ')': case ';': case ':': case '[': case ']':
	case '}':
		disp_error_message("parse_expr: unexpected character",p);
	}
	p=parse_subexpr(p,-1);
	return p;
}

/*==========================================
 * �Analysis of the line
 *------------------------------------------*/
const char* parse_line(const char* p)
{
	const char* p2;

	p=skip_space(p);
	if(*p==';') {
		// Close decision for if(); for(); while();
		p = parse_syntax_close(p + 1);
		return p;
	}
	if(*p==')' && parse_syntax_for_flag)
		return p+1;

	p = skip_space(p);
	if(p[0] == '{') {
		syntax.curly[syntax.curly_count].type  = TYPE_NULL;
		syntax.curly[syntax.curly_count].count = -1;
		syntax.curly[syntax.curly_count].index = -1;
		syntax.curly_count++;
		return p + 1;
	} else if(p[0] == '}') {
		return parse_curly_close(p);
	}
		
	// �Syntax-related processing
	p2 = parse_syntax(p);
	if(p2 != NULL)
		return p2;

	// attempt to process a variable assignment
	p2 = parse_variable(p);

	if( p2 != NULL )
	{// variable assignment processed so leave the method
		return parse_syntax_close(p2 + 1);
	}

	p = parse_callfunc(p,0,0);
	p = skip_space(p);
	
	if(parse_syntax_for_flag) {
		if( *p != ')' )
			disp_error_message("parse_line: expected ')'",p);
	} else {
		if( *p != ';' )
			disp_error_message("parse_line: expected ';'",p);
	}

	// Binding decision for if(), for(), while()
	p = parse_syntax_close(p+1);

	return p;
}

// { ... } Closing process
const char* parse_curly_close(const char* p)
{
	if(syntax.curly_count <= 0) {
		disp_error_message("parse_curly_close: unexpected string",p);
		return p + 1;
	} else if(syntax.curly[syntax.curly_count-1].type == TYPE_NULL) {
		syntax.curly_count--;
		// Close decision  if, for , while
		p = parse_syntax_close(p + 1);
		return p;
	} else if(syntax.curly[syntax.curly_count-1].type == TYPE_SWITCH) {
		// Closing switch()
		int pos = syntax.curly_count-1;
		char label[256];
		int l;
		// Remove temporary variables 
		sprintf(label,"set $@__SW%x_VAL,0;",syntax.curly[pos].index);
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// Go to the end pointer unconditionally
		sprintf(label,"goto __SW%x_FIN;",syntax.curly[pos].index);
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// You are here labeled
		sprintf(label,"__SW%x_%x",syntax.curly[pos].index,syntax.curly[pos].count);
		l=add_str(label);
		set_label(l,script_pos, p);

		if(syntax.curly[pos].flag) {
			// Exists default
			sprintf(label,"goto __SW%x_DEF;",syntax.curly[pos].index);
			syntax.curly[syntax.curly_count++].type = TYPE_NULL;
			parse_line(label);
			syntax.curly_count--;
		}

		// �Label end
		sprintf(label,"__SW%x_FIN",syntax.curly[pos].index);
		l=add_str(label);
		set_label(l,script_pos, p);
		linkdb_final(&syntax.curly[pos].case_label);	// free the list of case label
		syntax.curly_count--;
		// Closing decision if, for , while
		p = parse_syntax_close(p + 1);
		return p;
	} else {
		disp_error_message("parse_curly_close: unexpected string",p);
		return p + 1;
	}
}

// 	Syntax-related processing
//	break, case, continue, default, do, for, function,
//	if, switch, while ? will handle this internally.
const char* parse_syntax(const char* p)
{
	const char *p2 = skip_word(p);

	switch(*p) {
		case 'B':
		case 'b':
			if(p2 - p == 5 && !strncasecmp(p,"break",5)) {
				// Processing break
				char label[256];
				int pos = syntax.curly_count - 1;
				while(pos >= 0) {
					if(syntax.curly[pos].type == TYPE_DO) {
						sprintf(label,"goto __DO%x_FIN;",syntax.curly[pos].index);
						break;
					} else if(syntax.curly[pos].type == TYPE_FOR) {
						sprintf(label,"goto __FR%x_FIN;",syntax.curly[pos].index);
						break;
					} else if(syntax.curly[pos].type == TYPE_WHILE) {
						sprintf(label,"goto __WL%x_FIN;",syntax.curly[pos].index);
						break;
					} else if(syntax.curly[pos].type == TYPE_SWITCH) {
						sprintf(label,"goto __SW%x_FIN;",syntax.curly[pos].index);
						break;
					}
					pos--;
				}
				if(pos < 0) {
					disp_error_message("parse_syntax: unexpected 'break'",p);
				} else {
					syntax.curly[syntax.curly_count++].type = TYPE_NULL;
					parse_line(label);
					syntax.curly_count--;
				}
				p = skip_space(p2);
				if(*p != ';')
					disp_error_message("parse_syntax: expected ';'",p);
				// Closing decision if, for , while
				p = parse_syntax_close(p + 1);
				return p;
			}
			break;
		case 'c':
		case 'C':
			if(p2 - p == 4 && !strncasecmp(p,"case",4)) {
				// Processing case
				int pos = syntax.curly_count-1;
				if(pos < 0 || syntax.curly[pos].type != TYPE_SWITCH) {
					disp_error_message("parse_syntax: unexpected 'case' ",p);
					return p+1;
				} else {
					char label[256];
					int  l,v;
					char *np;
					if(syntax.curly[pos].count != 1) {
						// Jump for FALLTHRU
						sprintf(label,"goto __SW%x_%xJ;",syntax.curly[pos].index,syntax.curly[pos].count);
						syntax.curly[syntax.curly_count++].type = TYPE_NULL;
						parse_line(label);
						syntax.curly_count--;

						// You are here labeled
						sprintf(label,"__SW%x_%x",syntax.curly[pos].index,syntax.curly[pos].count);
						l=add_str(label);
						set_label(l,script_pos, p);
					}
					// Decision statement switch
					p = skip_space(p2);
					if(p == p2) {
						disp_error_message("parse_syntax: expected a space ' '",p);
					}
					// check whether case label is integer or not
					if(is_number(p)) {
						//Numeric value
						v = (int)strtol(p,&np,0);
						if((*p == '-' || *p == '+') && ISDIGIT(p[1])) // pre-skip because '-' can not skip_word
							p++;
						p = skip_word(p);
						if(np != p)
							disp_error_message("parse_syntax: 'case' label is not an integer",np);
					} else {
						//Check for constants
						p2 = skip_word(p);
						v = (int)(size_t)(p2-p); // length of word at p2
						memcpy(label,p,v);
						label[v]='\0';
						if( !script_get_constant(label, &v) )
							disp_error_message("parse_syntax: 'case' label is not an integer",p);
						p = skip_word(p);
					}
					p = skip_space(p);
					if(*p != ':')
						disp_error_message("parse_syntax: expect ':'",p);
					sprintf(label,"if(%d != $@__SW%x_VAL) goto __SW%x_%x;",
						v,syntax.curly[pos].index,syntax.curly[pos].index,syntax.curly[pos].count+1);
					syntax.curly[syntax.curly_count++].type = TYPE_NULL;
					// Bad I do not parse twice
					p2 = parse_line(label);
					parse_line(p2);
					syntax.curly_count--;
					if(syntax.curly[pos].count != 1) {
						// Label after the completion of FALLTHRU
						sprintf(label,"__SW%x_%xJ",syntax.curly[pos].index,syntax.curly[pos].count);
						l=add_str(label);
						set_label(l,script_pos,p);
					}
					// check duplication of case label [Rayce]
					if(linkdb_search(&syntax.curly[pos].case_label, (void*)__64BPRTSIZE(v)) != NULL)
						disp_error_message("parse_syntax: dup 'case'",p);
					linkdb_insert(&syntax.curly[pos].case_label, (void*)__64BPRTSIZE(v), (void*)1);

					sprintf(label,"set $@__SW%x_VAL,0;",syntax.curly[pos].index);
					syntax.curly[syntax.curly_count++].type = TYPE_NULL;
				
					parse_line(label);
					syntax.curly_count--;
					syntax.curly[pos].count++;
				}
				return p + 1;
			} else if(p2 - p == 8 && !strncasecmp(p,"continue",8)) {
				// Processing continue
				char label[256];
				int pos = syntax.curly_count - 1;
				while(pos >= 0) {
					if(syntax.curly[pos].type == TYPE_DO) {
						sprintf(label,"goto __DO%x_NXT;",syntax.curly[pos].index);
						syntax.curly[pos].flag = 1; // Flag put the link for continue
						break;
					} else if(syntax.curly[pos].type == TYPE_FOR) {
						sprintf(label,"goto __FR%x_NXT;",syntax.curly[pos].index);
						break;
					} else if(syntax.curly[pos].type == TYPE_WHILE) {
						sprintf(label,"goto __WL%x_NXT;",syntax.curly[pos].index);
						break;
					}
					pos--;
				}
				if(pos < 0) {
					disp_error_message("parse_syntax: unexpected 'continue'",p);
				} else {
					syntax.curly[syntax.curly_count++].type = TYPE_NULL;
					parse_line(label);
					syntax.curly_count--;
				}
				p = skip_space(p2);
				if(*p != ';')
					disp_error_message("parse_syntax: expected ';'",p);
				// Closing decision if, for , while
				p = parse_syntax_close(p + 1);
				return p;
			}
			break;
		case 'd':
		case 'D':
			if(p2 - p == 7 && !strncasecmp(p,"default",7)) {
				// Switch - default processing
				int pos = syntax.curly_count-1;
				if(pos < 0 || syntax.curly[pos].type != TYPE_SWITCH) {
					disp_error_message("parse_syntax: unexpected 'default'",p);
				} else if(syntax.curly[pos].flag) {
					disp_error_message("parse_syntax: dup 'default'",p);
				} else {
					char label[256];
					int l;
					// Put the label location
					p = skip_space(p2);
					if(*p != ':') {
						disp_error_message("parse_syntax: expected ':'",p);
					}
					sprintf(label,"__SW%x_%x",syntax.curly[pos].index,syntax.curly[pos].count);
					l=add_str(label);
					set_label(l,script_pos,p);

					// Skip to the next link w/o condition
					sprintf(label,"goto __SW%x_%x;",syntax.curly[pos].index,syntax.curly[pos].count+1);
					syntax.curly[syntax.curly_count++].type = TYPE_NULL;
					parse_line(label);
					syntax.curly_count--;

					// The default label
					sprintf(label,"__SW%x_DEF",syntax.curly[pos].index);
					l=add_str(label);
					set_label(l,script_pos,p);

					syntax.curly[syntax.curly_count - 1].flag = 1;
					syntax.curly[pos].count++;
				}
				return p + 1;
			} else if(p2 - p == 2 && !strncasecmp(p,"do",2)) {
				int l;
				char label[256];
				p=skip_space(p2);

				syntax.curly[syntax.curly_count].type  = TYPE_DO;
				syntax.curly[syntax.curly_count].count = 1;
				syntax.curly[syntax.curly_count].index = syntax.index++;
				syntax.curly[syntax.curly_count].flag  = 0;
				// Label of the (do) form here
				sprintf(label,"__DO%x_BGN",syntax.curly[syntax.curly_count].index);
				l=add_str(label);
				set_label(l,script_pos,p);
				syntax.curly_count++;
				return p;
			}
			break;
		case 'f':
		case 'F':
			if(p2 - p == 3 && !strncasecmp(p,"for",3)) {
				int l;
				char label[256];
				int  pos = syntax.curly_count;
				syntax.curly[syntax.curly_count].type  = TYPE_FOR;
				syntax.curly[syntax.curly_count].count = 1;
				syntax.curly[syntax.curly_count].index = syntax.index++;
				syntax.curly[syntax.curly_count].flag  = 0;
				syntax.curly_count++;

				p=skip_space(p2);

				if(*p != '(')
					disp_error_message("parse_syntax: expected '('",p);
				p++;

				// Execute the initialization statement
				syntax.curly[syntax.curly_count++].type = TYPE_NULL;
				p=parse_line(p);
				syntax.curly_count--;

				// Form the start of label decision
				sprintf(label,"__FR%x_J",syntax.curly[pos].index);
				l=add_str(label);
				set_label(l,script_pos,p);

				p=skip_space(p);
				if(*p == ';') {
					// For (; Because the pattern of always true ;)
					;
				} else {
					// Skip to the end point if the condition is false
					sprintf(label,"__FR%x_FIN",syntax.curly[pos].index);
					add_scriptl(add_str("jump_zero"));
					add_scriptc(C_ARG);
					p=parse_expr(p);
					p=skip_space(p);
					add_scriptl(add_str(label));
					add_scriptc(C_FUNC);
				}
				if(*p != ';')
					disp_error_message("parse_syntax: expected ';'",p);
				p++;
				
				// Skip to the beginning of the loop
				sprintf(label,"goto __FR%x_BGN;",syntax.curly[pos].index);
				syntax.curly[syntax.curly_count++].type = TYPE_NULL;
				parse_line(label);
				syntax.curly_count--;

				// Labels to form the next loop
				sprintf(label,"__FR%x_NXT",syntax.curly[pos].index);
				l=add_str(label);
				set_label(l,script_pos,p);
				
				// Process the next time you enter the loop
				// A ')' last for; flag to be treated as'
				parse_syntax_for_flag = 1;
				syntax.curly[syntax.curly_count++].type = TYPE_NULL;
				p=parse_line(p);
				syntax.curly_count--;
				parse_syntax_for_flag = 0;

				// Skip to the determination process conditions
				sprintf(label,"goto __FR%x_J;",syntax.curly[pos].index);
				syntax.curly[syntax.curly_count++].type = TYPE_NULL;
				parse_line(label);
				syntax.curly_count--;

				// Loop start labeling
				sprintf(label,"__FR%x_BGN",syntax.curly[pos].index);
				l=add_str(label);
				set_label(l,script_pos,p);
				return p;
			}
			else if( p2 - p == 8 && strncasecmp(p,"function",8) == 0 )
			{// internal script function
				const char *func_name;

				func_name = skip_space(p2);
				p = skip_word(func_name);
				if( p == func_name )
					disp_error_message("parse_syntax:function: function name is missing or invalid", p);
				p2 = skip_space(p);
				if( *p2 == ';' )
				{// function <name> ;
					// function declaration - just register the name
					int l;
					l = add_word(func_name);
					if( str_data[l].type == C_NOP )// register only, if the name was not used by something else
						str_data[l].type = C_USERFUNC;
					else if( str_data[l].type == C_USERFUNC )
						;  // already registered
					else
						disp_error_message("parse_syntax:function: function name is invalid", func_name);

					// Close condition of if, for, while
					p = parse_syntax_close(p2 + 1);
					return p;
				}
				else if(*p2 == '{')
				{// function <name> <line/block of code>
					char label[256];
					int l;

					syntax.curly[syntax.curly_count].type  = TYPE_USERFUNC;
					syntax.curly[syntax.curly_count].count = 1;
					syntax.curly[syntax.curly_count].index = syntax.index++;
					syntax.curly[syntax.curly_count].flag  = 0;
					++syntax.curly_count;

					// Jump over the function code
					sprintf(label, "goto __FN%x_FIN;", syntax.curly[syntax.curly_count-1].index);
					syntax.curly[syntax.curly_count].type = TYPE_NULL;
					++syntax.curly_count;
					parse_line(label);
					--syntax.curly_count;

					// Set the position of the function (label)
					l=add_word(func_name);
					if( str_data[l].type == C_NOP || str_data[l].type == C_USERFUNC )// register only, if the name was not used by something else
					{
						str_data[l].type = C_USERFUNC;
						set_label(l, script_pos, p);
						if( parse_options&SCRIPT_USE_LABEL_DB )
							strdb_iput(scriptlabel_db, get_str(l), script_pos);
					}
					else
						disp_error_message("parse_syntax:function: function name is invalid", func_name);

					return skip_space(p);
				}
				else
				{
					disp_error_message("expect ';' or '{' at function syntax",p);
				}
			}
			break;
		case 'i':
		case 'I':
			if(p2 - p == 2 && !strncasecmp(p,"if",2)) {
				// If process
				char label[256];
				p=skip_space(p2);
				if(*p != '(') { //Prevent if this {} non-c syntax. from Rayce (jA)
					disp_error_message("need '('",p);
				}
				syntax.curly[syntax.curly_count].type  = TYPE_IF;
				syntax.curly[syntax.curly_count].count = 1;
				syntax.curly[syntax.curly_count].index = syntax.index++;
				syntax.curly[syntax.curly_count].flag  = 0;
				sprintf(label,"__IF%x_%x",syntax.curly[syntax.curly_count].index,syntax.curly[syntax.curly_count].count);
				syntax.curly_count++;
				add_scriptl(add_str("jump_zero"));
				add_scriptc(C_ARG);
				p=parse_expr(p);
				p=skip_space(p);
				add_scriptl(add_str(label));
				add_scriptc(C_FUNC);
				return p;
			}
			break;
		case 's':
		case 'S':
			if(p2 - p == 6 && !strncasecmp(p,"switch",6)) {
				// Processing of switch ()
				char label[256];
				p=skip_space(p2);
				if(*p != '(') {
					disp_error_message("need '('",p);
				}
				syntax.curly[syntax.curly_count].type  = TYPE_SWITCH;
				syntax.curly[syntax.curly_count].count = 1;
				syntax.curly[syntax.curly_count].index = syntax.index++;
				syntax.curly[syntax.curly_count].flag  = 0;
				sprintf(label,"$@__SW%x_VAL",syntax.curly[syntax.curly_count].index);
				syntax.curly_count++;
				add_scriptl(add_str("set"));
				add_scriptc(C_ARG);
				add_scriptl(add_str(label));
				p=parse_expr(p);
				p=skip_space(p);
				if(*p != '{') {
					disp_error_message("parse_syntax: expected '{'",p);
				}
				add_scriptc(C_FUNC);
				return p + 1;
			}
			break;
		case 'w':
		case 'W':
			if(p2 - p == 5 && !strncasecmp(p,"while",5)) {
				int l;
				char label[256];
				p=skip_space(p2);
				if(*p != '(') {
					disp_error_message("need '('",p);
				}
				syntax.curly[syntax.curly_count].type  = TYPE_WHILE;
				syntax.curly[syntax.curly_count].count = 1;
				syntax.curly[syntax.curly_count].index = syntax.index++;
				syntax.curly[syntax.curly_count].flag  = 0;
				// Form the start of label decision
				sprintf(label,"__WL%x_NXT",syntax.curly[syntax.curly_count].index);
				l=add_str(label);
				set_label(l,script_pos,p);

				// Skip to the end point if the condition is false
				sprintf(label,"__WL%x_FIN",syntax.curly[syntax.curly_count].index);
				syntax.curly_count++;
				add_scriptl(add_str("jump_zero"));
				add_scriptc(C_ARG);
				p=parse_expr(p);
				p=skip_space(p);
				add_scriptl(add_str(label));
				add_scriptc(C_FUNC);
				return p;
			}
			break;
	}
	return NULL;
}

const char* parse_syntax_close(const char *p) {
	// If (...) for (...) hoge (); as to make sure closed closed once again
	int flag;

	do {
		p = parse_syntax_close_sub(p,&flag);
	} while(flag);
	return p;
}

// Close judgment if, for, while, of do
//	 flag == 1 : closed
//	 flag == 0 : not closed
const char* parse_syntax_close_sub(const char* p,int* flag)
{
	char label[256];
	int pos = syntax.curly_count - 1;
	int l;
	*flag = 1;

	if(syntax.curly_count <= 0) {
		*flag = 0;
		return p;
	} else if(syntax.curly[pos].type == TYPE_IF) {
		const char *bp = p;
		const char *p2;

		// if-block and else-block end is a new line
		parse_nextline(false, p);

		// Skip to the last location if
		sprintf(label,"goto __IF%x_FIN;",syntax.curly[pos].index);
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// Put the label of the location
		sprintf(label,"__IF%x_%x",syntax.curly[pos].index,syntax.curly[pos].count);
		l=add_str(label);
		set_label(l,script_pos,p);

		syntax.curly[pos].count++;
		p = skip_space(p);
		p2 = skip_word(p);
		if(!syntax.curly[pos].flag && p2 - p == 4 && !strncasecmp(p,"else",4)) {
			// else  or else - if
			p = skip_space(p2);
			p2 = skip_word(p);
			if(p2 - p == 2 && !strncasecmp(p,"if",2)) {
				// else - if
				p=skip_space(p2);
				if(*p != '(') {
					disp_error_message("need '('",p);
				}
				sprintf(label,"__IF%x_%x",syntax.curly[pos].index,syntax.curly[pos].count);
				add_scriptl(add_str("jump_zero"));
				add_scriptc(C_ARG);
				p=parse_expr(p);
				p=skip_space(p);
				add_scriptl(add_str(label));
				add_scriptc(C_FUNC);
				*flag = 0;
				return p;
			} else {
				// else
				if(!syntax.curly[pos].flag) {
					syntax.curly[pos].flag = 1;
					*flag = 0;
					return p;
				}
			}
		}
		// Close if
		syntax.curly_count--;
		// �Put the label of the final location
		sprintf(label,"__IF%x_FIN",syntax.curly[pos].index);
		l=add_str(label);
		set_label(l,script_pos,p);
		if(syntax.curly[pos].flag == 1) {
			// Because the position of the pointer is the same if not else for this
			return bp;
		}
		return p;
	} else if(syntax.curly[pos].type == TYPE_DO) {
		int l;
		char label[256];
		const char *p2;

		if(syntax.curly[pos].flag) {
			// (Come here continue) to form the label here
			sprintf(label,"__DO%x_NXT",syntax.curly[pos].index);
			l=add_str(label);
			set_label(l,script_pos,p);
		}

		// �Skip to the end point if the condition is false
		p = skip_space(p);
		p2 = skip_word(p);
		if(p2 - p != 5 || strncasecmp(p,"while",5))
			disp_error_message("parse_syntax: expected 'while'",p);

		p = skip_space(p2);
		if(*p != '(') {
			disp_error_message("need '('",p);
		}

		// do-block end is a new line
		parse_nextline(false, p);

		sprintf(label,"__DO%x_FIN",syntax.curly[pos].index);
		add_scriptl(add_str("jump_zero"));
		add_scriptc(C_ARG);
		p=parse_expr(p);
		p=skip_space(p);
		add_scriptl(add_str(label));
		add_scriptc(C_FUNC);

		// Skip to the starting point
		sprintf(label,"goto __DO%x_BGN;",syntax.curly[pos].index);
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// �Form label of the end point conditions
		sprintf(label,"__DO%x_FIN",syntax.curly[pos].index);
		l=add_str(label);
		set_label(l,script_pos,p);
		p = skip_space(p);
		if(*p != ';') {
			disp_error_message("parse_syntax: expected ';'",p);
			return p+1;
		}
		p++;
		syntax.curly_count--;
		return p;
	} else if(syntax.curly[pos].type == TYPE_FOR) {
		// for-block end is a new line
		parse_nextline(false, p);

		// Skip to the next loop
		sprintf(label,"goto __FR%x_NXT;",syntax.curly[pos].index);
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// End for labeling
		sprintf(label,"__FR%x_FIN",syntax.curly[pos].index);
		l=add_str(label);
		set_label(l,script_pos,p);
		syntax.curly_count--;
		return p;
	} else if(syntax.curly[pos].type == TYPE_WHILE) {
		// while-block end is a new line
		parse_nextline(false, p);

		// Skip to the decision while
		sprintf(label,"goto __WL%x_NXT;",syntax.curly[pos].index);
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// End while labeling
		sprintf(label,"__WL%x_FIN",syntax.curly[pos].index);
		l=add_str(label);
		set_label(l,script_pos,p);
		syntax.curly_count--;
		return p;
	} else if(syntax.curly[syntax.curly_count-1].type == TYPE_USERFUNC) {
		int pos = syntax.curly_count-1;
		char label[256];
		int l;
		// Back
		sprintf(label,"return;");
		syntax.curly[syntax.curly_count++].type = TYPE_NULL;
		parse_line(label);
		syntax.curly_count--;

		// Put the label of the location
		sprintf(label,"__FN%x_FIN",syntax.curly[pos].index);
		l=add_str(label);
		set_label(l,script_pos,p);
		syntax.curly_count--;
		return p;
	} else {
		*flag = 0;
		return p;
	}
}

/*==========================================
 * Added built-in functions
 *------------------------------------------*/
static void add_buildin_func(void)
{
	int i;

	for( i = 0; buildin_func[i].func; i++ ) {
		// arg must follow the pattern: (v|s|i|r|l)*\?*\*?
		// 'v' - value (either string or int or reference)
		// 's' - string
		// 'i' - int
		// 'r' - reference (of a variable)
		// 'l' - label
		// '?' - one optional parameter
		// '*' - unknown number of optional parameters
		const char* p = buildin_func[i].arg;

		while( *p == 'v' || *p == 's' || *p == 'i' || *p == 'r' || *p == 'l' ) ++p;
		while( *p == '?' ) ++p;
		if( *p == '*' ) ++p;
		if( *p != 0){
			ShowWarning("add_buildin_func: ignoring function \"%s\" with invalid arg \"%s\".\n", buildin_func[i].name, buildin_func[i].arg);
		} else if( *skip_word(buildin_func[i].name) != 0 ){
			ShowWarning("add_buildin_func: ignoring function with invalid name \"%s\" (must be a word).\n", buildin_func[i].name);
		} else {
			int n = add_str(buildin_func[i].name);

			str_data[n].type = C_FUNC;
			str_data[n].val = i;
			str_data[n].func = buildin_func[i].func;

			if( !strcmp(buildin_func[i].name, "set") ) buildin_set_ref = n;
			else if( !strcmp(buildin_func[i].name, "callsub") ) buildin_callsub_ref = n;
			else if( !strcmp(buildin_func[i].name, "callfunc") ) buildin_callfunc_ref = n;
			else if( !strcmp(buildin_func[i].name, "getelementofarray") ) buildin_getelementofarray_ref = n;
		}
	}
}

/// Retrieves the value of a constant.
bool script_get_constant(const char* name, int* value)
{
	int n = search_str(name);

	if( n == -1 || str_data[n].type != C_INT ) //Not found or not a constant
		return false;

	value[0] = str_data[n].val;

	return true;
}

/// Creates new constant or parameter with given value.
void script_set_constant(const char* name, int value, bool isparameter)
{
	int n = add_str(name);

	if( str_data[n].type == C_NOP ) { //New
		str_data[n].type = (isparameter ? C_PARAM : C_INT);
		str_data[n].val  = value;
	} else if( str_data[n].type == C_PARAM || str_data[n].type == C_INT ) //Existing parameter or constant
		ShowError("script_set_constant: Attempted to overwrite existing %s '%s' (old value=%d, new value=%d).\n", (str_data[n].type == C_PARAM) ? "parameter" : "constant", name, str_data[n].val, value);
	else //Existing name
		ShowError("script_set_constant: Invalid name for %s '%s' (already defined as %s).\n", (isparameter ? "parameter" : "constant"), name, script_op2name(str_data[n].type));
}

/*==========================================
 * Reading constant databases
 * const.txt
 *------------------------------------------*/
static void read_constdb(void)
{
	FILE *fp;
	char line[1024], name[1024], val[1024];
	int type;
	int entries = 0, skipped = 0, linenum = 0;

	sprintf(line, "%s/const.txt", db_path);
	fp = fopen(line, "r");
	if(fp == NULL) {
		ShowError("can't read %s\n", line);
		return ;
	}
	while(fgets(line, sizeof(line), fp)) {
		linenum++;
		if(line[0] == '\0' || line[0] == '\n' || line[0] == '\r') //Ignore empty line
			continue;
		if(line[0] == '/' && line[1] == '/') //Ignore commented line
			continue;
		type = 0;
		if(sscanf(line,"%1023[A-Za-z0-9/_],%1023[A-Za-z0-9/_-],%d", name, val, &type) >= 2 ||
			sscanf(line,"%1023[A-Za-z0-9/_] %1023[A-Za-z0-9/_-] %d", name, val, &type) >= 2) {
			entries++;
			script_set_constant(name, (int)strtol(val, NULL, 0), (bool)type);
		} else {
			skipped++;
			ShowWarning("Skipping line '"CL_WHITE"%d"CL_RESET"', invalid constant definition\n", linenum);
		}
	}
	fclose(fp);
	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s/const.txt"CL_RESET"'.\n", entries, db_path);
	if(skipped)
		ShowWarning("Skipped '"CL_WHITE"%d"CL_RESET"', entries\n", skipped);
}

/**
 * Sets source-end constants for NPC scripts to access.
 */
void script_hardcoded_constants(void)
{
	/* Server defines */
	script_set_constant("PACKETVER", PACKETVER, false);
	script_set_constant("MAX_LEVEL", MAX_LEVEL, false);
	script_set_constant("MAX_STORAGE", MAX_STORAGE, false);
	script_set_constant("MAX_INVENTORY", MAX_INVENTORY, false);
	script_set_constant("MAX_CART", MAX_INVENTORY, false);
	script_set_constant("MAX_ZENY", MAX_ZENY, false);
	script_set_constant("MAX_PARTY", MAX_PARTY, false);
	script_set_constant("MAX_GUILD", MAX_GUILD, false);
	script_set_constant("MAX_GUILDLEVEL", MAX_GUILDLEVEL, false);
	script_set_constant("MAX_GUILD_STORAGE", MAX_GUILD_STORAGE, false);
	script_set_constant("MAX_BG_MEMBERS", MAX_BG_MEMBERS, false);
	script_set_constant("MAX_CHAT_USERS", MAX_CHAT_USERS, false);
	script_set_constant("VIP_SCRIPT", VIP_SCRIPT, false);
	script_set_constant("MIN_STORAGE", MIN_STORAGE, false);
	script_set_constant("MAX_REFINE", MAX_REFINE, false);

	/* Status options */
	script_set_constant("Option_Nothing", OPTION_NOTHING, false);
	script_set_constant("Option_Sight", OPTION_SIGHT, false);
	script_set_constant("Option_Ruwach", OPTION_RUWACH, false);
	script_set_constant("Option_Hide", OPTION_HIDE, false);
	script_set_constant("Option_Cloak", OPTION_CLOAK, false);
	script_set_constant("Option_Falcon", OPTION_FALCON, false);
	script_set_constant("Option_Riding", OPTION_RIDING, false);
	script_set_constant("Option_Invisible", OPTION_INVISIBLE, false);
	script_set_constant("Option_Orcish", OPTION_ORCISH, false);
	script_set_constant("Option_Wedding", OPTION_WEDDING, false);
	script_set_constant("Option_Chasewalk", OPTION_CHASEWALK, false);
	script_set_constant("Option_Flying", OPTION_FLYING, false);
	script_set_constant("Option_Xmas", OPTION_XMAS, false);
	script_set_constant("Option_Transform", OPTION_TRANSFORM, false);
	script_set_constant("Option_Summer", OPTION_SUMMER, false);
	script_set_constant("Option_Dragon1", OPTION_DRAGON1, false);
	script_set_constant("Option_Wug", OPTION_WUG, false);
	script_set_constant("Option_Wugrider", OPTION_WUGRIDER, false);
	script_set_constant("Option_Madogear", OPTION_MADOGEAR, false);
	script_set_constant("Option_Dragon2", OPTION_DRAGON2, false);
	script_set_constant("Option_Dragon3", OPTION_DRAGON3, false);
	script_set_constant("Option_Dragon4", OPTION_DRAGON4, false);
	script_set_constant("Option_Dragon5", OPTION_DRAGON5, false);
	script_set_constant("Option_Hanbok", OPTION_HANBOK, false);
	script_set_constant("Option_Oktoberfest", OPTION_OKTOBERFEST, false);

	/* Status option compounds */
	script_set_constant("Option_Dragon", OPTION_DRAGON, false);
	script_set_constant("Option_Costume", OPTION_COSTUME, false);

	/* Bonus script flags */
	script_set_constant("BSF_REM_ON_DEAD", BSF_REM_ON_DEAD, false);
	script_set_constant("BSF_REM_ON_DISPELL", BSF_REM_ON_DISPELL, false);
	script_set_constant("BSF_REM_ON_CLEARANCE", BSF_REM_ON_CLEARANCE, false);
	script_set_constant("BSF_REM_ON_LOGOUT", BSF_REM_ON_LOGOUT, false);
	script_set_constant("BSF_REM_ON_BANISHING_BUSTER", BSF_REM_ON_BANISHING_BUSTER, false);
	script_set_constant("BSF_REM_ON_REFRESH", BSF_REM_ON_REFRESH, false);
	script_set_constant("BSF_REM_ON_LUXANIMA", BSF_REM_ON_LUXANIMA, false);
	script_set_constant("BSF_REM_ON_MADOGEAR", BSF_REM_ON_MADOGEAR, false);
	script_set_constant("BSF_REM_ON_DAMAGED", BSF_REM_ON_DAMAGED, false);
	script_set_constant("BSF_PERMANENT", BSF_PERMANENT, false);
	script_set_constant("BSF_REM_BUFF", BSF_REM_BUFF, false);
	script_set_constant("BSF_REM_DEBUFF", BSF_REM_DEBUFF, false);

	/* Status change flags */
	script_set_constant("SCFLAG_NONE", SCFLAG_NONE, false);
	script_set_constant("SCFLAG_NOAVOID", SCFLAG_NOAVOID, false);
	script_set_constant("SCFLAG_FIXEDTICK", SCFLAG_FIXEDTICK, false);
	script_set_constant("SCFLAG_LOADED", SCFLAG_LOADED, false);
	script_set_constant("SCFLAG_FIXEDRATE", SCFLAG_FIXEDRATE, false);
	script_set_constant("SCFLAG_NOICON", SCFLAG_NOICON, false);
}

/*==========================================
 * Display emplacement line of script
 *------------------------------------------*/
static const char* script_print_line(StringBuf* buf, const char* p, const char* mark, int line)
{
	int i;
	if( p == NULL || !p[0] ) return NULL;
	if( line < 0 )
		StringBuf_Printf(buf, "*% 5d : ", -line);
	else
		StringBuf_Printf(buf, " % 5d : ", line);
	for( i = 0; p[i] && p[i] != '\n'; i++) {
		if( p + i != mark )
			StringBuf_Printf(buf, "%c", p[i]);
		else
			StringBuf_Printf(buf, "\'%c\'", p[i]);
	}
	StringBuf_AppendStr(buf, "\n");
	return p + i + (p[i] == '\n' ? 1 : 0);
}

void script_errorwarning_sub(StringBuf *buf, const char* src, const char* file, int start_line, const char* error_msg, const char* error_pos)
{
	//Find the line where the error occurred
	int j;
	int line = start_line;
	const char *p;
	const char *linestart[5] = { NULL, NULL, NULL, NULL, NULL };

	for(p = src; p && *p; line++) {
		const char *lineend = strchr(p,'\n');
		if(lineend == NULL || error_pos < lineend) {
			break;
		}
		for(j = 0; j < 4; j++) {
			linestart[j] = linestart[j + 1];
		}
		linestart[4] = p;
		p = lineend + 1;
	}

	StringBuf_Printf(buf, "script error in file '%s' line %d\n", file, line);
	StringBuf_Printf(buf, "    %s\n", error_msg);
	for(j = 0; j < 5; j++ ) {
		script_print_line(buf, linestart[j], NULL, line + j - 5);
	}
	p = script_print_line(buf, p, error_pos, -line);
	for(j = 0; j < 5; j++) {
		p = script_print_line(buf, p, NULL, line + j + 1 );
	}
}

void script_error(const char* src, const char* file, int start_line, const char* error_msg, const char* error_pos) {
	StringBuf buf;

	StringBuf_Init(&buf);
	StringBuf_AppendStr(&buf, "\a\n");

	script_errorwarning_sub(&buf, src, file, start_line, error_msg, error_pos);

	ShowError("%s", StringBuf_Value(&buf));
	StringBuf_Destroy(&buf);
}

void script_warning(const char* src, const char* file, int start_line, const char* error_msg, const char* error_pos) {
	StringBuf buf;

	StringBuf_Init(&buf);

	script_errorwarning_sub(&buf, src, file, start_line, error_msg, error_pos);

	ShowWarning("%s", StringBuf_Value(&buf));
	StringBuf_Destroy(&buf);
}

/*==========================================
 * Analysis of the script
 *------------------------------------------*/
struct script_code* parse_script(const char *src,const char *file,int line,int options)
{
	const char *p, *tmpp;
	int i;
	struct script_code* code = NULL;
	static int first = 1;
	char end;
	bool unresolved_names = false;

	parser_current_src = src;
	parser_current_file = file;
	parser_current_line = line;

	if( src == NULL )
		return NULL; //Empty script

	memset(&syntax,0,sizeof(syntax));
	if( first ) {
		add_buildin_func();
		read_constdb();
		script_hardcoded_constants();
		first = 0;
	}

	script_buf = (unsigned char *)aMalloc(SCRIPT_BLOCK_SIZE*sizeof(unsigned char));
	script_pos = 0;
	script_size = SCRIPT_BLOCK_SIZE;
	parse_nextline(true, NULL);

	//Who called parse_script is responsible for clearing the database after using it, but just in case... lets clear it here
	if( options&SCRIPT_USE_LABEL_DB )
		db_clear(scriptlabel_db);
	parse_options = options;

	if( setjmp( error_jump ) != 0 ) {
		//Restore program state when script has problems. [from jA]
		int i;
		const int size = ARRAYLENGTH(syntax.curly);
		if( error_report )
			script_error(src,file,line,error_msg,error_pos);
		aFree( error_msg );
		aFree( script_buf );
		script_pos  = 0;
		script_size = 0;
		script_buf  = NULL;
		for( i = LABEL_START; i < str_num; i++ )
			if(str_data[i].type == C_NOP) str_data[i].type = C_NAME;
		for( i = 0; i < size; i++ )
			linkdb_final(&syntax.curly[i].case_label);
		return NULL;
	}

	parse_syntax_for_flag = 0;
	p = src;
	p = skip_space(p);
	if( options&SCRIPT_IGNORE_EXTERNAL_BRACKETS ) { //Does not require brackets around the script
		if( *p == '\0' && !(options&SCRIPT_RETURN_EMPTY_SCRIPT) ) { //Empty script and can return NULL
			aFree( script_buf );
			script_pos  = 0;
			script_size = 0;
			script_buf  = NULL;
			return NULL;
		}
		end = '\0';
	} else { //Requires brackets around the script
		if( *p != '{' )
			disp_error_message("not found '{'",p);
		p = skip_space(p + 1);
		if( *p == '}' && !(options&SCRIPT_RETURN_EMPTY_SCRIPT) ) { //Empty script and can return NULL
			aFree( script_buf );
			script_pos  = 0;
			script_size = 0;
			script_buf  = NULL;
			return NULL;
		}
		end = '}';
	}

	//Clear references of labels, variables and internal functions
	for( i = LABEL_START; i < str_num; i++ ) {
		if(
			str_data[i].type == C_POS || str_data[i].type == C_NAME ||
			str_data[i].type == C_USERFUNC || str_data[i].type == C_USERFUNC_POS
		) {
			str_data[i].type = C_NOP;
			str_data[i].backpatch = -1;
			str_data[i].label = -1;
		}
	}

	while( syntax.curly_count != 0 || *p != end ) {
		if( *p == '\0' )
			disp_error_message("unexpected end of script",p);
		//Special handling only label
		tmpp = skip_space(skip_word(p));
		if( *tmpp == ':' && !(!strncasecmp(p,"default:",8) && p + 7 == tmpp) ) {
			i = add_word(p);
			set_label(i,script_pos,p);
			if( parse_options&SCRIPT_USE_LABEL_DB )
				strdb_iput(scriptlabel_db, get_str(i), script_pos);
			p = tmpp + 1;
			p = skip_space(p);
			continue;
		}

		//All other lumped
		p = parse_line(p);
		p = skip_space(p);

		parse_nextline(false, p);
	}

	add_scriptc(C_NOP);

	//Trim code to size
	script_size = script_pos;
	RECREATE(script_buf,unsigned char,script_pos);

	//Default unknown references to variables
	for( i = LABEL_START; i < str_num; i++ ) {
		if( str_data[i].type == C_NOP ) {
			int j;

			str_data[i].type = C_NAME;
			str_data[i].label = i;
			for( j = str_data[i].backpatch; j >= 0 && j != 0x00ffffff; ) {
				int next = GETVALUE(script_buf,j);

				SETVALUE(script_buf,j,i);
				j = next;
			}
		} else if( str_data[i].type == C_USERFUNC ) { //'function name;' without follow-up code
			ShowError("parse_script: function '%s' declared but not defined.\n", str_buf+str_data[i].str);
			unresolved_names = true;
		}
	}

	if( unresolved_names ) {
		disp_error_message("parse_script: unresolved function references", p);
	}

#ifdef DEBUG_DISP
	for( i = 0; i < script_pos; i++ ) {
		if( (i&15) == 0 ) ShowMessage("%04x : ",i);
		ShowMessage("%02x ",script_buf[i]);
		if( (i&15) == 15 ) ShowMessage("\n");
	}
	ShowMessage("\n");
#endif
#ifdef DEBUG_DISASM
	{
		int i = 0,j;
		while( i < script_pos ) {
			c_op op = get_com(script_buf,&i);

			ShowMessage("%06x %s", i, script_op2name(op));
			j = i;
			switch( op ) {
				case C_INT:
					ShowMessage(" %d", get_num(script_buf,&i));
					break;
				case C_POS:
					ShowMessage(" 0x%06x", *(int*)(script_buf + i)&0xffffff);
					i += 3;
					break;
				case C_NAME:
					j = (*(int*)(script_buf + i)&0xffffff);
					ShowMessage(" %s", ( j == 0xffffff ) ? "?? unknown ??" : get_str(j));
					i += 3;
					break;
				case C_STR:
					j = strlen(script_buf + i);
					ShowMessage(" %s", script_buf + i);
					i += j+1;
					break;
			}
			ShowMessage(CL_CLL"\n");
		}
	}
#endif

	CREATE(code,struct script_code,1);
	code->script_buf  = script_buf;
	code->script_size = script_size;
	code->script_vars = idb_alloc(DB_OPT_RELEASE_DATA);
	return code;
}

/// Returns the player attached to this script, identified by the rid.
/// If there is no player attached, the script is terminated.
TBL_PC *script_rid2sd(struct script_state *st)
{
	TBL_PC *sd;

	if( !(sd = map_id2sd(st->rid)) ) {
		ShowError("script_rid2sd: fatal error ! Player not attached!\n");
		script_reportfunc(st);
		script_reportsrc(st);
		st->state = END;
	}
	return sd;
}

/// Dereferences a variable/constant, replacing it with a copy of the value.
///
/// @param st Script state
/// @param data Variable/constant
void get_val(struct script_state* st, struct script_data* data)
{
	const char* name;
	char prefix;
	char postfix;
	TBL_PC* sd = NULL;

	if( !data_isreference(data) )
		return; // Not a variable/constant

	name = reference_getname(data);
	prefix = name[0];
	postfix = name[strlen(name) - 1];

	// @TODO: Use reference_tovariable(data) when it's confirmed that it works [FlavioJS]
	if( !reference_toconstant(data) && not_server_variable(prefix) ) {
		sd = script_rid2sd(st);
		if( sd == NULL ) { // Needs player attached
			if( postfix == '$' ) { // String variable
				ShowWarning("script:get_val: cannot access player variable '%s', defaulting to \"\"\n", name);
				data->type = C_CONSTSTR;
				data->u.str = "";
			} else { // Integer variable
				ShowWarning("script:get_val: cannot access player variable '%s', defaulting to 0\n", name);
				data->type = C_INT;
				data->u.num = 0;
			}
			return;
		}
	}

	if( postfix == '$' ) { // String variable

		switch( prefix ) {
			case '@':
				data->u.str = pc_readregstr(sd, data->u.num);
				break;
			case '$':
				data->u.str = mapreg_readregstr(data->u.num);
				break;
			case '#':
				if( name[1] == '#' )
					data->u.str = pc_readaccountreg2str(sd, name); // Global
				else
					data->u.str = pc_readaccountregstr(sd, name); // Local
				break;
			case '.': {
					struct DBMap* n =
						data->ref      ? *data->ref:
						name[1] == '@' ?  st->stack->var_function: // Instance/scope variable
										  st->script->script_vars; // Npc variable
					if( n )
						data->u.str = (char*)idb_get(n,reference_getuid(data));
					else
						data->u.str = NULL;
				}
				break;
			case '\'': {
						int instance_id = script_instancegetid(st);
						if( instance_id )
							data->u.str = (char*)idb_get(instance_data[instance_id].vars,reference_getuid(data));
						else {
							ShowWarning("script:get_val: cannot access instance variable '%s', defaulting to \"\"\n", name);
							data->u.str = NULL;
						}
					}
				break;
			default:
				data->u.str = pc_readglobalreg_str(sd, name);
				break;
		}

		if( data->u.str == NULL || data->u.str[0] == '\0' ) { // Empty string
			data->type = C_CONSTSTR;
			data->u.str = "";
		} else { // Duplicate string
			data->type = C_STR;
			data->u.str = aStrdup(data->u.str);
		}

	} else { // Integer variable

		data->type = C_INT;

		if( reference_toconstant(data) ) {
			data->u.num = reference_getconstant(data);
		} else if( reference_toparam(data) ) {
			data->u.num = pc_readparam(sd, reference_getparamtype(data));
		} else
			switch( prefix ) {
				case '@':
					data->u.num = pc_readreg(sd, data->u.num);
					break;
				case '$':
					data->u.num = mapreg_readreg(data->u.num);
					break;
				case '#':
					if( name[1] == '#' )
						data->u.num = pc_readaccountreg2(sd, name); // Global
					else
						data->u.num = pc_readaccountreg(sd, name); // Local
					break;
				case '.': {
						struct DBMap* n =
							data->ref      ? *data->ref:
							name[1] == '@' ?  st->stack->var_function: // Instance/scope variable
											  st->script->script_vars; // Npc variable
						if( n )
							data->u.num = (int)idb_iget(n,reference_getuid(data));
						else
							data->u.num = 0;
					}
					break;
				case '\'': {
						int instance_id = script_instancegetid(st);
						if( instance_id )
							data->u.num = (int)idb_iget(instance_data[instance_id].vars,reference_getuid(data));
						else {
							ShowWarning("script:get_val: cannot access instance variable '%s', defaulting to 0\n", name);
							data->u.num = 0;
						}
					}
					break;
				default:
					data->u.num = pc_readglobalreg(sd, name);
					break;
			}

	}

	return;
}

struct script_data* push_val2(struct script_stack* stack, enum c_op type, int val, struct DBMap** ref);

/// Retrieves the value of a reference identified by uid (variable, constant, param)
/// The value is left in the top of the stack and needs to be removed manually.
void* get_val2(struct script_state* st, int uid, struct DBMap** ref)
{
	struct script_data* data;
	push_val2(st->stack, C_NAME, uid, ref);
	data = script_getdatatop(st, -1);
	get_val(st, data);
	return (data->type == C_INT ? (void*)__64BPRTSIZE(data->u.num) : (void*)__64BPRTSIZE(data->u.str));
}

/*==========================================
 * Stores the value of a script variable
 * Return value is 0 on fail, 1 on success.
 *------------------------------------------*/
static int set_reg(struct script_state* st, TBL_PC* sd, int num, const char* name, const void* value, struct DBMap** ref)
{
	char prefix = name[0];

	if( is_string_variable(name) ) { // String variable
		const char* str = (const char*)value;

		switch( prefix ) {
			case '@':
				return pc_setregstr(sd, num, str);
			case '$':
				return mapreg_setregstr(num, str);
			case '#':
				return (name[1] == '#') ?
					pc_setaccountreg2str(sd, name, str) :
					pc_setaccountregstr(sd, name, str);
			case '.': {
					struct DBMap* n = (ref) ? *ref : (name[1] == '@') ? st->stack->var_function : st->script->script_vars;

					if( n ) {
						idb_remove(n, num);
						if( str[0] )
							idb_put(n, num, aStrdup(str));
					}
				}
				return 1;
			case '\'': {
					int instance_id = script_instancegetid(st);

					if( instance_id ) {
						idb_remove(instance_data[instance_id].vars, num);
						if( str[0] )
							idb_put(instance_data[instance_id].vars, num, aStrdup(str));
					}
				}
				return 1;
			default:
				return pc_setglobalreg_str(sd, name, str);
		}
	} else { // Integer variable
		int val = (int)__64BPRTSIZE(value);

		if( str_data[num&0x00ffffff].type == C_PARAM ) {
			if( pc_setparam(sd, str_data[num&0x00ffffff].val, val) == 0 ) {
				if( st != NULL ) {
					ShowError("script:set_reg: failed to set param '%s' to %d.\n", name, val);
					script_reportsrc(st);
					st->state = END;
				}
				return 0;
			}
			return 1;
		}

		switch( prefix ) {
			case '@':
				return pc_setreg(sd, num, val);
			case '$':
				return mapreg_setreg(num, val);
			case '#':
				return (name[1] == '#') ?
					pc_setaccountreg2(sd, name, val) :
					pc_setaccountreg(sd, name, val);
			case '.': {
					struct DBMap* n = (ref) ? *ref : (name[1] == '@') ? st->stack->var_function : st->script->script_vars;

					if( n ) {
						idb_remove(n, num);
						if( val != 0 )
							idb_iput(n, num, val);
					}
				}
				return 1;
			case '\'': {
					int instance_id = script_instancegetid(st);

					if( instance_id ) {
						idb_remove(instance_data[instance_id].vars, num);
						if( val != 0 )
							idb_iput(instance_data[instance_id].vars, num, val);
					}
				}
				return 1;
			default:
				return pc_setglobalreg(sd, name, val);
		}
	}
}

int set_var(TBL_PC* sd, char* name, void* val)
{
    return set_reg(NULL, sd, reference_uid(add_str(name),0), name, val, NULL);
}

void setd_sub(struct script_state *st, TBL_PC *sd, const char *varname, int elem, void *value, struct DBMap **ref)
{
	set_reg(st, sd, reference_uid(add_str(varname),elem), varname, value, ref);
}

/// Converts the data to a string
const char* conv_str(struct script_state* st, struct script_data* data)
{
	char* p;

	get_val(st, data);
	if( data_isstring(data) ) {
		// nothing to convert
	} else if( data_isint(data) ) { // int -> string
		CREATE(p, char, ITEM_NAME_LENGTH);
		snprintf(p, ITEM_NAME_LENGTH, "%d", data->u.num);
		p[ITEM_NAME_LENGTH - 1] = '\0';
		data->type = C_STR;
		data->u.str = p;
	} else if( data_isreference(data) ) { // reference -> string
		// @TODO: When does this happen (check get_val) [FlavioJS]
		data->type = C_CONSTSTR;
		data->u.str = reference_getname(data);
	} else { // unsupported data type
		ShowError("script:conv_str: cannot convert to string, defaulting to \"\"\n");
		script_reportdata(data);
		script_reportsrc(st);
		data->type = C_CONSTSTR;
		data->u.str = "";
	}

	return data->u.str;
}

/// Converts the data to an int
int conv_num(struct script_state* st, struct script_data* data)
{
	get_val(st, data);
	if( data_isint(data) ) {
		// Nothing to convert
	} else if( data_isstring(data) ) { // string -> int
		// The result does not overflow or underflow, it is capped instead
		// ex: 999999999999 is capped to INT_MAX (2147483647)
		char* p = data->u.str;
		long num;

		errno = 0;
		num = strtol(data->u.str, NULL, 10); // Change radix to 0 to support octal numbers "o377" and hex numbers "0xFF"
		if( errno == ERANGE
#if LONG_MAX > INT_MAX
			|| num < INT_MIN || num > INT_MAX
#endif
			)
		{
			if( num <= INT_MIN ) {
				num = INT_MIN;
				ShowError("script:conv_num: underflow detected, capping to %ld\n", num);
			} else /*if( num >= INT_MAX )*/ {
				num = INT_MAX;
				ShowError("script:conv_num: overflow detected, capping to %ld\n", num);
			}
			script_reportdata(data);
			script_reportsrc(st);
		}
		if( data->type == C_STR )
			aFree(p);
		data->type = C_INT;
		data->u.num = (int)num;
	}
#if 0
	// FIXME this function is being used to retrieve the position of labels and
	// probably other stuff [FlavioJS]
	else { // unsupported data type
		ShowError("script:conv_num: cannot convert to number, defaulting to 0\n");
		script_reportdata(data);
		script_reportsrc(st);
		data->type = C_INT;
		data->u.num = 0;
	}
#endif
	return data->u.num;
}

//
// Stack operations
//

/// Increases the size of the stack
void stack_expand(struct script_stack* stack)
{
	stack->sp_max += 64;
	stack->stack_data = (struct script_data*)aRealloc(stack->stack_data,
			stack->sp_max * sizeof(stack->stack_data[0]) );
	memset(stack->stack_data + (stack->sp_max - 64), 0,
			64 * sizeof(stack->stack_data[0]) );
}

/// Pushes a value into the stack
#define push_val(stack,type,val) push_val2(stack, type, val, NULL)

/// Pushes a value into the stack (with reference)
struct script_data* push_val2(struct script_stack* stack, enum c_op type, int val, struct DBMap** ref)
{
	if( stack->sp >= stack->sp_max )
		stack_expand(stack);
	stack->stack_data[stack->sp].type  = type;
	stack->stack_data[stack->sp].u.num = val;
	stack->stack_data[stack->sp].ref   = ref;
	stack->sp++;
	return &stack->stack_data[stack->sp-1];
}

/// Pushes a string into the stack
struct script_data* push_str(struct script_stack* stack, enum c_op type, char* str)
{
	if( stack->sp >= stack->sp_max )
		stack_expand(stack);
	stack->stack_data[stack->sp].type  = type;
	stack->stack_data[stack->sp].u.str = str;
	stack->stack_data[stack->sp].ref   = NULL;
	stack->sp++;
	return &stack->stack_data[stack->sp-1];
}

/// Pushes a retinfo into the stack
struct script_data* push_retinfo(struct script_stack* stack, struct script_retinfo* ri, DBMap **ref)
{
	if( stack->sp >= stack->sp_max )
		stack_expand(stack);
	stack->stack_data[stack->sp].type = C_RETINFO;
	stack->stack_data[stack->sp].u.ri = ri;
	stack->stack_data[stack->sp].ref  = ref;
	stack->sp++;
	return &stack->stack_data[stack->sp-1];
}

/// Pushes a copy of the target position into the stack
struct script_data* push_copy(struct script_stack* stack, int pos)
{
	switch( stack->stack_data[pos].type ) {
		case C_CONSTSTR:
			return push_str(stack, C_CONSTSTR, stack->stack_data[pos].u.str);
			break;
		case C_STR:
			return push_str(stack, C_STR, aStrdup(stack->stack_data[pos].u.str));
			break;
		case C_RETINFO:
			ShowFatalError("script:push_copy: can't create copies of C_RETINFO. Exiting...\n");
			exit(1);
			break;
		default:
			return push_val2(
				stack,stack->stack_data[pos].type,
				stack->stack_data[pos].u.num,
				stack->stack_data[pos].ref
			);
			break;
	}
}

/// Removes the values in indexes [start,end] from the stack.
/// Adjusts all stack pointers.
void pop_stack(struct script_state* st, int start, int end)
{
	struct script_stack* stack = st->stack;
	struct script_data* data;
	int i;

	if( start < 0 )
		start = 0;
	if( end > stack->sp )
		end = stack->sp;
	if( start >= end )
		return; // Nothing to pop

	// Free stack elements
	for( i = start; i < end; i++ ) {
		data = &stack->stack_data[i];
		if( data->type == C_STR )
			aFree(data->u.str);
		if( data->type == C_RETINFO ) {
			struct script_retinfo* ri = data->u.ri;

			if( ri->var_function )
				script_free_vars(ri->var_function);
			if( data->ref )
				aFree(data->ref);
			aFree(ri);
		}
		data->type = C_NOP;
	}
	// Move the rest of the elements
	if( stack->sp > end ) {
		memmove(&stack->stack_data[start], &stack->stack_data[end], sizeof(stack->stack_data[0])*(stack->sp - end));
		for( i = start + stack->sp - end; i < stack->sp; ++i )
			stack->stack_data[i].type = C_NOP;
	}
	// Adjust stack pointers
	     if( st->start > end )   st->start -= end - start;
	else if( st->start > start ) st->start = start;
	     if( st->end > end )   st->end -= end - start;
	else if( st->end > start ) st->end = start;
	     if( stack->defsp > end )   stack->defsp -= end - start;
	else if( stack->defsp > start ) stack->defsp = start;
	stack->sp -= end - start;
}

///
///
///

/*==========================================
 * Release script dependent variable, dependent variable of function
 *------------------------------------------*/
void script_free_vars(struct DBMap* storage)
{
	if( storage ) // Destroy the storage construct containing the variables
		db_destroy(storage);
}

void script_free_code(struct script_code* code)
{
	nullpo_retv(code);

	script_free_vars(code->script_vars);
	aFree(code->script_buf);
	aFree(code);
}

/// Creates a new script state.
///
/// @param script Script code
/// @param pos Position in the code
/// @param rid Who is running the script (attached player)
/// @param oid Where the code is being run (npc 'object')
/// @return Script state
struct script_state* script_alloc_state(struct script_code* script, int pos, int rid, int oid)
{
	struct script_state* st;
	CREATE(st, struct script_state, 1);
	st->stack = (struct script_stack*)aMalloc(sizeof(struct script_stack));
	st->stack->sp = 0;
	st->stack->sp_max = 64;
	CREATE(st->stack->stack_data, struct script_data, st->stack->sp_max);
	st->stack->defsp = st->stack->sp;
	st->stack->var_function = idb_alloc(DB_OPT_RELEASE_DATA);
	st->state = RUN;
	st->script = script;
	//st->scriptroot = script;
	st->pos = pos;
	st->rid = rid;
	st->oid = oid;
	st->sleep.timer = INVALID_TIMER;
	st->npc_item_flag = battle_config.item_enabled_npc;
	return st;
}

/// Frees a script state.
///
/// @param st Script state
void script_free_state(struct script_state* st)
{
	if(st->bk_st) { // backup was not restored
		ShowDebug("script_free_state: Previous script state lost (rid=%d, oid=%d, state=%d, bk_npcid=%d).\n", st->bk_st->rid, st->bk_st->oid, st->bk_st->state, st->bk_npcid);
	}
	if( st->sleep.timer != INVALID_TIMER )
		delete_timer(st->sleep.timer, run_script_timer);
	script_free_vars(st->stack->var_function);
	pop_stack(st, 0, st->stack->sp);
	aFree(st->stack->stack_data);
	aFree(st->stack);
	st->stack = NULL;
	st->pos = -1;
	aFree(st);
}

//
// Main execution unit
//
/*==========================================
 * Read command
 *------------------------------------------*/
c_op get_com(unsigned char *script,int *pos)
{
	int i = 0, j = 0;

	if(script[*pos]>=0x80) {
		return C_INT;
	}
	while(script[*pos]>=0x40) {
		i=script[(*pos)++]<<j;
		j+=6;
	}
	return (c_op)(i+(script[(*pos)++]<<j));
}

/*==========================================
 * �Income figures
 *------------------------------------------*/
int get_num(unsigned char *script,int *pos)
{
	int i,j;
	i=0; j=0;
	while(script[*pos]>=0xc0) {
		i+=(script[(*pos)++]&0x7f)<<j;
		j+=6;
	}
	return i+((script[(*pos)++]&0x7f)<<j);
}

/*==========================================
 * Remove the value from the stack
 *------------------------------------------*/
int pop_val(struct script_state* st)
{
	if(st->stack->sp<=0)
		return 0;
	st->stack->sp--;
	get_val(st,&(st->stack->stack_data[st->stack->sp]));
	if(st->stack->stack_data[st->stack->sp].type==C_INT)
		return st->stack->stack_data[st->stack->sp].u.num;
	return 0;
}

/// Ternary operators
/// test ? if_true : if_false
void op_3(struct script_state* st, int op)
{
	struct script_data* data;
	int flag = 0;

	data = script_getdatatop(st, -3);
	get_val(st, data);

	if( data_isstring(data) )
		flag = data->u.str[0];// "" -> false
	else if( data_isint(data) )
		flag = data->u.num;// 0 -> false
	else
	{
		ShowError("script:op_3: invalid data for the ternary operator test\n");
		script_reportdata(data);
		script_reportsrc(st);
		script_removetop(st, -3, 0);
		script_pushnil(st);
		return;
	}
	if( flag )
		script_pushcopytop(st, -2);
	else
		script_pushcopytop(st, -1);
	script_removetop(st, -4, -1);
}

/// Binary string operators
/// s1 EQ s2 -> i
/// s1 NE s2 -> i
/// s1 GT s2 -> i
/// s1 GE s2 -> i
/// s1 LT s2 -> i
/// s1 LE s2 -> i
/// s1 ADD s2 -> s
void op_2str(struct script_state* st, int op, const char* s1, const char* s2)
{
	int a = 0;

	switch(op){
	case C_EQ: a = (strcmp(s1,s2) == 0); break;
	case C_NE: a = (strcmp(s1,s2) != 0); break;
	case C_GT: a = (strcmp(s1,s2) >  0); break;
	case C_GE: a = (strcmp(s1,s2) >= 0); break;
	case C_LT: a = (strcmp(s1,s2) <  0); break;
	case C_LE: a = (strcmp(s1,s2) <= 0); break;
	case C_ADD:
		{
			char* buf = (char *)aMalloc((strlen(s1)+strlen(s2)+1)*sizeof(char));
			strcpy(buf, s1);
			strcat(buf, s2);
			script_pushstr(st, buf);
			return;
		}
	default:
		ShowError("script:op2_str: unexpected string operator %s\n", script_op2name(op));
		script_reportsrc(st);
		script_pushnil(st);
		st->state = END;
		return;
	}

	script_pushint(st,a);
}

/// Binary number operators
/// i OP i -> i
void op_2num(struct script_state* st, int op, int i1, int i2)
{
	int ret;
	double ret_double;

	switch( op ) {
		case C_AND:  ret = i1 & i2;		break;
		case C_OR:   ret = i1 | i2;		break;
		case C_XOR:  ret = i1 ^ i2;		break;
		case C_LAND: ret = (i1 && i2);	break;
		case C_LOR:  ret = (i1 || i2);	break;
		case C_EQ:   ret = (i1 == i2);	break;
		case C_NE:   ret = (i1 != i2);	break;
		case C_GT:   ret = (i1 >  i2);	break;
		case C_GE:   ret = (i1 >= i2);	break;
		case C_LT:   ret = (i1 <  i2);	break;
		case C_LE:   ret = (i1 <= i2);	break;
		case C_R_SHIFT: ret = i1>>i2;	break;
		case C_L_SHIFT: ret = i1<<i2;	break;
		case C_DIV:
		case C_MOD:
			if( i2 == 0 ) {
				ShowError("script:op_2num: division by zero detected op=%s i1=%d i2=%d\n", script_op2name(op), i1, i2);
				script_reportsrc(st);
				script_pushnil(st);
				st->state = END;
				return;
			} else if( op == C_DIV )
				ret = i1 / i2;
			else //if( op == C_MOD )
				ret = i1 % i2;
			break;
		default:
			switch( op ) { // Operators that can overflow/underflow
				case C_ADD: ret = i1 + i2; ret_double = (double)i1 + (double)i2; break;
				case C_SUB: ret = i1 - i2; ret_double = (double)i1 - (double)i2; break;
				case C_MUL: ret = i1 * i2; ret_double = (double)i1 * (double)i2; break;
				default:
					ShowError("script:op_2num: unexpected number operator %s i1=%d i2=%d\n", script_op2name(op), i1, i2);
					script_reportsrc(st);
					script_pushnil(st);
					return;
			}
			if( ret_double < (double)INT_MIN ) {
				ShowWarning("script:op_2num: underflow detected op=%s i1=%d i2=%d\n", script_op2name(op), i1, i2);
				script_reportsrc(st);
				ret = INT_MIN;
			} else if( ret_double > (double)INT_MAX ) {
				ShowWarning("script:op_2num: overflow detected op=%s i1=%d i2=%d\n", script_op2name(op), i1, i2);
				script_reportsrc(st);
				ret = INT_MAX;
			}
	}
	script_pushint(st, ret);
}

/// Binary operators
void op_2(struct script_state *st, int op)
{
	struct script_data* left, leftref;
	struct script_data* right;

	leftref.type = C_NOP;

	left = script_getdatatop(st, -2);
	right = script_getdatatop(st, -1);

	if (st->op2ref) {
		if (data_isreference(left)) {
			leftref = *left;
		}

		st->op2ref = 0;
	}

	get_val(st, left);
	get_val(st, right);

	//Automatic conversions
	switch( op ) {
		case C_ADD:
			if( data_isint(left) && data_isstring(right) ) { //Convert int-string to string-string
				conv_str(st, left);
			} else if( data_isstring(left) && data_isint(right) ) { //Convert string-int to string-string
				conv_str(st, right);
			}
			break;
	}

	if( data_isstring(left) && data_isstring(right) ) { //ss => op_2str
		op_2str(st, op, left->u.str, right->u.str);
		script_removetop(st, leftref.type == C_NOP ? -3 : -2, -1);//Pop the two values before the top one

		if (leftref.type != C_NOP) {
			if (left->type == C_STR) //Don't free C_CONSTSTR
				aFree(left->u.str);
			*left = leftref;
		}
	} else if( data_isint(left) && data_isint(right) ) { //ii => op_2num
		int i1 = left->u.num;
		int i2 = right->u.num;

		script_removetop(st, leftref.type == C_NOP ? -2 : -1, 0);
		op_2num(st, op, i1, i2);

		if (leftref.type != C_NOP)
			*left = leftref;
	} else { //Invalid argument
		ShowError("script:op_2: invalid data for operator %s\n", script_op2name(op));
		script_reportdata(left);
		script_reportdata(right);
		script_reportsrc(st);
		script_removetop(st, -2, 0);
		script_pushnil(st);
		st->state = END;
	}
}

/// Unary operators
/// NEG i -> i
/// NOT i -> i
/// LNOT i -> i
void op_1(struct script_state* st, int op)
{
	struct script_data* data;
	int i1;

	data = script_getdatatop(st, -1);
	get_val(st, data);

	if( !data_isint(data) ) { // not a number
		ShowError("script:op_1: argument is not a number (op=%s)\n", script_op2name(op));
		script_reportdata(data);
		script_reportsrc(st);
		script_pushnil(st);
		st->state = END;
		return;
	}

	i1 = data->u.num;
	script_removetop(st, -1, 0);
	switch( op ) {
		case C_NEG: i1 = -i1; break;
		case C_NOT: i1 = ~i1; break;
		case C_LNOT: i1 = !i1; break;
		default:
			ShowError("script:op_1: unexpected operator %s i1=%d\n", script_op2name(op), i1);
			script_reportsrc(st);
			script_pushnil(st);
			st->state = END;
			return;
	}
	script_pushint(st, i1);
}


/// Checks the type of all arguments passed to a built-in function.
///
/// @param st Script state whose stack arguments should be inspected.
/// @param func Built-in function for which the arguments are intended.
static void script_check_buildin_argtype(struct script_state* st, int func)
{
	int idx, invalid = 0;

	for( idx = 2; script_hasdata(st, idx); idx++ ) {
		struct script_data* data = script_getdata(st, idx);
		script_function* sf = &buildin_func[str_data[func].val];
		char type = sf->arg[idx - 2];

		if( type == '?' || type == '*' )
			break; // Optional argument or unknown number of optional parameters (no types are after this)
		else if( type == 0 ) { // More arguments than necessary (should not happen, as it is checked before)
			ShowWarning("Found more arguments than necessary. unexpected arg type %s\n", script_op2name(data->type));
			invalid++;
			break;
		} else {
			const char* name = NULL;

			// Get name for variables to determine the type they refer to
			if( data_isreference(data) )
				name = reference_getname(data);

			switch( type ) {
				case 'v':
					if( !data_isstring(data) && !data_isint(data) && !data_isreference(data) ) { // Variant
						ShowWarning("Unexpected type for argument %d. Expected string, number or variable.\n", idx - 1);
						script_reportdata(data);
						invalid++;
					}
					break;
				case 's':
					if( !data_isstring(data) && !(data_isreference(data) && is_string_variable(name)) ) { // String
						ShowWarning("Unexpected type for argument %d. Expected string.\n", idx - 1);
						script_reportdata(data);
						invalid++;
					}
					break;
				case 'i':
					if( !data_isint(data) && !(data_isreference(data) &&
						(reference_toparam(data) || reference_toconstant(data) || !is_string_variable(name))) ) {
						// Int (params and constants are always int)
						ShowWarning("Unexpected type for argument %d. Expected number.\n", idx - 1);
						script_reportdata(data);
						invalid++;
					}
					break;
				case 'r':
					if( !data_isreference(data) ) { // Variables
						ShowWarning("Unexpected type for argument %d. Expected variable, got %s.\n", idx - 1, script_op2name(data->type));
						script_reportdata(data);
						invalid++;
					}
					break;
				case 'l':
					if( !data_islabel(data) && !data_isfunclabel(data) ) { // Label
						ShowWarning("Unexpected type for argument %d. Expected label, got %s\n", idx - 1, script_op2name(data->type));
						script_reportdata(data);
						invalid++;
					}
					break;
			}
		}
	}

	if( invalid ) {
		ShowDebug("Function: %s\n", get_str(func));
		script_reportsrc(st);
	}
}


/// Executes a buildin command.
/// Stack: C_NAME(<command>) C_ARG <arg0> <arg1> ... <argN>
int run_func(struct script_state *st)
{
	struct script_data* data;
	int i,start_sp,end_sp,func;

	end_sp = st->stack->sp;// position after the last argument
	for( i = end_sp-1; i > 0 ; --i )
		if( st->stack->stack_data[i].type == C_ARG )
			break;
	if( i == 0 ) {
		ShowError("script:run_func: C_ARG not found. please report this!!!\n");
		st->state = END;
		script_reportsrc(st);
		return 1;
	}
	start_sp = i-1;// C_NAME of the command
	st->start = start_sp;
	st->end = end_sp;

	data = &st->stack->stack_data[st->start];
	if( data->type == C_NAME && str_data[data->u.num].type == C_FUNC ) {
		func = data->u.num;
		st->funcname = reference_getname(data);
	} else {
		ShowError("script:run_func: not a buildin command.\n");
		script_reportdata(data);
		script_reportsrc(st);
		st->state = END;
		return 1;
	}

	if( script_config.warn_func_mismatch_argtypes )
		script_check_buildin_argtype(st, func);

	if(str_data[func].func) {
		if (str_data[func].func(st)) //Report error
			script_reportsrc(st);
	} else {
		ShowError("script:run_func: '%s' (id=%d type=%s) has no C function. please report this!!!\n", get_str(func), func, script_op2name(str_data[func].type));
		script_reportsrc(st);
		st->state = END;
	}

	// Stack's datum are used when re-running functions [Eoe]
	if( st->state == RERUNLINE )
		return 0;

	pop_stack(st, st->start, st->end);
	if( st->state == RETFUNC ) { // return from a user-defined function
		struct script_retinfo* ri;
		int olddefsp = st->stack->defsp;
		int nargs;

		pop_stack(st, st->stack->defsp, st->start);// pop distractions from the stack
		if( st->stack->defsp < 1 || st->stack->stack_data[st->stack->defsp-1].type != C_RETINFO ) {
			ShowWarning("script:run_func: return without callfunc or callsub!\n");
			script_reportsrc(st);
			st->state = END;
			return 1;
		}
		script_free_vars(st->stack->var_function);

		ri = st->stack->stack_data[st->stack->defsp - 1].u.ri;
		nargs = ri->nargs;
		st->pos = ri->pos;
		st->script = ri->script;
		st->stack->var_function = ri->var_function;
		st->stack->defsp = ri->defsp;
		memset(ri, 0, sizeof(struct script_retinfo));

		pop_stack(st, olddefsp-nargs - 1, olddefsp);// pop arguments and retinfo

		st->state = GOTO;
	}

	return 0;
}

/*==========================================
 * script execution
 *------------------------------------------*/
void run_script(struct script_code *rootscript,int pos,int rid,int oid)
{
	struct script_state *st;

	if( rootscript == NULL || pos < 0 )
		return;

	// @TODO: In jAthena, this function can take over the pending script in the player. [FlavioJS]
	//      It is unclear how that can be triggered, so it needs the be traced/checked in more detail.
	// NOTE At the time of this change, this function wasn't capable of taking over the script state because st->scriptroot was never set.
	st = script_alloc_state(rootscript, pos, rid, oid);
	run_script_main(st);
}

void script_stop_sleeptimers(int id)
{
	for( ;; ) {
		struct script_state* st = (struct script_state*)linkdb_erase(&sleep_db,(void*)__64BPRTSIZE(id));

		if( st == NULL )
			break; // no more sleep timers
		script_free_state(st);
	}
}

/*==========================================
 * Delete the specified node from sleep_db
 *------------------------------------------*/
struct linkdb_node* script_erase_sleepdb(struct linkdb_node *n)
{
	struct linkdb_node *retnode;

	if( n == NULL)
		return NULL;
	if( n->prev == NULL )
		sleep_db = n->next;
	else
		n->prev->next = n->next;
	if( n->next )
		n->next->prev = n->prev;
	retnode = n->next;
	aFree( n );
	return retnode;		// The following; return retnode
}

/*==========================================
 * Timer function for sleep
 *------------------------------------------*/
int run_script_timer(int tid, unsigned int tick, int id, intptr_t data)
{
	struct script_state *st     = (struct script_state *)data;
	struct linkdb_node *node    = (struct linkdb_node *)sleep_db;
	TBL_PC *sd = map_id2sd(st->rid);

	if((sd && sd->status.char_id != id) || (st->rid && !sd)) { //Character mismatch. Cancel execution.
		st->rid = 0;
		st->state = END;
	}
	while( node && st->sleep.timer != INVALID_TIMER ) {
		if( (int)__64BPRTSIZE(node->key) == st->oid && ((struct script_state *)node->data)->sleep.timer == st->sleep.timer ) {
			script_erase_sleepdb(node);
			st->sleep.timer = INVALID_TIMER;
			break;
		}
		node = node->next;
	}
	if(st->state != RERUNLINE)
		st->sleep.tick = 0;
	run_script_main(st);
	return 0;
}

/// Detaches script state from possibly attached character and restores it's previous script if any.
///
/// @param st Script state to detach.
/// @param dequeue_event Whether to schedule any queued events, when there was no previous script.
static void script_detach_state(struct script_state* st, bool dequeue_event)
{
	struct map_session_data* sd;

	if(st->rid && (sd = map_id2sd(st->rid))!=NULL) {
		sd->st = st->bk_st;
		sd->npc_id = st->bk_npcid;
		sd->state.disable_atcommand_on_npc = 0;
		if(st->bk_st) {
			//Remove tag for removal.
			st->bk_st = NULL;
			st->bk_npcid = 0;
		} else if(dequeue_event) {
#ifdef SECURE_NPCTIMEOUT
			/**
			 * We're done with this NPC session, so we cancel the timer (if existent) and move on
			 */
			if( sd->npc_idle_timer != INVALID_TIMER ) {
				delete_timer(sd->npc_idle_timer,npc_rr_secure_timeout_timer);
				sd->npc_idle_timer = INVALID_TIMER;
			}
#endif
			npc_event_dequeue(sd);
		}
	} else if(st->bk_st) { // rid was set to 0, before detaching the script state
		ShowError("script_detach_state: Found previous script state without attached player (rid=%d, oid=%d, state=%d, bk_npcid=%d)\n", st->bk_st->rid, st->bk_st->oid, st->bk_st->state, st->bk_npcid);
		script_reportsrc(st->bk_st);

		script_free_state(st->bk_st);
		st->bk_st = NULL;
	}
}

/// Attaches script state to possibly attached character and backups it's previous script, if any.
///
/// @param st Script state to attach.
static void script_attach_state(struct script_state* st)
{
	struct map_session_data* sd;

	if(st->rid && (sd = map_id2sd(st->rid)) != NULL) {
		if(st!=sd->st) {
			if(st->bk_st) { // There is already a backup
				ShowDebug("script_free_state: Previous script state lost (rid=%d, oid=%d, state=%d, bk_npcid=%d).\n", st->bk_st->rid, st->bk_st->oid, st->bk_st->state, st->bk_npcid);
			}
			st->bk_st = sd->st;
			st->bk_npcid = sd->npc_id;
		}
		sd->st = st;
		sd->npc_id = st->oid;
		sd->npc_item_flag = st->npc_item_flag; // Load default.
		sd->state.disable_atcommand_on_npc = (!pc_has_permission(sd, PC_PERM_ENABLE_COMMAND));
#ifdef SECURE_NPCTIMEOUT
		if( sd->npc_idle_timer == INVALID_TIMER )
			sd->npc_idle_timer = add_timer(gettick() + (SECURE_NPCTIMEOUT_INTERVAL*1000),npc_rr_secure_timeout_timer,sd->bl.id,0);
		sd->npc_idle_tick = gettick();
#endif
	}
}

/*==========================================
 * The main part of the script execution
 *------------------------------------------*/
void run_script_main(struct script_state *st)
{
	int cmdcount = script_config.check_cmdcount;
	int gotocount = script_config.check_gotocount;
	TBL_PC *sd;
	struct script_stack *stack = st->stack;

	script_attach_state(st);

	if (st->state == RERUNLINE) {
		run_func(st);
		if (st->state == GOTO)
			st->state = RUN;
	} else if (st->state != END)
		st->state = RUN;

	while (st->state == RUN) {
		enum c_op c = get_com(st->script->script_buf,&st->pos);

		switch (c) {
			case C_EOL:
				if (stack->defsp > stack->sp)
					ShowError("script:run_script_main: unexpected stack position (defsp=%d sp=%d). please report this!!!\n", stack->defsp, stack->sp);
				else
					pop_stack(st, stack->defsp, stack->sp); //Pop unused stack data. (unused return value)
				break;
			case C_INT:
				push_val(stack,C_INT,get_num(st->script->script_buf,&st->pos));
				break;
			case C_POS:
			case C_NAME:
				push_val(stack,c,GETVALUE(st->script->script_buf,st->pos));
				st->pos += 3;
				break;
			case C_ARG:
				push_val(stack,c,0);
				break;
			case C_STR:
				push_str(stack,C_CONSTSTR,(char*)(st->script->script_buf + st->pos));
				while (st->script->script_buf[st->pos++]);
				break;
			case C_FUNC:
				run_func(st);
				if (st->state == GOTO) {
					st->state = RUN;
					if (!st->freeloop && gotocount > 0 && (--gotocount) <= 0) {
						ShowError("run_script: infinity loop !\n");
						script_reportsrc(st);
						st->state = END;
					}
				}
				break;

			case C_REF:
				st->op2ref = 1;
				break;

			case C_NEG:
			case C_NOT:
			case C_LNOT:
				op_1(st,c);
				break;

			case C_ADD:
			case C_SUB:
			case C_MUL:
			case C_DIV:
			case C_MOD:
			case C_EQ:
			case C_NE:
			case C_GT:
			case C_GE:
			case C_LT:
			case C_LE:
			case C_AND:
			case C_OR:
			case C_XOR:
			case C_LAND:
			case C_LOR:
			case C_R_SHIFT:
			case C_L_SHIFT:
				op_2(st,c);
				break;

			case C_OP3:
				op_3(st,c);
				break;

			case C_NOP:
				st->state = END;
				break;

			default:
				ShowError("unknown command : %d @ %d\n",c,st->pos);
				st->state = END;
				break;
		}
		if (!st->freeloop && cmdcount > 0 && (--cmdcount) <= 0) {
			ShowError("run_script: too many opeartions being processed non-stop !\n");
			script_reportsrc(st);
			st->state = END;
		}
	}

	if (st->sleep.tick > 0) {
		//Restore previous script
		script_detach_state(st, false);
		//Delay execution
		sd = map_id2sd(st->rid); //Get sd since script might have attached someone while running. [Inkfish]
		st->sleep.charid = (sd ? sd->status.char_id : 0);
		st->sleep.timer = add_timer(gettick() + st->sleep.tick,
			run_script_timer,st->sleep.charid,(intptr_t)st);
		linkdb_insert(&sleep_db,(void*)__64BPRTSIZE(st->oid),st);
	} else if (st->state != END && st->rid) {
		//Resume later (st is already attached to player).
		if (st->bk_st) {
			ShowWarning("Unable to restore stack! Double continuation!\n");
			//Report BOTH scripts to see if that can help somehow.
			ShowDebug("Previous script (lost):\n");
			script_reportsrc(st->bk_st);
			ShowDebug("Current script:\n");
			script_reportsrc(st);

			script_free_state(st->bk_st);
			st->bk_st = NULL;
		}
	} else {
		//Dispose of script.
		if ((sd = map_id2sd(st->rid)) != NULL) { //Restore previous stack and save char.
			if(sd->state.using_fake_npc) {
				clif_clearunit_single(sd->npc_id, CLR_OUTSIGHT, sd->fd);
				sd->state.using_fake_npc = 0;
			}
			//Restore previous script if any.
			script_detach_state(st, true);
			if (sd->state.reg_dirty&2)
				intif_saveregistry(sd,2);
			if (sd->state.reg_dirty&1)
				intif_saveregistry(sd,1);
		}
		script_free_state(st);
	}
}

int script_config_read(char *cfgName)
{
	char line[1024],w1[1024],w2[1024];
	FILE *fp = fopen(cfgName,"r");

	if (fp == NULL) {
		ShowError("File not found: %s\n",cfgName);
		return 1;
	}
	while (fgets(line,sizeof(line),fp)) {
		int i;

		if (line[0] == '/' && line[1] == '/')
			continue;
		i = sscanf(line,"%1023[^:]: %1023[^\r\n]",w1,w2);
		if (i != 2)
			continue;

		if (strcmpi(w1,"warn_func_mismatch_paramnum") == 0)
			script_config.warn_func_mismatch_paramnum = config_switch(w2);
		else if (strcmpi(w1,"check_cmdcount") == 0)
			script_config.check_cmdcount = config_switch(w2);
		else if (strcmpi(w1,"check_gotocount") == 0)
			script_config.check_gotocount = config_switch(w2);
		else if (strcmpi(w1,"input_min_value") == 0)
			script_config.input_min_value = config_switch(w2);
		else if (strcmpi(w1,"input_max_value") == 0)
			script_config.input_max_value = config_switch(w2);
		else if (strcmpi(w1,"warn_func_mismatch_argtypes") == 0)
			script_config.warn_func_mismatch_argtypes = config_switch(w2);
		else if (strcmpi(w1,"import") == 0)
			script_config_read(w2);
		else
			ShowWarning("Unknown setting '%s' in file %s\n",w1,cfgName);
	}
	fclose(fp);

	return 0;
}

/**
 * @see DBApply
 */
static int db_script_free_code_sub(DBKey key, DBData *data, va_list ap)
{
	struct script_code *code = db_data2ptr(data);
	if (code)
		script_free_code(code);
	return 0;
}

void script_run_autobonus(const char *autobonus, struct map_session_data *sd, unsigned int pos)
{
	struct script_code *script = (struct script_code *)strdb_get(autobonus_db, autobonus);

	if( script ) {
		int j;

		ARR_FIND(0, EQI_MAX, j, sd->equip_index[j] >= 0 && sd->status.inventory[sd->equip_index[j]].equip == pos);
		if( j < EQI_MAX ) { //Single item autobonus
			current_equip_item_index = sd->equip_index[j];
			current_equip_combo_pos = 0;
		} else { //Combo autobonus
			current_equip_item_index = -1;
			current_equip_combo_pos = pos;
		}
		run_script(script,0,sd->bl.id,0);
	}
}

void script_add_autobonus(const char *autobonus)
{
	if( strdb_get(autobonus_db, autobonus) == NULL ) {
		struct script_code *script = parse_script(autobonus, "autobonus", 0, 0);

		if( script )
			strdb_put(autobonus_db, autobonus, script);
	}
}


/// resets a temporary character array variable to given value
void script_cleararray_pc(struct map_session_data* sd, const char* varname, void* value)
{
	int key;
	uint8 idx;

	if( not_array_variable(varname[0]) || !not_server_variable(varname[0]) ) {
		ShowError("script_cleararray_pc: Variable '%s' has invalid scope (char_id=%d).\n", varname, sd->status.char_id);
		return;
	}

	key = add_str(varname);

	if( is_string_variable(varname) ) {
		for( idx = 0; idx < SCRIPT_MAX_ARRAYSIZE; idx++ ) {
			pc_setregstr(sd, reference_uid(key, idx), (const char*)value);
		}
	} else {
		for( idx = 0; idx < SCRIPT_MAX_ARRAYSIZE; idx++ ) {
			pc_setreg(sd, reference_uid(key, idx), (int)__64BPRTSIZE(value));
		}
	}
}


/// sets a temporary character array variable element idx to given value
/// @param refcache Pointer to an int variable, which keeps a copy of the reference to varname and must be initialized to 0. Can be NULL if only one element is set.
void script_setarray_pc(struct map_session_data* sd, const char* varname, uint8 idx, void* value, int* refcache)
{
	int key;

	if( not_array_variable(varname[0]) || !not_server_variable(varname[0]) ) {
		ShowError("script_setarray_pc: Variable '%s' has invalid scope (char_id=%d).\n", varname, sd->status.char_id);
		return;
	}

	if( idx >= SCRIPT_MAX_ARRAYSIZE ) {
		ShowError("script_setarray_pc: Variable '%s' has invalid index '%d' (char_id=%d).\n", varname, (int)idx, sd->status.char_id);
		return;
	}

	key = ( refcache && refcache[0] ) ? refcache[0] : add_str(varname);

	if( is_string_variable(varname) ) {
		pc_setregstr(sd, reference_uid(key, idx), (const char*)value);
	} else {
		pc_setreg(sd, reference_uid(key, idx), (int)__64BPRTSIZE(value));
	}

	if( refcache ) { // save to avoid repeated add_str calls
		refcache[0] = key;
	}
}
#ifdef BETA_THREAD_TEST
int buildin_query_sql_sub(struct script_state* st, Sql* handle);

/* used to receive items the queryThread has already processed */
int queryThread_timer(int tid, unsigned int tick, int id, intptr_t data) {
	int i, cursor = 0;
	bool allOk = true;
	
	EnterSpinLock(&queryThreadLock);
	
	for( i = 0; i < queryThreadData.count; i++ ) {
		struct queryThreadEntry *entry = queryThreadData.entry[i];
		
		if( !entry->ok ) {
			allOk = false;
			continue;
		}

		run_script_main(entry->st);
		
		entry->st = NULL;/* empty entries */
		aFree(entry);
		queryThreadData.entry[i] = NULL;
	}

	
	if( allOk ) {
		/* cancel the repeating timer -- it'll re-create itself when necessary, dont need to remain looping */
		delete_timer(queryThreadData.timer, queryThread_timer);
		queryThreadData.timer = INVALID_TIMER;
	}
	
	/* now lets clear the mess. */
	for( i = 0; i < queryThreadData.count; i++ ) {
		struct queryThreadEntry *entry = queryThreadData.entry[i];
		if( entry == NULL )
			continue;/* entry on hold */
		
		/* move */
		memmove(&queryThreadData.entry[cursor], &queryThreadData.entry[i], sizeof(struct queryThreadEntry*));
		
		cursor++;
	}
	
	queryThreadData.count = cursor;
	
	LeaveSpinLock(&queryThreadLock);
	
	return 0;
}

void queryThread_add(struct script_state *st, bool type) {
	int idx = 0;
	struct queryThreadEntry* entry = NULL;
	
	EnterSpinLock(&queryThreadLock);
	
	if( queryThreadData.count++ != 0 )
		RECREATE(queryThreadData.entry, struct queryThreadEntry* , queryThreadData.count);
	
	idx = queryThreadData.count-1;
	
	CREATE(queryThreadData.entry[idx],struct queryThreadEntry,1);
	
	entry = queryThreadData.entry[idx];
	
	entry->st = st;
	entry->ok = false;
	entry->type = type;
	if( queryThreadData.timer == INVALID_TIMER ) { /* start the receiver timer */
		queryThreadData.timer = add_timer_interval(gettick() + 100, queryThread_timer, 0, 0, 100);
	}
	
	LeaveSpinLock(&queryThreadLock);
	
	/* unlock the queryThread */
	racond_signal(queryThreadCond);
}
/* adds a new log to the queue */
void queryThread_log(char * entry, int length) {
	int idx = logThreadData.count;
	
	EnterSpinLock(&queryThreadLock);
	
	if( logThreadData.count++ != 0 )
		RECREATE(logThreadData.entry, char* , logThreadData.count);
	
	CREATE(logThreadData.entry[idx], char, length + 1 );
	safestrncpy(logThreadData.entry[idx], entry, length + 1 );
	
	LeaveSpinLock(&queryThreadLock);

	/* unlock the queryThread */
	racond_signal(queryThreadCond);
}

/* queryThread_main */
static void *queryThread_main(void *x) {
	Sql *queryThread_handle = Sql_Malloc();
	int i;
	
	if ( SQL_ERROR == Sql_Connect(queryThread_handle, map_server_id, map_server_pw, map_server_ip, map_server_port, map_server_db) )
		exit(EXIT_FAILURE);
	
	if( strlen(default_codepage) > 0 )
		if ( SQL_ERROR == Sql_SetEncoding(queryThread_handle, default_codepage) )
			Sql_ShowDebug(queryThread_handle);

	if( log_config.sql_logs ) {
		logmysql_handle = Sql_Malloc();
		
		if ( SQL_ERROR == Sql_Connect(logmysql_handle, log_db_id, log_db_pw, log_db_ip, log_db_port, log_db_db) )
			exit(EXIT_FAILURE);
		
		if( strlen(default_codepage) > 0 )
			if ( SQL_ERROR == Sql_SetEncoding(logmysql_handle, default_codepage) )
				Sql_ShowDebug(logmysql_handle);
	}
	
	while( 1 ) {
		
		if(queryThreadTerminate > 0)
			break;
				
		EnterSpinLock(&queryThreadLock);
		
		/* mess with queryThreadData within the lock */
		for( i = 0; i < queryThreadData.count; i++ ) {
			struct queryThreadEntry *entry = queryThreadData.entry[i];
			
			if( entry->ok )
				continue;
			else if ( !entry->st || !entry->st->stack ) {
				entry->ok = true;/* dispose */
				continue;
			}
						
			buildin_query_sql_sub(entry->st, entry->type ? logmysql_handle : queryThread_handle);
			
			entry->ok = true;/* we're done with this */
		}
		
		/* also check for any logs in need to be sent */
		if( log_config.sql_logs ) {
			for( i = 0; i < logThreadData.count; i++ ) {
				if( SQL_ERROR == Sql_Query(logmysql_handle, logThreadData.entry[i]) )
					Sql_ShowDebug(logmysql_handle);
				aFree(logThreadData.entry[i]);
			}
			logThreadData.count = 0;
		}
		
		LeaveSpinLock(&queryThreadLock);
		
		ramutex_lock( queryThreadMutex );
		racond_wait( queryThreadCond,	queryThreadMutex,  -1 );
		ramutex_unlock( queryThreadMutex );

	}
		
	Sql_Free(queryThread_handle);
	
	if( log_config.sql_logs ) {
		Sql_Free(logmysql_handle);
	}
	
	return NULL;
}
#endif
/*==========================================
 * Destructor
 *------------------------------------------*/
void do_final_script(void) {
	int i;

#ifdef DEBUG_HASH
	if(battle_config.etc_log) {
		FILE *fp = fopen("hash_dump.txt","wt");
		if(fp) {
			int count[SCRIPT_HASH_SIZE];
			int count2[SCRIPT_HASH_SIZE]; // Number of buckets with a certain number of items
			int n = 0;
			int min = INT_MAX, max = 0, zero = 0;
			double mean = 0.0f;
			double median = 0.0f;

			ShowNotice("Dumping script str hash information to hash_dump.txt\n");
			memset(count, 0, sizeof(count));
			fprintf(fp,"num : hash : data_name\n");
			fprintf(fp,"---------------------------------------------------------------\n");
			for(i = LABEL_START; i<str_num; i++) {
				unsigned int h = calc_hash(get_str(i));
				fprintf(fp,"%04d : %4u : %s\n",i,h, get_str(i));
				++count[h];
			}
			fprintf(fp,"--------------------\n\n");
			memset(count2, 0, sizeof(count2));
			for(i = 0; i < SCRIPT_HASH_SIZE; i++) {
				fprintf(fp,"  hash %3d = %d\n",i,count[i]);
				if(min > count[i])
					min = count[i]; // Minimun count of collision
				if(max < count[i])
					max = count[i]; // Maximun count of collision
				if(count[i] == 0)
					zero++;
				++count2[count[i]];
			}
			fprintf(fp,"\n--------------------\n  items : buckets\n--------------------\n");
			for(i = min; i <= max; ++i) {
				fprintf(fp,"  %5d : %7d\n",i,count2[i]);
				mean += 1.0f * i * count2[i] / SCRIPT_HASH_SIZE; // NOTE: this will always result in <nr labels>/<nr buckets>
			}
			for(i = min; i <= max; ++i) {
				n += count2[i];
				if(n * 2 >= SCRIPT_HASH_SIZE) {
					if(SCRIPT_HASH_SIZE%2 == 0 && SCRIPT_HASH_SIZE / 2 == n)
						median = (i + i + 1) / 2.0f;
					else
						median = i;
					break;
				}
			}
			fprintf(fp,"--------------------\n    min = %d, max = %d, zero = %d\n    mean = %lf, median = %lf\n",min,max,zero,mean,median);
			fclose(fp);
		}
	}
#endif

	mapreg_final();

	db_destroy(scriptlabel_db);
	userfunc_db->destroy(userfunc_db, db_script_free_code_sub);
	autobonus_db->destroy(autobonus_db, db_script_free_code_sub);
	if(sleep_db) {
		struct linkdb_node *n = (struct linkdb_node *)sleep_db;

		while(n) {
			struct script_state *st = (struct script_state *)n->data;

			script_free_state(st);
			n = n->next;
		}
		linkdb_final(&sleep_db);
	}

	if(str_data)
		aFree(str_data);
	if(str_buf)
		aFree(str_buf);

	for(i = 0; i < atcmd_binding_count; i++)
		aFree(atcmd_binding[i]);

	if(atcmd_binding_count != 0)
		aFree(atcmd_binding);
#ifdef BETA_THREAD_TEST
	/* QueryThread */
	InterlockedIncrement(&queryThreadTerminate);
	racond_signal(queryThreadCond);
	rathread_wait(queryThread, NULL);

	// Destroy cond var and mutex.
	racond_destroy( queryThreadCond );
	ramutex_destroy( queryThreadMutex );

	/* Clear missing vars */
	for(i = 0; i < queryThreadData.count; i++)
		aFree(queryThreadData.entry[i]);

	aFree(queryThreadData.entry);

	for(i = 0; i < logThreadData.count; i++)
		aFree(logThreadData.entry[i]);

	aFree(logThreadData.entry);
#endif
}
/*==========================================
 * Initialization
 *------------------------------------------*/
void do_init_script(void) {
	userfunc_db = strdb_alloc(DB_OPT_DUP_KEY,0);
	scriptlabel_db = strdb_alloc(DB_OPT_DUP_KEY,50);
	autobonus_db = strdb_alloc(DB_OPT_DUP_KEY,0);

	mapreg_init();
#ifdef BETA_THREAD_TEST
	CREATE(queryThreadData.entry, struct queryThreadEntry*, 1);
	queryThreadData.count = 0;
	CREATE(logThreadData.entry, char *, 1);
	logThreadData.count = 0;
	/* QueryThread Start */
	
	InitializeSpinLock(&queryThreadLock);
	
	queryThreadData.timer = INVALID_TIMER;
	queryThreadTerminate = 0;
	queryThreadMutex = ramutex_create();
	queryThreadCond = racond_create();
	
	queryThread = rathread_create(queryThread_main, NULL);
	
	if(queryThread == NULL) {
		ShowFatalError("do_init_script: cannot spawn Query Thread.\n");
		exit(EXIT_FAILURE);
	}

	add_timer_func_list(queryThread_timer, "queryThread_timer");
#endif
}

void script_reload(void) {
	int i;

#ifdef BETA_THREAD_TEST
	/* We're reloading so any queries undergoing should be...exterminated. */
	EnterSpinLock(&queryThreadLock);
	
	for( i = 0; i < queryThreadData.count; i++ ) {
		aFree(queryThreadData.entry[i]);
	}
	queryThreadData.count = 0;
	
	if( queryThreadData.timer != INVALID_TIMER ) {
		delete_timer(queryThreadData.timer, queryThread_timer);
		queryThreadData.timer = INVALID_TIMER;
	}
	
	LeaveSpinLock(&queryThreadLock);
#endif

	
	userfunc_db->clear(userfunc_db, db_script_free_code_sub);
	db_clear(scriptlabel_db);

	// @commands (script based)
	// Clear bindings
	for( i = 0; i < atcmd_binding_count; i++ ) {
		aFree(atcmd_binding[i]);
	}
	
	if( atcmd_binding_count != 0 )
		aFree(atcmd_binding);
	
	atcmd_binding_count = 0;

	if(sleep_db) {
		struct linkdb_node *n = (struct linkdb_node *)sleep_db;

		while(n) {
			struct script_state *st = (struct script_state *)n->data;

			script_free_state(st);
			n = n->next;
		}
		linkdb_final(&sleep_db);
	}
	mapreg_reload();
}

//-----------------------------------------------------------------------------
// Buildin functions
//

#define BUILDIN_DEF(x,args) { buildin_ ## x , #x , args }
#define BUILDIN_DEF2(x,x2,args) { buildin_ ## x , x2 , args }
#define BUILDIN_FUNC(x) int buildin_ ## x (struct script_state* st)

/////////////////////////////////////////////////////////////////////
// NPC interaction
//

/// Appends a message to the npc dialog.
/// If a dialog doesn't exist yet, one is created.
///
/// mes "<message>";
BUILDIN_FUNC(mes)
{
	TBL_PC* sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	if( !script_hasdata(st, 3) ) { // only a single line detected in the script
		clif_scriptmes(sd, st->oid, script_getstr(st, 2));
	} else { // parse multiple lines as they exist
		int i;

		for( i = 2; script_hasdata(st, i); i++ ) {
			// send the message to the client
			clif_scriptmes(sd, st->oid, script_getstr(st, i));
		}
	}

	st->mes_active = 1; // Invoking character has a NPC dialog box open.
	return SCRIPT_CMD_SUCCESS;
}

/// Displays the button 'next' in the npc dialog.
/// The dialog text is cleared and the script continues when the button is pressed.
///
/// next;
BUILDIN_FUNC(next)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;
#ifdef SECURE_NPCTIMEOUT
	sd->npc_idle_type = NPCT_WAIT;
#endif
	st->state = STOP;
	clif_scriptnext(sd, st->oid);
	return SCRIPT_CMD_SUCCESS;
}

/// Ends the script and displays the button 'close' on the npc dialog.
/// The dialog is closed when the button is pressed.
///
/// close;
BUILDIN_FUNC(close)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	if( !st->mes_active ) {
		TBL_NPC* nd = map_id2nd(st->oid);
		st->state = END; // Keep backwards compatibility.
		ShowWarning("Incorrect use of 'close' command! (source:%s / path:%s)\n",nd?nd->name:"Unknown",nd?nd->path:"Unknown");
	} else {
		st->state = CLOSE;
		st->mes_active = 0;
	}

	clif_scriptclose(sd, st->oid);
	return SCRIPT_CMD_SUCCESS;
}

/// Displays the button 'close' on the npc dialog.
/// The dialog is closed and the script continues when the button is pressed.
///
/// close2;
BUILDIN_FUNC(close2)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	st->state = STOP;

	if( st->mes_active )
		st->mes_active = 0;

	clif_scriptclose(sd, st->oid);
	return SCRIPT_CMD_SUCCESS;
}

/// Counts the number of valid and total number of options in 'str'
/// If max_count > 0 the counting stops when that valid option is reached
/// total is incremented for each option (NULL is supported)
static int menu_countoptions(const char* str, int max_count, int* total)
{
	int count = 0;
	int bogus_total;

	if( total == NULL )
		total = &bogus_total;
	++(*total);

	// initial empty options
	while( *str == ':' )
	{
		++str;
		++(*total);
	}
	// count menu options
	while( *str != '\0' )
	{
		++count;
		--max_count;
		if( max_count == 0 )
			break;
		while( *str != ':' && *str != '\0' )
			++str;
		while( *str == ':' )
		{
			++str;
			++(*total);
		}
	}
	return count;
}

/// Displays a menu with options and goes to the target label.
/// The script is stopped if cancel is pressed.
/// Options with no text are not displayed in the client.
///
/// Options can be grouped together, separated by the character ':' in the text:
///   ex: menu "A:B:C",L_target;
/// All these options go to the specified target label.
///
/// The index of the selected option is put in the variable @menu.
/// Indexes start with 1 and are consistent with grouped and empty options.
///   ex: menu "A::B",-,"",L_Impossible,"C",-;
///       // displays "A", "B" and "C", corresponding to indexes 1, 3 and 5
///
/// NOTE: the client closes the npc dialog when cancel is pressed
///
/// menu "<option_text>",<target_label>{,"<option_text>",<target_label>,...};
BUILDIN_FUNC(menu)
{
	int i;
	const char* text;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

#ifdef SECURE_NPCTIMEOUT
	sd->npc_idle_type = NPCT_MENU;
#endif

	// @TODO: Detect multiple scripts waiting for input at the same time, and what to do when that happens
	if( sd->state.menu_or_input == 0 ) {
		struct StringBuf buf;

		if( script_lastdata(st) % 2 == 0 ) { // Argument count is not even (1st argument is at index 2)
			ShowError("script:menu: illegal number of arguments (%d).\n", (script_lastdata(st) - 1));
			st->state = END;
			return SCRIPT_CMD_FAILURE;
		}

		StringBuf_Init(&buf);
		sd->npc_menu = 0;
		for( i = 2; i < script_lastdata(st); i += 2 ) {
			struct script_data* data = script_getdata(st, i + 1); // Target label

			// Menu options
			text = script_getstr(st, i);

			if( !data_islabel(data) ) { // Not a label
				StringBuf_Destroy(&buf);
				ShowError("script:menu: argument #%d (from 1) is not a label or label not found.\n", i);
				script_reportdata(data);
				st->state = END;
				return SCRIPT_CMD_FAILURE;
			}

			// Append option(s)
			if( text[0] == '\0' )
				continue; // Empty string, ignore
			if( sd->npc_menu > 0 )
				StringBuf_AppendStr(&buf, ":");
			StringBuf_AppendStr(&buf, text);
			sd->npc_menu += menu_countoptions(text, 0, NULL);
		}
		st->state = RERUNLINE;
		sd->state.menu_or_input = 1;

		/**
		 * Menus beyond this length crash the client (see bugreport:6402)
		 */
		if( StringBuf_Length(&buf) >= 2047 ) {
			struct npc_data * nd = map_id2nd(st->oid);
			char* menu;

			CREATE(menu, char, 2048);
			safestrncpy(menu, StringBuf_Value(&buf), 2047);
			ShowWarning("NPC Menu too long! (source:%s / length:%d)\n",nd?nd->name:"Unknown",StringBuf_Length(&buf));
			clif_scriptmenu(sd, st->oid, menu);
			aFree(menu);
		} else
			clif_scriptmenu(sd, st->oid, StringBuf_Value(&buf));
		
		StringBuf_Destroy(&buf);

		if( sd->npc_menu >= 0xff ) {
			// Client supports only up to 254 entries; 0 is not used and 255 is reserved for cancel; excess entries are displayed but cause 'uint8' overflow
			ShowWarning("buildin_menu: Too many options specified (current=%d, max=254).\n", sd->npc_menu);
			script_reportsrc(st);
		}
	} else if( sd->npc_menu == 0xff ) { // Cancel was pressed
		sd->state.menu_or_input = 0;
		st->state = END;
	} else { // Goto target label
		int menu = 0;

		sd->state.menu_or_input = 0;
		if( sd->npc_menu <= 0 ) {
			ShowDebug("script:menu: unexpected selection (%d)\n", sd->npc_menu);
			st->state = END;
			return SCRIPT_CMD_FAILURE;
		}

		// Get target label
		for( i = 2; i < script_lastdata(st); i += 2 ) {
			text = script_getstr(st, i);
			sd->npc_menu -= menu_countoptions(text, sd->npc_menu, &menu);
			if( sd->npc_menu <= 0 )
				break; // Entry found
		}
		if( sd->npc_menu > 0 )
		{ // Invalid selection
			ShowDebug("script:menu: selection is out of range (%d pairs are missing?) - please report this\n", sd->npc_menu);
			st->state = END;
			return SCRIPT_CMD_FAILURE;
		}
		if( !data_islabel(script_getdata(st, i + 1)) )
		{ // @TODO: Remove this temporary crash-prevention code (fallback for multiple scripts requesting user input)
			ShowError("script:menu: unexpected data in label argument\n");
			script_reportdata(script_getdata(st, i + 1));
			st->state = END;
			return SCRIPT_CMD_FAILURE;
		}
		pc_setreg(sd, add_str("@menu"), menu);
		st->pos = script_getnum(st, i + 1);
		st->state = GOTO;
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Displays a menu with options and returns the selected option.
/// Behaves like 'menu' without the target labels.
///
/// select(<option_text>{,<option_text>,...}) -> <selected_option>
///
/// @see menu
BUILDIN_FUNC(select)
{
	int i;
	const char* text;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

#ifdef SECURE_NPCTIMEOUT
	sd->npc_idle_type = NPCT_MENU;
#endif

	if( sd->state.menu_or_input == 0 ) {
		struct StringBuf buf;

		StringBuf_Init(&buf);
		sd->npc_menu = 0;
		for( i = 2; i <= script_lastdata(st); ++i ) {
			text = script_getstr(st, i);
			
			if( sd->npc_menu > 0 )
				StringBuf_AppendStr(&buf, ":");

			StringBuf_AppendStr(&buf, text);
			sd->npc_menu += menu_countoptions(text, 0, NULL);
		}

		st->state = RERUNLINE;
		sd->state.menu_or_input = 1;
		
		/**
		 * menus beyond this length crash the client (see bugreport:6402)
		 */
		if( StringBuf_Length(&buf) >= 2047 ) {
			struct npc_data * nd = map_id2nd(st->oid);
			char* menu;
			CREATE(menu, char, 2048);
			safestrncpy(menu, StringBuf_Value(&buf), 2047);
			ShowWarning("NPC Menu too long! (source:%s / length:%d)\n",nd?nd->name:"Unknown",StringBuf_Length(&buf));
			clif_scriptmenu(sd, st->oid, menu);
			aFree(menu);
		} else
			clif_scriptmenu(sd, st->oid, StringBuf_Value(&buf));
		StringBuf_Destroy(&buf);

		if( sd->npc_menu >= 0xff ) {
			ShowWarning("buildin_select: Too many options specified (current=%d, max=254).\n", sd->npc_menu);
			script_reportsrc(st);
		}
	} else if( sd->npc_menu == 0xff ) {// Cancel was pressed
		sd->state.menu_or_input = 0;
		st->state = END;
	} else {// return selected option
		int menu = 0;

		sd->state.menu_or_input = 0;
		for( i = 2; i <= script_lastdata(st); ++i ) {
			text = script_getstr(st, i);
			sd->npc_menu -= menu_countoptions(text, sd->npc_menu, &menu);
			if( sd->npc_menu <= 0 )
				break;// entry found
		}
		pc_setreg(sd, add_str("@menu"), menu);
		script_pushint(st, menu);
		st->state = RUN;
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Displays a menu with options and returns the selected option.
/// Behaves like 'menu' without the target labels, except when cancel is
/// pressed.
/// When cancel is pressed, the script continues and 255 is returned.
///
/// prompt(<option_text>{,<option_text>,...}) -> <selected_option>
///
/// @see menu
BUILDIN_FUNC(prompt)
{
	int i;
	const char* text;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

#ifdef SECURE_NPCTIMEOUT
	sd->npc_idle_type = NPCT_MENU;
#endif

	if( sd->state.menu_or_input == 0 ) {
		struct StringBuf buf;

		StringBuf_Init(&buf);
		sd->npc_menu = 0;
		for( i = 2; i <= script_lastdata(st); ++i ) {
			text = script_getstr(st, i);
			if( sd->npc_menu > 0 )
				StringBuf_AppendStr(&buf, ":");
			StringBuf_AppendStr(&buf, text);
			sd->npc_menu += menu_countoptions(text, 0, NULL);
		}

		st->state = RERUNLINE;
		sd->state.menu_or_input = 1;
		
		/**
		 * Menus beyond this length crash the client (see bugreport:6402)
		 */
		if( StringBuf_Length(&buf) >= 2047 ) {
			struct npc_data * nd = map_id2nd(st->oid);
			char* menu;

			CREATE(menu, char, 2048);
			safestrncpy(menu, StringBuf_Value(&buf), 2047);
			ShowWarning("NPC Menu too long! (source:%s / length:%d)\n",nd?nd->name:"Unknown",StringBuf_Length(&buf));
			clif_scriptmenu(sd, st->oid, menu);
			aFree(menu);
		} else
			clif_scriptmenu(sd, st->oid, StringBuf_Value(&buf));
		StringBuf_Destroy(&buf);

		if( sd->npc_menu >= 0xff ) {
			ShowWarning("buildin_prompt: Too many options specified (current=%d, max=254).\n", sd->npc_menu);
			script_reportsrc(st);
		}
	} else if( sd->npc_menu == 0xff ) { // Cancel was pressed
		sd->state.menu_or_input = 0;
		pc_setreg(sd, add_str("@menu"), 0xff);
		script_pushint(st, 0xff);
		st->state = RUN;
	} else { // Return selected option
		int menu = 0;

		sd->state.menu_or_input = 0;
		for( i = 2; i <= script_lastdata(st); ++i ) {
			text = script_getstr(st, i);
			sd->npc_menu -= menu_countoptions(text, sd->npc_menu, &menu);
			if( sd->npc_menu <= 0 )
				break; // Entry found
		}
		pc_setreg(sd, add_str("@menu"), menu);
		script_pushint(st, menu);
		st->state = RUN;
	}
	return SCRIPT_CMD_SUCCESS;
}

/////////////////////////////////////////////////////////////////////
// ...
//

/// Jumps to the target script label.
///
/// goto <label>;
BUILDIN_FUNC(goto)
{
	if( !data_islabel(script_getdata(st,2)) )
	{
		ShowError("script:goto: not a label\n");
		script_reportdata(script_getdata(st,2));
		st->state = END;
		return 1;
	}

	st->pos = script_getnum(st,2);
	st->state = GOTO;
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * user-defined function call
 *------------------------------------------*/
BUILDIN_FUNC(callfunc)
{
	int i, j;
	struct script_retinfo* ri;
	struct script_code* scr;
	const char* str = script_getstr(st,2);
	DBMap **ref = NULL;

	scr = (struct script_code*)strdb_get(userfunc_db, str);
	if( !scr ) {
		ShowError("script:callfunc: function not found! [%s]\n", str);
		st->state = END;
		return 1;
	}

	for( i = st->start+3, j = 0; i < st->end; i++, j++ ) {
		struct script_data* data = push_copy(st->stack,i);
		if( data_isreference(data) && !data->ref ) {
			const char* name = reference_getname(data);
			if( name[0] == '.' ) {
				if( !ref ) {
					ref = (struct DBMap**)aCalloc(sizeof(struct DBMap*), 1);
					ref[0] = (name[1] == '@' ? st->stack->var_function : st->script->script_vars);
				}
				data->ref = ref;
			}
		}
	}

	CREATE(ri, struct script_retinfo, 1);
	ri->script       = st->script;// script code
	ri->var_function = st->stack->var_function;// scope variables
	ri->pos          = st->pos;// script location
	ri->nargs        = j;// argument count
	ri->defsp        = st->stack->defsp;// default stack pointer
	push_retinfo(st->stack, ri, ref);

	st->pos = 0;
	st->script = scr;
	st->stack->defsp = st->stack->sp;
	st->state = GOTO;
	st->stack->var_function = idb_alloc(DB_OPT_RELEASE_DATA);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * subroutine call
 *------------------------------------------*/
BUILDIN_FUNC(callsub)
{
	int i,j;
	struct script_retinfo* ri;
	int pos = script_getnum(st,2);
	DBMap **ref = NULL;

	if( !data_islabel(script_getdata(st,2)) && !data_isfunclabel(script_getdata(st,2)) ) {
		ShowError("script:callsub: argument is not a label\n");
		script_reportdata(script_getdata(st,2));
		st->state = END;
		return 1;
	}

	for( i = st->start+3, j = 0; i < st->end; i++, j++ ) {
		struct script_data* data = push_copy(st->stack,i);
		if( data_isreference(data) && !data->ref ) {
			const char* name = reference_getname(data);
			if( name[0] == '.' && name[1] == '@' ) {
				if ( !ref ) {
					ref = (struct DBMap**)aCalloc(sizeof(struct DBMap*),1);
					ref[0] = st->stack->var_function;
				}
				data->ref = ref;
			}
		}
	}

	CREATE(ri, struct script_retinfo, 1);
	ri->script       = st->script; // Script code
	ri->var_function = st->stack->var_function; // Scope variables
	ri->pos          = st->pos; // Script location
	ri->nargs        = j; // Argument count
	ri->defsp        = st->stack->defsp; // Default stack pointer
	push_retinfo(st->stack,ri,ref);

	st->pos = pos;
	st->stack->defsp = st->stack->sp;
	st->state = GOTO;
	st->stack->var_function = idb_alloc(DB_OPT_RELEASE_DATA);
	return SCRIPT_CMD_SUCCESS;
}

/// Retrieves an argument provided to callfunc/callsub.
/// If the argument doesn't exist
///
/// getarg(<index>{,<default_value>}) -> <value>
BUILDIN_FUNC(getarg)
{
	struct script_retinfo* ri;
	int idx;

	if( st->stack->defsp < 1 || st->stack->stack_data[st->stack->defsp - 1].type != C_RETINFO ) {
		ShowError("script:getarg: no callfunc or callsub!\n");
		st->state = END;
		return 1;
	}
	ri = st->stack->stack_data[st->stack->defsp - 1].u.ri;

	idx = script_getnum(st,2);

	if( idx >= 0 && idx < ri->nargs )
		push_copy(st->stack, st->stack->defsp - 1 - ri->nargs + idx);
	else if( script_hasdata(st,3) )
		script_pushcopy(st,3);
	else {
		ShowError("script:getarg: index (idx=%d) out of range (nargs=%d) and no default value found\n",idx,ri->nargs);
		st->state = END;
		return 1;
	}

	return SCRIPT_CMD_SUCCESS;
}

/// Returns from the current function, optionaly returning a value from the functions.
/// Don't use outside script functions.
///
/// return;
/// return <value>;
BUILDIN_FUNC(return)
{
	if( script_hasdata(st,2) ) { // Return value
		struct script_data* data;
		script_pushcopy(st,2);
		data = script_getdatatop(st,-1);
		if( data_isreference(data) ) {
			const char* name = reference_getname(data);
			if( name[0] == '.' && name[1] == '@' ) { // Scope variable
				if( !data->ref || data->ref == (DBMap**)&st->stack->var_function )
					get_val(st,data); // Current scope, convert to value
			} else if( name[0] == '.' && !data->ref ) { // Script variable, link to current script
				data->ref = &st->script->script_vars;
			}
		}
	} else // No return value
		script_pushnil(st);

	st->state = RETFUNC;
	return SCRIPT_CMD_SUCCESS;
}

/// Returns a random number from 0 to <range>-1.
/// Or returns a random number from <min> to <max>.
/// If <min> is greater than <max>, their numbers are switched.
/// rand(<range>) -> <int>
/// rand(<min>,<max>) -> <int>
BUILDIN_FUNC(rand)
{
	int range;
	int min;

	if( script_hasdata(st,3) ) { // Min, max
		int max = script_getnum(st,3);

		min = script_getnum(st,2);

		if( max < min )
			swap(min, max);
		range = max - min + 1;
	} else { // Range
		min = 0;
		range = script_getnum(st,2);
	}
	if( range <= 1 )
		script_pushint(st, min);
	else
		script_pushint(st, rnd()%range + min);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Warp sd to str,x,y or Random or SavePoint/Save
 *------------------------------------------*/
BUILDIN_FUNC(warp)
{
	int ret;
	int x,y;
	const char* str;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	str = script_getstr(st,2);
	x = script_getnum(st,3);
	y = script_getnum(st,4);

	if( strcmp(str,"Random") == 0 )
		ret = pc_randomwarp(sd,CLR_TELEPORT);
	else if( strcmp(str,"SavePoint") == 0 || strcmp(str,"Save") == 0 )
		ret = pc_setpos(sd,sd->status.save_point.map,sd->status.save_point.x,sd->status.save_point.y,CLR_TELEPORT);
	else
		ret = pc_setpos(sd,mapindex_name2id(str),x,y,CLR_OUTSIGHT);

	if( ret ) {
		ShowError("buildin_warp: moving player '%s' to \"%s\",%d,%d failed.\n", sd->status.name, str, x, y);
		return 1;
	}

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Warp a specified area
 *------------------------------------------*/
static int buildin_areawarp_sub(struct block_list *bl,va_list ap)
{
	int x2,y2,x3,y3;
	unsigned int index;
	
	index = va_arg(ap,unsigned int);
	x2 = va_arg(ap,int);
	y2 = va_arg(ap,int);
	x3 = va_arg(ap,int);
	y3 = va_arg(ap,int);
	
	if(index == 0)
		pc_randomwarp((TBL_PC *)bl,CLR_TELEPORT);
	else if(x3 && y3) {
		int max, tx, ty, j = 0;
		
		// choose a suitable max number of attempts
		if( (max = (y3-y2+1)*(x3-x2+1)*3) > 1000 )
			max = 1000;
		
		// find a suitable map cell
		do {
			tx = rnd()%(x3-x2+1)+x2;
			ty = rnd()%(y3-y2+1)+y2;
			j++;
		} while( map_getcell(index,tx,ty,CELL_CHKNOPASS) && j < max );
		
		pc_setpos((TBL_PC *)bl,index,tx,ty,CLR_OUTSIGHT);
	}
	else
		pc_setpos((TBL_PC *)bl,index,x2,y2,CLR_OUTSIGHT);
	return 0;
}

BUILDIN_FUNC(areawarp)
{
	int16 m, x0,y0,x1,y1, x2,y2,x3=0,y3=0;
	unsigned int index;
	const char *str;
	const char *mapname;

	mapname = script_getstr(st,2);
	x0  = script_getnum(st,3);
	y0  = script_getnum(st,4);
	x1  = script_getnum(st,5);
	y1  = script_getnum(st,6);
	str = script_getstr(st,7);
	x2  = script_getnum(st,8);
	y2  = script_getnum(st,9);
	
	if( script_hasdata(st,10) && script_hasdata(st,11) ) { // Warp area to area
		if( (x3 = script_getnum(st,10)) < 0 || (y3 = script_getnum(st,11)) < 0 ){
			x3 = 0;
			y3 = 0;
		} else if( x3 && y3 ) {
			// normalize x3/y3 coordinates
			if( x3 < x2 ) swap(x3,x2);
			if( y3 < y2 ) swap(y3,y2);
		}
	}

	if( (m = map_mapname2mapid(mapname)) < 0 )
		return 0;

	if( strcmp(str,"Random") == 0 )
		index = 0;
	else if( !(index=mapindex_name2id(str)) )
		return 0;

	map_foreachinarea(buildin_areawarp_sub, m,x0,y0,x1,y1, BL_PC, index,x2,y2,x3,y3);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * areapercentheal <map>,<x1>,<y1>,<x2>,<y2>,<hp>,<sp>
 *------------------------------------------*/
static int buildin_areapercentheal_sub(struct block_list *bl,va_list ap)
{
	int hp, sp;
	hp = va_arg(ap, int);
	sp = va_arg(ap, int);
	pc_percentheal((TBL_PC *)bl,hp,sp);
	return 0;
}

BUILDIN_FUNC(areapercentheal)
{
	int hp,sp,m;
	const char *mapname;
	int x0,y0,x1,y1;

	mapname=script_getstr(st,2);
	x0=script_getnum(st,3);
	y0=script_getnum(st,4);
	x1=script_getnum(st,5);
	y1=script_getnum(st,6);
	hp=script_getnum(st,7);
	sp=script_getnum(st,8);

	if( (m=map_mapname2mapid(mapname))< 0)
		return 0;

	map_foreachinarea(buildin_areapercentheal_sub,m,x0,y0,x1,y1,BL_PC,hp,sp);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * warpchar [LuzZza]
 * Useful for warp one player from
 * another player npc-session.
 * Using: warpchar "mapname",x,y,Char_ID;
 *------------------------------------------*/
BUILDIN_FUNC(warpchar)
{
	int x,y,a;
	const char *str;
	TBL_PC *sd;
	
	str=script_getstr(st,2);
	x=script_getnum(st,3);
	y=script_getnum(st,4);
	a=script_getnum(st,5);

	sd = map_charid2sd(a);
	if( sd == NULL )
		return 0;

	if(strcmp(str, "Random") == 0)
		pc_randomwarp(sd, CLR_TELEPORT);
	else
	if(strcmp(str, "SavePoint") == 0)
		pc_setpos(sd, sd->status.save_point.map,sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
	else
		pc_setpos(sd, mapindex_name2id(str), x, y, CLR_TELEPORT);

	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * Warpparty - [Fredzilla] [Paradox924X]
 * Syntax: warpparty "to_mapname",x,y,Party_ID,{"from_mapname"};
 * If 'from_mapname' is specified, only the party members on that map will be warped
 *------------------------------------------*/
BUILDIN_FUNC(warpparty)
{
	TBL_PC *sd = NULL;
	TBL_PC *pl_sd;
	struct party_data* p;
	int type;
	int mapindex;
	int i;

	const char* str = script_getstr(st,2);
	int x = script_getnum(st,3);
	int y = script_getnum(st,4);
	int p_id = script_getnum(st,5);
	const char* str2 = NULL;
	if ( script_hasdata(st,6) )
		str2 = script_getstr(st,6);

	p = party_search(p_id);
	if(!p)
		return 0;
	
	type = ( strcmp(str,"Random")==0 ) ? 0
	     : ( strcmp(str,"SavePointAll")==0 ) ? 1
		 : ( strcmp(str,"SavePoint")==0 ) ? 2
		 : ( strcmp(str,"Leader")==0 ) ? 3
		 : 4;

	switch (type)
	{
	case 3:
		for(i = 0; i < MAX_PARTY && !p->party.member[i].leader; i++);
		if (i == MAX_PARTY || !p->data[i].sd) //Leader not found / not online
			return 0;
		pl_sd = p->data[i].sd;
		mapindex = pl_sd->mapindex;
		x = pl_sd->bl.x;
		y = pl_sd->bl.y;
		break;
	case 4:
		mapindex = mapindex_name2id(str);
		break;
	case 2:
		//"SavePoint" uses save point of the currently attached player
		if (( sd = script_rid2sd(st) ) == NULL )
			return 0;
	default:
		mapindex = 0;
		break;
	}

	for (i = 0; i < MAX_PARTY; i++)
	{
		if( !(pl_sd = p->data[i].sd) || pl_sd->status.party_id != p_id )
			continue;

		if( str2 && strcmp(str2, map[pl_sd->bl.m].name) != 0 )
			continue;

		if( pc_isdead(pl_sd) )
			continue;

		switch( type )
		{
		case 0: // Random
			if(!map[pl_sd->bl.m].flag.nowarp)
				pc_randomwarp(pl_sd,CLR_TELEPORT);
		break;
		case 1: // SavePointAll
			if(!map[pl_sd->bl.m].flag.noreturn)
				pc_setpos(pl_sd,pl_sd->status.save_point.map,pl_sd->status.save_point.x,pl_sd->status.save_point.y,CLR_TELEPORT);
		break;
		case 2: // SavePoint
			if(!map[pl_sd->bl.m].flag.noreturn)
				pc_setpos(pl_sd,sd->status.save_point.map,sd->status.save_point.x,sd->status.save_point.y,CLR_TELEPORT);
		break;
		case 3: // Leader
		case 4: // m,x,y
			if(!map[pl_sd->bl.m].flag.noreturn && !map[pl_sd->bl.m].flag.nowarp)
				pc_setpos(pl_sd,mapindex,x,y,CLR_TELEPORT);
		break;
		}
	}

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Warpguild - [Fredzilla]
 * Syntax: warpguild "mapname",x,y,Guild_ID;
 *------------------------------------------*/
BUILDIN_FUNC(warpguild)
{
	TBL_PC *sd = NULL;
	TBL_PC *pl_sd;
	struct guild* g;
	struct s_mapiterator* iter;
	int type;

	const char* str = script_getstr(st,2);
	int x           = script_getnum(st,3);
	int y           = script_getnum(st,4);
	int gid         = script_getnum(st,5);

	g = guild_search(gid);
	if( g == NULL )
		return 0;
	
	type = ( strcmp(str,"Random") == 0 ) ? 0
	     : ( strcmp(str,"SavePointAll") == 0 ) ? 1
		 : ( strcmp(str,"SavePoint") == 0 ) ? 2
		 : 3;

	// "SavePoint" uses save point of the currently attached player
	if( type == 2 && (sd = script_rid2sd(st)) == NULL )
		return 0;

	iter = mapit_getallusers();
	for( pl_sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); pl_sd = (TBL_PC*)mapit_next(iter) ) {
		if( pl_sd->status.guild_id != gid )
			continue;

		switch( type ) {
			case 0: // Random
				if(!map[pl_sd->bl.m].flag.nowarp)
					pc_randomwarp(pl_sd,CLR_TELEPORT);
				break;
			case 1: // SavePointAll
				if(!map[pl_sd->bl.m].flag.noreturn)
					pc_setpos(pl_sd,pl_sd->status.save_point.map,pl_sd->status.save_point.x,pl_sd->status.save_point.y,CLR_TELEPORT);
				break;
			case 2: // SavePoint
				if(!map[pl_sd->bl.m].flag.noreturn)
					pc_setpos(pl_sd,sd->status.save_point.map,sd->status.save_point.x,sd->status.save_point.y,CLR_TELEPORT);
				break;
			case 3: // m,x,y
				if(!map[pl_sd->bl.m].flag.noreturn && !map[pl_sd->bl.m].flag.nowarp)
					pc_setpos(pl_sd,mapindex_name2id(str),x,y,CLR_TELEPORT);
				break;
		}
	}
	mapit_free(iter);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Force Heal a player (hp and sp)
 *------------------------------------------*/
BUILDIN_FUNC(heal)
{
	TBL_PC *sd;
	int hp,sp;
	
	sd = script_rid2sd(st);
	if( !sd )
		return 0;

	hp = script_getnum(st,2);
	sp = script_getnum(st,3);
	status_heal(&sd->bl, hp, sp, 1);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Heal a player by item (get vit bonus etc)
 *------------------------------------------*/
BUILDIN_FUNC(itemheal)
{
	TBL_PC *sd;
	int hp,sp;

	hp = script_getnum(st,2);
	sp = script_getnum(st,3);

	if( potion_flag == 1 ) {
		potion_hp = hp;
		potion_sp = sp;
		return 0;
	}

	sd = script_rid2sd(st);
	if( !sd )
		return 0;

	pc_itemheal(sd,sd->itemid,hp,sp);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(percentheal)
{
	int hp,sp;
	TBL_PC* sd;

	hp = script_getnum(st,2);
	sp = script_getnum(st,3);

	if( potion_flag == 1 ) {
		potion_per_hp = hp;
		potion_per_sp = sp;
		return 0;
	}

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

#ifdef RENEWAL
	if( sd->sc.data[SC_EXTREMITYFIST2] )
		sp = 0;
#endif

	pc_percentheal(sd,hp,sp);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(jobchange)
{
	int job, upper = -1;

	job = script_getnum(st,2);
	if( script_hasdata(st,3) )
		upper = script_getnum(st,3);

	if( pcdb_checkid(job) ) {
		TBL_PC* sd = script_rid2sd(st);

		if( sd == NULL )
			return 0;

		pc_jobchange(sd, job, upper);
	}

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(jobname)
{
	int class_ = script_getnum(st,2);

	script_pushconststr(st, (char*)job_name(class_));
	return SCRIPT_CMD_SUCCESS;
}

/// Get input from the player.
/// For numeric inputs the value is capped to the range [min,max]. Returns 1 if
/// the value was higher than 'max', -1 if lower than 'min' and 0 otherwise.
/// For string inputs it returns 1 if the string was longer than 'max', -1 is
/// shorter than 'min' and 0 otherwise.
///
/// input(<var>{,<min>{,<max>}}) -> <int>
BUILDIN_FUNC(input)
{
	TBL_PC* sd;
	struct script_data* data;
	int uid;
	const char* name;
	int min;
	int max;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	data = script_getdata(st,2);
	if( !data_isreference(data) ) {
		ShowError("script:input: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return 1;
	}
	uid = reference_getuid(data);
	name = reference_getname(data);
	min = (script_hasdata(st,3) ? script_getnum(st,3) : script_config.input_min_value);
	max = (script_hasdata(st,4) ? script_getnum(st,4) : script_config.input_max_value);

#ifdef SECURE_NPCTIMEOUT
	sd->npc_idle_type = NPCT_WAIT;
#endif

	if( !sd->state.menu_or_input ) { // first invocation, display npc input box
		sd->state.menu_or_input = 1;
		st->state = RERUNLINE;
		if( is_string_variable(name) )
			clif_scriptinputstr(sd,st->oid);
		else
			clif_scriptinput(sd,st->oid);
	} else { // take received text/value and store it in the designated variable
		sd->state.menu_or_input = 0;
		if( is_string_variable(name) ) {
			int len = (int)strlen(sd->npc_str);
			set_reg(st, sd, uid, name, (void*)sd->npc_str, script_getref(st,2));
			script_pushint(st, (len > max ? 1 : len < min ? -1 : 0));
		} else {
			int amount = sd->npc_amount;
			set_reg(st, sd, uid, name, (void*)__64BPRTSIZE(cap_value(amount,min,max)), script_getref(st,2));
			script_pushint(st, (amount > max ? 1 : amount < min ? -1 : 0));
		}
		st->state = RUN;
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Sets the value of a variable.
/// The value is converted to the type of the variable.
///
/// set(<variable>,<value>) -> <variable>
BUILDIN_FUNC(set)
{
	TBL_PC* sd = NULL;
	struct script_data* data;
	//struct script_data* datavalue;
	int num;
	const char* name;
	char prefix;

	data = script_getdata(st,2);
	//datavalue = script_getdata(st,3);
	if( !data_isreference(data) ) {
		ShowError("script:set: not a variable\n");
		script_reportdata(script_getdata(st,2));
		st->state = END;
		return 1;
	}

	num = reference_getuid(data);
	name = reference_getname(data);
	prefix = *name;

	if( not_server_variable(prefix) ) {
		sd = script_rid2sd(st);
		if( sd == NULL ) {
			ShowError("script:set: no player attached for player variable '%s'\n", name);
			return 0;
		}
	}

#if 0
	if( data_isreference(datavalue) ) { // The value being referenced is a variable
		const char* namevalue = reference_getname(datavalue);

		// Array variable being copied into another array variable
		if( !not_array_variable(*namevalue) ) {
			// Player must be attached in order to copy a player variable
			if( sd == NULL && not_server_variable(*namevalue) && !(sd = script_rid2sd(st)) ) {
				ShowError("script:set: no player attached for player variable '%s'\n", namevalue);
				return 0;
			}

			if( is_string_variable(namevalue) != is_string_variable(name) ) { // Non-matching array value types
				ShowWarning("script:set: two array variables do not match in type.\n");
				return 0;
			}

			// Push the maximum number of array values to the stack
			push_val(st->stack, C_INT, SCRIPT_MAX_ARRAYSIZE);

			// Call the copy array method directly
			return buildin_copyarray(st);
		}
	}
#endif

	if( is_string_variable(name) )
		set_reg(st,sd,num,name,(void*)script_getstr(st,3),script_getref(st,2));
	else
		set_reg(st,sd,num,name,(void*)__64BPRTSIZE(script_getnum(st,3)),script_getref(st,2));

	// Return a copy of the variable reference
	script_pushcopy(st,2);

	return SCRIPT_CMD_SUCCESS;
}

/////////////////////////////////////////////////////////////////////
/// Array variables
///

/// Returns the size of the specified array
static int32 getarraysize(struct script_state* st, int32 id, int32 idx, int isstring, struct DBMap** ref)
{
	int32 ret = idx;

	if( isstring ) {
		for( ; idx < SCRIPT_MAX_ARRAYSIZE; ++idx ) {
			char* str = (char*)get_val2(st, reference_uid(id, idx), ref);

			if( str && *str )
				ret = idx + 1;
			script_removetop(st, -1, 0);
		}
	} else {
		for( ; idx < SCRIPT_MAX_ARRAYSIZE; ++idx ) {
			int32 num = (int32)__64BPRTSIZE(get_val2(st, reference_uid(id, idx), ref));

			if( num )
				ret = idx + 1;
			script_removetop(st, -1, 0);
		}
	}
	return ret;
}

/// Sets values of an array, from the starting index.
/// ex: setarray arr[1],1,2,3;
///
/// setarray <array variable>,<value1>{,<value2>...};
BUILDIN_FUNC(setarray)
{
	struct script_data* data;
	const char* name;
	int32 start;
	int32 end;
	int32 id;
	int32 i;
	TBL_PC* sd = NULL;

	data = script_getdata(st, 2);
	if( !data_isreference(data) ) {
		ShowError("script:setarray: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return 1; // Not a variable
	}

	id = reference_getid(data);
	start = reference_getindex(data);
	name = reference_getname(data);
	if( not_array_variable(*name) ) {
		ShowError("script:setarray: illegal scope\n");
		script_reportdata(data);
		st->state = END;
		return 1; // Not supported
	}

	if( not_server_variable(*name) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; // No player attached
	}

	end = start + script_lastdata(st) - 2;
	if( end > SCRIPT_MAX_ARRAYSIZE )
		end = SCRIPT_MAX_ARRAYSIZE;

	if( is_string_variable(name) ) { // String array
		for( i = 3; start < end; ++start, ++i )
			set_reg(st, sd, reference_uid(id, start), name, (void*)script_getstr(st,i), reference_getref(data));
	} else { // Int array
		for( i = 3; start < end; ++start, ++i )
			set_reg(st, sd, reference_uid(id, start), name, (void*)__64BPRTSIZE(script_getnum(st,i)), reference_getref(data));
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Sets count values of an array, from the starting index.
/// ex: cleararray arr[0],0,1;
///
/// cleararray <array variable>,<value>,<count>;
BUILDIN_FUNC(cleararray)
{
	struct script_data* data;
	const char* name;
	int32 start;
	int32 end;
	int32 id;
	void* v;
	TBL_PC* sd = NULL;

	data = script_getdata(st, 2);
	if( !data_isreference(data) ) {
		ShowError("script:cleararray: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return 1; // Not a variable
	}

	id = reference_getid(data);
	start = reference_getindex(data);
	name = reference_getname(data);
	if( not_array_variable(*name) ) {
		ShowError("script:cleararray: illegal scope\n");
		script_reportdata(data);
		st->state = END;
		return 1; // Not supported
	}

	if( not_server_variable(*name) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; // No player attached
	}

	if( is_string_variable(name) )
		v = (void*)script_getstr(st, 3);
	else
		v = (void*)__64BPRTSIZE(script_getnum(st, 3));

	end = start + script_getnum(st, 4);
	if( end > SCRIPT_MAX_ARRAYSIZE )
		end = SCRIPT_MAX_ARRAYSIZE;

	for( ; start < end; ++start )
		set_reg(st, sd, reference_uid(id, start), name, v, script_getref(st,2));
	return SCRIPT_CMD_SUCCESS;
}

/// Copies data from one array to another.
/// ex: copyarray arr[0],arr[2],2;
///
/// copyarray <destination array variable>,<source array variable>,<count>;
BUILDIN_FUNC(copyarray)
{
	struct script_data* data1;
	struct script_data* data2;
	const char* name1;
	const char* name2;
	int32 idx1;
	int32 idx2;
	int32 id1;
	int32 id2;
	void* v;
	int32 i;
	int32 count;
	TBL_PC* sd = NULL;

	data1 = script_getdata(st, 2);
	data2 = script_getdata(st, 3);
	if( !data_isreference(data1) || !data_isreference(data2) ) {
		ShowError("script:copyarray: not a variable\n");
		script_reportdata(data1);
		script_reportdata(data2);
		st->state = END;
		return 1; // Not a variable
	}

	id1 = reference_getid(data1);
	id2 = reference_getid(data2);
	idx1 = reference_getindex(data1);
	idx2 = reference_getindex(data2);
	name1 = reference_getname(data1);
	name2 = reference_getname(data2);
	if( not_array_variable(*name1) || not_array_variable(*name2) ) {
		ShowError("script:copyarray: illegal scope\n");
		script_reportdata(data1);
		script_reportdata(data2);
		st->state = END;
		return 1; // Not supported
	}

	if( is_string_variable(name1) != is_string_variable(name2) ) {
		ShowError("script:copyarray: type mismatch\n");
		script_reportdata(data1);
		script_reportdata(data2);
		st->state = END;
		return 1; // Data type mismatch
	}

	if( not_server_variable(*name1) || not_server_variable(*name2) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; // No player attached
	}

	count = script_getnum(st, 4);
	if( count > SCRIPT_MAX_ARRAYSIZE - idx1 )
		count = SCRIPT_MAX_ARRAYSIZE - idx1;
	if( count <= 0 || (id1 == id2 && idx1 == idx2) )
		return 0; // Nothing to copy

	if( id1 == id2 && idx1 > idx2 ) { // Destination might be overlapping the source - copy in reverse order
		for( i = count - 1; i >= 0; --i ) {
			v = get_val2(st, reference_uid(id2, idx2 + i), reference_getref(data2));
			set_reg(st, sd, reference_uid(id1, idx1 + i), name1, v, reference_getref(data1));
			script_removetop(st, -1, 0);
		}
	} else { // Normal copy
		for( i = 0; i < count; ++i ) {
			if( idx2 + i < SCRIPT_MAX_ARRAYSIZE ) {
				v = get_val2(st, reference_uid(id2, idx2 + i), reference_getref(data2));
				set_reg(st, sd, reference_uid(id1, idx1 + i), name1, v, reference_getref(data1));
				script_removetop(st, -1, 0);
			} else // Out of range - assume ""/0
				set_reg(st, sd, reference_uid(id1, idx1 + i), name1, (is_string_variable(name1) ? (void*)"" : (void*)0), reference_getref(data1));
		}
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Returns the size of the array.
/// Assumes that everything before the starting index exists.
/// ex: getarraysize(arr[3])
///
/// getarraysize(<array variable>) -> <int>
BUILDIN_FUNC(getarraysize)
{
	struct script_data* data;
	const char* name;

	data = script_getdata(st, 2);
	if( !data_isreference(data) ) {
		ShowError("script:getarraysize: not a variable\n");
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1; // Not a variable
	}

	name = reference_getname(data);
	if( not_array_variable(*name) ) {
		ShowError("script:getarraysize: illegal scope\n");
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1; // Not supported
	}

	script_pushint(st, getarraysize(st, reference_getid(data), reference_getindex(data), is_string_variable(name), reference_getref(data)));
	return 0;
}

/// Deletes count or all the elements in an array, from the starting index.
/// ex: deletearray arr[4],2;
///
/// deletearray <array variable>;
/// deletearray <array variable>,<count>;
BUILDIN_FUNC(deletearray)
{
	struct script_data* data;
	const char* name;
	int start;
	int end;
	int id;
	TBL_PC *sd = NULL;

	data = script_getdata(st, 2);
	if( !data_isreference(data) ) {
		ShowError("script:deletearray: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return 1; // Not a variable
	}

	id = reference_getid(data);
	start = reference_getindex(data);
	name = reference_getname(data);
	if( not_array_variable(*name) ) {
		ShowError("script:deletearray: illegal scope\n");
		script_reportdata(data);
		st->state = END;
		return 1; // Not supported
	}

	if( not_server_variable(*name) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; // No player attached
	}

	end = SCRIPT_MAX_ARRAYSIZE;

	if( start >= end )
		return 0; // Nothing to free

	if( script_hasdata(st,3) ) {
		int count = script_getnum(st, 3);

		if( count > end - start )
			count = end - start;
		if( count <= 0 )
			return 0;// nothing to free

		// Move rest of the elements backward
		for( ; start + count < end; ++start ) {
			void* v = get_val2(st, reference_uid(id, start + count), reference_getref(data));

			set_reg(st, sd, reference_uid(id, start), name, v, reference_getref(data));
			script_removetop(st, -1, 0);
		}
	}

	// Clear the rest of the array
	if( is_string_variable(name) ) {
		for( ; start < end; ++start )
			set_reg(st, sd, reference_uid(id, start), name, (void *)"", reference_getref(data));
	} else {
		for( ; start < end; ++start )
			set_reg(st, sd, reference_uid(id, start), name, (void*)0, reference_getref(data));
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Returns a reference to the target index of the array variable.
/// Equivalent to var[index].
///
/// getelementofarray(<array variable>,<index>) -> <variable reference>
BUILDIN_FUNC(getelementofarray)
{
	struct script_data* data;
	const char* name;
	int32 id;
	int i;

	data = script_getdata(st, 2);
	if( !data_isreference(data) ) {
		ShowError("script:getelementofarray: not a variable\n");
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1; // Not a variable
	}

	id = reference_getid(data);
	name = reference_getname(data);
	if( not_array_variable(*name) ) {
		ShowError("script:getelementofarray: illegal scope\n");
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1; // Not supported
	}

	i = script_getnum(st, 3);
	if( i < 0 || i >= SCRIPT_MAX_ARRAYSIZE ) {
		ShowWarning("script:getelementofarray: index out of range (%d)\n", i);
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1; // Uut of range
	}

	push_val2(st->stack, C_NAME, reference_uid(id, i), reference_getref(data));
	return SCRIPT_CMD_SUCCESS;
}

/////////////////////////////////////////////////////////////////////
/// ...
///

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(setlook)
{
	int type,val;
	TBL_PC* sd;

	type = script_getnum(st,2);
	val = script_getnum(st,3);

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	pc_changelook(sd,type,val);

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(changelook)
{ // As setlook but only client side
	int type,val;
	TBL_PC* sd;

	type = script_getnum(st,2);
	val = script_getnum(st,3);

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	clif_changelook(&sd->bl,type,val);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(cutin)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	clif_cutin(sd,script_getstr(st,2),script_getnum(st,3));
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(viewpoint)
{
	int type, x, y, id, color;
	TBL_PC* sd;

	type = script_getnum(st,2);
	x = script_getnum(st,3);
	y = script_getnum(st,4);
	id = script_getnum(st,5);
	color = script_getnum(st,6);
	
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	clif_viewpoint(sd,st->oid,type,x,y,id,color);

	return SCRIPT_CMD_SUCCESS;
}

/// Returns number of items in inventory/cart/storage
/// countitem <nameID>{,<accountID>});
/// countitem2 <nameID>,<Identified>,<Refine>,<Attribute>,<Card0>,<Card1>,<Card2>,<Card3>{,<accountID>}) [Lupus]
/// cartcountitem <nameID>{,<accountID>});
/// cartcountitem2 <nameID>,<Identified>,<Refine>,<Attribute>,<Card0>,<Card1>,<Card2>,<Card3>{,<accountID>})
/// storagecountitem <nameID>{,<accountID>});
/// storagecountitem2 <nameID>,<Identified>,<Refine>,<Attribute>,<Card0>,<Card1>,<Card2>,<Card3>{,<accountID>})
BUILDIN_FUNC(countitem)
{
	int i = 0, count = 0, aid = 3;
	struct item_data* id = NULL;
	struct script_data* data;
	char *command = (char *)script_getfuncname(st);
	uint8 loc = 0;
	uint16 size;
	struct item *items;
	TBL_PC *sd = NULL;

	if( command[strlen(command) - 1] == '2' ) {
		i = 1;
		aid = 10;
	}
	if( script_hasdata(st,aid) ) {
		if( !(sd = map_id2sd((aid = script_getnum(st,aid)))) ) {
			ShowError("buildin_%s: player not found (AID=%d).\n", command, aid);
			st->state = END;
			return 1;
		}
	} else {
		if( !(sd = script_rid2sd(st)) )
			return 0;
	}
	if( !strncmp(command, "cart", 4) ) {
		loc = 1;
		size = MAX_CART;
		items = sd->status.cart;
	} else if( !strncmp(command, "storage", 7) ) {
		loc = 2;
		size = MAX_STORAGE;
		items = sd->status.storage.items;
	} //@TODO: 3 - Guild Storage
	else {
		size = MAX_INVENTORY;
		items = sd->status.inventory;
	}

	if( loc == 1 && !pc_iscarton(sd) ) {
		ShowError("buildin_%s: Player doesn't have cart (CID:%d).\n", command, sd->status.char_id);
		script_pushint(st,-1);
		return 0;
	}

	data = script_getdata(st, 2);
	get_val(st, data); //Convert into value in case of a variable

	if( data_isstring(data) ) //Item name
		id = itemdb_searchname(conv_str(st, data));
	else //Item id
		id = itemdb_exists(conv_num(st, data));

	if( id == NULL ) {
		ShowError("buildin_%s: Invalid item '%s'.\n", command, script_getstr(st,2)); //Returns string, regardless of what it was
		script_pushint(st,0);
		return 1;
	}

	if( !i ) { //For count/cart/storagecountitem function
		unsigned short nameid = id->nameid;

		for( i = 0; i < size; i++ )
			if( &items[i] && items[i].nameid == nameid )
				count += items[i].amount;
	} else { //For count/cart/storagecountitem2 function
		unsigned short nameid;
		int iden, ref, attr, c1, c2, c3, c4;

		nameid = id->nameid;
		iden = script_getnum(st,3);
		ref  = script_getnum(st,4);
		attr = script_getnum(st,5);
		c1 = script_getnum(st,6);
		c2 = script_getnum(st,7);
		c3 = script_getnum(st,8);
		c4 = script_getnum(st,9);

		for( i = 0; i < size; i++ )
			if( &items[i] && items[i].nameid > 0 && items[i].nameid == nameid &&
				items[i].amount > 0 && items[i].identify == iden &&
				items[i].refine == ref && items[i].attribute == attr &&
				items[i].card[0] == c1 && items[i].card[1] == c2 &&
				items[i].card[2] == c3 && items[i].card[3] == c4 )
				count += items[i].amount;
	}

	script_pushint(st,count);
	return SCRIPT_CMD_SUCCESS;
}

int checkweight_sub(TBL_PC *sd, int nbargs, unsigned short *eitemid, int32 *eamount)
{
	struct item_data* id = NULL;
	unsigned short nameid;
	int amount;
	uint32 weight = 0;
	uint16 amount2 = 0, slots, i;

	slots = pc_inventoryblank(sd); //NB of empty slot

	for( i = 0; i < nbargs; i++ ) {
		if( !eitemid[i] )
			continue;
		id = itemdb_exists(eitemid[i]);
		if( id == NULL ) {
			ShowError("checkweight_sub: Invalid item '%hu'.\n", eitemid[i]);
			return 0;
		}
		nameid = id->nameid;

		amount = eamount[i];
		if( amount < 1 ) {
			ShowError("checkweight_sub: Invalid amount '%d'.\n", eamount[i]);
			return 0;
		}

		weight += (id->weight)*amount; // Total weight for all chk
		if( weight + sd->weight > sd->max_weight ) // Too heavy
			return 0;

		switch( pc_checkadditem(sd, nameid, amount) ) {
			case CHKADDITEM_EXIST: // Item is already in inventory, but there is still space for the requested amount
				break;
			case CHKADDITEM_NEW:
				if( itemdb_isstackable(nameid) )
					amount2++; // Stackable
				else
					amount2 += amount; // Non-stackable
				if( slots < amount2)
					return 0;
				break;
			case CHKADDITEM_OVERAMOUNT:
				return 0;
		}
	}
	return 1;
}

/*==========================================
 * Check if item with this amount can fit in inventory
 * Checking : weight, stack amount >32k, slots amount >(MAX_INVENTORY)
 * Return
 *	0 : fail
 *	1 : success (npc side only)
 *------------------------------------------*/
BUILDIN_FUNC(checkweight)
{
	struct map_session_data* sd;
	struct item_data* id = NULL;
	unsigned short nameid[SCRIPT_MAX_ARRAYSIZE];
	int32 amount[SCRIPT_MAX_ARRAYSIZE];
	uint16 nbargs, i, j = 0;

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	nbargs = script_lastdata(st) + 1;
	if( nbargs%2 ) {
		ShowError("buildin_checkweight: Invalid nb of args should be a multiple of 2.\n");
		script_pushint(st,0);
		return 1;
	}

	for( i = 2; i < nbargs; i = i + 2 ) {
		struct script_data* data = script_getdata(st,i);

		get_val(st,data);  // Convert into value in case of a variable
		if( data_isstring(data) ) // Item name
			id = itemdb_searchname(conv_str(st,data));
		else // Item id
			id = itemdb_exists(conv_num(st,data));
		if( id == NULL ) {
			ShowError("buildin_checkweight: Invalid item '%s'.\n",script_getstr(st,i));  // Returns string, regardless of what it was
			script_pushint(st,0);
			return 1;
		}
		nameid[j] = id->nameid;
		amount[j] = script_getnum(st,i + 1);
		j++;
	}

	script_pushint(st,checkweight_sub(sd,(nbargs - 2) / 2,nameid,amount));
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(checkweight2) {
	//Variable sub checkweight
	unsigned short nameid[SCRIPT_MAX_ARRAYSIZE];
	int32 amount[SCRIPT_MAX_ARRAYSIZE], i;

	//Variable for array parsing
	struct script_data* data_it;
	struct script_data* data_nb;
	const char* name_it;
	const char* name_nb;
	int32 id_it, id_nb;
	int32 idx_it, idx_nb;
	int nb_it, nb_nb; //Array size

	TBL_PC *sd = script_rid2sd(st);

	if( sd == NULL )
		return 1;

	data_it = script_getdata(st,2);
	data_nb = script_getdata(st,3);

	if( !data_isreference(data_it) || !data_isreference(data_nb)) {
		ShowError("buildin_checkweight2: parameter not a variable\n");
		script_pushint(st,0);
		return 1; //Not a variable
	}

	id_it = reference_getid(data_it);
	id_nb = reference_getid(data_nb);
	idx_it = reference_getindex(data_it);
	idx_nb = reference_getindex(data_nb);
	name_it = reference_getname(data_it);
	name_nb = reference_getname(data_nb);

	if( not_array_variable(*name_it) || not_array_variable(*name_nb)) {
		ShowError("buildin_checkweight2: illegal scope\n");
		script_pushint(st,0);
		return 1; //Not supported
	}
	if( is_string_variable(name_it) || is_string_variable(name_nb) ) {
		ShowError("buildin_checkweight2: illegal type, need int\n");
		script_pushint(st,0);
		return 1; //Not supported
	}
	nb_it = getarraysize(st,id_it,idx_it,0,reference_getref(data_it));
	nb_nb = getarraysize(st,id_nb,idx_nb,0,reference_getref(data_nb));
	if( nb_it != nb_nb ) {
		ShowError("buildin_checkweight2: Size mistmatch: nb_it=%d, nb_nb=%d\n",nb_it,nb_nb);
		script_pushint(st,0);
		return 1;
	}

	for( i = 0; i < nb_it; i++ ) {
		nameid[i] = (int32)__64BPRTSIZE(get_val2(st,reference_uid(id_it,idx_it+i),reference_getref(data_it)));
		script_removetop(st,-1,0);
		amount[i] = (int32)__64BPRTSIZE(get_val2(st,reference_uid(id_nb,idx_nb+i),reference_getref(data_nb)));
		script_removetop(st,-1,0);
	} //End loop DO NOT break it prematurly we need to depop all stack

	script_pushint(st,checkweight_sub(sd,nb_it,nameid,amount)); //Push result of sub to script
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * getitem <item id>,<amount>{,<account ID>};
 * getitem "<item name>",<amount>{,<account ID>};
 *
 * getitembound <item id>,<amount>,<type>{,<account ID>};
 * getitembound "<item id>",<amount>,<type>{,<account ID>};
 * Type:
 *	0 - No bound
 *	1 - Account Bound
 *	2 - Guild Bound
 *	3 - Party Bound
 *	4 - Character Bound
 *------------------------------------------*/
BUILDIN_FUNC(getitem)
{
	int amount, get_count, i;
	unsigned short nameid;
	struct item it;
	TBL_PC *sd;
	struct script_data *data;
	unsigned char flag = 0;
	const char* command = script_getfuncname(st);

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) { // "<Item name>"
		const char *name = conv_str(st,data);
		struct item_data *item_data = itemdb_searchname(name);

		if( item_data == NULL ) {
			ShowError("buildin_getitem: Nonexistant item %s requested.\n",name);
			return 1; //No item created.
		}
		nameid = item_data->nameid;
	} else if( data_isint(data) ) { //<Item id>
		nameid = conv_num(st,data);
		if( !itemdb_exists(nameid) ) {
			ShowError("buildin_getitem: Nonexistant item %d requested.\n",nameid);
			return 1; //No item created.
		}
	} else {
		ShowError("buildin_getitem: invalid data type for argument #1 (%d).",data->type);
		return 1;
	}

	//<Amount>
	if( (amount = script_getnum(st,3)) <= 0 )
		return 0; //Return if amount <= 0, skip the useles iteration

	memset(&it,0,sizeof(it));
	it.nameid = nameid;
	it.identify = 1;
	it.bound = BOUND_NONE;

	if( !strcmp(command,"getitembound") ) {
		char bound = script_getnum(st,4);

		if( bound < BOUND_NONE || bound >= BOUND_MAX ) {
			ShowError("script_getitembound: Not a correct bound type! Type=%d\n",bound);
			return 1;
		}
		if( script_hasdata(st,5) )
			sd = map_id2sd(script_getnum(st,5));
		else
			sd = script_rid2sd(st); //Attached player
		it.bound = bound;
	} else if( script_hasdata(st,4) )
		sd = map_id2sd(script_getnum(st,4)); //<Account ID>
	else
		sd = script_rid2sd(st); //Attached player

	if( sd == NULL ) //No target
		return 0;

	//Check if it's stackable.
	if( !itemdb_isstackable(nameid) )
		get_count = 1;
	else
		get_count = amount;

	for( i = 0; i < amount; i += get_count ) {
		//If not pet egg
		if( !pet_create_egg(sd,nameid) ) {
			if( (flag = pc_additem(sd,&it,get_count,LOG_TYPE_SCRIPT)) ) {
				clif_additem(sd,0,0,flag);
				if( pc_candrop(sd,&it) )
					map_addflooritem(&it,get_count,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
			}
		}
	}
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * getitem2 <item id>,<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>};
 * getitem2 "<item name>",<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>};
 *
 * getitembound2 <item id>,<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>,<bound type>{,<account ID>};
 * getitembound2 "<item name>",<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>,<bound type>{,<account ID>};
 * Type:
 *	0 - No bound
 *	1 - Account Bound
 *	2 - Guild Bound
 *	3 - Party Bound
 *	4 - Character Bound
 *------------------------------------------*/
BUILDIN_FUNC(getitem2)
{
	int amount;
	unsigned short nameid;
	int iden, ref, attr;
	unsigned short c1, c2, c3, c4;
	char bound = BOUND_NONE;
	struct item_data *item_data = NULL;
	struct item item_tmp;
	TBL_PC *sd;
	struct script_data *data;
	const char* command = script_getfuncname(st);

	if( !strcmp(command,"getitembound2") ) {
		bound = script_getnum(st,11);
		if( bound < BOUND_NONE || bound >= BOUND_MAX ) {
			ShowError("script_getitembound2: Not a correct bound type! Type=%d\n",bound);
			return 1;
		}
		if( script_hasdata(st,12) )
			sd = map_id2sd(script_getnum(st,12));
		else
			sd = script_rid2sd(st); //Attached player
	} else if( script_hasdata(st,11) )
		sd = map_id2sd(script_getnum(st,11)); //<Account ID>
	else
		sd = script_rid2sd(st); //Attached player

	if( sd == NULL ) //No target
		return 0;

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);

		if( (item_data = itemdb_searchname(name)) == NULL ) {
			ShowError("buildin_getitem2: Nonexistant item %s requested (by conv_str).\n", name);
			return 1; //No item created.
		}
		nameid = item_data->nameid;
	} else {
		nameid = conv_num(st,data);
		if( (item_data = itemdb_exists(nameid)) == NULL ) {
			ShowError("buildin_getitem2: Nonexistant item %d requested (by conv_num).\n", nameid);
			return 1; //No item created.
		}
	}

	amount = script_getnum(st,3);
	iden = script_getnum(st,4);
	ref = script_getnum(st,5);
	attr = script_getnum(st,6);
	c1 = (unsigned short)script_getnum(st,7);
	c2 = (unsigned short)script_getnum(st,8);
	c3 = (unsigned short)script_getnum(st,9);
	c4 = (unsigned short)script_getnum(st,10);

	if( item_data ) {
		int get_count = 0, i;

		memset(&item_tmp,0,sizeof(item_tmp));
		if( item_data->type == IT_WEAPON || item_data->type == IT_ARMOR || item_data->type == IT_SHADOWGEAR ) {
			if( ref > MAX_REFINE )
				ref = MAX_REFINE;
		} else if( item_data->type == IT_PETEGG ) {
			iden = 1;
			ref = 0;
		} else {
			iden = 1;
			ref = attr = 0;
		}

		item_tmp.nameid = nameid;
		item_tmp.identify = iden;
		item_tmp.refine = ref;
		item_tmp.attribute = attr;
		item_tmp.card[0] = c1;
		item_tmp.card[1] = c2;
		item_tmp.card[2] = c3;
		item_tmp.card[3] = c4;
		item_tmp.bound = bound;

		//Check if it's stackable.
		if( !itemdb_isstackable(nameid) )
			get_count = 1;
		else
			get_count = amount;

		for( i = 0; i < amount; i += get_count ) {
			//If not pet egg
			if( !pet_create_egg(sd,nameid) ) {
				unsigned char flag = 0;

				if( (flag = pc_additem(sd,&item_tmp,get_count,LOG_TYPE_SCRIPT)) ) {
					clif_additem(sd,0,0,flag);
					if( pc_candrop(sd,&item_tmp) )
						map_addflooritem(&item_tmp,get_count,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
				}
			}
		}
	}
	return SCRIPT_CMD_SUCCESS;
}

/** Gives rental item to player
 * rentitem <item id>,<seconds>
 * rentitem "<item name>",<seconds>
 */
BUILDIN_FUNC(rentitem)
{
	struct map_session_data *sd;
	struct script_data *data;
	struct item it;
	int seconds;
	unsigned short nameid = 0;
	unsigned char flag = 0;

	data = script_getdata(st,2);
	get_val(st,data);

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);
		struct item_data *itd = itemdb_searchname(name);

		if( itd == NULL ) {
			ShowError("buildin_rentitem: Nonexistant item %s requested.\n", name);
			return 1;
		}
		nameid = itd->nameid;
	} else if( data_isint(data) ) {
		nameid = conv_num(st,data);
		if( !nameid || !itemdb_exists(nameid) ) {
			ShowError("buildin_rentitem: Nonexistant item %hu requested.\n", nameid);
			return 1;
		}
	} else {
		ShowError("buildin_rentitem: invalid data type for argument #1 (%d).\n", data->type);
		return 1;
	}

	seconds = script_getnum(st,3);
	memset(&it, 0, sizeof(it));
	it.nameid = nameid;
	it.identify = 1;
	it.expire_time = (unsigned int)(time(NULL) + seconds);
	it.bound = BOUND_NONE;

	if( (flag = pc_additem(sd, &it, 1, LOG_TYPE_SCRIPT)) ) {
		clif_additem(sd, 0, 0, flag);
		return 1;
	}
	return SCRIPT_CMD_SUCCESS;
}

/** Gives rental item to player with advanced option
 * rentitem2 <item id>,<time>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>;
 * rentitem2 "<item name>",<time>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>;
 */
BUILDIN_FUNC(rentitem2) {
	struct map_session_data *sd;
	struct script_data *data;
	struct item it;
	struct item_data *id;
	int seconds;
	unsigned short nameid = 0;
	unsigned char flag = 0;
	int iden, ref, attr, c1, c2, c3, c4;

	data = script_getdata(st,2);
	get_val(st,data);

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);

		id = itemdb_searchname(name);
		if( id == NULL ) {
			ShowError("buildin_rentitem2: Nonexistant item %s requested.\n", name);
			return 1;
		}
		nameid = id->nameid;
	} else if( data_isint(data) ) {
		nameid = conv_num(st,data);
		if( !(id = itemdb_search(nameid)) ) {
			ShowError("buildin_rentitem2: Nonexistant item %hu requested.\n", nameid);
			return 1;
		}
	} else {
		ShowError("buildin_rentitem2: invalid data type for argument #1 (%d).\n", data->type);
		return 1;
	}

	seconds = script_getnum(st,3);
	iden = script_getnum(st,4);
	ref = script_getnum(st,5);
	attr = script_getnum(st,6);

	if( id->type == IT_WEAPON || id->type == IT_ARMOR || id->type == IT_SHADOWGEAR ) {
		if( ref > MAX_REFINE )
			ref = MAX_REFINE;
	} else if( id->type == IT_PETEGG ) {
		iden = 1;
		ref = 0;
	} else {
		iden = 1;
		ref = attr = 0;
	}

	c1 = (short)script_getnum(st,7);
	c2 = (short)script_getnum(st,8);
	c3 = (short)script_getnum(st,9);
	c4 = (short)script_getnum(st,10);

	memset(&it, 0, sizeof(it));
	it.nameid = nameid;
	it.identify = iden;
	it.refine = ref;
	it.attribute = attr;
	it.card[0] = (short)c1;
	it.card[1] = (short)c2;
	it.card[2] = (short)c3;
	it.card[3] = (short)c4;
	it.expire_time = (unsigned int)(time(NULL) + seconds);

	if( (flag = pc_additem(sd, &it, 1, LOG_TYPE_SCRIPT)) ) {
		clif_additem(sd, 0, 0, flag);
		return 1;
	}

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Gets an item with someone's name inscribed [Skotlex]
 * getinscribeditem item_num, character_name
 * Returned Qty is always 1, only works on equip-able
 * equipment
 *------------------------------------------*/
BUILDIN_FUNC(getnameditem)
{
	unsigned short nameid;
	struct item item_tmp;
	TBL_PC *sd, *tsd;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL ) //Player not attached!
		return 0;

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);
		struct item_data *item_data = itemdb_searchname(name);

		if( item_data == NULL) { //Failed
			script_pushint(st,0);
			return 0;
		}
		nameid = item_data->nameid;
	} else
		nameid = conv_num(st,data);

	if( !itemdb_exists(nameid) /*|| itemdb_isstackable(nameid)*/ ) {
		//Even though named stackable items "could" be risky, they are required for certain quests.
		script_pushint(st,0);
		return 0;
	}

	data = script_getdata(st,3);
	get_val(st,data);
	if( data_isstring(data) ) //Char Name
		tsd = map_nick2sd(conv_str(st,data));
	else //Char Id was given
		tsd = map_charid2sd(conv_num(st,data));
	
	if( tsd == NULL ) { //Failed
		script_pushint(st,0);
		return 0;
	}

	memset(&item_tmp,0,sizeof(item_tmp));
	item_tmp.nameid = nameid;
	item_tmp.amount = 1;
	item_tmp.identify = 1;
	item_tmp.card[0] = CARD0_CREATE; //We don't use 255! because for example SIGNED WEAPON shouldn't get TOP10 BS Fame bonus [Lupus]
	item_tmp.card[2] = tsd->status.char_id;
	item_tmp.card[3] = tsd->status.char_id>>16;
	if(pc_additem(sd,&item_tmp,1,LOG_TYPE_SCRIPT)) {
		script_pushint(st,0);
		return 0; //Failed to add item, we will not drop if they don't fit
	}

	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Gets a random item ID from an item group [Skotlex]
 * groupranditem group_num
 *------------------------------------------*/
BUILDIN_FUNC(grouprandomitem)
{
	script_pushint(st,itemdb_searchrandomid(script_getnum(st,2),script_getnum(st,3)));
	return SCRIPT_CMD_SUCCESS;
}

/**
 * makeitem <item id>,<amount>,"<map name>",<X>,<Y>;
 * makeitem "<item name>",<amount>,"<map name>",<X>,<Y>;
 */
BUILDIN_FUNC(makeitem)
{
	int16 nameid;
	uint16 amount, flag = 0, x, y;
	const char *mapname;
	int m;
	struct item item_tmp;
	struct script_data *data;

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);
		struct item_data *item_data = itemdb_searchname(name);

		if( item_data )
			nameid = item_data->nameid;
		else
			nameid = UNKNOWN_ITEM_ID;
	} else
		nameid = conv_num(st,data);

	amount = script_getnum(st,3);
	mapname = script_getstr(st,4);
	x = script_getnum(st,5);
	y = script_getnum(st,6);

	if( strcmp(mapname,"this") == 0 ) {
		TBL_PC *sd = script_rid2sd(st);

		if( !sd )
			return 0; //Failed
		m = sd->bl.m;
	} else
		m = map_mapname2mapid(mapname);

	if( nameid < 0 ) {
		nameid = -nameid;
		flag = 1;
	}

	if( nameid > 0 ) {
		memset(&item_tmp,0,sizeof(item_tmp));
		item_tmp.nameid = nameid;
		if( !flag )
			item_tmp.identify = 1;
		else
			item_tmp.identify = itemdb_isidentified(nameid);

		map_addflooritem(&item_tmp,amount,m,x,y,0,0,0,4);
	}
	return SCRIPT_CMD_SUCCESS;
}

/**
 * makeitem2 <item id>,<amount>,"<map name>",<X>,<Y>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>;
 * makeitem2 "<item name>",<amount>,"<map name>",<X>,<Y>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>;
 */
BUILDIN_FUNC(makeitem2) {
	uint16 nameid, amount, x, y;
	const char *mapname;
	int m;
	struct item item_tmp;
	struct script_data *data;
	struct item_data *id;

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);
		struct item_data *item_data = itemdb_searchname(name);

		if( item_data )
			nameid = item_data->nameid;
		else
			nameid = UNKNOWN_ITEM_ID;
	} else
		nameid = conv_num(st,data);

	amount = script_getnum(st,3);
	mapname	= script_getstr(st,4);
	x = script_getnum(st,5);
	y = script_getnum(st,6);

	if( strcmp(mapname,"this") == 0 ) {
		TBL_PC *sd;

		sd = script_rid2sd(st);
		if( !sd )
			return 0; //Failed
		m = sd->bl.m;
	} else
		m = map_mapname2mapid(mapname);

	if( (id = itemdb_search(nameid)) ) {
		char iden, ref, attr;

		memset(&item_tmp,0,sizeof(item_tmp));
		item_tmp.nameid = nameid;

		iden = (char)script_getnum(st,7);
		ref = (char)script_getnum(st,8);
		attr = (char)script_getnum(st,9);		

		if( id->type == IT_WEAPON || id->type == IT_ARMOR || id->type == IT_SHADOWGEAR ) {
			if( ref > MAX_REFINE )
				ref = MAX_REFINE;
		} else if( id->type == IT_PETEGG ) {
			iden = 1;
			ref = 0;
		} else {
			iden = 1;
			ref = attr = 0;
		}

		item_tmp.identify = iden;
		item_tmp.refine = ref;
		item_tmp.attribute = attr;
		item_tmp.card[0] = script_getnum(st,10);
		item_tmp.card[1] = script_getnum(st,11);
		item_tmp.card[2] = script_getnum(st,12);
		item_tmp.card[3] = script_getnum(st,13);

		map_addflooritem(&item_tmp,amount,m,x,y,0,0,0,4);
	} else
		return 1;
	return SCRIPT_CMD_SUCCESS;
}

/// Counts/deletes the current item given by idx.
/// Used by buildin_delitem_search
/// Relies on all input data being already fully valid.
static void buildin_delitem_delete(struct map_session_data* sd, int idx, int* amount, uint8 loc, bool delete_items)
{
	int delamount;
	struct item *itm = NULL;

	switch( loc ) {
		case 1: //Cart
			itm = &sd->status.cart[idx];
			break;
		case 2: //Storage
			itm = &sd->status.storage.items[idx];
			break;
		default: //Inventory
			itm = &sd->status.inventory[idx];
			break;
	}

	delamount = (amount[0] < itm->amount) ? amount[0] : itm->amount;

	if( delete_items ) {
		if( itemdb_type(itm->nameid) == IT_PETEGG && itm->card[0] == CARD0_PET && CheckForCharServer() )
			intif_delete_petdata(MakeDWord(itm->card[1],itm->card[2])); //Delete associated pet
		switch( loc ) {
			case 1:
				pc_cart_delitem(sd,idx,delamount,0,LOG_TYPE_SCRIPT);
				break;
			case 2:
				storage_delitem(sd,idx,delamount);
				log_pick_pc(sd,LOG_TYPE_SCRIPT,-delamount,itm);
				break;
			default:
				pc_delitem(sd,idx,delamount,0,0,LOG_TYPE_SCRIPT);
				break;
		}
	}

	amount[0] -= delamount;
}


/// Searches for item(s) and checks, if there is enough of them.
/// Used by delitem and delitem2
/// Relies on all input data being already fully valid.
/// @param exact_match will also match item attributes and cards, not just name id
/// @return true when all items could be deleted, false when there were not enough items to delete
static bool buildin_delitem_search(struct map_session_data* sd, struct item* it, bool exact_match, uint8 loc)
{
	bool delete_items = false;
	int i, amount, size;
	struct item *items;

	//Prefer always non-equipped items
	it->equip = 0;

	//When searching for nameid only, prefer additionally
	if( !exact_match ) {
		it->refine = 0; //Non-refined items
		memset(it->card, 0, sizeof(it->card)); //Card-less items
	}

	switch( loc ) {
		case 1:	//Cart
			size = MAX_CART;
			items = sd->status.cart;
			break;
		case 2:	//Storage
			size = MAX_STORAGE;
			items = sd->status.storage.items;
			break;
		default: //Inventory
			size = MAX_INVENTORY;
			items = sd->status.inventory;
			break;
	}

	for( ;; ) {
		unsigned short important = 0;

		amount = it->amount;

		//1st pass -- less important items / exact match
		for( i = 0; amount && i < size; i++ ) {
			struct item *itm = NULL;

			if( !&items[i] || !(itm = &items[i])->nameid || itm->nameid != it->nameid ) //Wrong/invalid item
				continue;

			if( itm->equip != it->equip || itm->refine != it->refine ) { //Not matching attributes
				important++;
				continue;
			}

			if( exact_match ) {
				if( itm->identify != it->identify || itm->attribute != it->attribute || memcmp(itm->card, it->card, sizeof(itm->card)) )
					continue; //Not matching exact attributes
			} else {
				if( itemdb_type(itm->nameid) == IT_PETEGG ) {
					if( itm->card[0] == CARD0_PET && CheckForCharServer() ) //Pet which cannot be deleted
						continue;
				} else if( memcmp(itm->card, it->card, sizeof(itm->card)) ) { //Named/carded item
					important++;
					continue;
				}
			}

			//Count/delete item
			buildin_delitem_delete(sd, i, &amount, loc, delete_items);
		}

		//2nd pass -- any matching item
		if( amount == 0 || important == 0 ) { //Either everything was already consumed or no items were skipped
			;
		} else
			for( i = 0; amount && i < size; i++ ) {
				struct item *itm = NULL;

				if( !&items[i] || !(itm = &items[i])->nameid || itm->nameid != it->nameid ) //Wrong/invalid item
					continue;

				//Pet which cannot be deleted
				if( itemdb_type(itm->nameid) == IT_PETEGG && itm->card[0] == CARD0_PET && CheckForCharServer() )
					continue;

				if( exact_match &&
					(itm->refine != it->refine || itm->identify != it->identify || itm->attribute != it->attribute ||
					memcmp(itm->card, it->card, sizeof(itm->card))) )
						continue; //Not matching attributes

				//Count/delete item
				buildin_delitem_delete(sd, i, &amount, loc, delete_items);
			}

		if( amount ) //Not enough items
			return false;
		else if( delete_items ) //We are done with the work
			return true;
		else //Get rid of the items now
			delete_items = true;
	}
}


/// Deletes items from the target/attached player.
/// Prioritizes ordinary items.
///
/// delitem <item id>,<amount>{,<account id>}
/// delitem "<item name>",<amount>{,<account id>}
/// cartdelitem <item id>,<amount>{,<account id>}
/// cartdelitem "<item name>",<amount>{,<account id>}
/// storagedelitem <item id>,<amount>{,<account id>}
/// storagedelitem "<item name>",<amount>{,<account id>}
BUILDIN_FUNC(delitem)
{
	TBL_PC *sd;
	struct item it;
	struct script_data *data;
	uint8 loc = 0;
	char* command = (char*)script_getfuncname(st);

	if( !strncmp(command, "cart", 4) )
		loc = 1;
	else if( !strncmp(command, "storage", 7) )
		loc = 2;
	//@TODO: 3 - Guild Storage

	if( script_hasdata(st,4) ) {
		int account_id = script_getnum(st,4);

		sd = map_id2sd(account_id); //<account id>
		if( sd == NULL ) {
			ShowError("buildin_%s: player not found (AID=%d).\n", command, account_id);
			st->state = END;
			return 1;
		}
	} else {
		sd = script_rid2sd(st); //Attached player
		if( sd == NULL )
			return 0;
	}

	if( loc == 1 && !pc_iscarton(sd) ) {
		ShowError("buildin_cartdelitem: player doesn't have cart (CID=%d).\n", sd->status.char_id);
		return 1;
	}

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) {
		const char* item_name = conv_str(st, data);
		struct item_data* id = itemdb_searchname(item_name);

		if( id == NULL ) {
			ShowError("buildin_%s: unknown item \"%s\".\n", command, item_name);
			st->state = END;
			return 1;
		}
		it.nameid = id->nameid; //"<item name>"
	} else {
		it.nameid = conv_num(st,data); //<item id>
		if( !itemdb_exists( it.nameid ) ) {
			ShowError("buildin_%s: unknown item \"%hu\".\n", command, it.nameid);
			st->state = END;
			return 1;
		}
	}

	it.amount = script_getnum(st,3);

	if( it.amount <= 0 )
		return 0; //Nothing to do

	if( buildin_delitem_search(sd, &it, false, loc) ) //Success
		return 0;

	ShowError("buildin_%s: failed to delete %d items (AID=%d item_id=%hu).\n", command, it.amount, sd->status.account_id, it.nameid);
	st->state = END;
	st->mes_active = 0;
	clif_scriptclose(sd, st->oid);
	return SCRIPT_CMD_FAILURE;
}

/// Deletes items from the target/attached player.
///
/// delitem2 <item id>,<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>}
/// delitem2 "<Item name>",<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>}
/// cartdelitem2 <item id>,<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>}
/// cartdelitem2 "<Item name>",<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>}
/// storagedelitem2 <item id>,<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>}
/// storagedelitem2 "<Item name>",<amount>,<identify>,<refine>,<attribute>,<card1>,<card2>,<card3>,<card4>{,<account ID>}
BUILDIN_FUNC(delitem2)
{
	TBL_PC *sd;
	struct item it;
	struct script_data *data;
	uint8 loc = 0;
	char* command = (char*)script_getfuncname(st);

	if( !strncmp(command, "cart", 4) )
		loc = 1;
	else if( !strncmp(command, "storage", 7) )
		loc = 2;
	//@TODO: 3 - Guild Storage

	if( script_hasdata(st,11) ) {
		int account_id = script_getnum(st,11);

		sd = map_id2sd(account_id); //<account id>
		if( sd == NULL ) {
			ShowError("buildin_%s: player not found (AID=%d).\n", command, account_id);
			st->state = END;
			return 1;
		}
	} else {
		sd = script_rid2sd(st); //Attached player
		if( sd == NULL )
			return 0;
	}

	if( loc == 1 && !pc_iscarton(sd) ) {
		ShowError("buildin_cartdelitem: player doesn't have cart (CID=%d).\n", sd->status.char_id);
		script_pushint(st,-1);
		return 1;
	}

	data = script_getdata(st,2);
	get_val(st,data);
	if( data_isstring(data) ) {
		const char* item_name = conv_str(st,data);
		struct item_data* id = itemdb_searchname(item_name);

		if( id == NULL ) {
			ShowError("buildin_%s: unknown item \"%s\".\n", command, item_name);
			st->state = END;
			return 1;
		}
		it.nameid = id->nameid; //"<item name>"
	} else {
		it.nameid = conv_num(st,data); //<item id>
		if( !itemdb_exists( it.nameid ) ) {
			ShowError("buildin_%s: unknown item \"%hu\".\n", command, it.nameid);
			st->state = END;
			return 1;
		}
	}

	it.amount = script_getnum(st,3);
	it.identify = script_getnum(st,4);
	it.refine = script_getnum(st,5);
	it.attribute = script_getnum(st,6);
	it.card[0] = (short)script_getnum(st,7);
	it.card[1] = (short)script_getnum(st,8);
	it.card[2] = (short)script_getnum(st,9);
	it.card[3] = (short)script_getnum(st,10);

	if( it.amount <= 0 )
		return 0; //Nothing to do

	if( buildin_delitem_search(sd, &it, true, loc) ) //Success
		return 0;

	ShowError("buildin_%s: failed to delete %d items (AID=%d item_id=%hu).\n", command, it.amount, sd->status.account_id, it.nameid);
	st->state = END;
	st->mes_active = 0;
	clif_scriptclose(sd, st->oid);
	return SCRIPT_CMD_FAILURE;
}

/*==========================================
 * Enables/Disables use of items while in an NPC [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC(enableitemuse)
{
	TBL_PC *sd = script_rid2sd(st);

	if( sd )
		st->npc_item_flag = sd->npc_item_flag = 1;
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(disableitemuse)
{
	TBL_PC *sd = script_rid2sd(st);

	if( sd )
		st->npc_item_flag = sd->npc_item_flag = 0;
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Returns a character's specified stat.
 * Check pc_readparam for available options.
 *------------------------------------------*/
BUILDIN_FUNC(readparam)
{
	int type;
	TBL_PC *sd;

	type = script_getnum(st,2);
	if( script_hasdata(st,3) )
		sd = map_nick2sd(script_getstr(st,3));
	else
		sd = script_rid2sd(st);

	if( sd == NULL ) {
		script_pushint(st,-1);
		return 0;
	}
	script_pushint(st,pc_readparam(sd,type));
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Return charid identification
 * return by @num :
 *	0 : char_id
 *	1 : party_id
 *	2 : guild_id
 *	3 : account_id
 *	4 : bg_id
 *------------------------------------------*/
BUILDIN_FUNC(getcharid)
{
	int num;
	TBL_PC *sd;

	num = script_getnum(st,2);
	if( script_hasdata(st,3) )
		sd = map_nick2sd(script_getstr(st,3));
	else
		sd = script_rid2sd(st);

	if( sd == NULL ) {
		script_pushint(st,0); //Return 0, according docs
		return 0;
	}

	switch( num ) {
		case 0: script_pushint(st,sd->status.char_id); break;
		case 1: script_pushint(st,sd->status.party_id); break;
		case 2: script_pushint(st,sd->status.guild_id); break;
		case 3: script_pushint(st,sd->status.account_id); break;
		case 4: script_pushint(st,sd->bg_id); break;
		default:
			ShowError("buildin_getcharid: invalid parameter (%d).\n", num);
			script_pushint(st,0);
			break;
	}
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * returns the GID of an NPC
 *------------------------------------------*/
BUILDIN_FUNC(getnpcid)
{
	int num = script_getnum(st,2);
	struct npc_data* nd = NULL;

	if( script_hasdata(st,3) ) { // Unique npc name
		if( ( nd = npc_name2id(script_getstr(st,3)) ) == NULL ) {
			ShowError("buildin_getnpcid: No such NPC '%s'.\n", script_getstr(st,3));
			script_pushint(st,0);
			return 1;
		}
	}

	switch( num ) {
		case 0:
			script_pushint(st,nd ? nd->bl.id : st->oid);
			break;
		default:
			ShowError("buildin_getnpcid: invalid parameter (%d).\n", num);
			script_pushint(st,0);
			return 1;
	}
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Return the name of the party_id
 * null if not found
 *------------------------------------------*/
BUILDIN_FUNC(getpartyname)
{
	int party_id;
	struct party_data* p;

	party_id = script_getnum(st,2);

	if( (p = party_search(party_id)) != NULL )
		script_pushstrcopy(st,p->party.name);
	else
		script_pushconststr(st,"null");
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get the information of the members of a party by type
 * @party_id, @type
 * return by @type :
 *	- : nom des membres
 *	1 : char_id des membres
 *	2 : account_id des membres
 *------------------------------------------*/
BUILDIN_FUNC(getpartymember)
{
	struct party_data *p;
	unsigned char j = 0;

	p = party_search(script_getnum(st,2));

	if( p != NULL ) {
		unsigned char i;
		int type = 0;

		if( script_hasdata(st,3) )
			type = script_getnum(st,3);
		for( i = 0; i < MAX_PARTY; i++ ) {
			if( p->party.member[i].account_id ) {
				switch( type ) {
					case 2:
						mapreg_setreg(reference_uid(add_str("$@partymemberaid"), j),p->party.member[i].account_id);
						break;
					case 1:
						mapreg_setreg(reference_uid(add_str("$@partymembercid"), j),p->party.member[i].char_id);
						break;
					default:
						mapreg_setregstr(reference_uid(add_str("$@partymembername$"), j),p->party.member[i].name);
				}
				j++;
			}
		}
	}
	mapreg_setreg(add_str("$@partymembercount"),j);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Retrieves party leader. if flag is specified,
 * return some of the leader data. Otherwise, return name.
 *------------------------------------------*/
BUILDIN_FUNC(getpartyleader)
{
	int party_id, type = 0, i = 0;
	struct party_data *p;

	party_id = script_getnum(st,2);
	if( script_hasdata(st,3) )
 		type = script_getnum(st,3);

	p = party_search(party_id);

	if( p ) //Search leader
		for( i = 0; i < MAX_PARTY && !p->party.member[i].leader; i++ );

	if( !p || i == MAX_PARTY ) { //Leader not found
		if( type )
			script_pushint(st,-1);
		else
			script_pushconststr(st,"null");
		return 0;
	}

	switch( type ) {
		case 1: script_pushint(st,p->party.member[i].account_id); break;
		case 2: script_pushint(st,p->party.member[i].char_id); break;
		case 3: script_pushint(st,p->party.member[i].class_); break;
		case 4: script_pushstrcopy(st,mapindex_id2name(p->party.member[i].map)); break;
		case 5: script_pushint(st,p->party.member[i].lv); break;
		default: script_pushstrcopy(st,p->party.member[i].name); break;
	}
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Return the name of the @guild_id
 * null if not found
 *------------------------------------------*/
BUILDIN_FUNC(getguildname)
{
	int guild_id;
	struct guild* g;

	guild_id = script_getnum(st,2);
	if( (g = guild_search(guild_id)) != NULL )
		script_pushstrcopy(st,g->name);
	else
		script_pushconststr(st,"null");
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Return the name of the guild master of @guild_id
 * null if not found
 *------------------------------------------*/
BUILDIN_FUNC(getguildmaster)
{
	int guild_id;
	struct guild* g;

	guild_id = script_getnum(st,2);
	if( (g = guild_search(guild_id)) != NULL )
		script_pushstrcopy(st,g->member[0].name);
	else
		script_pushconststr(st,"null");
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getguildmasterid)
{
	int guild_id;
	struct guild* g;

	guild_id = script_getnum(st,2);
	if( (g = guild_search(guild_id)) != NULL )
		script_pushint(st,g->member[0].char_id);
	else
		script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get char string information by type :
 * Return by @type :
 *	0 : char_name
 *	1 : party_name or ""
 *	2 : guild_name or ""
 *	3 : map_name
 *	- : ""
 *------------------------------------------*/
BUILDIN_FUNC(strcharinfo)
{
	TBL_PC *sd;
	int num;
	struct guild* g;
	struct party_data* p;

	sd = script_rid2sd(st);
	if( !sd ) //Avoid crashing
		return 0;

	num = script_getnum(st,2);
	switch( num ) {
		case 0:
			script_pushstrcopy(st,sd->status.name);
			break;
		case 1:
			if( (p = party_search(sd->status.party_id)) != NULL )
				script_pushstrcopy(st,p->party.name);
			else
				script_pushconststr(st,"");
			break;
		case 2:
			if( (g = sd->guild) != NULL )
				script_pushstrcopy(st,g->name);
			else
				script_pushconststr(st,"");
			break;
		case 3:
			script_pushconststr(st,map[sd->bl.m].name);
			break;
		default:
			ShowWarning("buildin_strcharinfo: unknown parameter.\n");
			script_pushconststr(st,"");
			break;
	}

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get npc string information by type
 * Return by @type:
 *	0 : name
 *	1 : str#
 *	2 : #str
 *	3 : ::str
 *	4 : map name
 *------------------------------------------*/
BUILDIN_FUNC(strnpcinfo)
{
	TBL_NPC* nd;
	int num;
	char *buf, *name = NULL;

	nd = map_id2nd(st->oid);
	if( !nd ) {
		script_pushconststr(st,"");
		return 0;
	}

	num = script_getnum(st,2);
	switch( num ) {
		case 0: // Display name
			name = aStrdup(nd->name);
			break;
		case 1: // Visible part of display name
			if( (buf = strchr(nd->name,'#')) != NULL ) {
				name = aStrdup(nd->name);
				name[buf - nd->name] = 0;
			} else // Return the name, there is no '#' present
				name = aStrdup(nd->name);
			break;
		case 2: // # Fragment
			if( (buf = strchr(nd->name,'#')) != NULL )
				name = aStrdup(buf + 1);
			break;
		case 3: // Unique name
			name = aStrdup(nd->exname);
			break;
		case 4: // Map name
			if( nd->bl.m >= 0 ) // Only valid map indexes allowed (bugreport:8034)
				name = aStrdup(map[nd->bl.m].name);
			break;
	}

	if( name )
		script_pushstr(st,name);
	else
		script_pushconststr(st,"");

	return SCRIPT_CMD_SUCCESS;
}

// Aegis->Athena slot position conversion table
static unsigned int equip[] = {
	EQP_HEAD_TOP,
	EQP_ARMOR,
	EQP_HAND_L,
	EQP_HAND_R,
	EQP_GARMENT,
	EQP_SHOES,
	EQP_ACC_L,
	EQP_ACC_R,
	EQP_HEAD_MID,
	EQP_HEAD_LOW,
	EQP_COSTUME_HEAD_TOP,
	EQP_COSTUME_HEAD_MID,
	EQP_COSTUME_HEAD_LOW,
	EQP_COSTUME_GARMENT,
	EQP_AMMO,
	EQP_SHADOW_ARMOR,
	EQP_SHADOW_WEAPON,
	EQP_SHADOW_SHIELD,
	EQP_SHADOW_SHOES,
	EQP_SHADOW_ACC_R,
	EQP_SHADOW_ACC_L
};

/*==========================================
 * GetEquipID(Pos);     Pos: 1-14
 *------------------------------------------*/
BUILDIN_FUNC(getequipid)
{
	int i, num;
	TBL_PC* sd;
	struct item_data* item;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	num = script_getnum(st,2) - 1;
	if( num < 0 || num >= ARRAYLENGTH(equip) ) {
		script_pushint(st,-1);
		return 0;
	}

	// Get inventory position of item
	i = pc_checkequip(sd,equip[num]);
	if( i < 0 ) {
		script_pushint(st,-1);
		return 0;
	}
		
	item = sd->inventory_data[i];
	if( item != 0 )
		script_pushint(st,item->nameid);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get the equipement name at pos
 * return item jname or ""
 *------------------------------------------*/
BUILDIN_FUNC(getequipname)
{
	int i, num;
	TBL_PC* sd;
	struct item_data* item;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	num = script_getnum(st,2) - 1;
	if( num < 0 || num >= ARRAYLENGTH(equip) ) {
		script_pushconststr(st,"");
		return 0;
	}

	// Get inventory position of item
	i = pc_checkequip(sd,equip[num]);
	if( i < 0 ) {
		script_pushconststr(st,"");
		return 0;
	}

	item = sd->inventory_data[i];
	if( item != 0 )
		script_pushstrcopy(st,item->jname);
	else
		script_pushconststr(st,"");

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * getbrokenid [Valaris]
 *------------------------------------------*/
BUILDIN_FUNC(getbrokenid)
{
	int i, num, id = 0, brokencounter = 0;
	TBL_PC *sd;

	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	num = script_getnum(st,2);
	for(i = 0; i < MAX_INVENTORY; i++) {
		if(sd->status.inventory[i].attribute) {
				brokencounter++;
				if(num == brokencounter) {
					id = sd->status.inventory[i].nameid;
					break;
				}
		}
	}

	script_pushint(st,id);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * repair [Valaris]
 *------------------------------------------*/
BUILDIN_FUNC(repair)
{
	int i,num;
	int repaircounter = 0;
	TBL_PC *sd;

	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	num = script_getnum(st,2);
	for(i = 0; i < MAX_INVENTORY; i++) {
		if(sd->status.inventory[i].attribute) {
				repaircounter++;
				if(num == repaircounter) {
					sd->status.inventory[i].attribute = 0;
					clif_equiplist(sd);
					clif_produceeffect(sd,0,sd->status.inventory[i].nameid);
					clif_misceffect(&sd->bl,3);
					break;
				}
		}
	}

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * repairall
 *------------------------------------------*/
BUILDIN_FUNC(repairall)
{
	int i, repaircounter = 0;
	TBL_PC *sd;

	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	for(i = 0; i < MAX_INVENTORY; i++) {
		if(sd->status.inventory[i].nameid && sd->status.inventory[i].attribute) {
			sd->status.inventory[i].attribute = 0;
			clif_produceeffect(sd,0,sd->status.inventory[i].nameid);
			repaircounter++;
		}
	}

	if(repaircounter) {
		clif_misceffect(&sd->bl,3);
		clif_equiplistR,sd->vd.cloth_colSCRIPT_CMD_SUCCESSr);
		clif_skillinfoblock(sd);
	}

	return 0;
}

/*==Chk if player have something equiped at pos
 *------------------------------------------*/
BUILDIN_FUNC(getequipisequiped)
{
	int i = -1,num;
	TBL_PC *sd;

	num = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);

	if(i >= 0)
		script_pushint(st,1);
	else
		script_pushint(st,0;
	}
	mapitSCRIPT_CMD_SUCCESSfree(iter);
	script_pushint(st,1);
	return 0;
}

/*==Chk if the player have something equiped at pos
 * if so chk if this item ain't marked not refinable or rental
 * Return (npc)
 *	1 : true
 *	0 : false
 *------------------------------------------*/
BUILDIN_FUNC(getequipisenableref)
{
	int i = -1,num;
	TBL_PC *sd;

	num = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);
	if(i >= 0 && sd->inventory_data[i] && !sd->inventory_data[i]->flag.no_refine && !sd->status.inventory[i].expire_time)
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get the item refined value at pos
 * Return (npc)
 *	x : refine amount
 *	0 : false (not refined)
 *------------------------------------------*/
BUILDIN_FUNC(getequiprefinerycnt)
{
	int i = -1,num;
	TBL_PC *sd;

	num = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);
	if(i >= 0)
		script_pushint(st,sd->status.inventory[i].refine);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get the weapon level value at pos
 * (pos should normally only be EQI_HAND_L or EQI_HAND_R)
 * Return (npc)
 *	x : weapon level
 *	0 : false
 *------------------------------------------*/
BUILDIN_FUNC(getequipweaponlv)
{
	int i = -1,num;
	TBL_PC *sd;

	num = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);
	if(i >= 0 && sd->inventory_data[i])
		script_pushint(st,sd->inventory_data[i]->wlv);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Get the item refine chance (from refine.txt) for item at pos
 * Return (npc)
 *	x : refine chance
 *	0 : false (max refine level or unequip..)
 *------------------------------------------*/
BUILDIN_FUNC(getequippercentrefinery)
{
	int i = -1,num;
	TBL_PC *sd;

	num = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);
	if(i >= 0 && sd->status.inventory[i].nameid && sd->status.inventory[i].refine < MAX_REFINE)
		script_pushint(st,status_get_refine_chance((enum refine_type)itemdb_wlv(sd->status.inventory[i].nameid), (int)sd->status.inventory[i].refine));
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Refine +1 item at pos and log and display refine
 *------------------------------------------*/
BUILDIN_FUNC(successrefitem)
{
	short i = -1, up = 1;
	int pos;
	TBL_PC *sd;

	pos = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;
	if(script_hasdata(st,3))
		up = script_getnum(st,3);
	if(pos > 0 && pos <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[pos - 1]);
	if(i >= 0) {
		unsigned int ep = sd->status.inventory[i].equip;

		//Logs items, got from (N)PC scripts [Lupus]
		log_pick_pc(sd,LOG_TYPE_SCRIPT,-1,&sd->status.inventory[i]);

		if(sd->status.inventory[i].refine >= MAX_REFINE) {
			script_pushint(st,MAX_REFINE);
			return 0;
		}

		sd->status.inventory[i].refine += up;
		sd->status.inventory[i].refine = cap_value(sd->status.inventory[i].refine,0,MAX_REFINE);
		pc_unequipitem(sd,i,2); //Status calc will happen in pc_equipitem() below

		clif_refine(sd->fd,0,i,sd->status.inventory[i].refine);
		clif_delitem(sd,i,1,3);

		//Logs items, got from (N)PC scripts [Lupus]
		log_pick_pc(sd,LOG_TYPE_SCRIPT,1,&sd->status.inventory[i]);

		clif_additem(sd,i,1,0);
		pc_equipitem(sd,i,ep);
		clif_misceffect(&sd->bl,3);
		if(sd->status.inventory[i].refine == 10 &&
			sd->status.inventory[i].card[0] == CARD0_FORGE &&
		  	sd->status.char_id == (int)MakeDWord(sd->status.inventory[i].card[2],sd->status.inventory[i].card[3])
		) { // Fame point system [DracoRPG]
	 		switch (sd->inventory_data[i]->wlv) {
				case 1:
					pc_addfame(sd,battle_config.fame_refine_lv1); // Success to refine to +10 a lv1 weapon you forged = +1 fame point
					break;
				case 2:
					pc_addfame(sd,battle_config.fame_refine_lv2); // Success to refine to +10 a lv2 weapon you forged = +25 fame point
					break;
				case 3:
					pc_addfame(sd,battle_config.fame_refine_lv3); // Success to refine to +10 a lv3 weapon you forged = +1000 fame point
					break;
	 	 	 }
		}
		script_pushint(st,sd->status.inventory[i].refine);
		return 0;
	}

	ShowError("buildin_successrefitem: No item equipped at pos %d (CID=%d/AID=%d).\n",pos,sd->status.char_id,sd->status.account_id);
	script_pushint(st,-1);
	return SCRIPT_CMD_FAILURE;
}

/*==========================================
 * Show a failed Refine +1 attempt
 *------------------------------------------*/
BUILDIN_FUNC(failedrefitem)
{
	short i = -1;
	int pos;
	TBL_PC *sd;

	pos = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;
	if(pos > 0 && pos <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[pos - 1]);
	if(i >= 0) {
		sd->status.inventory[i].refine = 0;
		pc_unequipitem(sd,i,3); //Recalculate bonus
		clif_refine(sd->fd,1,i,sd->status.inventory[i].refine); //Notify client of failure
		pc_delitem(sd,i,1,0,2,LOG_TYPE_SCRIPT);
		clif_misceffect(&sd->bl,2); //Display failure effect
		script_pushint(st,1);
		return 0;
	}

	ShowError("buildin_failedrefitem: No item equipped at pos %d (CID=%d/AID=%d).\n",pos,sd->status.char_id,sd->status.account_id);
	script_pushint(st,0);
	return SCRIPT_CMD_FAILURE;
}

/*==========================================
 * Downgrades an Equipment Part by -1,
 * unless optional <downgrade_count> is provided
 * in the specified equipment slot of the invoking character. [Masao]
 *------------------------------------------*/
BUILDIN_FUNC(downrefitem)
{
	short i = -1, down = 1;
	int pos;
	TBL_PC *sd;

	pos = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;
	if(script_hasdata(st,3))
		down = script_getnum(st,3);
	if(pos > 0 && pos <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[pos - 1]);
	if(i >= 0) {
		unsigned int ep = sd->status.inventory[i].equip;

		//Logs items, got from (N)PC scripts [Lupus]
		log_pick_pc(sd,LOG_TYPE_SCRIPT,-1,&sd->status.inventory[i]);

		pc_unequipitem(sd,i,2); //Status calc will happen in pc_equipitem() below
		sd->status.inventory[i].refine -= down;
		sd->status.inventory[i].refine = cap_value(sd->status.inventory[i].refine,0,MAX_REFINE);

		clif_refine(sd->fd,2,i,sd->status.inventory[i].refine);
		clif_delitem(sd,i,1,3);

		//Logs items, got from (N)PC scripts [Lupus]
		log_pick_pc(sd,LOG_TYPE_SCRIPT,1,&sd->status.inventory[i]);

		clif_additem(sd,i,1,0);
		pc_equipitem(sd,i,ep);
		clif_misceffect(&sd->bl,2);
		script_pushint(st,sd->status.inventory[i].refine);
		return 0;
	}

	ShowError("buildin_downrefitem: No item equipped at pos %d (CID=%d/AID=%d).\n",pos,sd->status.char_id,sd->status.account_id);
	script_pushint(st,-1);
	return SCRIPT_CMD_FAILURE;
}

/*==========================================
 * Delete the item equipped at pos.
 *------------------------------------------*/
BUILDIN_FUNC(delequip)
{
	short i = -1;
	int pos;
	int8 ret;
	TBL_PC *sd;

	pos = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(pos > 0 && pos <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[pos - 1]);
	if(i >= 0) {
		pc_unequipitem(sd,i,3); //Recalculate bonus
		ret = !(pc_delitem(sd,i,1,0,2,LOG_TYPE_SCRIPT));
	} else {
		ShowError("buildin_delequip: No item equipped at pos %d (CID=%d/AID=%d).\n",pos,sd->status.char_id,sd->status.account_id);
		st->state = END;
		return 1;
	}

  script_pushint(st,ret);
  lse
			SCRIPT_CMD_SUCCESSlif_emotion(&sd->hd->bl,E_SWT);
	}
	return 0;
}

/*==Break the item equipped at pos.
 *------------------------------------------*/
BUILDIN_FUNC(breakequip)
{
	short i = -1;
	int pos;
	TBL_PC *sd;

	pos = script_getnum(st,2);
	sd = script_rid2sd(st);
	if(sd == NULL)
		return 0;

	if(pos > 0 && pos <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[pos - 1]);
	if(i >= 0) {
		sd->status.inventory[i].attribute = 1;
		pc_unequipitem(sd,i,3);
		clif_equiplist(sd);
		script_pushint(st,1);
		return 0;
	}

	ShowError("buildin_breakequip: No item equipped at pos %d (CID=%d/AID=%d).\n",pos,sd->status.char_id,sd->status.account_id);
	script_pushint(st,0);
	return SCRIPT_CMD_FAILURE;
}
_point + pc_resetskill(sd,2));
	return 0;
}

/*==========================================
 *
 *--------------statusup)
{
	int type;
	TBL_PC *sd;

	type = script_getnum(st,2);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	pc_statusup(sd,type,1);
		return SCRIPT_CMD_SUCCESSpeproperty2(&bl, ALL_SAMEMAP);
	}

	return 0;
}
/*==========================================
 *
 *---------------tatusup2)
{
	int type, val;
	TBL_PC *sd;

	type = script_getnum(st,2);
	val = script_getnum(st,3);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	pc_statusup2(sd,type,val);

	return SCRIPT_CMD_SUCCESS;
}

/// See 'doc/item_bonus.txt'
///
/// bonus <bonus type>,<val1>;
/// bonus2 <bonus type>,<val1>,<val2>;
/// bonus3 <bonus type>,<val1>,<val2>,<val3>;
/// bonus4 <bonus type>,<val1>,<val2>,<val3>,<val4>;
/// bonus5 <bonus type>,<val1>,<val2>,<val3>,<val4>,<val5>;
BUILDIN_FUNC(bonus)
{
	int type;
	int val1;
	int val2 = 0;
	int val3 = 0;
	int val4 = 0;
	int val5 = 0;
	TBL_PC* sd;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached

	type = script_getnum(st,2);
	switch( type ) {
		case SP_AUTOSPELL:
		case SP_AUTOSPELL_WHENHIT:
		case SP_AUTOSPELL_ONSKILL:
		case SP_SKILL_ATK:
		case SP_SKILL_HEAL:
		case SP_SKILL_HEAL2:
		case SP_ADD_SKILL_BLOW:
		case SP_CASTRATE:
		case SP_ADDEFF_ONSKILL:
		case SP_SKILL_USE_SP_RATE:
		case SP_SKILL_COOLDOWN:
		case SP_SKILL_FIXEDCAST:
		case SP_SKILL_VARIABLECAST:
		case SP_VARCASTRATE:
		case SP_FIXCASTRATE:
		case SP_SKILL_USE_SP:
			// These bonuses support skill names
			data = script_getdata(st,3);
			get_val(st,data); // Convert into value in case of a variable
			val1 = (data_isstring(data) ? skill_name2id(script_getstr(st,3)) : script_getnum(st,3));
			break;
		default:
			val1 = script_getnum(st,3);
			break;
	}

	switch( script_lastdata(st) - 2 ) {
		case 1:
			pc_bonus(sd,type,val1);
			break;
		case 2:
			val2 = script_getnum(st,4);
			pc_bonus2(sd,type,val1,val2);
			break;
		case 3:
			val2 = script_getnum(st,4);
			val3 = script_getnum(st,5);
			pc_bonus3(sd,type,val1,val2,val3);
			break;
		case 4:
			data = script_getdata(st,4);
			get_val(st, data); // Convert into value in case of a variable
			if( type == SP_AUTOSPELL_ONSKILL && data_isstring(data) )
				val2 = skill_name2id(script_getstr(st,4)); // 2nd value can be skill name
			else
				val2 = script_getnum(st,4);

			val3 = script_getnum(st,5);
			val4 = script_getnum(st,6);
			pc_bonus4(sd,type,val1,val2,val3,val4);
			break;
		case 5:
			data = script_getdata(st,4);
			get_val(st,data); // Convert into value in case of a variable
			if( type == SP_AUTOSPELL_ONSKILL && data_isstring(data) )
				val2 = skill_name2id(script_getstr(st,4)); // 2nd value can be skill name
			else
				val2 = script_getnum(st,4);

			val3 = script_getnum(st,5);
			val4 = script_getnum(st,6);
			val5 = script_getnum(st,7);
			pc_bonus5(sd,type,val1,val2,val3,val4,val5);
			break;
		default:
			ShowDebug("buildin_bonus: unexpected number of arguments (%d)\n",(script_lastdata(st) - 1));
			break;
	}

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(autobonus)
{
	unsigned int dur, pos;
	short rate;
	short atk_type = 0;
	TBL_PC* sd;
	const char *bonus_script, *other_script = NULL;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached

	if( current_equip_combo_pos )
		pos = current_equip_combo_pos;
	else
		pos = sd->status.inventory[current_equip_item_index].equip;

	if( (sd->state.autobonus&pos) == pos )
		return 0;

	rate = script_getnum(st,3);
	dur = script_getnum(st,4);
	bonus_script = script_getstr(st,2);
	if( !rate || !dur || !pos || !bonus_script )
		return 0;

	if( script_hasdata(st,5) )
		atk_type = script_getnum(st,5);

	if( script_hasdata(st,6) )
		other_script = script_getstr(st,6);

	if( pc_addautobonus(sd->autobonus,ARRAYLENGTH(sd->autobonus),bonus_script,rate,dur,atk_type,other_script,pos,false) ) {
		script_add_autobonus(bonus_script);
		if( other_script )
			script_add_autobonus(other_script);
	}

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(autobonus2)
{
	unsigned int dur, pos;
	short rate;
	short atk_type = 0;
	TBL_PC* sd;
	const char *bonus_script, *other_script = NULL;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached

	if( current_equip_combo_pos )
		pos = current_equip_combo_pos;
	else
		pos = sd->status.inventory[current_equip_item_index].equip;

	if( (sd->state.autobonus&pos) == pos )
		return 0;

	rate = script_getnum(st,3);
	dur = script_getnum(st,4);
	bonus_script = script_getstr(st,2);
	if( !rate || !dur || !pos || !bonus_script )
		return 0;

	if( script_hasdata(st,5) )
		atk_type = script_getnum(st,5);

	if( script_hasdata(st,6) )
		other_script = script_getstr(st,6);

	if( pc_addautobonus(sd->autobonus2,ARRAYLENGTH(sd->autobonus2),bonus_script,rate,dur,atk_type,other_script,pos,false) ) {
		script_add_autobonus(bonus_script);
		if( other_script )
			script_add_autobonus(other_script);
	}

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(autobonus3)
{
	unsigned int dur, pos;
	short rate,atk_type;
	TBL_PC* sd;
	const char *bonus_script, *other_script = NULL;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached

	if( current_equip_combo_pos )
		pos = current_equip_combo_pos;
	else
		pos = sd->status.inventory[current_equip_item_index].equip;

	if( (sd->state.autobonus&pos) == pos )
		return 0;

	rate = script_getnum(st,3);
	dur = script_getnum(st,4);
	data = script_getdata(st,5);
	get_val(st,data); // Convert into value in case of a variable
	atk_type = (data_isstring(data) ? skill_name2id(script_getstr(st,5)) : script_getnum(st,5));
	bonus_script = script_getstr(st,2);
	if( !rate || !dur || !pos || !atk_type || !bonus_script )
		return 0;

	if( script_hasdata(st,6) )
		other_script = script_getstr(st,6);

	if( pc_addautobonus(sd->autobonus3,ARRAYLENGTH(sd->autobonus3),bonus_script,rate,dur,atk_type,other_script,pos,true) ) {
		script_add_autobonus(bonus_script);
		if( other_script )
			script_add_autobonus(other_script);
	}

	return SCRIPT_CMD_SUCCESS;
}

/// Changes the level of a player skill.
/// <flag> defaults to 1
/// <flag>=0 : set the level of the skill
/// <flag>=1 : set the temporary level of the skill
/// <flag>=2 : add to the level of the skill
///
/// skill <skill id>,<level>,<flag>
/// skill <skill id>,<level>
/// skill "<skill name>",<level>,<flag>
/// skill "<skill name>",<level>
BUILDIN_FUNC(skill)
{
	int id;
	int level;
	int flag = 1;
	TBL_PC* sd;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	data = script_getdata(st,2);
	get_val(st,data); // Convert into value in case of a variable
	id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	level = script_getnum(st,3);
	if( script_hasdata(st,4) )
		flag = script_getnum(st,4);
	pc_skill(sd,id,level,flag);

	return SCRIPT_CMD_SUCCESS;
}

/// Changes the level of a player skill.
/// like skill, but <flag> defaults to 2
///
/// addtoskill <skill id>,<amount>,<flag>
/// addtoskill <skill id>,<amount>
/// addtoskill "<skill name>",<amount>,<flag>
/// addtoskill "<skill name>",<amount>
///
/// @see skill
BUILDIN_FUNC(addtoskill)
{
	int id;
	int level;
	int flag = 2;
	TBL_PC* sd;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	data = script_getdata(st,2);
	get_val(st,data); // Convert into value in case of a variable
	id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	level = script_getnum(st,3);
	if( script_hasdata(st,4) )
		flag = script_getnum(st,4);
	pc_skill(sd,id,level,flag);

	return SCRIPT_CMD_SUCCESS;
}

/// Increases the level of a guild skill.
///
/// guildskill <skill id>,<amount>;
/// guildskill "<skill name>",<amount>;
BUILDIN_FUNC(guildskill)
{
	int skill_id, id, max_points;
	int level;
	TBL_PC* sd;
	struct guild *gd;
	struct guild_skill gd_skill;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( (gd = sd->guild) == NULL )
		return 1;

	data = script_getdata(st,2);
	get_val(st,data); // Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	level = script_getnum(st,3);

	id = skill_id - GD_SKILLBASE;
	max_points = guild_skill_get_max(skill_id);

	if( (gd->skill[id].lv + level) > max_points )
		level = max_points - gd->skill[id].lv;

	if( level <= 0 )
		return 1;

	memcpy(&gd_skill,&(gd->skill[id]),sizeof(gd->skill[id]));
	gd_skill.lv += level;

	intif_guild_change_basicinfo(gd->guild_id,GBI_SKILLLV,&(gd_skill),sizeof(gd_skill));
	return SCRIPT_CMD_SUCCESS;
}

/// Returns the level of the player skill.
///
/// getskilllv(<skill id>) -> <level>
/// getskilllv("<skill name>") -> <level>
BUILDIN_FUNC(getskilllv)
{
	int id;
	TBL_PC* sd;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	data = script_getdata(st,2);
	get_val(st,data); // Convert into value in case of a variable
	id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	script_pushint(st,pc_checkskill(sd,id));

	return SCRIPT_CMD_SUCCESS;
}

/// Returns the level of the guild skill.
///
/// getgdskilllv(<guild id>,<skill id>) -> <level>
/// getgdskilllv(<guild id>,"<skill name>") -> <level>
BUILDIN_FUNC(getgdskilllv)
{
	int guild_id;
	uint16 skill_id;
	struct guild* g;
	struct script_data *data;

	guild_id = script_getnum(st,2);
	data = script_getdata(st,3);
	get_val(st,data); // Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,3)) : script_getnum(st,3));
	g = guild_search(guild_id);
	if( g == NULL )
		script_pushint(st,-1);
	else
		script_pushint(st,guild_checkskill(g,skill_id));

	return SCRIPT_CMD_SUCCESS;
}

/// Returns the 'basic_skill_check' setting.
/// This config determines if the server checks the skill level of NV_BASIC
/// before allowing the basic actions.
///
/// basicskillcheck() -> <bool>
BUILDIN_FUNC(basicskillcheck)
{
	script_pushint(st, battle_config.basic_skill_check);
	return SCRIPT_CMD_SUCCESS;
}

/// Returns the GM level of the player.
///
/// getgmlevel() -> <level>
BUILDIN_FUNC(getgmlevel)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	script_pushint(st, pc_get_group_level(sd));

	return SCRIPT_CMD_SUCCESS;
}

/// Returns the group ID of the player.
///
/// getgroupid() -> <int>
BUILDIN_FUNC(getgroupid)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1; // No player attached, report source
	script_pushint(st, pc_get_group_id(sd));

	return SCRIPT_CMD_SUCCESS;
}

/// Terminates the execution of this script instance.
///
/// end
BUILDIN_FUNC(end)
{
	TBL_PC* sd;

	sd = map_id2sd(st->rid);

	st->state = END;

	if( st->mes_active )
		st->mes_active = 0;

	if( sd )
		clif_scriptclose(sd, st->oid); // If a menu/select/prompt is active, close it.

	return SCRIPT_CMD_SUCCESS;
}

/// Checks if the player has that effect state (option).
///
/// checkoption(<option>) -> <bool>
BUILDIN_FUNC(checkoption)
{
	int option;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	option = script_getnum(st,2);
	if( sd->sc.option&option )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Checks if the player is in that body state (opt1).
///
/// checkoption1(<opt1>) -> <bool>
BUILDIN_FUNC(checkoption1)
{
	int opt1;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	opt1 = script_getnum(st,2);
	if( sd->sc.opt1 == opt1 )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Checks if the player has that health state (opt2).
///
/// checkoption2(<opt2>) -> <bool>
BUILDIN_FUNC(checkoption2)
{
	int opt2;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	opt2 = script_getnum(st,2);
	if( sd->sc.opt2&opt2 )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Changes the effect state (option) of the player.
/// <flag> defaults to 1
/// <flag>=0 : removes the option
/// <flag>=other : adds the option
///
/// setoption <option>,<flag>;
/// setoption <option>;
BUILDIN_FUNC(setoption)
{
	int option;
	int flag = 1;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	option = script_getnum(st,2);
	if( script_hasdata(st,3) )
		flag = script_getnum(st,3);
	else if( !option ) { // Request to remove everything.
		flag = 0;
		option = OPTION_FALCON|OPTION_RIDING;
#ifndef NEW_CARTS
		option |= OPTION_CART;
#endif
	}
	if( flag ) { // Add option
		if( option&OPTION_WEDDING && !battle_config.wedding_modifydisplay )
			option &= ~OPTION_WEDDING; // Do not show the wedding sprites
		pc_setoption(sd,sd->sc.option|option);
	} else // Remove option
		pc_setoption(sd,sd->sc.option&~option);

	return SCRIPT_CMD_SUCCESS;
}

/// Returns if the player has a cart.
///
/// checkcart() -> <bool>
///
/// @author Valaris
BUILDIN_FUNC(checkcart)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( pc_iscarton(sd) )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Sets the cart of the player.
/// <type> defaults to 1
/// <type>=0 : removes the cart
/// <type>=1 : Normal cart
/// <type>=2 : Wooden cart
/// <type>=3 : Covered cart with flowers and ferns
/// <type>=4 : Wooden cart with a Panda doll on the back
/// <type>=5 : Normal cart with bigger wheels, a roof and a banner on the back
///
/// setcart <type>;
/// setcart;
BUILDIN_FUNC(setcart)
{
	int type = 1;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( script_hasdata(st,2) )
		type = script_getnum(st,2);
	pc_setcart(sd,type);

	return SCRIPT_CMD_SUCCESS;
}

/// Returns if the player has a falcon.
///
/// checkfalcon() -> <bool>
///
/// @author Valaris
BUILDIN_FUNC(checkfalcon)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( pc_isfalcon(sd) )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Sets if the player has a falcon or not.
/// <flag> defaults to 1
///
/// setfalcon <flag>;
/// setfalcon;
BUILDIN_FUNC(setfalcon)
{
	int flag = 1;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( script_hasdata(st,2) )
		flag = script_getnum(st,2);

	pc_setfalcon(sd,flag);

	return SCRIPT_CMD_SUCCESS;
}

/// Returns if the player is riding.
///
/// checkriding() -> <bool>
///
/// @author Valaris
BUILDIN_FUNC(checkriding)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( pc_isriding(sd) )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Sets if the player is riding.
/// <flag> defaults to 1
///
/// setriding <flag>;
/// setriding;
BUILDIN_FUNC(setriding)
{
	int flag = 1;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( script_hasdata(st,2) )
		flag = script_getnum(st,2);
	pc_setriding(sd,flag);

	return SCRIPT_CMD_SUCCESS;
}

/// Returns if the player has a warg.
///
/// checkwug() -> <bool>
///
BUILDIN_FUNC(checkwug)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( pc_iswug(sd) || pc_isridingwug(sd) )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Returns if the player is wearing MADO Gear.
///
/// checkmadogear() -> <bool>
///
BUILDIN_FUNC(checkmadogear)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( pc_ismadogear(sd) )
		script_pushint(st,1);
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Sets if the player is riding MADO Gear.
/// <flag> defaults to 1
///
/// setmadogear <flag>;
/// setmadogear;
BUILDIN_FUNC(setmadogear)
{
	int flag = 1;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0; // No player attached, report source

	if( script_hasdata(st,2) )
		flag = script_getnum(st,2);
	pc_setmadogear(sd,flag);

	return SCRIPT_CMD_SUCCESS;
}

/// Sets the save point of the player.
///
/// save "<map name>",<x>,<y>
/// savepoint "<map name>",<x>,<y>
BUILDIN_FUNC(savepoint)
{
	int x;
	int y;
	short map;
	const char* str;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1; // No player attached, report source

	str = script_getstr(st,2);
	x   = script_getnum(st,3);
	y   = script_getnum(st,4);
	map = mapindex_name2id(str);
	if( map )
		pc_setsavepoint(sd,map,x,y);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * GetTimeTick(0: System Tick, 1: Time Second Tick)
 *------------------------------------------*/
BUILDIN_FUNC(gettimetick) /* Asgard Version */
{
	int type;
	time_t timer;
	struct tm *t;

	type = script_getnum(st,2);

	switch(type) {
		case 2:
			//type 2:(Get the number of seconds elapsed since 00:00 hours, Jan 1, 1970 UTC
			//        from the system clock.)
			script_pushint(st,(int)time(NULL));
			break;
		case 1:
			//type 1:(Second Ticks: 0-86399, 00:00:00-23:59:59)
			time(&timer);
			t = localtime(&timer);
			script_pushint(st,((t->tm_hour) * 3600 + (t->tm_min) * 60 + t->tm_sec));
			break;
		case 0:
		default:
			//type 0:(System Ticks)
			script_pushint(st,gettick());
			break);
		else
			SCRIPT_CMD_SUCCESSlif_emotion(&sd->hd->bl,E_SWT);
	}
	return 0;
}

/*==GetTime(Type);
 * 1: Sec     2: Min     3: Hour
 * 4: WeekDay     5: MonthDay     6: Month
 * 7: Year
 *------------------------------------------*/
BUILDIN_FUNC(gettime) /* Asgard Version */
{
	int type;
	time_t timer;
	struct tm *t;

	type = script_getnum(st,2);

	time(&timer);
	t = localtime(&timer);

	switch(type) {
		case 1://Sec(0~59)
			script_pushint(st,t->tm_sec);
			break;
		case 2://Min(0~59)
			script_pushint(st,t->tm_min);
			break;
		case 3://Hour(0~23)
			script_pushint(st,t->tm_hour);
			break;
		case 4://WeekDay(0~6)
			script_pushint(st,t->tm_wday);
			break;
		case 5://MonthDay(01~31)
			script_pushint(st,t->tm_mday);
			break;
		case 6://Month(01~12)
			script_pushint(st,t->tm_mon + 1);
			break;
		case 7://Year(20xx)
			script_pushint(st,t->tm_year + 1900);
			break;
		case 8://Year Day(01~366)
			script_pushint(st,t->tm_yday + 1);
			break;
		default://(format error)
			script_pushint(st,-1);
			break);
		else
			SCRIPT_CMD_SUCCESSlif_emotion(&sd->hd->bl,E_SWT);
	}
	return 0;
}

/*==GetTimeStr("TimeFMT", Length);
 *------------------------------------------*/
BUILDIN_FUNC(gettimestr)
{
	char *tmpstr;
	const char *fmtstr;
	int maxlen;
	time_t now = time(NULL);

	fmtstr = script_getstr(st,2);
	maxlen = script_getnum(st,3);

	tmpstr = (char *)aMalloc((maxlen + 1) * sizeof(char));
	strftime(tmpstr,maxlen,fmtstr,localtime(&now));
	tmpstr[maxlen] = '\0';

	script_pushstr(st,tmpstr);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Open player storage
 *------------------------------------------*/
BUILDIN_FUNC(openstorage)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	storage_storageopen(sd);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(guildopenstorage)
{
	TBL_PC* sd;
	int ret;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	ret = storage_guild_storageopen(sd);
	script_pushint(st,ret);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Make player use a skill trought item usage
 *------------------------------------------*/
/// itemskill <skill id>,<level>,{flag}
/// itemskill "<skill name>",<level>,{flag}
BUILDIN_FUNC(itemskill)
{
	int id;
	int lv;
	TBL_PC* sd;
	struct script_data *data;

	sd = script_rid2sd(st);
	if( sd == NULL || sd->ud.skilltimer != INVALID_TIMER )
		return 0;

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	lv = script_getnum(st,3);
	//Temporarily disabled, awaiting for confirmation
#if 0
	if( !script_hasdata(st,4) )
		if( !skill_check_condition_castbegin(sd,id,lv) || !skill_check_condition_castend(sd,id,lv) )
			return 0;
#endif
	sd->skillitem = id;
	sd->skillitemlv = lv;
	clif_item_skill(sd,id,lv);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * Attempt to create an item
 *------------------------------------------*/
BUILDIN_FUNC(produce)
{
	int trigger;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	trigger = script_getnum(st,2);
	clif_skill_produce_mix_list(sd,-1,trigger);
	return SCRIPT_CMD_SUCCESS;
}_point + pc_resetskill(sd,2));
	return 0;
}

/*==========================================
 *
 *---------------ooking)
{
	int trigger;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	trigger = script_getnum(st,2);
	clif_cooking_list(sd,trigger,AM_PHARMACY,1,1);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * Create a pet
 *------------------------------------------*/
BUILDIN_FUNC(makepet)
{
	TBL_PC* sd;
	int id, pet_id;

	id = script_getnum(st,2);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	pet_id = search_petDB_index(id, PET_CLASS);

	if( pet_id < 0 )
		pet_id = search_petDB_index(id, PET_EGG);
	if( pet_id >= 0 && sd ) {
		sd->catch_target_class = pet_db[pet_id].class_;
		intif_create_pet(
			sd->status.account_id, sd->status.char_id,
			(short)pet_db[pet_id].class_,(short)mob_db(pet_db[pet_id].class_)->lv,
			(short)pet_db[pet_id].EggID,0,(short)pet_db[pet_id].intimate,
			100,0,1,pet_db[pet_id].jname);
	}

	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * Give player exp base,job * quest_exp_rate/100
 *------------------------------------------*/
BUILDIN_FUNC(getexp)
{
	TBL_PC* sd;
	int base = 0,job = 0;
	double bonus;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	base = script_getnum(st,2);
	job = script_getnum(st,3);
	if( base < 0 || job < 0 )
		return 0;

	// Bonus for npc-given exp
	bonus = battle_config.quest_exp_rate / 100.;
	base = (int) cap_value(base * bonus,0,INT_MAX);
	job = (int) cap_value(job * bonus,0,INT_MAX);

	pc_gainexp(sd,&sd->bl,base,job,true);
 debug : SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Gain guild exp [Celest]
 *------------------------------------------*/
BUILDIN_FUNC(guildgetexp)
{
	TBL_PC* sd;
	int exp;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	exp = script_getnum(st,2);
	if( exp < 0 )
		return 0;
	if( sd && sd->status.guild_id > 0 )
		guild_getexp (sd,exp);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Changes the guild master of a guild [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC(guildchangegm)
{
	TBL_PC *sd;
	int guild_id;
	const char *name;

	guild_id = script_getnum(st,2);
	name = script_getstr(st,3);
	sd = map_nick2sd(name);

	if( !sd )
		script_pushint(st,0);
	else
		script_pushint(st,guild_gm_change(guild_id, sd));

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Spawn a monster :
 * *monster "<map name>",<x>,<y>,"<name to show>",<mob id>,<amount>{,"<event label>",<size>,<ai>};
 @mapn,x,y : location
 @str : monster name
 @class_ : mob_id
 @amount : nb to spawn
 @event : event to attach to mob
 *------------------------------------------*/
BUILDIN_FUNC(monster)
{
	const char* mapn    = script_getstr(st,2);
	int x               = script_getnum(st,3);
	int y               = script_getnum(st,4);
	const char* str     = script_getstr(st,5);
	int class_          = script_getnum(st,6);
	int amount          = script_getnum(st,7);
	const char* event   = "";
	unsigned int size   = SZ_SMALL;
	unsigned int ai     = AI_NONE;

	struct map_session_data* sd;
	int16 m;
	int i;

	if (script_hasdata(st, 8)) {
		event = script_getstr(st, 8);
		check_event(st, event);
	}

	if (script_hasdata(st, 9)) {
		size = script_getnum(st, 9);
		if (size > 3) {
			ShowWarning("buildin_monster: Attempted to spawn non-existing size %d for monster class %d\n", size, class_);
			return 1;
		}
	}

	if (script_hasdata(st, 10)) {
		ai = script_getnum(st, 10);
		if (ai >= AI_MAX) {
			ShowWarning("buildin_monster: Attempted to spawn non-existing ai %d for monster class %d\n", ai, class_);
			return 1;
		}
	}

	if (class_ >= 0 && !mobdb_checkid(class_)) {
		ShowWarning("buildin_monster: Attempted to spawn non-existing monster class %d\n", class_);
		return 1;
	}

	sd = map_id2sd(st->rid);

	if (sd && strcmp(mapn, "this") == 0)
		m = sd->bl.m;
	else
		m = map_mapname2mapid(mapn);

	for (i = 0; i < amount; i++) { //Not optimised
		int mobid = mob_once_spawn(sd, m, x, y, str, class_, 1, event, size, ai);

		if (mobid)
			mapreg_setreg(reference_uid(add_str("$@mobid"), i),mobid);
	}
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * Request List of Monster Drops
 *------------------------------------------*/
BUILDIN_FUNC(getmobdrops)
{
	int class_ = script_getnum(st,2);
	int i, j = 0;
	struct mob_db *mob;

	if( !mobdb_checkid(class_) ) {
		script_pushint(st, 0);
		return 0;
	}

	mob = mob_db(class_);

	for( i = 0; i < MAX_MOB_DROP; i++ ) {
		if( mob->dropitem[i].nameid < 1 )
			continue;
		if( itemdb_exists(mob->dropitem[i].nameid) == NULL )
			continue;

		mapreg_setreg(reference_uid(add_str("$@MobDrop_item"), j), mob->dropitem[i].nameid);
		mapreg_setreg(reference_uid(add_str("$@MobDrop_rate"), j), mob->dropitem[i].p);

		j++;
	}

	mapreg_setreg(add_str("$@MobDrop_count"), j);
	script_pushint(st, 1);

	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * Same as monster but randomize location in x0,x1,y0,y1 area
 *------------------------------------------*/
BUILDIN_FUNC(areamonster)
{
	const char* mapn    = script_getstr(st,2);
	int x0              = script_getnum(st,3);
	int y0              = script_getnum(st,4);
	int x1              = script_getnum(st,5);
	int y1              = script_getnum(st,6);
	const char* str     = script_getstr(st,7);
	int class_          = script_getnum(st,8);
	int amount          = script_getnum(st,9);
	const char* event   = "";
	unsigned int size   = SZ_SMALL;
	unsigned int ai     = AI_NONE;

	struct map_session_data* sd;
	int16 m;

	if (script_hasdata(st,10)) {
		event = script_getstr(st, 10);
		check_event(st, event);
	}
	
	if (script_hasdata(st, 11)) {
		size = script_getnum(st, 11);
		if (size > 3) {
			ShowWarning("buildin_monster: Attempted to spawn non-existing size %d for monster class %d\n", size, class_);
			return 1;
		}
	}
	
	if (script_hasdata(st, 12)) {
		ai = script_getnum(st, 12);
		if (ai >= AI_MAX) {
			ShowWarning("buildin_monster: Attempted to spawn non-existing ai %d for monster class %d\n", ai, class_);
			return 1;
		}
	}

	sd = map_id2sd(st->rid);

	if (sd && strcmp(mapn, "this") == 0)
		m = sd->bl.m;
	else
		m = map_mapname2mapid(mapn);

	mob_once_spawn_area(sd, m, x0, y0, x1, y1, str, class_, amount, event, size, ai);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 * KillMonster subcheck, verify if mob to kill ain't got an even to handle, could be force kill by allflag
 *------------------------------------------*/
 static int buildin_killmonster_sub_strip(struct block_list *bl,va_list ap)
{ //same fix but with killmonster instead - stripping events from mobs.
	TBL_MOB* md = (TBL_MOB*)bl;
	char *event=va_arg(ap,char *);
	int allflag=va_arg(ap,int);

	md->state.npc_killmonster = 1;
	
	if(!allflag) {
		if(strcmp(event,md->npc_event)==0)
			status_kill(bl);
	} else {
		if(!md->spawn)
			status_kill(bl);
	}
	md->state.npc_killmonster = 0;
	return 0;
}

static int buildin_killmonster_sub(struct block_list *bl,va_list ap)
{
	TBL_MOB* md = (TBL_MOB*)bl;
	char *event=va_arg(ap,char *);
	int allflag=va_arg(ap,int);

	if(!allflag){
		if(strcmp(event,md->npc_event)==0)
			status_kill(bl);
	}else{
		if(!md->spawn)
			status_kill(bl);
	}
	return 0;
}

BUILDIN_FUNC(killmonster)
{
	const char *mapname,*event;
	int16 m,allflag=0;
	mapname=script_getstr(st,2);
	event=script_getstr(st,3);
	if( strcmp(event,"All")==0 )
		allflag = 1;
	else
		check_event(st, event);

	if( (m=map_mapname2mapid(mapname))<0 )
		return 0;

	if( script_hasdata(st,4) ) {
		if ( script_getnum(st,4) == 1 ) {
			map_foreachinmap(buildin_killmonster_sub, m, BL_MOB, event ,allflag);
			return 0;
		}
	}

	map_freeblock_lock();
	map_foreachinmap(buildin_killmonster_sub_strip, m, BL_MOB, event ,allflag);
	map_freeblock_unlock();
	return SCRIPT_CMD_SUCCESS;
}

static int buildin_killmonsterall_sub_strip(struct block_list *bl,va_list ap)
{ //Strips the event from the mob if it's killed the old method.
	struct mob_data *md;
	
	md = BL_CAST(BL_MOB, bl);
	if (md->npc_event[0])
		md->npc_event[0] = 0;
		
	status_kill(bl);
	return 0;
}

static int buildin_killmonsterall_sub(struct block_list *bl,va_list ap)
{
	status_kill(bl);
	return 0;
}

BUILDIN_FUNC(killmonsterall)
{
	const char *mapname;
	int16 m;
	mapname=script_getstr(st,2);

	if( (m = map_mapname2mapid(mapname))<0 )
		return 0;

	if( script_hasdata(st,3) ) {
		if ( script_getnum(st,3) == 1 ) {
			map_foreachinmap(buildin_killmonsterall_sub,m,BL_MOB);
			return 0;
		}
	}

	map_foreachinmap(buildin_killmonsterall_sub_strip,m,BL_MOB);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Creates a clone of a player.
 * clone map, x, y, event, char_id, master_id, mode, flag, duration
 *------------------------------------------*/
BUILDIN_FUNC(clone)
{
	TBL_PC *sd, *msd = NULL;
	int char_id, master_id = 0, x, y, mode = 0, flag = 0, m;
	unsigned int duration = 0;
	const char *map, *event;

	map->chat_id)) == NULL )
		return 0;

	map_name = script_getstr(st,2);
	x = scrievent = script_getstr(st,5);
	char_id = script_getnum(st,6);

	if( script_hasdata(st,7) )
		master_id = script_getnum(st,7);

	if( script_hasdata(st,8) )
		mode = script_getnum(st,8);

	if( script_hasdata(st,9) )
		flag = script_getnum(st,9);
	
	if( script_hasdata(st,10) )
		duration = script_getnum(st,10);

	check_event(st, event);

	m = map_mapname2mapid(map);
	if( m < 0 ) return 0;

	sd = map_charid2sd(char_id);

	if( master_id ) {
		msd = map_charid2sd(master_id);
		if( msd )
			master_id = msd->bl.id;
		else
			master_id = 0;
	}
	if( sd ) //Return ID of newly crafted clone.
		script_pushint(st,mob_clone_spawn(sd,m,x,y,event,master_id,mode,flag,1000 * duration));
	else //Failed to create clone.
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(doevent)
{
	const char* event = script_getstr(st,2);
	struct map_session_data* sd;

	if( ( sd = script_rid2sd(st) ) == NULL ) {
		return 0;
	}

	check_event(st,event);
	npc_event(sd,event,0);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(donpcevent)
{
	const char* event = script_getstr(st,2);
	check_event(st,event);
	if( !npc_event_do(event) ) {
		struct npc_data * nd = map_id2nd(st->oid);
		ShowDebug("NPCEvent '%s' not found! (source: %s)\n",event,nd?nd->name:"Unknown");
		script_pushint(st,0);
	} else
		script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

/// for Aegis compatibility
/// basically a specialized 'donpcevent', with the event specified as two arguments instead of one
BUILDIN_FUNC(cmdothernpc)	// Added by RoVeRT
{
	const char* npc = script_getstr(st,2);
	const char* command = script_getstr(st,3);
	char event[EVENT_NAME_LENGTH];
	snprintf(event,sizeof(event),"%s::OnCommand%s",npc,command);
	check_event(st,event);
	npc_event_do(event);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(addtimer)
{
	int tick = script_getnum(st,2);
	const char* event = script_getstr(st,3);
	TBL_PC* sd;

	check_event(st,event);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	if( !pc_addeventtimer(sd,tick,event) ) {
		ShowWarning("buildin_addtimer: Event timer is full, can't add new event timer. (cid:%d timer:%s)\n",sd->status.char_id,event);
		return 1;
	}
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(deltimer)
{
	const char *event;
	TBL_PC* sd;

	event = script_getstr(st,2);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	check_event(st,event);
	pc_deleventtimer(sd,event);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(addtimercount)
{
	const char *event;
	int tick;
	TBL_PC* sd;

	tick = script_getnum(st,2);
	event = script_getstr(st,3);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	check_event(st,event);
	pc_addeventtimercount(sd,event,tick);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(initnpctimer)
{
	struct npc_data *nd;
	int flag = 0;

	if( script_hasdata(st,3) ) { //Two arguments: NPC name and attach flag.
		nd = npc_name2id(script_getstr(st,2));
		flag = script_getnum(st,3);
	} else if( script_hasdata(st,2) ) { //Check if argument is numeric (flag) or string (npc name)
		struct script_data *data;
		data = script_getdata(st,2);
		get_val(st,data);
		if( data_isstring(data) ) //NPC name
			nd = npc_name2id(conv_str(st,data));
		else if( data_isint(data) ) { //Flag
			nd = (struct npc_data *)map_id2bl(st->oid);
			flag = conv_num(st,data);
		} else {
			ShowError("initnpctimer: invalid argument type #1 (needs be int or string)).\n");
			return 1;
		}
	} else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( !nd )
		return 0;
	if( flag ) { //Attach
		TBL_PC* sd = script_rid2sd(st);
		if( sd == NULL )
			return 0;
		nd->u.scr.rid = sd->bl.id;
	}

	nd->u.scr.timertick = 0;
	npc_settimerevent_tick(nd,0);
	npc_timerevent_start(nd,st->rid);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(startnpctimer)
{
	struct npc_data *nd;
	int flag = 0;

	if( script_hasdata(st,3) ) { //Two arguments: NPC name and attach flag.
		nd = npc_name2id(script_getstr(st,2));
		flag = script_getnum(st,3);
	} else if( script_hasdata(st,2) ) { //Check if argument is numeric (flag) or string (npc name)
		struct script_data *data;
		data = script_getdata(st,2);
		get_val(st,data);
		if( data_isstring(data) ) //NPC name
			nd = npc_name2id(conv_str(st,data));
		else if( data_isint(data) ) { //Flag
			nd = (struct npc_data *)map_id2bl(st->oid);
			flag = conv_num(st,data);
		} else {
			ShowError("initnpctimer: invalid argument type #1 (needs be int or string)).\n");
			return 1;
		}
	} else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( !nd )
		return 0;
	if( flag ) { //Attach
		TBL_PC* sd = script_rid2sd(st);
		if( sd == NULL )
			return 0;
		nd->u.scr.rid = sd->bl.id;
	}

	npc_timerevent_start(nd,st->rid);
	return SCRIPT_CMD_SUCCESS;
}
/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(stopnpctimer)
{
	struct npc_data *nd;
	int flag = 0;

	if( script_hasdata(st,3) ) { //Two arguments: NPC name and attach flag.
		nd = npc_name2id(script_getstr(st,2));
		flag = script_getnum(st,3);
	} else if( script_hasdata(st,2) ) { //Check if argument is numeric (flag) or string (npc name)
		struct script_data *data;
		data = script_getdata(st,2);
		get_val(st,data);
		if( data_isstring(data) ) //NPC name
			nd = npc_name2id(conv_str(st,data));
		else if( data_isint(data) ) { //Flag
			nd = (struct npc_data *)map_id2bl(st->oid);
			flag = conv_num(st,data);
		} else {
			ShowError("initnpctimer: invalid argument type #1 (needs be int or string)).\n");
			return 1;
		}
	} else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( !nd )
		return 0;
	if( flag ) //Detach
		nd->u.scr.rid = 0;

	npc_timerevent_stop(nd);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(getnpctimer)
{
	struct npc_data *nd;
	TBL_PC *sd;
	int type = script_getnum(st,2);
	int val = 0;

	if( script_hasdata(st,3) )
		nd = npc_name2id(script_getstr(st,3));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);
	
	if( !nd || nd->bl.type != BL_NPC ) {
		script_pushint(st,0);
		ShowError("getnpctimer: Invalid NPC.\n");
		return 1;
	}

	switch( type ) {
		case 0: val = npc_gettimerevent_tick(nd); break;
		case 1:
			if( nd->u.scr.rid ) {
				sd = map_id2sd(nd->u.scr.rid);
				if( !sd ) {
					ShowError("buildin_getnpctimer: Attached player not found!\n");
					break;
				}
				val = (sd->npc_timer_id != INVALID_TIMER);
			} else
				val = (nd->u.scr.timerid != INVALID_TIMER);
			break;
		case 2: val = nd->u.scr.timeramount; break;
	}

	script_pushint(st,val);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(setnpctimer)
{
	int tick;
	struct npc_data *nd;

	tick = script_getnum(st,2);
	if( script_hasdata(st,3) )
		nd = npc_name2id(script_getstr(st,3));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( !nd || nd->bl.type != BL_NPC ) {
		script_pushint(st,1);
		ShowError("setnpctimer: Invalid NPC.\n");
		return 1;
	}

	npc_settimerevent_tick(nd,tick);
	script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * attaches the player rid to the timer [Celest]
 *------------------------------------------*/
BUILDIN_FUNC(attachnpctimer)
{
	TBL_PC *sd;
	struct npc_data *nd = (struct npc_data *)map_id2bl(st->oid);

	if( !nd || nd->bl.type != BL_NPC ) {
		script_pushint(st,1);
		ShowError("setnpctimer: Invalid NPC.\n");
		return 1;
	}

	if( script_hasdata(st,2) )
		sd = map_nick2sd(script_getstr(st,2));
	else
		sd = script_rid2sd(st);

	if( !sd ) {
		script_pushint(st,1);
		ShowWarning("attachnpctimer: Invalid player.\n");
		return 1;
	}

	nd->u.scr.rid = sd->bl.id;
	script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * detaches a player rid from the timer [Celest]
 *------------------------------------------*/
BUILDIN_FUNC(detachnpctimer)
{
	struct npc_data *nd;

	if( script_hasdata(st,2) )
		nd = npc_name2id(script_getstr(st,2));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( !nd || nd->bl.type != BL_NPC ) {
		script_pushint(st,1);
		ShowError("detachnpctimer: Invalid NPC.\n");
		return 1;
	}

	nd->u.scr.rid = 0;
	script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * To avoid "player not attached" script errors, this function is provided,
 * it checks if there is a player attached to the current script. [Skotlex]
 * If no, returns 0, if yes, returns the account_id of the attached player.
 *------------------------------------------*/
BUILDIN_FUNC(playerattached)
{
	if(st->rid == 0 || map_id2sd(st->rid) == NULL)
		script_pushint(st,0);
	else
		script_pushint(st,st->rid);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(announce)
{
	const char *mes       = script_getstr(st,2);
	int         flag      = script_getnum(st,3);
	const char *fontColor = script_hasdata(st,4) ? script_getstr(st,4) : NULL;
	int         fontType  = script_hasdata(st,5) ? script_getnum(st,5) : 0x190; // default fontType (FW_NORMAL)
	int         fontSize  = script_hasdata(st,6) ? script_getnum(st,6) : 12;    // default fontSize
	int         fontAlign = script_hasdata(st,7) ? script_getnum(st,7) : 0;     // default fontAlign
	int         fontY     = script_hasdata(st,8) ? script_getnum(st,8) : 0;     // default fontY
	
	if (flag&(BC_TARGET_MASK|BC_SOURCE_MASK)) { // Broadcast source or broadcast region defined
		send_target target;
		// If bc_npc flag is set, use NPC as broadcast source
		struct block_list *bl = (flag&BC_NPC) ? map_id2bl(st->oid) : (struct block_list *)script_rid2sd(st);
		if (bl == NULL)
			return 0;

		switch (flag&BC_TARGET_MASK) {
			case BC_MAP:  target = ALL_SAMEMAP; break;
			case BC_AREA: target = AREA;        break;
			case BC_SELF: target = SELF;        break;
			default:      target = ALL_CLIENT;  break; // BC_ALL
		}

		if (fontColor)
			clif_broadcast2(bl, mes, (int)strlen(mes) + 1, strtol(fontColor, (char **)NULL, 0), fontType, fontSize, fontAlign, fontY, target);
		else
			clif_broadcast(bl, mes, (int)strlen(mes) + 1, flag&BC_COLOR_MASK, target);
	} else {
		if (fontColor)
			intif_broadcast2(mes, (int)strlen(mes) + 1, strtol(fontColor, (char **)NULL, 0), fontType, fontSize, fontAlign, fontY);
		else
			intif_broadcast(mes, (int)strlen(mes) + 1, flag&BC_COLOR_MASK);
	}
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
static int buildin_announce_sub(struct block_list *bl, va_list ap)
{
	char *mes       = va_arg(ap, char *);
	int   len       = va_arg(ap, int);
	int   type      = va_arg(ap, int);
	char *fontColor = va_arg(ap, char *);
	short fontType  = (short)va_arg(ap, int);
	short fontSize  = (short)va_arg(ap, int);
	short fontAlign = (short)va_arg(ap, int);
	short fontY     = (short)va_arg(ap, int);
	if (fontColor)
		clif_broadcast2(bl, mes, len, strtol(fontColor, (char **)NULL, 0), fontType, fontSize, fontAlign, fontY, SELF);
	else
		clif_broadcast(bl, mes, len, type, SELF);
	return 0;
}

BUILDIN_FUNC(mapannounce)
{
	const char *mapname   = script_getstr(st,2);
	const char *mes       = script_getstr(st,3);
	int         flag      = script_getnum(st,4);
	const char *fontColor = script_hasdata(st,5) ? script_getstr(st,5) : NULL;
	int         fontType  = script_hasdata(st,6) ? script_getnum(st,6) : 0x190; // default fontType (FW_NORMAL)
	int         fontSize  = script_hasdata(st,7) ? script_getnum(st,7) : 12;    // default fontSize
	int         fontAlign = script_hasdata(st,8) ? script_getnum(st,8) : 0;     // default fontAlign
	int         fontY     = script_hasdata(st,9) ? script_getnum(st,9) : 0;     // default fontY
	int16 m;

	if ((m = map_mapname2mapid(mapname)) < 0)
		return 0;

	map_foreachinmap(buildin_announce_sub, m, BL_PC,
			mes, strlen(mes) + 1, flag&BC_COLOR_MASK, fontColor, fontType, fontSize, fontAlign, fontY);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(areaannounce)
{
	const char *mapname   = script_getstr(st,2);
	int         x0        = script_getnum(st,3);
	int         y0        = script_getnum(st,4);
	int         x1        = script_getnum(st,5);
	int         y1        = script_getnum(st,6);
	const char *mes       = script_getstr(st,7);
	int         flag      = script_getnum(st,8);
	const char *fontColor = script_hasdata(st,9) ? script_getstr(st,9) : NULL;
	int         fontType  = script_hasdata(st,10) ? script_getnum(st,10) : 0x190; // default fontType (FW_NORMAL)
	int         fontSize  = script_hasdata(st,11) ? script_getnum(st,11) : 12;    // default fontSize
	int         fontAlign = script_hasdata(st,12) ? script_getnum(st,12) : 0;     // default fontAlign
	int         fontY     = script_hasdata(st,13) ? script_getnum(st,13) : 0;     // default fontY
	int16 m;

	if ((m = map_mapname2mapid(mapname)) < 0)
		return 0;

	map_foreachinarea(buildin_announce_sub, m, x0, y0, x1, y1, BL_PC,
		mes, strlen(mes) + 1, flag&BC_COLOR_MASK, fontColor, fontType, fontSize, fontAlign, fontY);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(getusers)
{
	int flag, val = 0;
	struct map_session_data* sd;
	struct block_list* bl = NULL;

	flag = script_getnum(st,2);

	switch (flag&0x07) {
		case 0:
			if (flag&0x8) // NPC
				bl = map_id2bl(st->oid);
			else if ((sd = script_rid2sd(st)) != NULL) // PC
				bl = &sd->bl;

			if (bl)
				val = map[bl->m].users;
			break;
		case 1:
			val = map_getusers();
			break;
		default:
			ShowWarning("buildin_getusers: Unknown type %d.\n", flag);
			script_pushint(st,0);
			return 1;
	}

	script_pushint(st,val);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Works like @WHO - displays all online users names in window
 *------------------------------------------*/
BUILDIN_FUNC(getusersname)
{
	TBL_PC *sd, *pl_sd;
	int /*disp_num = 1,*/ group_level = 0;
	struct s_mapiterator* iter;

	sd = script_rid2sd(st);
	if (!sd)
		return 0;

	group_level = pc_get_group_level(sd);
	iter = mapit_getallusers();
	for (pl_sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); pl_sd = (TBL_PC*)mapit_next(iter)) {
		if (pc_has_permission(pl_sd, PC_PERM_HIDE_SESSION) && pc_get_group_level(pl_sd) > group_level)
			continue; // Skip hidden sessions

		/* Temporary fix for bugreport:1023.
		 * Do not uncomment unless you want thousands of 'next' buttons.
		if ((disp_num++)%10 == 0)
			clif_scriptnext(sd,st->oid);*/
		clif_scriptmes(sd,st->oid,pl_sd->status.name);
	}

	mapit_free(iter;
	sd=scripSCRIPT_CMD_SUCCESS_rid2sd(st);
	pc_resetskill(sd,1);
	return 0;
}

/*==getmapguildusers("mapname",guild ID) Returns the number guild members present on a map [Reddozen]
 *------------------------------------------*/
BUILDIN_FUNC(getmapguildusers)
{
	const char *str;
	int16 m;
	int gid;
	int c = 0;
	struct guild *g = NULL;

	str = script_getstr(st,2);
	gid = script_getnum(st,3);
	if ((m = map_mapname2mapid(str)) < 0) { // Map id on this server (m == -1 if not in actual map-server)
		script_pushint(st,-1);
		return 0;
	}
	g = guild_search(gid);

	if (g) {
		unsigned short i;

		for (i = 0; i < g->max_member; i++) {
			if (g->member[i].sd && g->member[i].sd->bl.m == m)
				c++;
		}
	}

	script_pushint(st,c);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(getmapusers)
{
	const char *str;
	int16 m;

	str = script_getstr(st,2);
	if ((m = map_mapname2mapid(str)) < 0) {
		script_pushint(st,-1);
		return 0;
	}
	script_pushint(st,map[m].users);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
static int buildin_getareausers_sub(struct block_list *bl,va_list ap)
{
	int *users = va_arg(ap,int *);

	(*users)++;
	return 0;
}

BUILDIN_FUNC(getareausers)
{
	const char *str;
	int16 m, x0, y0, x1, y1, users = 0; // Doubt we can have more then 32k users on

	str = script_getstr(st,2);
	x0 = script_getnum(st,3);
	y0 = script_getnum(st,4);
	x1 = script_getnum(st,5);
	y1 = script_getnum(st,6);
	if ((m = map_mapname2mapid(str)) < 0) {
		script_pushint(st,-1);
		return 0;
	}
	map_foreachinarea(buildin_getareausers_sub,m,x0,y0,x1,y1,BL_PC,&users);
	script_pushint(st,users);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
static int buildin_getareadropitem_sub(struct block_list *bl,va_list ap)
{
	int nameid = va_arg(ap,int);
	int *amount = va_arg(ap,int *);
	struct flooritem_data *drop = (struct flooritem_data *)bl;

	if (drop->item.nameid == nameid)
		(*amount) += drop->item.amount;
	return 0;
}

BUILDIN_FUNC(getareadropitem)
{
	const char *str;
	int16 m, x0, y0, x1, y1;
	int nameid, amount = 0;
	struct script_data *data;

	str = script_getstr(st,2);
	x0 = script_getnum(st,3);
	y0 = script_getnum(st,4);
	x1 = script_getnum(st,5);
	y1 = script_getnum(st,6);

	data = script_getdata(st,7);
	get_val(st,data);
	if (data_isstring(data)) {
		const char *name = conv_str(st,data);
		struct item_data *item_data = itemdb_searchname(name);

		nameid = UNKNOWN_ITEM_ID;
		if (item_data)
			nameid = item_data->nameid;
	} else
		nameid = conv_num(st,data);

	if ((m = map_mapname2mapid(str)) < 0) {
		script_pushint(st,-1);
		return 0;
	}
	map_foreachinarea(buildin_getareadropitem_sub,m,x0,y0,x1,y1,BL_ITEM,nameid,&amount);
	script_pushint(st,amount);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(enablenpc)
{
	const char *str = script_getstr(st,2);

	npc_enable(strsd;
	sd=scripSCRIPT_CMD_SUCCESS_rid2sd(st);
	pc_resetskill(sd,1);
	return 0;
}

/*=------------------------------------------*/
BUILDIN_FUNC(disablenpc)
{
	const char *str = script_getstr(st,2);

	npc_enable(str,0;
	sd=scripSCRIPT_CMD_SUCCESS_rid2sd(st);
	pc_resetskill(sd,1);
	return 0;
}

/*=------------------------------------------*/
BUILDIN_FUNC(hideoffnpc)
{
	const char *str = script_getstr(st,2);

	npc_enable(str,2);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(hideonnpc)
{
	const char *str = script_getstr(st,2);

	npc_enable(str,4);
	return SCRIPT_CMD_SUCCESS;
}

/* Starts a status effect on the target unit or on the attached player.
 *
 * sc_start  <effect_id>,<duration>,<val1>{,<rate>,<flag>,{<unit_id>}};
 * sc_start2 <effect_id>,<duration>,<val1>,<val2>{,<rate,<flag>,{<unit_id>}};
 * sc_start4 <effect_id>,<duration>,<val1>,<val2>,<val3>,<val4>{,<rate,<flag>,{<unit_id>}};
 * <flag>: @see enum scstart_flag
 */
BUILDIN_FUNC(sc_start)
{
	TBL_NPC * nd = map_id2nd(st->oid);
	struct block_list* bl;
	enum sc_type type;
	int tick, val1, val2, val3, val4 = 0, rate, flag;
	char start_type;
	const char* command = script_getfuncname(st);

	if(strstr(command, "4"))
		start_type = 4;
	else if(strstr(command, "2"))
		start_type = 2;
	else
		start_type = 1;

	type = (sc_type)script_getnum(st,2);
	tick = script_getnum(st,3);
	val1 = script_getnum(st,4);

	//If from NPC we make default flag SCFLAG_NOAVOID to be unavoidable
	if(nd && nd->bl.id == fake_nd->bl.id)
		flag = script_hasdata(st,5 + start_type) ? script_getnum(st,5 + start_type) : SCFLAG_FIXEDTICK;
	else
		flag = script_hasdata(st,5 + start_type) ? script_getnum(st,5 + start_type) : SCFLAG_NOAVOID;

	rate = script_hasdata(st,4 + start_type) ? min(script_getnum(st,4 + start_type),10000) : 10000;

	if(script_hasdata(st,(6 + start_type)))
		bl = map_id2bl(script_getnum(st,(6 + start_type)));
	else
		bl = map_id2bl(st->rid);

	//When there isn't a duration specified, try to get it from the skill_db
	if(tick == 0 && val1 > 0 && type > SC_NONE && type < SC_MAX && status_sc2skill(type) != 0)
		tick = skill_get_time(status_sc2skill(type), val1);

	if(potion_flag == 1 && potion_target) { //skill.c set the flags before running the script, this is a potion-pitched effect.
		bl = map_id2bl(potion_target);
		tick /= 2; //Thrown potions only last half.
		val4 = 1; //Mark that this was a thrown sc_effect
	}

	if(!bl)
		return 0;

	switch(start_type) {
		case 1:
			status_change_start(bl, bl, type, rate, val1, 0, 0, val4, tick, flag);
			break;
		case 2:
			val2 = script_getnum(st,5);
			status_change_start(bl, bl, type, rate, val1, val2, 0, val4, tick, flag);
			break;
		case 4:
			val2 = script_getnum(st,5);
			val3 = script_getnum(st,6);
			val4 = script_getnum(st,7);
			status_change_start(bl, bl, type, rate, val1, val2, val3, val4, tick, flag);
			break;
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Ends one or all status effects on the target unit or on the attached player.
///
/// sc_end <effect_id>{,<unit_id>};
BUILDIN_FUNC(sc_end)
{
	struct block_list* bl;
	int type;

	type = script_getnum(st, 2);
	if( script_hasdata(st, 3) )
		bl = map_id2bl(script_getnum(st, 3));
	else
		bl = map_id2bl(st->rid);

	if( potion_flag == 1 && potion_target ) //@TODO: How does this work [FlavioJS]
		bl = map_id2bl(potion_target);

	if( !bl )
		return 0;

	if( type >= 0 && type < SC_MAX ) {
		struct status_change *sc = status_get_sc(bl);
		struct status_change_entry *sce = sc ? sc->data[type] : NULL;

		if( !sce )
			return 0;

		switch( type ) {
			case SC_WEIGHT50:
			case SC_WEIGHT90:
			case SC_NOCHAT:
			case SC_PUSH_CART:
			case SC_ALL_RIDING:
			case SC_STYLE_CHANGE:
			case SC_MONSTER_TRANSFORM:
			case SC_MOONSTAR:
			case SC_SUPER_STAR:
			case SC_MTF_ASPD:
			case SC_MTF_RANGEATK:
			case SC_MTF_MATK:
			case SCcase SC_MTF_ASPD2:
			case SC_MTF_RANGEATK2:
			case SC_MTF_MATK2se SC_MTF_MLEATKED:
			case SC_MTF_CRIDAMAGE:
				return 0;

			default:
				break;
		}
		//This should help status_change_end force disabling the SC in case it has no limit.
		sce->val1 = sce->val2 = sce->val3 = sce->val4 = 0;
		status_change_end(bl, (sc_type)type, INVALID_TIMER);
	} else
		status_changeSCRIPT_CMD_SUCCESSclear(bl, 2); //Remove all effects
	return 0;
}

/*==========================================
 * �@FIXME atm will return reduced tick, 0 immune, 1 no tick
 *------------------------------------------*/
BUILDIN_FUNC(getscrate)
{
	struct block_list *bl;
	int type,rate;

	type = script_getnum(st,2);
	rate = script_getnum(st,3);
	if( script_hasdata(st,4) ) //Get for the bl assigned
		bl = map_id2bl(script_getnum(st,4));
	else
		bl = map_id2bl(st->rid);

	if( bl )
		rate = status_gSCFLAG_NONE);

	script_pushint(st,rate;
	}
	mapitSCRIPT_CMD_SUCCESSfree(iter);
	script_pushint(st,1);
	return 0;
}

/*==========================================
 * getstatus <type>{, <info>};
 *------------------------------------------*/
BUILDIN_FUNC(getstatus)
{
	int id, type;
	struct map_session_data* sd = script_rid2sd(st);

	if( sd == NULL ) { //No player attached
		return 0;
	}

	id = script_getnum(st,2);
	type = script_hasdata(st,3) ? script_getnum(st,3) : 0;

	if( id <= SC_NONE || id >= SC_MAX ) { //Invalid status type given
		ShowWarning("script.c:getstatus: Invalid status type given (%d).\n",id);
		return 0;
	}

	if( sd->sc.count == 0 || !sd->sc.data[id] ) { //No status is active
		script_pushint(st, 0);
		return 0;
	}
	
	switch( type ) {
		case 1:	 script_pushint(st, sd->sc.data[id]->val1);	break;
		case 2:  script_pushint(st, sd->sc.data[id]->val2);	break;
		case 3:  script_pushint(st, sd->sc.data[id]->val3);	break;
		case 4:  script_pushint(st, sd->sc.data[id]->val4);	break;
		case 5:
			{
				struct TimerData* timer = (struct TimerData*)get_timer(sd->sc.data[id]->timer);

				if( timer ) { //Return the amount of time remaining
					script_pushint(st, timer->tick - gettick());
				}
			}
			break;
		defaulSCRIPT_CMD_SUCCESSkill_point + pc_resetskill(sd,2));
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(debugmes)
{
	const char *str;
	str = script_getstr(st,2);
	ShowDebug("script debug : SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *------------------------------------------*/
BUILDIN_FUNC(----------------------------*/
BUILDIN_FUNC(catchpet)
{
	int pet_id;
	TBL_PC *sd;

	pet_id = script_getnum(st,2);
	sd = script_rid2sd(st);
	if( sd == NULL )
		return SCRIPT_CMD_SUCCESS;

	pet_catch_process1(sd,pet_id);
	return 0;
}

/*==========================================
 * [orn]
 *------------------------------------------*/
BUILDIN_FUNC(homunculus_evolution)
{
	TBL_PC *sd;

	sd = script_rid2hom_is_active(sd->hd) ) {
		if( sd->hd->homunculus.intimacy > 91000 )
			->hd->homunculus.intimacy > 91000 )
			merc_hom_evolution(sd->hd);
		else
			SCRIPT_CMD_SUCCESSlif_emotion(&sd->hd->bl,E_SWT);
	}
	return 0;
}

/*==========================================
 * Checks for vaporized morph state
 * and deletes ITEMID_STRANGE_EMBRYO.
 *------------------------------------------*/
BUI{
	struct item item_tmp;
	int m_class, i = 0;
	TBL_PC *sd;

	sd = script_rid2sd(st);
	if( sd == NULL || sd->hd == NULL )
		return 0;

	if( script_hasdata(st, 2) )
		homun_id = script_get%(st, 2);
	else
		homun_id = 6048 + (rnd() % 4);

	if( sd->hdint m_class = hom_class2mapid(sd->hd->homunculus.class_);
		int m_id = hom_class2mapid(homun_id);
		short 		m_id    = hom_class2mapid(homun_id);
		
		i = pc_search_inventory(sd, ITEMID_STRANGE_EMBRYO);

		if( m_
			(m_class&HOM_EVO) && (m_id&HOM_S) && sd->hd->homunculus.level >= 99 && i != INDEX_NOT_FOUND && sd->hd->homunculus.level >= 99 && i >= 0 ) {
			sd->hd->homunculus.vahom_call; // Remove morph st
			merc_call_homunculus(sd); // Respawn homunculus.
			hom_mutate(sd->hd, homun_id);
			pc_delitem(sd, i, 1, 0, 0, LOG_TYPE_SCRIPT);
			script_pushint(st, 1);
			return 0;
			
		} else
			clif_emotion(&sd->hd->bl, E_SWT);
	} else
		clif_emotion(&sd->blSCRIPT_CMD_SUCCESS E_SWT);

	script_pushint(st, 0);

	return 0;
}

/*==========================================
 * Puts homunculus into morph state
 * and gives ITEMID_STRANGE_EMBRYO.
 *------------------------------------------*/
BUILDIN_struct item item_tmp;
	int m_class, i = 0;
	TBL_PC *sd;

	sd = script_rid2sd(st);
	if( sd ==hom_is->hd == NULL )
		returint n 0;

	if( merc_is_hom_active(sd->hd) ) {
		m_class = hom_class2mapid(sd->hd->homunculus.class_);

		if( m_class != HT_INVALID && m_class&HOM_Echar i = 0;
OM_EVO && sd->hd->homunculus.level >= 99 ) {
			memset(&item_tmp, 0, sizeof(item_tmp));
			item_tmp.nameid = ITEMID_STRANGE_EMBRYO;
			item_tmp.identify = 1;

			if( item_tmp.nameid == 0 || (i = pc_additem(sd, &item_tmp, 1, LOG_TYPE_SCRIPT)) ) {
				clif_additem(sd, 0, 0, i);
				clif_emotion(&sd->bl, E_SWT); // Fail oid item drop exploit.
			} else {
				merc_hom_vaporize(sd, HOM_ST_MORPH);
				script_pushint(st, 1);
				return 0;
			}
		} else
			clif_emotion(&sd->hd->bl, E_SWT);
	} else
		clif_emotion(&sd->blSCRIPT_CMD_SUCCESS E_SWT);

	script_pushint(st, 0);

	return 0;
}

// [Zephyrus]
BUILDIN_FUNC(homunculus_shuffle)
{
	TBL_PC *sd;

	sd=script_ridhom_is_active(sd->hd))
		 0;

	if(merc_is_hom_active(sdSCRIPT_CMD_SUCCESS>hd))
		merc_hom_shuffle(sd->hd);

	return 0;
}

/*==========================================
 * Check for homunculus state.
 * Return:
 *	-1 = No homunculus
 *	 0 = Homunculus is active
 *	 1 = Homunculus is vaporized (rest)
 *	 2 = Homunculus is in morph state
 *------------------------------------------*/
BUILDIN_FUNC(checkhomcall)
{
	TBL_PC *sd = script_rid2sd(st);
	TBL_HOM *hd;

	f( sd == NULL )
		return 0;

	hd = sd->hd;

	if( !hd )
		script_pushint(st, -1);
	else
		script_pusSCRIPT_CMD_SUCCESSint(st, hd->homunculus.vaporize);

	return 0;
}

//These two functions bring the eA MAPID_* class functionality to sc
	if( script_hasdata(st,2) )
		class_ = script_getnum(st,2);
	else {
		TBL_PC *sd;

		sd = pt_getnum(st,2);
	else ( !sd )
			return 0;{
			script_pushint(st,-1);
			return 0;
		}
		class_ = sd->status.class_;
	}
	script_SCRIPT_CMD_SUCCESSushint(st,pc_jobid2mapid(class_));
	return script_getnum(st,2);
	int sex;

	int class_ =script_getnum(st,2);
	int sex;
	if( script_hasdata(st,3) )
		sex =
		if (st->rid && (sd = 
	else {
		TBL_PC *sd;
		if (st->rid && (sd=script_rid2sd(st)))
			sex = sd->status.sex;
		else
			sex = 1; //Just use male when not found.
sex)sd;
	sd=scrSCRIPT_CMD_SUCCESSpt_rid2sd(st);
	pc_resetstate(sd);
	return 0;
}

/*==========================================
 * Tells client to open a hatching window, used for pet incubator
 *---------------------------------------
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	if( sd->status.pet_id ) //Do not send egg list, when you already have a pet
		return 0;

	clif_sendeggC *sd;
	sd=scrSCRIPT_CMD_SUCCESSpt_rid2sd(st);
	pc_resetstate(sd);
	return 0;
}

/*==========================================
 * Added - AppleGirl For Advanced Classes, (Updated for Cleaner Script Purposes)
 * @type
 *	1 : make like after rebirth
 *	2 : blvl,jlvl=1, skillpoint=0
 * 	3 : don't reset skill, blvl=1
 *	4 : jlvl=0
 *---------------------------------------	int type = script_getnum(st,2);

	sd = C *sd;

	int type=script_getnum(st,2);

	sd=script_rid2sd(st);
	if( sd == NULL )
SCRIPT_CMD_SUCCESS;
}
turn 0;

	pc_resetlvl(sd,type);
	return 0;
}
/*==========================================
 * Reset a player status point
 *------------------------------------------
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1;

	pc_resetstate(sdpt debug : SCRIPT_CMD_SUCCESS;
}

/*==========================================
 *=========================================
 * script command resetskill
 *-----------------------------------------
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1;
skill)
{
	TBL_PC *sd;
	sd=scripSCRIPT_CMD_SUCCESS_rid2sd(st);
	pc_resetskill(sd,1);
	return 0;
}

/*==========================================
 * Counts total amount of skill points.
 *------------------------------------------*/
B
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1tcount)
{
	TBL_PC *sd;
	sd=script_rid2sd(st);
	script_pushint(st,sd->status.SCRIPT_CMD_SUCCESSkill_point + pc_resetskill(sd,2));
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(changebase)
{
	TBL_PC *sd=NULL;
	int vclass;

	if( script_hasdata(st,3) )
		sd=map_id2sd(script_getnum(st,3));
	else
	sd=script_rid2sd(st);

	if(sd == NULL)
		return 0;

	vclass = scr t_getnum(st,2);
	if(vclass == JOB_WEDDING)
	{
		if (!battle_config.wedding_modifydisplay || //Do not show) //Baby classes screw up when showing wedding sprites. [Skotlex] They don't seem to anymore.. [Skotlex] They don't seem to anymore.
			)
		return 0;
	}

	if(!sd->disguise && vclass != sd->vd.class_) {
		status_set_viewdata(&sd->bl, vclass);
		//Updated client view. Base, Weapon and Cloth Colors.
		clif_changelook(&sd->bl,LOOK_BASE,sd->vd.class_);
		clif_changelook(&sd->bl,LOOK_WEAPON,sd->status.weapon);
		if (sd->vd.cloth_color)
			clif_changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_colSCRIPT_CMD_SUCCESSr);
		clif_skillinfoblock(sd);
	}

	return 0;
}

/*==========================================
 * �Unequip all item and request for a changesex to char-serv
 *------------------------------------------*/
BUILDIN_FU
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1;

	pc_resetskill(sd,4);
	//To avoid any problem with equipment and invalid sex, equipment is unequiped.
	for( i = 0; i < d sex, equipment is unequiped.
	for( i=0; i<EQ
			_MAX; i++ )
		if( sd->equip_index[i] >= 0 ) pc_unequipitem(sd, sd->equip_SCRIPT_CMD_SUCCESSndex[i], 3);
	chrif_changesex(sd);
	return 0;
}

/*==========================================
 * Works like 'announce' but outputs in the common chat window
 *------------------------------------------*/
BUILDIN_FUNC(globalmes)
{
	struct block_list *bl = map_id2bl(st->oid);
	struct npc_data *n = NULL, *mes;

	mes = script_getstr(st,2);
	if( mes == NULL )
		return 0;

	if( script_hasdata(st,3) ) //NPC name to display
		name = script_getstr(st,3);
	else
		name = nd->name; //Use current npc name

	npc_globalmessage(name,mes); //B name
	}

	npc_globalmessage(name,mes);	// SCRIPT_CMD_SUCCESSadd_str("$@warpwaitingpcnum"), i);
	return 0;
}

/////////////////////////////////////////////////////////////////////
// NPC waiting room (chat room)
//

/// Creates a waiting room (chat room) for this npc.
///
/// waitingroom "<title>",<limit>{,"<event>"{,<trigger>{,<zeny>{,<minlvl>{,<maxlvl>}}}}};
BUILDINoom)
{
	struct npc_data* nd;
	int pub = 1;
	const char* title = script_getstr(st, 2);
	int limit = script_getnum(st, 3);
	const char* ev = script_hasdata(st,4) ? script_getstr(st,4) : "";
	int trigger =  script_hasdata(st,5) ? script_getnum(st,5) : limit;
	int zeny =  script_hasdata(st,6) ? script_getnum(st,6) : 0;
	int minLvl =  script_hasdata(st,7) ? script_getnum(st,7) : 1;
	int maxLvl =  script_hasdata(st,8) ? script_getnum(st,8) : MAX_LEVEL;

	nd = (struct npc_data *)map_id2bl(st->oid);
	if( nd != NU1)
		chat_createnpcchat(nd, title, limit, pub, tSCRIPT_CMD_SUCCESSigger, ev, zeny, minLvl, maxLvl);

	return 0;
}

/// Removes the waiting room of the current or target npc.
///
/// delwaitingroom "<npc_name>";
/// delwaitingroom;
BUILDIN_FUNC(delwaitingroom)
{
	struct npc_data* nd;
	if( script_hasdata(st,2) )
		nd = npc_name2id(script_getstr(st, 2));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);
	if( nd SCRIPT_CMD_SUCCESS= NULL )
		chat_deletenpcchat(nd);
	return 0;
}

/// Kicks all the players from the waiting room of the current or target npc.
///
/// kickwaitingroomall "<npc_name>";
/// kickwaitingroomall;
BUILDIN_FUNC(waitingroomkickall)
{
	struct npc_data* nd;
	struct chat_data* cd;

	if( script_hasdata(st,2) )
		nd = npc_name2id(script_getstr(st,2));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( nd != NULL && (cd=(struct chat_data *)map_id2bl(nd->chat_idSCRIPT_CMD_SUCCESS) != NULL )
		chat_npckickall(cd);
	return 0;
}

/// Enables the waiting room event of the current or target npc.
///
/// enablewaitingroomevent "<npc_name>";
/// enablewaitingroomevent;
BUILDIN_FUNC(enablewaitingroomevent)
{
	struct npc_data* nd;
	struct chat_data* cd;

	if( script_hasdata(st,2) )
		nd = npc_name2id(script_getstr(st, 2));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( nd != NULL && (cd=(struct chat_data *)map_id2bl(nd->chat_id)SCRIPT_CMD_SUCCESS != NULL )
		chat_enableevent(cd);
	return 0;
}

/// Disables the waiting room event of the current or target npc.
///
/// disablewaitingroomevent "<npc_name>";
/// disablewaitingroomevent;
BUILDIN_FUNC(disablewaitingroomevent)
{
	struct npc_data *nd;
	struct chat_data *cd;

	if( script_hasdata(st,2) )
		nd = npc_name2id(script_getstr(st, 2));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( nd != NULL && (cd=(struct chat_data *)map_id2bl(nd->chat_id))SCRIPT_CMD_SUCCESS!= NULL )
		chat_disableevent(cd);
	return 0;
}

/// Returns info on the waiting room of the current or target npc.
/// Returns -1 if the type unknown
/// <type>=0 : current number of users
/// <type>=1 : maximum number of users allowed
/// <type>=2 : the number of users that trigger the event
/// <type>=3 : if the trigger is disabled
/// <type>=4 : the title of the waiting room
/// <type>=5 : the password of the waiting room
/// <type>=16 : the name of the waiting room event
/// <type>=32 : if the waiting room is full
/// <type>=33 : if there are enough users to trigger the event
///
/// getwaitingroomstate(<type>,"<npc_name>") -> <info>
/// getwaitingroomstate(<type>) -> <info>
BUILDIN_FUNC(getwaitingroomstate)
{
	struct npc_data *nd;
	struct chat_data *cd;
	int type;

	type = script_getnum(st,2);
	if( script_hasdata(st,3) )
		nd = npc_name2id(script_getstr(st, 3));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( nd == NULL || (cd=(struct chat_data *)map_id2bl(nd->chat_id)) == NULL )
	{
		script_pushint(st, -1);
		return 0;
	}

	switch(type)
	{
	case 0:  script_pushint(st, cd->users); break;
	case 1:  script_pushint(st, cd->limit); break;
	case 2:  script_pushint(st, cd->trigger&0x7f); break;
	case 3:  script_pushint(st, ((cd->trigger&0x80)!=0)); break;
	case 4:  script_pushstrcopy(st, cd->title); break;
	case 5:  script_pushstrcopy(st, cd->pass); break;
	case 16: script_pushstrcopy(st, cd->npc_event);break;
	case 32: script_pushint(st, (cd->users >= cd->limit)); break;
	case 33: script_pushint(st, (cd->users >= cd->trigger)); break;
	defaultSCRIPT_CMD_SUCCESS script_pushint(st, -1); break;
	}
	return 0;
}

/// Warps the trigger or target amount of players to the target map and position.
/// Players are automatically removed from the waiting room.
/// Those waiting the longest will get warped first.
/// The target map can be "Random" for a random position in the current map,
/// and "SavePoint" for the savepoint map+position.
/// The map flag noteleport of the current map is only considered when teleporting to the savepoint.
///
/// The id's of the teleported players are put into the array $@warpwaitingpc[]
/// The total number of teleported players is put into $@warpwaitingpcnum
///
/// warpwaitingpc "<map name>",<x>,<y>,<number of players>;
/// warpwaitingpc "<map name>",<x>,<y>;
BUILDIN_FUNC(warpwaitingpc)
{
	int x;
	int y;
	int i;
	int n;
	const char* map_name;
	
	nd = (struct npc_data *)map_id2bl(st->oid);
	if( nd == NULL || (cd = )map_id2bl(st->oid);
	if( nd == NULL || (cd=(struct chat_data *)map_id2bl(nd->chat_id)) == NULL )
		return 0;

	map_name = script_getstr(st,2);
	x = script_getnum(st,3);
	y = script_getnum(st,4);
	n = cd->trigger&0x7f;

	if( script_hasdata(st,5) )
		n = script_getnum(st,5); {
		TBL_PC* sd = cd->usersd[0];

		if( strcmp(map_name, "SavePoint") == 0 && map[sd->bl.m].flag.noteleport ) // Can't teleport on this map
			break;

		if( cd->zeny ) { // Fee set
			if( (uint32)sd->status.zeny < cd->zeny ) // No zeny to cover set fee
				break;{// no zeny to cover set fee
				break;
			}
			pc_payzeny(sd, cd->zeny, LOG_TYPE_NPC, NULL);
		}

		mapreg_setreg(reference_uid(add_str("$@warpwaitingp "Random") == 0 )
			pc_randomwarp(sd, CLR_TELEPORT);
		else if( strcmp(map_name, d,CLR_TELEPORT);
		else if( strcmp(map_name,"SavePoint") == 0 )
			pc_setpos(sd, sd->status.save_point.map, sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
		else
			pc_setpos(sd, mapindex_name2id(map_name), x, y, CLR_OUTSIGHT);
	}
	mapreg_setregSCRIPT_CMD_SUCCESSadd_str("$@warpwaitingpcnum"), i);
	return 0;
}

/////////////////////////////////////////////////////////////////////
// ...
//

/// Detaches a character from a script.
///
/// @param st Script state to detach the character from.
static void script_detach_rid(struct script_state* st)
{
	if(st->rid) {
		script_detach_state(st, false);
		st->rid = 0;
	}
}

/*=========================================================================
 * Attaches a set of RIDs to the current script. [digitalhamster]
 * addrid(<type>{,<flag>{,<parameters>}});
 * <type>:
 *	0 : All players in the server.
 *	1 : All players in the map of the invoking player, or the invoking NPC if no player is attached.
 *	2 : Party members of a specified party ID.
 *	    [ Parameters: <party id> ]
 *	3 : Guild members of a specified guild ID.
 *	    [ Parameters: <guild id> ]
 *	4 : All players in a specified area of the map of the invoking player (or NPC).
 *	    [ Parameters: <x0>,<y0>,<x1>,<y1> ]
 *	Account ID: The specified account ID.
 * <flag>:
 *	0 : Players are always attached. (default)
 *	1 : Players currently running another script will not be attached.
 *-------------------------------------------------------------------------*/
static int buildin_addrid_sub(struct block_list *bl,va_list ap)
{
	int forceflag;
	struct map_session_data *sd = (TBL_PC *)bl;
	struct script_state* st;

	st = va_arg(ap,struct script_state*);
	forceflag = va_arg(ap,int);
	if(!forceflag || !sd->st)
		if(sd->status.account_id != st->rid)
			run_script(st->script,st->pos,sd->status.account_id,st->oid);
	return 0;
}

BUILDIN_FUNC(addrid)
{
	struct s_mapiterator* iter;
	struct block_list *bl;
	TBL_PC *sd;
	if(st->rid < 1) {
		st->state = END;
		bl = map_id2bl(st->oid);
	} else
		bl = map_id2bl(st->rid); //If run without rid it'd error,also oid if npc, else rid for map
	iter = mapit_getallusers();
	switch(script_getnum(st,2)) {
		case 0:
			for(sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter)) {
				if(!script_getnum(st,3) || !sd->st)
					if(sd->status.account_id != st->rid) //Attached player already runs.
						run_script(st->script,st->pos,sd->status.account_id,st->oid);
			}
			break;
		case 1:
			for(sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter)) {
				if(!script_getnum(st,3) || !sd->st)
					if((sd->bl.m == bl->m) && (sd->status.account_id != st->rid))
						run_script(st->script,st->pos,sd->status.account_id,st->oid);
			}
			break;
		case 2:
			if(script_getnum(st,4) == 0) {
				script_pushint(st,0);
				return 0;
			}
			for(sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter)) {
				if(!script_getnum(st,3) || !sd->st)
					//Attached player already runs.
					if((sd->status.account_id != st->rid) && (sd->status.party_id == script_getnum(st,4)))
						run_script(st->script,st->pos,sd->status.account_id,st->oid);
			}
			break;
		case 3:
			if(script_getnum(st,4) == 0) {
				script_pushint(st,0);
				return 0;
			}
			for(sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter)) {
				if(!script_getnum(st,3) || !sd->st)
					//Attached player already runs.
					if((sd->status.account_id != st->rid) && (sd->status.guild_id == script_getnum(st,4)))
						run_script(st->script,st->pos,sd->status.account_id,st->oid);
			}
			break;
		case 4:
			map_foreachinarea(buildin_addrid_sub,
			bl->m,script_getnum(st,4),script_getnum(st,5),script_getnum(st,6),script_getnum(st,7),BL_PC,
			st,script_getnum(st,3)); //4-x0 , 5-y0 , 6-x1, 7-y1
			break;
		default:
			if((map_id2sd(script_getnum(st,2))) == NULL) { //Player not online.
				script_pushint(st,0);
				return 0;
			}
			if(!script_getnum(st,3) || !map_id2sd(script_getnum(st,2))->st) {
				run_script(st->script,st->pos,script_getnum(st,2),st->oid);
				script_pushint(st,1);
			}
			return 0;
	}
	mapitSCRIPT_CMD_SUCCESSfree(iter);
	script_pushint(st,1);
	return 0;
}

/*==========================================
 * Attach sd char id to script and detach current one if any
 *------------------------------------------*/
BUILDIN_FUNC(attachrid)
{
	int rid = script_getnum(st,2);

	if (map_id2sd(rid) != NULL) {
		script_detach_rid(st);

		st->rid = rid;
		script_attach_state(st);
		script_pushint(st,SCRIPT_CMD_SUCCESS);
	} else
		script_pushint(st,0);
	return 0;
}
/*==========================================
 * Detach script to rid
 *------------------------------------------*/
BUILDIN_FUNC(dSCRIPT_CMD_SUCCESStachrid)
{
	script_detach_rid(st);
	return 0;
}
/*==========================================
 * Chk if account connected, (and charid from account if specified)
 *------------------------------------------*/
BUILDIN_FUNC(isloggedin)
{
	TBL_PC* sd = map_id2sd(script_getnum(st,2));
	if (script_hasdata(st,3) && sd &&
		sd->status.char_id != script_getnum(st,3))
		sd = NULL;
	SCRIPT_CMD_SUCCESS;
}
l_point + pc_resetskill(sd,2));
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
BUILDIN_FUNC(setmapflagnosave)
{
	int16 m,x,y;
	unsigned short mapindex;
	const char *str,*str2;

	str=script_getstr(st,2);
	str2=script_getstr(st,3);
	x=script_getnum(st,4);
	y=script_getnum(st,5);
	m = map_mapname2mapid(str);
	mapindex = mapindex_name2id(str2);
	
	i = 1;
		map[m].save.map = mapindex;
		map[m].save.x = x;
		map[m].save.y = apinde	return SCRIPT_CMD_SUCCESS].save.x=x;
		map[m].save.y=y;
	}

	return 0;
}

BUILDIN_FUNC(getmapflag)
{
	int16 m,i;
	const char *str;

	str = script_getstr(st,2);
	i = script_getnum(st,3);

	m = map_mapname2mapid(str);
	if(m >= 0) {
		switch(i) {
			case MF_NOMEMO:			script_pushint(st,map[m].flag.nomemo); break;
			case MF_NOTELEPORT:		script_pushint(st,map[m].flag.noteleport); break;
			case MF_NOSAVE:			script_pushint(st,map[m].flag.nosave); break;
			case MF_NOBRANCH:		script_pushint(st,map[m].flag.nobranch); break;
			case MF_NOPENALTY:		script_pushint(st,map[m].flag.noexppenalty); break;
			case MF_NOZENYPENALTY:		script_pushint(st,map[m].flag.nozenypenalty); break;
			case MF_PVP:			script_pushint(st,map[m].flag.pvp); break;
			case MF_PVP_NOPARTY:		script_pushint(st,map[m].flag.pvp_noparty); break;
			case MF_PVP_NOGUILD:		script_pushint(st,map[m].flag.pvp_noguild); break;
			case MF_GVG:			script_pushint(st,map[m].flag.gvg); break;
			case MF_GVG_NOPARTY:		script_pushint(st,map[m].flag.gvg_noparty); break;
			case MF_NOTRADE:		script_pushint(st,map[m].flag.notrade); break;
			case MF_NOSKILL:		script_pushint(st,map[m].flag.noskill); break;
			case MF_NOWARP:			script_pushint(st,map[m].flag.nowarp); break;
			case MF_PARTYLOCK:		script_pushint(st,map[m].flag.partylock); break;
			case MF_NOICEWALL:		script_pushint(st,map[m].flag.noicewall); break;
			case MF_SNOW:			script_pushint(st,map[m].flag.snow); break;
			case MF_FOG:			script_pushint(st,map[m].flag.fog); break;
			case MF_SAKURA:			script_pushint(st,map[m].flag.sakura); break;
			case MF_LEAVES:			script_pushint(st,map[m].flag.leaves); break;
			case MF_NOGO:			script_pushint(st,map[m].flag.nogo); break;
			case MF_CLOUDS:			script_pushint(st,map[m].flag.clouds); break;
			case MF_CLOUDS2:		script_pushint(st,map[m].flag.clouds2); break;
			case MF_FIREWORKS:		script_pushint(st,map[m].flag.fireworks); break;
			case MF_GVG_CASTLE:		script_pushint(st,map[m].flag.gvg_castle); break;
			case MF_GVG_DUNGEON:		script_pushint(st,map[m].flag.gvg_dungeon); break;
			case MF_NIGHTENABLED:		script_pushint(st,map[m].flag.nightenabled); break;
			case MF_NOBASEEXP:		script_pushint(st,map[m].flag.nobaseexp); break;
			case MF_NOJOBEXP:		script_pushint(st,map[m].flag.nojobexp); break;
			case MF_NOMOBLOOT:		script_pushint(st,map[m].flag.nomobloot); break;
			case MF_NOMVPLOOT:		script_pushint(st,map[m].flag.nomvploot); break;
			case MF_NORETURN:		script_pushint(st,map[m].flag.noreturn); break;
			case MF_NOWARPTO:		script_pushint(st,map[m].flag.nowarpto); break;
			case MF_NIGHTMAREDROP:		script_pushint(st,map[m].flag.pvp_nightmaredrop); break;
			case MF_RESTRICTED:		script_pushint(st,map[m].flag.restricted); break;
			case MF_NOCOMMAND:		script_pushint(st,map[m].nocommand); break;
			case MF_NODROP:			script_pushint(st,map[m].flag.nodrop); break;
			case MF_JEXP:			script_pushint(st,map[m].adjust.jexp); break;
			case MF_BEXP:			script_pushint(st,map[m].adjust.bexp); break;
			case MF_NOVENDING:		script_pushint(st,map[m].flag.novending); break;
			case MF_LOADEVENT:		script_pushint(st,map[m].flag.loadevent); break;
			case MF_NOCHAT:			script_pushint(st,map[m].flag.nochat); break;
			case MF_NOEXPPENALTY:		script_pushint(st,map[m].flag.noexppenalty ); break;
			case MF_GUILDLOCK:		script_pushint(st,map[m].flag.guildlock); break;
			case MF_TOWN:			script_pushint(st,map[m].flag.town); break;
			case MF_AUTOTRADE:		script_pushint(st,map[m].flag.autotrade); break;
			case MF_ALLOWKS:		script_pushint(st,map[m].flag.allowks); break;
			case MF_MONSTER_NOTELEPORT:	script_pushint(st,map[m].flag.monster_noteleport); break;
			case MF_PVP_NOCALCRANK:		script_pushint(st,map[m].flag.pvp_nocalcrank); break;
			case MF_BATTLEGROUND:		script_pushint(st,map[m].flag.battleground); break;
			case MF_RESET:			script_pushint(st,map[m].flag.reset); break;
			case MF_CHANNELAUTOJOIN:	script_pushint(st,map[m].flag.chmautojoin); break;
			case MF_NOUSECART:		script_pushint(st,map[m].flag.nousecart); break;
			case MF_NOITEMCONSUMPTION:	script_pushint(st,map[m].flag.noitemconsumption); break;
			case MF_SUMSTARTMIRACLE:	script_pushint(st,map[m].flag.nosumstarmiracle); break;
			case MF_NOMINEEFFECT:		script_pushint(st,map[m].flag.nomineeffect); break;
			case MF_NOLOCKON:		script_pushint(st,map[m].flag.nolockon); break;
			case MF_NOTOMB:			sc			case MF_NOCASHSHOP:			script_pushint(st,map[m].flag.nocashshopTOMB:			sc			case MF_NOBANKING:			script_pushint(st,map[m].flag.nobankingTOMB:			script_pushint(st,map[m].flag.notomb); break;
#ifdef ADJUST_SKILL_DAMAGE
			case MF_SKILLL_DAMAGE: {
					int ret_val = 0, type = 0;
					FETCH(4,type);
					switch(type) {
						case 1: ret_val = map[m].adjust.damage.pc; break;
						case 2: ret_val = map[m].adjust.damage.mob; break;
						case 3: ret_val = map[m].adjust.damage.boss; break;
						case 4: ret_val = map[m].adjust.damage.other; break;
						case 5: ret_val = map[m].adjust.damage.caster; break;
						default: ret_val = map[m].flag.skill_damage; break;
					}
					script_pushint(st,ret_	return SCRIPT_CMD_SUCCESS;
}
				}
				break;
#endif
		}
	}

	return 0;
}
/* PVP timer handling */
static int script_mapflag_pvp_sub(struct block_list *bl,va_list ap) {
	TBL_PC* sd = (TBL_PC*)bl;
	if (sd->pvp_timer == INVALID_TIMER) {
		sd->pvp_timer = add_timer(gettick() + 200, pc_calc_pvprank_timer, sd->bl.id, 0);
		sd->pvp_rank = 0;
		sd->pvp_lastusers = 0;
		sd->pvp_point = 5;
		sd->pvp_won = 0;
		sd->pvp_lost = 0;
	}
	clif_map_property(sd, MAPPROPERTY_F REEPVPZONE);
	clif_mmaptypeproperty2(&sd->bl,SELF);
	return 0;
}
BUILDIN_FUNC(setmapflag)
{
	int16 m,i;
	const char *str;
	int val = 0;

	str = script_getstr(st,2);
	i = script_getnum(st,3);
	FETCH(4,val);
	m = map_mapname2mapid(str);
	if(m >= 0) {
		switch(i) {
			case MF_NOMEMO:			map[m].flag.nomemo = 1; break;
			case MF_NOTELEPORT:		map[m].flag.noteleport = 1; break;
			case MF_NOSAVE:			map[m].flag.nosave = 1; break;
			case MF_NOBRANCH:		map[m].flag.nobranch = 1; break;
			case MF_NOPENALTY:		map[m].flag.noexppenalty = 1; map[m].flag.nozenypenalty = 1; break;
			case MF_NOZENYPENALTY:		map[m].flag.nozenypenalty = 1; break;
			case MF_PVP:
				map[m].fl
					map_foreachinmap(script_mapflag_pvp_sub,m,BL_PC);inmap(script_mapflag_pvp_sub,m,BL_PC);
				}
				break;
			case MF_PVP_NOPARTY:		map[m].flag.pvp_noparty = 1; break;
			case MF_PVP_NOGUILD:		map[m].flag.pvp_noguild = 1; break;
				case MF_GVG: {
					struct block_list bl;
					map[m].flag.gvg = 1;
					clif_map_property_mapall(m, MAPPROPERTY_AGITZONE);
					bl.type = BL_NUL;
		 			bl.m = m;
					clif_maptypeproperty2(&bl,ALL_SAMEMAP);
				}
				break;
			case MF_GVG_NOPARTY:		map[m].flag.gvg_noparty = 1; break;
			case MF_NOTRADE:		map[m].flag.notrade = 1; break;
			case MF_NOSKILL:		map[m].flag.noskill = 1; break;
			case MF_NOWARP:			map[m].flag.nowarp = 1; break;
			case MF_PARTYLOCK:		map[m].flag.partylock = 1; break;
			case MF_NOICEWALL:		map[m].flag.noicewall = 1; break;
			case MF_SNOW:			map[m].flag.snow = 1; break;
			case MF_FOG:			map[m].flag.fog = 1; break;
			case MF_SAKURA:			map[m].flag.sakura = 1; break;
			case MF_LEAVES:			map[m].flag.leaves = 1; break;
			case MF_NOGO:			map[m].flag.nogo = 1; break;
			case MF_CLOUDS:			map[m].flag.clouds = 1; break;
			case MF_CLOUDS2:		map[m].flag.clouds2 = 1; break;
			case MF_FIREWORKS:		map[m].flag.fireworks = 1; break;
			case MF_GVG_CASTLE:		map[m].flag.gvg_castle = 1; break;
			case MF_GVG_DUNGEON:		map[m].flag.gvg_dungeon = 1; break;
			case MF_NIGHTENABLED:		map[m].flag.nightenabled = 1; break;
			case MF_NOBASEEXP:		map[m].flag.nobaseexp = 1; break;
			case MF_NOJOBEXP:		map[m].flag.nojobexp = 1; break;
			case MF_NOMOBLOOT:		map[m].flag.nomobloot = 1; break;
			case MF_NOMVPLOOT:		map[m].flag.nomvploot = 1; break;
			case MF_NORETURN:		map[m].flag.noreturn = 1; break;
			case MF_NOWARPTO:		map[m].flag.nowarpto = 1; break;
			case MF_NIGHTMAREDROP:		map[m].flag.pvp_nightmaredrop = 1; break;
			ca + 1);
				map[m].flag.restricted = e |= 1<<(val+1);
				map[m].flag.restricted=1;
				break;
			case MF_NOCOMMAND:		map[m].nocommand = (val <= 0) ? 100 : val; break;
			case MF_NODROP:			map[m].flag.nodrop = 1; break;
			case MF_JEXP:			map[m].adjust.jexp = (val <= 0) ? 100 : val; break;
			case MF_BEXP:			map[m].adjust.bexp = (val <= 0) ? 100 : val; break;
			case MF_NOVENDING:		map[m].flag.novending = 1; break;
			case MF_LOADEVENT:		map[m].flag.loadevent = 1; break;
			case MF_NOCHAT:			map[m].flag.nochat = 1; break;
			case MF_NOEXPPENALTY:		map[m].flag.noexppenalty  = 1; break;
			case MF_GUILDLOCK:		map[m].flag.guildlock = 1; break;
			case MF_TOWN:			map[m].flag.town = 1; break;
			case MF_AUTOTRADE:		map[m].flag.autotrade = 1; break;
			case MF_ALLOWKS:		map[m].flag.allowks = 1; break;
			case MF_MONSTER_NOTELEPORT:	map[m].flag.monster_noteleport = 1; break;
			case MF_PVP_NOCALCRANK:		map[m].flag.pvp_nocalcrank = 1; break;
			case MF_BATTLEGROUND:		map[m].flag.battleground = (val <= 0 || val > 2) ? 1 : val; break;
			case MF_RESET:			map[m].flag.reset = 1; break;
			case MF_CHANNELAUTOJOIN:	map[m].flag.chmautojoin = 1 ; break;
			case MF_NOUSECART:		map[m].flag.nousecart = 1 ; break;
			case MF_NOITEMCONSUMPTION:	map[m].flag.noitemconsumption = 1 ; break;
			case MF_SUMSTARTMIRACLE:	map[m].flag.nosumstarmiracle = 1 ; break;
			case MF_NOMINEEFFECT:		map[m].flag.nomineeffect = 1 ; break;
			case MF_NOLOCKON:		map[m].flag.nolockon = 1 ; break;
			case 			case MF_NOCASHSHOP:			map[m].flag.nocashshopeak;
			case 			case MF_NOBANKING:			map[m].flag.nobankingeak;
			case MF_NOTOMB:			map[m].flag.notomb = 1; break;
#ifdef ADJUST_SKILL_DAMAGE
				case MF_SKILL_DAMAGE: {
					int type = 0;
					FETCH(5,type);
					switch (type) {
						case 1: map[m].adjust.damage.pc = val; break;
						case 2: map[m].adjust.damage.mob = val; break;
						case 3: map[m].adjust.damage.boss = val; break;
						case 4: map[m].adjust.damage.other = val; break;
						case 5: map[m].adjust.damage.caster = val; break;
					}
					map[m].flag.skill	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(remove&sd->bl,SELF);
	return 0;
}
BUILDIN_FUNC(setmapflag)
{
	int16 m,i;
	const char *str;
	int val = 0;

	str = script_getstr(st,2);
	i = script_getnum(st,3);
	FETCH(4,val);
	m = map_mapname2mapid(str);
	if(m >= 0) {
		switch(i) {
			case MF_NOMEMO:			map[m].flag.nomemo = 0; break;
			case MF_NOTELEPORT:		map[m].flag.noteleport = 0; break;
			case MF_NOSAVE:			map[m].flag.nosave = 0; break;
			case MF_NOBRANCH:		map[m].flag.nobranch = 0; break;
			case MF_NOPENALTY:		map[m].flag.noexppenalty = 0; map[m].flag.nozenypenalty = 0; break;
			case MF_NOZENYPENALTY:		map[m].flag.nozenypenalty = 0; break;
				case MF_PVP: {
					struct block_list bl;
					bl.type = BL_NUL;
					bl.m = m;
					map[m].flag.pvp = 0;
					clif_map_property_mapall(m, MAPPROPER TY_NOTHING);
					clif_maptypeproperty2(&bl,ALL_SAMEMAP);
				}
				break;
			case MF_PVP_NOPARTY:		map[m].flag.pvp_noparty = 0; break;
			case MF_PVP_NOGUILD:		map[m].flag.pvp_noguild = 0; break;
				case MF_GVG: {
					struct block_list bl;
					bl.type = BL_NUL;
					bl.m = m;
					map[m].flag.gvg = 0;
					clif_map_property_mapall(m, MAPPROPER TY_NOTHING);
					clif_maptypeproperty2(&bl,ALL_SAMEMAP);
				}
				break;
			case MF_GVG_NOPARTY:		map[m].flag.gvg_noparty = 0; break;
			case MF_NOTRADE:		map[m].flag.notrade = 0; break;
			case MF_NOSKILL:		map[m].flag.noskill = 0; break;
			case MF_NOWARP:			map[m].flag.nowarp = 0; break;
			case MF_PARTYLOCK:		map[m].flag.partylock = 0; break;
			case MF_NOICEWALL:		map[m].flag.noicewall = 0; break;
			case MF_SNOW:			map[m].flag.snow = 0; break;
			case MF_FOG:			map[m].flag.fog = 0; break;
			case MF_SAKURA:			map[m].flag.sakura = 0; break;
			case MF_LEAVES:			map[m].flag.leaves = 0; break;
			case MF_NOGO:			map[m].flag.nogo = 0; break;
			case MF_CLOUDS:			map[m].flag.clouds = 0; break;
			case MF_CLOUDS2:		map[m].flag.clouds2 = 0; break;
			case MF_FIREWORKS:		map[m].flag.fireworks = 0; break;
			case MF_GVG_CASTLE:		map[m].flag.gvg_castle = 0; break;
			case MF_GVG_DUNGEON:		map[m].flag.gvg_dungeon = 0; break;
			case MF_NIGHTENABLED:		map[m].flag.nightenabled = 0; break;
			case MF_NOBASEEXP:		map[m].flag.nobaseexp = 0; break;
			case MF_NOJOBEXP:		map[m].flag.nojobexp = 0; break;
			case MF_NOMOBLOOT:		map[m].flag.nomobloot = 0; break;
			case MF_NOMVPLOOT:		map[m].flag.nomvploot = 0; break;
			case MF_NORETURN:		map[m].flag.noreturn = 0; break;
			case MF_NOWARPTO:		map[m].flag.nowarpto = 0; break;
			case MF_NIGHTMAREDROP:		map[m].flag.pvp_nightmaredrop = 0; break;
			ca + 1);
				if(map[m].zone == 0)
					map[m].flag.restricted = 0;
				break;
			case MF_NOCOMMAND:		map[m].nocommand = 0; break;
			case MF_NODROP:			map[m].flag.nodrop = 0; break;
			case MF_JEXP:			map[m].adjust.jexp = 0; break;
			case MF_BEXP:			map[m].adjust.bexp = 0; break;
			case MF_NOVENDING:		map[m].flag.novending = 0; break;
			case MF_LOADEVENT:		map[m].flag.loadevent = 0; break;
			case MF_NOCHAT:			map[m].flag.nochat = 0; break;
			case MF_NOEXPPENALTY:		map[m].flag.noexppenalty  = 0; break;
			case MF_GUILDLOCK:		map[m].flag.guildlock = 0; break;
			case MF_TOWN:			map[m].flag.town = 0; break;
			case MF_AUTOTRADE:		map[m].flag.autotrade = 0; break;
			case MF_ALLOWKS:		map[m].flag.allowks = 0; break;
			case MF_MONSTER_NOTELEPORT:	map[m].flag.monster_noteleport = 0; break;
			case MF_PVP_NOCALCRANK:		map[m].flag.pvp_nocalcrank = 0; break;
			case MF_BATTLEGROUND:		map[m].flag.battleground = 0; break;
			case MF_RESET:			map[m].flag.reset = 0; break;
			case MF_CHANNELAUTOJOIN:	map[m].flag.chmautojoin = 0 ; break;
			case MF_NOUSECART:		map[m].flag.nousecart = 0 ; break;
			case MF_NOITEMCONSUMPTION:	map[m].flag.noitemconsumption = 0 ; break;
			case MF_SUMSTARTMIRACLE:	map[m].flag.nosumstarmiracle = 0 ; break;
			case MF_NOMINEEFFECT:		map[m].flag.nomineeffect = 0 ; break;
			case MF_NOLOCKON:		map[m].flag.nolockon = 0 ; break;
			case MF_NOTOMB:			map[m].flag.notomb = 0; break;
			case MF_NOCASHSHOP:			map[m].flag.nocashshop = 0; break;
			case MF_NOBANKING:			map[m].flag.nobanking = 0; break;
#ifdef ADJUST_SKILL_DAMAGE
			case MF_SKILL_DAMAGE: {
					map[m].flag.skill_damage = 0;
					memset(map[m].skill_damage,0,sizeof(map[m].skill_damage));
					memset(&map[m].adjust.damage,0,sizeof(map[m].adjust.damage));
				}
				break;
#endif
		}
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(pvpon)
{
	int16 m;
	const char *str;
	TBL_PC* sd = NULL;
	struct s_mapiterator* iter;
	struct block_list bl;

	str = script_getstr(st, 2);
	m = map_mapname2mapid(str);
	if( m < 0 || map[m].flag.pvp )
		return 0; // Nothing to do

	map[m].flag.pvp = 1;
	clif_map_property_mapall(m, MAPPROPERTY_FREEPVPZONE);
	bl.type = BL_NUL;
	bl.m = m;
	clif_maptypeproperty2(&bl, ALL_SAMEMAP);

	if(battle_config.pk_mode) // Disable ranking functions if pk_mode is on [Valaris]
		return 0;

	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) ) {
		if( sd->bl.m != m || sd->pvp_timer != INVALID_TIMER )
			continue; // Not applicable

		sd->pvp_timer = add_timer(gettick() + 200,pc_calc_pvprank_timer,sd->bl.id,0);
		sd->pvp_rank = 0;
		sd->pvp_lastusers = 0;
		sd->pvp_point = 5;
		sd->pvp_won = 0;
		sd->pvp_lost = 0;
	}
	mapit_free(iter);
	return SCRIPT_CMD_SUCCESS;
}

static int buildin_pvpoff_sub(struct block_list *bl,va_list ap)
{
	TBL_PC* sd = (TBL_PC*)bl;

	clif_pvpset(sd, 0, 0, 2);
	if (sd->pvp_timer != INVALID_TIMER) {
		delete_timer(sd->pvp_timer, pc_calc_pvprank_timer);
		sd->pvp_timer = INVALID_TIMER;
	}
	return 0;
}

BUILDIN_FUNC(pvpoff)
{
	int16 m;
	const char *str;
	struct block_list bl;

	str = script_getstr(st, 2);
	m = map_mapname2mapid(str);
	if(m < 0 || !map[m].flag.pvp)
		return 0; // Fixed Lupus

	map[m].flag.pvp = 0;
	clif_map_property_mapall(m, MAPPROPERTY_NOTHING);
	bl.type = BL_NUL;
	bl.m = m;
	clif_maptypeproperty2(&bl, ALL_SAMEMAP);

	if(battle_config.pk_mode) // Disable ranking options if pk_mode is on [Valaris]
		return 0;
	
	map_foreachinmap(buildin_pvpoff_sub, m, BL_PC);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(gvgon)
{
	int16 m;
	const char *str;
	struct block_list bl;

	str = script_getstr(st, 2);
	m = map_mapname2mapid(str);
	if(m >= 0 && !map[m].flag.gvg) {
		map[m].flag.gvg = 1;
		clif_map_property_mapall(m, MAPPROPERTY_AGITZONE);
		bl.type = BL_NUL;
		bl.m = m;
		clif_maptypeproperty2(&bl, ALL_SAMEMAP);
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(gvgoff)
{
	int16 m;
	const char *str;
	struct block_list bl;

	str = script_getstr(st, 2);
	m = map_mapname2mapid(str);
	if(m >= 0 && map[m].flag.gvg) {
		map[m].flag.gvg = 0;
		clif_map_property_mapall(m, MAPPROPERTY_NOTHING);
		bl.type = BL_NUL;
		bl.m = m;
		clif_maptypeproperty2(&bl, ALL_SAMEMAP);
	}
	return SCRIPT_CMD_SUCCESS,class_,evt,guardian,has_index));

	return 0;
}
/*=	Shows an emoticon on top of the player/npc
 *	emotion emotion#, <target: 0 - NPC, 1 - PC>, <NPC/PC name>
 *------------------------------------------*/
//Optional second parameter added by [Skotlex]
BUILDIN_FUNC(emotion)
{
	int type;
	int player = 0;
	
	type = script_getnum(st,2);
	if(type < 0 || type > 100)
		return 0;

	if(script_hasdata(st,3))
		player = script_getnum(st,3);
	
	if(player) {
		TBL_PC *sd = NULL;
		if(script_hasdata(st,4))
			sd = map_nick2sd(script_getstr(st,4));
		else
			sd = script_rid2sd(st);
		if(sd)
			clif_emotion(&sd->bl,type);
	} else
		if(script_hasdata(st,4)) {
			TBL_NPC *nd = npc_name2id(script_getstr(st,4));
			if(nd)
				clif_emotion(&nd->bl,type);
		} else
			clif_emotion(map_id2bl(st->oid),type);
	return SCRIPT_CMD_SUCCESS;
}

static int buildin_maprespawnguildid_sub_pc(struct map_session_data* sd, va_list ap)
{
	int16 m = va_arg(ap,int);
	int g_id = va_arg(ap,int);
	int flag = va_arg(ap,int);

	if(!sd || sd->bl.m != m)
		return 0;
	if(
		(sd->status.guild_id == g_id && flag&1) || //Warp out owners
		(sd->status.guild_id != g_id && flag&2) || //Warp out outsiders
		(sd->status.guild_id == 0 && flag&2)	// Warp out players not in guild
	)
		pc_setpos(sd,sd->status.save_point.map,sd->status.save_point.x,sd->status.save_point.y,CLR_TELEPORT);
	return 1;
}

static int buildin_maprespawnguildid_sub_mob(struct block_list *bl,va_list ap)
{
	struct mob_data *md = (struct mob_data *)bl;

	if(!md->guardian_data && md->mob_id != MOBID_EMPERIUM)
		status_kill(bl);

	return 0;
}

/*
 * Function to kick guild members out of a map and to their save points.
 * m : mapid
 * g_id : owner guild id
 * flag&1 : Warp out owners
 * flag&2 : Warp out outsiders
 * flag & 4 : reserved for mobs
*/
BUILDIN_FUNC(maprespawnguildid)
{
	const char *mapname = script_getstr(st,2);
	int g_id = script_getnum(st,3);
	int flag = script_getnum(st,4);

	int16 m = map_mapname2mapid(mapname);

	if(m == -1)
		return 0;

	//Catch ALL players (in case some are 'between maps' on execution time)
	map_foreachpc(buildin_maprespawnguildid_sub_pc,m,g_id,flag);
	if (flag&4) //Remove script mobs.
		map_foreachinmap(buildin_maprespawnguildid_sub_mob,m,BL_MOB);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(agitstart)
{
	if(agit_flag == 1)
		return SCRIPT_CMD_SUCCESS; // Agit already Start
	agit_flag = 1;
	guild_agit_start();
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(agitend)
{
	if(agit_flag == 0)
		return SCRIPT_CMD_SUCCESS; // Agit already End
	agit_flag = 0;
	guild_agit_end();
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(agitstart2)
{
	if(agit2_flag == 1)
		return SCRIPT_CMD_SUCCESS; // Agit2 already Start
	agit2_flag = 1;
	guild_agit2_start();
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(agitend2)
{
	if(agit2_flag == 0)
		return SCRIPT_CMD_SUCCESS; // Agit2 already End.
	agit2_flag = 0;
	guild_agit2_end(ay*	return SCRIPT_CMD_SUCCESSskill_bonus_timer, sd->bl.id, 0);

	return 0;
}

/*==Returns whether woe is on or off.	// Choice script
 *------------------------------------------*/
BUILDIN_FUNC(agitcheck)
{
	script_pushint(st,agit_flagay*	return SCRIPT_CMD_SUCCESSskill_bonus_timer, sd->bl.id, 0);

	return 0;
}

/*==Returns whether woese is on or off.	// Choice script
 *------------------------------------------*/
BUILDIN_FUNC(agitcheck2)
{
	script_pushint(st,agit2_flag);
	return SCRIPT_CMD_SUCCESS;
}

/// Sets the guild_id of this npc.
///
/// flagemblem <guild_id>;
BUILDIN_FUNC(flagemblem)
{
	TBL_NPC* nd;
	int g_id = script_getnum(st,2);

	if(g_id < 0)
		return 0;

	nd = (TBL_NPC*)map_id2nd(st->oid);
	if(nd == NULL)
		ShowError("script:flagemblem: npc %d not found\n", st->oid);
	else if(nd->subtype != NPCTYPE_SCRIPT)
		ShowError("script:flagemblem: unexpected subtype %d for npc %d '%s'\n", nd->subtype, st->oid, nd->exname);
	else {
		bool changed = (nd->u.scr.guild_id != g_id) ? true : false;

		nd->u.scr.guild_id = g_id;
		clif_guild_emblem_area(&nd->bl);
		/* Guild flag caching */
		if(g_id) /* Adding a id */
			guild_flag_add(nd);
		else if(changed) /* Removing a flag */
			guild_flag_remove(nd);
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getcastlename)
{
	const char* mapname = mapindex_getmapname(script_getstr(st,2),NULL);
	struct guild_castle* gc = guild_mapname2gc(mapname);
	const char* name = (gc) ? gc->castle_name : "";
	script_pushstrcopy(st,name);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getcastledata)
{
	const char *mapname = mapindex_getmapname(script_getstr(st,2),NULL);
	int index = script_getnum(st,3);
	struct guild_castle *gc = guild_mapname2gc(mapname);

	if (gc == NULL) {
		script_pushint(st,0);
		ShowWarning("buildin_setcastledata: guild castle for map '%s' not found\n", mapname);
		return 1;
	}

	switch (index) {
		case 1:
			script_pushint(st,gc->guild_id); break;
		case 2:
			script_pushint(st,gc->economy); break;
		case 3:
			script_pushint(st,gc->defense); break;
		case 4:
			script_pushint(st,gc->triggerE); break;
		case 5:
			script_pushint(st,gc->triggerD); break;
		case 6:
			script_pushint(st,gc->nextTime); break;
		case 7:
			script_pushint(st,gc->payTime); break;
		case 8:
			script_pushint(st,gc->createTime); break;
		case 9:
			script_pushint(st,gc->visibleC); break;
		default:
			if (index > 9 && index <= 9+MAX_GUARDIANS) {
				script_pushint(st,gc->guardian[index-10].visible);
				break;
			}
			script_pushint(st,0);
			ShowWarning("buildin_setcastledata: index = '%d' is out of allowed range\n", index);
			return 1;
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(setcastledata)
{
	const char *mapname = mapindex_getmapname(script_getstr(st,2),NULL);
	int index = script_getnum(st,3);
	int value = script_getnum(st,4);
	struct guild_castle *gc = guild_mapname2gc(mapname);

	if (gc == NULL) {
		ShowWarning("buildin_setcastledata: guild castle for map '%s' not found\n", mapname);
		return 1;
	}

	if (index <= 0 || index > 9 + MAX_GUARDIANS) {
		ShowWarning("buildin_setcastledata: index = '%d' is out of allowed range\n", index);
		return 1;
	}

	guild_castledatasave(gc->castle_id, index, value);
	return SCRIPT_CMD_SUCCESS;
}

/* =====================================================================
 * ---------------------------------------------------------------------*/
BUILDIN_FUNC(requestguildinfo)
{
	int guild_id = script_getnum(st,2);
	const char *event = NULL;

	if (script_hasdata(st,3)) {
		event = script_getstr(st,3);
		check_event(st,event);
	}

	if (guild_id > 0)
		guild_npc_request_info(guild_id,event);
	return SCRIPT_CMD_SUCCESS;
}

/// Returns the number of cards that have been compounded onto the specified equipped item.
/// getequipcardcnt(<equipment slot>);
BUILDIN_FUNC(getequipcardcnt)
{
	int i = -1,j,num;
	TBL_PC *sd;
	int count;

	num = script_getnum(st,2);
	sd = script_rid2sd(st);
	if (sd == NULL)
		return 1;

	if (num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);

	if (i < 0 || !sd->inventory_data[i]) {
		script_pushint(st,0);
		return 0;
	}

	if(itemdb_isspecial(sd->status.inventory[i].card[0])) {
		script_pushint(st,0);
		return 0;
	}

	count = 0;
	for (j = 0; j < sd->inventory_data[i]->slot; j++)
		if (sd->status.inventory[i].card[j] && itemdb_type(sd->status.inventory[i].card[j]) == IT_CARD)
			count++;

	script_pushint(st,count);
	return SCRIPT_CMD_SUCCESS;
}

/// Removes all cards from the item found in the specified equipment slot of the invoking character,
/// and give them to the character. If any cards were removed in this manner, it will also show a success effect.
/// successremovecards <slot>;
BUILDIN_FUNC(successremovecards) {
	int i = -1, c, cardflag = 0;
	TBL_PC* sd = script_rid2sd(st);
	int num = script_getnum(st,2);

	if (sd == NULL)
		return 1;

	if (num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num - 1]);

	if (i < 0 || !sd->inventory_data[i])
		return 0;

	if (itemdb_isspecial(sd->status.inventory[i].card[0]))
		return 0;

	for (c = sd->inventory_data[i]->slot - 1; c >= 0; --c) {
		if (sd->status.inventory[i].card[c] && itemdb_type(sd->status.inventory[i].card[c]) == IT_CARD ) { // Extract this card from the item
			unsigned char flag = 0;
			struct item item_tmp;

			memset(&item_tmp,0,sizeof(item_tmp));
			cardflag = 1;
			item_tmp.nameid   = sd->status.inventory[i].card[c];
			item_tmp.identify = 1;

			if ((flag = pc_additem(sd,&item_tmp,1,LOG_TYPE_SCRIPT))) { // Get back the cart in inventory
				clif_additem(sd,0,0,flag);
				map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
			}
		}
	}

	if (cardflag == 1) { // If card was remove remplace item with no card
		unsigned char flag = 0, j;
		struct item item_tmp;

		memset(&item_tmp,0,sizeof(item_tmp));

		item_tmp.nameid      = sd->status.inventory[i].nameid;
		item_tmp.identify    = 1;
		item_tmp.refine      = sd->status.inventory[i].refine;
		item_tmp.attribute   = sd->status.inventory[i].attribute;
		item_tmp.expire_time = sd->status.inventory[i].expire_time;
		item_tmp.bound       = sd->status.inventory[i].bound;

		for (j = sd->inventory_data[i]->slot; j < MAX_SLOTS; j++)
			item_tmp.card[j] = sd->status.inventory[i].card[j];

		pc_delitem(sd,i,1,0,3,LOG_TYPE_SCRIPT);
		if ((flag = pc_additem(sd,&item_tmp,1,LOG_TYPE_SCRIPT))) { // Check if can be spawn in inventory otherwise put on floor
			clif_additem(sd,0,0,flag);
			map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
		}

		clif_misceffect(&sd->bl,3);
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Removes all cards from the item found in the specified equipment slot of the invoking character.
/// failedremovecards <slot>, <type>;
/// <type>=0 : will destroy both the item and the cards.
/// <type>=1 : will keep the item, but destroy the cards.
/// <type>=2 : will keep the cards, but destroy the item.
/// <type>=? : will just display the failure effect.
BUILDIN_FUNC(failedremovecards) {
	int i = -1, c, cardflag = 0;
	TBL_PC* sd = script_rid2sd(st);
	int num = script_getnum(st,2);
	int typefail = script_getnum(st,3);

	if (sd == NULL)
		return 1;

	if (num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[num-1]);

	if (i < 0 || !sd->inventory_data[i])
		return 0;

	if (itemdb_isspecial(sd->status.inventory[i].card[0]))
		return 0;

	for (c = sd->inventory_data[i]->slot - 1; c >= 0; --c) {
		if (sd->status.inventory[i].card[c] && itemdb_type(sd->status.inventory[i].card[c]) == IT_CARD) {
			cardflag = 1;

			if (typefail == 2) { // Add cards to inventory, clear
				unsigned char flag = 0;
				struct item item_tmp;

				memset(&item_tmp,0,sizeof(item_tmp));

				item_tmp.nameid   = sd->status.inventory[i].card[c];
				item_tmp.identify = 1;

				if ((flag = pc_additem(sd,&item_tmp,1,LOG_TYPE_SCRIPT))) {
					clif_additem(sd,0,0,flag);
					map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
				}
			}
		}
	}

	if (cardflag == 1) {
		if (typefail == 0 || typefail == 2) // Destroy the item
			pc_delitem(sd,i,1,0,2,LOG_TYPE_SCRIPT);

		if (typefail == 1) { // Destroy the card
			unsigned char flag = 0, j;
			struct item item_tmp;

			memset(&item_tmp,0,sizeof(item_tmp));

			item_tmp.nameid      = sd->status.inventory[i].nameid;
			item_tmp.identify    = 1;
			item_tmp.refine      = sd->status.inventory[i].refine;
			item_tmp.attribute   = sd->status.inventory[i].attribute;
			item_tmp.expire_time = sd->status.inventory[i].expire_time;
			item_tmp.bound       = sd->status.inventory[i].bound;

			for (j = sd->inventory_data[i]->slot; j < MAX_SLOTS; j++)
				item_tmp.card[j] = sd->status.inventory[i].card[j];

			pc_delitem(sd,i,1,0,2,LOG_TYPE_SCRIPT);

			if ((flag = pc_additem(sd,&item_tmp,1,LOG_TYPE_SCRIPT))) {
				clif_additem(sd,0,0,flag);
				map_addflooritem(&item_tmp,1,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
			}
		}
		clif_misceffect(&sd->bl,2);
	}
	return SCRIPT_CMD_SUCCESS;
}

/* ================================================================
 * mapwarp "<from map>","<to map>",<x>,<y>,<type>,<ID for Type>;
 * type: 0=everyone, 1=guild, 2=party;	[Reddozen]
 * improved by [Lance]
 * ================================================================*/
BUILDIN_FUNC(mapwarp) // Added by RoVeRT
{
	int x,y,m,check_val=0,check_ID=0,i=0;
	struct guild *g = NULL;
	struct party_data *p = NULL;
	const char *str;
	const char *mapname;
	unsigned int index;
	mapname=script_getstr(st,2);
	str=script_getstr(st,3);
	x=script_getnum(st,4);
	y=script_getnum(st,5);
	if(script_hasdata(st,7)){
		check_val=script_getnum(st,6);
		check_ID=script_getnum(st,7);
	}

	if((m=map_mapname2mapid(mapname))< 0)
		return 0;

	if(!(index=mapindex_name2id(str)))
		return 0;

	switch(check_val){
		case 1:
			g = guild_search(check_ID);
			if (g){
				for( i=0; i < g->max_member; i++)
				{
					if(g->member[i].sd && g->member[i].sd->bl.m==m){
						pc_setpos(g->member[i].sd,index,x,y,CLR_TELEPORT);
					}
				}
			}
			break;
		case 2:
			p = party_search(check_ID);
			if(p){
				for(i=0;i<MAX_PARTY; i++){
					if(p->data[i].sd && p->data[i].sd->bl.m == m){
						pc_setpos(p->data[i].sd,index,x,y,CLR_TELEPORT);
					}
				}
			}
			break;
		default:
			map_foreachinmap(buildin_areawarp_sub,m,BL_PC,index,x,y,0,0);
			break;
	}
	return SCRIPT_CMD_SUCCESS;
}

static int buildin_mobcount_sub(struct block_list *bl,va_list ap)	// Added by RoVeRT
{
	char *event = va_arg(ap,char *);
	struct mob_data *md = ((struct mob_data *)bl);

	if( md->status.hp > 0 && (!event || strcmp(event,md->npc_event) == 0) )
		return 1;
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(mobcount) // Added by RoVeRT
{
	const char *mapname,*event;
	int16 m;

	mapname = script_getstr(st,2);
	event = script_getstr(st,3);

	if( strcmp(event, "all") == 0 )
		event = NULL;
	else
		check_event(st, event);

	if( strcmp(mapname, "this") == 0 ) {
		struct map_session_data *sd = script_rid2sd(st);

		if( sd == NULL )
			return 1;
		m = sd->bl.m;
	} else if( (m = map_mapname2mapid(mapname)) < 0 ) {
		script_pushint(st,-1);
		return 0;
	}

	script_pushint(st,map_foreachinmap(buildin_mobcount_sub, m, BL_MOB, event));
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(marriage)
{
	const char *partner = script_getstr(st,2);
	TBL_PC *sd = script_rid2sd(st);
	TBL_PC *p_sd = map_nick2sd(partner);

	if(!sd || !p_sd || !pc_marriage(sd,p_sd)) {
		script_pushint(st,0);
		return 0;
	}
	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(wedding_effect)
{
	TBL_PC *sd = script_rid2sd(st);
	struct block_list *bl;

	if(sd == NULL)
		return 1; //bl = map_id2bl(st->oid);
	bl = &sd->bl;
	clif_wedding_effect(bl);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(divorce)
{
	TBL_PC *sd = script_rid2sd(st);

	if(!sd || !pc_divorce(sd)) {
		script_pushint(st,0);
		return 0;
	}
	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(ispartneron)
{
	TBL_PC *sd = script_rid2sd(st);

	if(sd == NULL || !pc_ismarried(sd) || map_charid2sd(sd->status.partner_id) == NULL) {
		script_pushint(st,0);
		return 0;
	}

	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getpartnerid)
{
	TBL_PC *sd = script_rid2sd(st);

	if (sd == NULL)
		return 1;

	script_pushint(st,sd->status.partner_id);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getchildid)
{
	TBL_PC *sd = script_rid2sd(st);

	if (sd == NULL)
		return 1;

	script_pushint(st,sd->status.child);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getmotherid)
{
	TBL_PC *sd = script_rid2sd(st);

	if (sd == NULL)
		return 1;

	script_pushint(st,sd->status.mother);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getfatherid)
{
	TBL_PC *sd = script_rid2sd(st);

	if (sd == NULL)
		return 1;

	script_pushint(st,sd->status.father);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(warppartner)
{
	int x,y;
	unsigned short mapindex;
	const char *str;
	TBL_PC *sd=script_rid2sd(st);
	TBL_PC *p_sd=NULL;

	if(sd==NULL || !pc_ismarried(sd) || (p_sd=map_charid2sd(sd->status.partner_id)) == NULL) {
		script_pushint(st,0);
		return 0;
	}

	str=script_getstr(st,2);
	x=script_getnum(st,3);
	y=script_getnum(st,4);

	mapindex = mapindex_name2id(str);
	if (mapindex) {
		pc_setpos(p_sd,mapindex,x,y,CLR_OUTSIGHT);
		script_pushint(st,1);
	} else
		script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

/*================================================
 * Script for Displaying MOB Information [Valaris]
 *------------------------------------------------*/
BUILDIN_FUNC(strmobinfo)
{

	int num=script_getnum(st,2);
	int class_=script_getnum(st,3);

	if (!mobdb_checkid(class_)) {
		if (num < 3) //requested a string
			script_pushconststr(st,"");
		else
			script_pushint(st,0);
		return 0;
	}

	switch (num) {
		case 1: script_pushstrcopy(st,mob_db(class_)->name); break;
		case 2: script_pushstrcopy(st,mob_db(class_)->jname); break;
		case 3: script_pushint(st,mob_db(class_)->lv); break;
		case 4: script_pushint(st,mob_db(class_)->status.max_hp); break;
		case 5: script_pushint(st,mob_db(class_)->status.max_sp); break;
		case 6: script_pushint(st,mob_db(class_)->base_exp); break;
		case 7: script_pushint(st,mob_db(class_)->job_expse 6: script_pushint(st,hd->homunculus.level); break;
		default:
		SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * Summon guardians [Valaris]
 * guardian("<map name>",<x>,<y>,"<name to show>",<mob id>{,"<event label>"}{,<guardian index>}) -> <id>
 *------------------------------------------*/
BUILDIN_FUNC(guardian)
{
	int class_ = 0, x = 0, y = 0, guardian = 0;
	const char *str, *map, *evt = "";
	bool has_index = false;

	int x, y, m, size, dir;
	bool shootable;
	
	map = script_getstr(st,2);
	x = scriptr = script_getstr(st,5);
	class_ = script_getnum(st,6);

	if (script_hasdata(st,8)) { // "<event label>",<guardian index>
		evt = script_getstr(st,7);
		guardian = script_getnum(st,8);
		has_index = true;
	} else if (script_hasdata(st,7)) {
		struct script_data *data = script_getdata(st,7);

		get_val(st,data);
		if (data_isstring(data)) // "<event label>"
			evt = script_getstr(st,7);
		else if (data_isint(data)) { // <guardian index>
			guardian = script_getnum(st,7);
			has_index = true;
		} else {
			ShowError("script:guardian: invalid data type for argument #6 (from 1)\n");
			script_reportdata(data);
			return 1;
		}
	}

	check_event(st,evt);
	script_pushint(st,mob_spawn_guardian(map,x,y,str,class_,evt,guardian,has_index));
	return SCRIPT_CMD_SUCCESS,class_,evt,guardian,has_index));

	return 0;
}
/*==========================================
 * Invisible Walls [Zephyrus]
 *------------------------------------------*/
BUILDIN_FUNC(setwall)
{
	const char *map, *name;
	int x, y, m, size, dir;
	bool shootable;
	
	map = script_getstr(st,2);
	x = script_getnum(st,3);
	y = script_getnum(st,4);
	size = script_getnum(st,5);
	dir = script_getnum(st,6);
	shootable = script_getnum(st,7);
	name = script_getstr(st,8);

	if( (m = map_mapname2mapid(map)) < 0 )
		return 0; // Invalid Map

	map_iwall_set(m,SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(delwall)
{
	const char *name = script_getstr(st,2);
	map_iwall_remove(name);
	return SCRIPT_CMD_SUCCESSr(st,2);
	map_iwall_remove(name);

	return 0;
}

/// Retrieves various information about the specified guardian.
///
/// guardianinfo("<map_name>", <index>, <type>) -> <value>
/// type: 0 - whether it is deployed or not
///       1 - maximum hp
///       2 - current hp
///
BUILDIN_FUNC(guardianinfo)
{
	const char* mapname = mapindex_getmapname(script_getstr(st,2),NULL);
	int id = script_getnum(st,3);
	int type = script_getnum(st,4);

	struct guild_castle* gc = guild_mapname2gc(mapname);
	struct mob_data* gd;

	if( gc == NULL || id < 0 || id >= MAX_GUARDIANS ) {
		script_pushint(st,-1);
		return 0;
	}

	if( type == 0 )
		script_pushint(st, gc->guardian[id].visible);
	else if( !gc->guardian[id].visible )
		script_pushint(st,-1);
	else if( (gd = map_id2md(gc->guardian[id].id)) == NULL )
		script_pushint(st,-1);
	else {
		if ( type == 1 )
			script_pushint(st,gd->status.max_hp);
		else if( type == 2 )
			script_pushint(st,gd->stat	return SCRIPT_CMD_SUCCESSelse
			script_pushint(st,-1);
	}

	return 0;
}

/*==========================================
 * Get the item name by item_id or null
 *------------------------------unsigned shor----------*/
BUILDIN_FUNC(getitemname)
{
	int item_id = 0;
	struct item_data *i_data;
	char *item_name;
	struct script_data *data;

	data = script_getdata(st,2);
	get_val(st,data);

	if( data_isstring(data) ) {
		const char *name = conv_str(st,data);
		struct item_data *item_data = itemdb_searchname(name);
		if( item_data )
			item_id = item_data->nameid;
	} else
		item_id = conv_num(st,data);

	i_data = itemdb_exists(item_id);
	if( i_data == NULL ) {
		script_pushconststr(st,"null");
		return 0;
	}
	item_name = (char *)aMalloc(ITEM_NAME_LENGTH * sizeof(char));

	memcpy(item_name,i_data->jname,ITEM_NAME_LENGSCRIPT_CMD_SUCCESS;
}

	script_pushstr(st,item_name);
	return 0;
}
/*==========================================
 * Returns number of slots an item has. [Skotlex]
 *-------------------------------unsigned shor---------*/
BUILDIN_FUNC(getitemslots)
{
	int item_id;
	struct item_data *i_data;

	item_id = script_getnum(st,2);

	i_data = itemdb_exists(item_id);

	if (i_data)
		script_pushint(st,i_data->slSCRIPT_CMD_SUCCESSt);
	el@TODO: Add matk here if needed/once we get rid of RENEWALk here if needed/once we get rid of RENEWAL

/*==========================================
 * Returns some values of an item [Lupus]
 * Price, Weight, etc...
	getiteminfo(itemID,n), where n
		0 value_buy;
		1 value_sell;
		2 type;
		3 maxchance = Max drop chance of this item e.g. 1 = 0.01% , etc..
				if = 0, then monsters don't drop it at all (rare or a quest item)
				if = -1, then this item is sold in NPC shops only
		4 sex;
		5 equip;
		6 weight;
		7 atk;
		8 def;
		9 range;
		10 slot;
		11 look;
		12 elv;
		13 wlv;
		14 view id
 *------------------------------unsigned short item_id, netiteminfo)
{
	int item_id,n;
	int *item_arr;
	struct item_data *i_data;

	item_id	= scrii_data = itemdb_exists(item_id);

	if (i_data && n <= 14) {
		int *item_arr = (int*)&i_data->value_buy;
14) {
		item_arr = (int*)&i_data->value_buy;
		script_pushint(st,item_arr[n]SCRIPT_CMD_SUCCESS;
	} else
		script_pushint(st,-1);
	return 0;
}

/*==========================================
 * Set some values of an item [Lupus]
 * Price, Weight, etc...
	setiteminfo(itemID,n,Value), where n
		0 value_buy;
		1 value_sell;
		2 type;
		3 maxchance = Max drop chance of this item e.g. 1 = 0.01% , etc..
				if = 0, then monsters don't drop it at all (rare or a quest item)
				if = -1, then this item is sold in NPC shops only
		4 sex;
		5 equip;
		6 weight;
		7 atk;
		8 def;
		9 range;
		10 slot;
		11 look;
		12 elv;
		13 wlv;
		14 view id
  * Returns Value or -1 if the wrong field's been set
 *------------------------------unsigned short item_id;
	int n, valueetiteminfo)
{
	int item_id,n;
	int *item_arr;
	struct item_data *i_data;

	item_id	= script_getnum(st,2);
	n	= script_getnum(st,3);
	value	= script_getnum(st,4);
	i) {
		if (n >= 0 && n <= 14) {
			int *item_arr = (int*)&i_data->value_buy;

			item_arr[n] = value;
			script_pushint(st,value);
		}
	} else
		script_pushint(st,-1lay	return SCRIPT_CMD_SUCCESSskill_support_timer,sd->bl.id,0);

	return 0;
}

/*==========================================
 * Returns value from equipped item slot n [Lupus]
	getequipcardid(num,slot)
	where
		num = eqip position slot
		slot = 0,1,2,3 (Card Slot N)

	This func returns CARD ID, 255,254,-255 (for card 0, if the item is produced)
		it's useful when you want to check item cards or if it's signed
	Useful for such quests as "Sign this refined item with players name" etc
		Hat[0] +4 -> Player's Hat[0] +4
 *------------------------------------------*/ num, LDIN_FUNC(getequipcardid)
{
	int i = -1,num,slot;
	TBL_PC *sd;

	num = script_getnum(st,2);
	slot = sc
	if (sd == NULL)
		return 1;
	if (num > 0 && num <= ARRAYLENGTH(equip))
		i = pc_checkequip(sd,equip[numGTH(equip))
		i = pc_checkequip(sd,equip[num-1]);
	if (i >= 0 && slot >= 0 && slot < 4)
		script_pushint(st,sd->status.inventory[i]	return SCRIPT_CMD_SUCCESS]);
	else
		script_pushint(st,0);

	return 0;
}

/*==========================================
 * petskillbonus [Valaris] //Rewritten by [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC(petskillbonus)
{
	struct pet_data *pd;

	TBL_PC *sd = script_rid2sd(st);

	if (sd == NULL || sd->pd == NULL)
		return 0;

	pd = sd->pd;
	if (pd->bonus) { //Clear previous bonus
		if (pd->bonus->timer != INVALID_TIMER)
			delete_timer(pd->bonus->timer, pet_skill_bonus_timer);
	} else //Init
		pd->bonus = (struct pet_bonus *) aMalloc(sizeof(struct pet_bonus));

	pd->bonus->type = script_getnum(st,2);
	pd->bonus->val = script_getnum(st,3);
	pd->bonus->duration = script_getnum(st,4);
	pd->bonus->delay = script_getnum(st,5);

	if (pd->state.skillbonus == 1)
		pd->state.skillbonus = 0; // Waiting state

	// Wait for timer to start
	if (battle_config.pet_equip_required && pd->pet.equip == 0)
		pd->bonus->timer = INVALID_TIMER;
	e + pd->bonus->delay * imer = add_timer(gettick()+pd->bonus->delay*	return SCRIPT_CMD_SUCCESSskill_bonus_timer, sd->bl.id, 0);

	return 0;
}

/*==========================================
 * pet looting [Valaris] //Rewritten by [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC(petloot)
{
	int max;
	struct pet_data *pd;
	TBL_PC *sd = script_rid2sd(st);

	if(sd == NULL || sd->pd == NULL)
		return 0;

	max = script_getnum(st,2);

	if(max < 1)
		max = 1; //Let'em loot at least 1 item.
	else if (max > MAX_PETLOOT_SIZE)
		max = MAX_PETLOOT_SIZE;

	pd = sd->pd;
	if(pd->loot != NULL) { //Release whatever was there already and reallocate memory
		pet_lootitem_drop(pd, pd->master);
		aFree(pd->loot->item);
	} else
		pd->loot = (struct pet_loot *)aMalloc(sizeof(struct pet_loot));

	pd->loot->item = (struct item *)aCalloc(max,sizeof(struct item));

	pd->loot->max = max;
		return SCRIPT_CMD_SUCCESScount = 0;
	pd->loot->weight = 0;

	return 0;
}
/*==========================================
 * Set arrays with info of all sd inventory :
 * @inventorylist_id, @inventorylist_amount, @inventorylist_equip,
 * @inventorylist_refine, @inventorylist_identify, @inventorylist_attribute,
 * @inventorylist_card(0..3), @inventorylist_expire
 * @inventorylist_count = scalar
 *------------------------------------------*/
 = script_rid2sd(st);
	char card_var[NAME_LENGTH];
	int i, j = 0, k;

	if (!sd)
		return 0;
	for (i = 0; i < MAX_INVENTORY; i++) {
		if turn 0;
	for(i=0;i<MAX_INVENTORY;i++) {
		if(sd->status.inventory[i].nameid > 0 && sd->status.inventory[i].amount > 0) {
			pc_setreg(sdreference_uid(add_str("@inventorylist_id"), j),sd->status.inventory[i].nameid);
			pc_setreg(sd,refrence_uid(add_str("@inventorylist_amount"), j),sd->status.inventory[i].amount);
			pc_setreg(sd,rej),sd->status.inventory[i].equip);
			pc_setreg(sd,reference_uid(add_str("@inventorylist_refine"),rence_uid(add_str("@inventorylist_refine"), j),sd->status.inventory[i].refine);
			pc_setreg(sd,referj),sd->status.inventory[i].identify);
			pc_setreg(sd,reference_uid(add_str("@inventorylist_attribute"),ce_uid(add_str("@inventorylist_attribute"), j),sd->status.inventory[i].attribute);
			for (k = 0;"@inventorylist_card%d",k + 1);
				pc_setreg(sd,reference_uid(add_str(card_var),_setreg(sd,reference_uid(add_str(card_var), j),sd->status.inventory[i].card[k]);
			}
			pc_setreg(sd,refj),sd->status.inventory[i].expire_time);
			pc_setreg(sd,reference_uid(add_str("@inventorylist_bound"),erence_uid(add_str("@inventorylist_bound"), j),sd->status.inventory[i].bound);
			j++;
		}
	}
	pc_setreg(sd,SCRIPT_CMD_SUCCESSdd_str("@inventorylist_count"),j);
	return 0; = script_rid2sd(st);
	int i, j = 0;

	if (!sd)
		return 0;

	for (i = 0; i < MAX_SKILL; i++) {
		if ) return 0;
	for(i=0;i<MAX_SKILL;i++) {
		if(sd->status.skill[i].id > 0 && sd->status.skill[i].lv > 0) {
			pc_setre(sd,reference_uid(add_str("@skilllist_id"), j),sd->status.skill[i].id);
			pc_setrej),sd->status.skill[i].lv);
			pc_setreg(sd,reference_uid(add_str("@skilllist_flag"),d,reference_uid(add_str("@skilllist_flag"), j),sd->status.skill[i].flag);
			j++;
		}
	}
	pc_setregSCRIPT_CMD_SUCCESSsd,add_str("@skilllist_count"),j);
	return = script_rid2sd(st);
	int i;

	if (sd == NULL)
		return 0;

	for (i = 0; i < MAX_INVENTORY; i++)
		if (sd->status.inventory[i].amount)
			pc_delitem(sd,i,sd->status.inventory[i].amount,0,0,LOG_TYPE_SCRIPTay*	return SCRIPT_CMD_SUCCESSskill_bonus_timer, sd->bl.id, 0);

	return 0;
}

/*==========================================
 * Disguise Player (returns Mob/NPC ID if success, 0 on fail)
 *------------------------------------------*/
BUILDIN_FUNC(disguise)

	if (sd == NULL)
		C* sd = script_rid2sd(st);
	if (sd == NULL) return 0;

	id = script_getnum(st,2);

	if (mobdb_checkid(iid);
		script_pushint(st,id);
	} nt(st,sd->status.inventory[i]	return SCRIPT_CMD_SUCCESS]);
	else
		script_pushint(st,0);

	return 0;
}

/*==Undisguise Player (returns 1=====
 * Disguise Player (returns Mob/NPC ID if success, 0 on fail)
 *--------------undisguise)
{
	TBL_PC* sd = script_rid2sd(st);

	if (sd == NULL)
		C* sd = script_rid2sd(st);
	if (sd == NULL) return 0;

	if (sd->disguise) {
		pc_disguise(sd,0);
		script_pus	return SCRIPT_CMD_SUCCESS;
	} else
		script_pushint(st,1);

	return 0;
}

/*==========================================
 * Transform a bl to another _class,
 * @type unused
 *------------------------------------------*/
BUILDIN_FUNC(classchange)
{
	int _class,type;
	struct block_list
		*bl = map_id2bl(st->oid);

	if (bl == NULL) return 0;

	_class = script_getnum(st,2);
	type = script_getnum(st,3);
SCRIPT_CMD_SUCCESSclif_class_change(bl,_class,type);
	return 0;
}

/*==========================================
 * Display an effect
 *------------------------------------------*/
BUILDIN_FUNC(misceffect)
{
	int type;

	type = script_getnum(st,2);
	if (st->oid && st->oid != fake_nd->bl.id) {
		
		if (bl)
			clif_specialeffect(bl,type,AREA);
	} else{
		TBL_PC *sd = script_rid2sd(st);
A);
	} else{
		TBL_PC *sd=script_rid2sd(st);
		if (sd)
			clif_sSCRIPT_CMD_SUCCESSecialeffect(&sd->bl,type,AREA);
	}
	return 0;
}
/*==========================================
 * Play a BGM on a single client [Rikter/Yommy]
 *--------------------------struct map_session_data* sd = script_rid2sd(st);

	if (sd)
		clif_playBGM(sd, script_getstr(st,2));t	return SCRIPT_CMD_SUCCESS,2);
		clif_playBGM(sd, name);
	}

	return 0;
}

static int playBGM_sub(struct block_list* bl,va_list ap)
{
	const char* name = va_arg(ap,const char*)

	clif_playBGM(BL_CAST(BL_PC, bl), name);

	return 0;
}

static int playBGM_foreachpc_sub(struct map_session_data* sd, va_list args)
{
	const char* name = va_arg(args, const char*);

	clif_playBGM(sd, name);
	return 0;
}

/*==========================================
 * Play a BGM on multiple client [Rikter/Yommy]
 *------------------------------------------*/
BUILDIN_FUNC(playBGMall)
{
	cons	if( script_hasdata(st,7) ) { // Sr(st,2);

	if( script_hasdata(st,7) )
	{// specified part of map
		const char* map = script_getstr(st,3);
		int x0 = script_getnum(st,4);
		int y0 = script_getnum(st,5);
		int x1 = script_getnum(st,6);
		int y1 = script_getnum(st,7);

		map_foreachinarea(playBGM_sub, map_mapname2 else if( script_hasdata(st,3) ) { // E;
	}
	else if( script_hasdata(st,3) )
	{// entire map
		const char* map = script_getstr(st,3);

		map_foreachinmap(playBGM_s else // Entire server
		map_foreachpc(&playBGM_foreachpc_sub, name);
	return SCRIPT_CMD_SUCCESS&playBGM_foreachpc_sub, name);
	}

	return 0;
}

/*==========================================
 * Play a .wav sound for sd
 *------------------------------------------*/
BUILDIN_FUNC(soundeffect)
{
	TBL_PC* sd = script_rid2sd(st);
	const char* name = script_getstr(s sd )
		clif_soundeffect(sd,&sd->bl,name,type);
	return SCRIPT_CMD_SUCCESSndeffect(sd,&sd->bl,name,type);
	}
	return 0;
}

int soundeffect_sub(struct block_list* bl,va_list ap)
{
	char* name = va_arg(ap,char*);
	int type = va_arg(ap,int);

	clifsoundeffect((TBL_PC *)bl, bl, name, type);

	return 0;
}

/*==========================================
 * Play a sound effect (.wav) on multiple clients
 * soundeffectall "<filepath>",<type>{,"<map name>"}{,<x0>,<y0>,<x1>,<y1>};
 *------------------------------------------*/
BUILDIN_FUNC(soundeffectall)
{
	struct block_list* bl;
	const char* name;
	int type;

	bl = (st->rid) ? &(scri( !bl )
		return 0;

	name = script_getstr(st,2);
	type = script_getnum(st,3);

	// FIXME: E);
	type = script_getnum(st,3);

	//FIXME: enumerating map squares (map_foreach) is slower than enumerating the list of 	if( !script_hasdata(st,4) ) // Area around
		clif_soundeffectall(bl,name,type,AREA);
	else if( !script_hasdata(st,5) ) { // Entire map
		const char* map = script_getstr(st,4);
		map_foreachinmap(soundeffect_sub,map_mapname2mapid(map),BL_PC,name,type);
	} else if( script_hasdata(st,8) ) { // S;
	}
	else
	if(script_hasdata(st,8))
	{	// specified part of map
		const char* map = script_getstr(st,4);
		int x0 = script_getnum(st,5);
		int y0 = script_getnum(st,6);
		int x1 = script_getnum(st,7);
		int y1 = script_getnummap_mapname2mapid(map),x0,y0,x1,y1,BL_PC,name,type);
	} else y0, x1, y1, BL_PC, name, type);
	}
	else
	{
		ShowError("buildin_soundeffectall: insufficient 		return SCRIPT_CMD_SUCCESScount = 0;
	pd->loot->weight = 0;

	return 0;
}
/*==========================================
 * pet status recovery [Valaris] / Rewritten by [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC(petr = script_rid2sd(st);

	if( sd == NULL || sd->pd == NULL )
		return 0;

	pd = sd->pd;
	
	if( pd->recovery ) { //Halt previous bonus
		if( pd->recovery->timer != INVALID_TIMER )
			delete_timer(pd->recovery->timer,pet_recovery_timer);
	} else //Init
		pd->recovery = (struct pet_recovery *)aMalloc(sizeof(struct pet_recovery));
y *)aMalloc(sizeof(struct pet_recovery));
		
	pd->recovery->type = (sc_type)script_getnum(st,2);
	pd->recovery->delay = script_getnum(s	return SCRIPT_CMD_SUCCESS>recovery->timer = INVALID_TIMER;

	return 0;
}

/*==========================================
 * pet healing [Valaris] //Rewritten by [Skotlex]
 *------------------------------------------*/
BUILDIN_FUNC( = script_rid2sd(st);

	if( sd == NULL || sd->pd == NULL )
		return 0;

	pd = sd->pd;
	if( pd->s_skill ) { //Clear previous skill
		if( pd->s_skill->timer != INVALID_TIMER ) {
			if( pd->s_skill->id )
				delete_timer(pd->s_skill->timer,pet_skill_support_timer);
			else
				delete_timer(pd->s_skill->timer,pet_heal_timer);
		}
	} else //Init memory
		pd->s_skill = (struct pet_skill_support *)aMalloc(sizeof(struct pet_skill_support));
	
	pd->s_skill->id = uct pet_skill_support));
	
	pd->s_skill->id=0; //This id identifies that it IS petheal rather than pet_skillsupport
	//Use t = script_getnum(st,2);
	pd->s_skill->delay = script_getnum(st,3);
	pd->s_skill->hp = script_getnum(st,4);
	pd->s_skill->sp = script_getnum(st,5);

	//Use delay as initial offset to avoid skill/heal exploits
	if( battle_config.pet_equip_required && pd->pet.equip == 0 fig.pet_equip_required && pd->pet.equip == 0)
		pd->s_skill->timer = INVALID_TIMER;
	els + pd->s_skill->delay * 1000,pet_heal()+pd->s_skill->delay	return SCRIPT_CMD_SUCCESSskill_support_timer,sd->bl.id,0);

	return 0;
}

/*==pet attack skills [Valaris] //========
 * pet status recovery [Valaris] / Rewritten by [Skotlex]
 *------------------------------------------*/
/// petskillattack <skill id>,<level>,<rate>,<bonusrate>
/// petskillattack "<skill name>",<level>,<rate>,<bonusrate>
BUILDIN_FUstruct script_data *data;
	TBL_PC *sd = script_rid2sd(st);

	if( sd == NULL || sd->pd == NULL )
		return 0;

	pd = sd->pd;
	if( pd->a_skill == NULL )
		pd->a_skill = (struct pet_skill_attack *)aMalloc(sizeof(struct pet_skill_attack));

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	pd->a_skill->id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	pd->a_skill->lv = script_getnum(st,3);
	pd->a_skill->div_ = 0;
	pd->a_skill->rate = script_getnum(st,4);
	pd->a_skill->bonusrate = script_getnum(st,5);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================/*==========================================
 * pet attack skills [Valaris]
 *------------------------------------------*/
/// petskillattack2 <skill id>,<level>,<div>,<rate>,<bonusrate>
/// petskillattack2 "<skill name>",<level>,<div>,<rate>,<bonusrate>
BUILDIN_FUNstruct script_data *data;
	TBL_PC *sd = script_rid2sd(st);

	if( sd == NULL || sd->pd == NULL )
		return 0;

	pd = sd->pd;
	if( pd->a_skill == NULL )
		pd->a_skill = (struct pet_skill_attack *)aMalloc(sizeof(struct pet_skill_attack));

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	pd->a_skill->id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	pd->a_skill->lv = ) : script_getnum(st,2) );
	pd->a_skill->lv=script_getnum(st,3);
	pd->a_skill->di = script_getnum(st,5);
	pd->a_skill->bonusrate = cript_getnum(st,5);
		return SCRIPT_CMD_SUCCESSl->bonusrate=script_getnum(st,6);

	return 0;
}

/*==========================================
 * pet support skills [Skotlex]
 *------------------------------------------*/
/// petskillsupport <skill id>,<level>,<delay>,<hp>,<sp>
/// petskillsupport "<skill name>",<level>,<delay>,<hp>,<sp>
BUILDIN_FUNstruct script_data *data;
	TBL_PC *sd = script_rid2sd(st);

	if( sd == NULL || sd->pd == NULL )
		return 0;

	pd = sd->pd;
	if( pd->s_skill ) { //Clear previous skill
		if( pd->s_skill->timer != INVALID_TIMER ) {
			if( pd->s_skill->id r != INVALID_TIMER) {
			if (pd->s_skill->id)
				delete_timer(pd->s_skill->timer, pet_skill_support_timer);
			else
				delete_timer(pd->s_sInit memory
		pd->s_skill = (struct pet_skill_support *)aMalloc(sizeof(struct pet_skill_support));

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	pd->s_skill->id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	pd->s_skill->lv = script_getnum(st,3);
	pd->s_skill->delay = script_getnum(st,4);
	pd->s_skill->hp = script_getnum(st,5);
	pd->s_skill->sp = l->hp=script_getnum(st,5);
	pd->s_skill->sp=script_getnum(st,6);

	//Use delay as ini( battle_config.pet_equip_required && pd->pet.equip == 0 fig.pet_equip_required && pd->pet.equip == 0)
		pd->s_skill->timer = INVALID_TIMER;
	els + pd->s_skill->delay * er = add_timer(gettick()+pd->s_skill->delay	return SCRIPT_CMD_SUCCESSskill_support_timer,sd->bl.id,0);

	return 0;
}

/*==========================================
 * Scripted skill effects [Celest]
 *------------------------------------------*/
/// skilleffect <skill id>,<level>
/// skilleffect "<skill struct script_data *data = script_getdata(st,2);
	TBL_PC *sd = script_rid2sd(st);
	uint16 skill_id, skill_lv;

	get_val(st,data); //Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	skill_lv = script_getnum(st,3);
scif( sd == NULL )
		return 1;
sc/* Ensure we're standing because the following packet causes the client to virtually set the char to stand,
	 * which leaves the server thinking it still is sitting. */
	if( pc_issit(sd) ) {
		pc_setstand(sd);
		skill_sit(sd,0);
	}=script_getnum(st,3);
	sd=script_rid2sd(st);

	clif_skill_nod	return SCRIPT_CMD_SUCCESS>bl,&sd->bl,skill_id,skill_lv,1);

	return 0;
}

/*==========================================
 * NPC skill effects [Valaris]
 *------------------------------------------*/
/// npcskilleffect <skill id>,<level>,<x>,<y>
/// npcskilleffect "<skill name>",<level>,<x>,<y>
BUILDIN_F = map_id2bl(st->oid);
	struct script_data *data = script_getdata(st,2);
	uint16 skill_id, skill_lv;
	int x, y;

	get_val(st,data); //Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	skill_lv = script_getnum(st,3);
	x = script_getnum(st,4);
	y = script_getnum(st,5);

	if( bl (st,4);
	int y=script_getnum(st,5);

	if (bl)
		clif_skill_pos	return SCRIPT_CMD_SUCCESSskill_id,skill_lv,x,y,gettick());

	return 0;
}

/*==========================================
 * Special effects [Valaris]
 *------------------------------------------*/
BUILDIN_ = UNC(specialeffect)
{
	struct block_list *bl=map_id2bl(st->oid);
	int type = script_getnum(st,2);
	enum send_target target = script_hasdata(st,3) ? (s bl == NULL )
		return 0;

	if( script_hasdata(st,4) ) {
		TBL_NPC *nd = npc_name2id(script_getstr(st,4));
		if( nd )
			clif_specialeffect(&nd->bl,type,target);
	} else {
		if( target == SELF ) {
			TBL_PC *sd = script_rid2sd(st);

			if( sd )
				clif_specialeffect_single(bl,type,sd->fd);
		} else
			clif_specialeffect(bl,type,target);
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(specialeffect2)
{
	TBL_PC *sd
{
	struct block_list *bl=map_id2bl(st->oid);
	int type = script_getnum(st,2);
	enum send_target target = script_hasdata(st,3) ? (s script_hasdata(st,4) )
		sd = map_nick2sd(script_getstr(st,4));
	else
		sd = script_rid2sd(st);

	if( sd )
		clif_specialeffect(&sd->bl,type,target);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================/*==========================================
 * Nude [Valaris]
 *------------------------------------------*/
BUILDIN_FUNC(nude)
{
	TBL_PC *sd = script_rid2sd(st);
	int i, calcflag = 0;

	if( sd == NULL )
		return 0;

	for( i = 0 ; i < EQI_MAX; i++ ) {
		if( sd->equip_index[ i ] >= 0 ) {
			if( !calsd,sd->equip_index[i],pc_unequipitem( sd , sd->equip_index[ i ] , 2);
		}
	}

	if	return SCRIPT_CMD_SUCCESS )
		status_calc_pc(sd,SCO_NONE);

	return 0;
}

int atcommand_sub(*sd, dummy_sd;
	int fd;
	const char *ummy_sd;
	TBL_PC* sd;
	int fd;
	co	if( st->rid ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 1;
		fd = sd->fd;
	} else { //Use a dummy character.
		sd = &dummy_sd;
		fd = 0;ummy character.
		sd0,sizeof(TBL_PC));
		if( st->oid ) {
			struct block_list* bl = map_id2bl(st->oid);

			memcpy(&dummy_sd.bl,bl,sizeof(struct block_list));
			if( bl->type == BL_NPC )
				safestrncpy(dummy_sd.status.name,((TBL_NPC*)bl)->name,NAME_LENGTH);
			sd->mapindex = (bl->m > 0) ? bl->m : mapindex_name2id(MAP_DEFAULT);
		}

		// Init Group ID, Level, & permissions
		sd->group_id = sd->group_level = 99;
		sd->permissions |= PC_PERM_ALLPERMISSION;
	}
	if( !is_atcommand(fd, sd, cmd, type) ) {
		ShowWarning("buildin_atcommand: failed to execute command '%s'\n", cmd);
		script_reportsrc(st);
		return 1;
	}m(st,3);
SCRIPT_CMD_SUCCESSclif_class_change(bl,_class,type);
	return 0;
}

/*==gmcommand=================================
 * movenpc [MouseJstr]
 *------------------------------------------*/
BUILDIN_FUNC(atcommand)
{
  return atcommand_sub(st,0);
}

/*==========================================
 * Displays a message for the player only (like system messages like "you got an apple" )
 *------------------------------------------*/
BUILDIN_FUNC(dispbottom)
{
	TBL_PC *sd
	message = script_getstr(st,2);
	if( sd sage;
	message = script_getstr(st,2);
	if(sd)
		clif_disp_onlyselSCRIPT_CMD_SUCCESS(sd,message,(int)strlen(message));
	return 0;
}

/*===================================
 * Heal portion of recovery command
 *-----------------------------------*/
int recovery_sub(str revive&(1|4) && pc_isdead(sd)  revive)
{
	if(revive&(1|4) && pc_isdead(sd)) {
		status_revive(&sd->bl,100,100);
		clif_displaymessage(sd->fd,msg_txt(16)); //You've been revived!
		clif revive&(1|2) && !pc_isdead(sd) );
	} else if(revive&(1|2) && !pc_isdead(sd)) {
		status_percent_heal(&sd->bl,100,100);
		clif_displaymessage(sd->fd,msg_txt(6SCRIPT_CMD_SUCCESS0)); //You have been recovered!
	}
	return 0;
}

/*=========================================================================
 * Fully Recover a Character's HP/SP - [Capuche] & [Akinari]
 * recovery <type>{,<option>,<revive_flag>{,<map name>}};
 * <type> determines <option>:
 *	0 : char_id
 *	1 : party_id
 *	2 : guild_id
 *	3 : map_name
 *	4 : all characters
 * <revive_flag>:
 *	1 : Revive and heal all players (default)
 *	2 : Heal living players only
 *	4 : Revive dead players only
 * <map name>:
 *	for types 1-2 : map_name (null = all maps)
 *-------------------------------------------------------------------------*/
BUILDIN_FUNC(recovery)
{
	TBL_PC *sd;
	int map = 0, type = 0	if( script_hasdata(st,4) )
		revive = script_getnum(st,4);

	switch( type ) {
		case 0:
			if( script_hasdata(st,3) )
				sd = map_charid2sd(script_getnum(st,3));
			else if( (sd = script_rid2sd(st)) == NULL 
			else if((sd = script_rid2sd(st)) == NULL)
				return 0; //If we don't have sd by now, bail out
			recover	struct party_data* p;
				//When no party given, we use invoker party
				int p_id = 0, i;

				if( script_hasdata(st,5) ) { //Bad maps shouldn't cause issues
					map = map_mapname2mapid(script_getstr(st,5));
					if( map < 1 ) { //But we'll check anyways
						ShowDebug("recovery: bad map name given (%s)\n",script_getstr(st,5));
						return 1;
					}
				}
				if( script_hasdata(st,3) )
					p_id = script_getnum(st,3);
				else if( (sd = script_rid2sd(st)) )
					p_id = sd->status.party_id;
				p = party_search(p_id);
				if( p == NULL )
					return 0;
				for( i = 0; i < MAX_PARTY; i++ ) {
					struct map_session_data* pl_sd = p->data[i].sd;

					if( !pl_sd || pl_sd->status.party_id != p_id || (map && pl_sd->bl.m != map) )
						continue;
					recovery_sub(pl_sd,revive);
				}
			}
			break;
		case 2: {
				struct guild* g;
				//When no guild given, we use invoker guild
				int g_id = 0, i;

				if( script_hasdata(st,5) ) { //Bad maps shouldn't cause issues
					map = map_mapname2mapid(script_getstr(st,5));
					if( map < 1 ) { //But we'll check anyways
						ShowDebug("recovery: bad map name given (%s)\n",script_getstr(st,5));
						return 1;
					}
				}
				if( script_hasdata(st,3) )
					g_id = script_getnum(st,3);
				else if( (sd = script_rid2sd(st)) )
					g_id = sd->status.guild_id;
				g = guild_search(g_id);
				if( g == NULL )
					return 0;
				for( i = 0; i < MAX_GUILD; i++ ) {
					struct map_session_data* pl_sd = g->member[i].sd;

					if( !pl_sd || pl_sd->status.guild_id != g_id || (map && pl_sd->bl.m != map) )
						continue;
					recovery_sub(pl_sd,revive);
				}
			}
			break;
		case 3:
			if( script_hasdata(st,3) )
				map = map_mapname2mapid(script_getstr(st,3));
			else if( (sd = script_rid2sd(st)) )
				map = sd->bl.m;
			if( map < 1 )
				return 1; //No sd and no map given - return
		case 4: {
				struct s_mapiterator *iter;
				struct script_data *data = script_getdata(st,3);

				get_val(st,data); //Convert into value in case of a variable
				if( script_hasdata(st,3) && !data_isstring(data) )
					revive = script_getnum(st,3); //Recovery 4,<revive_flag>;
				iter = mapit_getallusers();
				for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) ) {
					if( type == 3 && sd->bl.m != map )
						continue;
					recovery_sub(sd, revive);
				}
				mapit_free(iter);
			}
			break;
		default:
			ShowWarning("script: buildin_recovery: Invalid type %d\n",ript: buildin_recovery: Invalid type %d\n", type);
			script_pushint(st,-1);
			return 1;
	}
	script_pushint(st,1); //SSCRIPT_CMD_SUCCESSccessfully executed without errors
	return 0;
}

/*==========================================
 * Get your pet info: getpetinfo(n)
 * n -> 0:pet_id 1:pet_class 2:pet_name
 * 3:friendly 4:hungry, 5: rename flag, 6:level
 *--------------------------------------- = script_rid2sd(st);
	TBL_PET *pd;
	int type = script_getnum(st,2);

	if( !sd || !sd->pd ) {
		if( type == 2 ,2);
	
	if(!sd || !sd->pd) {
		if (type == 2)
			script_pushconststr(st,"null");
		else
			script_pushint( type ) ;
		return 0;
	}
	pd = sd->pd;
	switch(type){
		case 0: script_pushint(st,pd->pet.pet_id); break;
		case 1: script_pushint(st,pd->pet.class_); break;
		case 2: script_pushstrcopy(st,pd->pet.name); break;
		case 3: script_pushint(st,pd->pet.intimate); break;
		case 4: script_pushint(st,pd->pet.hungry); break;
		case 5: script_pushint(st,pd->pet.rename_flag); break;
		case 6: script_pushint(st,(int)pd->pet.level); break;
		default:
		SCRIPT_CMD_SUCCESSscript_pushint(st,0);
			break;
	}
	return 0;
}

/*==========================================
 * Get your homunculus info: gethominfo(n)
 * n -> 0:hom_id 1:class 2:name
 * 3:friendly 4:hungry, 5: rename flag.
 * 6: level
 *--------------------------------------- = script_rid2sd(st);
	TBL_HOM *hd;
	int type = script_getnum(st,2);

	hd = sd ? sd->hd : NULL;
	if( !hd ) {
		if( type == 2 )
			script_pushconststr(st,"null");
		else
			script_pushint(st,0);
		return 0;
	}

	switch( type ) pushint(st,0);
		return 0;
	}

	switch(type){
		case 0: script_pushint(st,hd->homunculus.hom_id); break;
		case 1: script_pushint(st,hd->homunculus.class_); break;
		case 2: script_pushstrcopy(st,hd->homunculus.name); break;
		case 3: script_pushint(st,hd->homunculus.intimacy); break;
		case 4: script_pushint(st,hd->homunculus.hunger); break;
		case 5: script_pushint(st,hd->homunculus.rename_flag); break;
		case 6: script_pushint(st,hd->homunculus.level); break;
		default:
		SCRIPT_CMD_SUCCESSscript_pushint(st,0);
			break;
	}
	return 0;
}

/// Retrieves information about character's mercenary
/// getmercinfo <type>[,<charILDIN_FUNC(getmercinfo)
{
	int type, char_id;
	struct map_session_data* sd;
	struct mercen	if( script_hasdata(st,3) ) {
		int char_id = script_getnum(st,3);

		if( (sd = map_charid2sd(char_id)) == NULL ) {
			ShowError("buildin_getmercinfo: No such character (char_id=%d).\n",char_id);
			script_pushnil(st);
			return 1;
		}
	} else if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	md = (sd->status.mer_id && sd->md) ? sd->md : NULL;
	switch( type ) sd->md ) ? sd->md : NULL;

	switch( type )
	{
		case 0: script_pushint(st,md ? md->mercenary.mercenary_id : 0); break;
		case 1: script_pushint(st,md ? md->mercenary.class_ : 0); break;
		case 2:
			if( md )
				script_pushstrcopy(st,md->db->name);
			else
				script_pushconststr(st,"");
			break;
		case 3: script_pushint(st,md ? mercenary_get_faith(md) : 0); break;
		case 4: script_pushint(st,md ? mercenary_get_calls(md) : 0); break;
		case 5: script_pushint(st,md ? md->mercenary.kill_count : 0); break;
		case 6: script_pushint(st,md ? mercenary_get_lifetime(md) : 0); break;
		case 7: script_pushint(st,md ? md->db->lv : 0); break;
		default:
			ShowError("buildin_getype,sd->status.char_id);
			script_pushnil(st);
	mand '%s'\n", c	return SCRIPT_CMD_SUCCESSipt_reportsrc(st);
		return 1;
	}

	return 0;
}

/*==Shows wether your inventory(and equips) contain
   selected card or not.
   d equips) contain
   selected card or not.
	checkequipedcard(4001);
 *-----------------------------------int n, i, c = 0;
	TBL_PC *sd = script_rid2sd(st);

	if( sd == NULL )
		return 1;

	c = script_getnum(st,2);

	for( i = 0; i < MAX_INVENTORY; i++ ) {
		if( sd->status.inventory[i].nameid > 0 && sd->status.inventory[i].amount && sd->inventory_data[i] ) {
			if( itemdb_isspecial(sd->status.inventory[i].card[0]) )
				continue;
			for( n = 0 ; n < sd->inventory_data[i]->slot; n++ ) {
				if( sd->status.inventory[i].card[n] == c ) {
					script_pushint(st,1);
					return 0;
				}
			}
		}d->bl, CLR_OUTSIGHT);
		clif_spawn(&SCRIPT_CMD_SUCCESSd->bl);
	}

	script_pushint(st,0);
	retur
	sel = script_getnum(st,2);
	if( !sel ) {
		int pos;

		if( !data_islabel(script_getdata(st,3)) ) {
			ShowError("script: jump_zero: not label !\n");
			st->state = END;
			return 1;
		}
		pos = script_getnum(st,3);
		st->pos = pos;
		st->state = GOTO;
	}
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================/*==========================================
 * movenpc [MouseJstr]
 *------------------------------------------*/
BUILDIN_FUNC(movenpc)
{
	TBL_NPC *nd = NULL;
	const char *npc;
	int x,y;

	npc = script_getstr(st,2);
	x = script_( (nd = npc_name2id(npc)) == NULL )
		return -1;

	if( script_hasdata(st,5) )
		nd->ud.dir = script_getnum(st,5)%8;
	npc_movenpc(nd,x,y);
	return SCRIPT_CMD_SUCCESS;
}

/*==========================================
 * message=================================
 * movenpc [MouseJstr]
 *---------------essage)
{
	const char *msg,*player;
	TBL_PC *pl_sd = NULL;

	player = script_getstr(st,2);
	msg = script_getstr(st,3);

	if( (pl_sd = map_nick2sd((char *)player)) == NULL )
		return 0;
	clif_displaymessage(pl_sd->fd,msg);

	return SCRIPT_CMD_SUCCESS;
}

/*==========================================/*==========================================
 * npctalk (sends message to surrounding area)
 *------------------------------------------*/struct npc_data* nd = (struct npc_data *)map_id2bl(st->oid);

	str = script_getstr(st,2);
	if( nd ) {
		char name[NAME_LENGTH], message[256];

		safestrncpy(name,nd->name,sizeof(name));
		strtok(name,"#"); //Discard extra name identifier if present
		safesnprintf(message,sizeof(message),"%s : %s",name,str);
		clif_disp_overhead(&nd->bl,message);
	}
	return SCRIPT_CMD_SUCCESS;
}

//Cad(&nd->bl, message);
	}

	return 0;
}

// change npc walkspeed [Valaris]
BUILDIN_FUNC(npcspeed)
{
	struct npc_data* nd;
	int  speed;

	speed = script_getnum(st,2);
	nd =(stru {
		nd->speed = speed;
		nd->ud.state.speed_changed = 1;
	}

	return SCRIPT_CMD_SUCCESSd->u
//Mate.speed_changed = 1;
	}

	return 0;
}
// make an npc walk to a position [Valaris]
BUILDIN_FUNC(npcwalkto)
{
	struct npc_data *nd = (struct npc_data *)map_id2bl(st->oid);
	int x = 0, y = 0;

	x = script_g nd ) {
		if( !nd->status.hp getnum(st,3);

	if(nd) {
		if(!nd->status.hp)
			status_calc_npc(nd,SCO_FIRST);
		else
			status_calc_npc(nd,SC	return SCRIPT_CMD_SUCCESS	unit_wSlktoxy(&nd->bl,x,y,0);
	}

	return 0;
}
// stop an npc's movement [Valaris]
 = (struct npc_data *)map_id2bl(st->oid);

	if( nd truct npc_data *)map_id2bl(st->oid);	return SCRIPT_CMD_SUCCESS		unit_stop_walking(&nd->bl,1|4);

	return 0;
}


/*==========================================
 * getlook char info. getlook(arg)
 *------------------------------------------*/
BUILDIN_
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1;

	type = script_getnum(st,2);
	val = -1;
	switch( type ) {
		case LOOK_HAIR:     	val = sd->status.hair; break; //1
		case LOOK_WEAPON:   	val = sd->status.weapon; break; //2
		case LOOK_HEAD_BOTTOM:	val = sd->status.head_bottom; break; //3
		case LOOK_HEAD_TOP: 	val = sd->status.head_top; break; //4
		case LOOK_HEAD_MID: 	val = sd->status.head_mid; break; //5
		case LOOK_HAIR_COLOR:	val = mid; break; //5
		case LOOK_HAIR_COLOR:	val=sd->status.hair_colo = sd->status.clothes_color; break; //7
		case LOOK_SHIELD:   	val = sd->status.shield; break; //8
		case LOOK_SHOES:    	break; //9
		case LOOK_ROBE:     	val = sd->status.robe; break; //12
	}E:     	val=sd->status.robe; breakSCRIPT_CMD_SUCCESS //12
	}

	script_pushint(st,val);
	return 0;
}

/*==G===================================
 *     get char save point. argument: 0- map name, 1- x, 2- y
 *------------------------------------------*/
BUILDIN_FUNC(getsavepoint)
{
	TBL_PC* ( sd == NULL ) 
		return 1;

	type = script_getnum(st,2);

	switch( type 

	type = script_getnum(st,2);

	switch(type) {
		case 0: script_pushstrcopy(st,mapindex_id2name(sd->status.save_point.map)); break;
		case 1: script_pushint(st,sd->status.save_point.x); break;
		case 2: script_pushint(st,sd->status.save_point.y); break;
		default:
		SCRIPT_CMD_SUCCESSscript_pushint(st,0);
			break;
	}
	return 0;
}

/*==========================================
  * Get position for  char/npc/pet/mob objects. Added by Lorky
  *
  *     int getMapXY(MapName$,MapX,MapY,type,[CharName$]);
  *             where type:
  *                     MapName$ - String variable for output map name
  *                     MapX     - Integer variable for output coord X
  *                     MapY     - Integer variable for output coord Y
  *                     type     - type of object
  *                                0 - Character coord
  *                                1 - NPC coord
  *                                2 - Pet coord
  *                                3 - Mob coord (not released)
  *                                4 - Homun coord
  *                                5 - Mercenary coord
  *                                6 - Elemental coord
  *                     CharName$ - Name object. If miss or "this" the current object
  *
  *             Return:
  *                     0        - success
  *                     -1       - some error, MapName$,MapX,MapY contains unknown value.
  *------------------------------------------*/
BUILDIN_FUNC(getmapxy)
 = NULL;
	int num;
	const char *name;
	char prefix;
	int x,y,type;
	char mapname[MAP_NAME_LENGTH];

	if( !data_isreference(script_getdata(st,2)) ) f( !data_isreference(script_getdata(st,2)) ){
		ShowWarning("script: buildin_getmapxy: not mapname variable\n");
		script_pushint(st,-1);
		return 1;
	}
	i f( !data_isreference(script_getdata(st,3)) ){
		ShowWarning("script: buildin_getmapxy: not mapx variable\n");
		script_pushint(st,-1);
		return 1;
	}
	i f( !data_isreference(script_getdata(st,4)) ){
		ShowWarning("script: buildin_getmapxy: not mapy variable\n");
Possible needly check function parameters on C_STR,C_INT,C_INT
	type = script_getnum(st,5);
	switch( type ) {
		case 0:	//Get Character Position
			if( script_hasdata(st,6) )
				sd = map_nick2sd(script_getstr(st,6));
			else
				sd = script_rid2sd(st);

			if( sd 			else
				sd=script_rid2sd(st);

			if (sd)
				bl = &sd->bl;
			break;
		case 1:	//Get NPC Position
			if( script_hasda = npc_name2id(script_getstr(st,6));
				if( nd )
					bl = &nd->bl;
			} else //In case the origin is not an npc?
				bl = map_id2bl(st->oid);
			break;
		case 2:	//Get Pet Position
			if( script_hasdata(st,6) )
				sd = map_nick2sd(script_getstr(st,6));
			else
				sd = script_rid2sd(st);

			if( sd && sd->pd 		sd=script_rid2sd(st);

			if (sd && sd->pd)
				bl = &sd->pd->bl;
			break;
		case 3:	//Get Mob Position
			break; //Not supp script_hasdata(st,6) )
				sd = map_nick2sd(script_getstr(st,6));
			else
				sd = script_rid2sd(st);

			if( sd && sd->hd )
				bl = &sd->hd->bl;
			break;
		case 5: //Get Mercenary Position
			if( script_hasdata(st,6) )
				sd = map_nick2sd(script_getstr(st,6));
			else
				sd = script_rid2sd(st);

			if( sd && sd->md )
				bl = &sd->md->bl;
			break;
		case 6: //Get Elemental Position
			if( script_hasdata(st,6) )
				sd = map_nick2sd(script_getstr(st,6));
			else
				sd = script_rid2sd(st);

			if( sd && sd->ed 		sd=script_rid2sd(st);

			if (sd && sd->ed)
				bl = &sd->ed->bl;
			break;
		default:
			ShowWarning("stype);
			script_pushint(st,-1);
			return 1;
	}
	if( !bl ) { //No object found.
		script_pushint(st,-1);
		return 0;
	}

	x = bl->x;
	y = bl->y;
	safestrncpy(mapname,map[bl->m].name,MAP_NAME_LENGTH);

	//Set MapName$
	num = st->stack->stack_data[st->start + 2].u.num;
	name = get_str(num&0x00ffffff);
	prefix = *name;

	if( not_server_variable(prefix) )
		sd = script_rid2sd(st);
	else
		sd = NULL;
	set_reg(st,sd,num,name,(void*)mapname,script_getref(st,2));

	//Set MapX
	num = st->stack->stack_data[st->start + 3].u.num;
	name = get_str(num&0x00ffffff);
	prefix = *name;

	if( not_server_variable(prefix) )
		sd = script_rid2sd(st);
	else
		sd = NULL;
	set_reg(st,sd,num,name,(void*)__64BPRTSIZE(x),script_getref(st,3));

	//Set MapY
	num = st->stack->stack_data[st->start + 4].u.num;
	name = get_str(num&0x00ffffff);
	prefix = *name;

	if( not_server_variable(prefix) )
		sd = script_rid2sd(st);
	else
		sd = prefix))
		sd=script_rid2sd(st);
	else
		sd=NULL;
	set_reg(st,sd,num,name,(void*)__64BPRTSIZE(y),script_getref(st,4));

	//Return SSCRIPT_CMD_SUCCESSccess value
	script_pushint(st,0);
	return 0;
}

/*==========================================
 * Allows player to write NPC logs (i.e. Bank NPC, etc) [Lupus]
 *------------------------------------------*/
BUILDIN_FUNC(logmes)
{
	const char *str;
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1;

	str = scrSCRIPT_CMD_SUCCESSpt_getstr(st,2);
	log_npc(sd,str);
	return 0;
}
 = 0;
	const char *str,*event = "";
	TBL_PC *sd;
	struct mob_data *md;
	int tick = gettick();

	sd = script_rid2sd(st);
	if( !sd )
		return 0;

	str	= script_getstr(st,2);
	_class = script_getnum(st,3);
	if( script_hasdata(st,4) )
		timeout = script_getnum(st,4);
	if( script_hasdata(st,5) ) {
		event = script_getstr(st,5);
		check_event(st,vent=script_getstr(st,5);
		check_event(st, event);
	}

	clif_skill_poseffect(&sd->bl,AM_CALLHOMUN,1,sd->bl.x,sd->blsd->bl.m,sd->bl.x,sd->bl.y,str,_class,event,SZ_SMALL,AI_NONE);
	if( md ) {
		md->master_id = SMALL, AI_NONE);
	if (md) {
		md->master_id=sd->bl.id;
		md->special_state.ai = AI_ATTACK;
		if( md->deletetimer != INVAmob_timer_delete);
		md->deletetimer = add_timer(tick + (timeout > 0 ? timeout : 60000),mob_timer_delete,md->bl.id,0);
		mob_spawn,mob_timer_delete,md->bl.id,0);
		mob_spawn (md); //Now it is ready for spawning.
		clif_specia&md->bl,SC_MODECHANGE,100,1,0,MD_AGGRESSIVE,0,SC_MODECHANGE, 100,SCRIPT_CMD_SUCCESS1, 0, MD_AGGRESSIVE, 0, 60000);
	}
	return 0;
}

/*==========================================
 * Checks whether it is daytime/nighttime
 *------------------------------------------*/
BUILDIN_FUNC(isnight)
{
	scSCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(isday)
{
	script_pushint(st,(night_flag == 0));
	return SCRIPT_CMD_SUCCESSipt_pushint(st,(night_flag == 0));
	return 0;
}

/*================================================
 * Check how many items/cards in the list are
 * equipped - used for 2/15's cards patch [celest]
 *------------------------------------------------*/
BUILDINid = 1;
	int ret = 0
	int i, k, id = 1;
	int ret if (sd == NULL)
		return 1;

	for (i = 0; id != 0; i++) {
		short j;

		FETCH(i + 2, id) else id = 0;
		if (id <= 0)
			continue;

		for (j = 0; j < EQI_MAX; j++) {
			short index = sd->equip_index[j];

			if (index < 0)
				continue;

			if (pc_is_same_equip_index((enum equip_index)j, sd->equip_index, index))
				continue;

			if EQI_HEAD_LOW] == index)) continue;
			
			if(!sd->inventory_data[index])
				continue;

			if (itemdb_type(id) != IT_CARD) { //No card. Count amount in inventory.
				if (sd->i nventory_data[index]->nameid == id)
					ret+= sd->status.inventory[inshort k;
ry[index].amount;
			} else { //Count cards.
				if (itemdb_isspecial(sd->status.inventory[index] (k = 0; k < ))
					continue; //No cards
				for(k=0; k<sd->inventory_data[index]->slot; k++) {
					if (sd->status.inventory[index].card[k] == i
	script_pushint(st,ret);
	return SCRIPT_CMD_SUCCESS
		}
	}
	
	script_pushint(st,ret);
	return 0;
}

/*================================================
 * Check whether another card has been
 * equipped - used for 2/15's cards patch [celest]
 * -- Items checked cannot be reused in another
 * card set to prevent exploits
 *------------------------------------------------*/
BUILid = 1 *sd;
	int i, j, k, id = 1;
	int index, flag;
	int ret = -1;
	//Original hash to reverse it when full check fails.
	unsigned int setitem_hash = 0, setitem_sd == NULL)
		return 1;

	setitem_hash = sd->bonus.setitem_hash;
	setitem_hash2 = sd->bonus.setitem_hash2;

	for (i = 0; id != 0; i++) {
		int flag = 0;
		short j;

		FETCH(i + 2, id) else id = 0;
		if (id <= 0)
			continue;
		flag = 0;
		for (j = 0; j < EQI_MAX; j++) {
			short index = sd->equip_index[j];

			if (index < 0)
				continue;

			if (pc_is_same_equip_index((enum equip_index)i, sd->equip_index, index))
				continue;

			if (!sd->inventory_data[index])
				continue;
sd->inventory_data[index])
				continue;
			
			if (itemdb_type(id) != IT_CARD) {
				if (sd->inventory_data[index]->nameid != id)
					continue;
				fshort k;
				flag = 1;
				break;
			} else { //Cards
				if (sd->inventory_data[index]->slot == 0 ||
					itemdb_isspecial(sd->status.inventory[index].card[0]))
					continue;

				for (k = 0; k < sunsigned int hash = 0; d->inventory_data[index]->slot; k++) {
					//New hash system which should supporpment. [Skotlex]
					unsigned int hash = 0;
					if (sd->status.inventory[index].card[k < 5 ? j : j - 5) * 4 + k);
					// Check if card is already used by another set
					if ((j < 5 ? sd->bonus.setitem_hash : sd->bonus.setitem_hash2)&.setitem_hash : sd->bonus.setitem_hash2 ) & hash)
						continue;

					// We have found a match
					flag = 1;
					// Set hash so th < s card cannot be used by another
					if (j<5)
						sd->bonus.setitem_hash |= hash;
					else
						sd->bonus.setitem_hash2 
				break; // Card found
		}
		if (ret == -1)
			ret = flag;
		else
			ret &= flag;
		if (!ret)
			break;
	}
	if (!ret) { //  flag;
		if (!ret) break;
	}
	if (!ret) { //When check fails, restore original hash values. [Skotlex]
		sd->bonus.setitem_hash = setitem_hash;
		sd->bonus.setitem_hash2 = setitemSCRIPT_CMD_SUCCESShash2;
	}
	script_pushint(st,ret);
	return 0;
}

/*================================================
 * Check how many given inserted cards in the CURRENT
 * weapon - used for 2/15's cards patch [Lupus]
 *------------------------------------------------*/
BUILDIN_FUNC(cardscnt)
{
	TBL_PC *sd;
	int i, k, id = 1;
	int ret if (sd == NULL)
		return 1;

	for (i = 0; id != 0; i++) {
		FETCH(i + 2, id) else id = 0;
		if (id <= 0)
			continue;

		index = current_equip_item_index; // We get CURRENT WEAPON inventory index from status.c [Lupus]
		if (index < 0)
			continue;
			
		if (!sd->inventory_data[index])
			continue;

		if (itemdb_type(id) != IT_CARD) {
			if (sd->inventory_data[index]->nameid == id)
				ret inventory_data[index]->nameid == id)
				ret+= sd->status.inventory[index].amount;
		} else {
			if (itemdb_isspecial(sd->status.inv (k = 0; k < ndex].card[0]))
				continue;
			for(k=0; k<sd->inventory_data[index]->slot; k++) {
				if (sd->status.inventory[index].card[k] == id)
						//script_pushint(st,current_equip_item_index);
	return SCRIPT_CMD_SUCCESShint(st,current_equip_item_index);
	return 0;
}

/*=======================================================
 * Returns the refined number of the current item, or an
 * item with inventory index specified
 *--------------------------------------------------- = script_rid2sd(st);

	if (sd == NULL)
		return 1;

sd;
	if ((sd = script_rid2sd(st)) != NULL)
		script_pushint(st,sd->status.int,valueSCRIPT_CMD_SUCCESS;
	} else
		script_pushint(st,-1);
	return 0;
}

/*=======================================================
 * Day/Night controls
 *-------------------------------------------------------*/
		BUILDIN_FUNC(night)
{
	if (night_flag != 1) map_nigSCRIPT_CMD_SUCCESS;
}
imer(night_timer_tid, 0, 0, 1);
	return 0;
		}
BUILDIN_FUNC(day)
{
	if (night_flag != 0) mapSCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===============Unequip [Spectre]
//put>. By default will be case sensitive.
//--------------------------------------int pos;
	TBL_PC *sd = script_rid2sd(st);

	if (sd == NULL)
		return 1;

	pos = script_getnum(st,2);
	if (pos >= 1 && pos <= ARRAYLENGTH(equip)) {
		short i = pc_checkequip(sd,equip[pos - 1]);

		if (i >= 0) {
			pc_unequipitem(sd,i,1|2);
			script_pushint(st,1);
			return 0;
		}
	}

	ShowError("buildin_unequip: No item equipped at pos %d (CID=%d/AID=%d).\n",pos,sd->status.char_id,sd->status.account_id);
	script_pushint(st,0);
	return SCRIPT_CMD_FAILURE;
}

BUILDIN_FUNC(equip)
{
	unsigned short nameid = 0;
	int	i;
	TBL_PC *sd = script_rid2sd(st);
	struct item_data *item_data;

	if (sd == NULL)
		return 1;

	nameid = script_getnum(st,2);
	if id2sd(st);

	nameid=script_getnum(st) {
		ARR_FIND(0,MAX_INVENTORY,i,sd->status.inventory[i].nameid == nameid);
		if (i < MAX_INVENTORY) {
			pc_equipitem(sd,i,item_data->equip);
			script_pushint(st,1);
			return 0;
		}
	}

	ShowError("buildin_equip: Item %hu cannot be equipped\n",nameid);
	script_pushint(st,0);
	return SCRIPT_CMD_FAILUREequipitem(sd,i,item_data->equip)unsigned short nameid;
	int0;
}

BUILDIN_FUNC(autoequip)
{
	int 
	nameid = script_getnum(st,2);
	flag = script_getnum(st,3);

	if ((item_data = itemdb_exists(nameid)) == NULL) {
		ShowError("buildin_autoequip: Invalid item '%hu'.\n", nameid);
		return SCRIPT_CMD_FAILURE;
	}

	if (!itemdb_isequip2(item_data)urn 1;
	}

	if( !itemdb_isequip2(item_data)hu) {
		ShowError("buildin_autoequip: Item '%dSCRIPT_CMD_FAILURE;
	}

	item_data->flag.autoequip = (flag > 0 ? 1 : 0);
	return SCRIPT_CMD_SUCCESSdata->flag.autoequip = flag>0?1:0;
	return 0;
}

BUILDIN_FUNC(setbattleflag)
{
	const char *flag, *value;

	flag = script_getstr(st,2);
	value = script_getstr(st,3);  // HACK: Retrieve number as string (auto-converted) for battle_set_value
	
	if (battle_set_value(flag, value) == 0)
		ShowWarning("buildin_setbattleflag: unknown battle_config flag '%s'\n",flag);
	else
		ShowInfo("buildin_setbattleflag: battle_config flag '%s' iSCRIPT_CMD_SUCCESS now set to '%s'.\n",flag,value);

	return 0;
}

BUILDIN_FUNC(getbattleflag)
{
	const char *flag;
	flag = script_getstr(st,2);
	script_SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// strlen [Valaris]
//-------------------------------------------------------
BUILDIN_FUNC(getstrlen)
{

	const char *str = script_getstr(st,2);
	int len = (str) ? (int)strlen(SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===============isalpha==================================
// strlen [Valaris]
//-------------------------------------------------------
BUILDIN_FUNC(charisalpha)
{
	const char *str=script_getstr(st,2);
	int pos=script_getnum(st,3);

	int val = ( str && pos >= 0 && (unsigned int)pos < strlen(str) ) ? ISALPHA( str[pos] ) SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===================isupper <st===============
// insertchar <string>, <char>, <index>
//-------------------------------------------------------
BUILDIN_FUNC(charisupper)
{
	const char *str = script_getstr(st,2);
	int pos = script_getnum(st,3);

	int val = ( str && pos >= 0 && (unsigned int)pos < strlen(str) ) ? ISUPPER( str[poSCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===================islower <st===============
// insertchar <string>, <char>, <index>
//-------------------------charislow---------------------
BUILDIN_FUNC(charisupper)
{
	const char *str = script_getstr(st,2);
	int pos = script_getnum(st,3);

	int val = ( str && pos >=LOWER( str[pos] ) : 0;

	script_pushint(st,val);
	return SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// charat <str>, <index>
//-------------------------------------------------------
BUILDIN_FUNC(charat) {
	const char *str = script_getstr(st,2);
	int pos = script_getnum(st,3);
	
	if( pos >= 0 && (unsigned int)pos < strlen(str) ) {
		char output[2];
		output[0] = str[pos];
		output[1] = '\0';
		script_pushstrcopy(st, output);
	} SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// setchar <string>, <char>, <index>
//-------------------------------------------------------
BUILDIN_FUNC(setchar)
{
	const char *str = script_getstr(st,2);
	const char *c = script_getstr(st,3);
	int index = script_getnum(st,4);
	char *output = aStrdup(str);

	if(index >= 0 && index < strlen(output))
		output[index] SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===============inser======================================
// setchar <string>, <char>, <index>
//-------------------------inser----------------------------
BUILDIN_FUNC(setchar)
{
	const char *str = script_getstr(st,2);
	const char *c = script_getstr(st,3);
	int index = script_getnum(st,4);
	char *output;
	size_t len = strlen(str);

	if(index < 0)
		index = 0;
	else if(index > len)
		index = len;

	output = (char*)aMalloc(len + 2);

	memcpy(output, str, index);
	output[index] = c[0];
	memcpy(&output[index+1], &str[index], len - index);
	output[len+1] = SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===============delchar <string==============
// insertchar <string>, <char>, <index>
//-------------------------------------------------------
BUILDIN_FUNC(delchar)
{
	const char *str = script_getstr(st,2);
	int index = script_getnum(st,3);
	char *output;
	size_t len = strlen(str);

	if(index < 0 || index > len) {
		//return original
		output = aStrdup(str);
		script_pushstr(st, output);
		return 0;
	}

	output = (char*)aMalloc(len);

	memcpy(output, str, index);
	memcpy(&output[index], &str[index+1], len - inSCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// strtoupper <str>
//-------------------------------------------------------
BUILDIN_FUNC(strtoupper)
{
	const char *str = script_getstr(st,2);
	char *output = aStrdup(str);
	char *cursor = output;

	while (*cursor != '\0') {
		*cursor = TOUPPER(*cursor);
		cursor+SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// strtolower <str>
//-------------------------------------------------------
BUILDIN_FUNC(strtolower)
{
	const char *str = script_getstr(st,2);
	char *output = aStrdup(str);
	char *cursor = output;

	while (*cursor != '\0') {
		*cursor = TOLOWER(*cursor);
		cursor+SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// substr <str>, <start>, <end>
//-------------------------------------------------------
BUILDIN_FUNC(substr)
{
	const char *str = script_getstr(st,2);
	char *output;
	int start = script_getnum(st,3);
	int end = script_getnum(st,4);

	int len = 0;

	if(start >= 0 && end < strlen(str) && start <= end) {
		len = end - start + 1;
		output = (char*)aMalloc(len + 1);
		memcpy(output, &str[start], len);
	} else
		output = (char*)aMalloc(1);

	output[len] = SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// explode OTEst_string_array>, <str>, <delimiter>
// Note: delimiter is limited to 1 char
//-------------------------------------------------------
BUILDIN_FUNC(explode)
{
	struct script_data* data = script_getdata(st, 2);
	const char *str = script_getstr(st,3);
	const char delimiter = script_getstr(st, 4)[0];
	int32 id;
	size_t len = schar *temp;
	const char* name;
	TBL_PC* sd = NULL;

	temp = (char*)aMalloc(len + 1);

	if( !data_isreference(data) ) (len + 1);

	if( !data_isreference(data) )
	{
		ShowError("script:explode: not a variable\n");
		script_repo //Nta(data);
		st->state = END;
		return 1;// not a variable
	}

	id = reference_getid(data);
	start = reference_getindex(data);
	name = reference_getn {
		ShowError("script:ex_variable(*name) )
	{
		ShowError("script:implode: illegal scope\n");
		script_repo //Not supported
	}

	if( !is_string_variable(name) ) orted
	}

	if( !is_string_variable(name) )
	{
		ShowError("script:explode: not string array\n");
		script_repo //Data type mismatch
	}

	if( not_server_variable(*name) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; //No player attached
	}

	while( str[i] != '\0' ) {
		if( str[i] == delimiter && start < SCRIPT_MAX_ARRAYSIZE - 1 ) { //Bter && start < SCRIPT_MAX_ARRAYSIZE-1) { //break at delimiter but ignore after reaching last array index
			temp[j] = '\0';
			set_reg(st, sd, reference_uid(id, start++), name, (void*)temp, referen
			temp[j++] = str[i++];
	}
	//S else {
			temp[j++] = str[i++];
		}
	}
	//set last string
	temp[j] = '\0';
	set_reg(st, sd, reference_uid(id, start), name, (void*)temp, refeSCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// implode <string_array>
// implode <string_array>, <glue>
//-------------------------------------------------------
BUILDIN_FUNC(implode)
{
	s2);
	const char *name;
	int32 glue_len = 0, array_size, id;
	TBL_PC* sd = NULL;
	char *output;

	if( !data_isreference(data) ) {
		ShowError("script:implode: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return 1; //Nta(data);
		st->state = END;
		return 1;// not a variable
	}

	id = reference_getid(data);
	name = reference_getn e(data);

	if( not_array_variable(*name) )
	{
		ShowError("script:implode: illegal scope\n");
		script_repo //Not supported
	}

	if( !is_string_variable(name) ) {
		ShowError("script:img_variable(name) )
	{
		ShowError("script:explode: not string array\n");
		script_repo //Data type mismatch
	}

	if( not_server_variable(*name) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; //No player attached
	}

	//C)
			return 0;// no player attached
	}

	//count chars
	array_size = getarraysize(st, id, reference_getindex(data), is_string_variab array_size == -1 ) { //Empty array check (AmsTaff)
		ShowWarning("script:implode: array length = 0\n");
		output = (char*)aMalloc(sizeof(char) * 5);
		sprintf(output, "%s", "NULL");
	} else {
		const char *glue = NULL, *temp;
		size_t len = 0;
		int i, k = 0;

		for( i = 0; i <= array_size; ++i 
	} else {
		for(i = 0; i <= array_size; ++i) {
			temp = (char*) get_val2(st, reference_uid(id, i), reference_getref(data));
			-1,0);
		}

		//A;
			script_removetop(st, -1, 0);
		}

		//allocate mem
		if( script_hasdata(st,3) ) {
			glue = script_getstr(st,3);
			glue_len = strlen(glue);
			len += glue_len * (array_size);
	B}
		output = (char*)aMalloc(len + 1);

		//build output
		for(i = 0; i < array_size; ++i) {
			temp = (char*) get_val2(st, reference_uid(id, i), reference_getref(data));
			len = strlen(temp);
			memcpy(&output[k], temp, len);
			k += len;
			if(glue_len != 0) {
				memcpy(&output[k], glue, glue_len);
			-1, glue_len;
			}
			script_removetop(st, -1, 0);
		}
		temp = (char*) get_val2(st, reference_uid(id, array_size), reference_getref(data));
		len = strlen(temp);
		memcpy(&output[k], te-1,en);
		k += len;
		script_removetop(st, -1, 0);

		output[k] = '\0SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// sprintf(<format>, ...);
// Implements C sprintf, except format %n. The resulting string is
// returned, instead of being saved in variable by reference.
//------------------------------------	unsigned int len, argc = 0, arg = 0, buf2_len = 0;
	const char* format;
	char* p;
	char* q;
	char* buf  = NULL;
	char* buf2 = NULL;
	struct script_data* data;
	StringBuf final_buf;

	// Fetch init data
	format = script_getstr(st, 2);
	argc = script_lastdata(st)-2;
	len = strlen(format);

	// Skip parsing, where no parsing is required.
	if(len==0){
		script_pushconststr(st,"");
		return 0;
	}

	// Pessimistic alloc
	CREATE(buf, char, len+1);

	// Need not be parsed, just solve stuff like %%.
	if(argc==0){
		memcpy(buf,format,len+1);
		script_pushstrcopy(st, buf);
		aFree(buf);
		return 0;
	}

	safestrncpy(buf, format, len+1);

	// Issue sprintf for each parameter
	StringBuf_Init(&final_buf);
	q = buf;
	while((p = strchr(q, '%'))!=NULL){
		if(p!=q){
			len = p-q+1;
			if(buf2_len<len){
				RECREATE(buf2, char, len);
				buf2_len = len;
			}
			safestrncpy(buf2, q, len);
			StringBuf_AppendStr(&final_buf, buf2);
			q = p;
		}
		p = q+1;
		if(*p=='%'){  // %%
			StringBuf_AppendStr(&final_buf, "%");
			q+=2;
			continue;
		}
		if(*p=='n'){  // %n
			ShowWarning("buildin_sprintf: Format %%n not supported! Skipping...\n");
			script_reportsrc(st);
			q+=2;
			continue;
		}
		if(arg>=argc){
			ShowError("buildin_sprintf: Not enough arguments passed!\n");
			if(buf) aFree(buf);
			if(buf2) aFree(buf2);
			StringBuf_Destroy(&final_buf);
			script_pushconststr(st,"");
			return 1;
		}
		if((p = strchr(q+1, '%'))==NULL){
			p = strchr(q, 0);  // EOS
		}
		len = p-q+1;
		if(buf2_len<len){
			RECREATE(buf2, char, len);
			buf2_len = len;
		}
		safestrncpy(buf2, q, len);
		q = p;

		// NOTE: This assumes the passed value being the correct
		// type to the current format specifier. If not, the server
		// probably crashes or returns anything else, than expected,
		// but it would behave in normal code the same way so it's
		// the scripter's responsibility.
		data = script_getdata(st, arg+3);
		if(data_isstring(data)){  // String
			StringBuf_Printf(&final_buf, buf2, script_getstr(st, arg+3));
		}else if(data_isint(data)){  // Number
			StringBuf_Printf(&final_buf, buf2, script_getnum(st, arg+3));
		}else if(data_isreference(data)){  // Variable
			char* name = reference_getname(data);
			if(name[strlen(name)-1]=='$'){  // var Str
				StringBuf_Printf(&final_buf, buf2, script_getstr(st, arg+3));
			}else{  // var Int
				StringBuf_Printf(&final_buf, buf2, script_getnum(st, arg+3));
			}
		}else{  // Unsupported type
			ShowError("buildin_sprintf: Unknown argument type!\n");
			if(buf) aFree(buf);
			if(buf2) aFree(buf2);
			StringBuf_Destroy(&final_buf);
			script_pushconststr(st,"");
			return 1;
		}
		arg++;
	}

	// Append anything left
	if(*q){
		StringBuf_AppendStr(&final_buf, q);
	}

	// Passed more, than needed
	if(arg<argc){
		ShowWarning("buildin_sprintf: Unused arguments passed.\n");
		script_reportsrc(st);
	}

	script_pushstrcopy(st, StringBuf_Value(&final_buf));

	if(buf) aFree(buf);
	if(buf2) aFree(buf2);
	StringBuf_Destroy(&final_buf);

	return SCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// sscanf(<str>, <format>, ...);
// Implements C sscanf.
//----------------------------------	unsigned int argc, arg = 0, len;
	struct script_data* data;
	struct map_session_data* sd = NULL;
	const char* str;
	const char* format;
	const char* p;
	const char* q;
	char* buf = NULL;
	char* buf_p;
	char* ref_str = NULL;
	int ref_int;

	// Get data
	str = script_getstr(st, 2);
	format = script_getstr(st, 3);
	argc = script_lastdata(st) - 3;

	len = strlen(format);

	if(len != 0 && strlen(str) == 0) {
		// If the source string is empty but the format string is not, we return -1
		// according to the C specs. (if the format string is also empty, we shall
		// continue and return 0: 0 conversions took place out of the 0 attempted.)
		script_pushint(st, -1);
		return 0;
	}

	CREATE(buf, char, len * 2 + 1);

	// Issue sscanf for each parameter
	*buf = 0;
	q = format;
	while((p = strchr(q, '%'))) {
		if(p != q) {
			strncat(buf, q, (size_t)(p - q));
			q = p;
		}
		p = q + 1;
		if(*p == '*' || *p == '%') { // Skip
			strncat(buf, q, 2);
			q += 2;
			continue;
		}
		if(arg >= argc) {
			ShowError("buildin_sscanf: Not enough arguments passed!\n");
			script_pushint(st, -1);
			if(buf)
				aFree(buf);
			if(ref_str)
				aFree(ref_str);
			return 1;
		}
		if((p = strchr(q + 1, '%')) == NULL)
			p = strchr(q, 0); // EOS
		len = p - q;
		strncat(buf, q, len);
		q = p;
		// Validate output
		data = script_getdata(st, arg + 4);
		if(!data_isreference(data) || !reference_tovariable(data)) {
			ShowError("buildin_sscanf: Target argument is not a variable!\n");
			script_pushint(st, -1);
			if(buf)
				aFree(buf);
			if(ref_str)
				aFree(ref_str);
			return 1;
		}
		buf_p = reference_getname(data);
		if(not_server_variable(*buf_p) && (sd = script_rid2sd(st)) == NULL) {
			script_pushint(st, -1);
			if(buf)
				aFree(buf);
			if(ref_str)
				aFree(ref_str);
			return 0;
		}
		// Save value if any
		if(buf_p[strlen(buf_p) - 1] == '$') { // String
			if(ref_str == NULL)
				CREATE(ref_str, char, strlen(str) + 1);
			if(sscanf(str, buf, ref_str) == 0)
				break;
			set_reg(st, sd, reference_uid(reference_getid(data), reference_getindex(data)), buf_p, (void *)(ref_str), reference_getref(data));
		} else { // Number
			if(sscanf(str, buf, &ref_int) == 0)
				break;
			           break;
            }
            set_reg(st, sd, reference_uid( reference_getid(data), reference_getindex(data) ), buf_p, (void *)__64		}
		arg++;

		// Disable used format (%... -> %*...)
		buf_p = strchr(buf, 0);
		memmove(buf_p - len + 2, buf_p - len + 1, len);
		*(buf_p - len + 1) = '*';
	}

	script_pushint(st, arg);
	if(buf)
		aFree(buf);
	if(ref_str)
		aFree(ref_str);
 len - inSCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//=======================================================
// strpos(<haystack>, <needle>)
// strpos(<haystack>, <needle>, <offset>)
//
// Implements PHP style strpos. Adapted from code from
// http://www.daniweb.com/code/snippet313.html, Dave Sinkula
//-------------------------------------------------------
BUILDIN_FUNC(strpos) {
	const char *haystack = script_getstr(st,2);
	const char *needle = script_getstr(st,3);
	int i;
	size_t len;

	if( script_hasdata(st,4) )
		( needle[0] == '\0' ) {
		script_pushint(st,-1);
		return 0;
	}

	len = strlen(haystack);
	for( ; i < len; ++i ) {
		if( haystack[i] == *needle ) {
			//M {
		if ( haystack[i] == *needle ) {
			// matched starting char -- loop th
			for( h = &haystack[i], n = needle; *h && *n; ++h, ++n ) {
				if( *h != *n ) {
					break;
				}
			}
			if( !*n ) { //Matched all of 'needle' to null termination
				script_pushint(st,i);
				return 0;
			}
		}
	}
	script_pushint(st,-ag != 0) mapSCRIPT_CMD_SUCCESS] ) : 0;

	script_pushint(st,val);
	return 0;
}

//===============================================================
// replacestr <input>, <searchOTE<replace>{, <usecase>{, <count>}}
//
// Note: Finds all instances of <search> in <input> and replaces
// with <replace>. If specified will only replace as many
// instances as specified in <count>. By default will be case
// sensitive.
//---------------------------------------------------------------
BUILDIN_FUNC(replaces2);
	const char *find = script_getstr(st,3);
	const char *replace = script_getstr(st,4 2);
	const char *find = script_getstr(st, 3);
	size_t inputlen = strlstruct StringBuf output;
	bool usecase = true;
	int count = 0;
	int numFinds = 0;
	int i = 0, f = 0;

	if( findlen == 0 ) {
		ShowError("script:replacestr: Invalid search length.\n");
		st->state = END;
		return 1;
	}

	if( script_hasdata(st,5) ) {
		struct script_data *data = script_getdata(st,5);

		get_val(st,data); //Convert into value in case of a variable
		if( !data_isstring(data) )
			usecase = script_getnum(st,5) != 0;
		else {
			ShowError("script:replace4) != 0;
		else {
			ShowError("script:countstr: Invalid usecase value. Expected int got string\n")if( script_hasdata(st,6) ) {
		count = script_getnum(st,6);
		if( count == 0 ount = script_getnum(st, 6);
		if(count == 0) {
			ShowError("script:replacestr: Invalid count value. Expected int got string\n");
			st->state = END;
			return ; i < inputlen; i++ ) {
		if( count && count == numFinds ) //Found enough, stop looking
			break;

		for( f = 0; f <= findlen; f++ ) {
			if( f == findlen ) { //Complete match
				numFinds++;
				StringBuf_AppendStr(&output,replace);
				i += findlen - 1;
				break;
			} else {
				if( usecase ) {
					if( (i + f) > inputlen || input[i + f] != find[f] ) {
						StringBuf_Printf(&output,"%c",input[i]);
						break;
					}
				} else {
					if( );
						break;
					}
				} else {
					if(((i + f) > inputlen || + f]) != TOUPPER(find[f]) ) {
						StringBuf_Printf(&output,"%c",input[i]);
						break;
					}
				}
			}
		}
	}

	//Append excess after enough found
	if( i < inputlen )
		StringBuf_AppendStr(&output,&(input[i]));

	script_pushstrcopy(st,put, &(input[i]));

	script_pushstrcopy(st, StringBuf_Value(&outSCRIPT_CMD_SUCCESSut));
	StringBuf_Destroy(&output);
	return 0;
}

//========================================================
// couOTEtr <input>, <search>{, <usecase>}
//
// Note: Counts the number of times <search> occurs in
// <input>. By default will be case sensitive.
//--------------------------------------------------------
BUILDIN_FUNC(countstr)
{
	const char *input = script_getstr(st, 2);
	const char *find = script_getstr(st, 3);
	size_t inputlen = strlen(input);
	size_t fi	int numFinds = 0;
	int i = 0, f = 0;

	if( findlen == 0 ) {
		ShowError("script:countstr: Invalid search length.\n");
		st->state = END;
		return 1;
	}

	if( script_hasdata(st,4) ) {
		struct script_data *data = script_getdata(st,4);

		get_val(st, data); //Convert into value in case of a variable
    if( !data_isstring(data) )
			usecase = script_getnum(st,ring(st,4) )
			usecase = script_getnum(st, 4) != 0;
		else {
			ShowError("script:countstr: Invalid usecase value. Expected int got string\n");
		 ; i < inputlen; i++ ) {
		for( f = 0; f <= findlen; f++ ) {
			if( f == findlen ) { //C<= findlen; f++) {
			if(f == findlen) { //complete match
				numFinds++;
				i += usecase ) {
					if( (i + f) > inputlen || input[i + f] != find[f] )
						break;
				} else {
					if( );
						break;
					}
				} else {
					if(((i + f) > inputlen || + f]) != TOUPPER(find[f]) )
						break;
				}
			}
		}
	}
	script_pushint(st,				}
				}
			}
		SCRIPT_CMD_SUCCESS
	}
	script_pushint(st, numFinds);
	return 0;
}


/// Changes the display name and/or display class of the npc.
/// Returns 0 is successful, 1 if the npc does not exist.
///
/// setnpcdisplay("<npc name>", "<new display name>", <new class id>, <new size>) -> <int>
/// setnpcdisplay("<npc name>", "<new display name>", <new class id>) -> <int>
/// setnpcdisplay("<npc name>", "<new display name>") -> <int>
/// setnpcdisplay("<npc name>", <new class id>) -> <int>
BUILDIN_FUNC(setnpcdisplay)
{
	const char* name;
	const char* newname = NULL;
	int class_ = -1, size = -1;
	struct script_data* data;
	struct npc_data* nd;

	name = script_getstr(st,2);
	data = script_getdata(st,3);

	if( script_hasdata(st,4) )
		class_ = script_getnum(st,4);
	if( script_hasdata(st,5) )
		size = script_getnum(st,5);

	get_val(st, data);
	if( data_isstring(data) )
 		newname = conv_str(st,data);
	else if( data_isint(data) )
 		class_ = conv_num(st,data);
	else {
		ShowError("script:setnpcdisplay: expected string or number\n");
		script_reportdata(data);
		return 1;
	}

	ndNot found
		script_pushint(st,1);
		return 0;
	}

	//		script_pushint(st,1);
		return 0;
	}

	// Update npc
	if( newname )
		npc_setdisplayname(nd, newname);

	if( size != -1 && size != (int)nd->size )
		nd->size = size;
	else
		size = -1;

	if( class_ != -1 && nd->class_ != class_ )
		npc_setclRequired to update the visual size
		clif_clearunit_area(&nd->bl, visual size
		clif_clearunit_area(&nd->bl, CLR_OUTSIGHT);
		clif_spawn(&SCRIPT_CMD_SUCCESSd->bl);
	}

	script_pushint(st,0);
	return 0;
}

BUILDIN_FUNC(atoi)
{
	const char *value;
	value = script_getstr(st,2SCRIPT_CMD_SUCCESS;
	scBUILDIN_FUNC(axtoi)
{
	const char *hex = script_getstr(st,2);
	long value = strtol(hex, NULL, 16);

#if LONG_MAX > INT_MAX || LONG_MIN < INT_MIN
	value = cap_value(value, INT_MIN, INT_MAX);
#endif
	script_pushint(st, (int)value);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(strtol)
{
	const char *string = script_getstr(st, 2);
	int base = script_getnum(st, 3);
	long value = strtol(string, NULL, base);

#if LONG_MAX > INT_MAX || LONG_MIN < INT_MIN
	value = cap_value(value, INT_MIN, INT_MAX);
#endif
	script_pushint(st, (int)valuegetstr(st,2SCRIPT_CMD_SUCCESS;
	scriC_pushint(st,atoi(value));
	return 0;
}

// case-insensitive substring search [lordalfa]
BUILDIN_FUNC(compare)
{
	cconst char *message;
	const char *cmpstring;
	message = script_getstr(st,2);
	cmpstring = script_getstr(st,3);
	script_pushint(st,(stSCRIPT_CMD_SUCCESSistr(BUILDIN_FUNC(strcmp)
{
	const char *str1;
	const char *str2;

	str1 = script_getstr(st,2);
	str2 = script_getstr(st,3);
	script_pushint(st,strcmp(str1,str2shint(st,(stSCRIPT_CMD_SUCCESSistr(mesage,cmpstring) != NULL));
	return 0;
}

// [zBuffer] List of mathematics commaands --->
BUILDIN_FUNC(sqrt)
{
	double i, a;
	i = script_getnum(st,2);
	a = sSCRIPT_CMD_SUCCESSrt(i);
	script_pushint(st,(int)a);
	returnn 0;
}

BUILDIN_FUNC(pow)
{
	double i, a, b;
	a = script_getnum(st,2);
	b = script_getnum(st,3);
	i = poSCRIPT_CMD_SUCCESS(a,b);
	script_pushint(st,(int)i);
	return 0;
}

BUILDIN_FUNC(distance)
{
	int x0, y0, x1, y1;

	x0 = script_getnum(st,2);
	y0 = script_getnum(st,3);
	x1 = script_getnum(st,4);
	y1 = script_getnum(st,5);

	script_puSCRIPT_CMD_SUCCESShintst,distance_xy(x0,y0,x1,y1));
	return 0;
}

// <--- [zBuffer] List of mathematics commands

BUILDIN_FUNC(md5)
{
	const char *tmpstr;
	char *md5str;

	tmpstr = script + 1) * sizeof(char));
	MD5_String(tmpstr,md5str);
	script_pushstr(st,md5str);
	return SCRIPT_CMD_SUCCESS;
}

// [zBuffer] List of dynamic var commands --->
BUILDIN_FUNC(setd)
{
	TBL_PC *sd = NULL;
	char varname[100];
	const char *buffer;
	int elem;
	buffer = script_getstr(st,2);

	if( sscanf(buffer,"%99[^[][%d]",varname,&elem) < 2 )
		elem = 0;

	if( not_server_variable(*varname) ) {
		sd = script_rid2sd(st);
		if( sd == NULL ) {
			ShowError("script:setd: no player attached for player variable '%s'\n",buffer);
			return 0;
		}
	}

	if( is_string_variable(varname) )
		setd_sub(st,sd,varname,elem,(void *)script_getstr(st,3),NULL);
	else
		setd_sub(st,sd,varname,elem,(void *)__64BPRTSIZE(script_getnum(st,3)),NULL);_64BPRTSIZESCRIPT_CMD_SUCCESSscript_getnum(st, 3)), NULL);
	}
	
	return 0;
}

int buildin_query_sql_sub(struct script_state* st, Sql* handle)
{
	int i, j;
	TBL_PC* sd = NULL;
	const char* query;
	struct script_data* data;
	const char* naMaximum number of rows
	int num_vars;
	int num_cols;

	//Cof rows
	int num_vars;
	int num_cols;

	// check target variables
	for( i = 3; script_hasdati);
		if( data_isreference(data) ) { //It, i);
		if( data_isreference(data) ) { // it's a variable
			name = reference_getname(data);
			if( notRequires a player
				sd = script_rid2sd(st);
				if( sd == NULL ) //No player attached
					return 1;;
					st->state = END;
					return 1;
				}
			}
			if( n //Nrray_variable(*name) )
				max_rows = 1;// not an array, limit to one row
		} else {
			ShowError("script:query_sql: not a variable\n");
			script_reportdata(data);
			st->state = END;			return 1;
		}
	}
	num_vars = i - 3;

	// Execute the query
	query = script_getstr(stquery) ) {
		Sql_ShowDebug(handle);
		script_pushint(st,-1);
		return 1;
	}

	if( Sql_NumRows(handle) == 0 ) { //No data received
		Sql_FreeResult(handle);
		script_pushint(st,-1);
		return 0;
	}

	//	script_pushint(st, 0);
		return 0;
	}

	// Count the number of columns to store
	num_cols = Sql_NumColumns(handle);
	if( num_vars < num_cols ) {
		ShowWarning("script:query_sql: Too m(unsigned int)(num_cols - ast %u columns.\n", (unsigned int)(num_cols-num_vars));
		script_reportsrc(st);
	} else if( num_vars > num_cols ) {
		ShowWarning("script:q(unsigned int)(num_vars - num_cols));
		script_reportsrc(st);
	}

	//num_cols));
		script_reportsrc(st);
	}

	// Store data
	for( i = 0; i < max_rows && SQL_SUCCESS == Sql_NextRow(handle); ++i ) {
		for( j = 0; j < num_vars; ++j ) {
			char* str = NULL;

	j,&str,NULL);

			data = script_getdata(st,j + 3);
			name = reference_getname(data);
			if( is_string_variable(name) )
				setd_sub(st,sd,name,i,(void *)(str ? str : ""),reference_getref(data));
			else
				setd_sub(st,sd,name,i,(void *)__64BPRTSIZE((str ? atoi(str) : 0)),i, (void *)__64BPRTSIZE((str?atoi(str):0)), reference_getref(data));
		}
	}
	if( i == max_rows && max_rows < Sql_NumRows(handle) ) {
		ShowWarning("script:quermax_rows,(unsigned int)Sql_NumRows(handle));
		script_reportsrc(st);
	}

	//Free data
	Sql_FreeResult(handle);
	script_pushint(st,Sql_Freturn SCRIPT_CMD_SUCCESSandle);
	script_pushint(st, i);
		
	return 0;
}

BUILDIN_FUNC(query_sql) {
#ifdef BETA_THREAD_TEST
	if( st->state != RERUNLINE ) {
		queryThr /* Wadd(st,false);

		st->state = RERUNLINE;/* will continue when the query is finished running. */
	} else
		st->state = RUN;
		
	retur 0;
#else
	return buildin_query_sql_sub(st, mmysql_handle);
#endif
}

BUILDIN_FUNC(query_logsql) {
	if( !log_config.sql_logs ) {// logmysql_handle == NULL
		ShowWarning("buildin_query_logsql: SQL logs are diabled, query '%s' will not be executed.\n", script_getstr(st,2));
		script_pushint(st,-1);
		return 1;
	}
#ifdef BETA_THREAD_TEST
	if( st->state != RERUNLINE ) {
		queryThread_add(st,true);
		
		st->state = RERUNLINE;/* will continue when the query is finished running. */
	} else
		st->state = RUN;
	
	retur 0;
#else
	return buildin_query_sql_sub(st, logmysql_handle);
#endif
}

//Allows escaping of a given string.
BUILDIN_FUNC(escape_sql)
{
	const char *str;
	char *esc_str;
	size_t len;

	str = script_getstr(st,2);
	len  * 2 + 1);
	Sql_EscapeStringLen(mmysql_handle,esc_str,str,len);
	script_pushstr(st,le, esc_str, str, SCRIPT_CMD_SUCCESSen);
	script_pushstr(st, esc_str);
	return 0;
}

BUILDIN_FUNC(getd)
{
	char varname[100];
	const char *buf2);

	if(sscanf(buffer,"%99[^[][%d]",varname,&elem) < 2)
		elem = 0;

	//Push the 'pointer' so it's more flexible [Lance]
	push_val(st->stack,C_NAME,reference_uid(add_str(varname),ck, C_NAME, referSCRIPT_CMD_SUCCESSnce_uid(add_str(varname), elem));

	return 0;
}

BUILDIN_FUNC(callshop)
{
	TBL_PC *sd = NULL;
	struct npc_data
	sd = script_rid2sd(st);
	if( sd == NULL )
		return 1;d ) {
		script_pushint(st,0);
	return 0;
	}
	shopname = script_getstr(st, 2);
	if( script_hasdata(st,3) )
		flag = script_getnum(st,3);
	nd = npc_name2id(shopname);
	if( !nd NPCTYPE_SHOP && nd->subtype != NPCTYPE_CASHSHOP && nd->subtype != NPCTYPE_ITEMSHOP && nd->subtype != NPCTYPE_POINT (nd->subtype != SHOP && nd->subtype != CASHSHOP) ) {
		ShowError("buildin_callshop: Shop %s] not found (or NPC is not shop type)\n", shopna
	if( nd->subtype == NPCTYPE_SHOP || nd->subtype == NPCTYPE_ITEMSHOP || nd->subtype == NPCTYPE_POINTst,0);
		return 1;
	}
	
	if( nd->subtype == SHOP ) {
		//Flag the user as using a valid script call for opening the shop (for floating NPCs)
		sd->state.callshop = 1;

		switch( flag ) {
			case 1: npc_buysellsel(sd,nd->bl.id,0); break; //Buy window
			case 2: npc_buysellsel(sd,nd->bl.id,1); break; //Sell window
			default: clif_npcbuysell(sd,nd->bl.id); break; //Shnd);

	sd->npc_shopid = nd->bl.id;
	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(npcshopst,1);
	return 0;
}

BUILDIN_FUNC(npcshopdelitem)
{
	const char* npcname = script_getstr(st,2);
	sint n, i;
	int amount;

	if( !nd || (nd->subtype != NPCTYPE_SHOP && nd->subtype != NPCTYPE_CASHSHOP && nd->subtype != NPCTYPE_ITEMSHOP && nd->subtype != NPCTYPE_POINTSHOP) )
	{ //Not found.
		script_pushint(st,0);
		return 0;
	}

	//Get the count of new entries
	amount = (script_lastdata(st) - 2) / 2;

	//Generate new shop item list
	RECREATE(nd->u.shop.shop_item,struct npc_item_list,amount);
	for( n = 0, i = 3; n < amount; n++, i += 2 ) {
		nd->u.shop.shop_item[n].nameid = script_getnum(st,i);
		nd->u.shop.shop_item[n].value = script_getnum(st,i + 1);
	}
	nd->u.shop.count = n;

	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(npcshopadditem)
{
	const char* npcname = script_getstr(st,2);
	struct npc_data* nd = npc_name2id(npcname);
	int n, i;
	int amount;

	if( !nd || (nd->subtype != NPCTYPE_SHOP && nd->subtype != NPCTYPE_CASHSHOP && nd->subtype != NPCTYPE_ITEMSHOP && nd->subtype != NPCTYPE_POINTSHOP) )
	{ //Not found.
		script_pushint(st,0);
		return 0;
	}

	//Get the count of new entries
	amount = (script_lastdata(st) - 2) / 2;

	//Append new items to existing shop item list
	RECREATE(nd->u.shop.shop_item,struct npc_item_list,nd->u.shop.count + amount);
	for( n = nd->u.shop.count, i = 3; n < nd->u.shop.count+amount; n++, i += 2 ) {
		nd->u.shop.shop_item[n].nameid = script_getnum(st,i);
		nd->u.shop.shop_item[n].value = script_getnum(st,i + 1);
	}
	nd->u.shop.count = n;

	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(npcshopdelitem)
{
	const char* npcname = script_getstr(st,2);
	struct npc_data* nd = npc_name2id(npcname);
	unsigned short nameid;
	int n, i;
	int amount;
	int size;

	if( !nd || (nd->subtype != NPCTYPE_SHOP && nd->subtype != NPCTYPE_CASHSHOP && nd->subtype != NPCTYPE_ITEMSHOP && nd->subtype != NPCTYPE_POINTSHOP) )
	{ //Not found.
		script_pushint(st,0);
		return 0;
	}

	amount = script_lastdata(st) - 2;
	size = nd->u.shop.count;

	//Remove specified items from the shop item list
	for( i = 3; i < 3 + amount; i++ ) {
		nameid = script_getnum(st,i);
		ARR_FIND(0,size,n,nd->u.shop.shop_item[n].nameid == nameid);
		if( n < size ) {
			memmove(&nd->u.shop.shop_item[n],&nd->u.shop.shop_item[n + 1],sizeof(nd->u.shop.shop_item[0]) * (size - n));
			size--;
		}
	}

	RECREATE(nd->u.shop.shop_item,struct npc_item_list,size);
	nd->u.shop.count = size;

	script_pushint(st,1);
	return SCRIPT_CMD_SUCCESS;
}

//Sets a script to attach to a shop npc.
BUILDIN_FUNC(npcshopattach)
{
	const char* npcname = script_getstr(st,2);
	struct npc_data* nd = npc_name2id(npcname);
	int flag = 1;

	if( script_hasdata(st,3) )
		flag = script_getnum(st,3);

	if( !nd || (nd->subtype != NPCTYPE_SHOP && nd->subtype != NPCTYPE_CASHSHOP && nd->subtype != NPCTYPE_ITEMSHOP && nd->subtype != NPCTYPE_POINTSHOP) )
	{ //Not Found.
		script_pushint(st,0);
		return 0;
	}

	if( flag )
		nd->master_nd = ((struct npc_data *)map_id2bl(st->oid));
	else
		nd->master_nd = NULL;

	script_pushint(st,1); data ideSCRIPT_CMD_SUCCESStifier %d\n", type);
			break;
	}

	return 0;
}

/*==Returns some values of an item [Lupus]
 * Price, Weight, etc...
	setitemscript(itemID,"{new item bonus script}",[n]);
   Where n:
	0 - script
	1 - Equip script
	2 - Unequip script
 *------------------------------------------*/
BUILDIN_FUNC(setitemscript)
{
	unsigned short item_id;
	int n = 0;
	const char *script;
	struct item_data *i_data;
	struct script_code **dstscript;

	item_id	= script_getnum(st,2);
	script = script_getstr(st,3);
	if( script_hasdata(st,4) )
		n = script_getnum(st,4);
	i_data = itemdb_exists(item_id);

	if( !i_data || script == NULL || ( script[0] && script[0] != '{' ) ) {
		script_pushint(st,0);
		return 0;
	}
	switch( n ) {
		case 2:
			dstscript = &i_data->unequip_script;
			break;
		case 1:
			dstscript = &i_data->equip_script;
			break;
		default:
			dstscript = &i_data->script;
			break;
	}
	if( *dstscript )
		script_free_code(*dstscript);

	*dstscript = script[0] ? parse_script(script, "script_setitemscript", 0, 0) : NULL;
	script_pushint(st,1tory[i].amoSCRIPT_CMD_SUCCESSnt;
		}
	}

	script_pushint(st,j);
	return 0;
}

=============
 * Add or Update a mob drop temporarily [Akinari]
 * Original Idea By: [Lupus]
 *
 * addmonsterdrop <mob_id or name>,<item_id>,<rate>;
 *
 * If given an item the mob already drops, the rate
 * is updated to the new rate.  Rate cannot exceed 10000
 * Returns 1 if succeeded (added/updated a mob drop)
 *-------------------------------------------------------*/
BUILDIN_FUNC(addmonsterdrop)
{
	struct mob_db *mob;
	struct script_data *data;
	unsigned short item_id;
	int rate;

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	if( data_isstring(data) ULL)
		 = mob_db(mobdb_searchname(script_getstr(st,2)));
	else
		mob = mob_db(script_getnum(st,2));

	item_id = script_getnum(st,3);
	rate = script_getnum(st,4);

	if( !itemdb_exists(item_id) ) {
		ShowError("addmonsterdrop: Nonexistant item %hu requested.\n", item_id );
		return 1;
	}

	if( mob ) { //We got a valid monster, check for available drop slot
		unsigned char i, c = 0;

		for( i = 0; i < MAX_MOB_DROP; i++ ) {
			if( mob->dropitem[i].nameid ) {
				if( mob->dropitem[i].nameid == item_id ) { //If it equals item_id we update that drop
					c = i;
					break;
				}
				continue;
			}
			if( !c ) //Accept first available slot only
				c = i;
		}
		if( c ) { //Fill in the slot with the item and rate
			mob->dropitem[c].nameid = item_id;
			mob->dropitem[c].p = (rate > 10000) ? 10000 : rate;
			script_pushint(st,1);
		} else //No place to put the new drop
			script_pushint(st,0);
	} else {
		ShowWarning("addmonsterdrop: bad mob id given %d\n",script_getnum(st,2));
		return 1;
	}
	return SCRIPT_CMD_SUCCESS	script_pushint(st, instance_id);

	return 0;
}

=============
 * Delete a mob drop temporarily [Akinari]
 * Original Idea By: [Lupus]
 *
 * delmonsterdrop <mob_id or name>,<item_id>;
 *
 * Returns 1 if succeeded (deleted a mob drop)
 *-------------------------------------------------------*/
BUILDIN_FUNC(delmonsterdrop)
{
	struct mob_db *mob;
	struct script_data *data;
	unsigned short item_id;

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	if( data_isstring(data) ULL)
		 = mob_db(mobdb_searchname(script_getstr(st,2)));
	else
		mob = mob_db(script_getnum(st,2));

	item_id = script_getnum(st,3);
	if( !itemdb_exists(item_id) ) {
		ShowError("delmonsterdrop: Nonexistant item %hu requested.\n",item_id);
		return 1;
	}

	if( mob ) { //We got a valid monster, check for item drop on monster
		unsigned char i;

		for( i = 0; i < MAX_MOB_DROP; i++ ) {
			if( mob->dropitem[i].nameid == item_id ) {
				mob->dropitem[i].nameid = 0;
				mob->dropitem[i].p = 0;
				script_pushint(st,1);
				return 0;
			}
		}
		//No drop on that monster
		script_pushint(st,0);
	} else {
		ShowWarning("delmonsterdrop: bad mob id given %d\n",script_getnum(st,2));
		return 1;
	}
	return SCRIPT_CMD_SUCCESS	script_pushint(st, instance_id);

	return 0;
}

/*==Returns some values of a monster [Lupus]
 * Name, Level, race, size, etc...
	getmonsterinfo(monsterID,queryIndex);
 *------------------------------------------*/
BUILDIN_FUNC(getmonsterinfo)
{
	struct mob_db *mob;
	int mob_id;

	mob_id = script_getnum(st,2);
	if( !mobdb_checkid(mob_id) ) {
		//ShowError("buildin_getmonsterinfo: Wrong Monster ID: %i\n", mob_id);
		if( !script_getnum(st,3) ) //Requested a string
			script_pushconststr(st,"null");
		else
			script_pushint(st,-1);
		return 0;
	}
	mob = mob_db(mob_id);
	switch( script_getnum(st,3) ) {
		case 0: script_pushstrcopy(st,mob->jname); break;
		case 1: script_pushint(st,mob->lv); break;
		case 2: script_pushint(st,mob->status.max_hp); break;
		case 3: script_pushint(st,mob->base_exp); break;
		case 4: script_pushint(st,mob->job_exp); break;
		case 5: script_pushint(st,mob->status.rhw.atk); break;
		case 6: script_pushint(st,mob->status.rhw.atk2); break;
		case 7: script_pushint(st,mob->status.def); break;
		case 8: script_pushint(st,mob->status.mdef); break;
		case 9: script_pushint(st,mob->status.str); break;
		case 10: script_pushint(st,mob->status.agi); break;
		case 11: script_pushint(st,mob->status.vit); break;
		case 12: script_pushint(st,mob->status.int_); break;
		case 13: script_pushint(st,mob->status.dex); break;
		case 14: script_pushint(st,mob->status.luk); break;
		case 15: script_pushint(st,mob->status.rhw.range); break;
		case 16: script_pushint(st,mob->range2); break;
		case 17: script_pushint(st,mob->range3); break;
		case 18: script_pushint(st,mob->status.size); break;
		case 19: script_pushint(st,mob->status.race); break;
		case 20: script_pushint(st,mob->status.class_); break;
		case 21: script_pushint(st,mob->status.def_ele); break;
		case 22: script_pushint(st,mob->status.mode); break;
		case 23: script_pushint(st,mob->mexp); break;
		default: script_pushint(st,-1); //Wrong index
	}
	return SCRIPT_CMD_SUCCESS;
}

//Check vending [Nab4]
BUILDIN_FUNC(checkvending)
{
	TBL_PC *sd = NULL----------*/
BUILDIN_FUNC(2) )
		sd = map_nick2sd(script_getstr(st,, 2) )
		sd = map_nick2sd(script_getstr(st, 2))
		script_pushint(st,sd->state.autotrade ? 2 : sd->state.vending);
	else
		script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

//Check chatting [Marka]
BUILDIN_FUNC(checkchatting)
{
	TBL_PC *sd = NULL----------*/
BUILDIN_FUNC(2) )
		sd = map_nick2sd(script_getstr(st,, 2) )
		sd = map_nick2sd(script_getstr(st, 2))
		script_pushint(st,(sd->chatID != 0));
	else
		script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(checkidle)
{
	TBL_PC *sd = NULL----------*/
BUILDIN_FUNC(2) )
		sd = map_nick2sd(script_getstr(st,, 2) )
		sd = map_nick2sd(script_getstr(st, 2))
		script_pushint(st,DIFF_TICK(last_tick,sd->idletime));
	else
		script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(searchitem)
{
	struct script_data* data = script_getdata(st,2);
	const char *itemname = script_getstr(st,3);
	struct item_data *items[MAX_SEARCH];
	int count;
	char* name;
	int32 start;
	int32 id;
	int32 i;
	TBL_PC* sd = NULL;

	if( (items[0] = itemdb_exists(atoi(itemname))) )
		count = 1;
	else {
		count = itemdb_searchname_array(items,ARRAYLENGTH(items),itemname);
		if( count > MAX_SEARCH ) count = MAX_SEARCH;
	}

	if( !count ) {
		script_pushint(st, 0);
		return 0;
	}

	if( !data_isreference(data) ) {
		ShowError("script:searchitem: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return 1; //Not a variable
	}

	id = reference_getid(data);
	start = reference_getindex(data);
	name = reference_getname(data);
	if( not_array_variable(*name) ) {
		ShowError("script:searchitem: illegal scope\n");
		script_reportdata(data);
		st->state = END;
		return 1; //Not supported
	}

	if( not_server_variable(*name) ) {
		sd = script_rid2sd(st);
		if( sd == NULL )
			return 0; //No player attached
	}

	if( is_string_variable(name) ) { //String array
		ShowError("script:searchitem: not an integer array reference\n");
		script_reportdata(data);
		st->state = END;
		return 1; //Not supported
	}

	for( i = 0; i < count; ++start, ++i ) { // Set array
		void* v = (void*)__64BPRTSIZE((int)items[i]->nameid);

		set_reg(st,sd,reference_uid(id,start),name,v,reference_getref(data));
	}

	script_pushint(st,count);
	return SCRIPT_CMD_SUCCESS;
}

//[zBuffer] List of player cont commands --->
BUILDIN_FUNC(rid2name)
{
	struct block_list *bl = NULL;
	int rid = script_getnum(st,2);

	if( (bl = map_id2bl(rid)) ) {
		switch( bl->type ) {
			case BL_MOB: script_pushstrcopy(st,((TBL_MOB*)bl)->name); break;
			case BL_PC:  script_pushstrcopy(st,((TBL_PC*)bl)->status.name); break;
			case BL_NPC: script_pushstrcopy(st,((TBL_NPC*)bl)->exname); break;
			case BL_PET: script_pushstrcopy(st,((TBL_PET*)bl)->pet.name); break;
			case BL_HOM: script_pushstrcopy(st,((TBL_HOM*)bl)->homunculus.name); break;
			case BL_MER: script_pushstrcopy(st,((TBL_MER*)bl)->db->name); break;
			default:
				ShowError("buildin_rid2name: BL type unknown.\n");
				script_pushconststr(st,"");
				break;
		}
	} else {
		ShowError("buildin_rid2name: invalid RID\n");
		script_pushconststr(st,"(null)");
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(pcblockmove)
{
	int id, flag;
	TBL_PC *sd = NULL;

	id = script_getnum(st,2);
	flag = script_getnum(st,3);

	if( id )
		sd = map_id2sd(id);
	else
		sd = script_rid2sd(st);

	if( sd )
		sd->state.blockedmove = flag > 0;

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(pcfollow)
{
	int id, targetid;
	TBL_PC *sd = NULL;

	id = script_getnum(st,2);
	targetid = script_getnum(st,3);

	if( id )
		sd = map_id2sd(id);
	else
		sd = script_rid2sd(st);

	if( sd )
		pc_follow(sd, targetid);

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(pcstopfollow)
{
	int id;
	TBL_PC *sd = NULL;

	id = script_getnum(st,2);

	if( id )
		sd = map_id2sd(id);
	else
		sd = script_rid2sd(st);

	if( sd )
		pc_stop_following(sd);

	return SCRIPT_CMD_SUCCESS;
}
// <--- [zBuffer] List of player cont commands
// [zBuffer] List of mob control commands --->

/// Makes the unit walk to target position or map
/// Returns if it was successfull
///
/// unitwalk(<unit_id>,<x>,<y>) -> <bool>
/// unitwalk(<unit_id>,<target_id>) -> <bool>
BUILDIN_FUNC(unitwalk)
{
	struct block_list* bl;

	bl = map_id2bl(script_getnum(st,2));
	if( bl == NULL )
		script_pushint(st, 0);
	else if( script_hasdata(st,4) ) {
		int x = script_getnum(st,3);
		int y = script_getnum(st,4);

		if( script_pushint(st, unit_can_reach_pos(bl,x,y,0)) )
			add_timer(gettick() + 50,unit_delay_walktoxy_timer,bl->id,(x<<16)|(y&0xFFFF)); //Need timer to avoid mismatches
	} else {
		struct block_list* tbl = map_id2bl(script_getnum(st,3));

		if( tbl == NULL ) {
			ShowError("script:unitwalk: bad target destination\n");
			script_pushint(st,0);
		} else if( script_pushint(st,unit_can_reach_bl(bl,tbl,distance_bl(bl,tbl) + 1,0,NULL,NULL)) )
			add_timer(gettick() + 50,unit_delay_walktobl_timer,bl->id,tbl->id); //Need timer to avoid mismatches
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Kills the unit
///
/// unitkill <unit_id>;
BUILDIN_FUNC(unitkill)
{
	struct block_list* bl = map_id2bl(script_getnum(st,2));

	if( bl != NULL )
		status_kill(bl);

	return SCRIPT_CMD_SUCCESS;
}

/// Warps the unit to the target position in the target map
/// Returns if it was successfull
///
/// unitwarp(<unit_id>,"<map name>",<x>,<y>) -> <bool>
BUILDIN_FUNC(unitwarp)
{
	int unit_id;
	int map;
	short x;
	short y;
	struct block_list* bl;
	const char *mapname;

	unit_id = script_getnum(st,2);
	mapname = script_getstr(st, 3);
	x = (short)script_getnum(st,4);
	y = (short)script_getnum(st,5);

	if( !unit_id ) //Warp the script's runner
		bl = map_id2bl(st->rid);
	else
		bl = map_id2bl(unit_id);

	if( strcmp(mapname,"this") == 0 )
		map = bl?bl->m:-1;
	else
		map = map_mapname2mapid(mapname);

	if( map >= 0 && bl != NULL )
		script_pushint(st,unit_warp(bl,map,x,y,CLR_OUTSIGHT));
	else
		script_pushint(st,0);

	return SCRIPT_CMD_SUCCESS;
}

/// Makes the unit attack the target.
/// If the unit is a player and <action type> is not 0, it does a continuous
/// attack instead of a single attack.
/// Returns if the request was successful.
///
/// unitattack(<unit_id>,"<target name>"{,<action type>}) -> <bool>
/// unitattack(<unit_id>,<target_id>{,<action type>}) -> <bool>
BUILDIN_FUNC(unitattack)
{
	struct block_list* unit_bl;
	struct block_list* target_bl = NULL;
	struct script_data* data;
	int actiontype = 0;

	//Get unit
	unit_bl = map_id2bl(script_getnum(st,2));
	if( unit_bl == NULL ) {
		script_pushint(st,0);
		return 0;
	}
	data = script_getdata(st,3);
	get_val(st,data);
	if( data_isstring(data) ) {
		TBL_PC* sd = map_nick2sd(conv_str(st,data));
		if( sd != NULL )
			target_bl = &sd->bl;
	} else
		target_bl = map_id2bl(conv_num(st,data));
	//Request the attack
	if( target_bl == NULL ) {
		script_pushint(st, 0);
		return 0;
	}
	//Get actiontype
	if( script_hasdata(st,4) )
		actiontype = script_getnum(st,4);

	switch( unit_bl->type ) {
		case BL_PC:
			clif_parse_ActionRequest_sub(((TBL_PC *)unit_bl),actiontype > 0 ? 0x07 : 0x00,target_bl->id,gettick());
			script_pushint(st,1);
			return 0;
		case BL_MOB:
			((TBL_MOB *)unit_bl)->target_id = target_bl->id;
			break;
		case BL_PET:
			((TBL_PET *)unit_bl)->target_id = target_bl->id;
			break;
		default:
			ShowError("script:unitattack: unsupported source unit type %d\n",unit_bl->type);
			script_pushint(st,0);
			return 1;
	}
	script_pushint(st,unit_walktobl(unit_bl,target_bl,65025,2));
	return SCRIPT_CMD_SUCCESS;
}

/// Makes the unit stop attacking and moving
///
/// unitstop <unit_id>;
BUILDIN_FUNC(unitstop)
{
	int unit_id;
	struct block_list* bl;

	unit_id = script_getnum(st,2);
	bl = map_id2bl(unit_id);
	if( bl != NULL ) {
		unit_stop_attack(bl);
		unit_stop_walking(bl,4);
		if( bl->type == BL_MOB )
			((TBL_MOB*)bl)->target_id = 0;
	}

	return SCRIPT_CMD_SUCCESS;
}

/// Makes the unit say the message
///
/// unittalk <unit_id>,"<message>";
BUILDIN_FUNC(unittalk)
{
	int unit_id;
	const char* message;
	struct block_list* bl;

	unit_id = script_getnum(st,2);
	message = script_getstr(st,3);

	bl = map_id2bl(unit_id);
	if( bl != NULL ) {
		struct StringBuf sbuf;

		StringBuf_Init(&sbuf);
		StringBuf_Printf(&sbuf,"%s : %s",status_get_name(bl),message);
		clif_disp_overhead(bl,StringBuf_Value(&sbuf));
		StringBuf_Destroy(&sbuf);
	}

	return SCRIPT_CMD_SUCCESS;
}

/// Makes the unit do an emotion
///
/// unitemote <unit_id>,<emotion>;
///
/// @see e_* in const.txt
BUILDIN_FUNC(unitemote)
{
	int unit_id;
	int emotion;
	struct block_list* bl;

	unit_id = script_getnum(st,2);
	emotion = script_getnum(st,3);
	bl = map_id2bl(unit_id);
	if( bl != NULL )
		clif_emotion(bl, emotion);

	return SCRIPT_CMD_SUCCESS;
}

/// Makes the unit cast the skill on the target or self if no target is specified
///
/// unitskilluseid <unit_id>,<skill_id>,<skill_lv>{,<target_id>};
/// unitskilluseid <unit_id>,"<skill name>",<skill_lv>{,<target_id>};
BUILDIN_FUNC(unitskilluseid)
{
	int unit_id;
	uint16 skill_id;
	uint16 skill_lv;
	int target_id;
	struct block_list* bl;
	struct script_data *data;

	unit_id  = script_getnum(st,2);
	data = script_getdata(st,3);
	get_val(st,data); //Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,3)) : script_getnum(st,3));
	skill_lv = script_getnum(st,4);
	target_id = (script_hasdata(st,5) ? script_getnum(st,5) : unit_id);
	bl = map_id2bl(unit_id);
	if( bl != NULL )
		unit_skilluse_id(bl,target_id,skill_id,skill_lv);

	return SCRIPT_CMD_SUCCESS;
}

/// Makes the unit cast the skill on the target position.
///
/// unitskillusepos <unit_id>,<skill_id>,<skill_lv>,<target_x>,<target_y>;
/// unitskillusepos <unit_id>,"<skill name>",<skill_lv>,<target_x>,<target_y>;
BUILDIN_FUNC(unitskillusepos)
{
	int unit_id;
	uint16 skill_id;
	uint16 skill_lv;
	int skill_x;
	int skill_y;
	struct block_list* bl;
	struct script_data *data;

	unit_id  = script_getnum(st,2);
	data = script_getdata(st,3);
	get_val(st,data); //Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,3)) : script_getnum(st,3));
	skill_lv = script_getnum(st,4);
	skill_x  = script_getnum(st,5);
	skill_y  = script_getnum(st,6);
	bl = map_id2bl(unit_id);
	if( bl != NULL )
		unit_skilluse_pos(bl,skill_x,skill_y,skill_id,skill_lv);

	return SCRIPT_CMD_SUCCESS;
}
// <--- [zBuffer] List of mob control commands

/// Pauses the execution of the script, detaching the player
///
/// sleep <mili seconds>;
BUILDIN_FUNC(sleep)
{
	int ticks;

	ticks = script_getnum(st,2);
	//Detach the player
	script_detach_rid(st);
	if( ticks <= 0 ) {
		//Do nothing
	} else if( st->sleep.tick == 0 ) { //Sleep for the target amount of time
		st->state = RERUNLINE;
		st->sleep.tick = ticks;
	} else { //Sleep time is over
		st->state = RUN;
		st->sleep.tick = 0;
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Pauses the execution of the script, keeping the player attached
/// Returns if a player is still attached
///
/// sleep2(<mili secconds>) -> <bool>
BUILDIN_FUNC(sleep2)
{
	int ticks;

	ticks = script_getnum(st,2);
	if( ticks <= 0 )
		script_pushint(st,(map_id2sd(st->rid) != NULL));
	else if( !st->sleep.tick ) { //Sleep for the target amount of time
		st->state = RERUNLINE;
		st->sleep.tick = ticks;
	} else { //Sleep time is over
		st->state = RUN;
		st->sleep.tick = 0;
		script_pushint(st,(map_id2sd(st->rid) != NULL));
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Awakes all the sleep timers of the target npc
///
/// awake "<npc name>";
BUILDIN_FUNC(awake)
{
	struct npc_data* nd;
	struct linkdb_node *node = (struct linkdb_node *)sleep_db;

	nd = npc_name2id(script_getstr(st, 2));
	if( nd == NULL ) {
		ShowError("awake: NPC \"%s\" not found\n", script_getstr(st,2));
		return 1;
	}

	while( node ) {
		if( (int)__64BPRTSIZE(node->key) == nd->bl.id ) { //Sleep timer for the npc
			struct script_state* tst = (struct script_state*)node->data;
			TBL_PC* sd = map_id2sd(tst->rid);

			if( tst->sleep.timer == INVALID_TIMER ) { //Already awake???
				node = node->next;
				continue;
			}
			if( (sd && sd->status.char_id != tst->sleep.charid) || (tst->rid && !sd) ) {
				//Char not online anymore / another char of the same account is online - Cancel execution
				tst->state = END;
				tst->rid = 0;
			}

			delete_timer(tst->sleep.timer,run_script_timer);
			node = script_erase_sleepdb(node);
			tst->sleep.timer = INVALID_TIMER;
			if( tst->state != RERUNLINE )
				tst->sleep.tick = 0;
			run_script_main(tst);
		} else
			node = node->next;
	}
	return SCRIPT_CMD_SUCCESS;
}

/// Returns a reference to a variable of the target NPC.
/// Returns 0 if an error occurs.
///
/// getvariableofnpc(<variable>, "<npc name>") -> <reference>
BUILDIN_FUNC(getvariableofnpc)
{
	struct script_data* data;
	const char* name;
	struct npc_data* nd;

	data = script_getdata(st,2);
	if( !data_isreference(data) ) { //Not a reference (aka varaible name)
		ShowError("script:getvariableofnpc: not a variable\n");
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1;
	}

	name = reference_getname(data);
	if( *name != '.' || name[1] == '@' ) { //Not a npc variable
		ShowError("script:getvariableofnpc: invalid scope (not npc variable)\n");
		script_reportdata(data);
		script_pushnil(st);
		st->state = END;
		return 1;
	}

	nd = npc_name2id(script_getstr(st,3));
	if( nd == NULL || nd->subtype != NPCTYPE_SCRIPT || nd->u.scr.script == NULL ) { //NPC not found or has no script
		ShowError("script:getvariableofnpc: can't find npc %s\n",script_getstr(st,3));
		script_pushnil(st);
		st->state = END;
		return 1;
	}

	push_val2(st->stack,C_NAME,reference_getuid(data),&nd->u.scr.script->script_vars);
	return SCRIPT_CMD_SUCCESS;
}

/// Opens a warp portal.
/// Has no "portal opening" effect/sound, it opens the portal immediately.
///
/// warpportal <source x>,<source y>,"<target map>",<target x>,<target y>;
///
/// @author blackhole89
BUILDIN_FUNC(warpportal)
{
	int spx;
	int spy;
	unsigned short mapindex;
	int tpx;
	int tpy;
	struct skill_unit_group* group;
	struct block_list* bl;

	bl = map_id2bl(st->oid);
	if( bl == NULL ) {
		ShowError("buildin_warpportal: NPC is needed\n");
		return 1;
	}

	spx = script_getnum(st,2);
	spy = script_getnum(st,3);
	mapindex = mapindex_name2id(script_getstr(st,4));
	tpx = script_getnum(st,5);
	tpy = script_getnum(st,6);

	if( mapindex == 0 ) {
		ShowError("buildin_warpportal: Target map not found %s.\n",script_getstr(st,4));
		return 1; //Map not found
	}

	group = skill_unitsetting(bl,AL_WARP,4,spx,spy,0);
	if( group == NULL )
		return 1; //Failed
	group->val1 = (group->val1<<16)|(short)0;
	group->val2 = (tpx<<16) | tpy;
	group->val3 = mapindex;

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(openmail)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	mail_openmail(sd);

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(openauction)
{
	TBL_PC* sd;

	sd = script_rid2sd(st);
	if( sd == NULL )
		return 0;

	if( !battle_config.feature_auction ) {
		clif_colormes(sd, color_table[COLOR_RED], msg_txt(1489));
		return 0;
	}

	clif_Auction_openwindow(sd);

	return SCRIPT_CMD_SUCCESS;
}

/// Retrieves the value of the specified flag of the specified cell.
///
/// checkcell("<map name>",<x>,<y>,<type>) -> <bool>
///
/// @see cell_chk* constants in const.txt for the types
BUILDIN_FUNC(checkcell)
{
	int16 m = map_mapname2mapid(script_getstr(st,2));
	int16 x = script_getnum(st,3);
	int16 y = script_getnum(st,4);
	cell_chk type = (cell_chk)script_getnum(st,5);

	script_pushint(st,map_getcell(m,x,y,type));

	return SCRIPT_CMD_SUCCESS;
}

/// Modifies flags of cells in the specified area.
///
/// setcell "<map name>",<x1>,<y1>,<x2>,<y2>,<type>,<flag>;
///
/// @see cell_* constants in const.txt for the types
BUILDIN_FUNC(setcell)
{
	int16 m = map_mapname2mapid(script_getstr(st,2));
	int16 x1 = script_getnum(st,3);
	int16 y1 = script_getnum(st,4);
	int16 x2 = script_getnum(st,5);
	int16 y2 = script_getnum(st,6);
	cell_t type = (cell_t)script_getnum(st,7);
	bool flag = (bool)script_getnum(st,8);
	int x, y;

	if( x1 > x2 ) swap(x1,x2);
	if( y1 > y2 ) swap(y1,y2);

	for( y = y1; y <= y2; ++y )
		for( x = x1; x <= x2; ++x )
			map_setcell(m,x,y,type,flag);
t_pushintSCRIPT_CMD_SUCCESSst,party_add_member(party_id,sd));
	return 0;
}

/*==Mercenary Commands
 *------------------------------------------*/
BUILDIN_FUNC(mercenary_create)
{
	struct map_session_data *sd;
	int class_, contract_time;

	if( (sd = script_rid2sd(st)) == NULL || sd->md || sd->status.mer_id != 0 )
		return 0;

	class_ = script_getnum(st,2);

	if( !mercenary_class(class_) )
		return 0;

	contract_time = script_getnum(st,3);
	mercenary_create(sd,class_,contract_time);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(mercenary_heal)
{
	struct map_session_data *sd = script_rid2sd(st);
	int hp, sp;

	if( sd == NULL || sd->md == NULL )
		return 0;
	hp = script_getnum(st,2);
	sp = script_getnum(st,3);

	status_heal(&sd->md->bl,hp,sp,0);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(mercenary_sc_start)
{
	struct map_session_data *sd = script_rid2sd(st);
	enum sc_type type;
	int tick, val1;

	if( sd == NULL || sd->md == NULL )
		return 0;

	type = (sc|| sd->md == NULL )
		return 0;

	type = (sc_type)script_getnum(st,2);
	tick = script_getnum(st,3);
	val1 = scri&sd->md->bl,type,10000,val1,0,0,0,tick,SCFLAG_FIXEDTICK>md->bl, tySCRIPT_CMD_SUCCESSe, 10000, val1, 0, 0, 0, tick, 2);
	return 0;
}

BUILDIN_FUNC(mercenary_get_calls)
{
	struct map_session_data *sd = script_rid2sd(st);
	int guild;

	if( sd == NULL )
		return 0;

	g ld = script_getnum(st,2);
	switch( guild )
	{
		case ARCH_MERC_GUILD:
			script_pushint(st,sd->status.arch_calls);
			break;
		case SPEAR_MERC_GUILD:
			script_pushint(st,sd->status.spear_calls);
			break;
		case SWORD_MERC_GUILD:
			script_pushint(st,sd->status.sword_calls);
			break;
		def	return SCRIPT_CMD_SUCCESScript_pushint(st,0);
			break;
	}

	return 0;
}

BUILDIN_FUNC(mercenary_set_calls)
{
	struct map_session_data *sd = script_rid2sd(st);
	int guild, value, *calls;

	if( sd == NULL )
		return 0;

	guild = script_getnum(st,2);
	va e = script_getnum(st,3);

	switch( guild )
	{
		case ARCH_MERC_GUILD:
			calls = &sd->status.arch_calls;
			break;
		case SPEAR_MERC_GUILD:
			calls = &sd->status.spear_calls;
			break;
		case SWORD_MERC_GUILD:
			calls = &sd->status.swordInvalid Guild
	}

	*calls += value;
	*calls = cap_value(*calls,0,ls += valu	return SCRIPT_CMD_SUCCESS = cap_value(*calls, 0, INT_MAX);

	return 0;
}

BUILDIN_FUNC(mercenary_get_faith)
{
	struct map_session_data *sd = script_rid2sd(st);
	int guild;

	if( sd == NULL )
		return 0;

	g ld = script_getnum(st,2);
	switch( guild )
	{
		case ARCH_MERC_GUILD:
			script_pushint(st,sd->status.arch_faith);
			break;
		case SPEAR_MERC_GUILD:
			script_pushint(st,sd->status.spear_faith);
			break;
		case SWORD_MERC_GUILD:
			script_pushint(st,sd->status.sword_faith);
			break;
		def	return SCRIPT_CMD_SUCCESScript_pushint(st,0);
			break;
	}

	return 0;
}

BUILDIN_FUNC(mercenary_set_faith)
{
	struct map_session_data *sd = script_rid2sd(st);
	int guild, value, *calls;

	if( sd == NULL )
		return 0;

	guild = script_getnum(st,2);
	va e = script_getnum(st,3);

	switch( guild )
	{
		case ARCH_MERC_GUILD:
			calls = &sd->status.arch_faith;
			break;
		case SPEAR_MERC_GUILD:
			calls = &sd->status.spear_faith;
			break;
		case SWORD_MERC_GUILD:
			calls = &sd->status.swordfaith;
			break;
		default:
			return 0; // Invalid Guild
	}

	0,ls += value;
	*calls = cap_value(*calls, 0, INT_MAX);
	if( mercenary_get_guild(sd->md) == guild )
		clif_mercenSCRIPT_CMD_SUCCESSry_updatestatus(sd,SP_MERCFAITH);

	return 0;
}

/*------------------------------------------
 * Book Reading
 *------------------------------------------*/
BUILDIN_FUNC(readbook)
{
	struct map_session_data *sd;
	int book_id, page;

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	book_id = script_getnum(st,2);
	page = script_getnum(st,3);

	clSCRIPT_CMD_SUCCESSf_readbo
 * Questlog script commands
 */estlog script questinfo)
{
	struct npc_data *nd = map_id2nd(st->oid);
	int quest, icon;
	struct questinfo qi;

	if( nd == NULL || nd->bl.m == -1 )
		return 0;

	quest = script_getnum(st,2);
	icon = script_getnum(st,3);

#if PACKETVER >= 20120410
	if( icon < 0 || (icon > 8 && icon != 9999) || icon == 7 )
		icon = 9999; // Default to nothing if icon id is invalid.
#else
	if( icon < 0 || icon > 7 )
		icon = 0;
	else
		icon = icon + 1;
#endif

	qi.quest_id = quest;
	qi.icon = (unsigned char)icon;
	qi.nd = nd;

	if( script_hasdata(st,4) ) {
		int color = script_getnum(st,4);

		if( color < 0 || color > 3 ) {
			ShowWarning("buildin_questinfo: invalid color '%d', changing to 0\n",color);
			script_reportfunc(st);
			color = 0;
		}
		qi.color = (unsigned char)color;
	}

	qi.hasJob = false;

	if( script_hasdata(st,5) ) {
		int job = script_getnum(st,5);

		if( !pcdb_checkid(job) )
			ShowError("buildin_questinfo: Nonexistant Job Class.\n");
		else {
			qi.hasJob = true;
			qi.job = (unsigned short)job;
		}
	}

	map_add_questinfo(nd->bl.m,&qi);

	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(setquest)
{
	struct map_session_data *sd = script_rid2sd(st);
	unsigned short i;
	int quest_id;

	if( sd == NULL )
		return 1;

	quest_id = script_getnum(st, 2);
	quest_add(sd, quest_id);

	// If questinfo is set, remove quest bubble once quest is set.
	for( i = 0; i < map[sd->bl.m].qi_count; i++ ) {
		struct questinfo *qi = &map[sd->bl.m].qi_data[i];

		if( qi->quest_id == quest_id ) {
#if PACKETVER >= 20120410
			clif_quest_show_event(sd, &qi->nd->bl, 9999, 0);
#else
			clif_quest_show_event(sd, &qi->nd->bl, 0, 0);
#endif
		}
	}
(sd);

	qSCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(eras_COMPLETE);
	return 0;
}

BUILDIN_FUNC(changequest)
{
	strucctif( sd == NULL )
		return 1ata *sd = delete(sd, script_getnum(st, 2));
	return SCRIPT_CMD_SUCCESS_delete(sd, script_getnum(st, 2));
	return 0;
}

BUILDIN_FUNC(completequest)
{
	strucctif( sd == NULL )
		return 1ata *sd = script_rid2sd(st);
	nullpo_ret(sd);

	quest_update_status(sd,SCRIPT_CMD_SUCCESSscript_getnum(st, 2), Q_COMPLETE);
	return 0;
}

BUILDIN_FUNC(changequest)
{
	strucctif( sd == NULL )
		return 1ata *sd = script_rid2sd(st);
	nullpo_ret(sd);

	quest_change(sd, script_gSCRIPT_CMD_SUCCESStnum(st, 2),script_getnum(st, 3));
	return 0;
}

BUILDIN_FUNC(checkquest)
{
	struc	enum quest_check_type type = HAVEQUEST;

	if( sd == NULL )
		return 1;

	if( script_hasdata(st, 3) )
		type = (enum d);

	if( script_hasdata(st, 3) )
		type = (quest_check_type)script_getnum(st, 3);

	script_pushint(st, quest_checkSCRIPT_CMD_SUCCESSsd, script_getnum(isbegin_quest)
{
	struct map_session_data *sd = script_rid2sd(st);
	int i;

	nullpo_ret(sd);

	i = quest_check(sd, script_getnum(st, 2), (enum quest_check_type)HAVEQUEST);
	script_pushint(st, i + (i < 1, quest_checkSCRIPT_CMD_SUCCESSsd, script_getnum(st, 2), type));

	return 0;
}

BUILDIN_FUNC(showevent)
{
	TBL_PC *sd = script_rid2sd(st);
	stricon, color = 0;

	if( sd == NULL || nd == NULL )
		return 0;

	icon = script_getnum(st, 2);
	if( script_hasdata(st, 3) ) {
		color = script_getnum(st, 3);
		if( color < 0 || color > 3 ) {
			ShowWarning("buildin_showevent: invalid color '%d', changing to 0\n", color);
			script_reportfunc(st);
			color = 0;
		}
	}

#if PACKETVER >= 20120410
	if( icon < 0 || (icon > 8 && icon != 9999) || icon == 7 )
		icon = 9999; //Default to nothing if icon id is invalid.
#else
	if( icon < 0 || icon > 7 )
		icon = 0;
	else
		icon = icon + 1;
#endif

	clif_quest_show_event(sd, &nd->bl, icon, colortory[i].amoSCRIPT_CMD_SUCCESSnt;
		}
	}

	script_pushint(st,j);
	return 0;
}

/*==========================================
 * BattleGround System
 *------------------------------------------*/
BUILDIN_FUNC(waitingroom2bg)
{
	struct npc_data *nd;
	struct chat_data *cd;
	const charmapindex = 0, bg_id, n;
	unsigned char i = 0, bg_id, n;
	struct map_session_data *sd;

	if( script_hasdata(st,7) )
		nd = npc_name2id(script_getstr(st,7));
	else
		nd = (struct npc_data *)map_id2bl(st->oid);

	if( nd == NULL || (cd = (struct c t_data *)map_id2bl(nd->chat_id)) == NULL )
	{
		script_pushint(st,0);
		return 0;
	}

	map_name = script_ge {
		mapindex = mapindex_name2id(map_name);
		if( mapindex == 0 ) _nam2id(map_name);
		if( mapindex == 0 )
		{ // Invalid Map
			script_pushint(st,0);
			return 0;
		}
	}

	x = script_getnum(st,3);
	y = scriptLogout Event
	dev = script_getstr(st,6); //Die Event

	if( (bg_id = bg_create(mapindex, x, y, ev, dev)) == 0 ) { //eate(mapindex, x, y, ev, dev)) == 0 )
	{ // Creation failed
		script_pushint(st,0);
		return 0;
	}

	n = cd->users;
	fo {
		struct map_session_data *sd = cd->usersd[i];

		if( sd i++ )
	{
		if( (sd = cd->usersd[i]) != NULL && bg_team_join(bg_id, sd) )
			mapreg_setreg(reference_uid(add_str("$@arenamembers"), i), sd->bl.id);
		else
			mapreg_setreg(reference_uid(add_str("$@arenamembers"), i), 0);
	}

	mapreg_setreg(add_str("$@arenamembersnuSCRIPT_CMD_SUCCESS"), i);
	script_pushint(st,bg_id);
	return 0;
}

BUILDIN_FUNC(waitingroom2bg_single)
{
	const char* map_name;
	struct npc_data *nd;
	struct chat_data *cd;
	struct map_session_data *sd;
	int x, y, mapindex, bg_id;

	bg_id = script_getnum(st,2);
	map_name = script_getstr(st,3);
	if( (mapindex = mapindex_name2id(map_name)) == 0 )
		return 0; // Invalid Map

	x = script_getnum(st,4);
	y = script_getnum(st,5);
	nd = npc_name2id(script_getstr(st,6));

	if( nd == NULL || (cd = (struct chat_data *)map_id2bl(nd->chat_id)) == NULL || cd->users <= 0 )
		return 0;

	if( (sd = cd->usersd[0]) == NULL )
		return 0;

	if( bg_team_join(bg_id, sd) )
	{
		pc_setpos(sd, mapindex, x, y, CLR_TELEPORT);
		script_pushint(st,1)SCRIPT_CMD_SUCCESS
	}
	else
		script_pushint(st,0);

	return 0;
}

BUILDIN_FUNC(bg_team_setxy)
{
	struct battleground_data *bg;
	int bg_id;

	bg_id = script_getnum(st,2);
	if( (bg = bg_team_search(bg_id)) == NULL )
		return 0;

	bg->x = script_getnum(sSCRIPT_CMD_SUCCESS,3);
	bg->y = script_getnum(st,4);
	return 0;
}

BUILDIN_FUNC(bg_warp)
{
	int x, y, mapindex, bg_id;
	const char* map_name;

	bg_id = script_getnum(st,2);
	map_name = script_getstr(st,3);
	if( (mapindex = mapindex_name2id(map_name)) == 0 )
		return 0; // Invalid Map
	x = script_getnum(st,4);
	y = script_getnum(st,5);
	bSCRIPT_CMD_SUCCESS_team_warp(bg_id, mapindex, x, y);
	return 0;
}

BUILDIN_FUNC(bg_monster)
{
	int class_ = 0, x = 0, y = 0, bg_id = 0;
	const char *str,*map, *evt="";

	bg_id  = script_getnum(st,2);
	map    = script_getstr(st,3);
	x      = script_getnum(st,4);
	y      = script_getnum(st,5);
	str    = script_getstr(st,6);
	class_ = script_getnum(st,7);
	if( script_hasdata(st,8) ) evt = script_getstr(st,8);
	check_event(st, evt);
	script_pushint(st, mob_spawnSCRIPT_CMD_SUCCESSbg(map,x,y,str,class_,evt,bg_id));
	return 0;
}

BUILDIN_FUNC(bg_monster_set_team)
{
	struct mob_data *md;
	struct block_list *mbl;
	int id = script_g
	if( id == 0 ||st,2),
		bg_id = script_getnum(st,3);
	
	if( (mbl = map_id2bl(id)) == NULL || mbl->type != BL_MOB )
		return 0;
	md = (TBL_MOB *)mbl;
	md->bg_id = bg_id;

	mob_stop_attack(md);
	mob_stop_walking(md, 0);
	md->target_id = md->attacked_id =SCRIPT_CMD_SUCCESS0;
	clif_charnameack(0, &md->bl);

	return 0;
}

BUILDIN_FUNC(bg_leave)
{
	struct map_session_data *sd = script_rid2sd(st);
	if( sd == NULL || !sd->bg_id )
SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(bg_destroy)
{
	int bg_id = script_getnum(st,2);
	bg_team_delete(bg_id);
	return SCRIPT_CMD_SUCCESSnum(st,2);
	bg_team_delete(bg_id);
	return 0;
}

BUILDIN_FUNC(bg_getareausers)
{
	const char *str;
	int16 m, x0, y0, x1, y1;
	int bg_id;
	int i = 0, ca *bg = NULL;
	struct map_session_data *sd;

	bg_id = script_getnum(st,2);
	str = script_getstr(st,3);

	if( (bg = bg_team_search(bg_id)) = NULL || (m = map_mapname2mapid(str)) < 0 )
	{
		script_pushint(st,0);
		return 0;
	}

	x0 = script_getnum(st,4);
	y0 = script_getnum(st,5);
	x1 = script_getnum(st,6);
	y1 = script_getnum(st {
		struct map_session_data *sd = bg->members[i].sd;

		if( !sd )
	{
		if( (sd = bg->members[i].sd) == NULL )
			continue;
		if( sd->bl.m != m || sd->bl.x < x0 || sd->bl.y < y0 || sd->bl.x > x1 || sd->bl.y > y1 )
			continueSCRIPT_CMD_SUCCESS
		c++;
	}

	script_pushint(st,c);
	return 0;
}

BUILDIN_FUNC(bg_updatescore)
{
	const char *str;
	int16 m;

	str = script_getstr(st,2);
	if( (m = map_mapname2mapid(str)) < 0 )
		return 0;

	map[m].bgscore_lion = script_getnum(st,3);
	map[m].bgscore_eagle = script_getnSCRIPT_CMD_SUCCESSm(st,4);

	clif_bg_updatescore(m);
	return 0;
}

BUILDIN_FUNC(bg_get_data)
{
	struct battleground_data *bg;
	int bg_id = script_getnum(st,2),
		type = script_getnum(st,3);

	if( (bg = bg_team_search(bg_id)) == NULL ) {
		script_pushint(st,0);
		return 0;
	}

	switch( type ) {
		case 0: script_pushint(st, bg->count); break;
		default:
			ShowError("script:bg_get_data: unknown data ideSCRIPT_CMD_SUCCESStifier %d\n", type);
			break;
	}

	return 0;
}

/*==========================================
 * Instancing System
 *------------------------------------------*/
//Returns an Instance ID
//Checks NPC first, then if player is attached we check party
int script_instancegetid(struct script_state* st)
{
	short instance_id = 0;

	struct npc_data *nd;
	if( (nd = map_id2nd(st->oid)) && nd->instance_id > 0 )
		instance_id = nd->instance_id;
	else {
		struct map_session_data *sd;
		struct party_data *p;
		if( (sd = script_rid2sd(st)) != NULL && sd->status.party_id && (p = party_search(sd->status.party_id)) != NULL && p->instance_id )
			instance_id = p->instance_id;
	}

	return instance_id;
}

/*==========================================
 * Creates the instance
 * Returns Instance ID if created successfully
 *------------------------------------------*/
BUILDIN_FUNC(instance_create)
{
	struct map_session_data *sd;

	if((sd = script_rid2sd(st)) == NULL)
		return -1;

	script_pushint(st,instance_create(sd->statuSCRIPT_CMD_SUCCESS.party_id, script_getstr(st, 2)));
	return 0;
}

/*==========================================
 * Destroys an instance (unofficial)
 * Officially instances are only destroyed by timeout
 *
 * instance_destroy {<instance_id>};
 *------------------------------------------*/
BUILDIN_FUNC(instance_destroy)
{
	short instance_id;

	if( script_hasdata(st,2) )
		instance_id = script_getnum(st,2);
	else
		instance_id = script_instancegetid(st);

	if( instance_id <= 0 || instance_id >= MAX_MAP_PER_SERVER ) {
		ShowError("script:instance_destroy: Trying to destroy invalid instance %d.\n", instance_id);
		return 0;
SCRIPT_CMD_SUCCESS}

	instance_destroy(instance_id);
	return 0;
}

/*==========================================
 * Warps player to instance
 * Results:
 *	0: Success
 *	1: Character not in party
 *	2: Party doesn't have instance
 *	3: Other errors (instance not in DB, instance doesn't match with party, etc.)
 *------------------------------------------*/
BUILDIN_FUNC(instance_ (sd = script_rid2sd(st)) != NULL ) {
		if( script_hasdata(st,3) && script_hasdata(st,4) )
			script_pushint(st,instance_enter_position(sd,script_getstr(st,2),script_getnum(st,3),script_getnum(st,4)));
		else
			script_pushint(st,instance_enter(sd,script_getstr(st,2)));
	} ,instance_enter(sd,scriptSCRIPT_CMD_SUCCESSgetstr(st, 2)));
	else
		return 1;
	return 0;

}

/*==========================================
 * Returns the name of a duplicated NPC
 *
 * instance_npcname <npc_name>{,<instance_id>};
 * <npc_name> is the full name of an NPC.
 *------------------------------------------*/
BUILDIN_FUNC(instance_npcname)
{
	const char *str;
	short instance_id = 0;

	struct npc_data *nd;

	str = script_getstr(st,2);
	if( script_hasdata(st,3) )
		instance_id = script_getnum(st,3);
	else
		instance_id = script_instancegetid(st);

	if( instance_id && (nd = npc_name2id(str)) != NULL ) {
		static char npcname[NAME_LENGTH];
		snprintf(npcname, sizeof(npcname), "dup_%d_%d", instance_id, nd->bl.id);
 		script_pushconststr(st,npcname);
	} else {
		ShowError("script:instance_npcname: invalid instance NPC (instance_id: %d, NPC name: \"%s\".)\n", instance_id, str);SCRIPT_CMD_SUCCESS		st->state = END;
		return 1;
	}

	return 0;
}

/*==========================================
 * Returns the name of a duplicated map
 *
 * instance_mapname <map_name>{,<instance_id>};
 *------------------------------------------*/
BUILDIN_FUNC(instance_mapname)
{
 	const chr *str;
	int16 m;
	short instance_id = 0;

 	str = script_getstr(st,2);

	if( script_hasdata(st,3) )
		instance_id = script_getnum(st,3);
	else
		instance_id = script_instancegetid(st);

	// Check that instance mapname is a valid map
	if( !instance_id || (m = instance_mapname2mapid(str,instance_id)) < 0 )
		script_pushconststr(st, "");
	else
		scrSCRIPT_CMD_SUCCESSpt_pushconststr(st, map[m].name);

	return 0;
}

/*==========================================
 * Returns an Instance ID
 *------------------------------------------*/
BUILDIN_FUNC(instance_id)
{
	short instance_id;

	instance_id = script_instancegetid(st);

	if(!instance_id) {
		//ShowError("script:instance_id: No instance attached to NPC or player	script_pushint(st, instance_id);
	return SCRIPT_CMD_SUCCESS	script_pushint(st, instance_id);

	return 0;
}

/*==========================================
 * Warps all players inside an instance
 *
 * instance_warpall <map_name>,<x>,<y>{,<instance_id>};
 *------------------------------------------*/
BUILDIN_FUNC(_data *p;
	struct map_session_data *pl_sd;
	int16 m, i;
	short instance_id;
	const char *mapn;
	int x, y;

	mapn = script_getstr(st,2);
	x    = script_getnum(st,3);
	y    = script_getnum(st,4);
	if( script_hasdata(st,5) )
		instance_id = script_getnum(st,5);
	else
		instance_id = script_instancegetid(st);

	if( !instance_id || (m = map_mapname2mapid(mapn)) < 0 || (m = instance_mapname2mapid(map[m].name,instance_id)) < 0)
		return 0;

	if( !(p = party_search(instance_data[instance_id].party_id)) )
		 {
		struct map_session_data *pl_sd = p->data[i].sd;

		if( pl_sd && map[pl_sd->bl.m].instance_id == instance_id )
			pc_setpos(pl_sd,map_id2index(m),x,y,CLR_TELEPORT)known data ideSCRIPT_CMD_SUCCESStifier %d\n", type);
			break;
	}

	return 0;
}

/*==========================================
 * Broadcasts to all maps inside an instance
 *
 * instance_announce <instance id>,"<text>",<flag>{,<fontColor>{,<fontType>{,<fontSize>{,<fontAlign>{,<fontY>}}}}};
 * Using -1 for <instance id> will auto-detect the id.
 *------------------------------------------*/
BUILDIN_FUNC(instance_announce) {
	int         instance_id = script_getnum(st,2);
	const char *mes         = script_getstr(st,3);
	int         flag        = script_getnum(st,4);
	const char *fontColor   = script_hasdata(st,5) ? script_getstr(st,5) : NULL;
	int         fontType    = script_hasdata(st,6) ? script_getnum(st,6) : 0x190; //Default fontType (FW_NORMAL)
	int         fontSize    = script_hasdata(st,7) ? script_getnum(st,7) : 12;    //Default fontSize
	int         fontAlign   = script_hasdata(st,8) ? script_getnum(st,8) : 0;     //Default fontAlign
	int         fontY       = script_hasdata(st,9) ? sript_getnum(st,9) : 0;     //Default fontY

	int i;

	if( instance_id == -1 ) {
		instance_id = script_instancegetid(st);
	}

	if( !instan )
		return 0nce_data[instance_id] != NULL)
		return true;

	for( i = 0; i < instance_data[instance_id].cnt_map; i+instance_data[instance_id].map[i].m,BL_PC,
			mes,strlen(mes) + 1,flag&BC_COLOR_MASK,fontColor,fontType,fontSize,fontAlign,fontY);
t_pushintSCRIPT_CMD_SUCCESSst,party_add_member(party_id,sd));
	return 0;
}

/*==========================================
 * instance_check_party [malufett]
 * Values:
 * party_id : Party ID of the invoking character. [Required Parameter]
 * amount : Amount of needed Partymembers for the Instance. [Optional Parameter]
 * min : Minimum Level needed to join the Instance. [Optional Parameter]
 * max : Maxium Level allowed to join the Instance. [Optional Parameter]
 * Example: instance_check_party (getcharid(1){,amount}{,min}{,max});
 * Example 2: instance_check_party (getcharid(1),1,1,99);
 *---------------------------------------_party)
{
	struct map_session_data *pl_sd;
	int amount, min, max, i, party_id, c = 0;
	struct party_data *p = NULL;

	amount = scriptAmount of needed Partymembers for the Instance.
	min = script_hasdata(st,4) ? script_getnum(st,4) : 1; //hasdata(st,4) ? script_getnum(st,4) : 1; // Minimum Level needed to join the Instance.
	max  = script_hasdatast,5) ? script_getnum(st,5) : MAX_LEVEL; // Maxium Level allowed to join the Instance.

	if( min < 1 || min > MAX_LEVEL) {
		ShowError("sript:check_party: Invalid min level, %d\n", min);
		return 1;
	} else if(  max < 1 || max > MAX_LEVEL) {
		ShowError("sript:check_party: Invalid max level, %d\n", max);
		return 1;
	}

	if( script_hasdata(st,2) )
		party_id = script_getnum(st,2);
	else return 0;

	if( !(p = party_searc(party_id)) ) {
		script_pushint(st, 0); // Returns false if party does not exist.
		ret {
		struct map_session_data *pl_sd = p->data[i].sd;

		if( pl_sd ) {PARTY;  map_id2bl(pl_sd->bl.id) ) {
				if( pl_sd->status.base_level < min ) {
					script_pushint(st,0);
					return 0;
				} else if( pl_sd->status.base_level > max ) {
					script_pushint(st,0);
					return 0;
				}
				c++;
			}
		}
	}

	if( c < amount )
		script_pushint(st,0); // Not enough Members in the Party to join Instance.
	else
		script_pushint(st,1);
t_pushintSCRIPT_CMD_SUCCESSst,party_add_member(party_id,sd));
	return 0;
}

/*==========================================
 * Custom Fonts
 *------------------------------------------*/
BUILDIN_FUNC(setfont)
{
	struct map_session_data *sd = script_rid2sd(st);
	int font = script_getnum(st,2);

	if( sd == NULL )
		return 0;
	if( sd->status.font != font )
		sd->status.font = font;
	else
		sdSCRIPT_CMD_SUCCESS>status.font = 0;

	clif_font(sd);
	return 0;
}

static int buildin_mobuseskill_sub(struct block_list *bl,va_list ap)
{
	TBL_MOB* md		= (TBL_MOB*)bl;
	struct block_list *tbl;
	in= va_arg(ap,int);
	uint16 skill_lvkill_id		= va_arg(ap,int);
	uint16 skill_lv		= va_arg(ap,int);
	int casttime	= va_arg(ap,int);
	int cancel		= va_arg(ap,int);
	int emotion		= va_arg(ap,intmob_idt target		= va_arg(ap,int);
0:self, 1:target, 2:master, default:random
	switch( target ) 2:master, default:random
	switch( target )
	{
		case 0: tbl = map_id2bl(md->bl.id); break;
		case 1: tbl = map_id2bl(md->target_id); break;
		case 2: tbl =
			 map_id2bl(md->master_id); breDEFAULT_ENEMY_TYPE(md),skill_get_range2(&md->bl,skill_id,skill_lv)); break;
	}

	if( !tbl )
		return 0;

	if( md->ud.skilltimer != INVALID_TIMER ) //if( md->ud.skilltimer != INVALID_TIMER ) // Cancel the casting skill.
		unit_skillcastcancel(bl,0);

	if( skill_get_casttype(skill_id) ==tbl->x,tbl->y,skill_id,skill_lv,casttime,cancel);
	else
		unit_skilluse_id2(&md->bl,tbl->id,skill_id,skill_lv,casttime,cancel);

	clif_emotion(&md->bl,emotion);
t_pushintSCRIPT_CMD_SUCCESSst,party_add_member(party_id,sd));
	return 0;
}

/*==========================================
 * areamobuseskill "Map Name",<x>,<y>,<range>,<Mob ID>,"Skill Name"/<Skill ID>,<Skill Lv>,<Cast Time>,<Cancelable>,<Emotion>,<Target Type>;
 *------------------------------------------*/
BUILDIN_FUNC(arestruct script_data *data;
	int16 m;
	int range, mobid, skill_id, skill_lv, casttime, emotion, target, d,skill_id,skill_lv,casttime,emotion,target,cancel;

	if( (m = map_mapname2mapid(script_getstr(st,2))) < 0 ) {
		ShowError("areamobuseskill: invalid map name.\n");
		return 0;
	}

	center.m = m;
	center.x = script_getnum(st,3);
	center.y = script_getnum(st,4);
	range = script_gdata = script_getdata(st,7);
	get_val(st,data); //Convert into value in case of a variable
	skill_id = (data_isstring(data) ? skill_name2id(script_getstr(st,7)) : script_getnum(st,7)(script_getstr(st,7)) : script_getnum(st,7) );
	if( (skill_lv = script_getnum(st,8)) > battle_config.mob_max_skilllvl )
		skill_lv = battle_config.mob_max_skilllvl;

	casttime = script_getnum(st,9);
	cancel = script_getnum(st,10);
	emotion = script_get
	map_foreachinrange(buildin_mobuseskill_sub,&center,range,BL_MOB,mobid,skill_id,skill_lv,casttime,cancel,emotion,ll_id, skill_lv, SCRIPT_CMD_SUCCESS;
}ime, cancel, emotion, target);
	return 0;
}


BUILDIN_FUNC(progressbar)
{
	struct map_session_data * sd = script_rid2sd(st);
	const char * color;
	unsigned int second;

	if( !st || !sd )
		return 0;

	st->state = STOP;

	color = script_getstr(st,2);
	second = script_getnum(st,3);

	sd->progressbar.npc_id = st->oid;
	sd->progressbar.timeout = gettick() + second * 1000;

	clif_progressbar(sd, strtol(coSCRIPT_CMD_SUCCESSor, (char **)NULL, 0), second);
    return 0;
}

BUILDIN_FUNC(pushpc)
{
	uint8 dir;
	int cells, dx, dy;
	struct map_session_data* sd;

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	dir = script_getnum(st,2);
	cells = script_getnum(st,3);

	if( dir > 7 ) {
		ShowWarning("buildin_pushpc: Invalid direction %d specified.\n", dir);
		script_reportsrc(st);

		dir %= 8;  //Trim spin-over
	}

	if( !cells ) { //Zero distance
		return 0;
	} else if( cells < 0 ) { //Pushing backwards
		dir = (dir + 4)%8;  //Turn around
		cells = -cells;
	}

	dx = dirx[dir];
	dy = diry[dir];

SCRIPT_CMD_SUCCESS;
}_blown(&sd->bl,dx,dy,cells,0);
	return 0;
}


/// Invokes buying store preparation window
/// buyingstore <slots>;
BUILDIN_FUNC(buyingstore)
{
	struct map_session_data* sd;

	if( ( sd = script_rid2sd(st) ) == NULL )
		return 0;

	if( npc_isnear(&sd->bl) ) {
		char output[150];
		sprintf(output,msg_txt(662),battle_config.min_npc_vendchat_distance);
		clif_displaymessage(sd->fd,output);
		return 0;
	}

	buyingstSCRIPT_CMD_SUCCESS;
}tup(sd, script_getnum(st,2));
	return 0;
}



/// Invokes search store info window
/// searchstores <uses>,<effect>;
BUILDIN_FUNC(searchstores)
{
	unsigned short effect;
	unsigned int uses;
	struct map_session_data* sd;

	if( ( sd = script_rid2sd(st) ) == NULL ) {
		return 0;
	}

	uses   = script_getnum(st,2);
	effect = script_getnum(st,3);

	if( !uses ) {
		ShowError("buildin_searchstores: Amount of uses cannot be zero.\n");
		return 1;
	}

	if( effect > 1 ) {
		ShowError("buildin_searchstores: Invalid effect id %hu, specified.\n", effect);
		return 1;
	}

	SCRIPT_CMD_SUCCESS;
}
chstore_open(sd, uses, effect);
	return 0;
}
/// Displays a number as large digital clock.
/// showdigit <value>[,<type>];
BUILDIN_FUNC(showdigit)
{
	unsigned int type = 0;
	int value;
	struct map_session_data* sd;

	if( ( sd = script_rid2sd(st) ) == NULL ) {
		return 0;
	}

	value = script_getnum(st,2);

	if( script_hasdata(st,3) ) {
		type = script_getnum(st,3);

		if( type > 3 ) {
			ShowError("buildin_showdigit: Invalid type %u.\n", type);
			return 1;
		}
	}

	clif_showdigSCRIPT_CMD_SUCCESS;
}
d, (unsigned char)type value);
	return 0;
}
/**
 * Rune Knight
 **/
BUILDIN_FUNC(makerune) {
	TBL_PC* sd;
	if( (sd = script_rid2sd(st)) == NULL )
		return 0;
	clif_skill_produce_mix_list(sd,RK_RUNEMASTERY,24);SCRIPT_CMD_SUCCESS;
}
->itemid = script_getnum(st,2);
	return 0;
}
/**
 * checkdragon() returs 1 if mounting a dragon or 0 otherwise.
 **/
BUILDIN_FUNC(checkdragon) {
	TBL_PC* sd;
	if( (sd = script_rid2sd(st)) == NULL )
		return 0;
	if( pc_isridingdragon(sd) )
		script_pushint(sSCRIPT_CMD_SUCCESS;
}
;
	else
		script_pushint(st,0);
	return 0;
}
/**
 * setdragon({optional Color}) returns 1 on success or 0 otherwise
 * - Toggles the dragon on a RK if he can mount;
 * @param Color - when not provided uses the green dragon;
 * - 1 : Green Dragon
 * - 2 : Brown Dragon
 * - 3 : Gray Dragon * - 4 : Blue Dragon
 * - 5 : Red Dragon
 **/
BUILDIN_FUNC(setdragon) {
	TBL_PC* sd;
	int color = script_hasdata(st,2) ? script_getnum(st,2) : 0;

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;
	if( !pc_checkskill(sd,RK_DRAGONTRAINING) || (sd->class_&MAPID_THIRDMASK) != MAPID_RUNE_KNIGHT )
		script_pushint(st,0);//Doesn't have the skill or it's not a Rune Knight
	else if ( pc_isridingdragon(sd) ) {//Is mounted; release
		pc_setoption(sd, sd->sc.option&~OPTION_DRAGON);
		script_pushint(st,1);
	} else {//Not mounted; Mount now.
		unsigned int option = OPTION_DRAGON1;
		if( color ) {
			option = ( color == 1 ? OPTION_DRAGON1 :
					   color == 2 ? OPTION_DRAGON2 :
					   color == 3 ? OPTION_DRAGON3 :
					   color == 4 ? OPTION_DRAGON4 :
					   color == 5 ? OPTION_DRAGON5 : 0);
			if( !option ) {
				ShowWarning("script_setdragon: Unknown Color %d used; changing to green (1)\n",color);
				option = OPTION_DRAGON1;
			}
		}
		pc_setoption(sd, sd->sc.option|SCRIPT_CMD_SUCCESSption);
		script_pushint(st,1);
	}
	return 0;
}

/**
 * ismounting() returns1 if mounting a new mount or 0 otherwise
  **/
BUILDIN_FUNC(ismounting) {
	TBL_PC* sd;
	if( (sd = scsd->sc.data[SC_ALL_RIDING]ULL )
		return 0;
	if( pc_isridingdragon(sd) )
		script_pushint(sSCRIPT_CMD_SUCCESS;
}
;
	else
		sript_pushint(st,0);
	return 0;
}

/**
 * setmounting() returns 1 on success or 0 otherwise
 * - Toggles new mounts on a player when he can mount
 * - Will fail if the player is mounting a non-new mount, e.g. dragon, peco, wug, etc.
 * - Will unm/
BUILDIN_FUNC(setew mount or 0 otherwise
  **/
BUILDIN_FUNC(ismounting) {
	TBL_PC* sd;
	if( (sd = scsd->sc.option && (sd->sc.option&(OPTION_WUGRIDER|OPTION_RIDING|OPTION_DRAGON|OPTION_MADOGEAR)) ) {
		clif_msgtable(sd->fd,NEED_REINS_OF_MOUNT);
		script_pushint(st,0); //Can't mount with one of these
	} else {
		if( sd->sc.data[SC_ALL_RIDING] )
			status_change_end(&sd->bl,SC_ALL_RIDING,INVALID_TIMER);
		else
			sc_start(&sd->bl,&sd->bl,SC_ALL_RIDING,100,0,-1);
		script_pushint(st,1); //In both cases, return 1.pt_pushint(sSCRIPT_CMD_SUCCESS;
}
; //In both cases, return 1.
	}
	return 0;
}
/**
 * Retrieves quantity of arguments provided to callfunc/callsub.
 * getargcount() -> aount of arguments received in a function
 **/
BUILDIN_FUNC(getargcount) {
	struct script_retinfo* ri;

	if( st->stack->defsp < 1 || st->stack->stack_data[st->stack->defsp - 1].type != C_RETINFO ) {
		ShowError("script:getargcount: used out of function or callsub label!\n");
		st->state = END;
		return 1;
	}
	ri = st->stack->stack_data[st->stack->defsp 	return SCRIPT_CMD_SUCCESS;
}
script_pushint(st, ri->nargs);

	return 0;
}
/**
 * getcharip(<accunt ID>/<character ID>/<character name>)
 **/
BUILDIN_FUNC(getcharip)
Check if a character name is specified */
	if( script_hasdata(st,2) ) {
		struct script_data *data;

		data = script_getdata(st,2);
		get_val(st,data); //Convert into value in case of a variable
		if( data_isstring(data) )
			sd = map_nick2sd(script_getstr(st,2));
		else if (data_isint(data) || script_getnum(st,2)) {
			int id;

			id = script_getnum(st,2);
			sd = (map_id2sd(id) ? map_id2sd(id) : map_charid2sd(id));
		}
	} else
		sd = script_rid2sd(st);

	/* Check for sd and IP */
	if( !sd || !session[sd->fd]->client_addr ) {
		script_pushconststr(st,"");
		return 0;
	}

	/* Return the client ip_addr converted for output */
	if( sd && sd->fd && session[sd->fd] ) {
		/* Initiliaze */
		const char *ip_addr = NULL;
		uint32 ip;

		/* St char *ip_addr = NULL;
		uint32 ip;

		/* set ip, ip_addr and convert to ip and push str */
		ip = session[NULL);
		script_pushstrcopy(st, ip2str(ip, N	return SCRIPT_CMD_SUCCESS;
}
t_pushstrcopy(st, ip_addr);
	}

	return 0;
}
/**
 * is_function(<function nae>) -> 1 if function exists, 0 otherwise
 **/
BUILDIN_FUNC(is_function) {
	const char* str = script_getstr(st,2);

	if( strdb_exists(userfunc_db, str) )
		script_p	return SCRIPT_CMD_SUCCESS;
}

	else
		script_pushint(st,0);

	return 0;
}
/**
 * get_revision() -> retrieve the current svn revision (if available)
 **/
BUILDIN_FUNC(get_revision) {
	const char * revision;

	if ( (revision = get_svn_revision()) != 0 )
		script_pushint(st,atoi(revision	return SCRIPT_CMD_SUCCESS;
}
cript_pushint(st,-1);//unknown

	return 0;
}
/**
 * freeloop(<toggle>) -> toggles thi script instance's looping-	if( script_hasdata(st,2) ) {
		if( script_getnum(st,2) )
			st->freeloop = 1;
		else
			st->freeloop = 0;
	}st->freeloop = 1;
	ese
		st->freelo	return SCRIPT_CMD_SUCCESSscript_pushint(st, st->freeloop);

	retrn 0;
}

/**
 * @commands (script based)
 **/
BUILDIN_FUNC(bindatcmd) {
	const char* atcmd;
	const char* eventName;
	int i, lvel = 0, level2 = 0;
	bool create = false;
	
	atcmd = script_getstr(st,2);
	eventName = script_getstr(st,3);

	if( *atcmd == atcommand_symbol || *atcmd == charcommand_symbo
		level = script_getnum(st,4);
	if( script_hasdata(st,5) )
		t_getnum(st,4);
	if( script_hasdata(st,5) ) level2 = script_getnum(st,5);

	if( atcmd_binding_count == 0 ) {
		CREATE(atcmd_binding,struct atcmd_binding_data*,1);
		
		create = true;
	} else {
		ARR_FIND(0, atcmd_binding_count, i, strcmp(atcmd_binding[i]->command,atc /* Update existent entry */
			safestrncpy(atcmd_binding[i]->npc_event,eventName,cpy(atcmd_binding[i]->npc_event, eventName, 50);
			atcmd_binding[i]->level = level;
			atcmd_binding[i]->lev
	if( create ) {
		i = atcmd_binding_count;
if( create ) {
		i = atcmd_binding_count;
		
		if( atcmd_binding_count++ != 0 )
			RECREATE(atcmd_binding,struc
		CREATE(atcmd_binding[i],struct atcmd_binding_data,1);

		safestrncpy(atcmd_binding[i]->command,atcmd,50);
		safestrncpy(atcmd_binding[i]->npc_event,eventName,cpy(atcmd_binding[i]->npc_event, eventName, 50);
		atcmd_binding[i]->level = levereturn SCRIPT_CMD_SUCCESS_binding[i]->level2 = level2;
	}
	
	return 0;
}

BUILDIN_FUNC(unbindatcmd) {
	const char* atcmd;
	int i =  0;

	atcmd = script_getstr(st, 2);

	if( *atcmd == atcommand_symbol |
	if( atcmd_binding_count == 0 ) {
		script_pushint(st,0);
		return 0;
	}

	ARR_FIND(0,atcmd_binding_count,i,strcmp(atcmd_binding[i]->command,atcmd) == 0);
	if( i < atcmd_binding_count ) {
		int cursor = 0;

		aFree(atcmd_binding[i]);
		atcmd_binding[i] = NULL;
		/* Cnding[i]);
		atcmd_binding[i] = NULL;
		/* compact the list now that we freed a slot somewhere */
		for( i = 0, cursor = 0; i < atcmd_binding_count; i++ ) {
			i
			if( cursor != i ) {
				memmove(&atcmd_binding[cursor],&atcmd_binding[i],sizeof(struct atcmd_binding_data*));
			}
izeof(struct atcmd_binding_data*));
			}
			
			cursor++;
		}

		if( (atcmd_binding_coun
		script_pushint(st,1);
	} else
		script_pushint(st,0); /* Nst, 1);
	} e
	return SCRIPT_CMD_SUCCESSt_pushint(st, 0);/* not found */
	
	return 0;
}

BUILDIN_FUNC(useatcmd)
{
	return atcommand_sub(st,3);
}

BUILDIN_FUNC(checkre)
{
	in num ) {
		case 0:
#ifdef RENEWAL
				script_pushint(st,1);
#else
				script_pushint(st,0);
#endif
			break;
		case 1:
#ifdef RENEWAL_CAST
				script_pushint(st,1);
#else
				script_pushint(st,ushint(st, 1);
#else
				script_pushint(st, 0);
#endif
			break;
		case 21);
#else
				script_pushint(st,0);
#endif
			break;
		case 3:
#ifdef RENEWAL_EXP
				script_pushint(st,1);
#else
				script_pushint(st,0);
#endif
			break;
		case 4:
#ifdef RENEWAL_LVDMG
				script_pushint(st,1);
#else
				script_pushint(st,ushint(st, 1);
#else
				script_pushint(st, 0);
#endif
			break;
		case 1);
#else
				script_pushint(st,0);
#endif
			break;
		case 6:
#ifdef RENEWAL_ASPD
				script_pushint(st,1);
#else
				script_pushint(st,ushint(st, 1);
#else
				script_pushint(st, 0);
#endif
			break;
		default:
			ShowWarning("buildin_checkre: uSCRIPT_CMD_SUCCESSknown parameter.\n");
			break;
	}
	return 0;
{,<sub_group>} */
BUILDIN_FUNC(getrandgroupitem) {
	TBL_PC* sd;
	int i, get_count = 0;
	unsigned short nameid;
	uint16 group = script_getnum(st,2), qty = script_getnum(st,3);
	uint8 sub_group = script_getnum(st,4);
	struct item item_tmp;

	if( !(sd = script_rid2sd(st)) )
		return 0;dgroupi!group	}

	if( group < 1 || group >= MAX_ITEMGROUP ) {
		Sh(%d)!\n",group);
		return 1;
	}

	nameid = itemdb_searchrandomid(group,sub_group);
	memset(&item_tmp,0,sizeof(item_tmp));

	item_tmp.nameid = nameid;
	item_tmp.identify = itemdb_isidentified(nameid);

	if( !qty )
		qty = itemdb_get_randgroupitem_count(group,sub_group,nameid);

	//Check if it's stackable.
	if( !itemdb_isstackable(nameid) ) {
		item_tmp.amount = 1;
		get_count = qty;
	} else {
		item_tmp.amount = qty;
		get_count = 1;
	}

	for( i = 0; i < get_count; i++ ) {
		unsigned char flag = 0;

		//If not pet egg
		if( !pet_create_egg(sd,nameid) ) {
			if( (flag = pc_additem(sd,&item_tmp,item_tmp.amount,LOG_TYPE_SCRIPT)) ) {
				clif_additem(sd,0,0,flag);
				if( pc_candrop(sd,&item_tmp) )
					map_addflooritem(&item_tmp,item_tmp.amount,sd->bl.m,sd->bl.x,sd->bl.y,0,0,0,0);
			}
		}
	}

	return SCRIPT_CMD_SUCCESS;
}

/* getgroupitem <group_id>;
 * Gives item(s) to the attached player based on item group contents
 */
BUILDIN_FUNC(getgroupitem) {
	TBL_PC *sd;
	int group_id = script_getnum(st,2);

	if( !(sd = script_rid2sd(st)) )
		return 0;

	if (itemdb_pc_get_itemgroup(group_id,sd)) {
		ShowError("getgroupitem: Invalid group id '%d' specified.",group_id);
		return 1;d->bl.m,sd->bSCRIPT_CMD_SUCCESS.x,sd->bl.y,0,0,0,0);
			}
		}
	}

	return 0;
}

/* cleanmap <map_name>;
 * cleanarea <map_name>, <x0>, <y0>, <x1>, <y1>; */
static int atcommand_cleanfloor_sub(struct block_list *bl, va_list ap)
{
    nullpo_ret(bSCRIPT_CMD_SUCCESS);
    map_clearflooritem(bl);

    return 0;
}

BUILDIN(cleanmap)
{
	const char *map;
	int16 m = -1;
	int16 x0 = 0, y0 = 2);
	m = map_mapname2mapid(map);
	if( !m )
		return 1;

    if( (script_lastdata(st) - 2) < 4 )
        map_foreachinmap(atcommand_cleanfloor_sub,m,BL_ITEM);
    else {
		x0 = script_getnum(st,3);
		y0 = script_getnum(st,4);
		x1 = script_getnum(st,5);
		y1 = script_getnum(st,6);
        if( x0 > 0 && y0 > 0 && x1 > 0 && y1 > 0 )
            map_foreachinarea(atcommand_cleanfloor_sub,m,x0,y0,x1,y1,tcommand_cleanfloor_sub, m, x0, y0, x1, y1, BL_ITEM);
        else {
            ShowError("cleanarea: invalid coordinate defined!    return SCRIPT_CMD_SUCCESS     return 1;
        }
    }

    return 0;
}

/* Cast a skill on the attached player.
 * npcskill <skill id>, <skill lvl>, <stat point>, <NPC level>;
 * npcskill "<skill name>", <skill lvl>, <stat point>, <NPC level>; */
BUILDIN_FUNC(npcskill)
{
	uint16 skill_id;
	unsigned short skill_level;
	unsigned int stat_point;
	unsigned int npc_level;
	struct 	struct script_data *data;

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	skill_id  = data_isstring(data) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2);
	skill_level	= script_getnum(st,3);
	stat_point	= script_getnum(st,4);
	npc_level	= script_getnum(st,5);
	sd			= script_rid2sd(st);
	nd			= (struct npc_data *)map_id2bl(sd->npc_id);

	if( stat_point > battle_config.max_third_parameter at_point > battle_config.max_third_parameter) {
		ShowError("npcskill: stat point exceeded maximum);
		return 1;
	}

	if( npc_level > MAX_LEVEL ) {
		ShowError("npcskill: level exceeded maximum of %d.\n",MAX_LEVEL);
		return 1;
	}

	if( sd == NULL || nd == NULL ) //Ain't possible, but I don't trust people.
		return 1;

	nd->level = n( !nd->status.hp )
		status_calc_npc(nd,SCO_FIRST);
	else
		status_calc_npc(nd,SCO_NONE);

	if( skill_get_inf(skill_id)&INF_GROUND_SKILL )
		unit_skilluse_pos(&nd->bl,sd->bl.x,sd->bl.y,skill_id,skill_level);
	else
		unit_skilluse_id(&nd->bl,sd->bl.id,skill_id,t_skilluse_id(	return SCRIPT_CMD_SUCCESS;
}

/** Consumes an item
 * consumeitem <item id>;
 * consumeitem "<item name>";
 * @param item: Item ID or name
 item <item id>
 * consumeitem "<itemPC *sd;
	struct script_data *data;
	struct item_data *item_data;

	nullpo_retr(1,(sd = script_rid2sd(st)));

	data = script_getdata(st,2);
	get_val(st,data);

	if( data_isstring(data)) {
		const char *name = conv_str(st,(data)) {
		const char *name = conv_str(st, data);

		if( (item_data = itemdb_searchname(name)) == NULL ) {
			ShowError("buildin_conumeitem: Nonexistant item %s requested.\n", name);
			returunsigned shor1;
		}
	} else if( dataisint(data) ) {
		int nameid = conv_num(st, data);

		if( (item_data = itemdb_exists(nameid)) == NULL ) {
			ShowErhuor("buildin_connameid);
			return 1;
		}
	} else {
		ShowError("buildin_consumeitem: invalid data type for argument #1 (%d).",data->type);
		return 1;
	}

	run_script(item_data->script,0,sd->bl.id,0ipt_pushintSCRIPT_CMD_SUCCESSst,part* Makes====================================
 * Make a player sit/stand.
 * sit {"<character OTEe>"};
 * stand {"<character name>"};
 * Note: Use readparam(Sitting) wh @param name: Player name that will be invoked
 */
BUILDIN_FUNC(sit-------------------------*/
BUILDIN_FUNC(2) )
		sd = map_nick2sd(script_getstr(st,, 2) )
		sd = map_nick2sd(script_getstr(st, 2));
	else
		sd = scr1pt_rid2sd(st);

	if( sd == Npc_setsit(sd);
		skill_sit(sd,1);
		clif_sitting(&sd->bl;
		sk	return SCRIPT_CMD_SUCCESS;
}

/** Makes player to stand
 * @param name: Player name that will be set
 */
BUILDIN_FUNC(stand-------------------------*/
BUILDIN_FUNC(2) )
		sd = map_nick2sd(script_getstr(st,, 2) )
		sd = map_nick2sd(script_getstr(st, 2));
	els )
		return 1ipt_rid2sd(st);

	if( sd == NULL)
		return 0;

	if( pc_issi(sd) kill_sit(sdSCRIPT_CMD_SUCCESS 0);
		* Creates an array of bounded item IDs
 * countbound {<type>};
 * @param type: 0 - All bound items; 1 - Account Bound; 2 - Guild Bound; 3 - Party Bound
 * @return amt: Amount of items found
  *------------------------------------------*/
BUILDIN_FUNC(countbound)
{
	int i, type, j = 0, k = 0;
	TBL_PC *sd;

	if( (sd = scriptscript_getnum(st,2);

	for( i = 0; i < MAX_INVENTORY; i++ ) {
		if( sd->status.inventory[i].nameid > 0 &&
			( sd->status.inventory[i].nameid > 0 && (
			(!type && sd->status.inventory[i].bound > 0) ||
			(ty) )
		d->status.inventory[i].bound == type)
		) ) {
			pc_setrg(sd,reference_uid(add_str("@bound_items"), k),sd->status.inventory[i].nameid);
			k++;
			j += sd->status.inventory[i].amoSCRIPT_CMD_SUCCESSnt;
		}* Creates new party
 * party_create "<party name>"{,<char id>{,<item share>{,<item share type>}}};
 * @param party_name: String of party name that will be created
 * @param char_id: Chara that will be leader of this new party. If no char_id specified, the invoker will be party leader
 * @param item_share: 0-Each Take. 1-Party Share
 * @param item_share_type: 0-Each Take. 1-Even Share
 * @return val: Result value 0-Each Take. 1-Even Share
 * Return values:
 *	-3	- party name is exist
 *	-2	- player is in party already
 *	-1	- player is not found
 *	0	- unknown error
 *	1	- succes*------------------------------------------*/
BUILDIN_FUNC(party_create)
{
	char party_name[NAME_LENGTH];
	int item1 = 0, item2 = 0;
	TBL_PC *sd;

	if( (!script_hasdata(st,3) && !(sd = script_rid2sd(st))) || (script_hasdata(st,3) && !(sd = map_charid2sd(script_getnum(st,3)))) ) {
		script_pushint(st,-1);
		return 0;
	}

	if( sd->status.party_id ) {
		script_pushint(st,-2);
		return 0;
	}

	safestrncpy(party_name,script_ge
	if( party_searchname(party_name) ) {
		script_pushint(st,-3);
		return 0;
	}

	if( script_getnum(st,4) )
		item1 = 1;
;
	}
	if( script_getnum(st,4) )
		item1 = 1;
	if( script_getnum(st,5) )
		item2 = 1;

	party_create_byscript = 1;
	script_pushint(st,party_SCRIPT_CMD_SUCCESSreate(s* Adds player to specified party
 * party_addmember <party id>,<char id>;
 * @param party_id: The party that will be entered by player
 * @param char_id: Char id of player that will be joined to the party
 * @return val: Result value player to specified party
 * Return values:
 *	-4	- party is full
 *	-3	- party is not found
 *	-2	- player is in party already
 *	-1	- player is not *------------------------------------------*/
BUILDIN_FUNC(party_addmember)
{
	int party_id = script_getnum(st,2);
	TBL_PC *sd;
	struct party_data *party;

	if( !(sd = map_charid2sd(script_getnum(st,3))) ) {
		script_pushint(st,-1);
		return 0;
	}

	if( sd->status.party_id ) {
		script_pushint(st,-2);
		return 0;
	}

	if( !(party = party_search(party_id)) ) {
		script_pushint(st,-3);
		return 0;
	}

	if( party->party.count >= MAX_PARTY ) {
		script_pushint(st,-4);
		return 0;
	}
	sd->party_invite = party_id;
	script_pushintSCRIPT_CMD_SUCCESSst,part* Removes player from his/her party. If party_id and char_id is empty remove the invoker from his/her party
 * party_delmember {<char id>,<party_id>};
 * @param: char_id
 * @param: party_id
 * @return val: Result valueinvoker from his/her party
 * Return values:
 *	-3	- player is not in party
 *	-2	- party is not found
 *	-1	- player is not *------------------------------------------*/
BUILDIN_FUNC(party_delmember)
{
	TBL_PC *sd = NULL;

	if( !script_hasdata(st,2) && !script_hasdata(st,3) && !(sd = script_rid2sd(st)) ) {
		script_pushint(st,-1);
		return 0;
	}
	if( sd || (script_getnum(st,2) && (sd = map_charid2sd(script_getnum(st,2)))) )
		script_pushint(st,party_removemember2(sd,0,0));
	else
		script_pushint(st,party_removemember2(NULL,script_SCRIPT_CMD_SUCCESSetnum(s* Changes party leader of specified party (even the leader is offline)
 * party_changeleader <party id>,<char id>;
 * @param party_id: ID of party
 * @param char_id: Char ID of new leader
 * @return val: Result valuen the leader is not online
 * Return values:
 *	-4	- player is party leader already
 *	-3	- player is not in this party
 *	-2	- player is not found
 *	-1	- party is not *------------------------------------------*/
BUILDIN_FUNC(party_changeleader)
{
	int i, party_id = script_getnum(st,2);
	TBL_PC *sd = NULL;
	TBL_PC *tsd = NULL;
	struct party_data *party = NULL;

	if( !(party = party_search(party_id)) ) {
		script_pushint(st,-1);
		return 0;
	}

	if( !(tsd = map_charid2sd(script_getnum(st,3))) ) {
		script_pushint(st,-2);
		return 0;
	}

	if( tsd->status.party_id != party_id ) {
		script_pushint(st,-3);
		return 0;
	}

	ARR_FIND(0,MAX_PARTY,i,party->party.member[i].leader);
	if( i >= MAX_PARTY ) {	//This is should impossible!
		script_pushint(st,0);
		return 0;
	}
	if( party->data[i].sd == tsd ) {
		script_pushint(st,-4);
		return 0;
	}

	script_pushint(stSCRIPT_CMD_SUCCESSparty_c* Changes party option
 * party_changeoption <party id>,<option>,<flag>;
 * @param party_id: ID of party that will be changed
 * @param option: Type of option
 * @return val: -1 - party is not found, 0 - invalid option, 1 - success
  *------------------------------------------*/
BUILDIN_FUNC(party_changeoption)
{
	struct party_data *party;

	if( !(party = party_search(script_getnum(st,2))) ) {
		script_pushint(st,-1);
		return 0;
	}
	script_pushint(st,party_setoption(party,script_SCRIPT_CMD_SUCCESSetnum(s* Destroys party with party id.
 * party_destroy <party id>;
 * @param party_id: ID of party that will be destroyed
 * @return val: Result value
 *  0  - failed
 *  1  - success
  *------------------------------------------*/
BUILDIN_FUNC(party_destroy)
{
	int i;
	struct party_data *party;

	if( !(party = party_search(script_getnum(st,2))) ) {
		script_pushint(st,0);
		return 0;
	}

	ARR_FIND(0,MAX_PARTY,i,party->party.member[i].leader);
	if( i >= MAX_PARTY || !party->>data[i].sd ) { //Leader not online
		int j;
		for( j = 0; j < MAX_PARTY; j++ ) {
			TBL_PC *sd = party->data[j].sd;
			if( sd )
				party_member_withdraw(party->party.party_id,sd->status.account_id,sd->status.char_id);
			else if( party->party.member[j].char_id )
				intif_party_leave(party->party.party_id,party->party.member[j].account_id,party->party.member[j].char_id);
		}
		party_broken(party->party.party_id);
		script_pushint(st,1);
	} else //Leader leave = party broken
		script_pushint(SCRIPT_CMD_SUCCESSt,party*==========================================
* Checks if a player's clien * @param type: 0 - check by version number; 1 - check by date
 * @param data: Input
 * @return val: 1 - true, 0 - false
  *------------------------------------------*/
BUILDIN_FUNC(is_clientver) {
	TBL_PC *sd = NULL;
	int type = script_getnum(st,2);
	int da( script_hasdata(st,4) )
		sd = map_charid2sd(script_getnum(st,4));
	else
		sd = script_rid2sd(st);
	if( sdum(st,3);

	if( (bg = bg_team_search(bg_id)) == NULL ) {
		script_pushint(st,0);;
		return 0;
	}

	switch (type) {
		case 0:
			ret = (sd->packet_ver >= data) ? 1 : 0;
			break;
		case 1:
			ret = (sd->packet_ver >= date2version(data)) ? 1 : 0;
		SCRIPT_CMD_SUCCESSbreak;
* Turns a player into a monster and grants SC attribute effect. [malufett]
 * montransform <monster name/ID>, <duration>, <sc type>, <val1>, <val2>, <val3>, <val4>;
 * @param monster: Monster ID or name
 * @param duration: Transform duration in millisecond (ms)
 * @param sc_type: Type of SC that will be affected during the transformation
 * @param val1: Value for SC
 * @param val2: Value for SC
 * @param val3: Value for SC
 * @param val4: Value for SC
 * @return val: 1 - success, 0 - failed
 */
BUILDIN_FUNC(montransform) {
	TBL_PC *sd;
	enum sc_type type;
	int tick, mob_id, val1, val2, val3, val4;
	struct script_data *data;

	if( (sd = script_rid2sd(st)) == NULL )
		return 0;

	data = script_getdata(st,2);
	get_val(st,data); //Convert into value in case of a variable
	if( data_isstring(data) ULL)
		return 0;

	if (script_isstring(st,2))
		mob_id = mobdb_searchname(script_getstr(st,2));
	else
		mob_id = mobdb_checkid(script_g
	if( script_hasdata(st,4) )
		type = (sc_type)script_getnum(st,4);
	else
		type = SC_NONE;
st,3);
	type = (sc_type)script_getnum( mob_id == 0 ) {
		if( data_isstring(data)  (mob_id == 0) {
		if (script_isstring(st,2))
			ShowWarning("buildin_montransform: Attempted to use non-existing monster '%s'.\n", script_getstr(st,2));
		else
			ShowWarning("buildin_montransform: Attempted to use non-existing monster of I0 '%d'.\n"( !(type >= SC_NONE && type < SC_MAX) ) {
		ShowWarning("buildin_montransform: Unsupported status change id %d\n",type);
		return 0;
	}

	if( script_hasdata(st,5) )
		val1 = script_getnum(st,5);

	if( script_hasdata(st,6) )
		val2 = script_getnum(st,6);

	if( script_hasdata(st,7) )
		val3 = script_getnum(st,7);

	if( script_hasdata(st,8) )
		val4 = script_getnum(st,8);

	if( tick != 0 ) {
		char msg[CHAT_SIZE_MAX];
		struct mob_db *monster =  mob_db(mob_id);

		if( battle_config.mon_trans_disable_in_gvg && map_flag_gvg2(sd->bl.m) ns_disable_in_gvg && map_flag_gvg2(sd->bl.m)) {
			clif_displaymessage(sd->fd,msg_txt(1493)); //Transforming into monster is ot a( sd->disguise d Wars.
			return 0;
		}

		if (sd->disguise) {
			clif_displaymessage(sd->fd,msg_txt(1491)); //Cannot transform into onster while in disguise.
			return 0;
		}

		sprintf(msg,msg_txt(1490),monster->namShowScriptsformation-!! %s form!!
		clif_disp&sd->_overhead(&sd->bl,msg);
		status_change_end(bl,SC_MONSTER_TRANSFORM,INULL,&sd->bl,SC_MONSTER_TRANSFORM,100,mob_id,type,tick);
		if( script_hasdata(st,4) )
			sc_start4(NULL,&sd->bl,type,100,val1,val2,val3,val4,tick);
	}
	return SCRIPT_CMD_SUCCESS;
}

/**
 * Attach script to player for certain duration
 * bonus_script "<script code>",<duration>{,<flag>{,<type>{,<status_icon>{,<char_id>}}}};
 * @param "script code"
 * @param duration
 * @param flag
 * @param icon
 * @param char_id
 * @author [Cydh]
 * @return val: 1 - success, 0 - failed
 */
BUILDIN_FUNC(bonus_script) {
	uint8 i, type = 0;
	uint8 flag = 0;
	int16 icon = SI_BLANK;
	uint32 dur;
	TBL_PC* sd;
	const char *script_str = NULL;
	struct script_code *script = NULL;

	if( script_hasdata(st,7) )
		sd = map_charid2sd(script_getnum(st,7));
	else
		sd = script_rid2sd(st);

	if( sd == NULL )
		return 0;

	script_str = script_getstr(st,2);
	dur = 1000 * abs(script_getnum(st,3));
	FETCH(4,flag);
	FETCH(5,type);
	FETCH(6,icon);

	if( script_str[0] == '\0' || !dur ) {
		//ShowWarning("buildin_bonus_script: Invalid script. Skipping...\n");
		return 0;
	}

	//Skip duplicate entry
	ARR_FIND(0,MAX_PC_BONUS_SCRIPT,i,&sd->bonus_script[i] && sd->bonus_script[i].script_str && strcmp(sd->bonus_script[i].script_str,script_str) == 0);
	if( i < MAX_PC_BONUS_SCRIPT ) {
		//ShowWarning("buildin_bonus_script: Duplicate entry with bonus '%d'. Skipping...\n",i);
		return 1;
	}

	if( !(script = parse_script(script_str,"bonus_script",0,1)) ) {
		ShowWarning("buildin_bonus_script: Failed to parse script '%s' (cid:%d). Skipping...\n",script_str,sd->status.char_id);
		return 0;
	}

	//Find the empty slot
	ARR_FIND(0,MAX_PC_BONUS_SCRIPT,i,!sd->bonus_script[i].script);
	if( i >= MAX_PC_BONUS_SCRIPT ) {
		ShowWarning("buildin_itemscript: Maximum script_bonus is reached (cid:%d max: %d). Skipping...\n",sd->status.char_id,MAX_PC_BONUS_SCRIPT);
		return 1;
	}

	//Add the script data
	memcpy(sd->bonus_script[i].script_str,script_str,strlen(script_str) + 1);
	sd->bonus_script[i].script = script;
	sd->bonus_script[i].tick = gettick() + dur;
	sd->bonus_script[i].flag = flag;
	sd->bonus_script[i].type = type;
	sd->bonus_script[i].icon = icon;

	if (sd->bonus_script[i].icon != SI_BLANK) //Gives status icon if exist
		clif_status_change(&sd->bl,sd->bonus_script[i].icon,1,dur,1,0,0);

	status_calc_pc(sd,SCO_NONE);
	return SCRIPT_CMD_SUCCESS;
}

/**
 * Removes all bonus script from player
 * bonus_script_clear {<flag>,{<char_id>}};
 * @param flag 0 - Except permanent bonus, 1 - With permanent bonus
 * @param char_id Clear script from this character
 * @author [Cydh]
 */
BUILDIN_FUNC(bonus_script_clear) {
	TBL_PC* sd;
	bool flag = 0;

	if (script_hasdata(st,2))
		flag = script_getnum(st,2);

	if (script_hasdata(st,3))
		sd = map_charid2sd(script_getnum(st,3));
	else
		sd = script_rid2sd(st);

	if (sd == NULL)
		return 1;

	pc_bonus_script_clear_all(sd,flag); /// Don't remove permanent script
	return SCRIPT_CMD_SUCCESS;
}

/** Returns various information about a player's VIP status. Need to enable VIP system
 * vip_status <type>,{"<character name>"};
 * @param type: Info type, 1: VIP status, 2: Expired date, 3: Remaining time
 * @param name: Character name (Optional)
 * @return val: 1 - success, 0 - failed
 */
BUILDIN_FUNC(vip_status) {
#ifdef VIP_ENABLE
	TBL_PC *sd;
	char vip_str[26];
	time_t now = time(NULL);
	int type = script_getnum(st,2);

	if( script_hasdata(st,3) )
		sd = map_nick2sd(script_getstr(st,3));
	else
		sd = script_rid2sd(st);

	if( sd == NULL )
		return 0;

	switch( type ) {
		case 1: //Get VIP status.
			script_pushint(st,pc_isvip(sd));
			break;
		case 2: //Get VIP expire date.
			if( pc_isvip(sd) ) {
				time_t viptime = sd->vip.time;
				strftime(vip_str,24,"%Y-%m-%d %H:%M",localtime(&viptime));
				vip_str[25] = '\0';
				script_pushstrcopy(st,vip_str);
			} else
				script_pushint(st, 0);
			break;
		case 3: //Get remaining time.
			if( pc_isvip(sd) ) {
				time_t viptime_remain = sd->vip.time - now;
				int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

				split_time((int)viptime_remain,&year,&month,&day,&hour,&min,&sec);
				safesnprintf(vip_str,sizeof(vip_str),"%d-%d-%d %d:%d",year,month,day,hour,min);
				script_pushstrcopy(st,vip_str);
			} else
				script_pushint(st,0);
			break;
	}
#else
	script_pushint(st,0);
#endif
	return SCRIPT_CMD_SUCCESS;
}

/** Adds or removes VIP time in minutes. Need to enable VIP system
 * vip_time <time in mn>,{"<character name>"};
 * @param time: VIP duration in minutes. If time < 0 remove time, else add time.
 * @param name: Character name (optional)
 * @return val: 1 - success, 0 - failed
 */
BUILDIN_FUNC(vip_time) {
#ifdef VIP_ENABLE //Would be a pain for scripting npc otherwise
	TBL_PC *sd;
	int viptime = script_getnum(st,2) * 60; //Convert since it's given in minutes.

	if (script_hasdata(st,3))
		sd = map_nick2sd(script_getstr(st,3));
	else
		sd = script_rid2sd(st);

	if (sd == NULL)
		return 0;

	if (pc_get_group_level(sd) > 5) {
		clif_displaymessage(sd->fd, msg_txt(437)); // GM's cannot become a VIP.
		return 0;
	}

	chrif_req_login_operation(sd->status.account_id,sd->status.name,6,viptime,7,0);
#endif
	return SCRIPT_CMD_SUCCESS,100,/** Allows player to use atcommand while talking with NPC
 * enable_command;
 * @author [Cydh], [Kichi]
 */
BUILDIN_FUNC(enable_command) {
	TBL_PC* sd = script_rid2sd(st);

	if (!sd)
		return 1;
	sd->state.disable_atcommand_on_npc = 0;
	return SCRIPT_CMD_SUCCESS;
}

/** Prevents player to use atcommand while talking with NPC
 * disable_command;
 * @author [Cydh], [Kichi]
 */
BUILDIN_FUNC(disable_command) {
	TBL_PC* sd = script_rid2sd(st);

	if (!sd)
		return 1;
	sd->state.disable_atcommand_on_npc = 1;
	return SCRIPT_CMD_SUCCESS,100,/** Get the information of the members of a guild by type.
 * getguildmember <guild_id>{,<type>};
 * @param guild_id: ID of guild
 * @param type: Type of option (optional)
 */
BUILDIN_FUNC(getguildmember) {
	unsigned char j = 0;
	struct guild *g = guild_search(script_getnum(st,2));

	if (g) {
		unsigned char i, type = 0;

		if (script_hasdata(st,3))
			type = (unsigned char)script_getnum(st,3);
		for (i = 0; i < MAX_GUILD; i++) {
			if (g->member[i].account_id) {
				switch (type) {
					case 2:
						mapreg_setreg(reference_uid(add_str("$@guildmemberaid"),j),g->member[i].account_id);
						break;
					case 1:
						mapreg_setreg(reference_uid(add_str("$@guildmembercid"),j),g->member[i].char_id);
						break;
					default:
						mapreg_setregstr(reference_uid(add_str("$@guildmembername$"),j),g->member[i].name);
						break;
				}
				j++;
			}
		}
	}
	mapreg_setreg(add_str("$@guildmembercount"), j);
	return SCRIPT_CMD_SUCCESS,100,/** Adds spirit ball to player for 'duration' in milisecond
 * addspiritball <count>,<duration>{,<char_id>};
 * @param count How many spirit ball will be added
 * @param duration How long spiritball is active until it disappears
 * @param char_id Target player (Optional)
 * @author [Cydh]
 */
BUILDIN_FUNC(addspiritball) {
	uint8 i, count = script_getnum(st,2);
	uint16 duration = script_getnum(st,3);
	struct map_session_data *sd = NULL;

	if (!count)
		return 0;
	if (script_hasdata(st,4)) {
		if (!script_isstring(st,4))
			sd = map_charid2sd(script_getnum(st,4));
		else
			sd = map_nick2sd(script_getstr(st,4));
	} else
		sd = script_rid2sd(st);
	if (!sd)
		return 1;
	for (i = 0; i < count; i++)
		pc_addspiritball(sd,duration,10);
	return SCRIPT_CMD_SUCCESS;
}

/** Deletes the spirit ball(s) from player
 * delspiritball <count>{,<char_id>};
 * @param count How many spirit ball will be deleted
 * @param char_id Target player (Optional)
 * @author [Cydh]
 */
BUILDIN_FUNC(delspiritball) {
	uint8 count = script_getnum(st,2);
	struct map_session_data *sd = NULL;

	if (!count)
		count = 1;
	if (script_hasdata(st,3)) {
		if (!script_isstring(st,3))
			sd = map_charid2sd(script_getnum(st,3));
		else
			sd = map_nick2sd(script_getstr(st,3));
	} else
		sd = script_rid2sd(st);
	if (!sd)
		return 1;
	pc_delspiritball(sd,count,0);
	return SCRIPT_CMD_SUCCESS;
}

/** Counts the spirit ball that player has
 * countspiritball {,<char_id>};
 * @param char_id Target player (Optional)
 * @author [Cydh]
 */
BUILDIN_FUNC(countspiritball) {
	struct map_session_data *sd;

	if (script_hasdata(st,2)) {
		if (!script_isstring(st,2))
			sd = map_charid2sd(script_getnum(st,2));
		else
			sd = map_nick2sd(script_getstr(st,2));
	} else
		sd = script_rid2sd(st);
	if (!sd)
		return 1;
	script_pushint(st,sd->spiritball);
	return SCRIPT_CMD_SUCCESS,100,val1,val2,val3,val4,tick);
	}
	return 0;
}

#include "../custom/script.inc"

// Declarations that were supposed to be exported from npc_chat.c
#ifdef PCRE_SUPPORT
BUILDIN_FUNC(defpattern);
BUILDIN_FUNC(activatepset);
BUILDIN_FUNC#endifC
/** Regular expression matching
 * preg_match(<pattern>,<string>{,<offset>})
 */
BUILDIN_FUNC(preg_match) {
#ifdef PCRE_SUPPORT
	pcre *re;
	pcre_extra *pcreExtra;
	const char *error;
	int erroffset, r, offset = 0;
	int subStrVec[30];
	const char* pattern = script_getstr(st,2);
	const char* subject = script_getstr(st,3);

	if (script_hasdata(st,4))
		offset = script_getnum(st,4);

	re = pcre_compile(pattern,0,&error,&erroffset,NULL);
	pcreExtra = pcre_study(re,0,&error);

	r = pcre_exec(re,pcreExtra,subject,(int)strlen(subject),offset,0,subStrVec,30);

	pcre_free(re);
	if (pcreExtra != NULL)
		pcre_free(pcreExtra);

	if (r < 0)
		script_pushint(st,0);
	else
		script_pushint(st,(r > 0) ? r : 30 / 3);

	return SCRIPT_CMD_SUCCESS;
#else
	ShowDebug("script:preg_match: cannot run without PCRE library enabled.\n");
	script_pushint(st,0);
	return SCRIPT_CMD_SUCCESS;
#endif
}ivatepset);
BUILDIN_FUNC(deletepset);
#endif

/// Script command definitions
/// For an explanation on args, see add_buildin_func
struct script_function buildin_func[] = {
	// NPC interaction
	BUILDIN_DEF(mes,"s*"),
	BUILDIN_DEF(next,""),
	BUILDIN_DEF(close,""),
	BUILDIN_DEF(close2,""),
	BUILDIN_DEF(menu,"sl*"),
	BUILDIN_DEF(select,"s*"), //for future jA script compatibility
	BUILDIN_DEF(prompt,"s*"),
	//
	BUILDIN_DEF(goto,"l"),
	BUILDIN_DEF(callsub,"l*"),
	BUILDIN_DEF(callfunc,"s*"),
	BUILDIN_DEF(return,"?"),
	BUILDIN_DEF(getarg,"i?"),
	BUILDIN_DEF(jobchange,"i?"),
	BUILDIN_DEF(jobname,"i"),
	BUILDIN_DEF(input,"r??"),
	BUILDIN_DEF(warp,"sii"),
	BUILDIN_DEF(areawarp,"siiiisii??"),
	BUILDIN_DEF(warpchar,"siii"), // [LuzZza]
	BUILDIN_DEF(warpparty,"siii?"), // [Fredzilla] [Paradox924X]
	BUILDIN_DEF(warpguild,"siii"), // [Fredzilla]
	BUILDIN_DEF(setlook,"ii"),
	BUILDIN_DEF(changelook,"ii"), // Simulates but don't Store it
	BUILDIN_DEF(set,"rv"),
	BUILDIN_DEF(setarray,"rv*"),
	BUILDIN_DEF(cleararray,"rvi"),
	BUILDIN_DEF(copyarray,"rri"),
	BUILDIN_DEF(getarraysize,"r"),
	BUILDIN_DEF(deletearray,"r?"),
	BUILDIN_DEF(getelementofarray,"ri"),
	BUILDIN_DEF(getitem,"vi?")rentitem2,"viiiiiiiDEF(getitem,"vi?"),
	BUILDIN_DEF(rentitem,"vi"),
	BUILDIN_DEF(getitem2,"viiiiiiii?"),
	BUILDIN_DEF(getnameditem,"vv"),
	BUI?LDIN_DEF2(grouprandomitem,"groupranditem","i"),
	makeitem2,"visiiiiiiiupranditem","i"),
	BUILDIN_DEF(makeitem,"visii"2(delitem,"storagedelitem","vi?"),
	BUILDIN_DEF2(delitem,"cartdelitem","vi?"),
	BUILDIN_DEF(delitem2,"viiiiiiii?"),
	BUILDIN_DEF2(delitem2,"storagedelitem2","viiiiiiii?"),
	BUILDIN_DEF2(delitem2,"cartdelitem2"IN_DEF(delitem,"vi?"),
	BUILDIN_DEF(delitem2,"viiiiiiii?"),
	BUILDIN_DEF2(enableitemuse,"enable_items",""),
	BUILDIN_DEF2(disableitemuse,"disable_items",""),
	BUILDIN_DEF(cutin,"si"),
	BUILDIN_DEF(viewpoint,"iiiii"),
	BUILDIN_DEF(heal,"ii"),
	BUILDIN_DEF(itemheal,"ii"),
	BUILDIN_DEF(percentheal,"ii"),
	BUILD?"),
	BUILDIN_DEF2(countitem,"storagecountitem","v?"),
	BUILDIN_DEF2(countitem,"cartcountitem","v?"),
	BUILDIN_DEF2(countitem,"countitem2","viiiiiii?"),
	BUILDIN_DEF2(countitem,"storagecountitem2","viiiiiii?"),
	BUILDIN_DEF2(countitem,"cartcountitem2","viiiiiii?UILDIN_DEF2(countitem,"countitem2","viiiiiii"),
	BUILDIN_DEF(checkweight,"vi*"),
	BUILDIN_DEF(checkweight2,"rr"),
	BUILDIN_DEF(readparam,"i?"),
	BUILDIN_DEF(getcharid,"i?"),
	BUILDIN_DEF(getnpcid,"i?"),
	BUILDIN_DEF(getpartyname,"i"),
	BUILDIN_DEF(getpartymember,"i?"),
	BUILDIN_DEF(getpartyleader,"i?"),
	BUILDIN_DEF(getguildname,"i"),
	BUILDIN_DEF(getguildmaster,"i"),
	BUILDIN_DEF(getguildmasterid,"i"),
	BUILDIN_DEF(strcharinfo,"i"),
	BUILDIN_DEF(strnpcinfo,"i"),
	BUILDIN_DEF(getequipid,"i"),
	BUILDIN_DEF(getequipname,"i"),
	BUILDIN_DEF(getbrokenid,"i"), // [Valaris]
	BUILDIN_DEF(repair,"i"), // [Valaris]
	BUILDIN_DEF(repairall,""),
	BUILDIN_DEF(getequipisequiped,"i"),
	BUILDIN_DEF(getequipisenableref,"i"),
	BUILDIN_DEF(getequiprefinerycnt,"i"),
	BUILDIN_DEF(getequipweaponlv,"i"),
	BUILDIN_DEF(getequippercentr?efinery,"i"),
	BUILDIN_DEF(successrefitem,"i"),
	BUILDIN_DEF(failedrefitem,"i"),
	BUILDIN_DEF(downrefitem,"i?"),
	BUILDIN_DEF(statusup,"i"),
	BUILDIN_DEF(statusup2,"ii"),
	BUILDIN_DEF(bonus,"iv"),
	BUILDIN_DEF2(bonus,"bonus2","ivi"),
	BUILDIN_DEF2(bonus,"bonus3","ivii"),
	BUILDIN_DEF2(bonus,"bonus4","ivvii"),
	BUILDIN_DEF2(bonus,"bonus5","ivviii"),
	BUILDIN_DEF(autobonus,"sii??"),
	BUILDIN_DEF(autobonus2,"sii??"),
	BUILDIN_DEF(autobonus3,"siiv?"),
	BUILDIN_DEF(skill,"vi?"),
	BUILDIN_DEF(addtoskill,"vi?"), // [Valaris]
	BUILDIN_DEF(guildskill,"vi"),
	BUILDIN_DEF(getskilllv,"v"),
	BUILDIN_DEF(getgdskilllv,"iv"),
	BUILDIN_DEF(basicskillcheck,""),
	BUILDIN_DEF(getgmlevel,""),
	BUILDIN_DEF(getgroupid,""),
	BUILDIN_DEF(end,""),
	BUILDIN_DEF(checkoption,"i"),
	BUILDIN_DEF(setoption,"i?"),
	BUILDIN_DEF(setcart,"?"),
	BUILDIN_DEF(checkcart,""),
	BUILDIN_DEF(setfalcon,"?"),
	BUILDIN_DEF(checkfalcon,""),
	BUILDIN_DEF(setriding,"?"),
	BUILDIN_DEF(checkriding,""),
	BUILDIN_DEF(checkwug,""),
	BUILDIN_DEF(checkmadogear,""),
	BUILDIN_DEF(setmadogear,"?"),
	BUILDIN_DEF2(savepoint,"save","sii"),
	BUILDIN_DEF(savepoint,"sii"),
	BUILDIN_DEF(gettimetick,"i"),
	BUILDIN_DEF(gettime,"i"),
	BUILDIN_DEF(gettimestr,"si"),
	BUILDIN_DEF(openstorage,""),
	BUILDIN_DEF(guildopenstorage,""),
	BUILDIN_DEF(itemskill,"vi?"),
	BUILDIN_DEF(produce,"i"),
	BUILDIN_DEF(cooking,"i"),
	BUILDIN_DEF(monster,"siisii???"),
	BUILDIN_DEF(getmobdrops,"i"),
	BUILDIN_DEF(areamonster,"siiiisii???"),
	BUILDIN_DEF(killmonster,"ss?"),
	BUILDIN_DEF(killmonsterall,"s?"),
	BUILDIN_DEF(clone,"siisi????"),
	BUILDIN_DEF(doevent,"s"),
	BUILDIN_DEF(donpcevent,"s"),
	BUILDIN_DEF(cmdothernpc,"ss"),
	BUILDIN_DEF(addtimer,"is"),
	BUILDIN_DEF(deltimer,"s"),
	BUILDIN_DEF(addtimercount,"is"),
	BUILDIN_DEF(initnpctimer,"??"),
	BUILDIN_DEF(stopnpctimer,"??"),
	BUILDIN_DEF(startnpctimer,"??"),
	BUILDIN_DEF(setnpctimer,"i?"),
	BUILDIN_DEF(getnpctimer,"i?"),
	BUILDIN_DEF(attachnpctimer,"?"), // Attached the player id to the npc timer [Celest]
	BUILDIN_DEF(detachnpctimer,"?"), // Detached the player id from the npc timer [Celest]
	BUILDIN_DEF(playerattached,""), // Returns id of the current attached player. [Skotlex]
	BUILDIN_DEF(announce,"si?????"),
	BUILDIN_DEF(mapannounce,"ssi?????"),
	BUILDIN_DEF(areaannounce,"siiiisi?????"),
	BUILDIN_DEF(getusers,"i"),
	BUILDIN_DEF(getmapguildusers,"si"),
	BUILDIN_DEF(getmapusers,"s"),
	BUILDIN_DEF(getareausers,"siiii"),
	BUILDIN_DEF(getareadropitem,"siiiiv"),
	BUILDIN_DEF(enablenpc,"s"),
	BUILDIN_DEF(disablenpc,"s"),
	BUILDIN_DEF(hideoffnpc,"s"),
	BUILDIN_DEF(hideonnpc,"s"),
	BUILDIN_DEF(sc_start,"iii???"),
	BUILDIN_DEF2(sc_start,"sc_start2","iiii???"),
	BUILDIN_DEF2(sc_start,"sc_start4","iiiiii???"),
	BUILDIN_DEF(sc_end,"i?"),
	BUILDIN_DEF(getstatus, "i?"),
	BUILDIN_DEF(getscrate,"ii?"),
	BUILDIN_DEF(debugmes,"s"),
	BUILDIN_DEF2(catchpet,"pet","i"),
	BUILDIN_DEF2(birthpet,"bpet",""),
	BUILDIN_DEF(resetlvl,"i"),
	BUILDIN_DEF(resetstatus,""),
	BUILDIN_DEF(resetskill,""),
	BUILDIN_DEF(skillpointcount,""),
	BUILDIN_DEF(changebase,"i?"),
	BUILDIN_DEF(changesex,""),
	BUILDIN_DEF(waitingroom,"si?????"),
	BUILDIN_DEF(delwaitingroom,"?"),
	BUILDIN_DEF2(waitingroomkickall,"kickwaitingroomall","?"),
	BUILDIN_DEF(enablewaitingroomevent,"?"),
	BUILDIN_DEF(disablewaitingroomevent,"?"),
	BUILDIN_DEF2(enablewaitingroomevent,"enablearena",""),		// Added by RoVeRT
	BUILDIN_DEF2(disablewaitingroomevent,"disablearena",""),	// Added by RoVeRT
	BUILDIN_DEF(getwaitingroomstate,"i?"),
	BUILDIN_DEF(warpwaitingpc,"sii?"),
	BUILDIN_DEF(attachrid,"i"),
	BUILDIN_DEF(addrid,"i?????"),
	BUILDIN_DEF(detachrid,""),
	BUILDIN_DEF(isloggedin,"i?"),
	BUILDIN_DEF(setmapflagnosave,"ssii"),
	BUILDIN_DEF(getmapflag,"si?"),
	BUILDIN_DEF(setmapflag,"si??"),
	BUILDIN_DEF(removemapflag,"si?"),
	BUILDIN_DEF(pvpon,"s"),
	BUILDIN_DEF(pvpoff,"s"),
	BUILDIN_DEF(gvgon,"s"),
	BUILDIN_DEF(gvgoff,"s"),
	BUILDIN_DEF(emotion,"i??"),
	BUILDIN_DEF(maprespawnguildid,"sii"),
	BUILDIN_DEF(agitstart,""),	// <Agit>
	BUILDIN_DEF(agitend,""),
	BUILDIN_DEF(agitcheck,""),   // <Agitcheck>
	BUILDIN_DEF(flagemblem,"i"),	// Flag Emblem
	BUILDIN_DEF(getcastlename,"s"),
	BUILDIN_DEF(getcastledata,"si"),
	BUILDIN_DEF(setcastledata,"sii"),
	BUILDIN_DEF(requestguildinfo,"i?"),
	BUILDIN_DEF(getequipcardcnt,"i"),
	BUILDIN_DEF(successremovecards,"i"),
	BUILDIN_DEF(failedremovecards,"ii"),
	BUILDIN_DEF(marriage,"s"),
	BUILDIN_DEF2(wedding_effect,"wedding",""),
	BUILDIN_DEF(divorce,""),
	BUILDIN_DEF(ispartneron,""),
	BUILDIN_DEF(getpartnerid,""),
	BUILDIN_DEF(getchildid,""),
	BUILDIN_DEF(getmotherid,""),
	BUILDIN_DEF(getfatherid,""),
	BUILDIN_DEF(warppartner,"sii"),
	BUILDIN_DEF(getitemname,"v"),
	BUILDIN_DEF(getitemslots,"i"),
	BUILDIN_DEF(makepet,"i"),
	BUILDIN_DEF(getexp,"ii"),
	BUILDIN_DEF(getinventorylist,""),
	BUILDIN_DEF(getskilllist,""),
	BUILDIN_DEF(clearitem,""),
	BUILDIN_DEF(classchange,"ii"),
	BUILDIN_DEF(misceffect,"i"),
	BUILDIN_DEF(playBGM,"s"),
	BUILDIN_DEF(playBGMall,"s?????"),
	BUILDIN_DEF(soundeffect,"si"),
	BUILDIN_DEF(soundeffectall,"si?????"),	// SoundEffectAll [Codemaster]
	BUILDIN_DEF(strmobinfo,"ii"),	// display mob data [Valaris]
	BUILDIN_DEF(guardian,"siisi??"),	// summon guardians
	BUILDIN_DEF(guardianinfo,"sii"),	// display guardian data [Valaris]
	BUILDIN_DEF(petskillbonus,"iiii"), // [Valaris]
	BUILDIN_DEF(petrecovery,"ii"), // [Valaris]
	BUILDIN_DEF(petloot,"i"), // [Valaris]
	BUILDIN_DEF(petheal,"iiii"), // [Valaris]
	BUILDIN_DEF(petskillattack,"viii"), // [Skotlex]
	BUILDIN_DEF(petskillattack2,"viiii"), // [Valaris]
	BUILDIN_DEF(petskillsupport,"viiii"), // [Skotlex]
	BUILDIN_DEF(skilleffect,"vi"), // skill effect [Celest]
	BUILDIN_DEF(npcskilleffect,"viii"), // npc skill effect [Valaris]
	BUILDIN_DEF(specialeffect,"i??"), // npc skill effect [Valaris]
	BUILDIN_DEF(specialeffect2,"i??"), // skill effect on players[Valaris]
	BUILDIN_DEF(nude,""), // nude command [Valaris]
	BUILDIN_DEF(mapwarp,"ssii??"),		// Added by RoVeRT
	BUILDIN_DEF(atcommand,"s"), // [MouseJstr]
	BUILDIN_DEF2(atcommand,"charcommand","s"), // [MouseJstr]
	BUILDIN_DEF(movenpc,"sii?"), // [MouseJstr]
	BUILDIN_DEF(message,"ss"), // [MouseJstr]
	BUILDIN_DEF(npctalk,"s"), // [Valaris]
	BUILDIN_DEF(mobcount,"ss"),
	BUILDIN_DEF(getlook,"i"),
	BUILDIN_DEF(getsavepoint,"i"),
	BUILDIN_DEF(npcspeed,"i"), // [Valaris]
	BUILDIN_DEF(npcwalkto,"ii"), // [Valaris]
	BUILDIN_DEF(npcstop,""), // [Valaris]
	BUILDIN_DEF(getmapxy,"rrri?"),	//by Lorky [Lupus]
	BUILDIN_DEF(checkoption1,"i"),
	BUILDIN_DEF(checkoption2,"i"),
	BUILDIN_DEF(guildgetexp,"i"),
	BUILDIN_DEF(guildchangegm,"is"),
	BUILDIN_DEF(logmes,"s"), //this command actls as MES but rints info into LOG file either SQL/TXT [Lupus]
	BUILDIN_DEF(summon,"si??"), // summons a slave monster [Celest]
	BUILDIN_DEF(isnight,""), // check whether it is night time [Celest]
	BUILDIN_DEF(isday,""), // check whether it is day time [Celest]
	BUILDIN_DEF(isequipped,"i*"), // check whether another item/card has been equipped [Celest]
	BUILDIN_DEF(isequippedcnt,"i*"), // check how many items/cards are being equipped [Celest]
	BUILDIN_DEF(cardscnt,"i*"), // check how many items/cards are being equipped in the same arm [Lupus]
	BUILDIN_DEF(getrefine,""), // returns the refined number of the current item, or an item with index specified [celest]
	BUILDIN_DEF(night,""), // sets the server to night time
	BUILDIN_DEF(day,""), // sets the server to day time
#ifdef PCRE_SUPPORT
	BUILDIN_DEF(defpattern,"iss"), // Define pattern to listen for [MouseJstr]
	BUILDIN_DEF(activatepset,"i"), // Activate a pattern set [MouseJstr]
	BUILDIN_DEF(deactivatepset,"i"), // Deactive a pattern set [MouseJstr]
	BUILDIN_DEF(deletepse#endif
	BUILDIN_DEF(preg_match,"ss?"),, // Delete a pattern set [MouseJstr]
#endif
	BUILDIN_DEF(dispbottom,"s"), //added from jA [Lupus]
	BUILDIN_DEF(getusersname,""),
	BUILDIN_DEF(recovery,"i???"),
	BUILDIN_DEF(getpetinfo,"i"),
	BUILDIN_DEF(gethominfo,"i"),
	BUILDIN_DEF(getmercinfo,"i?"),
	BUILDIN_DEF(checkequipedcard,"i"),
	BUILDIN_DEF(jump_zero,"il"), //for future jA script compatibility
	BUILDIN_DEF(globalmes,"s?"), //end jA addition
	BUILDIN_DEF(unequip,"i"), // unequip command [Spectre]
	BUILDIN_DEF(getstrlen,"s"), //strlen [Valaris]
	BUILDIN_DEF(charisalpha,"si"), //isalpha [Valaris]
	BUILDIN_DEF(charat,"si"),
	BUILDIN_DEF(setchar,"ssi"),
	BUILDIN_DEF(insertchar,"ssi"),
	BUILDIN_DEF(delchar,"si"),
	BUILDIN_DEF(strtoupper,"s"),
	BUILDIN_DEF(strtolower,"s"),
	BUILDIN_DEF(charisupper, "si"),
	BUILDIN_DEF(charislower, "si"),
	BUILDIN_DEF(substr,"sii"),
	BUILDIN_DEF(explode, "rss"),
	BUILDIN_DEF(implode, "r?"),
	BUILDIN_DEF(sprintf,"s*"),  //[Mirei]
	BUILDIN_DEF(sscanf,"ss*"),  //[Mirei]
	BUILDIN_DEF(strpos,"ss?"),
	BUILDIN_DEF(replacestr,"sss??"),
	BUILDIN_DEF(countstr,"ss?"),
	BUILDIN_DEF(setnpcdisplay,"sv??"),
	BUILDIN_DEF(compare,"ss"), //Lordalfa - To bristrcmp,"ss"),dalfa - To bring strstr to scripting Engine.
	BUILDIN_DEF(getiteminfo,"ii"), //[Lupus] returns Items Buy / sell Price, etc info
	BUILDIN_DEF(setiteminfo,"iii"), //[Lupus] set Items Buy / sell Price, etc info
	BUILDIN_DEF(getequipcardid,"ii"), //[Lupus] returns CARD ID or other info from CARD slot N of equipped item
	//[zBuffer] List of mathematics commands --->
	BUILDIN_DEF(sqrt,"i"),
	BUILDIN_DEF(pow,"ii"),
	BUILDIN_DEF(distance,"iiii"),
	//<--- [zBuffer] List of mathematics commands
	BUILDIN_DEF(md5,"s"),
	//[zBuffer] List of dynamic var commands --->
	BUILDIN_DEF(getd,"s"),
	BUILDIN_DEF(setd,"sv"),
	BUILDIN_DEF(callshop,"s?"), //[Skotlex]
	BUILDIN_DEF(npcshopitem,"sii*"), //[Lance]
	BUILDIN_DEF(npcshopadditem,"sii*"),
	BUILDIN_DEF(npcshopdelitem,"si*"),
	BUILDIN_DEF(npcshopattach,"s?"),
	BUILDIN_DEF(equip,"i"),
	BUILDIN_DEF(autoequip,"ii"),
	BUILDIN_DEF(setbattleflag,"si"),
	BUILDIN_DEF(getbattleflag,"s"),
	BUILDIN_DEF(setitemscript,"is?"), //Set NEW item bonus script. Lupus
	BUILDIN_DEF(disguise,"i"), //Disguise player. Lupus
	BUILDIN_DEF(undisguise,""), //Undisguise player. Lupus
	BUILDIN_DEF(getmonsterinfo,"ii"), //Lupus
	BUILDIN_DEF(addmonsterdrop,"vii"), //Akinari [Lupus]
	BUILDIN_DEF(delmonsterdrop,"vi"), //Akinari [Lupus]
	BUILDIN_DEF(axtoi,"s"),
	BUILDIN_DEF(query_sql,"s*"),
	BUILDIN_DEF(query_logsql,"s*"),
	BUILDIN_DEBUILDIN_DEF(strtol,"siIN_DEF(escape_sql,"v"),
	BUILDIN_DEF(atoi,"s"),
	// [zBuffer] List of player cont commands --->
	BUILDIN_DEF(rid2name,"i"),
	BUILDIN_DEF(pcfollow,"ii"),
	BUILDIN_DEF(pcstopfollow,"i"),
	BUILDIN_DEF(pcblockmove,"ii"),
	// <--- [zBuffer] List of player cont commands
	// [zBuffer] List of mob control commands --->
	BUILDIN_DEF(unitwalk,"ii?"),
	BUILDIN_DEF(unitkill,"i"),
	BUILDIN_DEF(unitwarp,"isii"),
	BUILDIN_DEF(unitattack,"iv?"),
	BUILDIN_DEF(unitstop,"i"),
	BUILDIN_DEF(unittalk,"is"),
	BUILDIN_DEF(unitemote,"ii"),
	BUILDIN_DEF(unitskilluseid,"ivi?"), //Originally by Qamera [Celest]
	BUILD	IN_DEF(unitskillusepos,"iviii"), //[Celest]
// <--- [zBuffer] List of mob control commands
	BUILDIN_DEF(sleep,"i"),
	BUILDIN_DEF(sleep2,"i"),
	BUILDIN_DEF(awake,"s"),
	BUILDIN_DEF(getvariableofnpc,"rs"),
	BUILDIN_DEF(warpportal,"iisii"),
	BUILDIN_DEF2(homunculus_evolution,"homevolution",""), //[orn]
	BUILDIN_DEF2(homunculus_mutate,"hommutate","?"),
	BUILDIN_DEF(morphembryo,""),
	BUILDIN_DEF2(homunculus_shuffle,"homshuffle",""), //[Zephyrus]
	BUILDIN_DEF(checkhomcall,""),
	BUILDIN_DEF(eaclass,"?"), //[Skotlex]
	BUILDIN_DEF(roclass,"i?"), //[Skotlex]
	BUILDIN_DEF(checkvending,"?"),
	BUILDIN_DEF(checkchatting,"?"),
	BUILDIN_DEF(checkidle,"?"),
	BUILDIN_DEF(openmail,""),
	BUILDIN_DEF(openauction,""),
	BUILDIN_DEF(checkcell,"siii"),
	BUILDIN_DEF(setcell,"siiiiii"),
	BUILDIN_DEF(setwall,"siiiiis"),
	BUILDIN_DEF(delwall,"s"),
	BUILDIN_DEF(searchitem,"rs"),
	BUILDIN_DEF(mercenary_create,"ii"),
	BUILDIN_DEF(mercenary_heal,"ii"),
	BUILDIN_DEF(mercenary_sc_start,"iii"),
	BUILDIN_DEF(mercenary_get_calls,"i"),
	BUILDIN_DEF(mercenary_get_faith,"i"),
	BUILDIN_DEF(mercenary_set_calls,"ii"),
	BUILDIN_DEF(mercenary_set_faith,"ii"),
	BUILDIN_DEF(readbook,"ii"),
	BUILDIN_DEF(setfont,"i"),
	BUILDIN_DEF(areamobuseskill,"siiiiviiiii"),
	BUILDIN_DEF(progressbar,"si"),
	BUILDIN_DEF(pushpc,"ii"),
	BUILDIN_DEF(buyingstore,"i"),
	BUILDIN_DEF(searchstores,"ii"),
	BUILDIN_DEF(showdigit,"i?"),
	//WoE SE
	BUILDIN_DEF(agitstart2,""),
	BUILDIN_DEF(agitend2,""),
	BUILDIN_DEF(agitcheck2,""),
	//BattleGround
	BUILDIN_DEF(waitingroom2bg,"siiss?"),
	BUILDIN_DEF(waitingroom2bg_single,"isiis"),
	BUILDIN_DEF(bg_team_setxy,"iii"),
	BUILDIN_DEF(bg_warp,"isii"),
	BUILDIN_DEF(bg_monster,"isiisi?"),
	BUILDIN_DEF(bg_monster_set_team,"ii"),
	BUILDIN_DEF(bg_leave,""),
	BUILDIN_DEF(bg_destroy,"i"),
	BUILDIN_DEF(areapercentheal,"siiiiii"),
	BUILDIN_DEF(bg_get_data,"ii"),
	BUILDIN_DEF(bg_getareausers,"isiii"),
	BUILDIN_DEF(bg_updatescore,"sii"),

	//Instancing
	BUILDIN_DEF(instance_create,"s"),
	BUILDIN_DEF(instance_destroy,"?"),
	BUILDIN_DEF(ins??tance_id,""),
	BUILDIN_DEF(instance_enter,"s"),
	BUILDIN_DEF(instance_npcname,"s?"),
	BUILDIN_DEF(instance_mapname,"s?"),
	BUILDIN_DEF(instance_warpall,"sii?"),
	BUILDIN_DEF(instance_announce,"isi?????"),
	/3rd-relatedck_party,"i???"),
	/**
	 * 3rd-related
	 **/
	BUILDIN_DEF(makerune,"i"),
	BUILDIN_DEF(checkdragon,""), //[Ind]
	BUILDIN_DEF(setdragon,"?"), //[Ind]
	BUILDIN_DEF(ismounting,""), //[Ind]
	BUILDIN_DEF(setmountin/rAthena and beyond!kre,"i"),
	/**
	 * rAthena and beyond!
	 **/
	BUILDIN_DEF(getargcount,""),
	BUILDIN_DEF(getcharip,"?"),
	BUILDIN_DEF(is_function,"s"),
	BUILDIN_D?F(get_revision,""),
	BUILDIN_DEF(free?loop,"i"),
	BUILDIN_DEF(getrandgroupitem,"ii"),
	BUILDIN_DEF(cleanmap,"s"),
	BUILDIN_DEF2(cleanmap,"cleanarea","siiii"),
	BUILDIN_DEF(npcskill,"viii"),
	BUILDIN_DEF(consumeitem,"v"),
	BUILDIN_DEF(delequip,"i"),
	BUILDIN_DEF(breakequip,"i"),
	BUILD/@commands (script based)?"),
	/**
	 * @commands (script based)
	 **/
	BUILDIN_DEF(bindatcmd,"ss??"),
	BUILDIN_DEF(unindatcmd,"s"),
	BUILDIN_DEF(useatcmd,"s"),
questinfo,"ii??"),seatcmd,"s"),

	//Quest Log System [Inkfish]
	BUILDIN_DEF(setquest,"i"),
	BUILDIN_DEF(erasequest,"i"),
	BUILDIN_DEF(completequest,"i"),
isbegin_quest,"ipletequest,"i"),
	BUILDIN_DEF(checkquest,"i?"),
	BUILDIN_DEF(?hangquest,"ii"),
	BUILDIN_DEF(showevent,"ii"),

	//Party related
	BUILDIN_DEF(party_create,"s???"),
	BUILDIN_DEF(party_addmember,"ii"),
	BUILDIN_DEF(party_delmember,"??"),
	BUILDIN_DEF(party_changeleader,"ii"),
	BUILDIN_DEF(party_changeoptin,"iii"),
	BUILDIN_DEF(party_destroy,"i"),

	//Bound items [Xantara] & [Akinari]
	BUILDIN_DEF2(getitem,"getitembound","vii?"),
	BUILDIN_DEF2(getitem2,"countbound,"?"),
	BUILDIN_DEF(is_clientver,"ii?"),
	//Monster Transform [malufett]
	BUILDIN_DEF2(montransform,"transform","vi?????"),
	BUILDIN_DEF(bonus_script,"si????"),
	BUILDIN_DEF(bonus_script_clear,"??"),
	BUILDIN_DEF(vip_status,"i?"),
	BUILDIN_DEF(vip_time,"i?"),
	BUILDIN_DEF(getgroupitem,"i"),
	BUILDIN_DEF(enable_command,""),
	BUILDIN_DEF(disable_command,""),
	BUILDIN_DEF(getguildmember,"i?"),
	BUILDIN_DEF(addspiritball,"ii?"),
	BUILDIN_DEF(delspiritball,"i?"),
	BUILDIN_DEF(countspiritball,"?"),

#include "../custom/script_def.inc"

	{NULL,NULL,NULL},
};
