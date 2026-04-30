/**
 *  Copyright (C) 2024 Masatoshi Fukunaga
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */
#include <sys/uio.h>
#include <unistd.h>
// lua
#include <lauxhlib.h>
#include <lua_errno.h>

static int writev_lua(lua_State *L)
{
    int fd            = -1;
    struct iovec *iov = NULL;
    int iovcnt        = 0;
    size_t total_len  = 0;
    ssize_t nwritten  = 0;

    // check fd
    if (lauxh_isint(L, 1)) {
        fd = lauxh_checkint(L, 1);
    } else {
        fd = lauxh_fileno(L, 1);
    }
    // check iovec table
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_settop(L, 2);

    // build iovec array
    iovcnt = lauxh_rawlen(L, 2);
    iov    = (struct iovec *)lua_newuserdata(L, sizeof(struct iovec) * iovcnt);
    for (int i = 0; i < iovcnt; i++) {
        lua_rawgeti(L, 2, i + 1);
        if (lua_isnoneornil(L, -1)) {
            // set empty string
            iov[i].iov_base = "";
            iov[i].iov_len  = 0;
        } else if (lua_type(L, -1) != LUA_TSTRING) {
            return lauxh_argerror(L, 2, "table#%d: string expected, got %s",
                                  i + 1, luaL_typename(L, -1));
        } else {
            iov[i].iov_base = (void *)lua_tolstring(L, -1, &iov[i].iov_len);
        }
        total_len += iov[i].iov_len;
        lua_pop(L, 1);
    }

    if (iovcnt == 0) {
        // no data to write
        lua_pushinteger(L, 0);
        return 1;
    }

    nwritten = writev(fd, iov, iovcnt);
    if (nwritten == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            // again - need to merge remaining data
            lua_pushinteger(L, 0);
            lua_pushnil(L);
            lua_pushboolean(L, 1);
            lua_pushvalue(L, 2);
            return 4;
        } else if (errno == EPIPE) {
            // closed by peer
            return 0;
        }
        lua_pushnil(L);
        lua_errno_new(L, errno, "writev");
        return 2;
    }

    // all data written
    if ((size_t)nwritten == total_len) {
        lua_pushinteger(L, nwritten);
        return 1;
    }

    // partial write - remove written data from table
    lua_pushinteger(L, nwritten);
    lua_pushnil(L);
    lua_pushboolean(L, 1);
    lua_createtable(L, iovcnt, 0); // new table for remaining iovecs
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len <= (size_t)nwritten) {
            // skip this iovec
            nwritten -= iov[i].iov_len;
            continue;
        }

        // push remaining data
        lua_pushlstring(L, (const char *)iov[i].iov_base + nwritten,
                        iov[i].iov_len - nwritten);
        lua_rawseti(L, -2, 1);
        // copy remaining iovecs
        i++;
        for (int j = 2; i < iovcnt; i++, j++) {
            lua_rawgeti(L, 2, i + 1);
            lua_rawseti(L, -2, j);
        }
    }

    return 4;
}

LUALIB_API int luaopen_io_write(lua_State *L)
{
    lua_errno_loadlib(L);
    lua_pushcfunction(L, writev_lua);
    return 1;
}
