/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared definitions between scx_qmap.bpf.c and scx_qmap.c.
 *
 * The scheduler keeps all state in a single BPF arena map. struct
 * qmap_arena is the one object that lives at the base of the arena and is
 * mmap'd into userspace so the loader can read counters directly.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
#ifndef __SCX_QMAP_H
#define __SCX_QMAP_H

#ifdef __BPF__
#include <scx/bpf_arena_common.bpf.h>
#else
#include <linux/types.h>
#include <scx/bpf_arena_common.h>
#endif

#define MAX_SUB_SCHEDS		8

/*
 * cpu_ctxs[] is sized to a fixed cap so the layout is shared between BPF and
 * userspace. Keep this in sync with NR_CPUS used by the BPF side.
 */
#define SCX_QMAP_MAX_CPUS	1024

struct cpu_ctx {
	u64 dsp_idx;		/* dispatch index */
	u64 dsp_cnt;		/* remaining count */
	u32 avg_weight;
	u32 cpuperf_target;
};

/* Opaque to userspace; defined in scx_qmap.bpf.c. */
struct task_ctx;

struct qmap_fifo {
	struct task_ctx __arena *head;
	struct task_ctx __arena *tail;
	s32 idx;
};

struct qmap_arena {
	/* userspace-visible stats */
	u64 nr_enqueued, nr_dispatched, nr_reenqueued, nr_reenqueued_cid0;
	u64 nr_dequeued, nr_ddsp_from_enq;
	u64 nr_core_sched_execed;
	u64 nr_expedited_local, nr_expedited_remote;
	u64 nr_expedited_lost, nr_expedited_from_timer;
	u64 nr_highpri_queued;
	u32 test_error_cnt;
	u32 cpuperf_min, cpuperf_avg, cpuperf_max;
	u32 cpuperf_target_min, cpuperf_target_avg, cpuperf_target_max;

	/* kernel-side runtime state */
	u64 sub_sched_cgroup_ids[MAX_SUB_SCHEDS];
	u64 core_sched_head_seqs[5];
	u64 core_sched_tail_seqs[5];

	struct cpu_ctx cpu_ctxs[SCX_QMAP_MAX_CPUS];

	/* task_ctx slab; allocated and threaded by qmap_init() */
	struct task_ctx __arena *task_ctxs;
	struct task_ctx __arena *task_free_head;

	/* five priority FIFOs, each a doubly-linked list through task_ctx */
	struct qmap_fifo fifos[5];
};

#endif /* __SCX_QMAP_H */
