#ifndef SCRIPT_H
#define SCRIPT_H

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

extern lua_State* Lua;
int start_Lua();

#define sig_table_size 3
extern const char* sig_table[][2];

const char* get_sig(const char* func);
void script_call(const char *func, ...);

int luaopen_nethacklib(lua_State* L);

    
// RNG functions
int scripting_rn2(lua_State *L);

// You Have functions
int scripting_YouHaveAmulet(lua_State *L);
int scripting_YouHaveBell(lua_State *L);
int scripting_YouHaveBook(lua_State *L);
int scripting_YouHaveMenorah(lua_State *L);
int scripting_YouHaveQuestStart(lua_State* L);


int scripting_GetPlayerInfo(lua_State* L);

#endif
