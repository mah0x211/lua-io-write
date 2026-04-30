/**
 *  Copyright (C) 2024-2026 Masatoshi Fukunaga
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

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
// lua
#include <lauxhlib.h>
#include <lua.h>
#include <lua_errno.h>

#ifndef IOV_MAX
# ifdef _XOPEN_IOV_MAX
#  define IOV_MAX _XOPEN_IOV_MAX
# else
#  define IOV_MAX 16
# endif
#endif

// sync_fp: resync FILE* position to the current fd position after read(2).
// Non-seekable fds (pipes, sockets) return -1 from lseek; skip the sync.
// Returns 0 on success or when skipped, -1 on fseek failure (errno is set).
static inline int sync_fp(FILE *fp, int fd)
{
    if (fp) {
        off_t pos = lseek(fd, 0, SEEK_CUR);
        if (pos >= 0) {
            return fseek(fp, pos, SEEK_SET);
        }
    }
    return 0; // non-seekable fd; no position to sync
}

typedef struct {
    int iovmax;
    int iovcnt;
    struct iovec *iov;
    lua_Integer *idx2key;
} write_iov_t;

static int push_error_result(lua_State *L, write_iov_t *wiov, ssize_t n)
{
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        // NOTE: report zero-byte write on EAGAIN/EWOULDBLOCK/EINTR to allow
        // retrying
        lua_pushinteger(L, 0);
        if (n != -1) {
            // zero-byte write; no error
            return 1;
        }
        lua_pushnil(L);
        lua_pushboolean(L, 1);
        if (!wiov) {
            // no iovec; just return the error result
            return 3;
        }
        // just return the original table
        lua_pushvalue(L, 2);
        return 4;
    }

    // other errors
    lua_pushnil(L);
    lua_errno_new(L, errno, "write");
    return 2;
}

static int push_result(lua_State *L, write_iov_t *wiov, FILE *fp, int fd,
                       size_t len, ssize_t n)
{
    if (n <= 0) {
        return push_error_result(L, wiov, n);
    }

    // sync FILE* position to fd position after write; skip for non-seekable fds
    if (sync_fp(fp, fd) < 0) {
        // if sync fails, report error but ignore the write result
        lua_pushnil(L);
        lua_errno_new(L, errno, "write.sync");
        return 2;
    }

    lua_pushinteger(L, n);
    // fully written
    if ((size_t)n == len) {
        return 1;
    }

    // partial write
    lua_pushnil(L);
    lua_pushboolean(L, 1);
    if (!wiov) {
        // no iovec
        return 3;
    }

    // build remaining table: unwritten suffix of partial entry + all subsequent
    // entries
    for (int head = 0; head < wiov->iovcnt; head++) {
        if ((size_t)n >= wiov->iov[head].iov_len) {
            // skip fully written entry
            n -= (ssize_t)wiov->iov[head].iov_len;
            continue;
        }

        // Partial write in this entry
        lua_createtable(L, wiov->iovcnt - head, 0);

        // Add remaining suffix of the head entry
        lua_rawgeti(L, 2, (int)wiov->idx2key[head]);
        lua_pushlstring(L, (const char *)wiov->iov[head].iov_base + n,
                        wiov->iov[head].iov_len - (size_t)n);
        lua_replace(L, -2);
        lua_rawseti(L, -2, 1);

        // Add remaining entries after the head entry
        head++;
        for (int i = 2; head < wiov->iovcnt; i++, head++) {
            lua_rawgeti(L, 2, (int)wiov->idx2key[head]);
            lua_rawseti(L, -2, i);
        }
    }
    return 4;
}

static int writev_lua(lua_State *L, int fd, FILE *fp, off_t offset,
                      write_iov_t *wiov)
{
    size_t len = 0;
    ssize_t n  = 0;
    char empty = 0;

    // validate argument type and count; allow sparse iovec with holes (nil/none
    // values)
    lauxh_argcheck(L, lua_istable(L, 2), 2, "string or table expected");
    lua_settop(L, 2);

    // build iovec array from Lua strings
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (!lauxh_ispint(L, -2)) {
            // skip non-positive integer keys to allow sparse iovec with holes
            lua_pop(L, 1);
            continue;
        } else if (wiov->iovcnt == wiov->iovmax) {
            // exceed system limit; fail with EINVAL
            lua_pushnil(L);
            lua_errno_new(L, EINVAL, "write.iovcnt");
            return 2;
        }

        wiov->idx2key[wiov->iovcnt] = lua_tointeger(L, -2);
        switch (lua_type(L, -1)) {
        case LUA_TSTRING:
            wiov->iov[wiov->iovcnt].iov_base =
                (void *)lua_tolstring(L, -1, &wiov->iov[wiov->iovcnt].iov_len);
            len += wiov->iov[wiov->iovcnt].iov_len;
            if (wiov->iov[wiov->iovcnt].iov_len > 0) {
                // insert into sorted position to keep iov ordered by key
                lua_Integer new_key  = wiov->idx2key[wiov->iovcnt];
                struct iovec new_iov = wiov->iov[wiov->iovcnt];
                int j                = wiov->iovcnt - 1;
                while (j >= 0 && wiov->idx2key[j] > new_key) {
                    wiov->idx2key[j + 1] = wiov->idx2key[j];
                    wiov->iov[j + 1]     = wiov->iov[j];
                    j--;
                }
                wiov->idx2key[j + 1] = new_key;
                wiov->iov[j + 1]     = new_iov;
                wiov->iovcnt++;
            }
        case LUA_TNIL:
            // skip holes in the iovec
            break;

        default:
            return lauxh_argerror(L, 2,
                                  "table#%d must be string or nil (got %s)",
                                  lua_tointeger(L, -2), luaL_typename(L, -1));
        }
        lua_pop(L, 1);
    }

    if (wiov->iovcnt < 1) {
        // nothing to write; succeed with zero-byte write
        n = (offset >= 0) ? pwrite(fd, &empty, 0, offset) :
                            write(fd, &empty, 0);
        if (n < 0) {
            lua_pushnil(L);
            lua_errno_new(L, errno, "write");
            return 2;
        }
        lua_pushinteger(L, 0);
        return 1;
    }

    n = (offset >= 0) ? pwritev(fd, wiov->iov, wiov->iovcnt, offset) :
                        writev(fd, wiov->iov, wiov->iovcnt);
    return push_result(L, wiov, fp, fd, len, n);
}

static int write_lua(lua_State *L)
{
    write_iov_t *wiov = lua_touserdata(L, lua_upvalueindex(1));
    int fd            = -1;
    FILE *fp          = NULL;
    off_t offset      = 0;
    size_t len        = 0;
    const char *str   = NULL;
    ssize_t n         = 0;

    if (lauxh_isint(L, 1)) {
        fd = lauxh_checkint(L, 1);
    } else if (!(fp = lauxh_checkfile(L, 1))) {
        // file has been closed; report EBADF immediately
        lua_pushnil(L);
        lua_errno_new(L, EBADF, "write");
        return 2;
    } else if (fflush(fp) != 0 && errno != EBADF) {
        // NOTE: ignore EBADF which means the FILE* is not opened for
        // writing
        lua_pushnil(L);
        lua_errno_new(L, errno, "write.fflush");
        return 2;
    } else {
        fd = fileno(fp);
    }

    // get offset: -1 for current fd position, >=0 for pwrite
    offset = lauxh_optinteger(L, 3, -1);

    if (!lauxh_isstring(L, 2)) {
        // table of strings; use writev(2)
        wiov->iovcnt = 0;
        return writev_lua(L, fd, fp, offset, wiov);
    }
    lua_settop(L, 2);

    // string argument; use write(2)
    str = lua_tolstring(L, 2, &len);
    n   = (offset >= 0) ? pwrite(fd, str, len, offset) : write(fd, str, len);
    return push_result(L, NULL, fp, fd, len, n);
}

LUALIB_API int luaopen_io_write(lua_State *L)
{
    long iovmax = sysconf(_SC_IOV_MAX);

    if (iovmax < 0 || iovmax > IOV_MAX) {
        // fallback to a reasonable default if sysconf fails or returns an
        // unexpected value; this should not happen on compliant systems
        iovmax = IOV_MAX;
    }

    lua_errno_loadlib(L);

    // allocate write_iov_t as upvalue[1]; iov array as upvalue[2];
    // idx2key array as upvalue[3]; all three are GC-managed by Lua
    write_iov_t *wiov = lua_newuserdata(L, sizeof(write_iov_t));
    wiov->iovcnt      = 0;
    wiov->iovmax      = (int)iovmax;
    wiov->iov     = lua_newuserdata(L, (size_t)iovmax * sizeof(struct iovec));
    wiov->idx2key = lua_newuserdata(L, (size_t)iovmax * sizeof(lua_Integer));

    lua_pushcclosure(L, write_lua, 3);
    return 1;
}
