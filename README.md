# Relay — Inter-Process Chat Application Using IPC

A C++17 terminal chat application for Linux and Ubuntu on WSL. Independent
processes exchange messages in both directions through a **Unix domain stream
socket**. No web service, TCP port, internet connection or Windows agent is used.

## Start the hackathon demo

Open two Ubuntu terminals.

**Terminal 1 — build, start a background server, open Alice:**

```bash
cd /home/satyan/Espadas_OSSP/Project/Hackathon
./build.sh
./demo.sh
```

**Terminal 2 — open Bob:**

```bash
cd /home/satyan/Espadas_OSSP/Project/Hackathon
./chat --name Bob
```

Type messages and press Enter in either terminal. Both are real independent
client processes. Their PIDs and the server PID appear in the interface.
If the demo script started the server, quitting Alice also stops that server.
For a presentation, the explicit three-terminal setup below makes the processes
and their lifecycle especially easy to explain.

## Explicit three-process setup

From this directory, use a separate Ubuntu terminal for each command:

```bash
./chat server
./chat --name Alice
./chat --name Bob
```

The server stays running until Ctrl+C. Add more clients with unique names.

## Features

- Full-duplex chat: each client can send while receiving messages.
- Color terminal interface, Unicode messages, live online users and real PIDs.
- Multiple rooms and private messages across rooms.
- Last 100 room messages replayed when joining that room, for the current server session.
- Server acknowledgements, message IDs and IPC statistics.
- Input editing, command recall, conversation scrolling and terminal resizing.
- Nonblocking I/O, bounded queues, frame validation and request rate limits.
- Peer identity verified with Linux `SO_PEERCRED`; local same-OS-user clients only.
- Socket ownership lock, stale-socket recovery, clean shutdown and visible disconnects.
- Plain-text and JSON modes for terminals, pipes and automated tests.

## Commands and keys

| Input | Action |
| --- | --- |
| Plain text + Enter | Send to everyone in the current room, including yourself |
| `/join project` | Create or enter the `project` room |
| `/msg Bob hello` | Private message to Bob, even if he is in another room |
| `/who` | Refresh online users; plain/JSON modes print the list |
| `/stats` | Show server PID, uptime, client/room counts and routed-message count |
| `/help` | Show command help |
| `/clear` | Clear your local conversation view |
| `/quit`, Ctrl+D, Ctrl+C | Exit the client |
| Left/Right, Home/End, Backspace/Delete | Edit input |
| Up/Down | Recall previous input |
| Page Up/Page Down | Scroll conversation |
| Ctrl+U | Clear input |

Use at least **70 columns × 18 rows**. At 100 columns or wider, the online-users
sidebar appears. A large terminal or a smaller terminal font improves the demo.
Names and rooms are case-sensitive, with 1–20 ASCII letters, digits, `_` or `-`.

## Build and test

Required: `g++` with C++17, `make`, and `libncursesw` development headers/library.
Python 3 is needed only for tests. All dependencies are already available on the
machine where this project was built. For a fresh Ubuntu installation:

```bash
sudo apt install build-essential libncurses-dev python3
```

```bash
./build.sh
make test
```

`nlohmann/json` 3.12.0 is vendored under `include/nlohmann` with its MIT license.
It handles proper JSON parsing and UTF-8 validation. `ncursesw` provides drawing,
keyboard handling and terminal restoration. No package download is needed to
build this copy. Application code lives entirely in this folder.

## Layout

```text
Hackathon/
  src/main.cpp        command-line entry point
  src/protocol.*      bounded framing, partial I/O, descriptor ownership
  src/server.cpp      independent broker process and message routing
  src/client.cpp      client event loop, commands, acknowledgements
  src/ui.*            ncurses terminal interface and local view model
  include/nlohmann/   JSON dependency and license
  tests/              actual socket/process and terminal integration tests
  docs/               architecture and presentation guide
  build.sh            build command
  chat                launcher
  demo.sh             two-terminal demo helper
```

## Socket and lifecycle

The default endpoint is `/tmp/relay-chat-<UID>/server.sock`. The server requires
its containing directory to be owned by the current user and private (0700).
It refuses duplicate listeners and does not replace unrelated files. Clients
verify the server's OS user; the server obtains client PIDs directly from the
kernel, not from a client-supplied field.

To run an isolated instance, give **the same absolute socket path** to the server
and each client. Its containing directory must be private:

```bash
./chat server --socket "$HOME/.local/state/relay-demo/server.sock"
./chat --name Alice --socket "$HOME/.local/state/relay-demo/server.sock"
```

If the server dies, clients report disconnection. Restart the server and reopen
the clients. The restarted server safely removes its stale socket. History is
in memory and does not survive a server restart. Private messages are not
stored in server room history. An ACK means the server accepted a message;
it is **not** a recipient read receipt or a durable-delivery guarantee.

The broker is trusted and sees message content. There is no encryption or
cross-user authentication layer. This is intentionally a **local IPC project**,
not a network chat service. Maximums: 32 connections, 16 rooms, 2 KiB message
text, 16 KiB frames, 1 MiB pending output per connection. Sustained request
rate is limited to 20/second with a 40-request burst allowance. Slow consumers
that exceed the queue limit are disconnected so other users can continue.

## Troubleshooting

- **Cannot connect:** start `./chat server` first, or use `./demo.sh`.
- **Name already online:** pick another name or close that client's terminal.
- **Private directory error:** choose a new private subdirectory, not `/tmp` itself.
- **Terminal error:** use Ubuntu's terminal with `TERM=xterm-256color`, or `--plain`.
- **Compilation cannot find curses.h / -lncursesw:** install `libncurses-dev`.
- **Server already owns socket:** connect to it instead of starting a second one.

For the explanation and demo sequence, read [docs/HACKATHON_GUIDE.md](docs/HACKATHON_GUIDE.md).
