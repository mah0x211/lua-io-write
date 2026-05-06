# lua-io-write

[![test](https://github.com/mah0x211/lua-io-write/actions/workflows/test.yml/badge.svg)](https://github.com/mah0x211/lua-io-write/actions/workflows/test.yml)
[![codecov](https://codecov.io/gh/mah0x211/lua-io-write/branch/master/graph/badge.svg)](https://codecov.io/gh/mah0x211/lua-io-write)

Non-blocking `write(2)` / `writev(2)` for Lua file handles and file descriptors.


## Installation

```
luarocks install io-write
```

---

## Error Handling

The following functions return the `error` object created by https://github.com/mah0x211/lua-errno module.


## n, err, again, remain = write( file, data [, offset [, pos]] )

Writes a string or a table of strings to a file handle or file descriptor.

- When `data` is a **string**, uses `write(2)` or `pwrite(2)` (if `offset` is given).
- When `data` is a **table**, uses `writev(2)` or `pwritev(2)` (if `offset` is given) for vectorized I/O. Only positive-integer keys with non-empty string values are written; `nil` values, empty strings, and non-positive-integer keys are ignored. If no entries are written (empty table or all entries consumed by `pos`), a zero-byte `write(2)` is issued to validate the fd. The number of writable entries is limited to the effective `IOV_MAX`, determined as follows:
  1. If `IOV_MAX` is not defined by the system headers at compile time, `_XOPEN_IOV_MAX` is used if available; otherwise it falls back to `16`.
  2. At module load time, `sysconf(_SC_IOV_MAX)` is called to get the runtime limit. If it fails or returns a value larger than the compile-time `IOV_MAX`, the compile-time `IOV_MAX` is used instead.

**NOTE:** If the non-blocking flag is not set on `file`, the write will block until completion.

**Parameters**

- `file:file*|integer`: a file handle or a raw file descriptor.
- `data:string|table`: a string, or a table of strings to be written. In a table, `nil` values, empty strings, and non-positive-integer keys are silently skipped.
- `offset:integer` *(optional)*: byte offset at which to write (`pwrite(2)` / `pwritev(2)`). The file-handle position is not changed. Omit (or pass `nil`) to write at the current position.
- `pos:integer` *(optional, default `0`)*: source-byte position within `data` to start writing from. Use this to resume a partial write without copying or reallocating `data`. Must be `>= 0`. If `pos >= total length of data`, returns `0, nil, nil, 0` without writing any data (an empty string always calls `write(2)` to validate the fd).

**Returns**

- `n:integer`: number of bytes written, or `nil` on error.
- `err:any`: error object, or `nil` on success.
- `again:boolean`: `true` if the write could not complete due to `EAGAIN`, `EWOULDBLOCK`, or `EINTR`. Retry after the fd becomes writable.
- `remain:integer`: number of bytes in `data` (starting at `pos`) that were **not** written. `0` on a full write; `> 0` together with `again == true` on a partial write. Always returned when `n` is not `nil`.

**Return value summary**

| Situation | Returns |
|---|---|
| Full write | `n, nil, nil, 0` |
| Partial write / EAGAIN | `n, nil, true, remain` |
| pos >= data length | `0, nil, nil, 0` |
| Error | `nil, err` |


## Usage

```lua
local write = require('io.write')

-- write a string to a tmpfile
local f = assert(io.tmpfile())
local n, err = write(f, 'hello world')
assert(n == 11 and err == nil)

-- write a table of strings
n, err = write(f, {'hello', ' ', 'world'})
assert(n == 11 and err == nil)

-- write at a specific byte offset (pwrite/pwritev); FILE* position is unchanged
f:seek('set')
n, err = write(f, 'WORLD', 6)  -- overwrites bytes 6-10
assert(n == 5 and f:seek() == 0)

-- non-blocking write with retry using pos (pipe example)
local pipe = require('os.pipe')
local r, w = pipe(true)  -- O_NONBLOCK
local data = string.rep('x', 65536)
local head = 0
repeat
    local again, remain
    n, err, again, remain = write(w:fd(), data, nil, head)
    assert(n, err)
    head = head + n
    -- remain == 0 when fully written
    if again then
        -- wait until the fd is writable (select/poll/epoll/kqueue ...)
    end
until remain == 0
assert(head == #data)
```

