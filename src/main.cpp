#include "protocol.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    std::string mode="client",name,path=relay::default_socket(); bool plain=false,json=false;
    try {
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="server" || arg=="client" || arg=="check") mode=arg;
            else if((arg=="--name" || arg=="--socket") && i+1<argc) { auto value=std::string(argv[++i]); if(arg=="--name") name=value; else path=value; }
            else if(arg=="--plain") plain=true;
            else if(arg=="--json") json=true;
            else if(arg=="--help" || arg=="-h") {
                std::cout<<"RELAY - Inter-Process Chat Application Using IPC\n\n"
                         <<"  ./chat server [--socket PATH]             Start the independent broker process\n"
                         <<"  ./chat --name Alice [--socket PATH]       Open the terminal chat client\n"
                         <<"  ./chat --name Alice --plain               Plain text interface\n"
                         <<"  ./chat --name Alice --json                JSON events for scripts/testing\n"
                         <<"  ./chat check [--socket PATH]              Check that a socket accepts connections\n\n"
                         <<"Commands: /join room, /msg NAME text, /who, /stats, /help, /clear, /quit\n"
                         <<"Linux / Ubuntu WSL. IPC uses local Unix domain sockets, not TCP or the internet.\n"; return 0;
            } else throw std::runtime_error("Unknown or incomplete argument: "+arg+" (see --help)");
        }
        if(mode=="server") return relay::server(path);
        if(mode=="check") { auto socket=relay::connect_socket(path); std::cout<<"Socket is reachable\n"; return 0; }
        if(name.empty()) throw std::runtime_error("Choose a name: ./chat --name Alice");
        return relay::client(path,name,plain,json);
    } catch(const std::exception& e) { std::cerr<<"Error: "<<e.what()<<'\n'; return 1; }
}
