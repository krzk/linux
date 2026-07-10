/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Sub-scheduler hierarchy support.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
#ifndef _KERNEL_SCHED_EXT_SUB_H
#define _KERNEL_SCHED_EXT_SUB_H

#include "internal.h"
#include "cid.h"

#ifdef CONFIG_EXT_SUB_SCHED

struct scx_sched *scx_next_descendant_pre(struct scx_sched *pos, struct scx_sched *root);
void scx_set_task_sched(struct task_struct *p, struct scx_sched *sch);
struct cgroup *sch_cgroup(struct scx_sched *sch);
void set_cgroup_sched(struct cgroup *cgrp, struct scx_sched *sch);
void scx_pstack_recursion_on_dispatch(struct bpf_prog *prog);
void drain_descendants(struct scx_sched *sch);
void scx_sub_disable(struct scx_sched *sch);
void scx_sub_enable_workfn(struct kthread_work *work);
bool scx_bpf_sub_dispatch(u64 cgroup_id, const struct bpf_prog_aux *aux);

#else	/* CONFIG_EXT_SUB_SCHED */

static inline struct scx_sched *scx_next_descendant_pre(struct scx_sched *pos, struct scx_sched *root) { return pos ? NULL : root; }
static inline void scx_set_task_sched(struct task_struct *p, struct scx_sched *sch) {}
static inline struct cgroup *sch_cgroup(struct scx_sched *sch) { return NULL; }
static inline void set_cgroup_sched(struct cgroup *cgrp, struct scx_sched *sch) {}
static inline void drain_descendants(struct scx_sched *sch) { }
static inline void scx_sub_disable(struct scx_sched *sch) { }

#endif	/* CONFIG_EXT_SUB_SCHED */

/**
 * scx_for_each_descendant_pre - pre-order walk of a sched's descendants
 * @pos: iteration cursor
 * @root: sched to walk the descendants of
 *
 * Walk @root's descendants. @root is included in the iteration and the first
 * node to be visited. Must be called with either scx_enable_mutex or
 * scx_sched_lock held.
 */
#define scx_for_each_descendant_pre(pos, root)					\
	for ((pos) = scx_next_descendant_pre(NULL, (root)); (pos);		\
	     (pos) = scx_next_descendant_pre((pos), (root)))

/*
 * One user of this function is scx_bpf_dispatch() which can be called
 * recursively as sub-sched dispatches nest. Always inline to reduce stack usage
 * from the call frame.
 */
static __always_inline bool
scx_dispatch_sched(struct scx_sched *sch, struct rq *rq,
		   struct task_struct *prev, bool nested)
{
	struct scx_dsp_ctx *dspc = &this_cpu_ptr(sch->pcpu)->dsp_ctx;
	int nr_loops = SCX_DSP_MAX_LOOPS;
	s32 cpu = cpu_of(rq);
	bool prev_on_sch = (prev->sched_class == &ext_sched_class) &&
		scx_task_on_sched(sch, prev);

	if (scx_consume_global_dsq(sch, rq))
		return true;

	if (scx_bypass_dsp_enabled(sch)) {
		/* if @sch is bypassing, only the bypass DSQs are active */
		if (scx_bypassing(sch, cpu))
			return scx_consume_dispatch_q(sch, rq, scx_bypass_dsq(sch, cpu), 0);

#ifdef CONFIG_EXT_SUB_SCHED
		/*
		 * If @sch isn't bypassing but its children are, @sch is
		 * responsible for making forward progress for both its own
		 * tasks that aren't bypassing and the bypassing descendants'
		 * tasks. The following implements a simple built-in behavior -
		 * let each CPU try to run the bypass DSQ every Nth time.
		 *
		 * Later, if necessary, we can add an ops flag to suppress the
		 * auto-consumption and a kfunc to consume the bypass DSQ and,
		 * so that the BPF scheduler can fully control scheduling of
		 * bypassed tasks.
		 */
		struct scx_sched_pcpu *pcpu = per_cpu_ptr(sch->pcpu, cpu);

		if (!(pcpu->bypass_host_seq++ % SCX_BYPASS_HOST_NTH) &&
		    scx_consume_dispatch_q(sch, rq, scx_bypass_dsq(sch, cpu), 0)) {
			__scx_add_event(sch, SCX_EV_SUB_BYPASS_DISPATCH, 1);
			return true;
		}
#endif	/* CONFIG_EXT_SUB_SCHED */
	}

	if (unlikely(!SCX_HAS_OP(sch, dispatch)) || !scx_rq_online(rq))
		return false;

	dspc->rq = rq;

	/*
	 * The dispatch loop. Because scx_flush_dispatch_buf() may drop the rq
	 * lock, the local DSQ might still end up empty after a successful
	 * ops.dispatch(). If the local DSQ is empty even after ops.dispatch()
	 * produced some tasks, retry. The BPF scheduler may depend on this
	 * looping behavior to simplify its implementation.
	 */
	do {
		dspc->nr_tasks = 0;

		if (nested) {
			SCX_CALL_OP(sch, dispatch, rq, scx_cpu_arg(cpu),
				    prev_on_sch ? prev : NULL);
		} else {
			/* stash @prev so that nested invocations can access it */
			rq->scx.sub_dispatch_prev = prev;
			SCX_CALL_OP(sch, dispatch, rq, scx_cpu_arg(cpu),
				    prev_on_sch ? prev : NULL);
			rq->scx.sub_dispatch_prev = NULL;
		}

		scx_flush_dispatch_buf(sch, rq);

		if ((prev->scx.flags & SCX_TASK_QUEUED) && prev->scx.slice) {
			rq->scx.flags |= SCX_RQ_BAL_KEEP;
			return true;
		}
		if (rq->scx.local_dsq.nr)
			return true;
		if (scx_consume_global_dsq(sch, rq))
			return true;

		/*
		 * ops.dispatch() can trap us in this loop by repeatedly
		 * dispatching ineligible tasks. Break out once in a while to
		 * allow the watchdog to run. As IRQ can't be enabled in
		 * balance(), we want to complete this scheduling cycle and then
		 * start a new one. IOW, we want to call resched_curr() on the
		 * next, most likely idle, task, not the current one. Use
		 * __scx_bpf_kick_cpu() for deferred kicking.
		 */
		if (unlikely(!--nr_loops)) {
			scx_kick_cpu(sch, cpu, 0);
			break;
		}
	} while (dspc->nr_tasks);

	/*
	 * Prevent the CPU from going idle while bypassed descendants have tasks
	 * queued. Without this fallback, bypassed tasks could stall if the host
	 * scheduler's ops.dispatch() doesn't yield any tasks.
	 */
	if (scx_bypass_dsp_enabled(sch))
		return scx_consume_dispatch_q(sch, rq, scx_bypass_dsq(sch, cpu), 0);

	return false;
}

#endif /* _KERNEL_SCHED_EXT_SUB_H */
