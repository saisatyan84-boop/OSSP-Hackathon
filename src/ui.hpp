#pragma once
#include "protocol.hpp"
#include <optional>
#include <string>
#include <vector>

namespace relay {
struct Entry { std::string heading,text; int color=2; };
struct View {
    std::string name,room="lobby",status="Connecting",socket;
    int pid=0,serverPid=0;
    uint64_t received=0,sent=0,lastAck=0;
    bool connected=false;
    std::vector<Entry> entries;
    Json users=Json::array(),rooms=Json::array();
    void add(std::string heading,std::string text,int color=2);
};
class Ui {
    std::wstring input_;
    size_t cursor_=0,historyIndex_=0;
    std::vector<std::wstring> history_;
    int scroll_=0;
public:
    Ui();
    ~Ui();
    Ui(const Ui&)=delete;
    Ui& operator=(const Ui&)=delete;
    // Returns a submitted line; updates dirty for edits, scrolling and resize.
    std::optional<std::string> input(bool& dirty);
    void draw(const View& view);
};
}
