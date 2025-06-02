/*
* Binding for the libssh2 library. Note that there is not a one-to-one correspondence
* between functions in libssh2 and the binding.
* Currently, during the ssh2 handshake, a call to nsock.receive may result in an EOF
* error. This appears to only occur when stressing the ssh server (ie during a brute
* force attempt) or while behind a restrictive firewall/IDS.
* by Devin Bjelland
*/

extern "C" {
#include "libssh2.h"
}
#include "nse_lua.h"

#include "nse_nsock.h"
#include "nse_utility.h"
#include "nbase.h"

#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef WIN32
#include <Windows.h>
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Fcntl.h>
#include <io.h>
#include <assert.h>
#else
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif


enum {
    SSH2_UDATA = lua_upvalueindex(1)
};

#ifdef WIN32
struct ssh_userdata {
    SOCKET sp[2];
    LIBSSH2_SESSION *session;
};
#else
struct ssh_userdata {
    int sp[2];
    LIBSSH2_SESSION *session;
};
#endif


#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf c99_snprintf
#define vsnprintf c99_vsnprintf

__inline int c99_vsnprintf(char *outBuf, size_t size, const char *format, va_list ap) {
    int count = -1;

    if (size != 0)
        count = _vsnprintf_s(outBuf, size, _TRUNCATE, format, ap);
    if (count == -1)
        count = _vscprintf(format, ap);

    return count;
}

__inline int c99_snprintf(char *outBuf, size_t size, const char *format, ...) {
    int count;
    va_list ap;

    va_start(ap, format);
    count = c99_vsnprintf(outBuf, size, format, ap);
    va_end(ap);

    return count;
}
#endif

#ifdef WIN32
/*
*   make_socketpair:
*   If make_overlapped is nonzero, both sockets created will be usable for
*   "overlapped" operations via WSASend etc.  If make_overlapped is zero,
*   socks[0] (only) will be usable with regular ReadFile etc., and thus
*   suitable for use as stdin or stdout of a child process.  Note that the
*   sockets must be closed with closesocket() regardless.
*/

int make_socketpair (SOCKET socks[2], int make_overlapped) {
    union {
        struct sockaddr_in inaddr;
        struct sockaddr addr;
    } a;
    SOCKET listener;
    int e;
    socklen_t addrlen = sizeof(a.inaddr);
    DWORD flags = (make_overlapped ? WSA_FLAG_OVERLAPPED : 0);
    int reuse = 1;

    if (socks == 0) {
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    socks[0] = socks[1] = -1;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == -1)
        return SOCKET_ERROR;

    memset(&a, 0, sizeof(a));
    a.inaddr.sin_family = AF_INET;
    a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.inaddr.sin_port = 0;

    for (;;) {
        if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
            (char*)&reuse, (socklen_t) sizeof(reuse)) == -1)
            break;
        if (bind(listener, &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
            break;

        memset(&a, 0, sizeof(a));
        if (getsockname(listener, &a.addr, &addrlen) == SOCKET_ERROR)
            break;
        // win32 getsockname may only set the port number, p=0.0005.
        // ( http://msdn.microsoft.com/library/ms738543.aspx ):
        a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.inaddr.sin_family = AF_INET;

        if (listen(listener, 1) == SOCKET_ERROR)
            break;

        socks[0] = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, flags);
        if (socks[0] == -1)
            break;
        if (connect(socks[0], &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
            break;

        socks[1] = accept(listener, NULL, NULL);
        if (socks[1] == -1)
            break;

        closesocket(listener);
        return 0;
    }

    e = WSAGetLastError();
    closesocket(listener);
    closesocket(socks[0]);
    closesocket(socks[1]);
    WSASetLastError(e);
    socks[0] = socks[1] = -1;
    //return SOCKET_ERROR;

    return -1;
}
#else
int make_socketpair (int socks[2], int dummy) {
    if (socks == 0) {
        errno = EINVAL;
        return -1;
    }

    dummy = socketpair(AF_UNIX, SOCK_STREAM, 0, socks);

    if (dummy) {
        socks[0] = socks[1] = -1;
    }

    return dummy;
}
#endif


static int ssh_error (lua_State *L, LIBSSH2_SESSION *session, const char *msg) {
    char *errmsg;
    libssh2_session_last_error(session, &errmsg, NULL, 0);

    return nseU_safeerror(L, "%s: %s", msg, errmsg);
}

static int finish_send (lua_State *L, int status, lua_KContext ctx) {
    if (lua_toboolean(L, -2))
        return 0;
    else
        return lua_error(L); /* uses idx 6 */
}

static int finish_read (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    struct ssh_userdata *sshu = NULL;

    sshu = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");

    if (lua_toboolean(L, -2)) {
        size_t n = 0;
        size_t l = 0;
        lua_getuservalue(L, 1);
        lua_getfield(L, -1, "sp_buff");
        lua_pushvalue(L, 3);
        lua_concat(L, 2);
        const char *data = lua_tolstring(L, -1, &l);
        lua_pushliteral(L, "");
        lua_setfield(L, 4, "sp_buff");

        while (n < l) {
#ifdef WIN32
            rc = send(sshu->sp[1], data + n, l - n, 0);
#else
            rc = write(sshu->sp[1], data + n, l - n);
#endif
            if (rc == -1 && errno != EAGAIN) {
                luaL_error(L, "Writing to socket pair: %s", strerror(errno));
            }
            else if (rc == -1 && errno == EAGAIN) {
                lua_pushlstring(L, data + n, l - n);
                lua_setfield(L, 4, "sp_buff");
                break;
            }
            else {
                n += rc;
            }
        }
        return 0;
    }
    else {
        return lua_error(L); /* uses idx 6 */
    }
}

static int filter (lua_State *L) {
    int rc;
    char data[4096];
    struct ssh_userdata *sshu = NULL;

    sshu = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");

    lua_getuservalue(L, 1);
    lua_getfield(L, -1, "sock");
    lua_replace(L, -2);

#ifdef WIN32
    rc = recv(sshu->sp[1], data, sizeof(data), 0);

    if (WSAGetLastError() == WSAEWOULDBLOCK)
        rc = 0;
#else
    rc = read(sshu->sp[1], data, sizeof(data));
#endif

    if (rc > 0) {
        //write data to nsock socket
        lua_getfield(L, -1, "send");
        lua_insert(L, -2); /* swap */
        lua_pushlstring(L, data, rc);

        assert(lua_status(L) == LUA_OK);
        lua_callk(L, 2, 2, 0, finish_send);

        return finish_send(L,0,0);
    }
    else if (rc == -1 && errno != EAGAIN)
        return luaL_error(L, "%s", strerror(errno));

    lua_getfield(L, -1, "receive");
    lua_insert(L, -2); /* swap */

    assert(lua_status(L) == LUA_OK);
    lua_callk(L, 1, 2, 0, finish_read);

    return finish_read(L, 0, 0);
}

#define DO_OR_YIELD(_Stmt, _Sshu_index, _Func, _Ctx) \
    while ((_Stmt) == LIBSSH2_ERROR_EAGAIN) { \
        luaL_getmetafield(L, (_Sshu_index), "filter"); \
        lua_pushvalue(L, (_Sshu_index)); \
        lua_callk(L, 1, 0, (_Ctx), (_Func)); \
    }

static int do_session_handshake (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    struct ssh_userdata *sshu = NULL;

    assert(lua_gettop(L) == 4);
    sshu = (struct ssh_userdata *) nseU_checkudata(L, 3, SSH2_UDATA, "ssh2");

    DO_OR_YIELD((rc = libssh2_session_handshake(sshu->session, sshu->sp[0])),
        3, do_session_handshake, ctx);

    if (rc) {
        libssh2_session_free(sshu->session);
        sshu->session = NULL;
        return luaL_error(L, "Unable to complete libssh2 handshake.");
    }

    // lua_pushvalue(L, 3);
    lua_settop(L, 3);

    return 1;
}

static int finish_session_open (lua_State *L, int status, lua_KContext ctx) {
    assert(lua_gettop(L) == 6);
    if (lua_toboolean(L, -2)) {
        lua_pop(L, 2);
        return do_session_handshake(L,0,0);
    }
    else {
        struct ssh_userdata *state = NULL;

        state = (struct ssh_userdata *) nseU_checkudata(L, 3, SSH2_UDATA, "ssh2");
        if (state->session != NULL) {
            libssh2_session_free(state->session);
            state->session = NULL;
        }
        return lua_error(L);
    }
}

/*
* Creates libssh2 session, connects to hostname:port and tries to perform a
* ssh handshake on socket. Returns ssh_state on success, nil on failure.
*
* session_open(hostname, port)
*/
static int l_session_open (lua_State *L) {
    int rc;
    ssh_userdata *state = NULL;

    luaL_checkinteger(L, 2);
    lua_settop(L, 2);

    state = (ssh_userdata *)lua_newuserdatauv(L, sizeof(ssh_userdata), 1); /* index 3 */

    assert(lua_gettop(L) == 3);
    state->session = NULL;
    state->sp[0] = -1;
    state->sp[1] = -1;
    lua_pushvalue(L, lua_upvalueindex(1)); /* metatable */
    lua_setmetatable(L, 3);

    lua_newtable(L);
    lua_setuservalue(L, 3);
    lua_getuservalue(L, 3); /* index 4 - a table associated with userdata*/
    assert(lua_gettop(L) == 4);

    state->session = libssh2_session_init();

    if (state->session == NULL) {
        // A session could not be created because of memory limit
        return nseU_safeerror(L, "trying to initiate session");
    }

    libssh2_session_set_blocking(state->session, 0);

    if (make_socketpair(state->sp, 1) == -1)
        return nseU_safeerror(L, "trying to create socketpair");

#ifdef WIN32
    unsigned long s_mode = 1; // non-blocking

    rc = ioctlsocket(state->sp[1], FIONBIO, (unsigned long *)&s_mode);
    if (rc != NO_ERROR)
        return nseU_safeerror(L, "%s", strerror(errno));
#else
    // get file descriptor flags
    rc = fcntl(state->sp[1], F_GETFD);
    if (rc == -1)
        return nseU_safeerror(L, "%s", strerror(errno));

    // add non-blocking flag and update file descriptor flags
    rc |= O_NONBLOCK;
    rc = fcntl(state->sp[1], F_SETFL, rc);
    if (rc == -1)
        return nseU_safeerror(L, "%s", strerror(errno));
#endif

    lua_getglobal(L, "nmap");
    lua_getfield(L, -1, "new_socket");
    lua_replace(L, -2);
    lua_call(L, 0, 1);
    lua_setfield(L, 4, "sock");
    lua_pushliteral(L, "");
    lua_setfield(L, 4, "sp_buff");
    assert(lua_gettop(L) == 4);

    lua_getfield(L, 4, "sock");
    lua_getfield(L, -1, "connect");
    lua_insert(L, -2); /* swap */
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_callk(L, 3, 2, 3, finish_session_open);
    return finish_session_open(L,0,0);
}

/*
* Returns the SHA1 or MD5 hostkey hash of provided session or nil if it is not available
*/
static int l_hostkey_hash (lua_State *L) {
    luaL_Buffer B;
    static int hash_option[] = { LIBSSH2_HOSTKEY_HASH_MD5, LIBSSH2_HOSTKEY_HASH_SHA1 };
    static int hash_length[] = { 16, 20 };
    static const char *hashes[] = { "md5", "sha1", NULL };
    int type = luaL_checkoption(L, 2, "sha1", hashes);
    struct ssh_userdata *state = NULL;
    const unsigned char *hash = NULL;

    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
    hash = (const unsigned char *) libssh2_hostkey_hash(state->session, hash_option[type]);

    if (hash == NULL)
        return nseU_safeerror(L, "could not get hostkey hash");

    luaL_buffinit(L, &B);
    for (int i = 0; i < hash_length[type]; i++) {
        char byte[3]; /* with space for NULL */
        snprintf(byte, sizeof(byte), "%02X", (unsigned int)hash[i]);
        if (i)
            luaL_addchar(&B, ':');
        luaL_addlstring(&B, byte, 2);
    }
    luaL_pushresult(&B);

    return 1;
}

static int l_set_timeout(lua_State *L) {
    long timeout = luaL_checkinteger(L, 2);
    struct ssh_userdata *state = NULL;
    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");

    libssh2_session_set_timeout(state->session, timeout);

    return 0;
}

static int userauth_list (lua_State *L, int status, lua_KContext ctx) {
    char *auth_list = NULL;
    struct ssh_userdata *state = NULL;
    const char *username = luaL_checkstring(L, 2);

    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
    assert(state->session != NULL);

    DO_OR_YIELD(((auth_list = libssh2_userauth_list(state->session, username, lua_rawlen(L, 2))) == NULL ?
          libssh2_session_last_errno(state->session) : LIBSSH2_ERROR_NONE),
        1, userauth_list, ctx);

    if (auth_list) {
        const char *auth = strtok(auth_list, ",");
        lua_newtable(L);
        do {
            lua_pushstring(L, auth);
            lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
        }
        while ((auth = strtok(NULL, ",")));

        //libssh2_free(state->session, (void *)auth_list);
    }
    else if (libssh2_userauth_authenticated(state->session)) {
        lua_pushliteral(L, "none_auth");
    }
    else {
        return ssh_error(L, state->session, "userauth_list");
    }

    return 1;
}

/*
* Returns list of supported authentication methods
*/
static int l_userauth_list (lua_State *L) {
    return userauth_list(L, 0, 0);
}

static int userauth_banner (lua_State *L, int status, lua_KContext ctx) {
    char *auth_banner = NULL;
    struct ssh_userdata *state = NULL;

    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
    assert(state->session != NULL);

    if (LIBSSH2_ERROR_NONE == libssh2_userauth_banner(state->session, &auth_banner))
    {
      lua_pushstring(L, auth_banner);
      return 1;
    }
    return 0;
}

/*
* Returns pre-auth banner
*/
static int l_userauth_banner (lua_State *L) {
    return userauth_banner(L, 0, 0);
}

struct publickey_ctx {
  struct ssh_userdata *state;
  const char *username;
  size_t username_len;
  const char *privkey;
  size_t privkey_len;
  const char *pubkey;
  size_t pubkey_len;
  const char *passphrase;
};

static void validate_publickey_params(lua_State *L, struct publickey_ctx *ctx) {
  memset(ctx, 0, sizeof(struct publickey_ctx));
  ctx->state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
  ctx->username = luaL_checklstring(L, 2, &ctx->username_len);
  ctx->privkey = luaL_checklstring(L, 3, &ctx->privkey_len);

  ctx->passphrase = lua_isstring(L, 4) ? lua_tostring(L, 4) : NULL;
  ctx->pubkey = lua_isstring(L, 5) ? lua_tolstring(L, 5, &ctx->pubkey_len) : NULL;
}

static int userauth_publickey (lua_State *L, int status, lua_KContext ctx) {
    struct publickey_ctx *context = (struct publickey_ctx *)ctx;
    int rc;
    DO_OR_YIELD((rc = libssh2_userauth_publickey_fromfile_ex(
        context->state->session, context->username, context->username_len,
        context->pubkey, context->privkey, context->passphrase
        )),
        1, userauth_publickey, ctx);

    lua_pushboolean(L, (rc == 0));

    return 1;
}

static int l_userauth_publickey (lua_State *L) {
  publickey_ctx *params = NULL;
  params = (publickey_ctx *)lua_newuserdatauv(L, sizeof(publickey_ctx), 0);
  validate_publickey_params(L, params);
  return userauth_publickey(L, 0, (lua_KContext) params);
}

static int userauth_publickey_frommemory (lua_State *L, int status, lua_KContext ctx) {
    struct publickey_ctx *context = (struct publickey_ctx *)ctx;
    int rc;
    DO_OR_YIELD((rc = libssh2_userauth_publickey_frommemory(
        context->state->session, context->username, context->username_len,
        context->pubkey, context->pubkey_len, context->privkey,
        context->privkey_len, context->passphrase
        )),
        1, userauth_publickey_frommemory, ctx);

    lua_pushboolean(L, (rc == 0));

    return 1;
}

static int l_userauth_publickey_frommemory (lua_State *L) {
  publickey_ctx *params = NULL;
  params = (publickey_ctx *)lua_newuserdatauv(L, sizeof(publickey_ctx), 0);
  validate_publickey_params(L, params);
  return userauth_publickey_frommemory(L, 0, (lua_KContext) params);
}

static int l_read_publickey (lua_State *L) {
    FILE *fd;
    char c;
    const char* publickeyfile = luaL_checkstring(L, 1);
    luaL_Buffer publickey_data;

    lua_getglobal(L, "require");
    lua_pushliteral(L, "base64");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "dec");

    fd = fopen(publickeyfile, "r");
    if (!fd)
        return luaL_error(L, "Error reading file");

    luaL_buffinit(L, &publickey_data);
    while (fread(&c, 1, 1, fd) && c!= '\r' && c != '\n' && c != ' ') {
        continue;
    }
    while (fread(&c, 1, 1, fd) && c!= '\r' && c != '\n' && c != ' ') {
        luaL_addchar(&publickey_data, c);
    }
    fclose(fd);

    luaL_pushresult(&publickey_data);
    lua_call(L, 1, 1);

    return 1;
}

static int publickey_canauth_cb (LIBSSH2_SESSION *session, unsigned char **sig,
    size_t *sig_len, const unsigned char *data, size_t data_len, void **abstract) {
    // Must return an error, any error, other than LIBSSH2_ERROR_EAGAIN
    return LIBSSH2_ERROR_PUBLICKEY_PROTOCOL;
}

static int publickey_canauth (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    char *errmsg;
    const char *username;
    unsigned const char *publickey_data;
    size_t len = 0;
    struct ssh_userdata *state;

    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
    username = luaL_checkstring(L, 2);

    if (lua_isstring(L, 3))
        publickey_data = (unsigned const char*)lua_tolstring(L, 3, &len);
    else
        return luaL_error(L, "Invalid public key");

    DO_OR_YIELD((rc = libssh2_userauth_publickey(state->session,
        username, publickey_data, len, &publickey_canauth_cb, NULL)),
        1, publickey_canauth, ctx);

    libssh2_session_last_error(state->session, &errmsg, NULL, 0);

    if (rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED && !strncmp("Callback", errmsg, 8))
        // The username/publickey combination has been accepted because
        // the authentication flow progressed all the way to our dummy
        // callback where the private key is needed
        lua_pushboolean(L, 1);
    else if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED)
        // The server rejected the username/publickey combination
        lua_pushboolean(L, 0);
    else
        return luaL_error(L, "Invalid public key: %s", errmsg);

    return 1;
}

static int l_publickey_canauth (lua_State *L) {
    return publickey_canauth(L, 0, 0);
}

/*
* Attempts to authenticate session with provided username and password
* returns true on success and false otherwise
*
* userauth_password(state, username, password)
*/
static int userauth_password (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    const char *username, *password;
    struct ssh_userdata *state;

    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
    username = luaL_checkstring(L, 2);
    password = luaL_checkstring(L, 3);

    assert(state->session != NULL);
    DO_OR_YIELD((rc = libssh2_userauth_password(state->session, username, password)),
        1, userauth_password, ctx);

    if (rc == 0)
        lua_pushboolean(L, 1);
    else
        lua_pushboolean(L, 0);

    return 1;
}

static int l_userauth_password (lua_State *L) {
    return userauth_password(L, 0, 0);
}

static int session_close (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    struct ssh_userdata *state;

    state = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");

    if (state->session != NULL) {
        DO_OR_YIELD((rc = libssh2_session_disconnect(state->session, "Normal Shutdown")),
            1, session_close, ctx);

        if (rc < 0)
            return luaL_error(L, "unable to disconnect session");

        if (libssh2_session_free(state->session) < 0)
            return luaL_error(L, "unable to free session");

        state->session = NULL;
    }

    return 0;
}

static int l_session_close (lua_State *L) {
    return session_close(L, 0, 0);
}

static int channel_read (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    char buf[2048];
    size_t buflen = 2048;
    LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL *) lua_touserdata(L, 2);
    int stream_id = luaL_checkinteger(L, 3);

    DO_OR_YIELD((rc = libssh2_channel_read_ex(channel, stream_id, buf, buflen)),
        1, channel_read, ctx);

    if (rc > 0) {
        lua_pushlstring(L, buf, rc);
        return 1;
    }
    else if (rc < 0)
        return luaL_error(L, "Reading from channel");

    lua_pushnil(L);

    return 1;
}

static int l_channel_read (lua_State *L) {
    lua_pushinteger(L, 0);
    return channel_read(L, 0, 0);
}

static int l_channel_read_stderr(lua_State *L) {
    lua_pushinteger(L, SSH_EXTENDED_DATA_STDERR);
    return channel_read(L, 0, 0);
}

static int channel_write (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    const char *buf;
    size_t buflen = 0;
    LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL *) lua_touserdata(L, 2);

    if (lua_isstring(L, 3))
        buf = lua_tolstring(L, 3, &buflen);
    else
        return luaL_error(L, "Invalid buffer");

    DO_OR_YIELD((rc = libssh2_channel_write(channel, buf, buflen)),
        1, channel_write, ctx);

    if (rc < 0)
        return luaL_error(L, "Writing to channel");

    lua_pushinteger(L, rc);
    return 1;
}

static int l_channel_write (lua_State *L) {
    return channel_write(L, 0, 0);
}

static int channel_exec (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    // ssh_userdata *state = (ssh_userdata *)lua_touserdata(L, 1);
    LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL *) lua_touserdata(L, 2);
    const char *cmd = luaL_checkstring(L, 3);

    DO_OR_YIELD((rc = libssh2_channel_exec(channel, cmd)),
        1, channel_exec, ctx);
    if (rc != 0)
        return luaL_error(L, "Error executing command");

   return 0;
}

static int l_channel_exec (lua_State *L) {
    return channel_exec(L, 0, 0);
}

static int l_channel_eof(lua_State *L) {
    int result;
    LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL *) lua_touserdata(L, 1);

    result = libssh2_channel_eof(channel);
    if (result >= 0)
        lua_pushboolean(L, result);
    else
        return luaL_error(L, "Error checking for EOF");

    return 1;
}

static int channel_send_eof(lua_State *L, int status, lua_KContext ctx) {
    int rc;
    // ssh_userdata *state = (ssh_userdata *)lua_touserdata(L, 1);
    LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL *) lua_touserdata(L, 2);

    DO_OR_YIELD((rc = libssh2_channel_send_eof(channel)),
        1, channel_send_eof, ctx);
    if (rc != 0)
        return luaL_error(L, "Error sending EOF");

    return 0;
}

static int l_channel_send_eof(lua_State *L) {
    return channel_send_eof(L, 0, 0);
}

struct channel_context {
  bool request_pty;
  LIBSSH2_CHANNEL *channel;
};

static int setup_channel(lua_State *L, int status, lua_KContext ctx) {
    int rc;
    channel_context *channel_ctx = (channel_context *) ctx;
    ssh_userdata *state = (ssh_userdata *)lua_touserdata(L, 1);

    if (channel_ctx->channel == NULL) {
      DO_OR_YIELD(((channel_ctx->channel = libssh2_channel_open_session(state->session)) == NULL ?
            libssh2_session_last_errno(state->session) : LIBSSH2_ERROR_NONE),
          1, setup_channel, ctx);
      if (channel_ctx->channel == NULL) {
        free(channel_ctx);
        return luaL_error(L, "Opening channel");
      }
    }

    if (channel_ctx->request_pty) {
      DO_OR_YIELD((rc = libssh2_channel_request_pty(channel_ctx->channel, "vanilla")),
          1, setup_channel, ctx);
      if (rc != 0) {
        free(channel_ctx);
        return luaL_error(L, "Requesting pty");
      }
      channel_ctx->request_pty = false; // make sure we don't enter this block again
    }

    lua_pushlightuserdata(L, channel_ctx->channel);
    free(channel_ctx);

    return 1;
}

static int l_open_channel (lua_State *L) {
    //ssh_userdata *state = (ssh_userdata *)lua_touserdata(L, 1);
    bool no_pty = false;
    if (lua_gettop(L) > 1) {
      no_pty = lua_toboolean(L, 2);
    }
    lua_settop(L, 1);

    channel_context *ctx =  (channel_context *)safe_zalloc(sizeof(channel_context));
    ctx->request_pty = !no_pty;

    return setup_channel(L, 0, (lua_KContext) ctx);
}

static int channel_close (lua_State *L, int status, lua_KContext ctx) {
    int rc;
    // ssh_userdata *state = (ssh_userdata *)lua_touserdata(L, 1);
    LIBSSH2_CHANNEL *channel = (LIBSSH2_CHANNEL *) lua_touserdata(L, 2);

    DO_OR_YIELD((rc = libssh2_channel_close(channel)),
        1, channel_close, ctx);
    if (rc != 0)
        return luaL_error(L, "Error closing channel");;

    return 0;
}

static int l_channel_close (lua_State *L) {
    return channel_close(L, 0, 0);
}

static const struct luaL_Reg libssh2[] = {
    { "session_open", l_session_open },
    { "hostkey_hash", l_hostkey_hash },
    { "set_timeout", l_set_timeout },
    { "userauth_banner", l_userauth_banner },
    { "userauth_list", l_userauth_list },
    { "userauth_publickey", l_userauth_publickey },
    { "userauth_publickey_frommemory", l_userauth_publickey_frommemory },
    { "read_publickey", l_read_publickey },
    { "publickey_canauth", l_publickey_canauth },
    { "userauth_password", l_userauth_password },
    { "session_close", l_session_close },
    { "open_channel", l_open_channel},
    { "channel_read", l_channel_read},
    { "channel_read_stderr", l_channel_read_stderr},
    { "channel_write", l_channel_write},
    { "channel_exec", l_channel_exec},
    { "channel_send_eof", l_channel_send_eof},
    { "channel_eof", l_channel_eof},
    { "channel_close", l_channel_close},
    { NULL, NULL }
};

static int gc (lua_State *L) {
    struct ssh_userdata *sshu = NULL;

    sshu = (struct ssh_userdata *) nseU_checkudata(L, 1, SSH2_UDATA, "ssh2");
    if (!sshu) { return 0; }
    if (sshu) {
        // lua_pushvalue(L, lua_upvalueindex(1));
        // lua_getfield(L, -1, "session_close");
        // lua_insert(L, -2); /* swap */
        // lua_pcall(L, 1, 0, 0); /* if an error occurs, don't do anything */

        if (sshu->session != NULL) {
            if (libssh2_session_free(sshu->session) < 0) {
                // Unable to free libssh2 session
            }
            sshu->session = NULL;
        }
    }

#ifdef WIN32
    closesocket(sshu->sp[0]);
    closesocket(sshu->sp[1]);
#else
    close(sshu->sp[0]);
    close(sshu->sp[1]);
#endif

    return 0;
}

int luaopen_libssh2 (lua_State *L) {
    lua_settop(L, 0); /* clear the stack */

    luaL_newlibtable(L, libssh2);

    lua_newtable(L); /* ssh2 session metatable */
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, gc, 1);
    lua_setfield(L, -2, "__gc");
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, filter, 1);
    lua_setfield(L, -2, "filter");

    luaL_setfuncs(L, libssh2, 1);

    static bool libssh2_initialized = false;
    if (!libssh2_initialized && (libssh2_init(0) != 0))
        luaL_error(L, "unable to open libssh2");
    libssh2_initialized = true;

    return 1;
}
