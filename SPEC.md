# BH/1 — Binary HTTP-like Protocol, Version 1

**Status:** normative specification. An implementation written from this document alone
must interoperate with the reference server (`observe`) and client (`bcurl`).

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

---

## 1. Overview

BH/1 is a small request/response protocol for fetching files, carried over a single
TCP connection. It keeps the *ideas* of HTTP — a method, a path, headers, a status
code, a body — but none of its text syntax. Everything on the wire is a **frame**: an
8-byte fixed header followed by a payload whose length the header states.

```
client                                   server
  | ---- REQUEST  (GET /index.html) ----> |
  | <--- RESPONSE (200, headers, body) -- |
  | ---- REQUEST  (GET /hello.txt) -----> |      same TCP connection
  | <--- RESPONSE (200, headers, body) -- |
  | ---- REQUEST  (GET /missing.txt) ---> |
  | <--- RESPONSE (404, headers, body) -- |
```

BH/1 is **not** HTTP/1.1 and **not** HTTP/2 and is not compatible with either.

## 2. Design goals

1. **Unambiguous framing.** A receiver always knows where a frame ends before it
   looks at a single payload byte.
2. **Simplicity.** Every field is a whole number of bytes. There is one integer
   encoding and one byte-string encoding.
3. **Correctness under hostile input.** Every length is bounded and checked before
   memory is allocated; no field relies on a terminator.
4. **Forward compatibility.** Unknown frame types and unknown header identifiers can
   be stepped over without understanding them.
5. **Debuggability.** The header is 8 bytes — half a hexdump row — and starts with
   two ASCII letters, so frames are easy to find and read by eye.

## 3. Conventions

### 3.1 Integers and endianness

All multi-byte integers are **unsigned** and in **network byte order (big-endian)**:
the most significant byte is sent first. Four widths are used:

| Name  | Bytes | Range            |
|-------|-------|------------------|
| `u8`  | 1     | 0 – 255          |
| `u16` | 2     | 0 – 65,535       |
| `u24` | 3     | 0 – 16,777,215   |
| `u32` | 4     | 0 – 4,294,967,295|

Example: the `u24` value 43 is sent as `00 00 2b`; the `u16` value 200 as `00 c8`.

There is no padding and no alignment anywhere. Fields follow each other directly.

### 3.2 Byte strings

Every variable-length field is a **length-prefixed byte string**: an unsigned length
(the width is given where the field is defined) followed by exactly that many bytes.

* There is **no terminator**. A NUL byte (`0x00`) has no special meaning on the wire.
* Byte strings are opaque octets. BH/1 does not require or validate UTF-8 or any
  other character encoding. Comparison is byte-for-byte.
* By convention, paths and header values are ASCII/UTF-8 text, and bodies are
  arbitrary binary data.

### 3.3 Offsets

Offsets are in bytes from the first byte of the frame (offset 0 = first magic byte).
Payload offsets are given both relative to the payload and, in the examples, absolute.

## 4. Connection model and lifecycle

* BH/1 runs over **one TCP connection**, opened by the client to the server's port.
  There is no handshake and no greeting: the first bytes on the connection are the
  client's first frame.
* The connection is **persistent**. After a response the connection stays open and
  the client MAY send another request. A client MUST NOT open a new connection for
  each request, and a server MUST NOT close the connection merely because it has
  sent a response.
* Exchanges are strictly **sequential**: the server sends exactly one RESPONSE for
  each REQUEST, in the order the requests arrived. A client MAY send several
  requests without waiting (pipelining); responses still come back in request order.
  There are no request identifiers; the *n*-th response answers the *n*-th request.
* The **client** ends the conversation by closing the connection when it is not
  waiting for a response. The server treats end-of-stream on a frame boundary as a
  normal close.
* The **server** closes the connection only when:
  1. the client has closed it;
  2. the frame boundary has been lost (section 12, "fatal" errors);
  3. an I/O error or timeout occurs. A server MAY close a connection on which
     nothing has been received for an implementation-defined time (the reference
     server uses 30 seconds).
* TCP is a byte stream. A frame MAY arrive split across any number of segments, and
  several frames MAY arrive in one segment. Implementations MUST reassemble frames
  from the stream using the length field and MUST NOT assume that one read returns
  one frame or that one write sends a whole frame.

Lifecycle summary:

```
connect -> { REQUEST -> RESPONSE }*  -> client closes
```

## 5. Frame format

Every frame is a fixed 8-byte header followed by `Length` payload bytes.

### 5.1 Header diagram

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-------------------------------+---------------+---------------+
|       Magic (16) = "BH"       |  Version (8)  |   Type (8)    |
+---------------+---------------+---------------+---------------+
|   Flags (8)   |                  Length (24)                  |
+---------------+-----------------------------------------------+
|                  Payload (Length bytes) ...                   |
+---------------------------------------------------------------+
```

### 5.2 Header fields

| Offset | Width | Type  | Field   | Meaning |
|-------:|------:|-------|---------|---------|
| 0      | 2     | bytes | Magic   | The two bytes `0x42 0x48` (ASCII `"BH"`). Identifies the protocol. |
| 2      | 1     | `u8`  | Version | Protocol version. `0x01` for this specification. |
| 3      | 1     | `u8`  | Type    | Frame type (section 6). Tells the receiver how to interpret the payload. |
| 4      | 1     | `u8`  | Flags   | Reserved. No flags are defined in version 1. |
| 5      | 3     | `u24`, big-endian | Length | Number of payload bytes that follow the header. Does **not** include the 8 header bytes. |
| 8      | Length | bytes | Payload | Interpreted according to Type. |

The total size of a frame on the wire is therefore `8 + Length` bytes, and the next
frame starts at offset `8 + Length`.

### 5.3 Legal values and validation

A receiver MUST validate the header in this order, before using any other byte:

| Field   | Legal values | If violated |
|---------|--------------|-------------|
| Magic   | exactly `42 48` | **Fatal**: the stream is not BH, or synchronization is lost. |
| Version | exactly `01` | **Fatal**: the layout of other versions is unknown, so Length cannot be trusted. |
| Type    | any value `00`–`ff` | Never an error. Types the receiver does not process are skipped (6.3). |
| Flags   | `00` on REQUEST and RESPONSE frames | The frame is **rejected** but the stream stays in sync (section 12). Senders MUST send `00`. On skipped frame types Flags is ignored. |
| Length  | any value 0 – 16,777,215 | Never a *framing* error: every value locates the next frame. A receiver applies its own size limit (section 11); a frame over the limit is **rejected** but still consumed. |

`Length = 0` is legal at the framing level (an empty payload). Whether an empty
payload is valid depends on the frame type; it is not valid for REQUEST or RESPONSE.

The payload of a frame MUST be consumed exactly. The frame-type payload formats
(sections 8 and 9) are self-delimiting; if decoding them consumes fewer or more
bytes than `Length`, the frame is malformed.

## 6. Frame types

| Type value | Name     | Direction        | Payload |
|-----------:|----------|------------------|---------|
| `0x01`     | REQUEST  | client -> server | section 8 |
| `0x02`     | RESPONSE | server -> client | section 9 |
| `0x00`, `0x03`–`0xff` | (unassigned) | — | reserved for future use; skipped (6.3) |

### 6.1 REQUEST (`0x01`)

Sent by the client. Asks the server for one resource.

### 6.2 RESPONSE (`0x02`)

Sent by the server. Answers exactly one REQUEST.

### 6.3 Unknown frame types — skipping

A server processes only REQUEST frames; a client processes only RESPONSE frames.
When an endpoint receives a frame with a **valid header** (magic and version
correct) whose **Type it does not process**, it MUST:

1. read the Length field;
2. read and discard exactly Length payload bytes, without interpreting them and
   without needing to store them;
3. ignore the frame's Flags;
4. send nothing in reply;
5. continue by reading the next frame header.

It MUST NOT close the connection and MUST NOT report an error. This is what allows
a future revision to introduce new frame types that old implementations tolerate.

Example — a 3-byte frame of unassigned type `0x7f`, followed by a normal request.
The receiver reads `00 00 03`, discards `aa bb cc`, and is positioned exactly on
the next `42 48`:

```
42 48 01 7f 00 00 00 03  aa bb cc        <- skipped entirely
42 48 01 01 00 00 00 2b  01 00 0a ...    <- processed normally
```

Only the *type* may be unknown. A wrong magic or version is a fatal error
(section 12), because then the Length field itself cannot be trusted. A truncated
unknown frame (connection ends before Length bytes arrive) is an I/O error like any
other truncated frame.

In the reference implementation this is the loop in `recv_frame()` in `src/net.c`.

## 7. Header fields

Requests and responses carry a list of header fields (name/value pairs), as in HTTP.

### 7.1 Header block

```
+-----------+----------------+----------------+-----+
| Count u8  | Header field 1 | Header field 2 | ... |
+-----------+----------------+----------------+-----+
```

| Field | Width | Legal values | Meaning |
|-------|------:|--------------|---------|
| Count | 1 (`u8`) | 0 – 32 | Number of header fields that follow. |

Each header field starts with a one-byte identifier `ID`:

**Predefined name** (`ID` = 1–10) — the name costs one byte:

```
+-------+---------------+---------------------+
| ID u8 | ValueLen u16  | Value (ValueLen B)  |
+-------+---------------+---------------------+
```

**Custom name** (`ID` = 0, the escape) — the name is spelled out:

```
+---------+------------+-------------------+--------------+--------------------+
| ID = 00 | NameLen u8 | Name (NameLen B)  | ValueLen u16 | Value (ValueLen B) |
+---------+------------+-------------------+--------------+--------------------+
```

| Field    | Width | Byte order | Legal values | Meaning |
|----------|------:|------------|--------------|---------|
| ID       | 1     | —          | 0 – 255      | 0 = custom name follows; 1–10 = predefined name (7.2); 11–255 = unassigned (7.4). |
| NameLen  | 1     | —          | 1 – 64       | Present only when ID = 0. Length of Name in bytes. |
| Name     | NameLen | —        | any bytes    | Present only when ID = 0. The header name. |
| ValueLen | 2     | big-endian | 0 – 1024     | Length of Value in bytes. |
| Value    | ValueLen | —       | any bytes    | The header value. May be empty. |

Any value outside a legal range makes the whole frame malformed.

### 7.2 The ten predefined header names

| ID | Name             | Typical use |
|---:|------------------|-------------|
| 1  | Content-Type     | Media type of the body, e.g. `text/html`. |
| 2  | Content-Length   | Body size as ASCII decimal. Informational only (see below). |
| 3  | Host             | `host:port` the client connected to. |
| 4  | Accept           | Media types the client prefers. |
| 5  | User-Agent       | Client software name/version. |
| 6  | Server           | Server software name/version. |
| 7  | Date             | Time the response was generated. |
| 8  | Cache-Control    | Caching directives. |
| 9  | Content-Encoding | Encoding applied to the body. |
| 10 | Last-Modified    | Modification time of the resource. |

These are exactly ten; the table is fixed for version 1.

* Values are byte strings; by convention they hold the same ASCII text the
  equally-named HTTP header would.
* The body length is carried by the RESPONSE's `BodyLen` field (section 9), which
  is authoritative. `Content-Length` is optional and informational; receivers MUST
  NOT use it for framing.
* Senders SHOULD use the numeric ID rather than a custom name for these ten names.

### 7.3 Custom header names

Any name not in the table is sent with `ID = 0` and an explicit length-prefixed
name, e.g. `X-Request-Number: 1` is encoded as (21 bytes):

```
00                                               ID = 0 (custom)
10                                               NameLen = 16
58 2d 52 65 71 75 65 73 74 2d 4e 75 6d 62 65 72  "X-Request-Number"
00 01                                            ValueLen = 1
31                                               "1"
```

Custom names are case-sensitive byte strings. BH/1 assigns them no meaning.

### 7.4 Unassigned identifiers, order, repetition

* IDs 11–255 are **unassigned**. Senders MUST NOT use them in version 1.
  Receivers MUST NOT treat them as an error: the field is still
  `ID, ValueLen, Value`, so a receiver decodes it, checks `ValueLen ≤ 1024`, and
  ignores the field. (This is the header-level equivalent of 6.3.)
* Header order is preserved but has no meaning. A name MAY appear more than once;
  BH/1 assigns no meaning to repetition.
* A receiver MUST ignore header fields it does not use. The reference server does
  not interpret any request header.

## 8. REQUEST payload (Type `0x01`)

```
+-----------+-------------+---------------------+---------------------+
| Method u8 | PathLen u16 | Path (PathLen B)    | Header block (7.1)  |
+-----------+-------------+---------------------+---------------------+
```

| Payload offset | Width | Byte order | Field | Legal values | Meaning |
|---------------:|------:|------------|-------|--------------|---------|
| 0              | 1     | —          | Method  | `0x01` only | `0x01` = GET. It is the only method in version 1. |
| 1              | 2     | big-endian | PathLen | 1 – 1024    | Length of Path in bytes. |
| 3              | PathLen | —        | Path    | first byte MUST be `0x2f` (`/`) | The resource, as an absolute path. A byte string: not NUL-terminated. |
| 3 + PathLen    | 1 + … | —          | Header block | section 7 | Count, then Count header fields. |

Rules:

* **GET is the only method.** Any other Method value makes the request malformed.
  A GET request has no body.
* `Path` is used exactly as sent. There is **no percent-decoding**, no query string
  and no fragment: `/a%20b` names a file whose name contains the five characters
  `%20`. Segments are separated by `/` (`0x2f`).
* The payload MUST end exactly after the last header field. Extra bytes are an error.
* The smallest legal REQUEST payload is 5 bytes: `01 00 01 2f 00` (GET `/`, no headers).
* The largest legal REQUEST payload is
  `1 + 2 + 1024 + 1 + 32 × (1 + 1 + 64 + 2 + 1024) = 35,972` bytes.

## 9. RESPONSE payload (Type `0x02`)

```
+------------+---------------------+-------------+-------------------+
| Status u16 | Header block (7.1)  | BodyLen u32 | Body (BodyLen B)  |
+------------+---------------------+-------------+-------------------+
```

| Payload offset | Width | Byte order | Field | Legal values | Meaning |
|---------------:|------:|------------|-------|--------------|---------|
| 0              | 2     | big-endian | Status  | 100 – 599 | Status code (section 10). |
| 2              | 1 + … | —          | Header block | section 7 | Count, then Count header fields. |
| after headers  | 4     | big-endian | BodyLen | 0 – 16,711,680 | Length of Body in bytes. |
| after BodyLen  | BodyLen | —        | Body    | any bytes | The resource content, raw. |

Rules:

* `Body` is **raw binary data**. It may contain any byte value including `0x00`,
  and is never NUL-terminated. Receivers MUST take its length from `BodyLen`.
* `BodyLen` MUST equal the number of payload bytes remaining after the `BodyLen`
  field. If it claims more (body truncated) or fewer (trailing bytes), the frame
  is malformed. `BodyLen = 0` (empty body) is legal.
* The smallest legal RESPONSE payload is 7 bytes: `00 c8 00 00 00 00 00`
  (200, no headers, empty body).

## 10. Status codes

Status is a fixed-width `u16`. Codes have the same meaning as in HTTP; the first
digit gives the class (2xx success, 4xx client error, 5xx server error).

| Code | Wire bytes | Name | When the server sends it |
|-----:|-----------|------|--------------------------|
| 200  | `00 c8` | OK | The file exists and was read; the body is its content. |
| 400  | `01 90` | Bad Request | The request frame was malformed, violated a limit, or named a forbidden path (section 13). |
| 404  | `01 94` | Not Found | The request was valid but no such regular file exists under the document root. |
| 500  | `01 f4` | Internal Server Error | The request was valid and the file exists, but the server failed to deliver it (cannot open/read it, out of memory, or the file is larger than the maximum body size). |

A client MUST accept any code from 100 to 599 and interpret unknown codes by
their class. For non-200 responses the reference server sends a short
human-readable body (e.g. `404 Not Found\n`) with `Content-Type: text/plain`.

## 11. Limits

| Limit | Value | Applies to |
|-------|------:|-----------|
| Header size | 8 bytes | every frame |
| Maximum Length (any frame) | 16,777,215 (`0xFFFFFF`) | field width |
| Maximum REQUEST payload accepted by a server | 65,535 bytes | REQUEST |
| Maximum RESPONSE payload accepted by a client | 16,777,215 bytes | RESPONSE |
| Maximum path length | 1,024 bytes | REQUEST `PathLen` |
| Maximum header fields per message | 32 | `Count` |
| Maximum custom header-name length | 64 bytes (minimum 1) | `NameLen` |
| Maximum header-value length | 1,024 bytes | `ValueLen` |
| Maximum body length | 16,711,680 bytes (`0xFF0000`) | RESPONSE `BodyLen` |

* The 65,535-byte request limit is above the largest legal request (35,972
  bytes), so it never rejects a legal request; it lets a server refuse an absurd
  frame from the header alone, before allocating anything.
* The body limit leaves `16,777,215 − 16,711,680 = 65,535` payload bytes for
  Status, headers and BodyLen, more than the largest legal header block needs
  (`2 + 1 + 34,944 + 4 = 34,951`). A body of maximum size therefore always fits.
* Frames of unknown type may be any Length; they are discarded in a stream, never
  buffered, so they need no memory limit.
* Implementations MUST check every length against these limits **before**
  allocating memory or copying data, and MUST check that a length does not exceed
  the bytes remaining in the payload.

## 12. Error handling

The central question after any error is: *does the receiver still know where the
next frame starts?* If yes, the connection continues. If no, it must close.

### 12.1 Server

| Condition | Framing | Server MUST |
|-----------|---------|-------------|
| Wrong Magic | lost | send a `400` RESPONSE (best effort), then close the connection. **Fatal.** |
| Wrong Version | lost | same as wrong Magic. **Fatal.** |
| Type is not REQUEST | intact | skip the frame silently (6.3). No response. |
| REQUEST with Flags ≠ 0 | intact | consume the payload, send `400`, continue. |
| REQUEST with Length > 65,535 | intact | consume (discard) the payload without buffering it, send `400`, continue. |
| REQUEST payload malformed (any rule of sections 7–8: bad method, bad PathLen, path not starting with `/`, a limit exceeded, a length running past the payload, leftover bytes, empty payload) | intact | send `400`, continue. |
| Forbidden path (section 13) | intact | send `400`, continue. |
| End of stream exactly between frames | — | close quietly (normal end). |
| End of stream, timeout or I/O error inside a frame | — | close; no response. |

"Continue" means: keep the connection open and read the next frame header.
Every RESPONSE, including `400`, counts as the answer to one request in the
ordering of section 4.

### 12.2 Client

| Condition | Client MUST |
|-----------|-------------|
| Wrong Magic or Version | treat the response as malformed, close the connection, report failure. |
| Type is not RESPONSE | skip the frame silently (6.3) and keep waiting for the RESPONSE. |
| RESPONSE with Flags ≠ 0, or payload malformed (sections 7, 9) | report failure; MUST NOT use any part of the payload. |
| End of stream before a complete RESPONSE | report failure. |

A client MUST NOT retry by opening a second connection.

The reference client exits with status `0` if every response was 2xx, `1` if any
response had another status (4xx, 5xx, …), and `2` for usage, network or protocol
errors.

## 13. Security and path handling

All input from the network is untrusted. Beyond the limits of section 11:

**Mapping a path to a file.** The server is started with a *document root*
directory. For a well-formed REQUEST it proceeds as follows:

1. **Lexical check → `400`.** The request is refused if `Path` contains
   * a byte `0x00` (NUL), or
   * a byte `0x5c` (backslash `\`), or
   * a segment that is exactly `..` — where segments are the pieces of `Path`
     between `/` bytes. This covers `/../x`, `/a/../../x`, `/a/..`, and `/..\x`.
2. The candidate file name is the document root followed by `Path`.
3. **Resolution check → `404`.** The server resolves the candidate to its
   canonical absolute name, following symbolic links. If it cannot be resolved,
   or the result is not strictly inside the (canonical) document root — for
   instance a symlink inside the root pointing outside it — the answer is `404`.
4. **Type check → `404`.** If the result is not a regular file (a directory, FIFO,
   device, …), the answer is `404`. There is no directory listing and no implicit
   `index.html`.
5. **Delivery → `200` or `500`.** If the file cannot be opened or read, or is
   larger than the maximum body length, the answer is `500`. Otherwise `200` with
   the file's bytes as the body.

**Encoded traversal.** BH/1 has no percent-encoding or any other escaping in
paths, so there is no decoding step that could turn `%2e%2e` into `..` after the
check. `/%2e%2e/x` simply names a directory literally called `%2e%2e` (normally
`404`).

**Other requirements.**

* Lengths are validated before allocation; the largest allocation a peer can
  cause is one frame within the limits of section 11.
* No wire field is treated as a C string. Implementations MUST NOT depend on NUL
  termination of paths, names, values or bodies.
* Integer arithmetic on lengths MUST NOT overflow; all wire lengths fit in 32 bits.
* Reserved fields (Flags) MUST be checked, not ignored, on processed frame types.

## 14. Versioning and compatibility

* The first three bytes of every frame — Magic and Version — are the
  compatibility contract. Magic never changes.
* **Compatible extensions** stay at Version `01` and need no negotiation:
  * new **frame types** (old receivers skip them, 6.3);
  * new **header IDs** in the range 11–255 (old receivers ignore them, 7.4);
  * new **status codes** (old clients fall back to the class digit);
  * new **custom headers**.
* **Incompatible changes** — a different header layout, a new meaning for a Flags
  bit on an existing frame type, a change to the REQUEST or RESPONSE payload
  layout, a new method — MUST use a new Version value. A version-1 endpoint
  rejects such frames as a fatal error instead of misreading them.
* Flags bits are reserved for a future version; because a version-1 receiver
  rejects non-zero Flags on frames it processes, a flag can never be silently
  misinterpreted.

## 15. Worked example

The exchange produced by

```
./observe ./www 9000
./bcurl -v localhost:9000/hello.txt
```

where `www/hello.txt` contains the 21 bytes `Hello, binary world!\n`.
Logical content:

```
request:   GET /hello.txt
           Host: localhost:9000
           User-Agent: bcurl/1.0

response:  200
           Content-Type: text/plain
           Server: observe/1.0
           X-Request-Number: 1          (custom header: 1st request on this connection)
           <21 bytes of body>
```

The dumps in 15.1 and 15.2 are the verbatim output of `bcurl -v`. Columns:
offset (hex) · wire bytes (hex) · the same bytes as ASCII (`.` = non-printable) ·
field, with integer fields decoded to decimal.

### 15.1 Complete REQUEST frame (51 bytes)

```
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
```

Byte by byte:

| Offset (dec) | Offset (hex) | Bytes | Field | Value |
|---:|---:|---|---|---|
| 0–1   | 00–01 | `42 48` | Magic | `"BH"` |
| 2     | 02    | `01` | Version | 1 |
| 3     | 03    | `01` | Type | REQUEST |
| 4     | 04    | `00` | Flags | none |
| 5–7   | 05–07 | `00 00 2b` | Length | 43 payload bytes (frame = 8 + 43 = 51) |
| 8     | 08    | `01` | Method | GET |
| 9–10  | 09–0a | `00 0a` | PathLen | 10 |
| 11–20 | 0b–14 | `2f 68 65 6c 6c 6f 2e 74 78 74` | Path | `/hello.txt` |
| 21    | 15    | `02` | Count | 2 header fields |
| 22    | 16    | `03` | ID | 3 = Host (predefined: name costs 1 byte) |
| 23–24 | 17–18 | `00 0e` | ValueLen | 14 |
| 25–38 | 19–26 | `6c 6f 63 61 6c 68 6f 73 74 3a 39 30 30 30` | Value | `localhost:9000` |
| 39    | 27    | `05` | ID | 5 = User-Agent |
| 40–41 | 28–29 | `00 09` | ValueLen | 9 |
| 42–50 | 2a–32 | `62 63 75 72 6c 2f 31 2e 30` | Value | `bcurl/1.0` |

Check: payload = 1 + 2 + 10 + 1 + (1 + 2 + 14) + (1 + 2 + 9) = 43 = `0x2b`. ✔

### 15.2 Complete RESPONSE frame (84 bytes)

```
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
```

Byte by byte:

| Offset (dec) | Offset (hex) | Bytes | Field | Value |
|---:|---:|---|---|---|
| 0–1   | 00–01 | `42 48` | Magic | `"BH"` |
| 2     | 02    | `01` | Version | 1 |
| 3     | 03    | `02` | Type | RESPONSE |
| 4     | 04    | `00` | Flags | none |
| 5–7   | 05–07 | `00 00 4c` | Length | 76 payload bytes (frame = 8 + 76 = 84) |
| 8–9   | 08–09 | `00 c8` | Status | 200 |
| 10    | 0a    | `03` | Count | 3 header fields |
| 11    | 0b    | `01` | ID | 1 = Content-Type |
| 12–13 | 0c–0d | `00 0a` | ValueLen | 10 |
| 14–23 | 0e–17 | `74 65 78 74 2f 70 6c 61 69 6e` | Value | `text/plain` |
| 24    | 18    | `06` | ID | 6 = Server |
| 25–26 | 19–1a | `00 0b` | ValueLen | 11 |
| 27–37 | 1b–25 | `6f 62 73 65 72 76 65 2f 31 2e 30` | Value | `observe/1.0` |
| 38    | 26    | `00` | ID | 0 = custom name follows (the escape) |
| 39    | 27    | `10` | NameLen | 16 |
| 40–55 | 28–37 | `58 2d 52 65 71 75 65 73 74 2d 4e 75 6d 62 65 72` | Name | `X-Request-Number` |
| 56–57 | 38–39 | `00 01` | ValueLen | 1 |
| 58    | 3a    | `31` | Value | `1` |
| 59–62 | 3b–3e | `00 00 00 15` | BodyLen | 21 |
| 63–83 | 3f–53 | `48 65 6c 6c 6f 2c 20 62 69 6e 61 72 79 20 77 6f 72 6c 64 21 0a` | Body | `Hello, binary world!\n` |

Check: payload = 2 + 1 + (1 + 2 + 10) + (1 + 2 + 11) + (1 + 1 + 16 + 2 + 1) + 4 + 21
= 76 = `0x4c`. ✔ The response shows both header encodings side by side:
`Content-Type` costs 1 byte of name, `X-Request-Number` costs 1 + 1 + 16.

### 15.3 Reproducing the dumps

```
make
./observe ./www 9000 &
./bcurl -v localhost:9000/hello.txt
```

The dump is written to stderr, the body to stdout. The dump is generated by
running the real decoder with a per-field trace (`src/hexdump.c`), not by a
separate pretty-printer. The same 51 and 84 bytes are also hard-coded in
`tests/test_protocol.c` (`test_spec_example_bytes`) and compared against the
encoder's output on every `make test`, so this section cannot drift from the code.

### 15.4 Example error response

A request for a file that does not exist (`GET /nope`) is answered on the same
connection with (payload, 69 bytes): `01 94` (404), the same three headers, then
`00 00 00 0e` and the 14-byte body `404 Not Found\n`.

## 16. Design rationale

**"HTTP/2 chose 24 / 8 / 8 / 31. Why?"** HTTP/2's 9-byte header is Length(24),
Type(8), Flags(8), Reserved(1) + Stream ID(31). BH/1 chose **16 / 8 / 8 / 8 / 24**:

| Field | Bits | Why this width |
|-------|-----:|----------------|
| Magic | 16 | HTTP/2 has no per-frame magic (it has a one-off connection preface). BH/1 spends two bytes per frame so that every frame is self-identifying in a hexdump (`BH` in the ASCII column) and so that loss of synchronization, or a non-BH peer such as an HTTP/1.1 client, is detected at the very next header instead of being misread as a length. Two ASCII letters are enough for that; four would make the header 10 bytes. |
| Version | 8 | One byte gives 255 future versions and sits in the first three bytes, which never move. HTTP/2 negotiates its version outside the frame (ALPN); BH/1 has no negotiation, so the version must be in-band. |
| Type | 8 | Same as HTTP/2. Two of 256 values are used; a whole byte keeps the field byte-aligned and leaves ample room for skippable extensions. |
| Flags | 8 | Same as HTTP/2. None are needed yet, but reserving the byte now means a later version can add per-frame options without changing the header size. |
| Length | 24 | Same as HTTP/2. 16 bits (64 KiB) would be too small for ordinary files; 32 bits would invite 4 GiB frames that no receiver should buffer and would push the header to 9 bytes. 24 bits caps a frame at 16 MiB — a natural allocation bound — and brings the header to exactly 8 bytes. |
| Stream ID | 0 | HTTP/2 needs 31 bits to multiplex streams. BH/1 has no multiplexing: one exchange at a time, in order, so the field would always be zero. It is left out. |

Further decisions:

* **8-byte header.** A power of two, half a 16-byte hexdump row, no bit fields:
  every field can be read straight off a dump without shifting or masking.
* **Length counts the payload only.** Then every 24-bit value is a legal length,
  including 0; there is no "length smaller than the header" case to reject, and
  frame boundaries can never be ambiguous.
* **Length before payload, never a terminator.** A receiver can size its buffer,
  enforce limits and skip frames it does not understand without parsing them.
* **Big-endian.** Network byte order is the convention for wire protocols and
  reads left-to-right in a hexdump (`00 00 2b` is visibly 43).
* **Explicit per-field encoding, no struct dumps.** Struct layout, padding and
  host byte order are compiler details; the wire format is defined byte by byte.
* **Numeric IDs for ten common header names, ID 0 as the escape.** The common
  case costs one byte instead of 4–16; the uncommon case stays fully general. Zero
  is the natural "none of the above" value, and keeping the escape in the same
  byte as the ID means there is only one header grammar to parse.
* **Count-prefixed header list** rather than an end marker: the receiver knows
  the bound (≤ 32) before reading any field.
* **`u8` NameLen, `u16` ValueLen and PathLen.** Names are short (limit 64);
  values and paths need more than 255 bytes but far less than 65,535.
* **`u16` Status.** Three-digit codes up to 599 do not fit in one byte; a
  fixed-width binary integer avoids parsing ASCII digits.
* **`u32` BodyLen, although Length already implies it.** The redundancy is a
  cheap consistency check (a frame whose inner and outer lengths disagree is
  rejected), and it makes the payload self-delimiting, so its grammar does not
  depend on the frame header.
* **One frame per message.** A request or response is always a single frame, so
  there is no reassembly state. The cost is a 16 MiB ceiling on bodies; lifting it
  would be done with a new (skippable) continuation frame type, not by changing
  existing frames.
* **Rejected-but-in-sync vs fatal.** Because Length is validated independently of
  the payload, most bad requests cost the client a `400`, not the connection. Only
  a bad Magic or Version — the two cases where Length is meaningless — is fatal.
* **Byte-string paths without decoding.** No escaping layer means no "decode then
  re-check" bugs; traversal is decided on the exact bytes received.
