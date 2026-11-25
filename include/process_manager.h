#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <time.h>

#define MAX_CHILDREN   16  
#define MAX_RESTARTS   3   
#define MAX_ARGS       16  
#define PROC_NAME_LEN  32
#define PROC_PATH_LEN  128
#define PROC_ARG_LEN   64

typedef enum {
    PROC_RUNNING,               
    PROC_EXITED_NORMAL,         
    PROC_TERMINATED_ERROR,      
    PROC_RESTART_LIMIT_REACHED  
} ProcessState;

typedef struct {
    pid_t     pid;                             
    char      name[PROC_NAME_LEN];             
    char      executable[PROC_PATH_LEN];       
    char      args[MAX_ARGS][PROC_ARG_LEN];    
    int       arg_count;                       
    ProcessState state;
    int       restart_count;                   
    time_t    last_start_time;                 
} ChildProcess;

int init_process_manager(int interval_seconds);

int add_and_start_process(const char *name, const char *path, char *const argv[]);

void handle_child_terminations(void);

void periodic_process_check(void);

void set_shutdown_mode(int enabled);

void send_signal_to_all_children(int sig);

int count_running_children(void);

const ChildProcess *get_process_list(int *count);

const char *process_state_to_string(ProcessState state);

#endif