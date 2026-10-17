#include "protocol.hpp"
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>

namespace relay {
namespace {
volatile sig_atomic_t stopping=0;
void stop(int) { stopping=1; }
struct Endpoint {
    std::string path;
    Fd lock,listener;
    bool bound=false;
    explicit Endpoint(std::string value):path(std::move(value)) {
        if(path.empty() || path.size()>=sizeof(sockaddr_un::sun_path)) throw std::runtime_error("Invalid socket path length");
        auto parent=std::filesystem::path(path).parent_path();
        if(parent.empty()) throw std::runtime_error("Use an absolute socket path in a private directory");
        ::umask(0077);
        std::filesystem::create_directories(parent);
        struct stat st{};
        if(lstat(parent.c_str(),&st)<0 || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&0077))
            throw std::runtime_error("Socket directory must be owned by you and private (mode 0700)");
        lock=Fd(open((path+".lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600));
        if(lock.value<0 || fstat(lock.value,&st)<0 || !S_ISREG(st.st_mode) || st.st_uid!=getuid())
            throw std::runtime_error("Cannot open socket ownership lock");
        if(flock(lock.value,LOCK_EX|LOCK_NB)<0) throw std::runtime_error("A server already owns this socket. Connect with ./chat --name Alice");
        if(lstat(path.c_str(),&st)==0) {
            if(!S_ISSOCK(st.st_mode) || st.st_uid!=getuid()) throw std::runtime_error("Refusing to replace a non-socket or foreign file");
            // Even an unrelated listener without our lock must never be unlinked.
            Fd probe(::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
            sockaddr_un check{}; check.sun_family=AF_UNIX; std::strcpy(check.sun_path,path.c_str());
            if(::connect(probe.value,reinterpret_cast<sockaddr*>(&check),sizeof(check))==0)
                throw std::runtime_error("An active server already uses this path");
            if(errno!=ECONNREFUSED && errno!=ENOENT) throw std::runtime_error("Cannot verify stale socket");
            if(::unlink(path.c_str())<0) throw std::runtime_error("Cannot remove stale socket");
        } else if(errno!=ENOENT) throw std::runtime_error("Cannot inspect socket path");
        listener=Fd(::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
        sockaddr_un addr{}; addr.sun_family=AF_UNIX; std::strcpy(addr.sun_path,path.c_str());
        if(listener.value<0 || ::bind(listener.value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)
            throw std::runtime_error("Cannot bind Unix socket: "+std::string(std::strerror(errno)));
        bound=true;
        if(::listen(listener.value,32)<0) { ::unlink(path.c_str()); bound=false; throw std::runtime_error("listen failed"); }
    }
    ~Endpoint() { if(bound) ::unlink(path.c_str()); }
};
struct Peer {
    Channel channel;
    std::string name,room="lobby";
    int pid;
    bool closing=false;
    double tokens=40;
    Clock::time_point since=Clock::now(),rateTime=since,closingSince=since;
    Peer(int fd,int process):channel(fd),pid(process) {}
};
}
int server(const std::string& path) {
    stopping=0; std::signal(SIGINT,stop); std::signal(SIGTERM,stop);
    Endpoint endpoint(path);
    std::map<int,Peer> peers;
    std::map<std::string,std::deque<Json>> history{{"lobby",{}}};
    uint64_t sequence=0,messages=0;
    auto began=Clock::now();
    auto error=[](Peer& p,const std::string& text,bool close=false) {
        auto j=envelope("error"); j["text"]=text; p.channel.queue(j);
        if(close) { p.closing=true; p.closingSince=Clock::now(); }
    };
    auto broadcast=[&](const Json& j,const std::string& room="") {
        for(auto& [fd,p]:peers) if(!p.name.empty() && !p.closing && (room.empty() || p.room==room)) p.channel.queue(j);
    };
    auto presence=[&] {
        auto j=envelope("presence"); j["users"]=Json::array(); j["rooms"]=Json::array();
        for(const auto& [fd,p]:peers) if(!p.name.empty() && !p.closing && !p.channel.dead)
            j["users"].push_back({{"name",p.name},{"room",p.room},{"pid",p.pid}});
        for(const auto& [room,items]:history) j["rooms"].push_back(room);
        broadcast(j);
    };
    auto notice=[&](const std::string& text,const std::string& room) {
        auto j=envelope("notice"); j["text"]=text; j["room"]=room; j["time"]=timestamp(); broadcast(j,room);
    };
    auto room_data=[&](Peer& p) {
        auto j=envelope("room"); j["room"]=p.room; p.channel.queue(j);
        for(const auto& saved:history[p.room]) { auto item=saved; item["history"]=true; p.channel.queue(item); }
        j=envelope("history_end"); j["room"]=p.room; p.channel.queue(j);
    };
    std::cout<<"RELAY / IPC CHAT SERVER\nPID: "<<getpid()<<"\nTransport: AF_UNIX / SOCK_STREAM\nSocket: "<<path
             <<"\nReady. Open other Ubuntu terminals and run ./chat --name Alice and ./chat --name Bob\nCtrl+C stops the server.\n"<<std::flush;
    while(!stopping) {
        std::vector<pollfd> fds{{endpoint.listener.value,POLLIN,0}};
        for(auto& [fd,p]:peers) fds.push_back({fd,short((p.closing?0:POLLIN)|(p.channel.pending()?POLLOUT:0)),0});
        int result=::poll(fds.data(),fds.size(),250);
        if(result<0) { if(errno==EINTR) continue; throw std::runtime_error("Server poll failed"); }
        if(fds[0].revents&POLLIN) {
            for(int accepted=0;accepted<16;++accepted) {
                int fd=accept4(endpoint.listener.value,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
                if(fd<0) break;
                ucred credentials{}; socklen_t size=sizeof(credentials);
                if(peers.size()>=32 || getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&size)<0 || credentials.uid!=getuid()) { close(fd); continue; }
                peers.try_emplace(fd,fd,credentials.pid);
            }
        }
        for(size_t index=1;index<fds.size();++index) {
            auto it=peers.find(fds[index].fd); if(it==peers.end()) continue;
            auto& p=it->second;
            if(fds[index].revents&POLLOUT) p.channel.flush();
            if(fds[index].revents&(POLLIN|POLLHUP)) {
                std::vector<Json> frames; std::string why;
                p.channel.read(frames,why);
                for(const auto& j:frames) {
                    if(p.closing) break;
                    auto now=Clock::now();
                    p.tokens=std::min(40.,p.tokens+std::chrono::duration<double>(now-p.rateTime).count()*20); p.rateTime=now;
                    if(p.tokens<1) { error(p,"Rate limit: at most 20 requests/second",true); break; } --p.tokens;
                    try {
                        auto type=json_string(j,"type");
                        if(p.name.empty()) {
                            if(type!="hello") { error(p,"Send hello first",true); break; }
                            auto name=json_string(j,"name");
                            if(!identifier(name)) { error(p,"Name: 1-20 letters, digits, hyphens or underscores",true); break; }
                            bool taken=false;
                            for(const auto& [fd,other]:peers) if(other.name==name) taken=true;
                            if(taken) { error(p,"That name is already online. Choose another name.",true); break; }
                            p.name=name;
                            auto welcome=envelope("welcome"); welcome["name"]=p.name; welcome["pid"]=p.pid;
                            welcome["server_pid"]=getpid(); welcome["socket"]=path; p.channel.queue(welcome);
                            room_data(p); notice(p.name+" joined #lobby (PID "+std::to_string(p.pid)+")",p.room); presence();
                            std::cout<<timestamp()<<" JOIN "<<p.name<<" pid="<<p.pid<<'\n'<<std::flush;
                        } else if(type=="chat" || type=="dm") {
                            auto text=json_string(j,"text");
                            if(!valid_text(text)) { error(p,"Message must contain 1-2048 bytes, without control characters"); continue; }
                            Peer* target=nullptr; std::string to;
                            if(type=="dm") {
                                to=json_string(j,"to");
                                for(auto& [fd,other]:peers) if(other.name==to && !other.closing && !other.channel.dead) target=&other;
                                if(!target) { error(p,"User is not online: "+to); continue; }
                            }
                            auto message=envelope(type=="dm"?"private":"message");
                            message["id"]=++sequence; message["from"]=p.name; message["pid"]=p.pid;
                            message["room"]=p.room; message["text"]=text; message["time"]=timestamp(); message["history"]=false;
                            if(target) {
                                message["to"]=to; target->channel.queue(message); if(target!=&p) p.channel.queue(message);
                            } else {
                                auto& saved=history[p.room]; saved.push_back(message); if(saved.size()>100) saved.pop_front();
                                broadcast(message,p.room);
                            }
                            ++messages;
                            auto ack=envelope("ack"); ack["id"]=sequence; p.channel.queue(ack);
                        } else if(type=="join") {
                            auto room=json_string(j,"room");
                            if(!identifier(room)) { error(p,"Room: 1-20 letters, digits, hyphens or underscores"); continue; }
                            if(!history.count(room) && history.size()>=16) { error(p,"Room limit reached (16)"); continue; }
                            if(p.room==room) { error(p,"You are already in #"+room); continue; }
                            notice(p.name+" moved to #"+room,p.room); p.room=room; room_data(p);
                            notice(p.name+" joined #"+room,p.room); presence();
                        } else if(type=="who") presence();
                        else if(type=="stats") {
                            auto stats=envelope("stats"); stats["server_pid"]=getpid(); stats["messages"]=messages;
                            stats["rooms"]=history.size(); stats["clients"]=0;
                            for(const auto& [fd,other]:peers) if(!other.name.empty() && !other.channel.dead) stats["clients"]=stats["clients"].get<int>()+1;
                            stats["uptime_seconds"]=std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-began).count(); p.channel.queue(stats);
                        } else if(type=="quit") { p.closing=true; p.closingSince=Clock::now(); }
                        else error(p,"Unknown request type");
                    } catch(const std::exception&) { error(p,"Invalid request fields"); }
                }
            }
            if(fds[index].revents&(POLLERR|POLLNVAL)) p.channel.dead=true;
        }
        std::vector<std::pair<std::string,std::string>> departed;
        for(auto it=peers.begin();it!=peers.end();) {
            auto& p=it->second;
            if(p.name.empty() && Clock::now()-p.since>std::chrono::seconds(5)) p.channel.dead=true;
            if(p.closing && Clock::now()-p.closingSince>std::chrono::seconds(2)) p.channel.dead=true;
            if(p.channel.dead || (p.closing && !p.channel.pending())) {
                if(!p.name.empty()) departed.emplace_back(p.name,p.room);
                it=peers.erase(it);
            } else ++it;
        }
        if(!departed.empty()) {
            for(const auto& [name,room]:departed) { notice(name+" left",room); std::cout<<timestamp()<<" LEAVE "<<name<<'\n'<<std::flush; }
            presence();
        }
    }
    auto goodbye=envelope("shutdown"); goodbye["text"]="Server stopped. Messages in this session are not saved to disk.";
    broadcast(goodbye);
    for(auto& [fd,p]:peers) p.channel.flush();
    std::cout<<"Server stopped; socket removed.\n";
    return 0;
}
}
