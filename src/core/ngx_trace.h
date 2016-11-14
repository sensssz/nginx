#ifndef NGX_TRACCE_H
#define NGX_TRACCE_H

#include <pthread.h>
#include <time.h>

typedef struct timespec timespec;

#ifdef __cplusplus
extern "C" {
#endif

/** This macro is used for tracing the running time of
    a function call which appears inside an if statement*/
#define TRACE_S_E(function_call, index) (TRACE_START()|(function_call)|TRACE_END(index))

typedef unsigned long int ulint;

/** The global transaction id counter */
extern ulint transaction_id;

pthread_t get_thread();

void set_id(int id);

int get_thread_id();

void log_command(const char *command);

void log_address(void *address);

void SESSION_START();

void SESSION_END(int successful);

void SET_WRITE(long write_time);

void EXCLUDE_WRITE(int index);

void PATH_SET(int path_count);

int PATH_GET();

void PATH_INC();

void PATH_DEC();

timespec get_trx_start();

/********************************************************************//**
This function marks the start of a function call */
void TRACE_FUNCTION_START();

/********************************************************************//**
This function marks the end of a function call */
void TRACE_FUNCTION_END();

/********************************************************************//**
This function marks the start of a child function call. */
int TRACE_START();

/********************************************************************//**
This function marks the end of a child function call. */
int TRACE_END(
  int index);   /*!< Index of the child function call. */

#ifdef __cplusplus
}
#endif

#endif
