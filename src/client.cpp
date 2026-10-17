#include "protocol.hpp"
#include "ui.hpp"
#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace relay {
namespace {
volatile sig_atomic_t interrupted=0;
void interrupt(int) { interrupted=1; }
const std::string help="Send text with Enter. /join room creates/switches rooms; /msg NAME text sends privately; /who lists users; /stats shows IPC statistics; /clear clears your screen; /quit exits. PgUp/PgDn scroll, Up/Down recall input, Ctrl+U clears input, Ctrl+D quits.";
}
int client(const std::string& path,const std::string& name,bool plain,bool json_output) {
    if(!identifier(name)) throw std::runtime_error("Name must be 1-20 letters, digits, underscores or hyphens");
    auto socket=connect_socket(path); Channel connection(socket.value); socket.value=-1;
    View view; view.name=name; view.pid=getpid(); view.socket=path;
    std::unique_ptr<Ui> ui;
    if(!plain && !json_output) ui=std::make_unique<Ui>();
    interrupted=0; std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
    auto hello=envelope("hello"); hello["name"]=name; connection.queue(hello);
    view.add("WELCOME TO RELAY",help,1);
    bool dirty=true,quit=false,stdinOpen=true,everConnected=false;
    auto started=Clock::now(); std::string stdinBuffer;
    auto local=[&](const std::string& text,int color=4) {
        view.add("",text,color); dirty=true;
        if(!ui) { auto j=envelope("local"); j["text"]=text; std::cout<<(json_output?j.dump():text)<<'\n'<<std::flush; }
    };
    auto submit=[&](std::string line) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty()) return;
        if(line=="/quit") { connection.queue(envelope("quit")); quit=true; return; }
        if(line=="/help") { local(help,1); return; }
        if(line=="/clear") { view.entries.clear(); dirty=true; return; }
        if(!view.connected) { local("Not connected. Restart the client after starting the server."); return; }
        Json request;
        if(line[0]=='/') {
            std::istringstream in(line); std::string command,arg; in>>command;
            if(command=="/who" || command=="/stats") request=envelope(command.substr(1));
            else if(command=="/join") {
                in>>arg; std::string extra; in>>extra;
                if(!identifier(arg) || !extra.empty()) { local("Usage: /join room (1-20 letters/digits/_/-)"); return; }
                request=envelope("join"); request["room"]=arg;
            } else if(command=="/msg") {
                in>>arg; std::string text; std::getline(in>>std::ws,text);
                if(!identifier(arg) || !valid_text(text)) { local("Usage: /msg NAME message"); return; }
                request=envelope("dm"); request["to"]=arg; request["text"]=text;
            } else { local("Unknown command. Type /help."); return; }
        } else {
            if(!valid_text(line)) { local("Message must contain 1-2048 bytes, without control characters"); return; }
            request=envelope("chat"); request["text"]=line;
        }
        if(!connection.queue(request)) local("Outgoing buffer full. Connection closed.",7);
        dirty=true;
    };
    auto receive=[&](const Json& j) {
        auto type=json_string(j,"type");
        if(json_output) std::cout<<j.dump()<<'\n'<<std::flush;
        else if(!ui) {
            if(type=="message" || type=="private") std::cout<<'['<<time_label(j.value("time",""))<<"] "<<(type=="private"?"[private] ":"")<<j.value("from","")<<": "<<j.value("text","")<<'\n';
            else if(type=="presence") std::cout<<"Online: "<<j.at("users").dump()<<'\n';
            else if(type=="stats") std::cout<<"Server stats: "<<j.dump()<<'\n';
            else if(j.contains("text")) std::cout<<j.at("text").get<std::string>()<<'\n';
            else if(type=="welcome") std::cout<<"Connected as "<<name<<" (PID "<<getpid()<<") to server PID "<<j.at("server_pid")<<'\n';
            std::cout<<std::flush;
        }
        if(type=="welcome") { view.connected=everConnected=true; view.serverPid=j.at("server_pid").get<int>(); view.status="Messages use local IPC. /help for commands."; }
        else if(type=="room") { view.room=j.at("room").get<std::string>(); view.entries.clear(); view.add("#"+view.room,"Room history is held in server memory for this session.",1); }
        else if(type=="message" || type=="private") {
            auto sender=j.at("from").get<std::string>(); bool mine=sender==name;
            std::string label=time_label(j.value("time",""))+"  "+sender+" / PID "+std::to_string(j.at("pid").get<int>());
            if(type=="private") label+="  [PRIVATE -> "+j.at("to").get<std::string>()+"]";
            if(j.value("history",false)) label+="  [history]";
            view.add(label,j.at("text").get<std::string>(),type=="private"?5:mine?3:1);
            if(!mine && !j.value("history",false)) ++view.received;
        } else if(type=="presence") {
            if(!j.at("users").is_array() || j.at("users").size()>32 || !j.at("rooms").is_array()) throw std::runtime_error("Invalid presence");
            for(const auto& u:j.at("users")) { json_string(u,"name"); json_string(u,"room"); u.at("pid").get<int>(); }
            for(const auto& room:j.at("rooms")) room.get<std::string>();
            view.users=j.at("users"); view.rooms=j.at("rooms");
        } else if(type=="ack") { view.lastAck=j.at("id").get<uint64_t>(); ++view.sent; view.status="Server accepted message #"+std::to_string(view.lastAck)+" (recipient read status is not tracked)."; }
        else if(type=="notice" || type=="error" || type=="shutdown") {
            view.add(type=="error"?"ERROR":"",json_string(j,"text"),type=="error"?7:4);
            if(type=="shutdown") view.connected=false;
        } else if(type=="stats") {
            view.add("IPC SERVER STATISTICS","PID "+std::to_string(j.at("server_pid").get<int>())+" | clients "+std::to_string(j.at("clients").get<int>())+" | rooms "+std::to_string(j.at("rooms").get<int>())+" | messages "+std::to_string(j.at("messages").get<uint64_t>())+" | uptime "+std::to_string(j.at("uptime_seconds").get<int>())+"s",1);
        }
        dirty=true;
    };
    while(!interrupted && !quit) {
        if(ui && dirty) { ui->draw(view); dirty=false; }
        pollfd fds[2]{{connection.dead?-1:connection.fd.value,short(POLLIN|(connection.pending()?POLLOUT:0)),0},
                      {stdinOpen&&(ui || view.connected)?STDIN_FILENO:-1,POLLIN,0}};
        int result=poll(fds,2,ui?50:250);
        if(result<0 && errno!=EINTR) throw std::runtime_error("Client poll failed");
        if(fds[0].revents&POLLOUT) connection.flush();
        if(fds[0].revents&(POLLIN|POLLHUP|POLLERR)) {
            std::vector<Json> frames; std::string why; bool alive=connection.read(frames,why);
            try { for(const auto& frame:frames) receive(frame); }
            catch(const std::exception&) { connection.dead=true; why="Invalid server message"; alive=false; }
            if(!alive) { view.connected=false; view.status="Disconnected / "+why+". /quit to exit."; local(view.status,7); if(!ui) quit=true; }
        }
        if(!everConnected && Clock::now()-started>std::chrono::seconds(6)) {
            local("Server handshake timed out.",7); break;
        }
        if(connection.dead && view.connected) {
            view.connected=false; view.status="Disconnected / send failed. /quit to exit.";
            local(view.status,7); if(!ui) quit=true;
        }
        if(ui) {
            for(int i=0;i<64 && !quit;++i) {
                bool changed=false;
                if(auto line=ui->input(changed)) submit(*line);
                dirty=dirty || changed;
                if(!changed) break;
            }
        }
        else if(fds[1].revents&(POLLIN|POLLHUP)) {
            char data[4096]; auto n=::read(STDIN_FILENO,data,sizeof(data));
            if(n<=0) { stdinOpen=false; quit=true; }
            else {
                stdinBuffer.append(data,size_t(n)); size_t pos;
                while((pos=stdinBuffer.find('\n'))!=std::string::npos) { submit(stdinBuffer.substr(0,pos)); stdinBuffer.erase(0,pos+1); }
                if(stdinBuffer.size()>MAX_TEXT) { local("Input line too long",7); quit=true; }
            }
        }
    }
    // Best effort bounded drain of queued messages, never hang on an unresponsive peer.
    auto deadline=Clock::now()+std::chrono::milliseconds(300);
    while(connection.pending() && !connection.dead && Clock::now()<deadline) {
        pollfd fd{connection.fd.value,POLLOUT,0}; if(poll(&fd,1,25)>0) connection.flush();
    }
    ui.reset(); return everConnected?0:1;
}
}
