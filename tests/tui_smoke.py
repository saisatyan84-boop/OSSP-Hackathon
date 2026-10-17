#!/usr/bin/env python3
"""Exercise the actual ncurses client through a pseudo-terminal."""
import fcntl
import json
import os
import pathlib
import pty
import select
import signal
import socket
import struct
import subprocess
import tempfile
import termios
import time

ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=ROOT/'build/relay-chat'

with tempfile.TemporaryDirectory(prefix='relay-tui-') as tmp:
    path=pathlib.Path(tmp)/'chat.sock'
    server=subprocess.Popen([str(BIN),'server','--socket',str(path)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    master,slave=pty.openpty()
    def resize(rows,cols):fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
    resize(34,120)
    client=None;peer=None;output=bytearray()
    def drain(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if select.select([master],[],[],.03)[0]:
                try:data=os.read(master,65536)
                except OSError:break
                output.extend(data)
    def type_line(text):os.write(master,(text+'\n').encode());drain(.25)
    def send(kind,**args):peer.sendall((json.dumps(dict(v=1,type=kind,**args))+'\n').encode())
    try:
        until=time.monotonic()+3
        while not path.exists() and time.monotonic()<until:time.sleep(.02)
        peer=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);peer.connect(str(path))
        send('hello',name='Bob')
        client=subprocess.Popen([str(BIN),'--name','Alice','--socket',str(path)],stdin=slave,stdout=slave,stderr=slave,
                                env={**os.environ,'TERM':'xterm-256color'},start_new_session=True)
        drain(.8)
        send('chat',text='A real message from a different process');drain(.3)
        type_line('Hello Bob from the terminal UI')
        send('chat',text='Unicode works: नमस्ते — café');drain(.3)
        assert 'café'.encode() in output
        for command in ['/help','/who','/stats','/msg Bob private-terminal-test','/join demo']:
            type_line(command)
        assert client.poll() is None
        assert b'RELAY' in output and b'CONVERSATION' in output and b'private-terminal-test' in output
        # ncurses reuses the existing '#' cell when changing the room title.
        assert b'IPC SERVER STATISTICS' in output and b'demo  /  CONVERSATION' in output
        for rows,cols in [(18,70),(12,50),(34,120)]:
            resize(rows,cols);os.kill(client.pid,signal.SIGWINCH);drain(.3)
        assert b'Resize to at least' in output
        os.write(master,b'\x1b[5~\x1b[6~');drain(.2)
        type_line('/join lobby')
        send('chat',text='Still receiving after resize');drain(.3)
        assert b'Still receiving after resize' in output
        type_line('/quit');client.wait(timeout=3)
        assert client.returncode==0
        print('PASS: real ncurses UI, incoming/outgoing messages, commands, private chat, rooms, resizing and clean exit')
    finally:
        (ROOT/'var').mkdir(exist_ok=True)
        (ROOT/'var/tui-test.log').write_bytes(output)
        if client and client.poll() is None:client.kill();client.wait()
        if peer:peer.close()
        server.terminate();server.wait(timeout=3)
        os.close(master);os.close(slave)
