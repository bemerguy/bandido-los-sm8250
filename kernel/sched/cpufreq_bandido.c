/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sched.h"

#include <linux/sched/cpufreq.h>
#include <trace/events/power.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/mm.h>

extern bool lcd_is_on;

struct sugov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		rtg_boost_freq;
};

struct sugov_policy {
	struct cpufreq_policy	*policy;

	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;
	unsigned long		rtg_boost_util;
	unsigned long		max;
	unsigned long		dvfs_capacity;
	unsigned long		target_load_thresh;
	u16			dvfs_headroom_lut[SCHED_CAPACITY_SCALE + 1];

	raw_spinlock_t		update_lock;	/* For shared policies */
	u64			last_freq_update_time;
	s64			min_rate_limit_ns;
	s64			down_rate_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;
	unsigned int		prev_cached_raw_freq;

	bool			limits_changed;
	bool			need_freq_update;
};

struct sugov_cpu {
	struct update_util_data	update_util;
	struct sugov_policy	*sg_policy;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	struct sched_walt_cpu_load walt_load;

	unsigned long util;
	unsigned int flags;

	unsigned long		bw_dl;
	unsigned long		min;
	unsigned long		max;
	u16			*dvfs_headroom_lut;

#ifdef CONFIG_BANDIDO_UI_BOOST
	u64			last_ui_time;
#endif
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);
static DEFINE_PER_CPU(struct sugov_tunables *, cached_tunables);
static unsigned int stale_ns;

/************************ Governor internals ***********************/

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	if (!cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;

	if (unlikely(sg_policy->limits_changed)) {
		sg_policy->limits_changed = false;
		sg_policy->need_freq_update = true;
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static inline bool use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return false;
#else
	return true;
#endif
}

static bool sugov_up_down_rate_limit(struct sugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns = time - sg_policy->last_freq_update_time;

	/* 20ms ramp-up delay when screen is off to save battery */
	if (!lcd_is_on && next_freq > sg_policy->next_freq && delta_ns < 20000000)
		return true;

	/* Never delay ramp-up — respond instantly to load spikes */
	if (next_freq >= sg_policy->next_freq)
		return false;

	/* Delay ramp-down to prevent stutter on bursty workloads */
	return delta_ns < sg_policy->down_rate_delay_ns;
}

static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
				   unsigned int next_freq)
{
	if (sg_policy->next_freq == next_freq)
		return false;

	if (sugov_up_down_rate_limit(sg_policy, time, next_freq)) {
		/* Restore cached freq as next_freq is not changed */
		sg_policy->cached_raw_freq = sg_policy->prev_cached_raw_freq;
		return false;
	}

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

static void sugov_fast_switch(struct sugov_policy *sg_policy, u64 time,
			      unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;

	if (!sugov_update_next_freq(sg_policy, time, next_freq))
		return;

	next_freq = cpufreq_driver_fast_switch(policy, next_freq);
	if (!next_freq)
		return;

	policy->cur = next_freq;
	sg_policy->next_freq = next_freq;
}

#define DEFAULT_CPU0_RTG_BOOST_FREQ 1344000
#define DEFAULT_CPU4_RTG_BOOST_FREQ 2246400
#define DEFAULT_CPU7_RTG_BOOST_FREQ 2265600

static unsigned long freq_to_util(struct sugov_policy *sg_policy,
				  unsigned int freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long max = sg_policy->max;

	if (!freq)
		return 0;

	if (!max)
		max = arch_scale_cpu_capacity(NULL, policy->cpu);

	freq = min(freq, policy->cpuinfo.max_freq);
	return mult_frac(max, freq, policy->cpuinfo.max_freq);
}

static inline unsigned long target_util(struct sugov_policy *sg_policy,
					unsigned int freq)
{
	return freq_to_util(sg_policy, freq);
}

static void sugov_update_rtg_boost_util(struct sugov_policy *sg_policy,
					unsigned long max)
{
	sg_policy->max = max;
	sg_policy->rtg_boost_util = target_util(sg_policy,
						sg_policy->tunables->rtg_boost_freq);
}

static inline bool sugov_rtg_boost_active(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);

	return sg_cpu->walt_load.rtgb_active &&
	       (rq->grp_time.curr_runnable_sum > 0 || rq->grp_time.prev_runnable_sum > 0);
}

static inline bool bandido_touch(u64 time)
{
#ifdef CONFIG_CPU_BOOST
	return time < READ_ONCE(cpuboost_input_until_ns);
#else
	return false;
#endif
}

/*
 * User interaction: a touch within the cpu-boost window, or a scheduler
 * boost requested by Android itself (perf HAL scroll/launch hints).
 */
static inline bool bandido_interaction(u64 time)
{
	return bandido_touch(time) || sched_boost() != NO_BOOST;
}

/*
 * Something is drawing: a UI/render thread ran on any CPU recently, or the
 * user is interacting. Animations and video with no touch count too.
 */
static inline bool bandido_drawing(u64 time)
{
#ifdef CONFIG_BANDIDO_UI_BOOST
	if (bandido_ui_drawing(time))
		return true;
#endif
	return bandido_interaction(time);
}

#ifdef CONFIG_BANDIDO_UI_BOOST
static inline bool sugov_ui_active(struct sugov_cpu *sg_cpu, u64 time)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	bool has_ui = (rq->nr_ui_running > 0) || is_ui_thread(rq->curr);

	if (has_ui) {
		sg_cpu->last_ui_time = time;
		bandido_ui_seen(time);
		return true;
	}

	/*
	 * Hold the boost across inter-frame VSYNC sleeps for up to 32ms
	 * (~2 frames at 60Hz, ~4 at 120Hz) so the next frame starts fast.
	 */
	if (sg_cpu->last_ui_time && (time - sg_cpu->last_ui_time < 32000000ULL))
		return true;

	return false;
}
#else
static inline bool sugov_ui_active(struct sugov_cpu *sg_cpu, u64 time)
{
	return false;
}
#endif

static void sugov_walt_adjust(struct sugov_cpu *sg_cpu, unsigned long *util,
			      unsigned long *max, u64 time)
{
#ifdef CONFIG_SCHED_WALT
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	bool boost_active = sugov_rtg_boost_active(sg_cpu) || sugov_ui_active(sg_cpu, time);

	if (boost_active) {
		if (sg_policy->policy->cpu == 0 || bandido_drawing(time))
			*util = max(*util, sg_policy->rtg_boost_util);
	}
#endif
}

static inline unsigned long sugov_apply_dvfs_headroom(unsigned long util,
						      unsigned long capacity,
						      unsigned long threshold)
{
	unsigned long delta, headroom;
	unsigned long capped_util = min(util, capacity);
	unsigned long delta_t = (capacity * 220) >> 10;

	delta = capacity - capped_util;

	headroom = min((delta_t * capped_util) / threshold,
			(delta_t * delta) / (capacity - threshold));

	return capped_util + headroom;
}

/* Above this load, headroom drops to 1/16 when nothing is drawing */
#define BANDIDO_TARGET_LOAD	60

static void sugov_build_dvfs_headroom_lut(struct sugov_policy *sg_policy)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long capacity = arch_scale_cpu_capacity(NULL, policy->cpu);
	unsigned long threshold;
	unsigned long util;

	if (sg_policy->dvfs_capacity == capacity)
		return;

	sg_policy->dvfs_capacity = capacity;
	sg_policy->target_load_thresh = (capacity * BANDIDO_TARGET_LOAD) / 100;
	threshold = (capacity * 15) / 100;

	for (util = 0; util <= SCHED_CAPACITY_SCALE; util++)
		sg_policy->dvfs_headroom_lut[util] =
			sugov_apply_dvfs_headroom(util, capacity, threshold);
}

static unsigned long bandido_map_util_freq(unsigned long util,
					unsigned long freq, unsigned long cap,
					struct sugov_cpu *sg_cpu, bool drawing)
{
	unsigned long thresh;

	if (drawing) {
		if (sg_cpu->dvfs_headroom_lut)
			util = sg_cpu->dvfs_headroom_lut[min_t(unsigned long, util, SCHED_CAPACITY_SCALE)];

		return (freq + (freq >> 2)) * util / cap;
	}

	/* waltgov target_load: taper headroom near fmax when nothing is drawing */
	thresh = sg_cpu->sg_policy->target_load_thresh;
	if (util >= thresh)
		return max((freq + (freq >> 4)) * util,
			   (freq + (freq >> 2)) * thresh) / cap;

	return (freq + (freq >> 2)) * util / cap;
}

static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max, u64 time)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;

	freq = bandido_map_util_freq(util, freq, max, &per_cpu(sugov_cpu, policy->cpu),
				     bandido_drawing(time));
#ifdef CONFIG_CPU_BOOST
	if (bandido_touch(time))
		freq = max(freq, cpuboost_input_freq(policy->cpu));
#endif

	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
		return sg_policy->next_freq;

	sg_policy->need_freq_update = false;
	sg_policy->prev_cached_raw_freq = sg_policy->cached_raw_freq;
	sg_policy->cached_raw_freq = freq;

	return cpufreq_driver_resolve_freq(policy, freq);
}

#ifdef CONFIG_SCHED_WALT
static unsigned long sugov_get_util(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	unsigned long max = arch_scale_cpu_capacity(NULL, sg_cpu->cpu);

	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);

	return stune_util(sg_cpu->cpu, 0, &sg_cpu->walt_load);
}
#else
static unsigned long sugov_get_util(struct sugov_cpu *sg_cpu)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);

	unsigned long util_cfs = cpu_util_cfs(rq);
	unsigned long max = arch_scale_cpu_capacity(NULL, sg_cpu->cpu);

	sg_cpu->max = max;
	sg_cpu->bw_dl = cpu_bw_dl(rq);

	return schedutil_cpu_util(sg_cpu->cpu, util_cfs, max,
				  FREQUENCY_UTIL, NULL);
}
#endif



static bool sugov_iowait_reset(struct sugov_cpu *sg_cpu, u64 time,
			       bool set_iowait_boost)
{
	s64 delta_ns = time - sg_cpu->last_update;

	if (delta_ns <= TICK_NSEC)
		return false;

	sg_cpu->iowait_boost = set_iowait_boost ? sg_cpu->min : 0;
	sg_cpu->iowait_boost_pending = set_iowait_boost;

	return true;
}



static void sugov_iowait_boost(struct sugov_cpu *sg_cpu, u64 time,
			       unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	if (sg_cpu->iowait_boost &&
	    sugov_iowait_reset(sg_cpu, time, set_iowait_boost))
		return;

	if (!set_iowait_boost)
		return;

	if (sg_cpu->iowait_boost_pending)
		return;
	sg_cpu->iowait_boost_pending = true;

	if (sg_cpu->iowait_boost) {
		sg_cpu->iowait_boost =
			min_t(unsigned int, sg_cpu->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	sg_cpu->iowait_boost = SCHED_CAPACITY_SCALE >> 1;
}

static unsigned long sugov_iowait_apply(struct sugov_cpu *sg_cpu, u64 time,
					unsigned long util, unsigned long max)
{
	unsigned long boost;

	if (!sg_cpu->iowait_boost)
		return util;

	if (sugov_iowait_reset(sg_cpu, time, false))
		return util;

	if (!sg_cpu->iowait_boost_pending) {
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < sg_cpu->min) {
			sg_cpu->iowait_boost = 0;
			return util;
		}
	}

	sg_cpu->iowait_boost_pending = false;

	boost = (sg_cpu->iowait_boost * max) >> SCHED_CAPACITY_SHIFT;
	return max(boost, util);
}

static inline void ignore_dl_rate_limit(struct sugov_cpu *sg_cpu, struct sugov_policy *sg_policy)
{
	if (unlikely((cpu_bw_dl(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_dl)))
		sg_policy->limits_changed = true;
}

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	ignore_dl_rate_limit(sg_cpu, sg_policy);

	sg_cpu->util = util = sugov_get_util(sg_cpu);
	max = sg_cpu->max;

	sugov_update_rtg_boost_util(sg_policy, max);
	util = sugov_iowait_apply(sg_cpu, time, util, max);
	sugov_walt_adjust(sg_cpu, &util, &max, time);
	next_f = get_next_freq(sg_policy, util, max, time);

	/*
	 * Fast-Ramp: Bypass the rate limit if the next frequency is
	 * significantly higher (>20% jump) than the current one,
	 * OR if UI / RTG Boost is active (prioritizing GUI responsiveness).
	 */
	if (!sugov_should_update_freq(sg_policy, time) &&
	    next_f < (sg_policy->next_freq + (sg_policy->next_freq / 5)) &&
	    !sugov_rtg_boost_active(sg_cpu) && !sugov_ui_active(sg_cpu, time))
		return;

	sugov_fast_switch(sg_policy, time, next_f);
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long j_util, j_max;

		j_sg_cpu->util = sugov_get_util(j_sg_cpu);
		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (!j_max)
			continue;

		j_util = sugov_iowait_apply(j_sg_cpu, time, j_util, j_max);
		sugov_walt_adjust(j_sg_cpu, &j_util, &j_max, time);

		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}
	}

	return get_next_freq(sg_policy, util, max, time);
}

static void
sugov_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;

	sg_cpu->util = sugov_get_util(sg_cpu);
	sg_cpu->flags = flags;
	raw_spin_lock(&sg_policy->update_lock);

	sugov_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;
	sugov_update_rtg_boost_util(sg_policy, sg_cpu->max);

	ignore_dl_rate_limit(sg_cpu, sg_policy);

	next_f = sugov_next_freq_shared(sg_cpu, time);

	/*
	 * Busy protection shared: avoid ramp-down if < 1ms holds.
	 * This prevents premature frequency drops during bursty but busy periods.
	 */
	if (next_f < sg_policy->next_freq && 
	    (time - sg_policy->last_freq_update_time) < 1000000) {
		next_f = sg_policy->next_freq;
		sg_policy->cached_raw_freq = 0;
	}

	/*
	 * Fast-Ramp Shared: Bypass the rate limit if the next frequency is
	 * significantly higher (>20% jump) than the current one,
	 * OR if UI / RTG Boost is active (prioritizing GUI responsiveness).
	 */
	if (!sugov_should_update_freq(sg_policy, time) &&
	    next_f < (sg_policy->next_freq + (sg_policy->next_freq / 5)) &&
	    !sugov_rtg_boost_active(sg_cpu) && !sugov_ui_active(sg_cpu, time)) {
		raw_spin_unlock(&sg_policy->update_lock);
		return;
	}

	sugov_fast_switch(sg_policy, time, next_f);

	raw_spin_unlock(&sg_policy->update_lock);
}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

#define BANDIDO_DOWN_RATE_LIMIT_US	12000

static void update_min_rate_limit_ns(struct sugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = 1000000;
	sg_policy->down_rate_delay_ns = BANDIDO_DOWN_RATE_LIMIT_US * NSEC_PER_USEC;
	mutex_unlock(&min_rate_lock);
}

static ssize_t rtg_boost_freq_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return scnprintf(buf, PAGE_SIZE, "%u\n", tunables->rtg_boost_freq);
}

static ssize_t rtg_boost_freq_store(struct gov_attr_set *attr_set,
				    const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned long flags;
	unsigned int val;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	tunables->rtg_boost_freq = val;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
		sugov_update_rtg_boost_util(sg_policy, sg_policy->max);
		raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);
	}

	return count;
}

static ssize_t boost_status_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_policy *sg_policy;
	ssize_t count = 0;
	u64 now = ktime_get_ns();

	count += scnprintf(buf + count, PAGE_SIZE - count,
			   "touch=%d interaction=%d drawing=%d sched_boost=%d\n",
			   bandido_touch(now), bandido_interaction(now),
			   bandido_drawing(now), sched_boost());

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		struct cpufreq_policy *policy = sg_policy->policy;
		unsigned int cpu;

		count += scnprintf(buf + count, PAGE_SIZE - count,
				   "Policy%u (cur: %u kHz, rtg_boost_util: %lu):\n",
				   policy->cpu, policy->cur, sg_policy->rtg_boost_util);

		for_each_cpu(cpu, policy->cpus) {
			struct rq *rq = cpu_rq(cpu);
			struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
			bool rtg_act = sugov_rtg_boost_active(sg_cpu);
			bool ui_act = sugov_ui_active(sg_cpu, now);
			int st_boost = schedtune_cpu_boost_with(cpu, NULL);

			count += scnprintf(buf + count, PAGE_SIZE - count,
					   "  CPU%u: nr_ui=%u curr='%s' (ui=%d) rtg_act=%d ui_act=%d st_boost=%d\n",
					   cpu,
#ifdef CONFIG_BANDIDO_UI_BOOST
					   rq->nr_ui_running,
#else
					   0,
#endif
					   rq->curr ? rq->curr->comm : "none",
					   rq->curr ? is_ui_thread(rq->curr) : 0,
					   rtg_act, ui_act, st_boost);
		}
	}

	return count;
}

static struct governor_attr rtg_boost_freq = __ATTR_RW(rtg_boost_freq);
static struct governor_attr boost_status = __ATTR_RO(boost_status);

static struct attribute *sugov_attributes[] = {
	&rtg_boost_freq.attr,
	&boost_status.attr,
	NULL
};

static void sugov_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);

	kfree(to_sugov_tunables(attr_set));
}

static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sugov_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	kfree(sg_policy);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_save(struct cpufreq_policy *policy,
		struct sugov_tunables *tunables)
{
	int cpu;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!have_governor_per_policy())
		return;

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	cached->rtg_boost_freq = tunables->rtg_boost_freq;
}

static void sugov_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static void sugov_tunables_restore(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	struct sugov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (cached)
		tunables->rtg_boost_freq = cached->rtg_boost_freq;
}

static struct cpufreq_governor bandido_gov;

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;

	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_sg_policy;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_sg_policy;
	}

	switch (policy->cpu) {
	default:
	case 0:
		tunables->rtg_boost_freq = DEFAULT_CPU0_RTG_BOOST_FREQ;
		break;
	case 4:
		tunables->rtg_boost_freq = DEFAULT_CPU4_RTG_BOOST_FREQ;
		break;
	case 7:
		tunables->rtg_boost_freq = DEFAULT_CPU7_RTG_BOOST_FREQ;
		break;
	}

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	sugov_tunables_restore(policy);
	sugov_update_rtg_boost_util(sg_policy,
				    arch_scale_cpu_capacity(NULL, policy->cpu));

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   bandido_gov.name);
	if (ret)
		goto fail;

	stale_ns = sched_ravg_window + (sched_ravg_window >> 3);

	out:
	mutex_unlock(&global_tunables_lock);
	return 0;

	fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sugov_clear_global_tunables();

	free_sg_policy:
	mutex_unlock(&global_tunables_lock);
	sugov_policy_free(sg_policy);

	disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		sugov_tunables_save(policy, tunables);
		sugov_clear_global_tunables();
	}

	mutex_unlock(&global_tunables_lock);

	sugov_policy_free(sg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sugov_build_dvfs_headroom_lut(sg_policy);
	update_min_rate_limit_ns(sg_policy);
	sg_policy->last_freq_update_time	= 0;
	sg_policy->next_freq			= 0;
	sg_policy->limits_changed		= false;
	sg_policy->need_freq_update		= false;
	sg_policy->cached_raw_freq		= 0;
	sg_policy->prev_cached_raw_freq		= 0;
	sugov_update_rtg_boost_util(sg_policy,
				    arch_scale_cpu_capacity(NULL, policy->cpu));

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu			= cpu;
		sg_cpu->sg_policy		= sg_policy;
		sg_cpu->dvfs_headroom_lut	= sg_policy->dvfs_headroom_lut;
		sg_cpu->min			=
			(SCHED_CAPACITY_SCALE * policy->cpuinfo.min_freq) /
			policy->cpuinfo.max_freq;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							sugov_update_shared :
							sugov_update_single);
	}
	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_sched();
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq;

	sugov_build_dvfs_headroom_lut(sg_policy);

	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = policy->cur;
	now = ktime_get_ns();

	freq = cpufreq_driver_resolve_freq(policy, freq);
	sg_policy->cached_raw_freq = freq;

	sugov_fast_switch(sg_policy, now, freq);

	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	sg_policy->limits_changed = true;
}

static struct cpufreq_governor bandido_gov = {
	.name			= "Bandido",
	.owner			= THIS_MODULE,
	.dynamic_switching	= true,
	.init			= sugov_init,
	.exit			= sugov_exit,
	.start			= sugov_start,
	.stop			= sugov_stop,
	.limits			= sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_BANDIDO
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &bandido_gov;
}
#endif

static int __init sugov_register(void)
{
	return cpufreq_register_governor(&bandido_gov);
}
fs_initcall(sugov_register);
