local testcase = require('testcase')
local assert = require('assert')
local fileno = require('io.fileno')
local pipe = require('os.pipe')
local write = require('io.write')

-- fill a non-blocking write-end until write(2) returns EAGAIN
local function fill_pipe(wfd)
    local n, err, again
    repeat
        n, err, again = write(wfd, string.rep('x', 4096))
    until (n == 0 and again) or err
    assert.is_nil(err)
end

-- DRAIN must exceed PIPE_BUF on every supported platform so that writes of
-- DRAIN*2 bytes to a pipe with exactly DRAIN bytes free produce a partial
-- write rather than EAGAIN.  Linux PIPE_BUF=4096, macOS PIPE_BUF=512.
-- DRAIN must also be a multiple of PAGE_SIZE (4096) because the Linux kernel
-- only writes to a pipe in page-aligned increments.
local DRAIN = 8192

function testcase.write_string()
    local f = assert(io.tmpfile())

    -- test that writing a string returns the byte count
    local n, err, again = write(f, 'hello world')
    assert.equal(n, 11)
    assert.is_nil(err)
    assert.is_nil(again)

    -- test that the content was written correctly
    f:seek('set')
    assert.equal(f:read('*a'), 'hello world')

    -- test that writing to a closed file returns EBADF
    f:close()
    n, err, again = write(f, 'x')
    assert.is_nil(n)
    assert.is_nil(again)
    assert.match(err, 'EBADF')
end

function testcase.write_string_to_fd()
    local f = assert(io.tmpfile())
    local fd = fileno(f)

    -- test that writing via a raw integer fd returns the byte count
    local n, err, again = write(fd, 'hello')
    assert.equal(n, 5)
    assert.is_nil(err)
    assert.is_nil(again)

    -- test that the content was written correctly
    f:seek('set')
    assert.equal(f:read('*a'), 'hello')

    -- test that writing to an invalid fd returns EBADF
    n, err, again = write(99999, 'x')
    assert.is_nil(n)
    assert.is_nil(again)
    assert.match(err, 'EBADF')

    f:close()
end

function testcase.write_string_with_offset()
    local f = assert(io.tmpfile())
    f:write('hello world')

    -- test that pwrite writes at the given offset without moving FILE* position
    f:seek('set')
    local n, err, again = write(f, 'WORLD', 6)
    assert.equal(n, 5)
    assert.is_nil(err)
    assert.is_nil(again)

    -- test that FILE* position is unchanged after pwrite
    assert.equal(f:seek(), 0)

    -- test that the content was patched at the correct offset
    assert.equal(f:read('*a'), 'hello WORLD')

    f:close()
end

function testcase.write_empty_string()
    local f = assert(io.tmpfile())

    -- test that writing an empty string returns 0 with no error and no again
    local n, err, again = write(f, '')
    assert.equal(n, 0)
    assert.is_nil(err)
    assert.is_nil(again)

    f:close()
end

function testcase.write_interleaved()
    local f = assert(io.tmpfile())

    -- test that f:write() then write() then f:write() produces correct content,
    -- confirming that sync_fp keeps the FILE* cursor consistent
    f:write('foo')
    local n, err = write(f, 'bar')
    assert.equal(n, 3)
    assert.is_nil(err)
    f:write('baz')

    f:seek('set')
    assert.equal(f:read('*a'), 'foobarbaz')

    -- test that FILE* position is correct after the sequence
    assert.equal(f:seek(), 9)

    f:close()
end

function testcase.write_table()
    local f = assert(io.tmpfile())

    -- test that writing a table of strings concatenates them correctly and
    -- returns no remaining table on a full write
    local n, err, again, remaining = write(f, {
        'hello',
        ' ',
        'world',
    })
    assert.equal(n, 11)
    assert.is_nil(err)
    assert.is_nil(again)
    assert.is_nil(remaining)

    f:seek('set')
    assert.equal(f:read('*a'), 'hello world')

    f:close()
end

function testcase.write_sparse_table()
    local f = assert(io.tmpfile())

    -- test that sparse tables (gaps in integer keys) skip the missing entries
    local n, err, again = write(f, {
        [1] = 'a',
        [3] = 'b',
    })
    assert.equal(n, 2)
    assert.is_nil(err)
    assert.is_nil(again)

    f:seek('set')
    assert.equal(f:read('*a'), 'ab')

    f:close()
end

function testcase.write_empty_table()
    local f = assert(io.tmpfile())

    -- test that an empty table returns 0 with no error
    local n, err, again = write(f, {})
    assert.equal(n, 0)
    assert.is_nil(err)
    assert.is_nil(again)

    f:close()
end

function testcase.write_table_with_offset()
    local f = assert(io.tmpfile())
    f:write('hello world')

    -- test that pwritev writes a table at the given offset without moving FILE* position
    f:seek('set')
    local n, err, again = write(f, {
        'W',
        'O',
        'R',
        'L',
        'D',
    }, 6)
    assert.equal(n, 5)
    assert.is_nil(err)
    assert.is_nil(again)

    -- test that FILE* position is unchanged after pwritev
    assert.equal(f:seek(), 0)

    -- test that the content was patched correctly
    assert.equal(f:read('*a'), 'hello WORLD')

    f:close()
end

function testcase.write_table_invalid_type_errors()
    local f = assert(io.tmpfile())

    -- test that a non-string, non-nil value in a table raises an argument error
    local ok, err = pcall(write, f, {
        'a',
        123,
    })
    assert.is_false(ok)
    assert.match(err, 'table#2')

    f:close()
end

function testcase.write_table_non_positive_key_skipped()
    local f = assert(io.tmpfile())

    -- test that a non-positive integer key (0) is skipped; only key 1 is written
    local n, err, again = write(f, {
        [0] = 'skip',
        [1] = 'hello',
    })
    assert.equal(n, 5)
    assert.is_nil(err)
    assert.is_nil(again)

    f:seek('set')
    assert.equal(f:read('*a'), 'hello')

    f:close()
end

function testcase.write_table_exceeds_iov_max()
    local f = assert(io.tmpfile())

    -- test that a table with more than IOV_MAX (1024) non-empty entries returns EINVAL
    local tbl = {}
    for i = 1, 1025 do
        tbl[i] = 'x'
    end
    local n, err = write(f, tbl)
    assert.is_nil(n)
    assert.match(err, 'EINVAL')

    f:close()
end

function testcase.write_empty_strings_invalid_fd()
    -- test that a table of only empty strings on an invalid fd returns EBADF:
    -- all entries have iov_len==0 so iovcnt stays 0, triggering the zero-byte
    -- write path; the invalid fd makes write(2) fail
    local n, err = write(99999, {
        '',
        '',
    })
    assert.is_nil(n)
    assert.match(err, 'EBADF')
end

-- A write exceeding PIPE_BUF bytes to a non-blocking pipe with exactly DRAIN
-- bytes free produces a partial write; the remainder is indicated by again=true.
function testcase.write_string_nonblock()
    local r, w = pipe(true)
    local wfd = w:fd()
    fill_pipe(wfd)

    -- test that write returns EAGAIN when the pipe is full
    local n, err, again = write(wfd, 'x')
    assert.equal(n, 0)
    assert.is_nil(err)
    assert.is_true(again)

    -- drain DRAIN bytes (> PIPE_BUF) to create partial-write space
    assert.equal(#r:read(DRAIN), DRAIN)

    -- write DRAIN*2 bytes with only DRAIN bytes free → partial write
    n, err, again = write(wfd, string.rep('a', DRAIN * 2))
    assert.equal(n, DRAIN)
    assert.is_nil(err)
    assert.is_true(again)

    r:close()
    w:close()
end

-- writev with a total write exceeding PIPE_BUF bytes on a non-blocking pipe
-- with limited free space produces partial writes or EAGAIN.
function testcase.write_table_nonblock()
    local r, w = pipe(true)
    local wfd = w:fd()
    fill_pipe(wfd)

    -- test that EAGAIN returns the original table as the 4th value
    local n, err, again, remaining = write(wfd, {
        'foo',
        'bar',
    })
    assert.equal(n, 0)
    assert.is_nil(err)
    assert.is_true(again)
    assert.is_table(remaining)
    assert.equal(remaining[1], 'foo')
    assert.equal(remaining[2], 'bar')

    -- drain DRAIN bytes (> PIPE_BUF); test partial write where the first
    -- entry fits exactly and the second entry has no room: only second returned
    assert.equal(#r:read(DRAIN), DRAIN)

    n, err, again, remaining = write(wfd, {
        string.rep('a', DRAIN),
        'rest',
    })
    assert.equal(n, DRAIN)
    assert.is_nil(err)
    assert.is_true(again)
    assert.is_table(remaining)
    assert.equal(remaining[1], 'rest')
    assert.is_nil(remaining[2])

    -- pipe is full again; drain DRAIN bytes and test partial write where the
    -- first entry fits, the second entry is partially written (DRAIN-512 bytes
    -- fit out of DRAIN), and the third entry is not written at all.
    -- entry1(512) + partial entry2(DRAIN-512) = DRAIN bytes written;
    -- remaining: tail of entry2 = rep('b', 512), all of entry3 = rep('c', 100)
    assert.equal(#r:read(DRAIN), DRAIN)

    n, err, again, remaining = write(wfd, {
        string.rep('a', 512),
        string.rep('b', DRAIN),
        string.rep('c', 100),
    })
    assert.equal(n, DRAIN)
    assert.is_nil(err)
    assert.is_true(again)
    assert.is_table(remaining)
    assert.equal(remaining[1], string.rep('b', 512))
    assert.equal(remaining[2], string.rep('c', 100))
    assert.is_nil(remaining[3])

    r:close()
    w:close()
end
