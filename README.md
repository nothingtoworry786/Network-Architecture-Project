# HTTP, in binary — two tracks, one protocol

A tiny file server and client that speak **BH/1**, a custom binary HTTP-like
protocol over TCP. It is *not* HTTP/1.1 or HTTP/2: every message is a binary frame
with an 8-byte header, and one specification ([SPEC.md](SPEC.md)) is shared by both
programs.

| Program   | Role   | Usage |
|-----------|--------|-------|
| `observe` | server | `./observe <document_root> <port>` |
| `bcurl`   | client | `./bcurl [-v] host:port/path [/path ...]` |

C11, POSIX sockets, no external libraries, no threads.

## Build

Requires Linux (or any POSIX system) with `gcc` and `make`.

```
make            # builds ./observe and ./bcurl with -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
make test       # builds and runs every test
make debug      # rebuild with -O0 -g3
make asan       # rebuild + run all tests under AddressSanitizer
make ubsan      # rebuild + run all tests under UndefinedBehaviorSanitizer
make clean
```

On Windows, use WSL or Docker:

```
docker run --rm -v "${PWD}:/src:ro" gcc:14 sh -c "cp -r /src /work && cd /work && make test"
```

## Run

Terminal 1 — start the server:

```
$ ./observe ./www 9000
observe: serving ./www, listening on port 9000
```

Terminal 2 — fetch a file (only the body goes to stdout):

```
$ ./bcurl localhost:9000/hello.txt
Hello, binary world!
$ echo $?
0

$ ./bcurl localhost:9000/index.html > copy.html      # binary-safe: bytes are written as-is

$ ./bcurl localhost:9000/missing.txt
bcurl: /missing.txt: status 404
404 Not Found
$ echo $?
1
```

Several paths in one run go over **the same TCP connection**, one after another:

```
$ ./bcurl localhost:9000/index.html /hello.txt /missing.txt
```

and the server log (stderr) shows a single connection serving all three:

```
observe: [conn 1] accepted 127.0.0.1:35722
observe: [conn 1] #1 GET /index.html -> 200
observe: [conn 1] #2 GET /hello.txt -> 200
observe: [conn 1] #3 GET /missing.txt -> 404
observe: [conn 1] closed after 3 request(s)
```

Stop the server with Ctrl-C (it finishes the current connection and exits 0).

### Client exit status

| Status | Meaning |
|-------:|---------|
| 0 | every response was 2xx |
| 1 | at least one response was not 2xx (4xx, 5xx, …) |
| 2 | usage, network or protocol error (e.g. connection refused, malformed response) |

### Verbose mode

`-v` prints an annotated hexdump of every frame sent and received to **stderr**
(stdout still carries only the body). Each row is: offset, the actual wire bytes,
their ASCII, and the protocol field they belong to.

```
$ ./bcurl -v localhost:9000/hello.txt
== request (client -> server): REQUEST frame, type 0x01, flags 0x00, payload 43 bytes, 51 bytes on the wire ==
0000  42 48                                            |BH              |  magic "BH"
0002  01                                               |.               |  version = 1
0003  01                                               |.               |  type = 1
0004  00                                               |.               |  flags = 0
0005  00 00 2b                                         |..+             |  payload length = 43
0008  01                                               |.               |  method = 1
0009  00 0a                                            |..              |  path length = 10
000b  2f 68 65 6c 6c 6f 2e 74 78 74                    |/hello.txt      |  path
0015  02                                               |.               |  header count = 2
0016  03                                               |.               |  header id = 3
0017  00 0e                                            |..              |  value length = 14
0019  6c 6f 63 61 6c 68 6f 73 74 3a 39 30 30 30        |localhost:9000  |  value (Host)
0027  05                                               |.               |  header id = 5
0028  00 09                                            |..              |  value length = 9
002a  62 63 75 72 6c 2f 31 2e 30                       |bcurl/1.0       |  value (User-Agent)

== response (server -> client): RESPONSE frame, type 0x02, flags 0x00, payload 76 bytes, 84 bytes on the wire ==
0000  42 48                                            |BH              |  magic "BH"
0002  01                                               |.               |  version = 1
0003  02                                               |.               |  type = 2
0004  00                                               |.               |  flags = 0
0005  00 00 4c                                         |..L             |  payload length = 76
0008  00 c8                                            |..              |  status = 200
000a  03                                               |.               |  header count = 3
000b  01                                               |.               |  header id = 1
000c  00 0a                                            |..              |  value length = 10
000e  74 65 78 74 2f 70 6c 61 69 6e                    |text/plain      |  value (Content-Type)
0018  06                                               |.               |  header id = 6
0019  00 0b                                            |..              |  value length = 11
001b  6f 62 73 65 72 76 65 2f 31 2e 30                 |observe/1.0     |  value (Server)
0026  00                                               |.               |  header id = 0
0027  10                                               |.               |  name length = 16
0028  58 2d 52 65 71 75 65 73 74 2d 4e 75 6d 62 65 72  |X-Request-Number|  custom header name
0038  00 01                                            |..              |  value length = 1
003a  31                                               |1               |  value (custom header)
003b  00 00 00 15                                      |....            |  body length = 21
003f  48 65 6c 6c 6f 2c 20 62 69 6e 61 72 79 20 77 6f  |Hello, binary wo|  body
004f  72 6c 64 21 0a                                   |rld!.           |

Hello, binary world!
```

The dump is produced by running the real decoder with a per-field trace, so it
always shows what the implementation actually does with the bytes.

## Protocol summary

Full details, rationale and byte-by-byte examples are in [SPEC.md](SPEC.md).

```
 0               1               2               3
+---------------+---------------+---------------+---------------+
|         Magic "BH"            |  Version = 1  |     Type      |
+---------------+---------------+---------------+---------------+
|     Flags     |          Payload length (24 bits)             |
+---------------+-----------------------------------------------+
|                       Payload ...                             |
```

* **8-byte header**, all integers big-endian, no padding.
* **Types:** `0x01` REQUEST, `0x02` RESPONSE. Any other type is *skipped* using
  the length field — the connection is not dropped.
* **REQUEST payload:** `method u8` (1 = GET, the only method) · `path_len u16` ·
  path bytes · headers.
* **RESPONSE payload:** `status u16` · headers · `body_len u32` · raw body bytes.
* **Headers:** `count u8`, then per header `id u8` · (`name_len u8` · name, only
  if id = 0) · `value_len u16` · value. IDs 1–10 are predefined names; ID 0 is the
  escape for custom names.

  | ID | Name | ID | Name |
  |---:|------|---:|------|
  | 1 | Content-Type | 6 | Server |
  | 2 | Content-Length | 7 | Date |
  | 3 | Host | 8 | Cache-Control |
  | 4 | Accept | 9 | Content-Encoding |
  | 5 | User-Agent | 10 | Last-Modified |

* **Status codes:** 200, 400 (malformed request or forbidden path), 404, 500.
* **Persistent connection:** request, response, request, response, … on one TCP
  connection until the client closes it.
* **Limits:** path ≤ 1024 bytes, ≤ 32 headers, header name ≤ 64, header value ≤
  1024, request payload ≤ 65,535, body ≤ 16,711,680 bytes.
* **Errors:** a malformed or oversized request gets `400` and the connection
  *stays usable*; only a bad magic/version (frame boundary lost) closes it.

## Tests

```
make test
```

runs three suites and prints `ALL TESTS PASSED` at the end:

| Suite | What it is |
|-------|------------|
| `tests/test_protocol` | Unit tests for the codec and the stream helpers, over socketpairs. |
| `tests/test_server.sh` | Starts `./observe` and drives it with hand-built frames (`tests/test_conn.c`). |
| `tests/test_client.sh` | Runs the real `./bcurl` against `./observe` and checks stdout, stderr, exit status. |

The integration tests listen on port 19471; override with `PORT=12345 make test`.

Where each required behaviour is tested:

| # | Behaviour | Test |
|--:|-----------|------|
| 1 | GET existing file → 200 | `test_conn main`, `test_client.sh` |
| 2 | GET missing file → 404 | `test_conn main`, `test_client.sh` |
| 3 | Malformed request → 400 | `test_conn main` (lying path length, empty payload, bad method, stray byte) |
| 4 | Multiple requests on one connection | `test_conn main` (`/index.html`, `/hello.txt`, `/missing.txt` → 200, 200, 404, plus ~30 more exchanges on the same socket) |
| 5 | Binary response body | 1 MiB of random bytes, compared byte-for-byte (both scripts); NUL bytes in `test_response_roundtrip` |
| 6 | Empty response body | `/empty.txt` (both scripts), `test_response_roundtrip` |
| 7 | Long path within limits | a real file at a 1024-byte path in `test_conn main`; `test_limits` |
| 8 | Oversized frame rejected | `test_conn main` (65,536-byte request → 400, still in sync); `test_bad_frames_on_a_stream` |
| 9 | Invalid magic rejected | `test_conn desync`, `test_malformed_frames`, `test_bad_frames_on_a_stream` |
| 10 | Invalid version rejected | same three |
| 11 | Invalid lengths rejected | `test_malformed_frames` (every truncation, trailing byte, body-length mismatch), `test_limits` |
| 12 | Unknown frame type skipped | `test_unknown_frame_type_is_skipped`; `test_conn main` + server log check |
| 13 | Partial network reads | `test_partial_reads` (1 byte per send); `test_conn main` (request dribbled byte by byte over TCP) |
| 14 | Partial network writes | `test_partial_writes_and_eintr` (4 MiB through a slow reader under a 500 µs signal storm: short writes *and* EINTR) |
| 15 | Path traversal rejected | `test_conn main` (`/../`, `/a/../../`, `/..`, `..\`, no leading `/`, embedded NUL, symlink escape, `%2e%2e`); `test_client.sh` |
| 16 | Client non-zero on 4xx | `test_client.sh` |
| 17 | Client non-zero on 5xx | `test_client.sh` (a 17 MiB file cannot fit in one frame → 500) |
| 18 | `-v` shows request and response dumps | `test_client.sh` |
| 19 | No second TCP connection | both scripts count `accepted` lines in the server log; the server also numbers responses per connection (`X-Request-Number`) and the test client checks the sequence |

Additionally: `test_spec_example_bytes` compares the encoder's output with the
exact bytes printed in SPEC.md section 15, and the client is run against a fake
server that answers with non-BH/1 garbage.

## Layout

```
.
├── Makefile
├── README.md
├── SPEC.md              the protocol specification
├── include/
│   ├── protocol.h       constants, limits, encode/decode API
│   ├── net.h            read_full / write_full / recv_frame
│   └── hexdump.h
├── src/
│   ├── protocol.c       wire format: explicit big-endian encode/decode, no sockets
│   ├── net.c            partial reads/writes, EINTR, unknown-frame skipping
│   ├── hexdump.c        annotated dump (driven by the real decoder)
│   ├── observe.c        server
│   └── bcurl.c          client
├── tests/
│   ├── test_protocol.c  unit tests
│   ├── test_conn.c      raw-frame test client
│   ├── testutil.h
│   ├── lib.sh           shared fixture for the two scripts
│   ├── test_server.sh
│   └── test_client.sh
└── www/
    ├── index.html
    └── hello.txt
```

## Assumptions and known limits

* The server handles **one connection at a time** (no threads, as the brief asks);
  a second client waits in the listen backlog. An idle connection is dropped after
  30 seconds so it cannot block the server forever.
* One message is one frame, so a file larger than 16,711,680 bytes cannot be served
  and gets `500`.
* IPv4 only. There is no TLS, no authentication, no caching, and GET is the only
  method.
* Paths are raw bytes with no percent-decoding; `/` does not map to `index.html`.
