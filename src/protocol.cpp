#include "protocol.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace relay {
Fd::~Fd() { if(value>=0) ::close(value); }
Fd::Fd(Fd&& other) noexcept:value(std::exchange(other.value,-1)) {}
Fd& Fd::operator=(Fd&& other) noexcept { if(this!=&other) { if(value>=0) ::close(value); value=std::exchange(other.value,-1); } return *this; }
void nonblocking(int fd) {
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) throw std::runtime_error("Cannot configure nonblocking socket");
}
bool Channel::queue(const Json& frame) {
    if(dead) return false;
    auto data=frame.dump()+"\n";
    if(data.size()>MAX_FRAME || outgoing.size()-sent+data.size()>MAX_PENDING) { dead=true; return false; }
    if(sent) { outgoing.erase(0,sent); sent=0; }
    outgoing+=data;
    return true;
}
bool Channel::flush() {
    size_t budget=65536;
    while(pending() && budget) {
        auto n=::send(fd.value,outgoing.data()+sent,std::min(budget,outgoing.size()-sent),MSG_NOSIGNAL);
        if(n>0) { sent+=size_t(n); budget-=size_t(n); }
        else if(n<0 && errno==EINTR) continue;
        else if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return true;
        else { dead=true; return false; }
    }
    if(!pending()) { outgoing.clear(); sent=0; }
    return true;
}
bool Channel::read(std::vector<Json>& frames,std::string& error) {
    char data[8192]; size_t budget=65536;
    while(budget) {
        auto n=::recv(fd.value,data,std::min(sizeof(data),budget),0);
        if(n>0) {
            budget-=size_t(n); incoming.append(data,size_t(n));
            size_t at;
            while((at=incoming.find('\n'))!=std::string::npos) {
                if(at==0 || at+1>MAX_FRAME) { error="Invalid frame length"; dead=true; return false; }
                try {
                    auto j=Json::parse(incoming.substr(0,at),[](int depth,Json::parse_event_t,Json&) {
                        if(depth>8) throw std::runtime_error("Frame nesting limit exceeded");
                        return true;
                    });
                    if(!j.is_object() || !j.contains("v") || !j["v"].is_number_integer() || j["v"]!=VERSION ||
                       !j.contains("type") || !j["type"].is_string()) throw std::runtime_error("Invalid protocol envelope");
                    frames.push_back(std::move(j));
                } catch(const std::exception&) { error="Invalid JSON/protocol frame"; dead=true; return false; }
                incoming.erase(0,at+1);
            }
            if(incoming.size()>=MAX_FRAME) { error="Frame too large"; dead=true; return false; }
        } else if(n==0) { error=incoming.empty()?"Connection closed":"Incomplete frame at EOF"; dead=true; return false; }
        else if(errno==EINTR) continue;
        else if(errno==EAGAIN || errno==EWOULDBLOCK) return true;
        else { error=std::strerror(errno); dead=true; return false; }
    }
    return true;
}
std::string default_socket() { return "/tmp/relay-chat-"+std::to_string(getuid())+"/server.sock"; }
Fd connect_socket(const std::string& path) {
    sockaddr_un addr{}; addr.sun_family=AF_UNIX;
    if(path.empty() || path.size()>=sizeof(addr.sun_path)) throw std::runtime_error("Socket path is too long");
    std::memcpy(addr.sun_path,path.c_str(),path.size()+1);
    Fd fd(::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0));
    if(fd.value<0 || ::connect(fd.value,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)
        throw std::runtime_error("Cannot connect to "+path+": "+std::strerror(errno)+". Start ./chat server first.");
    ucred peer{}; socklen_t size=sizeof(peer);
    if(getsockopt(fd.value,SOL_SOCKET,SO_PEERCRED,&peer,&size)<0 || peer.uid!=getuid())
        throw std::runtime_error("Server belongs to another OS user");
    nonblocking(fd.value); return fd;
}
bool identifier(const std::string& s) {
    if(s.empty() || s.size()>20) return false;
    for(unsigned char c:s) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return false;
    return true;
}
bool valid_text(const std::string& s) {
    if(s.empty() || s.size()>MAX_TEXT || s.find_first_not_of(' ')==std::string::npos) return false;
    for(unsigned char c:s) if(c<32 || c==127) return false;
    // nlohmann's parser has already validated the UTF-8 encoding on received text.
    return true;
}
Json envelope(const std::string& type) { return {{"v",VERSION},{"type",type}}; }
std::string json_string(const Json& j,const char* key) {
    if(!j.contains(key) || !j[key].is_string()) throw std::runtime_error(std::string("Expected string: ")+key);
    return j[key].get<std::string>();
}
std::string timestamp() {
    auto t=std::time(nullptr); std::tm tm{}; localtime_r(&t,&tm);
    char buffer[32]; std::strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S",&tm); return buffer;
}
std::string time_label(const std::string& value) { return value.size()>=19?value.substr(11,8):value; }
}
