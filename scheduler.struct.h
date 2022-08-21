#ifndef RTOS_TASK_STRUCT_H
#define RTOS_TASK_STRUCT_H

#include "dlist.struct.h"

#include <stdint.h>

typedef struct Task {
  DNode node;

  void* sp;
  int32_t r4;
  int32_t r5;
  int32_t r6;
  int32_t r7;
  int32_t r8;
  int32_t r9;
  int32_t r10;
  int32_t r11;

  TaskEntry entry;
  int priority;
  int32_t* stack;
  int32_t stack_size;
  int lock_count;
} Task;

#endif  // RTOS_TASK_STRUCT_H
