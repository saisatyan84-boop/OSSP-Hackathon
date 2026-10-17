#include "ui.hpp"
#include <ncursesw/curses.h>
#include <algorithm>
#include <clocale>
#include <cwchar>
#include <climits>
#include <cwctype>
#include <stdexcept>
#include <unistd.h>

namespace relay {
namespace {
std::wstring wide(const std::string& s) {
    std::wstring out; std::mbstate_t state{};
    for(size_t at=0;at<s.size();) {
        wchar_t c{}; size_t n=std::mbrtowc(&c,s.data()+at,s.size()-at,&state);
        if(n==size_t(-1) || n==size_t(-2)) return L"[invalid UTF-8]";
        out+=c; at+=n?n:1;
    }
    return out;
}
std::string utf8(const std::wstring& s) {
    std::string out; std::mbstate_t state{}; char bytes[MB_LEN_MAX];
    for(auto c:s) {
        size_t n=std::wcrtomb(bytes,c,&state);
        if(n==size_t(-1)) throw std::runtime_error("Invalid Unicode input");
        out.append(bytes,n);
    }
    return out;
}
std::wstring printable(std::wstring value) { for(auto& c:value) if(!iswprint(c)) c=L'?'; return value; }
int cells(wchar_t c) { return std::max(0,::wcwidth(c)); }
std::wstring clipped(const std::wstring& s,int limit) {
    std::wstring out; int width=0;
    for(auto c:s) { if(width+cells(c)>limit) break; width+=cells(c); out+=c; } return out;
}
void text(int y,int x,const std::string& value,int width,int color=2,bool bold=false) {
    if(width<=0) return;
    attrset(COLOR_PAIR(color)|(bold?A_BOLD:0));
    auto s=clipped(printable(wide(value)),width); mvaddnwstr(y,x,s.c_str(),int(s.size()));
}
void panel(int y,int x,int h,int w,const std::string& title,int color=1) {
    attrset(COLOR_PAIR(color));
    // Literal wide glyphs avoid legacy alternate-character-set rendering issues.
    cchar_t horizontal{},vertical{};
    setcchar(&horizontal,L"─",A_NORMAL,short(color),nullptr);
    setcchar(&vertical,L"│",A_NORMAL,short(color),nullptr);
    mvhline_set(y,x+1,&horizontal,w-2); mvhline_set(y+h-1,x+1,&horizontal,w-2);
    mvvline_set(y+1,x,&vertical,h-2); mvvline_set(y+1,x+w-1,&vertical,h-2);
    text(y,x,"╭",1,color); text(y,x+w-1,"╮",1,color);
    text(y+h-1,x,"╰",1,color); text(y+h-1,x+w-1,"╯",1,color);
    text(y,x+2," "+title+" ",w-5,color,true);
}
struct Line { std::string text; int color; bool bold; };
void wrap(std::vector<Line>& lines,const std::string& value,int width,int color) {
    auto s=printable(wide(value)); std::wstring part; int count=0;
    for(wchar_t c:s) {
        if(count+cells(c)>width && !part.empty()) { lines.push_back({utf8(part),color,false}); part.clear(); count=0; }
        part+=c; count+=cells(c);
    }
    lines.push_back({utf8(part),color,false});
}
}
void View::add(std::string heading,std::string text,int color) {
    entries.push_back({std::move(heading),std::move(text),color});
    if(entries.size()>500) entries.erase(entries.begin(),entries.begin()+100);
}
Ui::Ui() {
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) throw std::runtime_error("Interactive mode requires a terminal; use --plain or --json for pipes");
    std::setlocale(LC_ALL,"");
    if(MB_CUR_MAX==1) std::setlocale(LC_CTYPE,"C.UTF-8");
    if(!initscr()) throw std::runtime_error("Cannot initialize terminal");
    cbreak(); noecho(); keypad(stdscr,TRUE); nodelay(stdscr,TRUE); set_escdelay(25);
    start_color(); use_default_colors();
    init_pair(1,COLOR_CYAN,-1); init_pair(2,COLOR_WHITE,-1); init_pair(3,COLOR_GREEN,-1);
    init_pair(4,COLOR_YELLOW,-1); init_pair(5,COLOR_MAGENTA,-1); init_pair(6,COLOR_BLUE,-1); init_pair(7,COLOR_RED,-1);
}
Ui::~Ui() { endwin(); }
std::optional<std::string> Ui::input(bool& dirty) {
    wint_t key{}; int result=get_wch(&key); if(result==ERR) return {};
    dirty=true;
    if(result==KEY_CODE_YES) {
        switch(key) {
            case KEY_LEFT: if(cursor_) --cursor_; break;
            case KEY_RIGHT: if(cursor_<input_.size()) ++cursor_; break;
            case KEY_HOME: cursor_=0; break;
            case KEY_END: cursor_=input_.size(); break;
            case KEY_BACKSPACE: if(cursor_) input_.erase(--cursor_,1); break;
            case KEY_DC: if(cursor_<input_.size()) input_.erase(cursor_,1); break;
            case KEY_PPAGE: scroll_+=5; break;
            case KEY_NPAGE: scroll_=std::max(0,scroll_-5); break;
            case KEY_UP: if(historyIndex_) { input_=history_[--historyIndex_]; cursor_=input_.size(); } break;
            case KEY_DOWN:
                if(historyIndex_<history_.size()) ++historyIndex_;
                input_=historyIndex_<history_.size()?history_[historyIndex_]:L""; cursor_=input_.size(); break;
            case KEY_RESIZE: clearok(stdscr,TRUE); break;
            default: break;
        }
    } else if(key=='\n' || key=='\r') {
        auto value=utf8(input_);
        if(!input_.empty()) { history_.push_back(input_); if(history_.size()>50) history_.erase(history_.begin()); }
        historyIndex_=history_.size(); input_.clear(); cursor_=0; scroll_=0; return value;
    } else if(key==4) return "/quit";
    else if(key==21) { input_.clear(); cursor_=0; }
    else if(key==127 || key==8) { if(cursor_) input_.erase(--cursor_,1); }
    else if(iswprint(static_cast<wchar_t>(key))) {
        auto next=input_; next.insert(next.begin()+static_cast<long>(cursor_),static_cast<wchar_t>(key));
        if(utf8(next).size()<=MAX_TEXT) { input_=std::move(next); ++cursor_; } else beep();
    }
    return {};
}
void Ui::draw(const View& v) {
    int h,w; getmaxyx(stdscr,h,w); erase();
    if(w<70 || h<18) {
        curs_set(0); text(0,0,"RELAY / IPC CHAT",w,1,true);
        if(h>2) text(2,0,"Resize to at least 70 columns x 18 rows. Ctrl+C quits.",w,4);
        refresh(); return;
    }
    text(0,2,"RELAY",8,1,true); text(0,10,"INTER-PROCESS CHAT",30,2,true);
    text(0,w-23,v.connected?"LOCAL IPC / CONNECTED":"LOCAL IPC / OFFLINE",21,v.connected?3:7,true);
    text(1,2,v.name+"  /  client PID "+std::to_string(v.pid)+"  <->  server PID "+std::to_string(v.serverPid),w-4,2);
    text(2,2,"AF_UNIX stream  |  "+v.socket,w-4,6);
    int side=w>=100?26:0,main=w-4-(side?side+1:0),top=4,body=h-10;
    panel(top,2,body,main,"#"+v.room+"  /  CONVERSATION");
    std::vector<Line> lines;
    for(const auto& e:v.entries) {
        if(!e.heading.empty()) lines.push_back({e.heading,e.color,true});
        wrap(lines,e.text,main-4,e.heading.empty()?e.color:2);
        lines.push_back({"",2,false});
    }
    int visible=body-2, maxScroll=std::max(0,int(lines.size())-visible);
    scroll_=std::clamp(scroll_,0,maxScroll);
    int start=std::max(0,int(lines.size())-visible-scroll_);
    for(int i=0;i<visible && start+i<int(lines.size());++i) text(top+1+i,4,lines[start+i].text,main-4,lines[start+i].color,lines[start+i].bold);
    if(side) {
        int x=main+3; panel(top,x,body,side,"ONLINE / "+std::to_string(v.users.size()));
        int y=top+1;
        for(const auto& u:v.users) {
            if(y+2>=top+body-1) break;
            auto name=u.value("name","");
            text(y++,x+2,(name==v.name?"> ":"  ")+name,side-4,name==v.name?3:2,true);
            text(y++,x+2,"  #"+u.value("room","")+" / "+std::to_string(u.value("pid",0)),side-4,6);
        }
        if(y+3<top+body-1) { text(++y,x+2,"ROOMS",side-4,1,true); for(const auto& room:v.rooms) {
            if(++y>=top+body-1) break;
            text(y,x+2,"#"+room.get<std::string>(),side-4,2);
        } }
    }
    text(h-6,3,(scroll_?"HISTORY / PgDn to return":"LIVE")+std::string("  |  sent ")+std::to_string(v.sent)+"  received "+std::to_string(v.received)+"  |  server ACK #"+std::to_string(v.lastAck),w-6,scroll_?4:3);
    panel(h-5,2,3,w-4,"MESSAGE / Enter to send");
    int available=w-10,startCol=0;
    for(size_t i=0;i<cursor_;++i) startCol+=cells(input_[i]);
    size_t begin=0; int hidden=0;
    while(startCol-hidden>=available && begin<cursor_) hidden+=cells(input_[begin++]);
    text(h-4,4,utf8(input_.substr(begin)),available,2);
    text(h-2,2,v.status,w-4,v.connected?6:4);
    text(h-1,2,"/help  /join room  /msg user text  /who  /stats  /quit   PgUp/PgDn history",w-4,1);
    curs_set(1); move(h-4,4+startCol-hidden); refresh();
}
}
