// Test-only dual-core runtime snapshot. The fixed task table bounds RAM and sampling work.
#ifdef X11_RUNTIME_PROFILE

#include "x11_runtime_profile.h"
#include "x11_latency_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

namespace {
constexpr UBaseType_t kTaskCapacity = 40;
constexpr int64_t kSamplePeriodUs = 1000000;

struct PreviousTask {
  TaskHandle_t handle;
  UBaseType_t number;
  configRUN_TIME_COUNTER_TYPE runtime;
};

struct TaskMetric {
  const char *name;
  uint32_t cpu_pct_x100;
  uint32_t stack_free_bytes;
  uint32_t priority;
  uint8_t state;
  bool seen;
};

static TaskStatus_t tasks[kTaskCapacity];
static PreviousTask previous[kTaskCapacity];
static PreviousTask next_previous[kTaskCapacity];
static TaskMetric metrics[] = {
    {"loopTask"}, {"wifi"}, {"tcpip_thread"}, {"sys_evt"}, {"arduino_events"},
    {"esp_timer"}, {"Tmr Svc"}, {"IDLE0"}, {"IDLE1"}, {"main"},
};
static UBaseType_t previous_count;
static UBaseType_t task_count;
static uint32_t sample_us, sample_cost_us_max, select_wait_us_max;
static uint32_t select_wait_samples, blocked_count, ready_count, suspended_count;
static uint32_t core_busy_pct_x100[2];
static configRUN_TIME_COUNTER_TYPE idle_previous[2];
static int64_t sample_started_us, previous_loop_us;
static uint32_t loop_gap_us_max;
static uint32_t overflow_count;
static bool baseline_ready;

static bool same_task(const PreviousTask &a, const TaskStatus_t &b) {
  return a.handle == b.xHandle && a.number == b.xTaskNumber;
}

static uint32_t percent_x100(uint64_t used, uint64_t elapsed) {
  if (!elapsed) return 0;
  uint64_t value = used * 10000u / elapsed;
  return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint8_t state_code(eTaskState state) {
  switch (state) {
    case eRunning: return 'R';
    case eReady: return 'r';
    case eBlocked: return 'B';
    case eSuspended: return 'S';
    case eDeleted: return 'D';
    default: return '?';
  }
}

static int metric_index(const char *name) {
  for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i)
    if (strcmp(name, metrics[i].name) == 0) return (int)i;
  return -1;
}

static void take_sample(int64_t now) {
  int64_t sample_t0 = esp_timer_get_time();
  UBaseType_t count = uxTaskGetNumberOfTasks();
  if (count > kTaskCapacity) {
    if (overflow_count != UINT32_MAX) ++overflow_count;
    task_count = count;
    sample_started_us = now;
    baseline_ready = false;
    uint32_t cost = (uint32_t)(esp_timer_get_time() - sample_t0);
    if (cost > sample_cost_us_max) sample_cost_us_max = cost;
    return;
  }

  configRUN_TIME_COUNTER_TYPE total_runtime = 0;
  UBaseType_t found = uxTaskGetSystemState(tasks, kTaskCapacity, &total_runtime);
  if (!found) {
    if (overflow_count != UINT32_MAX) ++overflow_count;
    task_count = uxTaskGetNumberOfTasks();
    sample_started_us = now;
    baseline_ready = false;
    uint32_t cost = (uint32_t)(esp_timer_get_time() - sample_t0);
    if (cost > sample_cost_us_max) sample_cost_us_max = cost;
    return;
  }
  configRUN_TIME_COUNTER_TYPE idle_now[2] = {
      ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)};
  task_count = found;
  blocked_count = ready_count = suspended_count = 0;
  for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
    metrics[i].seen = false;
    metrics[i].cpu_pct_x100 = 0;
    metrics[i].stack_free_bytes = 0;
    metrics[i].priority = 0;
    metrics[i].state = '?';
  }

  uint64_t elapsed = baseline_ready ? (uint64_t)(now - sample_started_us) : 0;
  UBaseType_t next_count = 0;
  for (UBaseType_t i = 0; i < found; ++i) {
    TaskStatus_t *task = &tasks[i];
    configRUN_TIME_COUNTER_TYPE old_runtime = task->ulRunTimeCounter;
    bool matched = false;
    for (UBaseType_t j = 0; j < previous_count; ++j) {
      if (same_task(previous[j], *task)) {
        old_runtime = previous[j].runtime;
        matched = true;
        break;
      }
    }
    uint64_t delta = matched ? (uint64_t)(task->ulRunTimeCounter - old_runtime) : 0;
    int mi = task->pcTaskName ? metric_index(task->pcTaskName) : -1;
    if (mi >= 0) {
      TaskMetric &m = metrics[mi];
      m.seen = true;
      m.cpu_pct_x100 = percent_x100(delta, elapsed);
      m.stack_free_bytes = (uint32_t)task->usStackHighWaterMark * sizeof(StackType_t);
      m.priority = task->uxCurrentPriority;
      m.state = state_code(task->eCurrentState);
    }
    if (task->eCurrentState == eBlocked) ++blocked_count;
    else if (task->eCurrentState == eReady || task->eCurrentState == eRunning) ++ready_count;
    else if (task->eCurrentState == eSuspended) ++suspended_count;
    next_previous[next_count++] = {task->xHandle, task->xTaskNumber, task->ulRunTimeCounter};
  }
  memcpy(previous, next_previous, found * sizeof(previous[0]));
  previous_count = found;

  if (baseline_ready && elapsed) {
    for (int core = 0; core < 2; ++core) {
      configRUN_TIME_COUNTER_TYPE idle_delta = idle_now[core] - idle_previous[core];
      uint32_t idle_pct = percent_x100((uint64_t)idle_delta, elapsed);
      core_busy_pct_x100[core] = idle_pct >= 10000 ? 0 : 10000 - idle_pct;
    }
    sample_us = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
  }
  idle_previous[0] = idle_now[0];
  idle_previous[1] = idle_now[1];
  sample_started_us = now;
  baseline_ready = true;
  uint32_t cost = (uint32_t)(esp_timer_get_time() - sample_t0);
  if (cost > sample_cost_us_max) sample_cost_us_max = cost;
  (void)total_runtime;
}

static size_t appendf(char *out, size_t cap, size_t used, const char *fmt, ...) {
  if (used >= cap) return used;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(out + used, cap - used, fmt, ap);
  va_end(ap);
  if (n < 0) return used;
  size_t available = cap - used;
  return used + ((size_t)n < available ? (size_t)n : available - 1);
}
}  // namespace

void x11_runtime_profile_poll(void) {
  int64_t now = esp_timer_get_time();
  if (previous_loop_us) {
    uint64_t gap = (uint64_t)(now - previous_loop_us);
    if (gap > loop_gap_us_max) loop_gap_us_max = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    x11_latency_trace_add(X11_TRACE_LOOP_GAP, gap);
  }
  previous_loop_us = now;
  if (!sample_started_us || now - sample_started_us >= kSamplePeriodUs) take_sample(now);
}

void x11_runtime_profile_select_wait(uint32_t wait_us) {
  x11_latency_trace_add(X11_TRACE_SELECT_WAIT, wait_us);
  if (wait_us > select_wait_us_max) select_wait_us_max = wait_us;
  if (select_wait_samples != UINT32_MAX) ++select_wait_samples;
}

size_t x11_runtime_profile_diag(char *out, size_t cap) {
  size_t used = 0;
  used = appendf(out, cap, used,
      "rt.sample_us=%lu\nrt.core0_busy_pct=%lu.%02lu\nrt.core1_busy_pct=%lu.%02lu\n"
      "rt.tasks_seen=%lu\nrt.task_capacity=%lu\nrt.task_overflow=%lu\n"
      "rt.snapshot_us_max=%lu\nrt.loop_gap_us_max=%lu\nrt.select_wait_us_max=%lu\n"
      "rt.select_wait_samples=%lu\nrt.blocked_snapshot=%lu\nrt.ready_snapshot=%lu\nrt.suspended_snapshot=%lu\n",
      (unsigned long)sample_us,
      (unsigned long)(core_busy_pct_x100[0] / 100), (unsigned long)(core_busy_pct_x100[0] % 100),
      (unsigned long)(core_busy_pct_x100[1] / 100), (unsigned long)(core_busy_pct_x100[1] % 100),
      (unsigned long)task_count, (unsigned long)kTaskCapacity, (unsigned long)overflow_count,
      (unsigned long)sample_cost_us_max, (unsigned long)loop_gap_us_max,
      (unsigned long)select_wait_us_max, (unsigned long)select_wait_samples,
      (unsigned long)blocked_count, (unsigned long)ready_count, (unsigned long)suspended_count);
  for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i) {
    const TaskMetric &m = metrics[i];
    if (!m.seen || used >= cap) continue;
    used = appendf(out, cap, used,
        "rt.task.%s.cpu_pct=%lu.%02lu\nrt.task.%s.stack_free_bytes=%lu\n"
        "rt.task.%s.priority=%lu\nrt.task.%s.state=%c\n",
        m.name, (unsigned long)(m.cpu_pct_x100 / 100), (unsigned long)(m.cpu_pct_x100 % 100),
        m.name, (unsigned long)m.stack_free_bytes, m.name, (unsigned long)m.priority,
        m.name, m.state);
  }
  return used;
}

#endif  // X11_RUNTIME_PROFILE
