
#ifdef CONFIG_SCHED_TUNE

#include <linux/reciprocal_div.h>

/*
 * System energy normalization constants
 */
struct target_nrg {
	unsigned long min_power;
	unsigned long max_power;
	struct reciprocal_value rdiv;
};

int schedtune_cpu_boost_with(int cpu, struct task_struct *p);
int schedtune_task_boost(struct task_struct *tsk);

int schedtune_prefer_idle(struct task_struct *tsk);

/*
 * Identifies threads that directly participate in active UI frame rendering.
 * Strictly focuses on dynamic render/animation threads (which sleep when idle),
 * excluding background daemons (system_server, HwBinder, SysUiBg) to allow
 * full CPU powerdown at idle.
 *
 * Note: task_struct->comm is at most TASK_COMM_LEN-1 (15) chars.
 */
static inline bool is_ui_thread_name(struct task_struct *p)
{
	const char *comm = p->comm;
	char c = comm[0];

	if (c == 'R' && (!strcmp(comm, "RenderThread") || !strcmp(comm, "RenderEngine")))
		return true;
	if (c == 'w' && !strcmp(comm, "wmshell.anim"))
		return true;
	if (c == 'a' && !strcmp(comm, "android.anim"))
		return true;
	if (c == 'I' && (!strcmp(comm, "InsetsAnimation") || !strcmp(comm, "InteractionJank")))
		return true;
	/* SurfaceFlinger composes every frame, including video with no app redraw */
	if (c == 's' && !strcmp(comm, "surfaceflinger"))
		return true;
	/* SDE display commit / frame_done kthreads (crtc_commit:N, crtc_event:N) */
	if (c == 'c' && !strncmp(comm, "crtc_", 5))
		return true;

	return false;
}

bool is_top_app_main_thread(struct task_struct *p);

static inline bool is_ui_thread(struct task_struct *p)
{
	return p->is_ui || is_top_app_main_thread(p);
}


void schedtune_enqueue_task(struct task_struct *p, int cpu);
void schedtune_dequeue_task(struct task_struct *p, int cpu);

#else /* CONFIG_SCHED_TUNE */

#define schedtune_cpu_boost_with(cpu, p)  0
#define schedtune_task_boost(tsk) 0

#define schedtune_prefer_idle(tsk) 0

#define schedtune_enqueue_task(task, cpu) do { } while (0)
#define schedtune_dequeue_task(task, cpu) do { } while (0)

#define stune_util(cpu, other_util, walt_load) cpu_util_cfs(cpu_rq(cpu))

static inline bool is_ui_thread(struct task_struct *p)
{
	return p->is_ui;
}
#endif /* CONFIG_SCHED_TUNE */
