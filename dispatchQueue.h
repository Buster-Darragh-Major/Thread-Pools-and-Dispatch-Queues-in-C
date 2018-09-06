/* 
 * File:   dispatchQueue.h
 * Author: robert
 *
 * Modified by: bmaj406
 */

#ifndef DISPATCHQUEUE_H
#define	DISPATCHQUEUE_H
#define _GNU_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
    
#define error_exit(MESSAGE)     perror(MESSAGE), exit(EXIT_FAILURE)

    typedef enum { // whether dispatching a task synchronously or asynchronously
        ASYNC, SYNC
    } task_dispatch_type_t;
    
    typedef enum { // The type of dispatch queue.
        CONCURRENT, SERIAL
    } queue_type_t;

    typedef struct task {
        char name[64];              // to identify it when debugging
        void (*work)(void *);       // the function to perform
        void *params;               // parameters to pass to the function
        task_dispatch_type_t type;  // asynchronous or synchronous
        struct task *next_task;      // the text task in the queue of tasks (struct because typedef not "finished" yet)
        bool complete;
    } task_t;

    typedef struct dispatch_queue_t dispatch_queue_t; // the dispatch queue type
    typedef struct dispatch_queue_thread_t dispatch_queue_thread_t; // the dispatch queue thread type
    // The reason for these is really weird ^, just use the [ typedef struct foo {...} foo; ] in the furure

    typedef struct thread_pool {
        dispatch_queue_thread_t *top_thread;
        dispatch_queue_thread_t *delegate_thread;
    } thread_pool_t;

    struct dispatch_queue_thread_t {
        dispatch_queue_t *queue;// the queue this thread is associated with
        pthread_t pthread;       // the thread which runs the task
        sem_t *thread_semaphore; // the semaphore the thread waits on until a task is allocated
        task_t *task;           // the current task for this thread
        struct dispatch_queue_thread_t *next_thread; // The next thread in the thread pool
    };

    typedef struct in_progress {
        struct in_progress *next;
        bool *complete;
    } in_progress_t;

    typedef struct in_progress_list {
        in_progress_t *head;
    } in_progress_list_t;

    struct dispatch_queue_t {
        queue_type_t queue_type;            // the type of queue - serial or concurrent
        task_t *head_task;
        task_t *tail_task;
        thread_pool_t *thread_pool;         // The queues associated thread pool
        in_progress_list_t *in_progress_list;
    };
    
    task_t *task_create(void (*)(void *), void *, char*);
    
    void task_destroy(task_t *);

    dispatch_queue_t *dispatch_queue_create(queue_type_t);
    
    void dispatch_queue_destroy(dispatch_queue_t *);
    
    void dispatch_async(dispatch_queue_t *, task_t *);
    
    void dispatch_sync(dispatch_queue_t *, task_t *);
    
    void dispatch_for(dispatch_queue_t *, long, void (*)(long));
    
    void dispatch_queue_wait(dispatch_queue_t *);

#endif	/* DISPATCHQUEUE_H */
