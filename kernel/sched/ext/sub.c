// SPDX-License-Identifier: GPL-2.0
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Sub-scheduler hierarchy support.
 *
 * A sub-scheduler is an scx_sched attached to a cgroup subtree under another
 * scx_sched. This file holds the sub-scheduler implementation: the scheduler
 * tree walk, capability delegation, per-shard cap state and its sync, and the
 * sub-scheduler enable/disable paths. The core dispatch/enqueue machinery it
 * builds on lives in ext.c.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
#include <linux/rhashtable.h>
#include "internal.h"
#include "cid.h"
#include "arena.h"
#include "sub.h"

#ifdef CONFIG_EXT_SUB_SCHED

/**
 * scx_next_descendant_pre - find the next descendant for pre-order walk
 * @pos: the current position (%NULL to initiate traversal)
 * @root: sched whose descendants to walk
 *
 * To be used by scx_for_each_descendant_pre(). Find the next descendant to
 * visit for pre-order traversal of @root's descendants. @root is included in
 * the iteration and the first node to be visited.
 */
struct scx_sched *scx_next_descendant_pre(struct scx_sched *pos, struct scx_sched *root)
{
	struct scx_sched *next;

	lockdep_assert(lockdep_is_held(&scx_enable_mutex) ||
		       lockdep_is_held(&scx_sched_lock));

	/* if first iteration, visit @root */
	if (!pos)
		return root;

	/* visit the first child if exists */
	next = list_first_entry_or_null(&pos->children, struct scx_sched, sibling);
	if (next)
		return next;

	/* no child, visit my or the closest ancestor's next sibling */
	while (pos != root) {
		if (!list_is_last(&pos->sibling, &scx_parent(pos)->children))
			return list_next_entry(pos, sibling);
		pos = scx_parent(pos);
	}

	return NULL;
}

static struct scx_sched *scx_find_sub_sched(u64 cgroup_id)
{
	return rhashtable_lookup(&scx_sched_hash, &cgroup_id,
				 scx_sched_hash_params);
}

void scx_set_task_sched(struct task_struct *p, struct scx_sched *sch)
{
	rcu_assign_pointer(p->scx.sched, sch);
}

struct cgroup *sch_cgroup(struct scx_sched *sch)
{
	return sch->cgrp;
}

/* for each descendant of @cgrp including self, set ->scx_sched to @sch */
void set_cgroup_sched(struct cgroup *cgrp, struct scx_sched *sch)
{
	struct cgroup *pos;
	struct cgroup_subsys_state *css;

	cgroup_for_each_live_descendant_pre(pos, css, cgrp)
		rcu_assign_pointer(pos->scx_sched, sch);
}

static DECLARE_WAIT_QUEUE_HEAD(scx_unlink_waitq);

void drain_descendants(struct scx_sched *sch)
{
	/*
	 * Child scheds that finished the critical part of disabling will take
	 * themselves off @sch->children. Wait for it to drain. As propagation
	 * is recursive, empty @sch->children means that all proper descendant
	 * scheds reached unlinking stage.
	 */
	wait_event(scx_unlink_waitq, list_empty(&sch->children));
}

static void scx_fail_parent(struct scx_sched *sch,
			    struct task_struct *failed, s32 fail_code)
{
	struct scx_sched *parent = scx_parent(sch);
	struct scx_task_iter sti;
	struct task_struct *p;

	scx_error(parent, "ops.init_task() failed (%d) for %s[%d] while disabling a sub-scheduler",
		  fail_code, failed->comm, failed->pid);

	/*
	 * Once $parent is bypassed, it's safe to put SCX_TASK_NONE tasks into
	 * it. This may cause downstream failures on the BPF side but $parent is
	 * dying anyway.
	 */
	scx_bypass(parent, true);

	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		if (scx_task_on_sched(parent, p))
			continue;

		scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
			scx_disable_and_exit_task(sch, p);
			scx_set_task_sched(p, parent);
		}
	}
	scx_task_iter_stop(&sti);
}

void scx_sub_disable(struct scx_sched *sch)
{
	struct scx_sched *parent = scx_parent(sch);
	struct scx_task_iter sti;
	struct task_struct *p;
	int ret;

	/*
	 * Guarantee forward progress and wait for descendants to be disabled.
	 * To limit disruptions, $parent is not bypassed. Tasks are fully
	 * prepped and then inserted back into $parent.
	 */
	scx_bypass(sch, true);
	drain_descendants(sch);

	/*
	 * Here, every runnable task is guaranteed to make forward progress and
	 * we can safely use blocking synchronization constructs. Actually
	 * disable ops.
	 */
	mutex_lock(&scx_enable_mutex);
	percpu_down_write(&scx_fork_rwsem);
	scx_cgroup_lock();

	set_cgroup_sched(sch_cgroup(sch), parent);

	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		struct rq *rq;
		struct rq_flags rf;

		/* filter out duplicate visits */
		if (scx_task_on_sched(parent, p))
			continue;

		/*
		 * By the time control reaches here, all descendant schedulers
		 * should already have been disabled.
		 */
		WARN_ON_ONCE(!scx_task_on_sched(sch, p));

		/*
		 * @p is pinned by the iter: css_task_iter_next() takes a
		 * reference and holds it until the next iter_next() call, so
		 * @p->usage is guaranteed > 0.
		 */
		get_task_struct(p);

		scx_task_iter_unlock(&sti);

		/*
		 * $p is READY or ENABLED on @sch. Initialize for $parent,
		 * disable and exit from @sch, and then switch over to $parent.
		 *
		 * If a task fails to initialize for $parent, the only available
		 * action is disabling $parent too. While this allows disabling
		 * of a child sched to cause the parent scheduler to fail, the
		 * failure can only originate from ops.init_task() of the
		 * parent. A child can't directly affect the parent through its
		 * own failures.
		 */
		ret = __scx_init_task(parent, p, false);
		if (ret) {
			scx_fail_parent(sch, p, ret);
			put_task_struct(p);
			break;
		}

		rq = task_rq_lock(p, &rf);

		if (scx_get_task_state(p) == SCX_TASK_DEAD) {
			/*
			 * sched_ext_dead() raced us between __scx_init_task()
			 * and this rq lock and ran exit_task() on @sch (the
			 * sched @p was on at that point), not on $parent.
			 * $parent's just-completed init is owed an exit_task()
			 * and we issue it here.
			 */
			scx_sub_init_cancel_task(parent, p);
			task_rq_unlock(rq, p, &rf);
			put_task_struct(p);
			continue;
		}

		scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
			/*
			 * $p is initialized for $parent and still attached to
			 * @sch. Disable and exit for @sch, switch over to
			 * $parent, override the state to READY to account for
			 * $p having already been initialized, and then enable.
			 */
			scx_disable_and_exit_task(sch, p);
			scx_set_task_state(p, SCX_TASK_INIT_BEGIN);
			scx_set_task_state(p, SCX_TASK_INIT);
			scx_set_task_sched(p, parent);
			scx_set_task_state(p, SCX_TASK_READY);
			scx_enable_task(parent, p);
		}

		task_rq_unlock(rq, p, &rf);
		put_task_struct(p);
	}
	scx_task_iter_stop(&sti);

	scx_disable_dump(sch);

	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);

	/*
	 * All tasks are moved off of @sch but there may still be on-going
	 * operations (e.g. ops.select_cpu()). Drain them by flushing RCU. Use
	 * the expedited version as ancestors may be waiting in bypass mode.
	 * Also, tell the parent that there is no need to keep running bypass
	 * DSQs for us.
	 */
	synchronize_rcu_expedited();
	scx_disable_bypass_dsp(sch);

	scx_unlink_sched(sch);

	mutex_unlock(&scx_enable_mutex);

	/*
	 * @sch is now unlinked from the parent's children list. Notify and call
	 * ops.sub_detach/exit(). Note that ops.sub_detach/exit() must be called
	 * after unlinking and releasing all locks. See scx_claim_exit().
	 */
	wake_up_all(&scx_unlink_waitq);

	if (parent->ops.sub_detach && sch->sub_attached) {
		struct scx_sub_detach_args sub_detach_args = {
			.ops = &sch->ops,
			.cgroup_path = sch->cgrp_path,
		};
		SCX_CALL_OP(parent, sub_detach, NULL,
			    &sub_detach_args);
	}

	scx_log_sched_disable(sch);

	if (sch->ops.exit)
		SCX_CALL_OP(sch, exit, NULL, sch->exit_info);
	if (sch->sub_kset)
		kobject_del(&sch->sub_kset->kobj);
	kobject_del(&sch->kobj);
}

/* verify that a scheduler can be attached to @cgrp and return the parent */
static struct scx_sched *find_parent_sched(struct cgroup *cgrp)
{
	struct scx_sched *parent = cgrp->scx_sched;
	struct scx_sched *pos;

	lockdep_assert_held(&scx_sched_lock);

	/* can't attach twice to the same cgroup */
	if (parent->cgrp == cgrp)
		return ERR_PTR(-EBUSY);

	/* does $parent allow sub-scheds? */
	if (!parent->ops.sub_attach)
		return ERR_PTR(-EOPNOTSUPP);

	/* can't insert between $parent and its exiting children */
	list_for_each_entry(pos, &parent->children, sibling)
		if (cgroup_is_descendant(pos->cgrp, cgrp))
			return ERR_PTR(-EBUSY);

	return parent;
}

static bool assert_task_ready_or_enabled(struct task_struct *p)
{
	u32 state = scx_get_task_state(p);

	switch (state) {
	case SCX_TASK_READY:
	case SCX_TASK_ENABLED:
		return true;
	default:
		WARN_ONCE(true, "sched_ext: Invalid task state %d for %s[%d] during enabling sub sched",
			  state, p->comm, p->pid);
		return false;
	}
}

void scx_sub_enable_workfn(struct kthread_work *work)
{
	struct scx_enable_cmd *cmd = container_of(work, struct scx_enable_cmd, work);
	struct sched_ext_ops *ops = cmd->ops;
	struct cgroup *cgrp;
	struct scx_sched *parent, *sch;
	struct scx_task_iter sti;
	struct task_struct *p;
	s32 i, ret;

	mutex_lock(&scx_enable_mutex);

	if (!scx_enabled()) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* See scx_root_enable_workfn() for the @ops->priv check. */
	if (rcu_access_pointer(ops->priv)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	cgrp = cgroup_get_from_id(ops->sub_cgroup_id);
	if (IS_ERR(cgrp)) {
		ret = PTR_ERR(cgrp);
		goto out_unlock;
	}

	raw_spin_lock_irq(&scx_sched_lock);
	parent = find_parent_sched(cgrp);
	if (IS_ERR(parent)) {
		raw_spin_unlock_irq(&scx_sched_lock);
		ret = PTR_ERR(parent);
		goto out_put_cgrp;
	}
	kobject_get(&parent->kobj);
	raw_spin_unlock_irq(&scx_sched_lock);

	/* scx_alloc_and_add_sched() consumes @cgrp whether it succeeds or not */
	sch = scx_alloc_and_add_sched(cmd, cgrp, parent);
	kobject_put(&parent->kobj);
	if (IS_ERR(sch)) {
		ret = PTR_ERR(sch);
		goto out_unlock;
	}

	ret = scx_link_sched(sch);
	if (ret)
		goto err_disable;

	if (sch->level >= SCX_SUB_MAX_DEPTH) {
		scx_error(sch, "max nesting depth %d violated",
			  SCX_SUB_MAX_DEPTH);
		goto err_disable;
	}

	if (sch->ops.init) {
		ret = SCX_CALL_OP_RET(sch, init, NULL);
		if (ret) {
			ret = scx_ops_sanitize_err(sch, "init", ret);
			scx_error(sch, "ops.init() failed (%d)", ret);
			goto err_disable;
		}
		sch->exit_info->flags |= SCX_EFLAG_INITIALIZED;
	}

	ret = scx_arena_pool_init(sch);
	if (ret)
		goto err_disable;

	ret = scx_set_cmask_scratch_alloc(sch);
	if (ret)
		goto err_disable;

	if (scx_validate_ops(sch, ops))
		goto err_disable;

	struct scx_sub_attach_args sub_attach_args = {
		.ops = &sch->ops,
		.cgroup_path = sch->cgrp_path,
	};

	ret = SCX_CALL_OP_RET(parent, sub_attach, NULL,
			      &sub_attach_args);
	if (ret) {
		ret = scx_ops_sanitize_err(sch, "sub_attach", ret);
		scx_error(sch, "parent rejected (%d)", ret);
		goto err_disable;
	}
	sch->sub_attached = true;

	scx_bypass(sch, true);

	for (i = SCX_OPI_BEGIN; i < SCX_OPI_END; i++)
		if (((void (**)(void))ops)[i])
			set_bit(i, sch->has_op);

	percpu_down_write(&scx_fork_rwsem);
	scx_cgroup_lock();

	/*
	 * Set cgroup->scx_sched's and check CSS_ONLINE. Either we see
	 * !CSS_ONLINE or scx_cgroup_lifetime_notify() sees and shoots us down.
	 */
	set_cgroup_sched(sch_cgroup(sch), sch);
	if (!(cgrp->self.flags & CSS_ONLINE)) {
		scx_error(sch, "cgroup is not online");
		goto err_unlock_and_disable;
	}

	/*
	 * Initialize tasks for the new child $sch without exiting them for
	 * $parent so that the tasks can always be reverted back to $parent
	 * sched on child init failure.
	 */
	WARN_ON_ONCE(scx_enabling_sub_sched);
	scx_enabling_sub_sched = sch;

	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		struct rq *rq;
		struct rq_flags rf;

		/*
		 * Task iteration may visit the same task twice when racing
		 * against exiting. Use %SCX_TASK_SUB_INIT to mark tasks which
		 * finished __scx_init_task() and skip if set.
		 *
		 * A task may exit and get freed between __scx_init_task()
		 * completion and scx_enable_task(). In such cases,
		 * scx_disable_and_exit_task() must exit the task for both the
		 * parent and child scheds.
		 */
		if (p->scx.flags & SCX_TASK_SUB_INIT)
			continue;

		/* @p is pinned by the iter; see scx_sub_disable() */
		get_task_struct(p);

		if (!assert_task_ready_or_enabled(p)) {
			ret = -EINVAL;
			goto abort;
		}

		scx_task_iter_unlock(&sti);

		/*
		 * As $p is still on $parent, it can't be transitioned to INIT.
		 * Let's worry about task state later. Use __scx_init_task().
		 */
		ret = __scx_init_task(sch, p, false);
		if (ret)
			goto abort;

		rq = task_rq_lock(p, &rf);

		if (scx_get_task_state(p) == SCX_TASK_DEAD) {
			/*
			 * sched_ext_dead() raced us between __scx_init_task()
			 * and this rq lock and ran exit_task() on $parent (the
			 * sched @p was on at that point), not on @sch. @sch's
			 * just-completed init is owed an exit_task() and we
			 * issue it here.
			 */
			scx_sub_init_cancel_task(sch, p);
			task_rq_unlock(rq, p, &rf);
			put_task_struct(p);
			continue;
		}

		p->scx.flags |= SCX_TASK_SUB_INIT;
		task_rq_unlock(rq, p, &rf);

		put_task_struct(p);
	}
	scx_task_iter_stop(&sti);

	/*
	 * All tasks are prepped. Disable/exit tasks for $parent and enable for
	 * the new @sch.
	 */
	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		/*
		 * Use clearing of %SCX_TASK_SUB_INIT to detect and skip
		 * duplicate iterations.
		 */
		if (!(p->scx.flags & SCX_TASK_SUB_INIT))
			continue;

		scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
			/*
			 * $p must be either READY or ENABLED. If ENABLED,
			 * __scx_disabled_and_exit_task() first disables and
			 * makes it READY. However, after exiting $p, it will
			 * leave $p as READY.
			 */
			assert_task_ready_or_enabled(p);
			__scx_disable_and_exit_task(parent, p);

			/*
			 * $p is now only initialized for @sch and READY, which
			 * is what we want. Assign it to @sch and enable.
			 */
			scx_set_task_sched(p, sch);
			scx_enable_task(sch, p);

			p->scx.flags &= ~SCX_TASK_SUB_INIT;
		}
	}
	scx_task_iter_stop(&sti);

	scx_enabling_sub_sched = NULL;

	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);

	scx_bypass(sch, false);

	pr_info("sched_ext: BPF sub-scheduler \"%s\" enabled\n", sch->ops.name);
	kobject_uevent(&sch->kobj, KOBJ_ADD);
	ret = 0;
	goto out_unlock;

out_put_cgrp:
	cgroup_put(cgrp);
out_unlock:
	mutex_unlock(&scx_enable_mutex);
	cmd->ret = ret;
	return;

abort:
	put_task_struct(p);
	scx_task_iter_stop(&sti);

	/*
	 * Undo __scx_init_task() for tasks we marked. scx_enable_task() never
	 * ran for @sch on them, so calling scx_disable_task() here would invoke
	 * ops.disable() without a matching ops.enable(). scx_enabling_sub_sched
	 * must stay set until SUB_INIT is cleared from every marked task -
	 * scx_disable_and_exit_task() reads it when a task exits concurrently.
	 */
	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		if (p->scx.flags & SCX_TASK_SUB_INIT) {
			scx_sub_init_cancel_task(sch, p);
			p->scx.flags &= ~SCX_TASK_SUB_INIT;
		}
	}
	scx_task_iter_stop(&sti);
	scx_enabling_sub_sched = NULL;
err_unlock_and_disable:
	/* we'll soon enter disable path, keep bypass on */
	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);
err_disable:
	mutex_unlock(&scx_enable_mutex);
	/*
	 * Some enable failures only return an errno (e.g. -ENOMEM from an
	 * allocation) without calling scx_error(). Record it so
	 * scx_flush_disable_work() runs the disable and ops.exit() fires.
	 */
	scx_error(sch, "scx_sub_enable() failed (%d)", ret);
	scx_flush_disable_work(sch);
	cmd->ret = 0;
}

static s32 scx_cgroup_lifetime_notify(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct cgroup *cgrp = data;
	struct cgroup *parent = cgroup_parent(cgrp);

	if (!cgroup_on_dfl(cgrp))
		return NOTIFY_OK;

	switch (action) {
	case CGROUP_LIFETIME_ONLINE:
		/* inherit ->scx_sched from $parent */
		if (parent)
			rcu_assign_pointer(cgrp->scx_sched, parent->scx_sched);
		break;
	case CGROUP_LIFETIME_OFFLINE:
		/* if there is a sched attached, shoot it down */
		if (cgrp->scx_sched && cgrp->scx_sched->cgrp == cgrp)
			scx_exit(cgrp->scx_sched, SCX_EXIT_UNREG_KERN,
				 SCX_ECODE_RSN_CGROUP_OFFLINE,
				 "cgroup %llu going offline", cgroup_id(cgrp));
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block scx_cgroup_lifetime_nb = {
	.notifier_call = scx_cgroup_lifetime_notify,
};

static s32 __init scx_cgroup_lifetime_notifier_init(void)
{
	return blocking_notifier_chain_register(&cgroup_lifetime_notifier,
						&scx_cgroup_lifetime_nb);
}
core_initcall(scx_cgroup_lifetime_notifier_init);

void scx_pstack_recursion_on_dispatch(struct bpf_prog *prog)
{
	struct scx_sched *sch;

	guard(rcu)();
	sch = scx_prog_sched(prog->aux);
	if (unlikely(!sch))
		return;

	scx_error(sch, "dispatch recursion detected");
}

__bpf_kfunc_start_defs();

/**
 * scx_bpf_sub_dispatch - Trigger dispatching on a child scheduler
 * @cgroup_id: cgroup ID of the child scheduler to dispatch
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Allows a parent scheduler to trigger dispatching on one of its direct
 * child schedulers. The child scheduler runs its dispatch operation to
 * move tasks from dispatch queues to the local runqueue.
 *
 * Returns: true on success, false if cgroup_id is invalid, not a direct
 * child, or caller lacks dispatch permission.
 */
__bpf_kfunc bool scx_bpf_sub_dispatch(u64 cgroup_id, const struct bpf_prog_aux *aux)
{
	struct rq *this_rq = this_rq();
	struct scx_sched *parent, *child;

	guard(rcu)();
	parent = scx_prog_sched(aux);
	if (unlikely(!parent))
		return false;

	child = scx_find_sub_sched(cgroup_id);

	if (unlikely(!child))
		return false;

	if (unlikely(scx_parent(child) != parent)) {
		scx_error(parent, "trying to dispatch a distant sub-sched on cgroup %llu",
			  cgroup_id);
		return false;
	}

	return scx_dispatch_sched(child, this_rq, this_rq->scx.sub_dispatch_prev,
				  true);
}

__bpf_kfunc_end_defs();

#endif	/* CONFIG_EXT_SUB_SCHED */
