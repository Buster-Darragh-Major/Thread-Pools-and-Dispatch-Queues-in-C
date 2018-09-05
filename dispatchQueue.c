#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include "dispatchQueue.h"

pthread_mutex_t queue_lock;

void enqueue(dispatch_queue_t *queue, task_t *task)
{
    // If the queue is empty the head task will be null
    if (queue->head_task == NULL)
    {
        queue->head_task = task;
    }
    else // Else set the current tail's next task to be the queued task
    {
        task_t *current_tail = queue->tail_task;
        current_tail->next_task = task; // Set next task of current tail to be the new tail
    }
    
    // TODO: memory issues if the queue is empty bu a tail still exists?
    queue->tail_task = task; // Set new tail of queue to be current task
}

task_t* dequeue(dispatch_queue_t *queue)
{
    task_t *current_head = queue->head_task;
    queue->head_task = current_head->next_task;

    return current_head;
}

void push(thread_pool_t *thread_pool, dispatch_queue_thread_t *thread)
{
    // If there exists a thread on the top of the thread pool, replace it with the newly created
    // thread while setting the newly made top threads next thread reference to the old top thread,
    // or in other words put a new thread on top of the stack.
    if (thread_pool->top_thread != NULL)
    {
        dispatch_queue_thread_t *current_top_thread = thread_pool->top_thread; // Get the current top thread in the pool stack
        thread->next_thread = current_top_thread; // Set the next thread from the current newly created thread to the current top thread
    }
    else
    {
        thread_pool->top_thread = malloc(sizeof(dispatch_queue_thread_t));
    }

    // Set the current top thread in the pool to the newly created thread regardless of whether one already exists
    thread_pool->top_thread = thread; 

}

dispatch_queue_thread_t* pop(thread_pool_t *thread_pool)
{
    dispatch_queue_thread_t *current_top = thread_pool->top_thread;
    thread_pool->top_thread = current_top->next_thread;

    return current_top;

}

bool is_empty(thread_pool_t *thread_pool) 
{
    if (thread_pool->top_thread!= NULL)
    {
        return true;
    }
    else
    {
        return false;
    }
}

struct run_task_args {
    dispatch_queue_t *queue;
    sem_t *semaphore;
};

// This task is intended to be run in a thread which is meant to run tasks from a dispatch_queue_t.
// It works by looping through andchecking a related semaphore to the thread which indicates whether
// the thread should be "Awake" or not. If the semaphore is posted to then the thread is awakened,
// and the task first in the queue is dequed and executed. Once finished the sem_wait() is called
// once again and the thread once again blocks.
void *run_task(void* ptr) 
{
    struct run_task_args *args = (struct run_task_args *)ptr;
    for (;;)
    {
        sem_wait(args->semaphore); // TODO make dequeueing atomic
        task_t *task = dequeue(args->queue);
        (task->work)(task->params);
    }
}

// Creates a dispatch queue, probably setting up any associated threads and a linked list to be used by
// the added tasks. The queueType is either CONCURRENT or SERIAL .
// Returns: A pointer to the created dispatch queue.
dispatch_queue_t *dispatch_queue_create(queue_type_t queue_type)
{
    dispatch_queue_t *return_queue = malloc(sizeof(dispatch_queue_t));
    return_queue->queue_type = queue_type;

    thread_pool_t *pool = malloc(sizeof(thread_pool_t)); // thead pool for associated queue
    pool->top_thread = NULL;
    int number_of_cores = get_nprocs();
    sem_t *sem = malloc(sizeof(sem_t));
    sem_init(sem, 0, 0);

    // Allocate as many threads as there are cores to the thread pool.
    // For each new thread create a new pthread and run the run_task() method on it.
    // Description of run_task() is found in its header.
    for (int i = 0; i < number_of_cores; i++)
    {
        pthread_t thread = (pthread_t)malloc(sizeof(pthread_t));

        struct run_task_args *args = malloc(sizeof(struct run_task_args));
        args->queue = return_queue;
        sem_t *sem = (sem_t *)malloc(sizeof(sem_t));
        sem_init(sem, 0, 0);
        args->semaphore = sem;
        pthread_create(&thread, NULL, (void *)run_task, (void *)args); // Run the run task polling function

        dispatch_queue_thread_t *dispatch_queue_thread = malloc(sizeof(dispatch_queue_thread_t));
        dispatch_queue_thread->queue = return_queue;
        dispatch_queue_thread->pthread = thread;
        dispatch_queue_thread->thread_semaphore = sem;

        // Add thread to the pool
        push(pool, dispatch_queue_thread);

    }

    return_queue->thread_pool = pool;

    return return_queue;
}

// Destroys the dispatch queue queue . All allocated memory and resources such as semaphores are
// released and returned.
void dispatch_queue_destroy(dispatch_queue_t *queue)
{
    // TODO: delete other stuff
    free(queue);
}

typedef struct wrapper_args {
    void (*work)(void *);
    void *params;
    bool *ready;
} wrapper_args_t;

void *work_wrapper(wrapper_args_t *args)
{
    (args->work)(args->params);
    *(args->ready) = true;
}

// Creates a task. work is the function to be called when the task is executed, param is a pointer to
// either a structure which holds all of the parameters for the work function to execute with or a single
// parameter which the work function uses. If it is a single parameter it must either be a pointer or
// something which can be cast to or from a pointer. The name is a string of up to 63 characters. This
// is useful for debugging purposes.
// Returns: A pointer to the created task.
task_t *task_create(void (* work)(void *), void *param, char* name)
{
    task_t *task = malloc(sizeof(task_t));
    strcpy(task->name, name);
    task->params = param;
    task->work = work;

    return task;
}

// Destroys the task . Call this function as soon as a task has completed. All memory allocated to the
// task should be returned.
void task_destroy(task_t *task)
{
    // TODO: release all other members of task
    free(task);
}

// Sends the task to the queue (which could be either CONCURRENT or SERIAL ). This function does
// not return to the calling thread until the task has been completed.
void dispatch_sync(dispatch_queue_t *queue, task_t *task)
{
    bool ready = false;

    wrapper_args_t *args = malloc(sizeof(wrapper_args_t));
    args->params = task->params;
    args->work = task->work;
    args->ready = &ready;

    task->params = args;
    task->work = (void (*)(void *))work_wrapper;// wrapped task

    enqueue(queue, task);
    dispatch_queue_thread_t *thread = queue->thread_pool->top_thread;
    sem_post(thread->thread_semaphore);

    while (!ready);
}

// Sends the task to the queue (which could be either CONCURRENT or SERIAL ). This function
// returns immediately, the task will be dispatched sometime in the future.
void dispatch_async(dispatch_queue_t *queue, task_t *task)
{
    enqueue(queue, task);

    pthread_mutex_lock(&queue_lock);

    dispatch_queue_thread_t *thread = queue->thread_pool->top_thread;
    pop(queue->thread_pool);
    sem_post(thread->thread_semaphore);

    pthread_mutex_unlock(&queue_lock);
}

// Waits (blocks) until all tasks on the queue have completed. If new tasks are added to the queue
// after this is called they are ignored.
void dispatch_queue_wait(dispatch_queue_t *queue)
{
    if (queue->queue_type == SERIAL) //  Make a single thread for execution 
    {
        task_t *task;
        queue->head_task;

    }
   
}

// Executes the work function number of times (in parallel if the queue is CONCURRENT ). Each
// iteration of the work function is passed an integer from 0 to number-1 . The dispatch_for
// function does not return until all iterations of the work function have completed.
void dispatch_for(dispatch_queue_t *queue, long number, void (*work)(long))
{
    // Enter loop
    for (long i = 0; i < number; i++) {

    }
}