#!/usr/bin/env python3
"""Real Unix socket and independent executable-process integration tests; no packages needed."""
import json
import os
import pathlib
import select
import signal
import socket
import subprocess
import tempfile
import time

ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=ROOT/'build/relay-chat'


class Events:
    def __init__(self, fd):
        self.fd=fd
        self.data=b''
        self.events=[]
    def pump(self, timeout=.1):
        if select.select([self.fd],[],[],timeout)[0]:
            chunk=os.read(self.fd,65536)
            if not chunk: return False
            self.data+=chunk
            while b'\n' in self.data:
                row,self.data=self.data.split(b'\n',1)
                self.events.append(json.loads(row))
        return True
    def wait(self, kind, predicate=lambda e:True, timeout=3):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            for i,e in enumerate(self.events):
                if e.get('type')==kind and predicate(e): return self.events.pop(i)
            self.pump(.05)
        raise AssertionError((kind,self.events))
    def absent(self, kind, predicate=lambda e:True):
        end=time.monotonic()+.2
        while time.monotonic()<end:self.pump(.02)
        assert not any(e.get('type')==kind and predicate(e) for e in self.events)


class Client:
    def __init__(self,path,name):
        self.p=subprocess.Popen([str(BIN),'--socket',str(path),'--name',name,'--json'],
                                stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.e=Events(self.p.stdout.fileno())
    def send(self,text):
        self.p.stdin.write((text+'\n').encode());self.p.stdin.flush()
    def close(self):
        if self.p.poll() is None:
            self.send('/quit')
            self.p.wait(timeout=3)
        assert self.p.returncode==0


def connect(path):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.settimeout(2);s.connect(str(path));return s


def frame(kind,**fields):return (json.dumps(dict(v=1,type=kind,**fields))+'\n').encode()


def start(path,log):
    p=subprocess.Popen([str(BIN),'server','--socket',str(path)],stdout=log,stderr=log)
    end=time.monotonic()+3
    while not path.exists() and p.poll() is None and time.monotonic()<end:time.sleep(.02)
    assert p.poll() is None and path.exists()
    return p


with tempfile.TemporaryDirectory(prefix='relay-tests-') as tmp:
    path=pathlib.Path(tmp)/'chat.sock'
    with open(pathlib.Path(tmp)/'server.log','w+') as log:
        server=start(path,log);clients=[]
        try:
            assert path.stat().st_mode&0o777==0o700 or path.stat().st_mode&0o777==0o600
            alice=Client(path,'Alice');bob=Client(path,'Bob');clients.extend([alice,bob])
            a=alice.e.wait('welcome');b=bob.e.wait('welcome')
            assert a['pid']==alice.p.pid and b['pid']==bob.p.pid
            assert a['server_pid']==server.pid and len({a['pid'],b['pid'],server.pid})==3
            alice.e.wait('presence',lambda e:len(e['users'])==2)
            alice.send('Hello Bob — नमस्ते!')
            message=bob.e.wait('message',lambda e:e['text']=='Hello Bob — नमस्ते!')
            assert message['from']=='Alice' and message['pid']==alice.p.pid
            assert alice.e.wait('ack')['id']==message['id']
            bob.send('Hello Alice!')
            assert alice.e.wait('message',lambda e:e['from']=='Bob')['text']=='Hello Alice!'
            print('PASS: independent process IDs, two-way Unicode chat and server ACKs')

            duplicate=Client(path,'Alice')
            assert 'already online' in duplicate.e.wait('error')['text']
            duplicate.p.wait(timeout=3);assert duplicate.p.returncode==1
            bob.send('/join project');bob.e.wait('room',lambda e:e['room']=='project')
            bob.send('Room-only message')
            bob.e.wait('ack')
            alice.e.absent('message',lambda e:e['text']=='Room-only message')
            alice.send('/msg Bob Private across rooms')
            assert bob.e.wait('private')['text']=='Private across rooms'
            charlie=Client(path,'Charlie');clients.append(charlie);charlie.e.wait('welcome')
            charlie.e.absent('private')
            charlie.send('/join project');charlie.e.wait('room',lambda e:e['room']=='project')
            saved=charlie.e.wait('message',lambda e:e['text']=='Room-only message')
            assert saved['history'] is True
            charlie.e.absent('private')
            print('PASS: duplicate names rejected, rooms isolated, private routing, room history')

            # Split a single JSON frame and coalesce several frames in one write.
            raw=connect(path); events=Events(raw.fileno())
            hello=frame('hello',name='Framing')
            raw.sendall(hello[:7]);time.sleep(.03);raw.sendall(hello[7:]+frame('stats'))
            events.wait('welcome');stats=events.wait('stats');assert stats['clients']==4
            raw.sendall(frame('chat',text='')+frame('chat',text='\x1b[31mcontrol')+frame('chat',text='x'*2049)+frame('stats'))
            for _ in range(3):events.wait('error')
            events.wait('stats');raw.close()
            print('PASS: fragmented/coalesced frames and invalid message validation')

            for bad in [b'not json\n',b'x'*16384,frame('hello',name='bad name'),b'{"v":99,"type":"hello","name":"Old"}\n']:
                raw=connect(path);raw.sendall(bad)
                # Rejection may include one bounded error event before the close.
                end=time.monotonic()+3
                while time.monotonic()<end:
                    try:
                        if not raw.recv(65536):break
                    except ConnectionResetError:break
                else:raise AssertionError('Malformed client was not disconnected')
                raw.close()
            second=subprocess.run([str(BIN),'server','--socket',str(path)],capture_output=True,timeout=3)
            assert second.returncode!=0 and path.exists() and server.poll() is None
            alice.send('/stats');alice.e.wait('stats')
            print('PASS: malformed/oversized clients isolated; duplicate server preserves listener')

            bob.p.kill();bob.p.wait()
            alice.e.wait('presence',lambda e:all(u['name']!='Bob' for u in e['users']))
            replacement=Client(path,'Bob');clients.append(replacement);replacement.e.wait('welcome')
            for c in clients:
                if c.p.poll() is None:c.close()
            server.send_signal(signal.SIGTERM);server.wait(timeout=3)
            assert server.returncode==0 and not path.exists()
            print('PASS: abrupt client exit, name reuse and graceful server socket cleanup')

            server=start(path,log);server.kill();server.wait()
            assert path.exists()
            server=start(path,log)
            test=Client(path,'Restart');clients.append(test);test.e.wait('welcome')
            test.send('/stats');assert test.e.wait('stats')['messages']==0
            server.terminate();server.wait(timeout=3)
            test.e.wait('shutdown');test.p.wait(timeout=3)
            assert not path.exists()
            print('PASS: recovery from a killed server, session reset and shutdown notification')
        finally:
            for c in clients:
                if c.p.poll() is None:c.p.kill();c.p.wait()
            if server.poll() is None:server.terminate();server.wait(timeout=3)
print('All IPC integration tests passed.')
