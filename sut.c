#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include "sut.h"
#include "queue.h"
#include "a1_lib.h"

pthread_t C_EXEC, I_EXEC;
ucontext_t c_exec_context;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // will allow to lock eveyrtime we want to enqueue or dequeue
struct queue c_exec_queue, i_exec_queue, wait_queue;
struct queue_entry *ptr;
int sleeping_time = 1000;

//C_Exec global variables
int thread_number;
threaddesc thread_array[MAX_THREADS], *tdescptr;

//I_Exec global variables
iodesc *iodescptr;
int sockfd;
char received_from_server[SIZE];

// Neccesary flags in order to shutdown I_Exec and C_Exec
bool create_flag = false, open_flag = false, c_exec_shutdown_flag = false, close_flag = false, connection_failed_flag = false;

/* Creation of the C_EXEC thread */
void *C_Exec(void *arg)
{
    while (true)
    {
        // Activate the lock in order to get the first element from the queue
        pthread_mutex_lock(&mutex);
        struct queue_entry *ptr_local = queue_peek_front(&c_exec_queue);
        pthread_mutex_unlock(&mutex);
        //Check if the queue is empty
        if (ptr_local != NULL)
        {
            pthread_mutex_lock(&mutex);
            ptr = queue_pop_head(&c_exec_queue);
            pthread_mutex_unlock(&mutex);
            // Get the data for the current task to run
            threaddesc *current_task = (threaddesc *)ptr->data;
            // Swapcontext in order to launch the first element from the queue
            swapcontext(&c_exec_context, &current_task->threadcontext);
        }
        // Close the pthreads, once the queue is empty, and there are different cases to check such as if the IO was used or no
        else if ((!open_flag && create_flag) || (close_flag && create_flag) || connection_failed_flag)
        {
            c_exec_shutdown_flag = true;
            break;
        }
        usleep(sleeping_time);
    }
    pthread_exit(0);
}

/* Creation of the I_EXEC thread*/
void *I_Exec(void *arg)
{
    // Useful variables linked to the socket
    int port_number, size_value;
    char *function_name, *destination, *message;
    while (true)
    {
        // Activate the lock in order to get the first element from the queue
        pthread_mutex_lock(&mutex);
        struct queue_entry *ptr_local = queue_peek_front(&i_exec_queue);
        pthread_mutex_unlock(&mutex);
        //Check if the queue is empty
        if (ptr_local != NULL)
        {
            pthread_mutex_lock(&mutex);
            struct queue_entry *ptr_local_1 = queue_pop_head(&i_exec_queue);
            pthread_mutex_unlock(&mutex);

            function_name = ((iodesc *)ptr_local_1->data)->iofunction;
            port_number = ((iodesc *)ptr_local_1->data)->port;
            destination = ((iodesc *)ptr_local_1->data)->dest;
            message = ((iodesc *)ptr_local_1->data)->buffer;
            size_value = ((iodesc *)ptr_local_1->data)->size;

            // Check which function is called
            if (strcmp(function_name, "open") == 0)
            {
                // Open will connect to the server
                if (connect_to_server(destination, port_number, &sockfd) < 0)
                {
                    connection_failed_flag = true;
                    fprintf(stderr, "oh no\n");
                }
                else
                {
                    // Remove the open task from the wait_queue because it is finished and add it to the c_exec_queue
                    struct queue_entry *node = queue_pop_head(&wait_queue);
                    pthread_mutex_lock(&mutex);
                    queue_insert_tail(&c_exec_queue, node);
                    pthread_mutex_unlock(&mutex);
                }
            }
            else if (strcmp(function_name, "write") == 0)
            {
                // Write will write into the server
                send_message(sockfd, message, size_value);

                // Used for read, it should not appear here
                recv_message(sockfd, received_from_server, size_value);
            }
            else if (strcmp(function_name, "read") == 0)
            {
                // Read will read from the server
                /*memset(received_from_server, 0, size_value);
                recv_message(sockfd, received_from_server, size_value);*/
                struct queue_entry *node = queue_pop_head(&wait_queue);
                pthread_mutex_lock(&mutex);
                queue_insert_tail(&c_exec_queue, node);
                pthread_mutex_unlock(&mutex);
            }
            else if (strcmp(function_name, "close") == 0)
            {
                // Close will shutdown the server
                close(sockfd);
            }
        }
        // Close the pthreads, once the queue is empty, and there are different cases to check such as 
        // if the IO was used or no and if we are not going to use the io anymore
        else if ((close_flag && c_exec_shutdown_flag) || (!open_flag && c_exec_shutdown_flag) || connection_failed_flag)
        {
            break;
        }
        usleep(sleeping_time);
    }
    pthread_exit(0);
}

/* Initialization of C_EXEC and I_EXEC as well as their corresponding queues and necessary variables */
void sut_init()
{
    // Initialize the threads of the two executors C_EXEC and I_EXEC
    pthread_create(&C_EXEC, NULL, C_Exec, &mutex);
    pthread_create(&I_EXEC, NULL, I_Exec, &mutex);

    // Initialize the queues
    // C_exec
    c_exec_queue = queue_create();
    queue_init(&c_exec_queue);
    // I_exec
    i_exec_queue = queue_create();
    queue_init(&i_exec_queue);
    // Wait queue
    wait_queue = queue_create();
    queue_init(&wait_queue);

    // Initialize important values
    thread_number = 0;
}

/* Creation of all the task and add it to be scheduled in the C_EXEC thread */
bool sut_create(sut_task_f fn)
{
    // Necessary in order to shutdown later on
    create_flag = true;
    // Check for a limited number of threads
    if (thread_number >= MAX_THREADS)
    {
        printf("FATAL: Maximum thread limit reached... creation failed! \n");
        return false;
    }

    // Generation of the task/context
    tdescptr = &(thread_array[thread_number]);
    getcontext(&(tdescptr->threadcontext));
    tdescptr->threadid = thread_number;
    tdescptr->threadstack = (char *)malloc(THREAD_STACK_SIZE);
    tdescptr->threadcontext.uc_stack.ss_sp = tdescptr->threadstack;
    tdescptr->threadcontext.uc_stack.ss_size = THREAD_STACK_SIZE;
    tdescptr->threadcontext.uc_link = 0;
    tdescptr->threadcontext.uc_stack.ss_flags = 0;
    tdescptr->threadfunc = fn;

    makecontext(&tdescptr->threadcontext, fn, 0);

    // Insert the struct in the C_exec queue.
    struct queue_entry *node = queue_new_node(tdescptr);
    pthread_mutex_lock(&mutex);
    queue_insert_tail(&c_exec_queue, node);
    // Incrementation of the number of threads
    thread_number++;
    pthread_mutex_unlock(&mutex);

    return true;
}

/* The current running task will be saved and rescheduled to be resumed */
void sut_yield()
{
    // ptr has been set from the C_EXEC method, and we want to get the current task so we can swap context
    threaddesc *current_task = (threaddesc *)ptr->data;

    // Enqueue the structure which was just dequeued
    pthread_mutex_lock(&mutex);
    queue_insert_tail(&c_exec_queue, ptr);
    pthread_mutex_unlock(&mutex);

    // Go back to the c_exec_context i.e. C_EXEC function
    swapcontext(&current_task->threadcontext, &c_exec_context);
}

/* The current running task will be destroyed-should not resume later */
void sut_exit()
{
    // Get the current task and totally remove it
    threaddesc *current_task = (threaddesc *)ptr->data;
    free(current_task->threadstack);

    // Go back to the c_exec_context i.e. C_EXEC function
    swapcontext(&current_task->threadcontext, &c_exec_context);
}

/* This function will open TCP socket connection to the address specified by dest on the port port */
void sut_open(char *dest, int port)
{
    // Necessary in order to shutdown the threads later on
    open_flag = true;

    // Generation of the structure
    iodescptr = (iodesc *)malloc(sizeof(iodesc));

    iodescptr->port = port;
    iodescptr->dest = dest;
    iodescptr->iofunction = "open";

    // Enqueue the structure
    struct queue_entry *node = queue_new_node(iodescptr);
    // The structure is added to the i_exec_queue and consequently the current_task is put in the waiting queue
    pthread_mutex_lock(&mutex);
    queue_insert_tail(&i_exec_queue, node);
    queue_insert_tail(&wait_queue, ptr);
    pthread_mutex_unlock(&mutex);

    // Go to the c_exec_context i.e. C_EXEC function in order to avoid blocking it
    threaddesc *current_io_task = (threaddesc *)ptr->data;
    swapcontext(&current_io_task->threadcontext, &c_exec_context);
}

/* This function will write size bytes from buf to the socket associated with the current task */
void sut_write(char *buf, int size)
{
    if (!open_flag)
    {
        printf("ERROR: sut_open() must be called before sut_write()\n\n");
        return;
    }

    // Generation of the structure
    iodescptr = (iodesc *)malloc(sizeof(iodesc));

    iodescptr->buffer = buf;
    iodescptr->size = size;
    iodescptr->iofunction = "write";

    // Enqueue the structure
    struct queue_entry *node = queue_new_node(iodescptr);
    // The structure is added to the i_exec_queue
    pthread_mutex_lock(&mutex);
    queue_insert_tail(&i_exec_queue, node);
    pthread_mutex_unlock(&mutex);
}

/* This function will close the socket associated with the current task */
void sut_close()
{
    if (!open_flag)
    {
        printf("ERROR: sut_open() must be called before sut_close()\n\n");
        return;
    }
    
    // Neccessary in order to shutdown the threads later on
    close_flag = true;

    // Generation of the structure
    iodescptr = (iodesc *)malloc(sizeof(iodesc));

    iodescptr->iofunction = "close";

    // Enqueue the structure
    struct queue_entry *node = queue_new_node(iodescptr);
    // The structure is added to the i_exec_queue
    pthread_mutex_lock(&mutex);
    queue_insert_tail(&i_exec_queue, node);
    pthread_mutex_unlock(&mutex);

    // Go to the c_exec_context i.e. C_EXEC function in order to avoid blocking it
    threaddesc *current_io_task = (threaddesc *)ptr->data;
    swapcontext(&current_io_task->threadcontext, &c_exec_context);
}

/* This function will read from the task's associated socket until there is no more data */
char *sut_read()
{
    if (!open_flag)
    {
        printf("ERROR: sut_open() must be called before sut_read()\n\n");
        // received_from_server should be empty
        return received_from_server;
    }

    // Generation of the task/context
    iodescptr = (iodesc *)malloc(sizeof(iodesc));

    iodescptr->iofunction = "read";

    // Insert the struct in the C_exec queue.
    struct queue_entry *node = queue_new_node(iodescptr);
    pthread_mutex_lock(&mutex);
    queue_insert_tail(&i_exec_queue, node);
    queue_insert_tail(&wait_queue, ptr);
    pthread_mutex_unlock(&mutex);

    // Go to the c_exec_context i.e. C_EXEC function in order to avoid blocking it
    threaddesc *current_task = (threaddesc *)ptr->data;
    swapcontext(&current_task->threadcontext, &c_exec_context);

    return received_from_server;
}

/* This function will clean up any internal library state after the currently running tasks finish */
void sut_shutdown()
{
    // Launch the C_Exec and I_Exec threads
    pthread_join(C_EXEC, NULL);
    pthread_join(I_EXEC, NULL);
}
