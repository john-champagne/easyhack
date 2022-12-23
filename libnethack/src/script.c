#include "nhcurses.h"
#include "script.h"
#include "hack.h"


static const struct luaL_Reg nethacklib [] = {
    {"GetPlayer", scripting_GetPlayerInfo},
    {NULL, NULL}
};

int luaopen_nethacklib(lua_State* L) {
    lua_newtable(L);
    for (int i = 0; nethacklib[i].name != NULL; i++) {
        lua_pushcfunction(L, nethacklib[i].func);
        lua_setfield(L,-2, nethacklib[i].name);
        printf("%s\n", nethacklib[i].name);
    }
    lua_setglobal(L, "API");
    return 1;
}



lua_State* Lua;

const char* sig_table[][2] = {
    {"hp_loss_modifier", "i>i"},
    {"room.number_of_items", "iii>i"},
    {"room.gen_altar", ">b"},
    {"room.gen_fountain", ">b"},
    {"room.gen_sink", ">b"},
    {NULL, NULL}
};

// TODO: Make list dynamic.
const char* scripts[] = { "header.lua", "ruleset.lua", "room.lua", NULL};

// TODO: Cross platform support.
int load_script(const char* script_name) {
    char path[1024];
    sprintf(path, "./ruleset/%s", script_name);
    return luaL_dofile(Lua, path);
}


// Initializes the lua scripting interface.
int start_Lua() {
    Lua = luaL_newstate();
    luaL_openlibs(Lua);
    luaopen_nethacklib(Lua);
    
    for (int i = 0; scripts[i] != NULL; i++) {
        if (load_script(scripts[i]) != 0) {
            // Error running script.
            printf("start_Lua(): Error calling %s...\n%s\nExiting...\n", scripts[i], lua_tostring(Lua, -1));
            exit(0);
        }
    }

    return 0;
}

const char* get_sig(const char* func) {
    for (int i = 0; sig_table[i][0] != NULL; i++) {
        if (strcmp(func, sig_table[i][0]) == 0)
            return sig_table[i][1];
    }
    printf("ERROR: Could not find %s on signature table.\nQuitting...\n", func);
    exit(0);
    return 0;
}

void print_stack() {
    FILE* fout = fopen("lua_out", "a");
    fprintf(fout,"STACK DUMP:\n", lua_gettop(Lua));
    assert(lua_checkstack(Lua, 3));
    int top = lua_gettop(Lua);
    int bottom = 1;
    lua_getglobal(Lua, "tostring");
    for(int i = top; i >= bottom; i--)
    {
        lua_pushvalue(Lua, -1);
        lua_pushvalue(Lua, i);
        lua_pcall(Lua, 1, 1, 0);
        const char *str = lua_tostring(Lua, -1);
        if (str) {
            fprintf(fout,"%s\n", str);
        } else {
        printf(fout,"%s\n", luaL_typename(Lua, i));
        }
        lua_pop(Lua, 1);
    }
    lua_pop(Lua, 1);
    fclose(fout);
}

void script_call(const char *func, ...) {
    const char* sig = get_sig(func); 
    
    /* Get top of stack */
    int stack_top = lua_gettop(Lua);
    char buf[256];
   
    // __f = function that we want to call.
    sprintf(buf, "__f = %s", func);
    luaL_dostring(Lua, buf);
    
    va_list vl;
    int narg, nres;  /* number of arguments and results */
    va_start(vl, func);
    lua_getglobal(Lua, "__f");  /* push function */

    for (narg = 0; *sig; narg++) {  /* repeat for each argument */
        /* check stack space */
        luaL_checkstack(Lua, 1, "too many arguments");
        switch (*sig++) {
            case 'd':  /* double argument */
                lua_pushnumber(Lua, va_arg(vl, double));
                break;
            case 'i':  /* int argument */
                lua_pushinteger(Lua, va_arg(vl, int));
                break;
            case 's':  /* string argument */
                lua_pushstring(Lua, va_arg(vl, char *));
                break;
            case '>':  /* end of arguments */
                goto endargs;  /* break the loop */
            default:
                error(Lua, "invalid option (%c)", *(sig - 1));
        }
    }
    endargs:
    nres = strlen(sig);  /* number of expected results */
    if (lua_pcall(Lua, narg, nres, 0) != 0)  /* do the call */
        error(Lua, "error calling '%s': %s", func,
    lua_tostring(Lua, -1));
    nres = -nres;  /* stack index of first result */
    while (*sig) {  /* repeat for each result */
        switch (*sig++) {
            case 'd': {  /* double result */
                int isnum;
                double n = lua_tonumberx(Lua, nres, &isnum);
                if (!isnum)
                    error(Lua, "wrong result type");
                *va_arg(vl, double *) = n;
                break;
            }
            case 'i': {  /* int result */
                int isnum;
                int n = lua_tointegerx(Lua, nres, &isnum);
                if (!isnum)
                    error(Lua, "wrong result type");
                *va_arg(vl, int *) = n;
                break;
            }
            case 's': {  /* string result */
                const char *s = lua_tostring(Lua, nres);
                if (s == NULL)
                    error(Lua, "wrong result type");
                *va_arg(vl, const char **) = s;
                break;
            }
            case 'b': { /* boolean */
                int b = lua_toboolean(Lua, nres);
                *va_arg(vl, int*) = b;
                break;
            }
            default:
                error(Lua, "invalid option (%c)", *(sig - 1));
        }
        nres++;
    }
    va_end(vl);
    // __f unassigned
    luaL_dostring(Lua, "__f = nil");
    // Reset stack value
    lua_settop(Lua, stack_top);
}

// You Have functions
int scripting_YouHaveAmulet(lua_State *L) {
    lua_pushboolean(L, Uhave_amulet == NULL ? 0:1);
    return 1;
}

int scripting_YouHaveBell(lua_State *L) {
    lua_pushboolean(L, Uhave_bell == NULL ? 0:1);
    return 1;
}

int scripting_YouHaveBook(lua_State *L) {
    lua_pushboolean(L, Uhave_book == NULL ? 0:1);
    return 1;
}

int scripting_YouHaveMenorah(lua_State *L) {
    lua_pushboolean(L, Uhave_menorah == NULL ? 0:1);
    return 1;
}

int scripting_YouHaveQuestStart(lua_State* L) {
    lua_pushboolean(L, Uhave_questart == NULL ? 0:1);
    return 1;
}

int scripting_GetPlayerInfo(lua_State* L) {
    lua_newtable(L);
    
    // Autogenerated by lua.py:
    lua_pushinteger(L,u.ulevel);
    lua_setfield(L,-2,"XP_Level");

    lua_pushinteger(L,u.ulevelmax);
    lua_setfield(L,-2,"XP_LevelMax");

    lua_pushinteger(L,u.ux);
    lua_setfield(L,-2,"UX");

    lua_pushinteger(L,u.uy);
    lua_setfield(L,-2,"UY");

    lua_pushinteger(L,u.utrap);
    lua_setfield(L,-2,"TrapTimeout");

    lua_pushinteger(L,u.utraptype);
    lua_setfield(L,-2,"TrapType");

    lua_pushinteger(L,u.uhunger);
    lua_setfield(L,-2,"Hunger");

    lua_pushinteger(L,u.uhs);
    lua_setfield(L,-2,"HungerState");

    lua_pushinteger(L,u.uluck);
    lua_setfield(L,-2,"BaseLuck");

    lua_pushinteger(L,u.moreluck);
    lua_setfield(L,-2,"BonusLuck");

    lua_pushinteger(L,(u.uluck + u.moreluck));
    lua_setfield(L,-2,"Luck");

    lua_pushinteger(L,u.uhp);
    lua_setfield(L,-2,"HP");

    lua_pushinteger(L,u.uhpmax);
    lua_setfield(L,-2,"HP_Max");

    lua_pushinteger(L,u.uen);
    lua_setfield(L,-2,"EN");

    lua_pushinteger(L,u.uenmax);
    lua_setfield(L,-2,"EN_Max");  

    return 1;  
}


