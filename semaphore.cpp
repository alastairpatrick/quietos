#include "semaphore.h"
#include "semaphore.internal.h"

#include "atomic.h"
#include "critical.h"
#include "dlist_it.h"
#include "scheduler.h"
#include "scheduler.internal.h"

#include <cassert>
#include <cstdarg>

Semaphore* qos_new_semaphore(int32_t initial_count) {
  auto semaphore = new Semaphore;
  qos_init_semaphore(semaphore, initial_count);
  return semaphore;
}

void qos_init_semaphore(Semaphore* semaphore, int32_t initial_count) {
  assert(initial_count >= 0);
  semaphore->core = get_core_num();
  semaphore->count = initial_count;
  qos_init_dlist(&semaphore->waiting.tasks);
}

static qos_task_state_t STRIPED_RAM acquire_semaphore_critical(qos_scheduler_t* scheduler, va_list args) {
  auto semaphore = va_arg(args, Semaphore*);
  auto count = va_arg(args, int32_t);
  auto timeout = va_arg(args, qos_tick_count_t);

  auto current_task = scheduler->current_task;

  auto old_count = semaphore->count;
  auto new_count = old_count - count;
  if (new_count >= 0) {
    semaphore->count = new_count;
    qos_set_current_critical_section_result(scheduler, true);
    return TASK_RUNNING;
  }

  if (timeout == 0) {
    return TASK_RUNNING;
  }

  current_task->sync_state = count;
  qos_internal_insert_scheduled_task(&semaphore->waiting, current_task);
  qos_delay_task(scheduler, current_task, timeout);

  return TASK_SYNC_BLOCKED;
}

bool STRIPED_RAM qos_acquire_semaphore(Semaphore* semaphore, int32_t count, qos_tick_count_t timeout) {
  assert(semaphore->core == get_core_num());
  assert(count >= 0);
  qos_normalize_tick_count(&timeout);

  auto old_count = semaphore->count;
  auto new_count = old_count - count;
  if (new_count >= 0 && qos_atomic_compare_and_set(&semaphore->count, old_count, new_count) == old_count) {
    return true;
  }

  if (timeout == 0) {
    return false;
  }

  return qos_critical_section_va(acquire_semaphore_critical, semaphore, count, timeout);
}

static qos_task_state_t STRIPED_RAM release_semaphore_critical(qos_scheduler_t* scheduler, va_list args) {
  auto semaphore = va_arg(args, Semaphore*);
  auto count = va_arg(args, int32_t);

  semaphore->count += count;

  auto should_yield = false;
  auto position = begin(semaphore->waiting);
  while (position != end(semaphore->waiting)) {
    auto task = &*position;

    if (task->sync_state <= semaphore->count) {
      semaphore->count -= task->sync_state;

      position = remove(position);

      qos_set_critical_section_result(scheduler, task, true);
      should_yield |= qos_ready_task(scheduler, task);
    } else {
      ++position;
    }
  }

  if (should_yield) {
    return TASK_READY;
  } else {
    return TASK_RUNNING;
  }
}

void STRIPED_RAM qos_release_semaphore(Semaphore* semaphore, int32_t count) {
  assert(semaphore->core == get_core_num());
  assert(count >= 0);
  qos_critical_section_va(release_semaphore_critical, semaphore, count);
}
