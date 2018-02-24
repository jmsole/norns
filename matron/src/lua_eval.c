/*
** $Id: lua.c,v 1.228 2016/12/13 15:50:58 roberto Exp roberto $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*
* modified by ezra buchla ( @catfact ) 2017
*/

#define lua_c

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

#include "lua_eval.h"

#define LUA_VERSUFFIX          "_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR

#if !defined(LUA_MAXINPUT)
#define LUA_MAXINPUT            512
#endif

#define LUA_INITVARVERSION      LUA_INIT_VAR LUA_VERSUFFIX

static lua_State *globalL = NULL;

static const char *progname = "lua";

// a stringbuilder for saving incomplete statement lines
#define STATUS_INCOMPLETE 999

static char *saveBuf = NULL;
static int saveBufLen = 0;
static int continuing = 0;

static void save_statement_buffer(char *buf) {
    saveBufLen = strlen(buf);
    saveBuf = realloc(saveBuf, saveBufLen + 1);
    strcpy(saveBuf, buf);
    continuing = 1;
}

static void clear_statement_buffer(void) {
    free(saveBuf);
    saveBufLen = 0;
    saveBuf = NULL;
    continuing = 0;
}

/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop (lua_State *L, lua_Debug *ar) {
    (void)ar;                   /* unused arg. */
    lua_sethook(L, NULL, 0, 0); /* reset hook */
    luaL_error(L, "interrupted!");
}

/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction (int i) {
    signal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
    lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}

/*
** Prints an error message, adding the program name in front of it
** (if present)
*/
static void l_message (const char *pname, const char *msg) {
    if (pname) {lua_writestringerror("%s: ", pname); }
    lua_writestringerror("%s\n", msg);
}

/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack. It assumes that the error object
** is a string, as it was either generated by Lua or by 'msghandler'.
*/
static int report (lua_State *L, int status) {
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        l_message(progname, msg);
        lua_pop(L, 1); /* remove message */
    }
    return status;
}

// FIXME: for now, a wrapper
int l_report (lua_State *L, int status) {
    report(L, status);
    return 0;
}

/*
** Message handler used to run all chunks
*/
static int msghandler (lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) {                             /* is error object not a
                                                    * string?
                                                    * */
        if ( luaL_callmeta(L, 1, "__tostring") &&  /* does it have a
                                                    * metamethod
                                                    **/
             ( lua_type(L, -1) == LUA_TSTRING) ) { /* that produces a string?
                                                   **/
            return 1;                              /* that is the message */
        }
        else{
            msg = lua_pushfstring( L, "(error object is a %s value)",
                                   luaL_typename(L, 1) );
        }
    }
    luaL_traceback(L, L, msg, 1); /* append a standard traceback */
    return 1;                     /* return the traceback */
}

/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
static int docall (lua_State *L, int narg, int nres) {
    int status;
    int base = lua_gettop(L) - narg;  /* function index */
    lua_pushcfunction(L, msghandler); /* push message handler */
    lua_insert(L, base);              /* put it under function and args */
    globalL = L;                      /* to be available to 'laction' */
    signal(SIGINT, laction);          /* set C-signal handler */
    status = lua_pcall(L, narg, nres, base);
    signal(SIGINT, SIG_DFL);          /* reset C-signal handler */
    lua_remove(L, base);              /* remove message handler from the stack
                                      **/
    return status;
}

// FIXME: for now, a wrapper
int l_docall (lua_State *L, int narg, int nres) {
    int stat = docall(L, narg, nres);
    // FIXME: put some goddamn error handling in here
    return stat;
}

/* static void print_version (void) { */
/*   lua_writestring(LUA_COPYRIGHT, strlen(LUA_COPYRIGHT)); */
/*   lua_writeline(); */
/* } */

static int dochunk (lua_State *L, int status) {
    if (status == LUA_OK) {status = docall(L, 0, 0); }
    return report(L, status);
}

/* static int dofile (lua_State *L, const char *name) { */
/*   return dochunk(L, luaL_loadfile(L, name)); */
/* } */

static int dostring (lua_State *L, const char *s, const char *name) {
    return dochunk( L, luaL_loadbuffer(L, s, strlen(s), name) );
}

// FIXME: for now, an extern wrapper
int l_dostring (lua_State *L, const char *s, const char *name) {
    dostring(L, s, name);
    return 0;
}

/* mark in error messages for incomplete statements */
#define EOFMARK         "<eof>"
#define marklen         (sizeof(EOFMARK) / sizeof(char) - 1)

/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete (lua_State *L, int status) {
    if (status == LUA_ERRSYNTAX) {
        size_t lmsg;
        const char *msg = lua_tolstring(L, -1, &lmsg);
        if ( (lmsg >= marklen) &&
             (strcmp(msg + lmsg - marklen, EOFMARK) == 0) ) {
            lua_pop(L, 1);
            return 1;
        }
    }
    return 0; /* else... */
}

/*
** Try to compile line on the stack as 'return <line>;'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int add_return (lua_State *L) {
    const char *line = lua_tostring(L, -1); /* original line */
    const char *retline = lua_pushfstring(L, "return %s;", line);
    int status = luaL_loadbuffer(L, retline, strlen(retline), "=stdin");
    if (status == LUA_OK) {
        lua_remove(L, -2); /* remove modified line */
        lua_remove(L, -2); /* remove original line */
    }
    else {
        lua_pop(L, 2);     /* pop result from 'luaL_loadbuffer' and modified
                            * line */
    }
    return status;
}

/*
** try the line on the top of the stack.
** if incomplete, add it to the save buffer,
** otherwise return with compiled chunk on stack
*/
static int try_statement(lua_State *L) {
    size_t len;
    int status;
    char *line = (char *)lua_tolstring(L, 1, &len); /* get what it has */
    char *buf;

    if(continuing) {
        buf = malloc(saveBufLen + 1 + strlen(line) + 1); /* add to saved */
        sprintf(buf, "%s\n%s", saveBuf, line);
        len += saveBufLen + 1;
    } else {
        buf = line;
    }
    status = luaL_loadbuffer(L, buf,  len, "=stdin"); /* try it */

    if( incomplete(L, status) ) {
        status = STATUS_INCOMPLETE;
        save_statement_buffer(buf);
    } else {
        clear_statement_buffer();
        // remove line from stack, leaving compiled chunk
        lua_remove(L, -2);
    }
    return status;
}

/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void l_print (lua_State *L) {
    int n = lua_gettop(L);
    if (n > 0) { /* any result to be printed? */
        luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
        lua_getglobal(L, "print");
        lua_insert(L, 1);
        if (lua_pcall(L, n, 0, 0) != LUA_OK) {
            l_message( progname,
                       lua_pushfstring( L, "error calling 'print' (%s)",
                                        lua_tostring(L, -1) ) );
        }
        fflush(stdout);
    }
}

// push and evaluate a line buffer provided by caller
int l_handle_line (lua_State *L, char *line) {
    // fprintf(stderr, "l_handle_line: %s \n", line);
    size_t l;
    int status;
    lua_settop(L, 0);
    l = strlen(line);
    if ( (l > 0) && (line[l - 1] == '\n') ) {
        line[--l] = '\0';
    }

    lua_pushlstring(L, line, l);
    // try evaluating as an expression
    status = add_return(L);
    if(status == LUA_OK) {
        goto call;
    }
    // try as a statement, maybe with continuation
    status = try_statement(L);
    if (status == LUA_OK) {
        goto call;
    }

    if(status == STATUS_INCOMPLETE) {
        fprintf(stderr, "<incomplete>\n");
        goto exit;
    }

call: // call the compiled function on the top of the stack
    status = docall(L, 0, LUA_MULTRET);
    if (status == LUA_OK) {
        //    printf("<evaluation completed with %d stack elements>\n",
        // lua_gettop(L));
        if(lua_gettop(L) == 0) {
            fprintf(stderr, "<ok>\n");
        }
        l_print(L);
        fprintf(stderr, "\n");
    } else {
        report(L, status);
    }

exit:
    lua_settop(L, 0); /* clear stack */
    // caller is responsible for freeing the buffer
    return 0;
}
