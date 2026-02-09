#include <csignal>   // signal
#include <cstdlib>   // exit
#include <atomic>
#include <iostream>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "fitcache_timer.h"
#include "fitcache_comm.h"
#include "fitcache_data_mover_internal.h"

#define FitCache_SERVER 1

extern "C" {
#include "fitcache_logging.h"
}

#include "fitcache_cache_policy.h"

__thread bool tl_disable_redirect = false;
uint32_t fitcache_server_count = 0;
static std::atomic<bool> g_running{true};

struct fitcache_lookup_arg {
	hg_class_t *hg_class;
	hg_context_t *context;
	hg_id_t id;
	hg_addr_t addr;
};

void signal_exit(int signum)
{
    std::cerr << "\n[fitcache_server] Caught signal " << signum
              << ", exiting...\n";

    g_running = false;

    std::exit(0);
}

int fitcache_start_comm_server(void)
{
    FitCache_TIMING("HvacServer_start_comm_server_Total");
    HG_Set_log_level("DEBUG");

    // !--- BEGIN: Write PID to file with server ID ---
    pid_t current_pid = getpid();
    const char* pid_file_path = "/tmp/fitcache_server.pid";
    
    FILE* pid_f = fopen(pid_file_path, "w");
    if (pid_f) {
        fprintf(pid_f, "%d\n", current_pid);
        fclose(pid_f);
        L4C_INFO("Server (PID: %d) wrote PID to %s", current_pid, pid_file_path);
    } else {
        L4C_ERR("Server (PID: %d) FAILED to write PID to %s. errno: %d", current_pid, pid_file_path, errno);
        // Decide if this is a fatal error
    }
    // !--- END: Write PID to file ---

    /* Start the data mover before anything else */
    pthread_t fitcache_data_mover_tid;
    {
        // FitCache_TIMING("HvacServer_data_mover_thread_create");
        if (pthread_create(&fitcache_data_mover_tid, NULL, fitcache_data_mover_fn, NULL) != 0){
            L4C_FATAL("Failed to initialized mecury progress thread\n");
        }
    }

    L4C_INFO("Starting FitCache comm server");
    /* True means we're a listener */
    fitcache_init_comm(true);

    /* Post our address */
    fitcache_comm_list_addr();

    /* Register basic RPC */
    fitcache_rpc_register_server();
    fitcache_open_rpc_register_server();
    fitcache_close_rpc_register_server();
    fitcache_seek_rpc_register_server();

    // // ! Register the trigger RPC
    // fitcache_trigger_srv_print_stats_rpc_register(); 

    while (g_running)
        sleep(1);

    return EXIT_SUCCESS;
}


int main(int argc, char **argv)
{
    std::signal(SIGINT,  signal_exit);  
    std::signal(SIGTERM, signal_exit);  
    std::signal(SIGKILL, signal_exit);  
    FitCache_TIMING("HvacServer_main_Total");
    
    int l_error = 0;

    // Quick and dirty for prototype
    if (argc < 2)
    {
        fprintf(stderr, "Please supply server count\n");
        exit(-1);
    }

    fitcache_server_count = atoi(argv[1]);

    fitcache_init_logging();
    L4C_INFO("Server process starting up");
    //int *pid_server = NULL;
    //PMI_Get_rank(pid_server);
    //L4C_INFO("PMI Rank ID: %d \n", pid_server);  
    fitcache_start_comm_server();
    L4C_INFO("FitCache Server process shutting down");
    return (l_error);
}
