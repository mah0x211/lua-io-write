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

// depend
#include "lauxhlib.h"
#include "lua_errno.h"
// lua
#include <lauxlib.h>
// system
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>

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

// push_error_result: push return values when n <= 0.
// remaining: effective bytes not yet written (passed through from the caller).
static int push_error_result(lua_State *L, ssize_t n, size_t remaining)
{
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        lua_pushinteger(L, 0);
        lua_pushnil(L);
        if (n == -1) {
            // EAGAIN/EWOULDBLOCK/EINTR: signal the caller to retry
            lua_pushboolean(L, 1);
        } else {
            // zero-byte write returned by the OS; no error, no retry needed
            lua_pushnil(L);
        }
        lua_pushinteger(L, (lua_Integer)remaining);
        return 4;
    }

    // other errors
    lua_pushnil(L);
    lua_errno_new(L, errno, "write");
    return 2;
}

// push_result: push return values after a write syscall.
// len: effective bytes attempted (total data length minus pos).
// n:   bytes written by the syscall, or <=0 on error/EAGAIN.
//
// Always returns exactly 4 values when n > 0:
//   n, nil, nil,  0        -- fully written
//   n, nil, true, remain   -- partial write; remain = len - n
static int push_result(lua_State *L, FILE *fp, int fd, size_t len, ssize_t n)
{
    if (n > 0) {
        size_t remaining = ((size_t)n < len) ? len - (size_t)n : 0;

        // sync FILE* position to fd position after write; skip for non-seekable
        // fds
        if (sync_fp(fp, fd) < 0) {
            /* LCOV_EXCL_START - fseek failure is not reproducible in tests */
            lua_pushnil(L);
            lua_errno_new(L, errno, "write.sync");
            return 2;
            /* LCOV_EXCL_STOP */
        }

        lua_pushinteger(L, n);
        lua_pushnil(L);
        if (remaining) {
            lua_pushboolean(L, 1);
        } else {
            lua_pushnil(L);
        }
        lua_pushinteger(L, (lua_Integer)remaining);
        return 4;
    }

    return push_error_result(L, n, len);
}

static int writev_lua(lua_State *L, int fd, FILE *fp, off_t offset,
                      write_iov_t *wiov, lua_Integer pos)
{
    size_t len = 0;
    ssize_t n  = 0;
    char empty = 0;

    // validate argument type and count; allow sparse iovec with holes
    lauxh_argcheck(L, lua_istable(L, 2), 2, "string or table expected");
    lua_settop(L, 2);

    // build iovec array from Lua strings, consuming pos bytes during iteration
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (!lauxh_ispint(L, -2)) {
            // skip non-positive integer keys
            lua_pop(L, 1);
            continue;
        }

        switch (lua_type(L, -1)) {
        case LUA_TSTRING: {
            lua_Integer new_key = lua_tointeger(L, -2);
            size_t iov_len      = 0;
            char *iov_base      = (char *)lua_tolstring(L, -1, &iov_len);
            int idx             = 0;

            if ((lua_Integer)iov_len <= pos) {
                // this entry is fully covered by pos; skip it
                pos -= (lua_Integer)iov_len;
                lua_pop(L, 1);
                continue;
            } else if (wiov->iovcnt == wiov->iovmax) {
                // exceed system limit; fail with EINVAL
                lua_pushnil(L);
                lua_errno_new(L, EINVAL, "write.iovcnt");
                return 2;
            }
            iov_len -= (size_t)pos;
            iov_base += (size_t)pos;
            pos = 0;
            len += iov_len;

            // insert into sorted position to keep iov ordered by key
            for (idx = wiov->iovcnt - 1;
                 idx >= 0 && wiov->idx2key[idx] > new_key; idx--) {
                wiov->idx2key[idx + 1] = wiov->idx2key[idx];
                wiov->iov[idx + 1]     = wiov->iov[idx];
            }
            wiov->idx2key[idx + 1] = new_key;
            wiov->iov[idx + 1]     = (struct iovec){
                .iov_base = iov_base,
                .iov_len  = iov_len,
            };
            wiov->iovcnt++;
            break;
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
        // empty table (or all entries consumed by pos): zero-byte write to
        // validate the fd
        n = (offset >= 0) ? pwrite(fd, &empty, 0, offset) :
                            write(fd, &empty, 0);
        if (n < 0) {
            lua_pushnil(L);
            lua_errno_new(L, errno, "write");
            return 2;
        }
        lua_pushinteger(L, 0);
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushinteger(L, 0);
        return 4;
    }

    // iov is already trimmed by pos; len is the effective byte count
    n = (offset >= 0) ? pwritev(fd, wiov->iov, wiov->iovcnt, offset) :
                        writev(fd, wiov->iov, wiov->iovcnt);
    return push_result(L, fp, fd, len, n);
}

static int write_lua(lua_State *L)
{
    write_iov_t *wiov = lua_touserdata(L, lua_upvalueindex(1));
    int fd            = -1;
    FILE *fp          = NULL;
    off_t offset      = 0;
    lua_Integer pos   = 0;
    size_t len        = 0;
    const char *str   = NULL;
    ssize_t n         = 0;

    if (lauxh_isint(L, 1)) {
        fd = lua_tointeger(L, 1);
    } else if (!(fp = lauxh_checkfile(L, 1))) {
        // file has been closed; report EBADF immediately
        lua_pushnil(L);
        lua_errno_new(L, EBADF, "write");
        return 2;
    } else if (fflush(fp) != 0 && errno != EBADF) {
        // NOTE: ignore EBADF which means the FILE* is not opened for
        // writing
        /* LCOV_EXCL_START - fflush non-EBADF failure requires mocking */
        lua_pushnil(L);
        lua_errno_new(L, errno, "write.fflush");
        return 2;
        /* LCOV_EXCL_STOP */
    } else {
        fd = fileno(fp);
    }

    // get pos: source-byte position to start writing from (default 0)
    pos    = lauxh_optuinteger(L, 3, 0);
    // get offset: -1 for current fd position, >=0 for pwrite
    offset = lauxh_optinteger(L, 4, -1);

    if (!lauxh_isstring(L, 2)) {
        // table of strings; use writev(2)
        wiov->iovcnt = 0;
        return writev_lua(L, fd, fp, offset, wiov, pos);
    }
    lua_settop(L, 2);

    // string argument; use write(2)
    str = lua_tolstring(L, 2, &len);

    // apply pos: skip bytes already written by the caller.
    // for empty string (len==0), fall through to call write(2) so that an
    // invalid fd is still detected (same behaviour as before pos was added).
    if (len > 0 && (size_t)pos >= len) {
        lua_pushinteger(L, 0);
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushinteger(L, 0);
        return 4;
    }
    str += (size_t)pos;
    len -= (size_t)pos;

    n = (offset >= 0) ? pwrite(fd, str, len, offset) : write(fd, str, len);
    return push_result(L, fp, fd, len, n);
}

LUALIB_API int luaopen_io_write(lua_State *L)
{
    long iovmax = sysconf(_SC_IOV_MAX);

    if (iovmax < 0 || iovmax > IOV_MAX) {
        // fallback to a reasonable default if sysconf fails or returns an
        // unexpected value; this should not happen on compliant systems
        /* LCOV_EXCL_START - sysconf failure is not reproducible in tests */
        iovmax = IOV_MAX;
        /* LCOV_EXCL_STOP */
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
