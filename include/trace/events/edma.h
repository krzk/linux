/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM edma

/*
 * Notice that this file is not protected like a normal header.
 * We also must allow for rereading of this file. The
 *
 *  || defined(TRACE_HEADER_MULTI_READ)
 *
 * serves this purpose.
 */
#if !defined(_TRACE_EDMA_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EDMA_H

/*
 * All trace headers should include tracepoint.h, until we finally
 * make it into a standard header.
 */
#include <linux/tracepoint.h>

TRACE_EVENT(edma_terminate_all,
	TP_PROTO(unsigned long fsl_chan, bool locked),
	TP_ARGS(fsl_chan, locked),
	TP_STRUCT__entry(
		__field(unsigned long, fsl_chan)
		__field(bool, locked)
	),
	TP_fast_assign(
		__entry->fsl_chan = fsl_chan;
		__entry->locked = locked;
	),
	TP_printk("fsl_chan=%lx locked=%d", __entry->fsl_chan, __entry->locked)
);

TRACE_EVENT(edma_xfer_desc,
	TP_PROTO(unsigned long fsl_chan, unsigned long edesc),
	TP_ARGS(fsl_chan, edesc),
	TP_STRUCT__entry(
		__field(unsigned long, fsl_chan)
		__field(unsigned long, edesc)
	),
	TP_fast_assign(
		__entry->fsl_chan = fsl_chan;
		__entry->edesc = edesc;
	),
	TP_printk("fsl_chan=%lx edesc=%lx", __entry->fsl_chan, __entry->edesc)
);

TRACE_EVENT(edma_tx_handler,
	TP_PROTO(unsigned long fsl_chan, bool locked, unsigned long edesc),
	TP_ARGS(fsl_chan, locked, edesc),
	TP_STRUCT__entry(
		__field(unsigned long, fsl_chan)
		__field(bool, locked)
		__field(unsigned long, edesc)
	),
	TP_fast_assign(
		__entry->fsl_chan = fsl_chan;
		__entry->locked = locked;
		__entry->edesc = edesc;
	),
	TP_printk("fsl_chan=%lx locked=%d edesc=%lx", __entry->fsl_chan, __entry->locked, __entry->edesc)
);

#endif

/***** NOTICE! The #if protection ends here. *****/

#include <trace/define_trace.h>
