#include "pch_msc.hpp"

#include "../csv.h"
#include <iostream>
#include <string>
#include <chrono>

#include "libaquery.h"
#include "monetdb_conn.h"
#ifdef THREADING
#include "threading.h"
#endif
#ifdef _WIN32
#include "winhelper.h"
#else 
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
struct SharedMemory
{
    int hFileMap;
    void* pData;
    SharedMemory(const char* fname) {
        hFileMap = open(fname, O_RDWR, 0);
        if (hFileMap != -1)
            pData = mmap(NULL, 8, PROT_READ | PROT_WRITE, MAP_SHARED, hFileMap, 0);
        else 
            pData = 0;
    }
    void FreeMemoryMap() {

    }
};
#endif

#include "aggregations.h"
typedef int (*code_snippet)(void*);
typedef void (*module_init_fn)(Context*);

int test_main();

int n_recv = 0;
char** n_recvd = nullptr;

extern "C" void __DLLEXPORT__ receive_args(int argc, char**argv){
    n_recv = argc;
    n_recvd = argv;
}

enum BinaryInfo_t {
	MSVC, MSYS, GCC, CLANG, AppleClang
};

extern "C" int __DLLEXPORT__ binary_info() {
#if defined(_MSC_VER) && !defined (__llvm__)
	return MSVC;
#elif defined(__CYGWIN__) || defined(__MINGW32__) || defined(__MINGW64__)
	return MSYS;
#elif defined(__clang__)
	return CLANG;
#elif defined(__GNUC__)
    return GCC;
#endif
}

__AQEXPORT__(bool) have_hge(){
#if defined(__MONETDB_CONN_H__)
    return Server::havehge();
#else
    return false;
#endif
}

Context::Context() {
    current.memory_map = new std::unordered_map<void*, deallocator_t>;
    init_session();
}

Context::~Context() {
    auto memmap = (std::unordered_map<void*, deallocator_t>*) this->current.memory_map;
    delete memmap;
}

void Context::init_session(){
    if (log_level == LOG_INFO){
        memset(&(this->current.stats), 0, sizeof(Session::Statistic));
    }
    auto memmap = (std::unordered_map<void*, deallocator_t>*) this->current.memory_map;
    memmap->clear();
}

void Context::end_session(){
    auto memmap = (std::unordered_map<void*, deallocator_t>*) this->current.memory_map;
    for (auto& mem : *memmap) {
        mem.second(mem.first);
    }
    memmap->clear();
}

void* Context::get_module_function(const char* fname){
    auto fmap = static_cast<std::unordered_map<std::string, void*>*>
        (this->module_function_maps);
    // printf("%p\n", fmap->find("mydiv")->second);
    //  for (const auto& [key, value] : *fmap){
    //      printf("%s %p\n", key.c_str(), value);
    //  }
    auto ret = fmap->find(fname);
    return ret == fmap->end() ? nullptr : ret->second;
}

void initialize_module(const char* module_name, void* module_handle, Context* cxt){
    auto _init_module = reinterpret_cast<module_init_fn>(dlsym(module_handle, "init_session"));
    if (_init_module) {
        _init_module(cxt);
    }
    else {
        printf("Warning: module %s have no session support.\n", module_name);
    }
}

int dll_main(int argc, char** argv, Context* cxt){
    Config *cfg = reinterpret_cast<Config *>(argv[0]);
    std::unordered_map<std::string, void*> user_module_map;
    if (cxt->module_function_maps == 0)
        cxt->module_function_maps = new std::unordered_map<std::string, void*>();
    auto module_fn_map = 
        static_cast<std::unordered_map<std::string, void*>*>(cxt->module_function_maps);
    
    auto buf_szs = cfg->buffer_sizes;
    void** buffers = (void**)malloc(sizeof(void*) * cfg->n_buffers);
    for (int i = 0; i < cfg->n_buffers; i++) 
        buffers[i] = static_cast<void *>(argv[i + 1]);

    cxt->buffers = buffers;
    cxt->cfg = cfg;
    cxt->n_buffers = cfg->n_buffers;
    cxt->sz_bufs = buf_szs;
    cxt->alt_server = NULL;

    while(cfg->running){
        if (cfg->new_query) {
            void *handle = 0;
            void *user_module_handle = 0;
            if (cfg->backend_type == BACKEND_MonetDB){
                if (cxt->alt_server == 0)
                    cxt->alt_server = new Server(cxt);
                Server* server = reinterpret_cast<Server*>(cxt->alt_server);
                if(n_recv > 0){
                    if (cfg->backend_type == BACKEND_AQuery || cfg->has_dll) {
                        handle = dlopen("./dll.so", RTLD_LAZY);
                    }
                    for (const auto& module : user_module_map){
                        initialize_module(module.first.c_str(), module.second, cxt);
                    }
                    cxt->init_session();
                    for(int i = 0; i < n_recv; ++i)
                    {
                        //printf("%s, %d\n", n_recvd[i], n_recvd[i][0] == 'Q');
                        switch(n_recvd[i][0]){
                        case 'Q': // SQL query for monetdbe
                            {
                                // coulson notes: the add 1 is for skipping the "Q" character
                                server->exec(n_recvd[i] + 1);
                                printf("Sever Exec monetdb Q%d: %s", i, n_recvd[i]);
                            }
                            break;
                        case 'P': // Postprocessing procedure 
                            printf("Sever ostprocessing: %s", n_recvd[i]);
                            if(handle && !server->haserror()) {
                                code_snippet c = reinterpret_cast<code_snippet>(dlsym(handle, n_recvd[i]+1));
                                c(cxt);
                            }
                            break;
                        case 'M': // Load Module
                            {   
                                printf("Sever load module: %s", n_recvd[i]);
                                auto mname = n_recvd[i] + 1;
                                user_module_handle = dlopen(mname, RTLD_LAZY);
                                //getlasterror
                                
                                if (!user_module_handle)
#ifndef _WIN32
                                    puts(dlerror());
#else
                                    printf("Fatal Error: Module %s failed to load with error code %d.\n", mname, dlerror());
#endif
                                user_module_map[mname] = user_module_handle;
                                initialize_module(mname, user_module_handle, cxt);
                            }
                            break;
                        case 'F': // Register Function in Module
                            {
                                auto fname = n_recvd[i] + 1;
                                //printf("F:: %s: %p, %p\n", fname, user_module_handle, dlsym(user_module_handle, fname));
                                module_fn_map->insert_or_assign(fname, dlsym(user_module_handle, fname));
                                //printf("F::: %p\n", module_fn_map->find("mydiv") != module_fn_map->end() ? module_fn_map->find("mydiv")->second : nullptr);
                            }
                            break;
                        case 'U': // Unload Module
                            {
                                auto mname = n_recvd[i] + 1;
                                auto it = user_module_map.find(mname);
                                if (user_module_handle == it->second)
                                    user_module_handle = 0;
                                dlclose(it->second);
                                user_module_map.erase(it);
                            }
                            break;
                        }
                    }
                    if(handle) {
                        dlclose(handle);
                        handle = 0;
                    }
                    cxt->end_session();
                    n_recv = 0;
                }
                if(server->last_error == nullptr){
                    // TODO: Add feedback to prompt.
                }   
                else{
                    server->last_error = nullptr;
                    continue;
                } 
            }
            
            // puts(cfg->has_dll ? "true" : "false");
            if (cfg->backend_type == BACKEND_AQuery) {
                handle = dlopen("./dll.so", RTLD_LAZY);
                code_snippet c = reinterpret_cast<code_snippet>(dlsym(handle, "dllmain"));
                c(cxt);
            }
            if (handle) dlclose(handle);
            cfg->new_query = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}

int launcher(int argc, char** argv){
#ifdef _WIN32
    constexpr char sep = '\\';
#else
    constexpr char sep = '/';
#endif
    std::string str = " ";
    std::string pwd = "";
    if (argc > 0)
        pwd = argv[0];
    
    auto pos = pwd.find_last_of(sep);
    if (pos == std::string::npos)
        pos = 0;
    pwd = pwd.substr(0, pos);
    for (int i = 1; i < argc; i++){
        str += argv[i];
        str += " ";
    }
    str = std::string("cd ") + pwd + std::string("&& python3 ./prompt.py ") + str;
    return system(str.c_str());
}

extern "C" int __DLLEXPORT__ main(int argc, char** argv) {
#ifdef __AQ_BUILD_LAUNCHER__
   return launcher(argc, argv);
#endif
   puts("running");
   Context* cxt = new Context();
   cxt->log("%d %s\n", argc, argv[1]);

#ifdef THREADING
    auto tp = new ThreadPool();
    cxt->thread_pool = tp;
#endif
    
   const char* shmname;
   if (argc < 0)
        return dll_main(argc, argv, cxt);
   else if (argc <= 1)
        return test_main();
   else
       shmname = argv[1];
   SharedMemory shm = SharedMemory(shmname);
   if (!shm.pData)
       return 1;
   bool &running = static_cast<bool*>(shm.pData)[0], 
       &ready = static_cast<bool*>(shm.pData)[1];
   using namespace std::chrono_literals;
   cxt->log("running: %s\n", running? "true":"false");
   cxt->log("ready: %s\n", ready? "true":"false");
   while (running) {
       std::this_thread::sleep_for(1ms);
       if(ready){
           cxt->log("running: %s\n", running? "true":"false");
           cxt->log("ready: %s\n", ready? "true":"false");
           void* handle = dlopen("./dll.so", RTLD_LAZY);
           cxt->log("handle: %p\n", handle);
           if (handle) {
               cxt->log("inner\n");
               code_snippet c = reinterpret_cast<code_snippet>(dlsym(handle, "dllmain"));
               cxt->log("routine: %p\n", c);
               if (c) {
                   cxt->log("inner\n");
                   cxt->err("return: %d\n", c(cxt));
               }
           }
           ready = false;
       }
   }
   shm.FreeMemoryMap();
   return 0;
}

#include "utils.h"
#include "table_ext_monetdb.hpp"
int test_main()
{
    Context* cxt = new Context();
    if (cxt->alt_server == 0)
        cxt->alt_server = new Server(cxt);
    Server* server = reinterpret_cast<Server*>(cxt->alt_server);

    const char* qs[]= {
        "QCREATE TABLE trade(stocksymbol INT, time INT, quantity INT, price INT);",
        "QCOPY OFFSET 2 INTO trade FROM  'w:/gg/AQuery++/data/trade_numerical.csv'  ON SERVER    USING DELIMITERS  ',';",
        "QSELECT stocksymbol, (SUM((quantity * price)) / SUM(quantity)) AS weighted_average  FROM trade GROUP BY stocksymbol  ;",
        "Pdll_5lYrMY",
        "QSELECT stocksymbol, price  FROM trade ORDER BY time  ;",
        "Pdll_4Sg6Ri",
        "QSELECT stocksymbol, quantity, price  FROM trade ORDER BY time  ;",
        "Pdll_5h4kL2",
        "QSELECT stocksymbol, price  FROM trade ORDER BY time  ;",
        "Pdll_7tEWCO",
        "QSELECT query_c.weighted_moving_averages, query_c.stocksymbol  FROM query_c;",
        "Pdll_7FCPnF"
    };
    n_recv = sizeof(qs)/(sizeof (char*));
	n_recvd = const_cast<char**>(qs);
            void* handle = 0;
                    handle = dlopen("./dll.so", RTLD_LAZY);
                    cxt->init_session();
                    for (int i = 0; i < n_recv; ++i)
                    {
                        //printf("%s, %d\n", n_recvd[i], n_recvd[i][0] == 'Q');
                        switch (n_recvd[i][0]) {
                        case 'Q': // SQL query for monetdbe
                        {
                            server->exec(n_recvd[i] + 1);
                            printf("Exec Q%d: %s\n", i, n_recvd[i]);
                        }
                        break;
                        case 'P': // Postprocessing procedure 
                            if (handle && !server->haserror()) {
                                code_snippet c = reinterpret_cast<code_snippet>(dlsym(handle, n_recvd[i] + 1));
                                c(cxt);
                            }
                            break;
                        }
                    }
                    n_recv = 0;

    //static_assert(std::is_same_v<decltype(fill_integer_array<5, 1>()), std::integer_sequence<bool, 1,1,1,1,1>>, "");
    
    return 0;
}

