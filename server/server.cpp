#include "pch_msc.hpp"
#define TESTMAIN
#include <iostream>
#include <string>
#include <chrono>
#include <thread>

#include "libaquery.h"
#include "monetdb_conn.h"
#include "duckdb_conn.h"

constexpr  create_server_t get_server[] = {
    CreateNULLServer, 
    [](Context* cxt) -> void*{ return new MonetdbServer(cxt); }, 
    CreateNULLServer, 
    [](Context* cxt) -> void*{ return new DuckdbServer(cxt); }, 
    CreateNULLServer, 
};

#pragma region misc
#ifdef THREADING
#include "threading.h"
#endif

#ifdef _WIN32
#include "winhelper.h"
#else 
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <atomic>

struct SharedMemory
{
    std::atomic<bool> a;
    int hFileMap;
    void* pData;
    explicit SharedMemory(const char* fname) {
        hFileMap = open(fname, O_RDWR, 0);
        if (hFileMap != -1)
            pData = mmap(nullptr, 8, PROT_READ | PROT_WRITE, MAP_SHARED, hFileMap, 0);
        else 
            pData = nullptr;
    }
    void FreeMemoryMap() const {
        // automatically unmapped in posix
    }
};

#endif // _WIN32

#ifdef __AQUERY_ITC_USE_SEMPH__
A_Semaphore prompt{ true }, engine{ false };
#define PROMPT_ACQUIRE() prompt.acquire()
#define PROMPT_RELEASE() prompt.release()
#define ENGINE_ACQUIRE() engine.acquire()
#define ENGINE_RELEASE() engine.release()
#else
#define PROMPT_ACQUIRE() 
#define PROMPT_RELEASE() std::this_thread::sleep_for(std::chrono::nanoseconds(0))
#define ENGINE_ACQUIRE() 
#define ENGINE_RELEASE() 
#endif

typedef void (*module_init_fn)(Context*);

int n_recv = 0;
char** n_recvd = nullptr;

__AQEXPORT__(void) wait_engine() {
    PROMPT_ACQUIRE();
}

__AQEXPORT__(void) wake_engine() {
    ENGINE_RELEASE();
}

extern "C" void __DLLEXPORT__ receive_args(int argc, char**argv){
    n_recv = argc;
    n_recvd = argv;
}

enum BinaryInfo_t : int { // For ABI consistency between compiler
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

__AQEXPORT__(bool) 
have_hge() {
#if defined(__MONETDB_CONN_H__)
    return MonetdbServer::havehge();
#else
    return false;
#endif
}

Context* _g_cxt;

__AQEXPORT__(StoredProcedure)
get_procedure_ex(const char* name) {
    return get_procedure(_g_cxt, name);
}

void activate_callback_based_trigger(Context* context, const char* cmd)
{
	const char* query_name = cmd + 2;
	const char* action_name = query_name;
	while (*action_name++);
	if(auto q = get_procedure(context, query_name), 
			a = get_procedure(context, action_name); 
			q.name == nullptr || a.name == nullptr
	)
		printf("Warning: Invalid query or action name: %s %s", 
			query_name, action_name);
	else {
		auto query = AQ_DupObject(&q);
		auto action = AQ_DupObject(&a);

		context->ct_host->execute_trigger(query, action);
	}
}


// This function contains heap allocations, free after use
template<class String_T>
char* to_lpstr(const String_T& str) {
    auto ret = static_cast<char*>(malloc(str.size() + 1));
    memcpy(ret, str.c_str(), str.size());
    ret[str.size()] = '\0';
    return ret;
}
char* copy_lpstr(const char* str) {
    auto len = strlen(str);
    auto ret = static_cast<char*>(malloc(len + 1));
    memcpy(ret, str, len + 1);
    return ret;
}


#ifndef __AQ_USE_THREADEDGC__
void aq_init_gc(void *handle, Context* cxt)
{
    typedef void (*aq_gc_init_t) (Context*);
    if (handle && cxt){
        auto sym = dlsym(handle, "__AQ_Init_GC__");
        if(sym){
            ((aq_gc_init_t)sym)(cxt);
        }
    }
}
#else //__AQ_USE_THREADEDGC__
#define aq_init_gc(h, c) 
#endif //__AQ_USE_THREADEDGC__

void initialize_module(const char* module_name, void* module_handle, Context* cxt){
    auto _init_module = reinterpret_cast<module_init_fn>(dlsym(module_handle, "init_session"));
    if (_init_module) {
        _init_module(cxt);
    }
    else {
        printf("Warning: module %s have no session support.\n", module_name);
    }
}

#pragma endregion
int threaded_main(int argc, char** argv, Context* cxt){
    aq_timer timer;
    Config *cfg = reinterpret_cast<Config *>(argv[0]);
    std::unordered_map<std::string, void*> user_module_map;
    std::string pwd = (char*)(std::filesystem::current_path().string().c_str());
    char sep = std::filesystem::path::preferred_separator;
    pwd += sep;
    std::string procedure_root = (pwd + "procedures") + sep;
    std::string procedure_name = "";
    StoredProcedure current_procedure;
    vector_type<char *> recorded_queries;
    vector_type<void *> recorded_libraries;
    bool procedure_recording = false, 
         procedure_replaying = false;
    uint32_t procedure_module_cursor = 0;
    try {
        if (!std::filesystem::is_directory(procedure_root)) {
            if (std::filesystem::exists(procedure_root))
                std::filesystem::remove_all(procedure_root);
        }
        if (!std::filesystem::exists(procedure_root)) {
            std::filesystem::create_directory(procedure_root);
        }
    }
    catch (std::filesystem::filesystem_error& e) {
        printf("Failed to create directory %s: %s\n", procedure_root.c_str(), e.what());
    }
    
    if (cxt->module_function_maps == nullptr)
        cxt->module_function_maps = new std::unordered_map<std::string, void*>();
    auto module_fn_map = 
        static_cast<std::unordered_map<std::string, void*>*>(cxt->module_function_maps);
    
    auto buf_szs = cfg->buffer_sizes;
    void** buffers = (void**) malloc (sizeof(void*) * cfg->n_buffers);
    for (int i = 0; i < cfg->n_buffers; i++) 
        buffers[i] = static_cast<void *>(argv[i + 1]);
    cxt->buffers = buffers;
    cxt->cfg = cfg;
    cxt->n_buffers = cfg->n_buffers;
    cxt->sz_bufs = buf_szs;

    
    const auto& update_backend = [&cxt, &cfg](){
        auto backend_type = cfg->backend_type;
        if (backend_type == Backend_Type::BACKEND_AQuery) {
            backend_type = Backend_Type::BACKEND_MonetDB;
        }
        auto& curr_server = cxt->alt_server[backend_type];
        if (curr_server == nullptr) {
            curr_server = get_server[cfg->backend_type](cxt);
            cxt->alt_server[cfg->backend_type] = curr_server;
            static_cast<DataSource*>(curr_server)->exec("SELECT '**** WELCOME TO AQUERY++! ****';");
            puts(*(const char**)(static_cast<DataSource*>(curr_server)->getCol(0, types::Types<const char*>::getType())));
        }
        cxt->curr_server = curr_server;
    };
    update_backend();

    while(cfg->running){
        ENGINE_ACQUIRE();
        if (cfg->new_query) {
            cfg->stats.postproc_time = 0;
            cfg->stats.monet_time = 0;
start:

            void *handle = nullptr;
            void *user_module_handle = nullptr;
            if (cfg->backend_type == BACKEND_MonetDB||
                cfg->backend_type == BACKEND_DuckDB ||
                cfg->backend_type == BACKEND_AQuery 
            ) {
                update_backend();
                auto server = reinterpret_cast<DataSource*>(cxt->curr_server);
                if(n_recv > 0){
                    if (cfg->backend_type == BACKEND_AQuery || cfg->has_dll) {
                        const char* proc_name = "./dll.so";
                        std::string dll_path;
                        if (procedure_recording) {
                            dll_path = procedure_root + 
                                procedure_name + std::to_string(recorded_libraries.size) + ".so";
                            
                            try{
                                if (std::filesystem::exists(dll_path))
                                    std::filesystem::remove(dll_path);
                                std::filesystem::copy_file(proc_name, dll_path);
                            } catch(std::filesystem::filesystem_error& e){
                                puts(e.what());
                                dll_path = proc_name;
                            }
                            proc_name = dll_path.c_str();
                            //if(recorded_libraries.size)
                            recorded_queries.emplace_back(copy_lpstr("N"));
                        }
                        handle = dlopen(proc_name, RTLD_NOW);
                        aq_init_gc(handle, cxt);
                        if (procedure_recording) {
                            recorded_libraries.emplace_back(handle);
                        }
                    }
                    for (const auto& module : user_module_map){
                        initialize_module(module.first.c_str(), module.second, cxt);
                    }
                    cxt->init_session();
                    for(int i = 0; i < n_recv; ++i)
                    {
                        printf("%s, %d\n", n_recvd[i], n_recvd[i][0] == 'Q');
                        switch(n_recvd[i][0]){
                        case 'Q': // SQL query for monetdbe
                            {
                                if(procedure_recording){
                                    recorded_queries.emplace_back(copy_lpstr(n_recvd[i]));
                                }
                                timer.reset();
                                server->exec(n_recvd[i] + 1);
                                cfg->stats.monet_time += timer.elapsed();
                                printf("Exec Q%d: %s", i, n_recvd[i]);
                            }
                            break;
                        case 'P': // Postprocessing procedure 
                            if(handle && !server->haserror()) {
                                printf("Server load/exec generated cpp: %s", n_recvd[i]);
                                if (procedure_recording) {
                                    recorded_queries.emplace_back(copy_lpstr(n_recvd[i]));
                                }
                                code_snippet c = reinterpret_cast<code_snippet>(dlsym(handle, n_recvd[i]+1));
                                printf("%p", dlsym(handle, n_recvd[i] + 1));
                                timer.reset();
                                c(cxt);
                                cfg->stats.postproc_time += timer.elapsed();
                            }
                            break;
                        case 'M': // Load Module
                            {
                                auto mname = n_recvd[i] + 1;
                                user_module_handle = dlopen(mname, RTLD_NOW);
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
                        case 'O':
                            {
                                if(!server->haserror()){
                                    if (procedure_recording){
                                        char* buf = (char*) malloc (sizeof(char) * 6);
                                        memcpy(buf, n_recvd[i], 5);
                                        buf[5] = '\0';
                                        recorded_queries.emplace_back(buf);
                                    }
                                    uint32_t limit;
                                    memcpy(&limit, n_recvd[i] + 1, sizeof(uint32_t));
                                    if (limit == 0)
                                        continue;
                                    timer.reset();
                                    print_monetdb_results(server, " ", "\n", limit);        
                                    cfg->stats.postproc_time += timer.elapsed();
                                }
                            }
                            break;
                        case 'U': // Unload Module
                            {
                                auto mname = n_recvd[i] + 1;
                                auto it = user_module_map.find(mname);
                                if (user_module_handle == it->second)
                                    user_module_handle = nullptr;
                                dlclose(it->second);
                                user_module_map.erase(it);
                            }
                            break;
                        case 'N':
                            {
                                if(procedure_module_cursor < current_procedure.postproc_modules)
                                    handle = current_procedure.__rt_loaded_modules[procedure_module_cursor++];
                                printf("Load %i = %p\n", procedure_module_cursor, handle);
                            }
                            break;
                        case 'R': //recorded procedure
                            {
                                auto proc_name = n_recvd[i] + 2;
                                proc_name = *proc_name?proc_name : proc_name + 1;
                                puts(proc_name);
                                const auto& load_modules = [&](StoredProcedure &p) {
                                    if (!p.__rt_loaded_modules){
                                        p.__rt_loaded_modules = static_cast<void**>(
                                            malloc(sizeof(void*) * p.postproc_modules));
                                        for(uint32_t j = 0; j < p.postproc_modules; ++j){
                                            auto pj = dlopen((procedure_root + p.name + std::to_string(j) + ".so").c_str(), RTLD_NOW);
                                            if (pj == nullptr){
                                                printf("Error: failed to load module %s\n", p.name);
                                                return true;
                                            }
                                            aq_init_gc(pj, cxt);

                                            p.__rt_loaded_modules[j] = pj;
                                        }
                                    }
                                    return false;
                                };
                                const auto& save_proc_tofile = [&](const StoredProcedure& p)  {
                                    auto config_name = procedure_root + procedure_name + ".aqp";
                                    auto fp = fopen(config_name.c_str(), "wb");
                                    if (fp == nullptr){
                                        printf("Error: failed to open file %s\n", config_name.c_str());
                                        return true;
                                    }
                                    fwrite(&p.cnt, sizeof(p.cnt), 1, fp);
                                    fwrite(&p.postproc_modules, sizeof(p.postproc_modules), 1, fp);
                                    for(uint32_t j = 0; j < p.cnt; ++j){
                                        auto current_query = p.queries[j];
                                        auto len_query = strlen(current_query);
                                        fwrite(current_query, len_query + 1, 1, fp);
                                    }
                                    fclose(fp);
                                    return false;
                                };
                                const auto& load_proc_fromfile = [&](StoredProcedure& p)  {
                                    auto config_name = procedure_root + p.name + ".aqp";
                                    puts(p.name);
                                    auto fp = fopen(config_name.c_str(), "rb");
                                    if(fp == nullptr){
                                        puts("ERROR: Procedure not found on disk.");
                                        return true;
                                    }
                                    fread(&p.cnt, sizeof(p.cnt), 1, fp);
                                    fread(&p.postproc_modules, sizeof(p.postproc_modules), 1, fp);
                                    auto offset_now = ftell(fp);
                                    fseek(fp, 0, SEEK_END);
                                    auto queries_size = ftell(fp) - offset_now;
                                    fseek(fp, offset_now, SEEK_SET);

                                    p.queries = static_cast<char**>(malloc(sizeof(char*) * p.cnt));
                                    p.queries[0] = static_cast<char*>(malloc(sizeof(char) * queries_size));
                                    fread(p.queries[0], 1, queries_size, fp);

                                    for(uint32_t j = 1; j < p.cnt; ++j){
                                        p.queries[j] = p.queries[j-1];
                                        while(*(p.queries[j]) != '\0')
                                            ++p.queries[j];
                                        ++p.queries[j];
                                        puts(p.queries[j-1]);
                                    }
                                    fclose(fp);
                                    p.__rt_loaded_modules = nullptr;
                                    return load_modules(p);
                                };
                                switch(n_recvd[i][1]){
                                    case '\0':
                                        current_procedure.name = copy_lpstr(proc_name);
                                        AQ_ZeroMemory(current_procedure);
                                        procedure_recording = true;
                                        procedure_name = proc_name;
                                    break;
                                    case 'T':
                                        current_procedure.queries = recorded_queries.container;
                                        current_procedure.cnt = recorded_queries.size;
                                        current_procedure.name = copy_lpstr(proc_name);
                                        current_procedure.postproc_modules = recorded_libraries.size;
                                        current_procedure.__rt_loaded_modules = recorded_libraries.container;
                                        AQ_ZeroMemory(recorded_queries);
                                        AQ_ZeroMemory(recorded_libraries);
                                        procedure_recording = false;
                                        save_proc_tofile(current_procedure);
                                        cxt->stored_proc.insert_or_assign(procedure_name, current_procedure);
                                        procedure_name = "";
                                    break;
                                    case 'E': // execute procedure
                                    {
                                        procedure_module_cursor = 0;
                                        auto _proc = cxt->stored_proc.find(proc_name);
                                        if (_proc == cxt->stored_proc.end()){
                                            printf("Procedure %s not found. Trying load from disk.\n", proc_name);
                                            current_procedure.name = copy_lpstr(proc_name);
                                            if (!load_proc_fromfile(current_procedure)){
                                                cxt->stored_proc.insert_or_assign(proc_name, current_procedure);
                                            }
                                            else {
                                                continue;
                                            }
                                        }
                                        else{
                                            current_procedure = _proc->second;
                                        }
                                        n_recv = current_procedure.cnt;
                                        n_recvd = current_procedure.queries;
                                        load_modules(current_procedure);
                                        procedure_replaying = true;
                                        goto start; // yes, I know, refactor later!!
                                    }
                                    break;
                                    case 'D': // delete procedure
                                    break;
                                    case 'S': // save procedure
                                    break;
                                    case 'L': // load procedure
                                    current_procedure.name = copy_lpstr(proc_name);
                                    if (!load_proc_fromfile(current_procedure)) {
                                        cxt->stored_proc.insert_or_assign(proc_name, current_procedure);
                                    }
                                    break;
                                    case 'd': // display all procedures
                                    for(const auto& p : cxt->stored_proc){
                                        printf("Procedure: %s, %d queries, %d modules:\n", p.first.c_str(), 
                                            p.second.cnt, p.second.postproc_modules);
                                        for(uint32_t j = 0; j < p.second.cnt; ++j){
                                            printf("\tQuery %d: %s\n", j, p.second.queries[j]);
                                        }
                                        puts("");
                                    }
                                    break;
                                }
                            }
                            break;
                        case 'T': // triggers
                        {
                            puts(n_recvd[i]);
                            switch(n_recvd[i][1]){
                                case 'I': // register interval based trigger
                                {
                                    const char* action_name = n_recvd[i] + 2;
                                    while(*action_name++);
                                    if(auto p = get_procedure(cxt, action_name); p.name == nullptr) 
                                        printf("Invalid action name: %s\n", action_name);
                                    else {
                                        auto action = AQ_DupObject(&p);
                                        const char* interval = action_name;
                                        while(*interval++);
                                        const auto i_interval = getInt<uint32_t>(interval);
                                        cxt->it_host->add_trigger(n_recvd[i] + 2, action, i_interval);
                                    }
                                }
                                break;
                                case 'C' : //register callback based trigger
                                {
                                    const char* trigger_name = n_recvd[i] + 2;
                                    const char* table_name = trigger_name;
                                    while(*table_name++);
                                    const char* query_name = table_name;
                                    while(*query_name++);
                                    const char* action_name = query_name;
                                    while(*action_name++);
                                    cxt->ct_host->add_trigger(trigger_name, table_name, query_name, action_name);
                                }
                                break;
                                case 'A': // activate callback based trigger
                                activate_callback_based_trigger(cxt, n_recvd[i]);
                                break;
                                case 'N':
                                cxt->ct_host->execute_trigger(n_recvd[i] + 2);
                                break;
                                case 'R': // remove trigger
                                {
                                    cxt->it_host->remove_trigger(n_recvd[i] + 2);
                                }
                                break;
                                default:
                                printf("Corrupted message from prompt: %s\n", n_recvd[i]);
                                break;
                            }
                        }
                        break;
                        case 'C': //Caching 
                        {
                            char* cached_table = n_recvd[i] + 1;
                            char *lazy = (cached_table + 1);
                            cached_table = AQ_DupString(cached_table);
                            while(*lazy++);
                            // get schema
                            int* n_cols = reinterpret_cast<int *>(lazy + 2);
                            char* col_schema = reinterpret_cast<char *>(n_cols + 1);
                            TableInfo<void> *tbl = new TableInfo<void>;
                            tbl->name = cached_table;//AQ_DupString(cached_table);
                            tbl->n_cols = *n_cols;
                            
                            for (int i = 0; i < *n_cols; ++i) {
                                char* col_name = col_schema;
                                char* mem_coltype = col_name + 1;
                                while(*mem_coltype++);
                                int coltype = *(reinterpret_cast<int*>(mem_coltype));
                                // 
                                tbl->colrefs[i].name = AQ_DupString(col_name);
                                tbl->colrefs[i].ty = static_cast<types::Type_t>(coltype);    
                            }
                            
                            server->getDSTable(cached_table, tbl);
                            cxt->tables[cached_table] = tbl;
                            // server->exec( (
                            //     std::string("SELECT * FROM ") + cached_table + std::string(";")
                            // ).c_str() );
                            // server->getCol()
                            // free(cached_table);
                            break;
                        }
                        }
                    }
                    
                    printf("%lld, %lld", cfg->stats.monet_time, cfg->stats.postproc_time);
                    cxt->end_session();
                    n_recv = 0;
                }
                if (server->last_error != nullptr) {
                    printf("Monetdbe Error: %s\n", server->last_error);
                    server->last_error = nullptr;
                    //goto finalize;
                }   
            }
            
            // puts(cfg->has_dll ? "true" : "false");
            // if (cfg->backend_type == BACKEND_AQuery) {
            //     handle = dlopen("./dll.so", RTLD_NOW);
            //     code_snippet c = reinterpret_cast<code_snippet>(dlsym(handle, "dllmain"));
            //     c(cxt);
            // }
            if (handle && 
                !procedure_replaying && !procedure_recording) { 
                printf("Destroy %p\n", handle);
                dlclose(handle);
                handle = nullptr;
            }
            procedure_replaying = false;
            cfg->new_query = 0;
        }
        //puts(cfg->running? "true": "false");
//finalize:
        PROMPT_RELEASE();
    }
    
    return 0;
}
#ifdef __AQ_BUILD_LAUNCHER__
int main(int argc, char** argv){
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
#endif

extern "C" int __DLLEXPORT__ dllmain(int argc, char** argv) {
   // puts("running");
   Context* cxt = new Context();
   _g_cxt = cxt;
   cxt->aquery_root_path = to_lpstr(std::filesystem::current_path().string());
   // cxt->log("%d %s\n", argc, argv[1]);

#ifdef THREADING
    auto tp = new ThreadPool();
    cxt->thread_pool = tp;
    cxt->it_host = new IntervalBasedTriggerHost(tp, cxt);
    cxt->ct_host = new CallbackBasedTriggerHost(tp, cxt);
#endif
    
   const char* shmname;
   if (argc < 0)
        return threaded_main(argc, argv, cxt);
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
       if(ready) {
           cxt->log("running: %s\n", running? "true":"false");
           cxt->log("ready: %s\n", ready? "true":"false");
           void* handle = dlopen("./dll.so", RTLD_NOW);
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

