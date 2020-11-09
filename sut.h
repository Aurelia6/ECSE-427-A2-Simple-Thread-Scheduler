#ifndef __SUT_H__
#define __SUT_H__
#include <stdbool.h>
#include <ucontext.h>

#define MAX_THREADS 32
#define THREAD_STACK_SIZE 1024*64
#define SIZE 200

typedef struct __threaddesc
{
	int threadid;
	char *threadstack;
	void *threadfunc;
	ucontext_t threadcontext;
} threaddesc;

extern threaddesc thread_array[MAX_THREADS];

typedef struct __iodesc
{
	char *iofunction;		// "open" || "write" || "read" || "close"

	int ioid;
	char *iostack;

	char *dest;
	int port;

	char *buffer;
	int size;
} iodesc;

//Functions
typedef void (*sut_task_f)();

void sut_init();
bool sut_create(sut_task_f fn);
void sut_yield();
void sut_exit();
void sut_open(char *dest, int port);
void sut_write(char *buf, int size);
void sut_close();
char *sut_read();
void sut_shutdown();


#endif
