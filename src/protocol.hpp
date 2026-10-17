#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

namespace relay {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
constexpr size_t MAX_FRAME=16384, MAX_PENDING=1024*1024, MAX_TEXT=2048;
constexpr int VERSION=1;

// Move-only ownership prevents double-close and descriptor leaks on every exit path.
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd) {}
    ~Fd();
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
};

// One socket is a byte stream, not a stream of messages. This class handles
// fragmented/coalesced JSON Lines, short writes and bounded backpressure.
struct Channel {
    Fd fd;
    std::string incoming, outgoing;
    size_t sent=0;
    bool dead=false;
    explicit Channel(int descriptor=-1):fd(descriptor) {}
    bool queue(const Json& frame);
    bool flush();
    bool read(std::vector<Json>& frames, std::string& error);
    bool pending() const { return sent<outgoing.size(); }
};

void nonblocking(int fd);
std::string default_socket();
Fd connect_socket(const std::string& path);
bool identifier(const std::string& text);
bool valid_text(const std::string& text);
Json envelope(const std::string& type);
std::string timestamp();
std::string time_label(const std::string& iso);
std::string json_string(const Json& j,const char* key);
int server(const std::string& path);
int client(const std::string& path,const std::string& name,bool plain,bool json_output);
}
