#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#include "thread.h"
#include "queue.h"

#define MAIN_TID -1
#define MAX_SEMAPHORES 100

/* Some error codes. */
typedef enum {ERR_THREAD_MAX, ERR_SEM_MAX} thread_err_t;
static thread_err_t thread_errno;

static int current_tid; /* Current thread id used by thread scheduler. */
static int tid_count = 0;   /* Where to insert the next thread in the control block table. */
static int sem_count = 0;   /* Where to insert the next semaphore in the table. */
static int quantum_ns = 1000; /* Is this a good initial value? I dunno */

/* This library at the moment supports 1000 threads and 100 semaphores. */
static thread_control_block *cb_table[MAX_THREADS];
static sem_t *sem_table[MAX_SEMAPHORES];

static ucontext_t uctx_main;
static Queue *run_queue;
static struct itimerval tval;

/* Utility function prototypes. */
void switch_thread();
void context_swap();

int thread_init()
{
    /* Initialize the run queue. */
    run_queue = queue_create();
}

int thread_create(char *threadname, void (*threadfunc)(), int stacksize)
{
    if (tid_count >= MAX_THREADS) 
    {   
        thread_errno = ERR_THREAD_MAX;   
        return thread_errno;
    }

    thread_control_block *cb = malloc(sizeof(thread_control_block));
    strncpy(cb->thread_name, threadname, THREAD_NAME_LEN);
    cb->thread_name[THREAD_NAME_LEN - 1] = '\0';
    
    /* Initialize ucontext_t */
    if (getcontext(&cb->context) == -1)
        printf("Error getting context...what the hell do i do now?\n");
    
    /* Allocate some stack space. */
    void *func_stack = malloc(stacksize);
    /* Create the thread context. */
    cb->context.uc_stack.ss_sp = func_stack;
    cb->context.uc_stack.ss_size = stacksize;
    cb->context.uc_link = &uctx_main;
    cb->func = threadfunc;
    makecontext(&cb->context, cb->func, 0);
    
    /* Set the state of the thread to RUNNABLE. */
    cb->state = RUNNABLE;
    
    /* Add this control block to the table. */
    cb_table[tid_count] = cb;
    
    /* Insert thread in the run queue. Apparently this is dependent on the system design (may not need to). */
    enqueue(run_queue, tid_count);
    
    tid_count++;
}

void thread_exit()
{
    /* I *think* that current_tid is safe to use. Never runs at same time
       as main context, which is the only thing that modifies the value. */
       
    /* If the thread is running then it shouldn't be in the run queue...by they way I've set it up. 
       No need to pop. Also, don't remove from the table. */
    
    /* Set the state to EXIT. */
    thread_control_block *cb = cb_table[current_tid];
    cb->state = EXIT;
    
    /* Perform a context swap back to the scheduler. */
    context_swap();
}

void run_threads()
{
    /* For now I'm setting this up so that no context switch is performed here...seems 
       difficult to manage timing issues. I don't like that every thread switch will
       have to have this check but it works for now. */
    current_tid = MAIN_TID;
    
    /* Configure and start the thread scheduler. */
    sigset(SIGALRM, switch_thread);
    set_quantum_size(100);
}

/* TODO: the assignment instructions say that n should be
   in nanoseconds...I should follow up by asking prof. I found
   the header code and suseconds_t (the type of tv_usec) appears 
   to just be an alias for an unsigned long. See
   http://www.sde.cs.titech.ac.jp/~gondow/dwarf2-xml/HTML-rxref/app/gcc-3.3.2/lib/gcc-lib/sparc-sun-solaris2.8/3.3.2/include/sys/types.h.html */
void set_quantum_size(int n)
{
    /* Set the timer value. */
    tval.it_interval.tv_sec = 0;
    tval.it_interval.tv_usec = n;
    tval.it_value.tv_sec = 0;
    tval.it_value.tv_usec = n;
    
    setitimer(ITIMER_REAL, &tval, NULL);
}

int create_semaphore(int value)
{
    /* Add a new semaphore to the table. */
    sem_t *sem = malloc(sizeof(sem_t));
    sem->init = value;
    sem->count = value;
    sem->wait_queue = queue_create();
    sem_table[sem_count] = sem;
    return sem_count++;
}

void semaphore_wait(int semaphore)
{
    if (semaphore >= sem_count) return; /* This semaphore does not exist... */
    
    sem_t *sem = sem_table[semaphore];
    sem->count--;
    
    if (sem->count < 0)
    {
        /* The calling thread needs to be put in a waiting queue. */
        thread_control_block *cb = cb_table[current_tid];
        cb->state = BLOCKED;
        enqueue(sem->wait_queue, current_tid);
        
        /* Perform a context switch to return control to the scheduler. TODO:
           disable signaling to the blocked thread. */
        context_swap();
    }
}

void semaphore_signal(int semaphore)
{
    if (semaphore >= sem_count) return; /* This semaphore does not exist... */
    
    sem_t *sem = sem_table[semaphore];
    sem->count++;
    
    if (sem->count <= 0)    /* i.e. If there are threads waiting on this semaphore...*/
    {
        /* No threads are waiting on this semaphore; nothing to do. Should not happen in principle. */
        if (queue_size(sem->wait_queue) <= 0) return;
        
        /* Pop a blocked thread from the wait queue of this semaphore. Re-enable signaling asap. */
        int tid = dequeue(sem->wait_queue);
        thread_control_block *cb = cb_table[tid];
        cb->state = RUNNABLE;
        /* Now add the thread to the run queue. */
        enqueue(run_queue, tid);
    }
}

void destroy_semaphore(int semaphore)
{
    if (semaphore >= sem_count) return; /* This semaphore does not exist... */
    
    sem_t *sem = sem_table[semaphore];
    
    /* Check to see if threads are waiting first. */
    if (queue_size(sem->wait_queue) > 0)
    {
        printf("Error: Cannot destroy semaphore %d because there are threads waiting on it.\n", semaphore);
        return;
    }
    else
    {
        if (sem->count != sem->init)
            printf("Warning: One or more threads have waited on semaphore %d but not signalled yet.\n", semaphore);
        queue_release(sem->wait_queue);
        free(sem);
        
        /* TODO: What do I do with index...maybe array isn't the best data structure. */
    }
}

void thread_state()
{   
    printf("Thread Name\tState\tRunning Time\n");
    
    int i;
    for (i = 0; i < 100; i++)
    {
        char state[20];
        thread_control_block cb = *cb_table[i];
        
        switch (cb.state)
        {
            case RUNNABLE:
                strcpy(state, "RUNNABLE");
                break;
            case RUNNING:
                strcpy(state, "RUNNING");
                break;
            case BLOCKED:
                strcpy(state, "BLOCKED");
                break;
            case EXIT:
                strcpy(state, "EXIT");
                break;
            default:
                strcpy(state, "ERROR-UNKNOWN");
        }
        
        printf("%s\t%s\t%.2f\n", cb.thread_name, state, 4.2);
    }
}

/*********************/
/* UTILITY FUNCTIONS */
/*********************/

/* A round-robin thread scheduler. */
void switch_thread()
{
    /* Pop thread from the run queue and insert current thread into back of queue. */
    if (queue_size(run_queue) <= 0) return; /* No threads left in run queue. */
    int tid = dequeue(run_queue);
    
    if (current_tid != MAIN_TID)
    {
        /* Change current thread's state to RUNNABLE. */   
        thread_control_block *cb_curr = cb_table[current_tid];
        cb_curr->state = RUNNABLE;
        enqueue(run_queue, current_tid);
    }
    
    /* Change next thread's state to RUNNING. */
    thread_control_block *cb = cb_table[tid];
    cb->state = RUNNING;
    current_tid = tid;
    
    /* We are ready to perform a context switch. */
    context_swap();
}

/* Wrapper function to perform a context switch and handle any errors. */
void context_swap()
{
    ucontext_t *uctx;
    if (getcontext(uctx) == -1)
        printf("Error getting context...what the hell do i do now?\n");
    if (swapcontext(uctx, uctx->uc_link) == -1)
        printf("Couldn't swap context, the fuck??\n");
}
