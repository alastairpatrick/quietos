#include "interrupt.h"

#include "critical.h"
#include "dlist.h"
#include "dlist_it.h"
#include "scheduler.h"
#include "scheduler.internal.h"
#include "hardware/irq.h"

#include "hardware/regs/m0plus.h"
#include "hardware/structs/scb.h"

extern "C" {
  void qos_supervisor_await_irq_handler();
  void qos_supervisor_await_irq(qos_scheduler_t* scheduler);
}

void STRIPED_RAM qos_supervisor_await_irq(qos_scheduler_t* scheduler) {
  int32_t ipsr;
  __asm__ volatile ("mrs %0, ipsr" : "=r"(ipsr));
  auto irq = (ipsr & 0x3F) - 16;

  auto& tasks = scheduler->awaiting_irq[irq];
  
  // Atomically disable IRQ but leave it pending.
  *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ICER_OFFSET)) = 1 << irq;

  bool should_preempt = false;
  while (!empty(begin(tasks))) {
    should_preempt |= qos_ready_task(scheduler, &*begin(tasks));
  }

  if (should_preempt) {
    scb_hw->icsr = M0PLUS_ICSR_PENDSVSET_BITS;
  }
}

void qos_init_await_irq(int32_t irq) {
  assert(irq >= 0 && irq < QOS_MAX_IRQS);

  irq_set_exclusive_handler(irq, qos_supervisor_await_irq_handler);
  irq_set_priority(irq, PICO_LOWEST_IRQ_PRIORITY);
}

static void STRIPED_RAM unblock_await_irq(qos_task_t* task) {
  // Disable interrupt.
  if (task->sync_ptr) {
    hw_clear_bits((io_rw_32*) task->sync_ptr, task->sync_state);
  }
}

qos_task_state_t STRIPED_RAM qos_await_irq_critical(qos_scheduler_t* scheduler, va_list args) {
  auto irq = va_arg(args, int32_t);
  auto enable = va_arg(args, io_rw_32*);
  auto mask = va_arg(args, int32_t);
  auto timeout = va_arg(args, qos_tick_count_t);
  auto current_task = scheduler->current_task;

  auto& awaiting_irq = scheduler->awaiting_irq;
  auto irq_mask = 1 << irq;

  // Enable interrupt.
  if (enable) {
    hw_set_bits(enable, mask);
  }

  // Yield if the interrupt is pending. Yield rather than run so other tasks
  // waiting on the same IRQ are not starved.
  *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ICPR_OFFSET)) = irq_mask;
  auto pending = *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ICPR_OFFSET));
  if (pending & irq_mask) {
    if (enable) {
      hw_clear_bits(enable, mask);
    }
    return TASK_READY;
  }

  // Enable IRQ. It's okay if this pends the interrupt; it has the same
  // priority as this supervisor call so it won't run until after this
  // supervisor call exits.
  *((io_rw_32 *) (PPB_BASE + M0PLUS_NVIC_ISER_OFFSET)) = irq_mask;

  current_task->sync_ptr = enable;
  current_task->sync_state = mask;
  current_task->sync_unblock_task_proc = unblock_await_irq;

  qos_internal_insert_scheduled_task(&awaiting_irq[irq], current_task);
  qos_delay_task(scheduler, current_task, timeout);

  return TASK_SYNC_BLOCKED;
}

bool STRIPED_RAM qos_await_irq(int32_t irq, io_rw_32* enable, int32_t mask, qos_tick_count_t timeout) {
  assert(irq >= 0 && irq < QOS_MAX_IRQS);
  qos_normalize_tick_count(&timeout);
  assert(timeout != 0);

  return qos_critical_section_va(qos_await_irq_critical, irq, enable, mask, timeout);
}
