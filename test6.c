/* 
 * File: test6.c
 * Author Buster Major
 */

#include "dispatchQueue.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void print_stuff(void *i)
{
    int *num = (int*)i;
    sleep(1);
    printf("%d:\tCan be printed in any order\n", *num);
}

int main(int argc, char** argv)
{
    dispatch_queue_t *dispatch_queue;
    task_t *task1;
    task_t *task2;
    task_t *task3;
    task_t *task4;
    task_t *task5;

    dispatch_queue = dispatch_queue_create(CONCURRENT);

    int *i1 = malloc(sizeof(int));
    *i1 = 1;
    task1 = task_create(print_stuff, (void *)i1, "print");
    int *i2 = malloc(sizeof(int));
    *i2 = 2;
    task2 = task_create(print_stuff, (void *)i2, "print");
    int *i3 = malloc(sizeof(int));
    *i3 = 3;
    task3 = task_create(print_stuff, (void *)i3, "print");
    int *i4 = malloc(sizeof(int));
    *i4 = 4;
    task4 = task_create(print_stuff, (void *)i4, "print");
    int *i5 = malloc(sizeof(int));
    *i5 = 5;
    task5 = task_create(print_stuff, (void *)i5, "print");
    
    dispatch_async(dispatch_queue, task1);
    dispatch_async(dispatch_queue, task2);
    dispatch_async(dispatch_queue, task3);
    dispatch_async(dispatch_queue, task4);
    dispatch_async(dispatch_queue, task5);
    printf("All tasks dipatched\n");
    sleep(7);
    dispatch_async(dispatch_queue, task1);
    sleep(3);

}