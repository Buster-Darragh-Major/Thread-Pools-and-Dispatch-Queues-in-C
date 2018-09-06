#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include "dispatchQueue.h"

#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define BLU   "\x1B[34m"
#define MAG   "\x1B[35m"
#define CYN   "\x1B[36m"
#define WHT   "\x1B[37m"
#define RESET "\x1B[0m"

// todo: make local?
pthread_mutex_t pool_lock;
pthread_mutex_t queue_lock;
pthread_mutex_t in_progress_lock;

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
    #ifdef DEBUG
    printf(CYN "Task %p:\tQueued\n" RESET, task);
    #endif
}

task_t* dequeue(dispatch_queue_t *queue)
{
     #ifdef DEBUG
    printf(BLU "Task %p:\tDequeued, %p is now the head task\n" RESET, queue->head_task, queue->head_task->next_task);
    #endif

    task_t *current_head = queue->head_task;
    queue->head_task = current_head->next_task;
    current_head->next_task = NULL; // If task is added again later then ensure that any previous state is destroyed

    return current_head;
}

bool queue_is_empty(dispatch_queue_t *queue)
{
    if (queue->head_task == NULL)
    {
        #ifdef DEBUG
        printf(RED "Task queue is empty!\n" RESET);
        #endif

        return true;
    }
    else
    {
        return false;
    }
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

    #ifdef DEBUG
    printf(MAG "Thread %lx:\tPushed onto the stack\n" RESET, thread->pthread);
    #endif
}

dispatch_queue_thread_t* pop(thread_pool_t *thread_pool)
{
    dispatch_queue_thread_t *current_top = thread_pool->top_thread;
    thread_pool->top_thread = current_top->next_thread;
    current_top->next_thread = NULL; // Sever ties with next thread in pool

    #ifdef DEBUG
    printf(GRN "Thread %lx:\tPopped off the stack\n" RESET, current_top->pthread);
    #endif

    return current_top;

}

bool is_empty(thread_pool_t *thread_pool) 
{
    if (thread_pool->top_thread == NULL)
    {
        return true;
    }
    else
    {
        return false;
    }
}

struct run_task_args 
{
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
        sem_wait(args->semaphore);

        #ifdef DEBUG
        pthread_t pthread = pthread_self();
        int *sem_value = malloc(sizeof(int));
        sem_getvalue(args->semaphore, sem_value);
        printf(YEL "Thread %lx:\tAwakened!\n" RESET, pthread);
        #endif

        task_t *task = dequeue(args->queue);
        pthread_mutex_unlock(&queue_lock);
        (task->work)(task->params); 
        // After this point the thread this runs in is pushed to the thread pool
        // ***NOTE*** The thread pool also becomes locked after the thread is pushed to allow for any potential
        // re-popping of the thread further down.

        bool entered_loop;
        dispatch_queue_thread_t *this_thread;

        // After this if there are any tasks in the queue then run em!
        pthread_mutex_lock(&queue_lock);
        if (!queue_is_empty(args->queue))
        {
            entered_loop = true;
            // Guaranteed to be the same thread as this one is on because the pool has been locked since it has been pushed.
            this_thread = pop(args->queue->thread_pool);
            pthread_mutex_unlock(&pool_lock); // The thread has been popped 
        }

        while (!queue_is_empty(args->queue))
        {
            task_t *task = dequeue(args->queue);
            pthread_mutex_unlock(&queue_lock);
            (task->work)(task->params); // Do the extra work
            task->complete = true;

            pthread_mutex_lock(&queue_lock); // Ensures that checking if queue is empty and popping tasks is atomic
        }

        // If the loop was entered then this thread was popped again. Push it back
        if (entered_loop)
        {
            push(args->queue->thread_pool, this_thread); // Push the thread back on after it was popped 
        }

        pthread_mutex_unlock(&pool_lock); // Unlock pool if doesnt enter while loop in case
        pthread_mutex_unlock(&queue_lock); // Unlock anyway in case it doesnt enter while loop
    }
}

// Creates a dispatch queue, probably setting up any associated threads and a linked list to be used by
// the added tasks. The queueType is either CONCURRENT or SERIAL .
// Returns: A pointer to the created dispatch queue.
dispatch_queue_t *dispatch_queue_create(queue_type_t queue_type)
{
    dispatch_queue_t *return_queue = malloc(sizeof(dispatch_queue_t));
    return_queue->queue_type = queue_type;
    return_queue->in_progress_list = malloc(sizeof(in_progress_list_t));

    thread_pool_t *pool = malloc(sizeof(thread_pool_t)); // thead pool for associated queue
    pool->top_thread = NULL;
    int number_of_cores;

    if (queue_type == CONCURRENT) // Concurrent queue gets thread pool of size |no of cores|
    {
        number_of_cores = get_nprocs();
    }
    else if (queue_type == SERIAL) // Serial queue gets 1 thread for queue
    {
        number_of_cores = 1;
    }

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

typedef struct wrapper_args 
{
    void (*work)(void *);
    void *params;
    bool *ready;
} wrapper_args_t;


// This method is to be passed to a dispatch order desired to be synchronous. It is given an argument of
// wrapper_args which contains a function to do work (work), parameters for the function (params) and a
// pointer to a boolean variable (ready). The value of this pointer can be changed and will affect the 
// state of the function that called / created the work_wrapper args struct instance. The function runs
/// the given work function with the supplied params and then sets the ready to true, implying that any
// other code that holds a reference to ready can identify a change in state of this particular function.
void *work_wrapper(wrapper_args_t *args)
{
    (args->work)(args->params); // Do the work
    *(args->ready) = true; // set ready to true
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
    task->complete = false;

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

    // Create wrapper_args instance with the given task function and parameters, as well as the
    // previously initialized boolean ready.
    wrapper_args_t *args = malloc(sizeof(wrapper_args_t));
    args->params = task->params;
    args->work = task->work;
    args->ready = &ready;

    // For synchronous dispatch, create a wrapper function that is able to indicate the tate or "readiness"
    // of the given task function. We will give the task function to this wrapper function as well as some
    // indicator (ready) to assert when the task is finished.
    task->params = args; // "Overwrite" the current task's arguments to be the created wrapper_args
    task->work = (void (*)(void *))work_wrapper; // "Overwrite" the task's arguments to be the work wrapper function

    // Here we call the asynchronous function with our altered task which actually contains the wrapper function.
    // While this still immediately returns, we now have the referenced ready variable, which we can use to
    // assert the task's completion.
    dispatch_async(queue, task);
   
    // Wait for the referenced ready varable to turn to true before continuing, indicating the completion of the task.
    while (!ready);
}

typedef struct pushback_wrapper_args 
{
    void (*work)(void *);
    void *params;
    dispatch_queue_t *queue;
    dispatch_queue_thread_t *thread;
    bool *complete;
} pushback_wrapper_args_t;

// This wrapper function is intended to take a piece of work and its arguments, along with a reference to that piece 
// of work's disptach_queue_thread_t and dispatch_queue_t, and to push them back onto the stack when the work is 
// completed. This work itself could potentially be either a synchronous or asynchronous task, but never the less
// the task will onlY have it's thread pushed back onto the thread pool on completion.
void *pushback_wrapper(pushback_wrapper_args_t *args) 
{
    (args->work)(args->params); // Do the work
    // Lock the pool, this will be unlocked when the wrapped run_task wants to unlock it, i.e. when it has identified
    // no further unclaimed tasks and cannot repush/repop itself.
    pthread_mutex_lock(&pool_lock); 
    push(args->queue->thread_pool, args->thread); // Push task back onto thread pool
    *(args->complete) = true;
}

void add_in_progress_list(in_progress_list_t *list, bool *complete)
{
    in_progress_t *in_progress = malloc(sizeof(in_progress_t));
    in_progress->complete = complete;

    if (list->head == NULL)
    {
        list->head = in_progress;
        return;
    }

    in_progress_t *current = list->head;
    while (current->next != NULL)
    {
        current = current->next;
    }
    current->next = in_progress;
}

// Sends the task to the queue (which could be either CONCURRENT or SERIAL ). This function
// returns immediately, the task will be dispatched sometime in the future.
void dispatch_async(dispatch_queue_t *queue, task_t *task)
{
    // Ensure that queueing the task, popping the top thread off the stack and awakening the thread are atomic.
    pthread_mutex_lock(&pool_lock);

    if (!is_empty(queue->thread_pool)) // There are thread available to use
    {
        // Add a task wrapper here that takes the thread and the thread pool and pushes it back when the taskj is finished.
        dispatch_queue_thread_t *thread = pop(queue->thread_pool);

        // Map current state onto args struct
        pushback_wrapper_args_t *pushback_args = malloc(sizeof(pushback_wrapper_args_t)); 
        pushback_args->work = task->work;
        pushback_args->params = task->params;
        pushback_args->thread = thread;
        pushback_args->queue = queue;
        pushback_args->complete = &(task->complete);
        add_in_progress_list(queue->in_progress_list, &(task->complete));

        task->work = (void (*)(void *))pushback_wrapper; // "Overwrite" current task work args
        task->params = pushback_args; // "Overwrite" current task args


        pthread_mutex_lock(&queue_lock);
        enqueue(queue, task);
        sem_post(thread->thread_semaphore);
    }
    else // All threads are currently occupied
    {
        add_in_progress_list(queue->in_progress_list, &(task->complete));
        enqueue(queue, task); // Queue the task anyway
        #ifdef DEBUG
        printf(RED "Thread pool is empty!\n" RESET);
        #endif
    }

    pthread_mutex_unlock(&pool_lock);
    
}

bool exist_unfinished_tasks(bool *complete[], int size)
{
    for (int i = 0; i < size; i++)
    {
        if (!*(complete[i])) 
        {
            return true;
        }
    }
    return false;
}

// Waits (blocks) until all tasks on the queue have completed. If new tasks are added to the queue
// after this is called they are ignored.
void dispatch_queue_wait(dispatch_queue_t *queue)
{
    pthread_mutex_lock(&in_progress_lock);
    // Make a "copy" of the current list of boolean pointers, we can make it an array this time as fixed size
    int num_of_tasks = 0;
    in_progress_t *in_progress = queue->in_progress_list->head;
    while (in_progress != NULL)
    {
        num_of_tasks++;
        in_progress = in_progress->next;
    }

    in_progress = queue->in_progress_list->head;
    bool *in_progress_array[num_of_tasks];
    for (int i = 0; i < num_of_tasks; i++)
    {
        in_progress_array[i] = in_progress->complete;
        in_progress = in_progress->next;
    }

    pthread_mutex_unlock(&in_progress_lock);
    // Wait while the array contains tasks that are incomplete
    while(exist_unfinished_tasks(in_progress_array, num_of_tasks));
}

// Executes the work function number of times (in parallel if the queue is CONCURRENT ). Each
// iteration of the work function is passed an integer from 0 to number-1 . The dispatch_for
// function does not return until all iterations of the work function have completed.
void dispatch_for(dispatch_queue_t *queue, long number, void (*work)(long))
{
    // If queue is concurrent then all tasks can be looped through and executed asynchrnonously, 
    // as this method returns immediately the tasks can be queued all at once and as a result
    // executed in a concurrent fashion.
    if (queue->queue_type == CONCURRENT)
    {
        for (long i = 0; i < number; i++)
        {
            task_t *task = task_create((void (*)(void *))work, (void *)i, "Stub");
            dispatch_async(queue, task);
        }
    }
    // If queue type is serial then it suffices to loop through and call all tasks synchronously,
    // as this method only returns when the task is completed another task can only be called and
    // queued once the previous tasks have completed.
    else if(queue->queue_type == SERIAL)
    {
        for (long i = 0; i < number; i++)
        {
            task_t *task = task_create((void (*)(void *))work, (void *)i, "Stub");
            dispatch_sync(queue, task);
        }
    }
    dispatch_queue_wait(queue); // Wait for all tasks to finish before returning
}