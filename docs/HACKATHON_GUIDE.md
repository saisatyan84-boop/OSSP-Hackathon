# Hackathon guide

**Official title:** Inter-Process Chat Application Using IPC  
**Application name:** Relay

## A 60-second explanation

“Relay demonstrates two-way communication between independent operating-system
processes. Each terminal runs a separate C++ client, and a third process runs
the message broker. They communicate through a Unix domain socket in the Linux
kernel. This is local IPC, so no internet or TCP server is involved.

“The clients send structured requests, and the broker routes them to a room or
one private recipient. Every client can send and receive over the same socket.
The screen shows actual kernel-verified process IDs, messages, online users,
and acknowledgements.

“The engineering challenge is that stream sockets deliver bytes, not complete
messages. Our protocol reconstructs fragmented JSON frames, handles partial
writes, and limits queue sizes. A slow or invalid client does not stop the
whole server. We also handle process crashes and stale socket files.”

## Demo sequence: about three minutes

1. Start `./chat server` in Terminal 1. Point out its PID and Unix socket path.
2. Start `./chat --name Alice` and `./chat --name Bob` in two more terminals.
   Point out three different PIDs. Messages do not share a process or global memory.
3. Alice sends “Hello Bob”; Bob replies “Hello Alice.” Explain full-duplex IPC.
4. Alice enters `/msg Bob This is private`. Add Charlie if convenient and show
   that Charlie does not receive the private message.
5. Bob enters `/join project` and sends a message. Alice, still in lobby, does
   not receive it. Alice joins `/join project` and sees its history.
6. Enter `/stats` to display server information and message counts.
7. Resize a client terminal and scroll with Page Up/Page Down.
8. Close one client; show updated online users. Stop the server with Ctrl+C;
   the remaining client reports disconnection instead of crashing.

Keep the running clients on screen for the judging walkthrough. There is no
need for administrator privileges. Run `make test` before the presentation.

## Architecture

```text
   Client process A                         Client process B
   ncurses UI / PID A                       ncurses UI / PID B
          |                                       |
          | bidirectional AF_UNIX streams          |
          +------------------+--------------------+
                             |
                   Broker process / PID S
                   poll() event loop
                   validate -> route -> queue
                   users / rooms / history
                             |
                   Client process C (optional)
```

All participants are independent processes. The kernel buffers bytes between
them. Only the broker owns authoritative room membership and history. There
are no worker threads or shared mutable memory between application processes.

## IPC and protocol decisions

**Why Unix domain sockets?** They provide bidirectional local byte streams,
connection lifecycle events, and peer credentials. They also scale naturally
from two clients to a group chat through a broker.

**Why not ordinary pipes?** Anonymous pipes are usually one-way and typically
connected between related processes. FIFOs can connect independent processes,
but a full-duplex multi-client chat requires more endpoint/routing machinery.
Those alternatives are valid IPC mechanisms; sockets fit this application's
communication pattern directly.

**Why a server process?** It handles discovery, routing, username uniqueness,
room membership, message ordering, and history in one event loop. Clients do
not have to create a separate connection to every other client. The tradeoff
is a single point of failure; restart creates a new in-memory session.

**Framing:** UTF-8 JSON, one object per newline. JSON escapes any newline inside
strings, so the delimiter is unambiguous. The application intentionally accepts
single-line chat input. Receivers accumulate partial reads until a full frame
arrives. Send queues retain unsent suffixes after short writes.

Example client requests:

```json
{"v":1,"type":"hello","name":"Alice"}
{"v":1,"type":"chat","text":"Hello Bob"}
{"v":1,"type":"dm","to":"Bob","text":"Private message"}
{"v":1,"type":"join","room":"project"}
{"v":1,"type":"stats"}
```

Server events include `welcome`, `presence`, `room`, `message`, `private`,
`ack`, `notice`, `stats`, `error`, `history_end`, and `shutdown`. The server
assigns message IDs and sender identities. Clients cannot choose another
sender PID. Message IDs are unique within one server session.

**Concurrency:** `poll()` waits for readable/writable descriptors. Nonblocking
sockets keep one slow peer from pausing unrelated clients. Each socket owns an
input buffer and bounded output buffer. The server limits connections, frame
size, history and request rate.

**Cleanup:** RAII closes descriptors. SIGINT/SIGTERM cause the broker to notify
clients and unlink its socket. A file lock prevents two servers from owning the
same path. After SIGKILL, a restarted server verifies the stale endpoint before
removing it. A `.lock` file may remain, but the kernel releases its lock when
the owning process exits.

## Likely judge questions

| Question | Answer |
| --- | --- |
| Where is IPC happening? | In `send`/`recv` on AF_UNIX socket descriptors between independent processes. |
| Is this just two screens in one program? | No. Show client and server PIDs; tests verify distinct OS process IDs. |
| What makes it two-way? | Every connected client writes requests and reads events on the same full-duplex connection. |
| Does it require internet? | No; the pathname identifies a local kernel socket. |
| Is one read equal to one message? | No. Our framing layer handles split/coalesced frames. |
| What if two clients send together? | The broker processes readable sockets sequentially and assigns a unique session message ID. |
| What if a client stops reading? | Its bounded pending queue eventually causes disconnection; other clients continue. |
| Are private messages encrypted? | No. They are routed only to the sender and recipient; the local broker is trusted. |
| Does ACK mean Bob read it? | No. It means the broker accepted it, not that Bob viewed it. |
| What happens after server failure? | Clients show disconnection; restart recovers the socket, but in-memory history resets. |
| What would you add next? | Persistent history, reconnection/session recovery, optional cross-user access control, file-transfer framing. |

## What was tested

`make test` launches real server/client executables and checks bidirectional
Unicode messages, true process IDs, ACKs, private routing, room isolation,
history replay, duplicate names, split/coalesced frames, invalid/oversized input,
duplicate servers, abrupt client exit, stale-socket recovery, server shutdown,
and the real ncurses interface through a pseudo-terminal, including resizing.

## Credits and scope

The JSON parser (`nlohmann/json`) and terminal library (`ncursesw`) are
dependencies. The IPC protocol, broker, command handling, chat UI and tests
are implemented in this project.
