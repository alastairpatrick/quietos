#include "scheduler.h"
#include "scheduler.struct.h"
#include "scheduler.inl.c"

#include "atomic.h"
#include "critical.h"
#include "dlist_it.h"
#include "dlist.inl.c"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>

#include "hardware/exception.h"
#include "hardware/irq.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "pico/platform.h"

struct ExceptionFrame {
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
  void* lr;
  TaskEntry return_addr;
  uint32_t xpsr;
};

struct Scheduler {
  DList ready_tasks;    // Always in descending priority order
  DList blocked_tasks;  // Always in descending priority order
  DList pending_tasks;  // Always in descending priority order
  volatile bool ready_blocked_tasks;
};

static Scheduler g_schedulers[NUM_CORES];

extern "C" {
  void rtos_internal_init_stacks();
  void rtos_supervisor_svc_handler();
  void rtos_supervisor_systick_handler();
  void rtos_supervisor_pendsv_handler();
  Task* rtos_supervisor_context_switch(int new_state, Task* current);
  void rtos_supervisor_sleep(uint32_t time);
}

static void init_scheduler(Scheduler& scheduler) {
  if (scheduler.ready_tasks.sentinel.next) {
    return;
  }

  init_dlist(&scheduler.ready_tasks);
  init_dlist(&scheduler.blocked_tasks);
  init_dlist(&scheduler.pending_tasks);
}

Task *new_task(int priority, TaskEntry entry, int32_t stack_size) {
  auto& scheduler = g_schedulers[get_core_num()];
  init_scheduler(scheduler);

  auto& ready_tasks = scheduler.ready_tasks;

  Task* task = new Task;
  init_dnode(&task->node);

  // Maintain ready tasks in descending priority order.
  auto position = begin<Task>(ready_tasks);
  for (; position != end<Task>(ready_tasks); ++position) {
    if (position->priority < priority) {
      break;
    }
  }
  splice(position, *task);

  task->entry = entry;
  task->priority = priority;
  task->stack_size = stack_size;
  task->stack = new int32_t[(stack_size + 3) / 4];

  task->sp = ((uint8_t*) task->stack) + stack_size - sizeof(ExceptionFrame);
  ExceptionFrame* frame = (ExceptionFrame*) task->sp;

  frame->lr = 0;
  frame->return_addr = entry;
  frame->xpsr = 0x1000000;

  return task;
}

void start_scheduler() {
  auto& scheduler = g_schedulers[get_core_num()];
  init_scheduler(scheduler);

  Task* idle_task = current_task = new Task;
  memset(idle_task, 0, sizeof(idle_task));
  init_dnode(&idle_task->node);
  idle_task->priority = INT_MIN;

  rtos_internal_init_stacks();

  systick_hw->csr = 0;

  exception_set_exclusive_handler(PENDSV_EXCEPTION, rtos_supervisor_pendsv_handler);
  irq_set_priority(PENDSV_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);

  exception_set_exclusive_handler(SVCALL_EXCEPTION, rtos_supervisor_svc_handler);
  irq_set_priority(SVCALL_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);

  exception_set_exclusive_handler(SYSTICK_EXCEPTION, rtos_supervisor_systick_handler);
  irq_set_priority(SYSTICK_EXCEPTION, PICO_LOWEST_IRQ_PRIORITY);

  // Enable SysTick, processor clock, enable exception
  systick_hw->rvr = QUANTUM;
  systick_hw->cvr = 0;
  systick_hw->csr = M0PLUS_SYST_CSR_CLKSOURCE_BITS | M0PLUS_SYST_CSR_TICKINT_BITS | M0PLUS_SYST_CSR_ENABLE_BITS;

  yield();

  // Become the idle task.
  for (;;) {
    __wfe();
  }
}

void STRIPED_RAM ready_blocked_tasks() {
  auto& scheduler = g_schedulers[get_core_num()];
  scheduler.ready_blocked_tasks = true;
}

void STRIPED_RAM conditional_proactive_yield() {
  //return;
  // Heuristic to avoid Systick preempting while lock held.
  if ((current_task->lock_count == 0) && (remaining_quantum() < QUANTUM/2)) {
    yield();
  }
}

void STRIPED_RAM increment_lock_count() {
  conditional_proactive_yield();
  ++current_task->lock_count;
}

void STRIPED_RAM decrement_lock_count() {
  assert(--current_task->lock_count >= 0);
  conditional_proactive_yield();
}

static TaskState STRIPED_RAM yield_critical(void*) {
  return TASK_READY;
}

void STRIPED_RAM yield() {
  critical_section(yield_critical, 0);
}


Task* STRIPED_RAM rtos_supervisor_context_switch(int blocked, Task* current) {
  auto& scheduler = g_schedulers[get_core_num()];
  auto& ready_tasks = scheduler.ready_tasks;
  auto& blocked_tasks = scheduler.blocked_tasks;
  auto& pending_tasks = scheduler.pending_tasks;

  assert(current == current_task);
  int current_priority = current->priority;

  // If some tasks might be able to transition from blocked to ready, make all blocked tasks ready.
  if (scheduler.ready_blocked_tasks) {
    scheduler.ready_blocked_tasks = false;

    if (!is_dlist_empty(&blocked_tasks)) {
      // The blocked tasks are in descending order and all have >= priority than any pending task
      // so could just insert the blocked tasks at the beginning of the pending list. However,
      // to give tasks with equal priority round robin scheduling, skip any already pending
      // tasks with priority equal to the highest priority blocked task.
      int blocked_priority = front<Task>(blocked_tasks)->priority;

      auto position = begin<Task>(pending_tasks);
      for (; position != end<Task>(pending_tasks); ++position) {
        if (position->priority < blocked_priority) {
          break;
        }
      }
      splice(position, begin<Task>(blocked_tasks), end<Task>(blocked_tasks));
      assert(is_dlist_empty(&blocked_tasks));
    }
  }

  if (blocked) {
    // Maintain blocked_tasks in descending priority order.
    assert(is_dlist_empty(&blocked_tasks) || current_priority <= back<Task>(blocked_tasks)->priority);
    splice(end<Task>(blocked_tasks), *current);
  } else  {
    // Maintain ready_tasks in descending priority order.
    if (is_dlist_empty(&ready_tasks) || current_priority <= back<Task>(ready_tasks)->priority) {
      // Fast path for common case.
      splice(end<Task>(ready_tasks), *current);
    } else {
      // This can happen if a task blocks, is rescheduled and then yields.
      auto position = begin<Task>(ready_tasks);
      for (; position != end<Task>(ready_tasks); ++position) {
        if (position->priority < current_priority) {
          break;
        }
      }
      splice(position, *current);
    }
  }

  if (is_dlist_empty(&pending_tasks)) {
    swap_dlist(&pending_tasks, &ready_tasks);

    // The idle task never blocks so is always be either pending or ready.
    assert(!is_dlist_empty(&pending_tasks));
  }

  current = &*front<Task>(pending_tasks);
  remove_dnode(&current->node);

  // Reset SysTick.
  systick_hw->cvr = 0;

  return current;
}

void STRIPED_RAM rtos_supervisor_sleep(uint32_t time) {

}
