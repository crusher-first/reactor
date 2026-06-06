#include "server.hpp"
#include <iostream>
#include <csignal>
#include <getopt.h>
using namespace high_perf;
class EpollServer;
static EpollServer* gs = nullptr;
static void sh(int) { if (gs) gs->stop(); }
int main(int argc, char* argv[]) {
    ServerConfig cfg;
    static struct option lo[] = {
        {"port",1,0,'p'},{"root",1,0,'r'},{"cache-size",1,0,'c'},{"backlog",1,0,'b'},{"help",0,0,'h'},{0,0,0,0}
    };
    int o, i=0;
    while ((o = getopt_long(argc,argv,"p:r:c:b:h",lo,&i)) != -1) {
        switch(o) { case 'p': cfg.port=std::stoi(optarg); break;
            case 'r': cfg.root_dir=optarg; break;
            case 'c': cfg.file_cache_size=std::stoul(optarg); break;
            case 'b': cfg.backlog=std::stoi(optarg); break;
            case 'h': std::cout << "Usage: " << argv[0] << " -p port -r root\n"; return 0;
            default: return 1; }
    }
    std::cout << "[epoll] Port:" << cfg.port << " Root:" << cfg.root_dir << "\n";
    EpollServer srv(cfg); gs = &srv;
    signal(SIGINT,sh); signal(SIGTERM,sh);
    srv.start();
    return 0;
}