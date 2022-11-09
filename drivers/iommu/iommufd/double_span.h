/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef __IOMMUFD_DOUBLE_SPAN_H
#define __IOMMUFD_DOUBLE_SPAN_H

#include <linux/interval_tree.h>

struct interval_tree_double_span_iter {
	struct rb_root_cached *itrees[2];
	struct interval_tree_span_iter spans[2];
	union {
		unsigned long start_hole;
		unsigned long start_used;
	};
	union {
		unsigned long last_hole;
		unsigned long last_used;
	};
	/* 0 = hole, 1 = used span[0], 2 = used span[1], -1 done iteration */
	int is_used;
};

static void interval_tree_double_span_iter_update(
	struct interval_tree_double_span_iter *iter)
{
	unsigned long last_hole = ULONG_MAX;
	unsigned int i;

	for (i = 0; i != ARRAY_SIZE(iter->spans); i++) {
		if (interval_tree_span_iter_done(&iter->spans[i])) {
			iter->is_used = -1;
			return;
		}

		if (iter->spans[i].is_hole) {
			last_hole = min(last_hole, iter->spans[i].last_hole);
			continue;
		}

		iter->is_used = i + 1;
		iter->start_used = iter->spans[i].start_used;
		iter->last_used = min(iter->spans[i].last_used, last_hole);
		return;
	}

	iter->is_used = 0;
	iter->start_hole = iter->spans[0].start_hole;
	iter->last_hole =
		min(iter->spans[0].last_hole, iter->spans[1].last_hole);
}

static void interval_tree_double_span_iter_first(
	struct interval_tree_double_span_iter *iter,
	struct rb_root_cached *itree1, struct rb_root_cached *itree2,
	unsigned long first_index, unsigned long last_index)
{
	unsigned int i;

	iter->itrees[0] = itree1;
	iter->itrees[1] = itree2;
	for (i = 0; i != ARRAY_SIZE(iter->spans); i++)
		interval_tree_span_iter_first(&iter->spans[i], iter->itrees[i],
					      first_index, last_index);
	interval_tree_double_span_iter_update(iter);
}

static void
interval_tree_double_span_iter_next(struct interval_tree_double_span_iter *iter)
{
	unsigned int i;

	if (iter->is_used == -1 ||
	    iter->last_hole == iter->spans[0].last_index) {
		iter->is_used = -1;
		return;
	}

	for (i = 0; i != ARRAY_SIZE(iter->spans); i++)
		interval_tree_span_iter_advance(
			&iter->spans[i], iter->itrees[i], iter->last_hole + 1);
	interval_tree_double_span_iter_update(iter);
}

static inline bool
interval_tree_double_span_iter_done(struct interval_tree_double_span_iter *state)
{
	return state->is_used == -1;
}

#define interval_tree_for_each_double_span(span, itree1, itree2, first_index, \
					   last_index)                        \
	for (interval_tree_double_span_iter_first(span, itree1, itree2,       \
						  first_index, last_index);   \
	     !interval_tree_double_span_iter_done(span);                      \
	     interval_tree_double_span_iter_next(span))

#endif
