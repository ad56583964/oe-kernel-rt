/*
 * Pressure stall information for CPU, memory and IO
 *
 * Copyright (c) 2018 Facebook, Inc.
 * Author: Johannes Weiner <hannes@cmpxchg.org>
 *
 * Polling support by Suren Baghdasaryan <surenb@google.com>
 * Copyright (c) 2018 Google, Inc.
 *
 * When CPU, memory and IO are contended, tasks experience delays that
 * reduce throughput and introduce latencies into the workload. Memory
 * and IO contention, in addition, can cause a full loss of forward
 * progress in which the CPU goes idle.
 *
 * This code aggregates individual task delays into resource pressure
 * metrics that indicate problems with both workload health and
 * resource utilization.
 *
 *			Model
 *
 * The time in which a task can execute on a CPU is our baseline for
 * productivity. Pressure expresses the amount of time in which this
 * potential cannot be realized due to resource contention.
 *
 * This concept of productivity has two components: the workload and
 * the CPU. To measure the impact of pressure on both, we define two
 * contention states for a resource: SOME and FULL.
 *
 * In the SOME state of a given resource, one or more tasks are
 * delayed on that resource. This affects the workload's ability to
 * perform work, but the CPU may still be executing other tasks.
 *
 * In the FULL state of a given resource, all non-idle tasks are
 * delayed on that resource such that nobody is advancing and the CPU
 * goes idle. This leaves both workload and CPU unproductive.
 *
 *	SOME = nr_delayed_tasks != 0
 *	FULL = nr_delayed_tasks != 0 && nr_productive_tasks == 0
 *
 * What it means for a task to be productive is defined differently
 * for each resource. For IO, productive means a running task. For
 * memory, productive means a running task that isn't a reclaimer. For
 * CPU, productive means an oncpu task.
 *
 * Naturally, the FULL state doesn't exist for the CPU resource at the
 * system level, but exist at the cgroup level. At the cgroup level,
 * FULL means all non-idle tasks in the cgroup are delayed on the CPU
 * resource which is being used by others outside of the cgroup or
 * throttled by the cgroup cpu.max configuration.
 *
 * The percentage of wallclock time spent in those compound stall
 * states gives pressure numbers between 0 and 100 for each resource,
 * where the SOME percentage indicates workload slowdowns and the FULL
 * percentage indicates reduced CPU utilization:
 *
 *	%SOME = time(SOME) / period
 *	%FULL = time(FULL) / period
 *
 *			Multiple CPUs
 *
 * The more tasks and available CPUs there are, the more work can be
 * performed concurrently. This means that the potential that can go
 * unrealized due to resource contention *also* scales with non-idle
 * tasks and CPUs.
 *
 * Consider a scenario where 257 number crunching tasks are trying to
 * run concurrently on 256 CPUs. If we simply aggregated the task
 * states, we would have to conclude a CPU SOME pressure number of
 * 100%, since *somebody* is waiting on a runqueue at all
 * times. However, that is clearly not the amount of contention the
 * workload is experiencing: only one out of 256 possible exceution
 * threads will be contended at any given time, or about 0.4%.
 *
 * Conversely, consider a scenario of 4 tasks and 4 CPUs where at any
 * given time *one* of the tasks is delayed due to a lack of memory.
 * Again, looking purely at the task state would yield a memory FULL
 * pressure number of 0%, since *somebody* is always making forward
 * progress. But again this wouldn't capture the amount of execution
 * potential lost, which is 1 out of 4 CPUs, or 25%.
 *
 * To calculate wasted potential (pressure) with multiple processors,
 * we have to base our calculation on the number of non-idle tasks in
 * conjunction with the number of available CPUs, which is the number
 * of potential execution threads. SOME becomes then the proportion of
 * delayed tasks to possibe threads, and FULL is the share of possible
 * threads that are unproductive due to delays:
 *
 *	threads = min(nr_nonidle_tasks, nr_cpus)
 *	   SOME = min(nr_delayed_tasks / threads, 1)
 *	   FULL = (threads - min(nr_productive_tasks, threads)) / threads
 *
 * For the 257 number crunchers on 256 CPUs, this yields:
 *
 *	threads = min(257, 256)
 *	   SOME = min(1 / 256, 1)             = 0.4%
 *	   FULL = (256 - min(256, 256)) / 256 = 0%
 *
 * For the 1 out of 4 memory-delayed tasks, this yields:
 *
 *	threads = min(4, 4)
 *	   SOME = min(1 / 4, 1)               = 25%
 *	   FULL = (4 - min(3, 4)) / 4         = 25%
 *
 * [ Substitute nr_cpus with 1, and you can see that it's a natural
 *   extension of the single-CPU model. ]
 *
 *			Implementation
 *
 * To assess the precise time spent in each such state, we would have
 * to freeze the system on task changes and start/stop the state
 * clocks accordingly. Obviously that doesn't scale in practice.
 *
 * Because the scheduler aims to distribute the compute load evenly
 * among the available CPUs, we can track task state locally to each
 * CPU and, at much lower frequency, extrapolate the global state for
 * the cumulative stall times and the running averages.
 *
 * For each runqueue, we track:
 *
 *	   tSOME[cpu] = time(nr_delayed_tasks[cpu] != 0)
 *	   tFULL[cpu] = time(nr_delayed_tasks[cpu] && !nr_productive_tasks[cpu])
 *	tNONIDLE[cpu] = time(nr_nonidle_tasks[cpu] != 0)
 *
 * and then periodically aggregate:
 *
 *	tNONIDLE = sum(tNONIDLE[i])
 *
 *	   tSOME = sum(tSOME[i] * tNONIDLE[i]) / tNONIDLE
 *	   tFULL = sum(tFULL[i] * tNONIDLE[i]) / tNONIDLE
 *
 *	   %SOME = tSOME / period
 *	   %FULL = tFULL / period
 *
 * This gives us an approximation of pressure that is practical
 * cost-wise, yet way more sensitive and accurate than periodic
 * sampling of the aggregate task states would be.
 */

#include "../workqueue_internal.h"
#include <linux/sched/loadavg.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/seqlock.h>
#include <linux/uaccess.h>
#include <linux/cgroup.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/psi.h>
#include "sched.h"

#include <trace/events/sched.h>

static int psi_bug __read_mostly;

DEFINE_STATIC_KEY_FALSE(psi_disabled);

#ifdef CONFIG_PSI_CGROUP_V1
DEFINE_STATIC_KEY_TRUE(psi_v1_disabled);
#endif

#ifdef CONFIG_PSI_DEFAULT_DISABLED
static bool psi_enable;
#else
static bool psi_enable = true;
#endif
static int __init setup_psi(char *str)
{
	return kstrtobool(str, &psi_enable) == 0;
}
__setup("psi=", setup_psi);

/* Running averages - we need to be higher-res than loadavg */
#define PSI_FREQ	(2*HZ+1)	/* 2 sec intervals */
#define EXP_10s		1677		/* 1/exp(2s/10s) as fixed-point */
#define EXP_60s		1981		/* 1/exp(2s/60s) */
#define EXP_300s	2034		/* 1/exp(2s/300s) */

/* PSI trigger definitions */
#define WINDOW_MIN_US 500000	/* Min window size is 500ms */
#define WINDOW_MAX_US 10000000	/* Max window size is 10s */
#define UPDATES_PER_WINDOW 10	/* 10 updates per window */

/* Sampling frequency in nanoseconds */
static u64 psi_period __read_mostly;

/* System-level pressure and stall tracking */
static DEFINE_PER_CPU(struct psi_group_cpu, system_group_pcpu);
struct psi_group psi_system = {
	.pcpu = &system_group_pcpu,
};

#ifdef CONFIG_PSI_FINE_GRAINED
/* System-level fine grained pressure and stall tracking */
static DEFINE_PER_CPU(struct psi_group_stat_cpu, system_stat_group_pcpu);
struct psi_group_ext psi_stat_system = {
	.pcpu = &system_stat_group_pcpu,
};

struct psi_group_ext *to_psi_group_ext(struct psi_group *psi)
{
	if (psi == &psi_system)
		return &psi_stat_system;
	else
		return container_of(psi, struct psi_group_ext, psi);
}
#else
static inline struct psi_group_ext *to_psi_group_ext(struct psi_group *psi)
{
	return NULL;
}
#endif

static void psi_avgs_work(struct work_struct *work);

static void poll_timer_fn(struct timer_list *t);

static void group_init(struct psi_group *group)
{
	int cpu;

	for_each_possible_cpu(cpu)
		seqcount_init(&per_cpu_ptr(group->pcpu, cpu)->seq);
	group->avg_last_update = sched_clock();
	group->avg_next_update = group->avg_last_update + psi_period;
	INIT_DELAYED_WORK(&group->avgs_work, psi_avgs_work);
	mutex_init(&group->avgs_lock);
	/* Init trigger-related members */
	mutex_init(&group->trigger_lock);
	INIT_LIST_HEAD(&group->triggers);
	group->poll_min_period = U32_MAX;
	group->polling_next_update = ULLONG_MAX;
	init_waitqueue_head(&group->poll_wait);
	timer_setup(&group->poll_timer, poll_timer_fn, 0);
	rcu_assign_pointer(group->poll_task, NULL);
}

void __init psi_init(void)
{
	if (!psi_enable) {
		static_branch_enable(&psi_disabled);
		return;
	}

	psi_period = jiffies_to_nsecs(PSI_FREQ);
	group_init(&psi_system);
}

static bool test_state(unsigned int *tasks, enum psi_states state, bool oncpu)
{
	switch (state) {
	case PSI_IO_SOME:
		return unlikely(tasks[NR_IOWAIT]);
	case PSI_IO_FULL:
		return unlikely(tasks[NR_IOWAIT] && !tasks[NR_RUNNING]);
	case PSI_MEM_SOME:
		return unlikely(tasks[NR_MEMSTALL]);
	case PSI_MEM_FULL:
		return unlikely(tasks[NR_MEMSTALL] &&
			tasks[NR_RUNNING] == tasks[NR_MEMSTALL_RUNNING]);
	case PSI_CPU_SOME:
		return unlikely(tasks[NR_RUNNING] > oncpu);
	case PSI_CPU_FULL:
		return unlikely(tasks[NR_RUNNING] && !oncpu);
	case PSI_NONIDLE:
		return tasks[NR_IOWAIT] || tasks[NR_MEMSTALL] ||
			tasks[NR_RUNNING];
	default:
		return false;
	}
}

static void get_recent_times(struct psi_group *group, int cpu,
			     enum psi_aggregators aggregator, u32 *times,
			     u32 *pchanged_states)
{
#ifdef CONFIG_PSI_FINE_GRAINED
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
	struct psi_group_stat_cpu *ext_groupc = per_cpu_ptr(psi_ext->pcpu, cpu);
#endif
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	u64 now, state_start;
	enum psi_states s;
	unsigned int seq;
	u32 state_mask;

	*pchanged_states = 0;

	/* Snapshot a coherent view of the CPU state */
	do {
		seq = read_seqcount_begin(&groupc->seq);
		now = cpu_clock(cpu);
		memcpy(times, groupc->times, sizeof(groupc->times));
		state_mask = groupc->state_mask;
		state_start = groupc->state_start;
	} while (read_seqcount_retry(&groupc->seq, seq));

	/* Calculate state time deltas against the previous snapshot */
	for (s = 0; s < NR_PSI_STATES; s++) {
		u32 delta;
		/*
		 * In addition to already concluded states, we also
		 * incorporate currently active states on the CPU,
		 * since states may last for many sampling periods.
		 *
		 * This way we keep our delta sampling buckets small
		 * (u32) and our reported pressure close to what's
		 * actually happening.
		 */
		if (state_mask & (1 << s))
			times[s] += now - state_start;

		delta = times[s] - groupc->times_prev[aggregator][s];
		groupc->times_prev[aggregator][s] = times[s];

		times[s] = delta;
		if (delta)
			*pchanged_states |= (1 << s);
	}
#ifdef CONFIG_PSI_FINE_GRAINED
	ext_groupc->times_delta = now - state_start;
#endif
}

static void calc_avgs(unsigned long avg[3], int missed_periods,
		      u64 time, u64 period)
{
	unsigned long pct;

	/* Fill in zeroes for periods of no activity */
	if (missed_periods) {
		avg[0] = calc_load_n(avg[0], EXP_10s, 0, missed_periods);
		avg[1] = calc_load_n(avg[1], EXP_60s, 0, missed_periods);
		avg[2] = calc_load_n(avg[2], EXP_300s, 0, missed_periods);
	}

	/* Sample the most recent active period */
	pct = div_u64(time * 100, period);
	pct *= FIXED_1;
	avg[0] = calc_load(avg[0], EXP_10s, pct);
	avg[1] = calc_load(avg[1], EXP_60s, pct);
	avg[2] = calc_load(avg[2], EXP_300s, pct);
}

#ifdef CONFIG_PSI_FINE_GRAINED

static void record_stat_times(struct psi_group_ext *psi_ext, int cpu)
{
	struct psi_group_stat_cpu *ext_grpc = per_cpu_ptr(psi_ext->pcpu, cpu);

	u32 delta = ext_grpc->psi_delta;

	if (ext_grpc->state_mask & (1 << PSI_MEMCG_RECLAIM_SOME)) {
		ext_grpc->times[PSI_MEMCG_RECLAIM_SOME] += delta;
		if (ext_grpc->state_mask & (1 << PSI_MEMCG_RECLAIM_FULL))
			ext_grpc->times[PSI_MEMCG_RECLAIM_FULL] += delta;
	}
	if (ext_grpc->state_mask & (1 << PSI_GLOBAL_RECLAIM_SOME)) {
		ext_grpc->times[PSI_GLOBAL_RECLAIM_SOME] += delta;
		if (ext_grpc->state_mask & (1 << PSI_GLOBAL_RECLAIM_FULL))
			ext_grpc->times[PSI_GLOBAL_RECLAIM_FULL] += delta;
	}
	if (ext_grpc->state_mask & (1 << PSI_COMPACT_SOME)) {
		ext_grpc->times[PSI_COMPACT_SOME] += delta;
		if (ext_grpc->state_mask & (1 << PSI_COMPACT_FULL))
			ext_grpc->times[PSI_COMPACT_FULL] += delta;
	}
	if (ext_grpc->state_mask & (1 << PSI_ASYNC_MEMCG_RECLAIM_SOME)) {
		ext_grpc->times[PSI_ASYNC_MEMCG_RECLAIM_SOME] += delta;
		if (ext_grpc->state_mask & (1 << PSI_ASYNC_MEMCG_RECLAIM_FULL))
			ext_grpc->times[PSI_ASYNC_MEMCG_RECLAIM_FULL] += delta;
	}
	if (ext_grpc->state_mask & (1 << PSI_SWAP_SOME)) {
		ext_grpc->times[PSI_SWAP_SOME] += delta;
		if (ext_grpc->state_mask & (1 << PSI_SWAP_FULL))
			ext_grpc->times[PSI_SWAP_FULL] += delta;
	}
}

static bool test_fine_grained_stat(unsigned int *stat_tasks,
				   unsigned int nr_running,
				   enum psi_stat_states state)
{
	switch (state) {
	case PSI_MEMCG_RECLAIM_SOME:
		return unlikely(stat_tasks[NR_MEMCG_RECLAIM]);
	case PSI_MEMCG_RECLAIM_FULL:
		return unlikely(stat_tasks[NR_MEMCG_RECLAIM] &&
		       nr_running == stat_tasks[NR_MEMCG_RECLAIM_RUNNING]);
	case PSI_GLOBAL_RECLAIM_SOME:
		return unlikely(stat_tasks[NR_GLOBAL_RECLAIM]);
	case PSI_GLOBAL_RECLAIM_FULL:
		return unlikely(stat_tasks[NR_GLOBAL_RECLAIM] &&
		       nr_running == stat_tasks[NR_GLOBAL_RECLAIM_RUNNING]);
	case PSI_COMPACT_SOME:
		return unlikely(stat_tasks[NR_COMPACT]);
	case PSI_COMPACT_FULL:
		return unlikely(stat_tasks[NR_COMPACT] &&
		       nr_running == stat_tasks[NR_COMPACT_RUNNING]);
	case PSI_ASYNC_MEMCG_RECLAIM_SOME:
		return unlikely(stat_tasks[NR_ASYNC_MEMCG_RECLAIM]);
	case PSI_ASYNC_MEMCG_RECLAIM_FULL:
		return unlikely(stat_tasks[NR_ASYNC_MEMCG_RECLAIM] &&
		      nr_running == stat_tasks[NR_ASYNC_MEMCG_RECLAIM_RUNNING]);
	case PSI_SWAP_SOME:
		return unlikely(stat_tasks[NR_SWAP]);
	case PSI_SWAP_FULL:
		return unlikely(stat_tasks[NR_SWAP] &&
		       nr_running == stat_tasks[NR_SWAP_RUNNING]);
	default:
		return false;
	}
}

static void psi_group_stat_change(struct psi_group *group, int cpu,
				  int clear, int set)
{
	int t;
	u32 state_mask = 0;
	enum psi_stat_states s;
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	struct psi_group_stat_cpu *ext_groupc = per_cpu_ptr(psi_ext->pcpu, cpu);

	write_seqcount_begin(&groupc->seq);
	record_stat_times(psi_ext, cpu);

	for (t = 0; clear; clear &= ~(1 << t), t++)
		if (clear & (1 << t))
			ext_groupc->tasks[t]--;
	for (t = 0; set; set &= ~(1 << t), t++)
		if (set & (1 << t))
			ext_groupc->tasks[t]++;
	for (s = 0; s < PSI_CPU_CFS_BANDWIDTH_FULL; s++)
		if (test_fine_grained_stat(ext_groupc->tasks,
					   groupc->tasks[NR_RUNNING], s))
			state_mask |= (1 << s);
	if (unlikely(groupc->state_mask & PSI_ONCPU) &&
		     cpu_curr(cpu)->memstall_type)
		state_mask |= (1 << (cpu_curr(cpu)->memstall_type * 2 - 1));

	ext_groupc->state_mask = state_mask;
	write_seqcount_end(&groupc->seq);
}

static void update_psi_stat_delta(struct psi_group *group, int cpu, u64 now)
{
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
	struct psi_group_stat_cpu *ext_groupc = per_cpu_ptr(psi_ext->pcpu, cpu);
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);

	ext_groupc->psi_delta = now - groupc->state_start;
}

static void psi_stat_flags_change(struct task_struct *task, int *stat_set,
				  int *stat_clear, int set, int clear)
{
	if (!task->memstall_type)
		return;

	if (clear) {
		if (clear & TSK_MEMSTALL)
			*stat_clear |= 1 << (2 * task->memstall_type - 2);
		if (clear & TSK_MEMSTALL_RUNNING)
			*stat_clear |= 1 << (2 * task->memstall_type - 1);
	}
	if (set) {
		if (set & TSK_MEMSTALL)
			*stat_set |= 1 << (2 * task->memstall_type - 2);
		if (set & TSK_MEMSTALL_RUNNING)
			*stat_set |= 1 << (2 * task->memstall_type - 1);
	}
	if (!task->in_memstall)
		task->memstall_type = 0;
}

static void get_recent_stat_times(struct psi_group *group, int cpu,
				  enum psi_aggregators aggregator, u32 *times)
{
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
	struct psi_group_stat_cpu *ext_groupc = per_cpu_ptr(psi_ext->pcpu, cpu);
	enum psi_stat_states s;
	u32 delta;

	memcpy(times, ext_groupc->times, sizeof(ext_groupc->times));
	for (s = 0; s < NR_PSI_STAT_STATES; s++) {
		if (ext_groupc->state_mask & (1 << s))
			times[s] += ext_groupc->times_delta;
		delta = times[s] - ext_groupc->times_prev[aggregator][s];
		ext_groupc->times_prev[aggregator][s] = times[s];
		times[s] = delta;
	}
}

static void update_stat_averages(struct psi_group_ext *psi_ext,
				 unsigned long missed_periods, u64 period)
{
	int s;

	for (s = 0; s < NR_PSI_STAT_STATES; s++) {
		u32 sample;

		sample = psi_ext->total[PSI_AVGS][s] - psi_ext->avg_total[s];
		if (sample > period)
			sample = period;
		psi_ext->avg_total[s] += sample;
		calc_avgs(psi_ext->avg[s], missed_periods, sample, period);
	}
}
#else
static inline void psi_group_stat_change(struct psi_group *group, int cpu,
					 int clear, int set) {}
static inline void update_psi_stat_delta(struct psi_group *group, int cpu,
					 u64 now) {}
static inline void psi_stat_flags_change(struct task_struct *task,
					 int *stat_set, int *stat_clear,
					 int set, int clear) {}
static inline void record_stat_times(struct psi_group_ext *psi_ext, int cpu) {}
static inline void update_stat_averages(struct psi_group_ext *psi_ext,
					unsigned long missed_periods,
					u64 period) {}
#endif

#if defined(CONFIG_CFS_BANDWIDTH) && defined(CONFIG_CGROUP_CPUACCT) && \
	defined(CONFIG_PSI_FINE_GRAINED)
static void record_cpu_stat_times(struct psi_group *group, int cpu)
{
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
	struct psi_group_cpu *groupc = per_cpu_ptr(group->pcpu, cpu);
	struct psi_group_stat_cpu *ext_groupc = per_cpu_ptr(psi_ext->pcpu, cpu);
	u32 delta = ext_groupc->psi_delta;

	if (groupc->state_mask & (1 << PSI_CPU_FULL)) {
		if (ext_groupc->prev_throttle == CPU_CFS_BANDWIDTH)
			ext_groupc->times[PSI_CPU_CFS_BANDWIDTH_FULL] += delta;
#ifdef CONFIG_QOS_SCHED
		else if (ext_groupc->prev_throttle == QOS_THROTTLED)
			ext_groupc->times[PSI_CPU_QOS_FULL] += delta;
#endif
	}
}

static void update_throttle_type(struct task_struct *task, int cpu, bool next)
{
	struct cgroup *cpuacct_cgrp;
	struct psi_group_ext *psi_ext;
	struct psi_group_stat_cpu *groupc;
	struct task_group *tsk_grp;

	if (!cgroup_subsys_on_dfl(cpuacct_cgrp_subsys)) {
		rcu_read_lock();
		cpuacct_cgrp = task_cgroup(task, cpuacct_cgrp_id);
		if (cgroup_parent(cpuacct_cgrp)) {
			psi_ext = to_psi_group_ext(cgroup_psi(cpuacct_cgrp));
			groupc = per_cpu_ptr(psi_ext->pcpu, cpu);
			tsk_grp = task_group(task);
			if (next)
				groupc->prev_throttle = groupc->cur_throttle;
			groupc->cur_throttle = tsk_grp->cfs_rq[cpu]->throttled;
		}
		rcu_read_unlock();
	}
}
#else
static inline void record_cpu_stat_times(struct psi_group *group, int cpu) {}
static inline void update_throttle_type(struct task_struct *task, int cpu,
					bool next) {}
#endif

static void collect_percpu_times(struct psi_group *group,
				 enum psi_aggregators aggregator,
				 u32 *pchanged_states)
{
#ifdef CONFIG_PSI_FINE_GRAINED
	u64 stat_delta[NR_PSI_STAT_STATES] = { 0 };
	u32 stat_times[NR_PSI_STAT_STATES] = { 0 };
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
#endif
	u64 deltas[NR_PSI_STATES - 1] = { 0, };
	unsigned long nonidle_total = 0;
	u32 changed_states = 0;
	int cpu;
	int s;

	/*
	 * Collect the per-cpu time buckets and average them into a
	 * single time sample that is normalized to wallclock time.
	 *
	 * For averaging, each CPU is weighted by its non-idle time in
	 * the sampling period. This eliminates artifacts from uneven
	 * loading, or even entirely idle CPUs.
	 */
	for_each_possible_cpu(cpu) {
		u32 times[NR_PSI_STATES];
		u32 nonidle;
		u32 cpu_changed_states;

		get_recent_times(group, cpu, aggregator, times,
				&cpu_changed_states);
		changed_states |= cpu_changed_states;

		nonidle = nsecs_to_jiffies(times[PSI_NONIDLE]);
		nonidle_total += nonidle;

		for (s = 0; s < PSI_NONIDLE; s++)
			deltas[s] += (u64)times[s] * nonidle;
#ifdef CONFIG_PSI_FINE_GRAINED
		get_recent_stat_times(group, cpu, aggregator, stat_times);
		for (s = 0; s < NR_PSI_STAT_STATES; s++)
			stat_delta[s] += (u64)stat_times[s] * nonidle;
#endif
	}

	/*
	 * Integrate the sample into the running statistics that are
	 * reported to userspace: the cumulative stall times and the
	 * decaying averages.
	 *
	 * Pressure percentages are sampled at PSI_FREQ. We might be
	 * called more often when the user polls more frequently than
	 * that; we might be called less often when there is no task
	 * activity, thus no data, and clock ticks are sporadic. The
	 * below handles both.
	 */

	/* total= */
	for (s = 0; s < NR_PSI_STATES - 1; s++)
		group->total[aggregator][s] +=
				div_u64(deltas[s], max(nonidle_total, 1UL));

#ifdef CONFIG_PSI_FINE_GRAINED
	for (s = 0; s < NR_PSI_STAT_STATES; s++)
		psi_ext->total[aggregator][s] +=
				div_u64(stat_delta[s], max(nonidle_total, 1UL));
#endif

	if (pchanged_states)
		*pchanged_states = changed_states;
}

static u64 update_averages(struct psi_group *group, u64 now)
{
	struct psi_group_ext *psi_ext = to_psi_group_ext(group);
	unsigned long missed_periods = 0;
	u64 expires, period;
	u64 avg_next_update;
	int s;

	/* avgX= */
	expires = group->avg_next_update;
	if (now - expires >= psi_period)
		missed_periods = div_u64(now - expires, psi_period);

	/*
	 * The periodic clock tick can get delayed for various
	 * reasons, especially on loaded systems. To avoid clock
	 * drift, we schedule the clock in fixed psi_period intervals.
	 * But the deltas we sample out of the per-cpu buckets above
	 * are based on the actual time elapsing between clock ticks.
	 */
	avg_next_update = expires + ((1 + missed_periods) * psi_period);
	period = now - (group->avg_last_update + (missed_periods * psi_period));
	group->avg_last_update = now;

	for (s = 0; s < NR_PSI_STATES - 1; s++) {
		u32 sample;

		sample = group->total[PSI_AVGS][s] - group->avg_total[s];
		/*
		 * Due to the lockless sampling of the time buckets,
		 * recorded time deltas can slip into the next period,
		 * which under full pressure can result in samples in
		 * excess of the period length.
		 *
		 * We don't want to report non-sensical pressures in
		 * excess of 100%, nor do we want to drop such events
		 * on the floor. Instead we punt any overage into the
		 * future until pressure subsides. By doing this we
		 * don't underreport the occurring pressure curve, we
		 * just report it delayed by one period length.
		 *
		 * The error isn't cumulative. As soon as another
		 * delta slips from a period P to P+1, by definition
		 * it frees up its time T in P.
		 */
		if (sample > period)
			sample = period;
		group->avg_total[s] += sample;
		calc_avgs(group->avg[s], missed_periods, sample, period);
	}

	update_stat_averages(psi_ext, missed_periods, period);
	return avg_next_update;
}

static void psi_avgs_work(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct psi_group *group;
	u32 changed_states;
	bool nonidle;
	u64 now;

	dwork = to_delayed_work(work);
	group = container_of(dwork, struct psi_group, avgs_work);

	mutex_lock(&group->avgs_lock);

	now = sched_clock();

	collect_percpu_times(group, PSI_AVGS, &changed_states);
	nonidle = changed_states & (1 << PSI_NONIDLE);
	/*
	 * If there is task activity, periodically fold the per-cpu
	 * times and feed samples into the running averages. If things
	 * are idle and there is no data to process, stop the clock.
	 * Once restarted, we'll catch up the running averages in one
	 * go - see calc_avgs() and missed_periods.
	 */
	if (now >= group->avg_next_update)
		group->avg_next_update = update_averages(group, now);

	if (nonidle) {
		schedule_delayed_work(dwork, nsecs_to_jiffies(
				group->avg_next_update - now) + 1);
	}

	mutex_unlock(&group->avgs_lock);
}

/* Trigger tracking window manupulations */
static void window_reset(struct psi_window *win, u64 now, u64 value,
			 u64 prev_growth)
{
	win->start_time = now;
	win->start_value = value;
	win->prev_growth = prev_growth;
}

/*
 * PSI growth tracking window update and growth calculation routine.
 *
 * This approximates a sliding tracking window by interpolating
 * partially elapsed windows using historical growth data from the
 * previous intervals. This minimizes memory requirements (by not storing
 * all the intermediate values in the previous window) and simplifies
 * the calculations. It works well because PSI signal changes only in
 * positive direction and over relatively small window sizes the growth
 * is close to linear.
 */
static u64 window_update(struct psi_window *win, u64 now, u64 value)
{
	u64 elapsed;
	u64 growth;

	elapsed = now - win->start_time;
	growth = value - win->start_value;
	/*
	 * After each tracking window passes win->start_value and
	 * win->start_time get reset and win->prev_growth stores
	 * the average per-window growth of the previous window.
	 * win->prev_growth is then used to interpolate additional
	 * growth from the previous window assuming it was linear.
	 */
	if (elapsed > win->size)
		window_reset(win, now, value, growth);
	else {
		u32 remaining;

		remaining = win->size - elapsed;
		growth += div64_u64(win->prev_growth * remaining, win->size);
	}

	return growth;
}

static void init_triggers(struct psi_group *group, u64 now)
{
	struct psi_trigger *t;

	list_for_each_entry(t, &group->triggers, node)
		window_reset(&t->win, now,
				group->total[PSI_POLL][t->state], 0);
	memcpy(group->polling_total, group->total[PSI_POLL],
		   sizeof(group->polling_total));
	group->polling_next_update = now + group->poll_min_period;
}

static u64 update_triggers(struct psi_group *group, u64 now)
{
	struct psi_trigger *t;
	bool new_stall = false;
	u64 *total = group->total[PSI_POLL];

	/*
	 * On subsequent updates, calculate growth deltas and let
	 * watchers know when their specified thresholds are exceeded.
	 */
	list_for_each_entry(t, &group->triggers, node) {
		u64 growth;

		/* Check for stall activity */
		if (group->polling_total[t->state] == total[t->state])
			continue;

		/*
		 * Multiple triggers might be looking at the same state,
		 * remember to update group->polling_total[] once we've
		 * been through all of them. Also remember to extend the
		 * polling time if we see new stall activity.
		 */
		new_stall = true;

		/* Calculate growth since last update */
		growth = window_update(&t->win, now, total[t->state]);
		if (growth < t->threshold)
			continue;

		/* Limit event signaling to once per window */
		if (now < t->last_event_time + t->win.size)
			continue;

		/* Generate an event */
		if (cmpxchg(&t->event, 0, 1) == 0) {
			if (t->of)
				kernfs_notify(t->of->kn);
			else
				wake_up_interruptible(&t->event_wait);
		}
		t->last_event_time = now;
	}

	if (new_stall)
		memcpy(group->polling_total, total,
				sizeof(group->polling_total));

	return now + group->poll_min_period;
}

/* Schedule polling if it's not already scheduled. */
static void psi_schedule_poll_work(struct psi_group *group, unsigned long delay)
{
	struct task_struct *task;

	/*
	 * Do not reschedule if already scheduled.
	 * Possible race with a timer scheduled after this check but before
	 * mod_timer below can be tolerated because group->polling_next_update
	 * will keep updates on schedule.
	 */
	if (timer_pending(&group->poll_timer))
		return;

	rcu_read_lock();

	task = rcu_dereference(group->poll_task);
	/*
	 * kworker might be NULL in case psi_trigger_destroy races with
	 * psi_task_change (hotpath) which can't use locks
	 */
	if (likely(task))
		mod_timer(&group->poll_timer, jiffies + delay);

	rcu_read_unlock();
}

static void psi_poll_work(struct psi_group *group)
{
	u32 changed_states;
	u64 now;

	mutex_lock(&group->trigger_lock);

	now = sched_clock();

	collect_percpu_times(group, PSI_POLL, &changed_states);

	if (changed_states & group->poll_states) {
		/* Initialize trigger windows when entering polling mode */
		if (now > group->polling_until)
			init_triggers(group, now);

		/*
		 * Keep the monitor active for at least the duration of the
		 * minimum tracking window as long as monitor states are
		 * changing.
		 */
		group->polling_until = now +
			group->poll_min_period * UPDATES_PER_WINDOW;
	}

	if (now > group->polling_until) {
		group->polling_next_update = ULLONG_MAX;
		goto out;
	}

	if (now >= group->polling_next_update)
		group->polling_next_update = update_triggers(group, now);

	psi_schedule_poll_work(group,
		nsecs_to_jiffies(group->polling_next_update - now) + 1);

out:
	mutex_unlock(&group->trigger_lock);
}

static int psi_poll_worker(void *data)
{
	struct psi_group *group = (struct psi_group *)data;

	sched_set_fifo_low(current);

	while (true) {
		wait_event_interruptible(group->poll_wait,
				atomic_cmpxchg(&group->poll_wakeup, 1, 0) ||
				kthread_should_stop());
		if (kthread_should_stop())
			break;

		psi_poll_work(group);
	}
	return 0;
}

static void poll_timer_fn(struct timer_list *t)
{
	struct psi_group *group = from_timer(group, t, poll_timer);

	atomic_set(&group->poll_wakeup, 1);
	wake_up_interruptible(&group->poll_wait);
}

static void record_times(struct psi_group_cpu *groupc, u64 now)
{
	u32 delta;

	delta = now - groupc->state_start;
	groupc->state_start = now;

	if (groupc->state_mask & (1 << PSI_IO_SOME)) {
		groupc->times[PSI_IO_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_IO_FULL))
			groupc->times[PSI_IO_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_MEM_SOME)) {
		groupc->times[PSI_MEM_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_MEM_FULL))
			groupc->times[PSI_MEM_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_CPU_SOME)) {
		groupc->times[PSI_CPU_SOME] += delta;
		if (groupc->state_mask & (1 << PSI_CPU_FULL))
			groupc->times[PSI_CPU_FULL] += delta;
	}

	if (groupc->state_mask & (1 << PSI_NONIDLE))
		groupc->times[PSI_NONIDLE] += delta;
}

static void psi_group_change(struct psi_group *group, int cpu,
			     unsigned int clear, unsigned int set, u64 now,
			     bool wake_clock)
{
	struct psi_group_cpu *groupc;
	unsigned int t, m;
	enum psi_states s;
	u32 state_mask;

	groupc = per_cpu_ptr(group->pcpu, cpu);

	/*
	 * First we assess the aggregate resource states this CPU's
	 * tasks have been in since the last change, and account any
	 * SOME and FULL time these may have resulted in.
	 *
	 * Then we update the task counts according to the state
	 * change requested through the @clear and @set bits.
	 */
	write_seqcount_begin(&groupc->seq);

	record_times(groupc, now);
	record_cpu_stat_times(group, cpu);

	/*
	 * Start with TSK_ONCPU, which doesn't have a corresponding
	 * task count - it's just a boolean flag directly encoded in
	 * the state mask. Clear, set, or carry the current state if
	 * no changes are requested.
	 */
	if (unlikely(clear & TSK_ONCPU)) {
		state_mask = 0;
		clear &= ~TSK_ONCPU;
	} else if (unlikely(set & TSK_ONCPU)) {
		state_mask = PSI_ONCPU;
		set &= ~TSK_ONCPU;
	} else {
		state_mask = groupc->state_mask & PSI_ONCPU;
	}

	/*
	 * The rest of the state mask is calculated based on the task
	 * counts. Update those first, then construct the mask.
	 */
	for (t = 0, m = clear; m; m &= ~(1 << t), t++) {
		if (!(m & (1 << t)))
			continue;
		if (groupc->tasks[t]) {
			groupc->tasks[t]--;
		} else if (!psi_bug) {
			printk_deferred(KERN_ERR "psi: task underflow! cpu=%d t=%d tasks=[%u %u %u %u] clear=%x set=%x\n",
					cpu, t, groupc->tasks[0],
					groupc->tasks[1], groupc->tasks[2],
					groupc->tasks[3], clear, set);
			psi_bug = 1;
		}
	}

	for (t = 0; set; set &= ~(1 << t), t++)
		if (set & (1 << t))
			groupc->tasks[t]++;

	for (s = 0; s < NR_PSI_STATES; s++) {
		if (test_state(groupc->tasks, s, state_mask & PSI_ONCPU))
			state_mask |= (1 << s);
	}

	/*
	 * Since we care about lost potential, a memstall is FULL
	 * when there are no other working tasks, but also when
	 * the CPU is actively reclaiming and nothing productive
	 * could run even if it were runnable. So when the current
	 * task in a cgroup is in_memstall, the corresponding groupc
	 * on that cpu is in PSI_MEM_FULL state.
	 */
	if (unlikely((state_mask & PSI_ONCPU) && cpu_curr(cpu)->in_memstall))
		state_mask |= (1 << PSI_MEM_FULL);

	groupc->state_mask = state_mask;

	write_seqcount_end(&groupc->seq);

	if (state_mask & group->poll_states)
		psi_schedule_poll_work(group, 1);

	if (wake_clock && !delayed_work_pending(&group->avgs_work))
		schedule_delayed_work(&group->avgs_work, PSI_FREQ);
}

static struct psi_group *iterate_groups(struct task_struct *task, void **iter)
{
#ifdef CONFIG_CGROUPS
	struct cgroup *cgroup = NULL;

	if (!*iter) {
#ifndef CONFIG_PSI_CGROUP_V1
		cgroup = task->cgroups->dfl_cgrp;
#else
#ifdef CONFIG_CGROUP_CPUACCT
		if (!cgroup_subsys_on_dfl(cpuacct_cgrp_subsys)) {
			if (!static_branch_likely(&psi_v1_disabled)) {
				rcu_read_lock();
				cgroup = task_cgroup(task, cpuacct_cgrp_id);
				rcu_read_unlock();
			}
		} else {
			cgroup = task->cgroups->dfl_cgrp;
		}
#else
		cgroup = NULL;
#endif
#endif
	} else if (*iter == &psi_system)
		return NULL;
	else
		cgroup = cgroup_parent(*iter);

	if (cgroup && cgroup_parent(cgroup)) {
		*iter = cgroup;
		return cgroup_psi(cgroup);
	}
#else
	if (*iter)
		return NULL;
#endif
	*iter = &psi_system;
	return &psi_system;
}

static void psi_flags_change(struct task_struct *task, int clear, int set)
{
	if (((task->psi_flags & set) ||
	     (task->psi_flags & clear) != clear) &&
	    !psi_bug) {
		printk_deferred(KERN_ERR "psi: inconsistent task state! task=%d:%s cpu=%d psi_flags=%x clear=%x set=%x\n",
				task->pid, task->comm, task_cpu(task),
				task->psi_flags, clear, set);
		psi_bug = 1;
	}

	task->psi_flags &= ~clear;
	task->psi_flags |= set;
}

void psi_task_change(struct task_struct *task, int clear, int set)
{
	int cpu = task_cpu(task);
	struct psi_group *group;
	void *iter = NULL;
	u64 now;
	int stat_set = 0;
	int stat_clear = 0;

	if (!task->pid)
		return;

	psi_flags_change(task, clear, set);
	psi_stat_flags_change(task, &stat_set, &stat_clear, set, clear);

	now = cpu_clock(cpu);

	while ((group = iterate_groups(task, &iter))) {
		update_psi_stat_delta(group, cpu, now);
		psi_group_change(group, cpu, clear, set, now, true);
		psi_group_stat_change(group, cpu, stat_clear, stat_set);
	}
}

void psi_task_switch(struct task_struct *prev, struct task_struct *next,
		     bool sleep)
{
	struct psi_group *group, *common = NULL;
	int cpu = task_cpu(prev);
	void *iter;
	u64 now = cpu_clock(cpu);

	if (next->pid) {
		update_throttle_type(next, cpu, true);
		psi_flags_change(next, 0, TSK_ONCPU);
		/*
		 * Set TSK_ONCPU on @next's cgroups. If @next shares any
		 * ancestors with @prev, those will already have @prev's
		 * TSK_ONCPU bit set, and we can stop the iteration there.
		 */
		iter = NULL;
		while ((group = iterate_groups(next, &iter))) {
			if (per_cpu_ptr(group->pcpu, cpu)->state_mask &
			    PSI_ONCPU) {
				common = group;
				break;
			}

			update_psi_stat_delta(group, cpu, now);
			psi_group_change(group, cpu, 0, TSK_ONCPU, now, true);
			psi_group_stat_change(group, cpu, 0, 0);
		}
	}

	if (prev->pid) {
		int clear = TSK_ONCPU, set = 0;
		bool wake_clock = true;
		int stat_set = 0;
		int stat_clear = 0;
		bool memstall_type_change = false;

		update_throttle_type(prev, cpu, false);
		/*
		 * When we're going to sleep, psi_dequeue() lets us
		 * handle TSK_RUNNING, TSK_MEMSTALL_RUNNING and
		 * TSK_IOWAIT here, where we can combine it with
		 * TSK_ONCPU and save walking common ancestors twice.
		 */
		if (sleep) {
			clear |= TSK_RUNNING;
			if (prev->in_memstall)
				clear |= TSK_MEMSTALL_RUNNING;
			if (prev->in_iowait)
				set |= TSK_IOWAIT;

			/*
			 * Periodic aggregation shuts off if there is a period of no
			 * task changes, so we wake it back up if necessary. However,
			 * don't do this if the task change is the aggregation worker
			 * itself going to sleep, or we'll ping-pong forever.
			 */
			if (unlikely((prev->flags & PF_WQ_WORKER) &&
				     wq_worker_last_func(prev) == psi_avgs_work))
				wake_clock = false;
		}

		psi_flags_change(prev, clear, set);
		psi_stat_flags_change(prev, &stat_set, &stat_clear, set, clear);

		iter = NULL;
		while ((group = iterate_groups(prev, &iter)) && group != common) {
			update_psi_stat_delta(group, cpu, now);
			psi_group_change(group, cpu, clear, set, now, wake_clock);
			psi_group_stat_change(group, cpu, stat_clear, stat_set);
		}
#ifdef CONFIG_PSI_FINE_GRAINED
		if (next->memstall_type != prev->memstall_type)
			memstall_type_change = true;
#endif
		/*
		 * TSK_ONCPU is handled up to the common ancestor. If there are
		 * any other differences between the two tasks (e.g. prev goes
		 * to sleep, or only one task is memstall), finish propagating
		 * those differences all the way up to the root.
		 */
		if ((prev->psi_flags ^ next->psi_flags) & ~TSK_ONCPU ||
		     memstall_type_change) {
			clear &= ~TSK_ONCPU;
			for (; group; group = iterate_groups(prev, &iter)) {
				update_psi_stat_delta(group, cpu, now);
				psi_group_change(group, cpu, clear, set, now, wake_clock);
				psi_group_stat_change(group, cpu, stat_clear,
						      stat_set);
			}
		}
	}
}

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
void psi_account_irqtime(struct task_struct *task, u32 delta)
{
	int cpu = task_cpu(task);
	void *iter = NULL;
	struct psi_group *group;
	struct psi_group_cpu *groupc;
	u64 now;

	if (static_branch_likely(&psi_disabled))
		return;

	if (!task->pid)
		return;

	now = cpu_clock(cpu);

	while ((group = iterate_groups(task, &iter))) {
		groupc = per_cpu_ptr(group->pcpu, cpu);

		write_seqcount_begin(&groupc->seq);

		update_psi_stat_delta(group, cpu, now);
		record_stat_times(to_psi_group_ext(group), cpu);
		record_times(groupc, now);
		record_cpu_stat_times(group, cpu);
		groupc->times[PSI_IRQ_FULL] += delta;

		write_seqcount_end(&groupc->seq);

		if (group->poll_states & (1 << PSI_IRQ_FULL))
			psi_schedule_poll_work(group, 1);
	}
}
#endif

/**
 * psi_memstall_enter - mark the beginning of a memory stall section
 * @flags: flags to handle nested sections
 *
 * Marks the calling task as being stalled due to a lack of memory,
 * such as waiting for a refault or performing reclaim.
 */
void psi_memstall_enter(unsigned long *flags)
{
	struct rq_flags rf;
	struct rq *rq;
#ifdef CONFIG_PSI_FINE_GRAINED
	unsigned long stat_flags = *flags;
#endif

	if (static_branch_likely(&psi_disabled))
		return;

	*flags = current->in_memstall;
	if (*flags)
		return;

	trace_psi_memstall_enter(_RET_IP_);
	/*
	 * in_memstall setting & accounting needs to be atomic wrt
	 * changes to the task's scheduling state, otherwise we can
	 * race with CPU migration.
	 */
	rq = this_rq_lock_irq(&rf);

	current->in_memstall = 1;
#ifdef CONFIG_PSI_FINE_GRAINED
	if (stat_flags)
		current->memstall_type = stat_flags;
#endif
	psi_task_change(current, 0, TSK_MEMSTALL | TSK_MEMSTALL_RUNNING);

	rq_unlock_irq(rq, &rf);
}

/**
 * psi_memstall_leave - mark the end of an memory stall section
 * @flags: flags to handle nested memdelay sections
 *
 * Marks the calling task as no longer stalled due to lack of memory.
 */
void psi_memstall_leave(unsigned long *flags)
{
	struct rq_flags rf;
	struct rq *rq;

	if (static_branch_likely(&psi_disabled))
		return;

	if (*flags)
		return;

	trace_psi_memstall_leave(_RET_IP_);

	/*
	 * in_memstall clearing & accounting needs to be atomic wrt
	 * changes to the task's scheduling state, otherwise we could
	 * race with CPU migration.
	 */
	rq = this_rq_lock_irq(&rf);

	current->in_memstall = 0;
	psi_task_change(current, TSK_MEMSTALL | TSK_MEMSTALL_RUNNING, 0);

	rq_unlock_irq(rq, &rf);
}

#ifdef CONFIG_CGROUPS
int psi_cgroup_alloc(struct cgroup *cgroup)
{
#ifdef CONFIG_PSI_FINE_GRAINED
	struct psi_group_ext *psi_ext;
#endif

	if (static_branch_likely(&psi_disabled))
		return 0;

#ifdef CONFIG_PSI_FINE_GRAINED
	psi_ext = kzalloc(sizeof(struct psi_group_ext), GFP_KERNEL);
	if (!psi_ext)
		return -ENOMEM;
	psi_ext->pcpu = alloc_percpu(struct psi_group_stat_cpu);
	if (!psi_ext->pcpu) {
		kfree(psi_ext);
		return -ENOMEM;
	}
	cgroup->psi = &psi_ext->psi;
#else
	cgroup->psi = kzalloc(sizeof(struct psi_group), GFP_KERNEL);
	if (!cgroup->psi)
		return -ENOMEM;

#endif
	cgroup->psi->pcpu = alloc_percpu(struct psi_group_cpu);
	if (!cgroup->psi->pcpu) {
#ifdef CONFIG_PSI_FINE_GRAINED
		free_percpu(psi_ext->pcpu);
		kfree(psi_ext);
#else
		kfree(cgroup->psi);
#endif
		return -ENOMEM;
	}
	group_init(cgroup->psi);
	return 0;
}

void psi_cgroup_free(struct cgroup *cgroup)
{
	if (static_branch_likely(&psi_disabled))
		return;

	cancel_delayed_work_sync(&cgroup->psi->avgs_work);
	free_percpu(cgroup->psi->pcpu);
	/* All triggers must be removed by now */
	WARN_ONCE(cgroup->psi->poll_states, "psi: trigger leak\n");
#ifdef CONFIG_PSI_FINE_GRAINED
	free_percpu(to_psi_group_ext(cgroup->psi)->pcpu);
	kfree(to_psi_group_ext(cgroup->psi));
#else
	kfree(cgroup->psi);
#endif
}

/**
 * cgroup_move_task - move task to a different cgroup
 * @task: the task
 * @to: the target css_set
 *
 * Move task to a new cgroup and safely migrate its associated stall
 * state between the different groups.
 *
 * This function acquires the task's rq lock to lock out concurrent
 * changes to the task's scheduling state and - in case the task is
 * running - concurrent changes to its stall state.
 */
void cgroup_move_task(struct task_struct *task, struct css_set *to)
{
	unsigned int task_flags;
	struct rq_flags rf;
	struct rq *rq;

	if (static_branch_likely(&psi_disabled)) {
		/*
		 * Lame to do this here, but the scheduler cannot be locked
		 * from the outside, so we move cgroups from inside sched/.
		 */
		rcu_assign_pointer(task->cgroups, to);
		return;
	}

	rq = task_rq_lock(task, &rf);

	/*
	 * We may race with schedule() dropping the rq lock between
	 * deactivating prev and switching to next. Because the psi
	 * updates from the deactivation are deferred to the switch
	 * callback to save cgroup tree updates, the task's scheduling
	 * state here is not coherent with its psi state:
	 *
	 * schedule()                   cgroup_move_task()
	 *   rq_lock()
	 *   deactivate_task()
	 *     p->on_rq = 0
	 *     psi_dequeue() // defers TSK_RUNNING & TSK_IOWAIT updates
	 *   pick_next_task()
	 *     rq_unlock()
	 *                                rq_lock()
	 *                                psi_task_change() // old cgroup
	 *                                task->cgroups = to
	 *                                psi_task_change() // new cgroup
	 *                                rq_unlock()
	 *     rq_lock()
	 *   psi_sched_switch() // does deferred updates in new cgroup
	 *
	 * Don't rely on the scheduling state. Use psi_flags instead.
	 */
	task_flags = task->psi_flags;

	if (task_flags)
		psi_task_change(task, task_flags, 0);

	/* See comment above */
	rcu_assign_pointer(task->cgroups, to);

	if (task_flags)
		psi_task_change(task, 0, task_flags);

	task_rq_unlock(rq, task, &rf);
}
#endif /* CONFIG_CGROUPS */

int psi_show(struct seq_file *m, struct psi_group *group, enum psi_res res)
{
	bool only_full = false;
	int full;
	u64 now;

	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	/* Update averages before reporting them */
	mutex_lock(&group->avgs_lock);
	now = sched_clock();
	collect_percpu_times(group, PSI_AVGS, NULL);
	if (now >= group->avg_next_update)
		group->avg_next_update = update_averages(group, now);
	mutex_unlock(&group->avgs_lock);

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	only_full = res == PSI_IRQ;
#endif

	for (full = 0; full < 2 - only_full; full++) {
		unsigned long avg[3] = { 0, };
		u64 total = 0;
		int w;

		/* CPU FULL is undefined at the system level */
		if (!(group == &psi_system && res == PSI_CPU && full)) {
			for (w = 0; w < 3; w++)
				avg[w] = group->avg[res * 2 + full][w];
			total = div_u64(group->total[PSI_AVGS][res * 2 + full],
					NSEC_PER_USEC);
		}

		seq_printf(m, "%s avg10=%lu.%02lu avg60=%lu.%02lu avg300=%lu.%02lu total=%llu\n",
			   full || only_full ? "full" : "some",
			   LOAD_INT(avg[0]), LOAD_FRAC(avg[0]),
			   LOAD_INT(avg[1]), LOAD_FRAC(avg[1]),
			   LOAD_INT(avg[2]), LOAD_FRAC(avg[2]),
			   total);
	}

	return 0;
}

struct psi_trigger *psi_trigger_create(struct psi_group *group, char *buf,
				       size_t nbytes, enum psi_res res,
				       struct kernfs_open_file *of)
{
	struct psi_trigger *t;
	enum psi_states state;
	u32 threshold_us;
	u32 window_us;

	if (static_branch_likely(&psi_disabled))
		return ERR_PTR(-EOPNOTSUPP);

	if (sscanf(buf, "some %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_SOME + res * 2;
	else if (sscanf(buf, "full %u %u", &threshold_us, &window_us) == 2)
		state = PSI_IO_FULL + res * 2;
	else
		return ERR_PTR(-EINVAL);

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	if (res == PSI_IRQ && --state != PSI_IRQ_FULL)
		return ERR_PTR(-EINVAL);
#endif

	if (state >= PSI_NONIDLE)
		return ERR_PTR(-EINVAL);

	if (window_us < WINDOW_MIN_US ||
		window_us > WINDOW_MAX_US)
		return ERR_PTR(-EINVAL);

	/* Check threshold */
	if (threshold_us == 0 || threshold_us > window_us)
		return ERR_PTR(-EINVAL);

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return ERR_PTR(-ENOMEM);

	t->group = group;
	t->state = state;
	t->threshold = threshold_us * NSEC_PER_USEC;
	t->win.size = window_us * NSEC_PER_USEC;
	window_reset(&t->win, 0, 0, 0);

	t->event = 0;
	t->last_event_time = 0;
	t->of = of;
	if (!of)
		init_waitqueue_head(&t->event_wait);

	mutex_lock(&group->trigger_lock);

	if (!rcu_access_pointer(group->poll_task)) {
		struct task_struct *task;

		task = kthread_create(psi_poll_worker, group, "psimon");
		if (IS_ERR(task)) {
			kfree(t);
			mutex_unlock(&group->trigger_lock);
			return ERR_CAST(task);
		}
		atomic_set(&group->poll_wakeup, 0);
		wake_up_process(task);
		rcu_assign_pointer(group->poll_task, task);
	}

	list_add(&t->node, &group->triggers);
	group->poll_min_period = min(group->poll_min_period,
		div_u64(t->win.size, UPDATES_PER_WINDOW));
	group->nr_triggers[t->state]++;
	group->poll_states |= (1 << t->state);

	mutex_unlock(&group->trigger_lock);

	return t;
}

void psi_trigger_destroy(struct psi_trigger *t)
{
	struct psi_group *group;
	struct task_struct *task_to_destroy = NULL;

	/*
	 * We do not check psi_disabled since it might have been disabled after
	 * the trigger got created.
	 */
	if (!t)
		return;

	group = t->group;
	/*
	 * Wakeup waiters to stop polling and clear the queue to prevent it from
	 * being accessed later. Can happen if cgroup is deleted from under a
	 * polling process.
	 */
	if (t->of)
		kernfs_notify(t->of->kn);
	else
		wake_up_interruptible(&t->event_wait);

	mutex_lock(&group->trigger_lock);

	if (!list_empty(&t->node)) {
		struct psi_trigger *tmp;
		u64 period = ULLONG_MAX;

		list_del(&t->node);
		group->nr_triggers[t->state]--;
		if (!group->nr_triggers[t->state])
			group->poll_states &= ~(1 << t->state);
		/* reset min update period for the remaining triggers */
		list_for_each_entry(tmp, &group->triggers, node)
			period = min(period, div_u64(tmp->win.size,
					UPDATES_PER_WINDOW));
		group->poll_min_period = period;
		/* Destroy poll_task when the last trigger is destroyed */
		if (group->poll_states == 0) {
			group->polling_until = 0;
			task_to_destroy = rcu_dereference_protected(
					group->poll_task,
					lockdep_is_held(&group->trigger_lock));
			rcu_assign_pointer(group->poll_task, NULL);
			del_timer(&group->poll_timer);
		}
	}

	mutex_unlock(&group->trigger_lock);

	/*
	 * Wait for psi_schedule_poll_work RCU to complete its read-side
	 * critical section before destroying the trigger and optionally the
	 * poll_task.
	 */
	synchronize_rcu();
	/*
	 * Stop kthread 'psimon' after releasing trigger_lock to prevent a
	 * deadlock while waiting for psi_poll_work to acquire trigger_lock
	 */
	if (task_to_destroy) {
		/*
		 * After the RCU grace period has expired, the worker
		 * can no longer be found through group->poll_task.
		 */
		kthread_stop(task_to_destroy);
	}
	kfree(t);
}

__poll_t psi_trigger_poll(void **trigger_ptr,
				struct file *file, poll_table *wait)
{
	__poll_t ret = DEFAULT_POLLMASK;
	struct psi_trigger *t;

	if (static_branch_likely(&psi_disabled))
		return DEFAULT_POLLMASK | EPOLLERR | EPOLLPRI;

	t = smp_load_acquire(trigger_ptr);
	if (!t)
		return DEFAULT_POLLMASK | EPOLLERR | EPOLLPRI;

	if (t->of)
		kernfs_generic_poll(t->of, wait);
	else
		poll_wait(file, &t->event_wait, wait);

	if (cmpxchg(&t->event, 1, 0) == 1)
		ret |= EPOLLPRI;

	return ret;
}

#ifdef CONFIG_PROC_FS
static int psi_io_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_IO);
}

static int psi_memory_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_MEM);
}

static int psi_cpu_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_CPU);
}

static int psi_io_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_io_show, NULL);
}

static int psi_memory_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_memory_show, NULL);
}

static int psi_cpu_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_cpu_show, NULL);
}

static ssize_t psi_write(struct file *file, const char __user *user_buf,
			 size_t nbytes, enum psi_res res)
{
	char buf[32];
	size_t buf_size;
	struct seq_file *seq;
	struct psi_trigger *new;

	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	if (!nbytes)
		return -EINVAL;

	buf_size = min(nbytes, sizeof(buf));
	if (copy_from_user(buf, user_buf, buf_size))
		return -EFAULT;

	buf[buf_size - 1] = '\0';

	seq = file->private_data;

	/* Take seq->lock to protect seq->private from concurrent writes */
	mutex_lock(&seq->lock);

	/* Allow only one trigger per file descriptor */
	if (seq->private) {
		mutex_unlock(&seq->lock);
		return -EBUSY;
	}

	new = psi_trigger_create(&psi_system, buf, nbytes, res, NULL);
	if (IS_ERR(new)) {
		mutex_unlock(&seq->lock);
		return PTR_ERR(new);
	}

	smp_store_release(&seq->private, new);
	mutex_unlock(&seq->lock);

	return nbytes;
}

static ssize_t psi_io_write(struct file *file, const char __user *user_buf,
			    size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_IO);
}

static ssize_t psi_memory_write(struct file *file, const char __user *user_buf,
				size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_MEM);
}

static ssize_t psi_cpu_write(struct file *file, const char __user *user_buf,
			     size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_CPU);
}

static __poll_t psi_fop_poll(struct file *file, poll_table *wait)
{
	struct seq_file *seq = file->private_data;

	return psi_trigger_poll(&seq->private, file, wait);
}

static int psi_fop_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;

	psi_trigger_destroy(seq->private);
	return single_release(inode, file);
}

static const struct proc_ops psi_io_proc_ops = {
	.proc_open	= psi_io_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_io_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};

static const struct proc_ops psi_memory_proc_ops = {
	.proc_open	= psi_memory_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_memory_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};

static const struct proc_ops psi_cpu_proc_ops = {
	.proc_open	= psi_cpu_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_cpu_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};

#ifdef CONFIG_PSI_FINE_GRAINED
static const char *const psi_stat_names[] = {
	"cgroup_memory_reclaim",
	"global_memory_reclaim",
	"compact",
	"cgroup_async_memory_reclaim",
	"swap",
	"cpu_cfs_bandwidth",
	"cpu_qos",
};

static void get_stat_names(struct seq_file *m, int i, bool is_full)
{
	if (i <= PSI_SWAP_FULL && !is_full)
		return seq_printf(m, "%s\n", psi_stat_names[i / 2]);
	else if (i == PSI_CPU_CFS_BANDWIDTH_FULL)
		return seq_printf(m, "%s\n", "cpu_cfs_bandwidth");
#ifdef CONFIG_QOS_SCHED
	else if (i == PSI_CPU_QOS_FULL)
		return seq_printf(m, "%s\n", "cpu_qos");
#endif
}

int psi_stat_show(struct seq_file *m, struct psi_group *group)
{
	struct psi_group_ext *psi_ext;
	unsigned long avg[3] = {0, };
	int i, w;
	bool is_full;
	u64 now, total;

	if (static_branch_likely(&psi_disabled))
		return -EOPNOTSUPP;

	psi_ext = to_psi_group_ext(group);
	mutex_lock(&group->avgs_lock);
	now = sched_clock();
	collect_percpu_times(group, PSI_AVGS, NULL);
	if (now >= group->avg_next_update)
		group->avg_next_update = update_averages(group, now);
	mutex_unlock(&group->avgs_lock);
	for (i = 0; i < NR_PSI_STAT_STATES; i++) {
		is_full = i % 2 || i > PSI_SWAP_FULL;
		for (w = 0; w < 3; w++)
			avg[w] = psi_ext->avg[i][w];
		total = div_u64(psi_ext->total[PSI_AVGS][i], NSEC_PER_USEC);
		get_stat_names(m, i, is_full);
		seq_printf(m, "%s avg10=%lu.%02lu avg60=%lu.%02lu avg300=%lu.%02lu total=%llu\n",
			   is_full ? "full" : "some",
			   LOAD_INT(avg[0]), LOAD_FRAC(avg[0]),
			   LOAD_INT(avg[1]), LOAD_FRAC(avg[1]),
			   LOAD_INT(avg[2]), LOAD_FRAC(avg[2]),
			   total);
	}
	return 0;
}
static int system_psi_stat_show(struct seq_file *m, void *v)
{
	return psi_stat_show(m, &psi_system);
}

static int psi_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, system_psi_stat_show, NULL);
}

static const struct proc_ops psi_stat_proc_ops = {
	.proc_open	= psi_stat_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= psi_fop_release,
};
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
static int psi_irq_show(struct seq_file *m, void *v)
{
	return psi_show(m, &psi_system, PSI_IRQ);
}

static int psi_irq_open(struct inode *inode, struct file *file)
{
	return single_open(file, psi_irq_show, NULL);
}

static ssize_t psi_irq_write(struct file *file, const char __user *user_buf,
			     size_t nbytes, loff_t *ppos)
{
	return psi_write(file, user_buf, nbytes, PSI_IRQ);
}

static const struct proc_ops psi_irq_proc_ops = {
	.proc_open	= psi_irq_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= psi_irq_write,
	.proc_poll	= psi_fop_poll,
	.proc_release	= psi_fop_release,
};
#endif

static int __init psi_proc_init(void)
{
	if (psi_enable) {
		proc_mkdir("pressure", NULL);
		proc_create("pressure/io", 0, NULL, &psi_io_proc_ops);
		proc_create("pressure/memory", 0, NULL, &psi_memory_proc_ops);
		proc_create("pressure/cpu", 0, NULL, &psi_cpu_proc_ops);
#ifdef CONFIG_IRQ_TIME_ACCOUNTING
		proc_create("pressure/irq", 0, NULL, &psi_irq_proc_ops);
#endif
#ifdef CONFIG_PSI_FINE_GRAINED
		proc_create("pressure/stat", 0, NULL, &psi_stat_proc_ops);
#endif
	}
	return 0;
}
module_init(psi_proc_init);

#endif /* CONFIG_PROC_FS */