/* SPDX-License-Identifier: GPL-2.0 */
#if !defined(_MSM_GPU_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _MSM_GPU_TRACE_H_

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM drm_msm_gpu
#define TRACE_INCLUDE_FILE msm_gpu_trace

TRACE_EVENT(msm_gpu_submit,
	    TP_PROTO(pid_t pid, u32 ringid, u32 id, u32 nr_bos, u32 nr_cmds),
	    TP_ARGS(pid, ringid, id, nr_bos, nr_cmds),
	    TP_STRUCT__entry(
		    __field(pid_t, pid)
		    __field(u32, id)
		    __field(u32, ringid)
		    __field(u32, nr_cmds)
		    __field(u32, nr_bos)
		    ),
	    TP_fast_assign(
		    __entry->pid = pid;
		    __entry->id = id;
		    __entry->ringid = ringid;
		    __entry->nr_bos = nr_bos;
		    __entry->nr_cmds = nr_cmds
		    ),
	    TP_printk("id=%d pid=%d ring=%d bos=%d cmds=%d",
		    __entry->id, __entry->pid, __entry->ringid,
		    __entry->nr_bos, __entry->nr_cmds)
);

TRACE_EVENT(msm_gpu_submit_flush,
	    TP_PROTO(struct msm_gem_submit *submit, u64 ticks),
	    TP_ARGS(submit, ticks),
	    TP_STRUCT__entry(
		    __field(pid_t, pid)
		    __field(u32, id)
		    __field(u32, ringid)
		    __field(u32, seqno)
		    __field(u64, ticks)
		    ),
	    TP_fast_assign(
		    __entry->pid = pid_nr(submit->pid);
		    __entry->id = submit->ident;
		    __entry->ringid = submit->ring->id;
		    __entry->seqno = submit->seqno;
		    __entry->ticks = ticks;
		    ),
	    TP_printk("id=%d pid=%d ring=%d:%d ticks=%lld",
		    __entry->id, __entry->pid, __entry->ringid, __entry->seqno,
		    __entry->ticks)
);

TRACE_EVENT(msm_gpu_submit_flush_wptr,
	    TP_PROTO(struct msm_ringbuffer *ring, u32 wptr, bool skipped_wptr_write),
	    TP_ARGS(ring, wptr, skipped_wptr_write),
	    TP_STRUCT__entry(
		    __field(u32, wptr)
		    __field(u32, ring)
		    __field(u32, skipped_wptr_write)
		    ),
	    TP_fast_assign(
		    __entry->wptr = wptr;
		    __entry->ring = ring->id;
		    __entry->skipped_wptr_write = skipped_wptr_write;
		    ),
	    TP_printk("ring %u wptr=%x skipped=%u",
		    __entry->ring, __entry->wptr, __entry->skipped_wptr_write)
);


TRACE_EVENT(msm_gpu_submit_retired,
	    TP_PROTO(struct msm_gem_submit *submit, u64 elapsed, u64 clock,
		    u64 start, u64 end),
	    TP_ARGS(submit, elapsed, clock, start, end),
	    TP_STRUCT__entry(
		    __field(pid_t, pid)
		    __field(u32, id)
		    __field(u32, ringid)
		    __field(u32, seqno)
		    __field(u64, elapsed)
		    __field(u64, clock)
		    __field(u64, start_ticks)
		    __field(u64, end_ticks)
		    ),
	    TP_fast_assign(
		    __entry->pid = pid_nr(submit->pid);
		    __entry->id = submit->ident;
		    __entry->ringid = submit->ring->id;
		    __entry->seqno = submit->seqno;
		    __entry->elapsed = elapsed;
		    __entry->clock = clock;
		    __entry->start_ticks = start;
		    __entry->end_ticks = end;
		    ),
	    TP_printk("id=%d pid=%d ring=%d:%d elapsed=%lld ns mhz=%lld start=%lld end=%lld",
		    __entry->id, __entry->pid, __entry->ringid, __entry->seqno,
		    __entry->elapsed, __entry->clock,
		    __entry->start_ticks, __entry->end_ticks)
);


TRACE_EVENT(msm_gpu_freq_change,
		TP_PROTO(u32 freq),
		TP_ARGS(freq),
		TP_STRUCT__entry(
			__field(u32, freq)
			),
		TP_fast_assign(
			/* trace freq in MHz to match intel_gpu_freq_change, to make life easier
			 * for userspace
			 */
			__entry->freq = DIV_ROUND_UP(freq, 1000000);
			),
		TP_printk("new_freq=%u", __entry->freq)
);


TRACE_EVENT(msm_gmu_freq_change,
		TP_PROTO(u32 freq, u32 perf_index),
		TP_ARGS(freq, perf_index),
		TP_STRUCT__entry(
			__field(u32, freq)
			__field(u32, perf_index)
			),
		TP_fast_assign(
			__entry->freq = freq;
			__entry->perf_index = perf_index;
			),
		TP_printk("freq=%u, perf_index=%u", __entry->freq, __entry->perf_index)
);


TRACE_EVENT(msm_gem_shrink,
		TP_PROTO(u32 nr_to_scan, u32 purged, u32 evicted,
			 u32 active_purged, u32 active_evicted),
		TP_ARGS(nr_to_scan, purged, evicted, active_purged, active_evicted),
		TP_STRUCT__entry(
			__field(u32, nr_to_scan)
			__field(u32, purged)
			__field(u32, evicted)
			__field(u32, active_purged)
			__field(u32, active_evicted)
			),
		TP_fast_assign(
			__entry->nr_to_scan = nr_to_scan;
			__entry->purged = purged;
			__entry->evicted = evicted;
			__entry->active_purged = active_purged;
			__entry->active_evicted = active_evicted;
			),
		TP_printk("nr_to_scan=%u pg, purged=%u pg, evicted=%u pg, active_purged=%u pg, active_evicted=%u pg",
			  __entry->nr_to_scan, __entry->purged, __entry->evicted,
			  __entry->active_purged, __entry->active_evicted)
);


TRACE_EVENT(msm_gem_purge_vmaps,
		TP_PROTO(u32 unmapped),
		TP_ARGS(unmapped),
		TP_STRUCT__entry(
			__field(u32, unmapped)
			),
		TP_fast_assign(
			__entry->unmapped = unmapped;
			),
		TP_printk("Purging %u vmaps", __entry->unmapped)
);


TRACE_EVENT(msm_gpu_suspend,
		TP_PROTO(int dummy),
		TP_ARGS(dummy),
		TP_STRUCT__entry(
			__field(u32, dummy)
			),
		TP_fast_assign(
			__entry->dummy = dummy;
			),
		TP_printk("%u", __entry->dummy)
);


TRACE_EVENT(msm_gpu_resume,
		TP_PROTO(int dummy),
		TP_ARGS(dummy),
		TP_STRUCT__entry(
			__field(u32, dummy)
			),
		TP_fast_assign(
			__entry->dummy = dummy;
			),
		TP_printk("%u", __entry->dummy)
);

TRACE_EVENT(msm_gpu_preemption_trigger,
		TP_PROTO(u32 ring_id_from, u32 ring_id_to, u32 wptr),
		TP_ARGS(ring_id_from, ring_id_to, wptr),
		TP_STRUCT__entry(
			__field(u32, ring_id_from)
			__field(u32, ring_id_to)
			__field(u32, wptr)
			),
		TP_fast_assign(
			__entry->ring_id_from = ring_id_from;
			__entry->ring_id_to = ring_id_to;
			__entry->wptr = wptr;
			),
		TP_printk("preempting %u -> %u wptr %x", __entry->ring_id_from, __entry->ring_id_to, __entry->wptr)
);

TRACE_EVENT(msm_gpu_preemption_irq,
		TP_PROTO(u32 ring_id),
		TP_ARGS(ring_id),
		TP_STRUCT__entry(
			__field(u32, ring_id)
			),
		TP_fast_assign(
			__entry->ring_id = ring_id;
			),
		TP_printk("preempted to %u", __entry->ring_id)
);

TRACE_EVENT(msm_gpu_fault,
		TP_PROTO(u32 ring_id),
		TP_ARGS(ring_id),
		TP_STRUCT__entry(
			__field(u32, ring_id)
			),
		TP_fast_assign(
			__entry->ring_id = ring_id;
			),
		TP_printk("fault current ring %u", __entry->ring_id)
);

TRACE_EVENT(msm_gpu_submit_ib,
		TP_PROTO(u32 ring_id, u32 ring_offset, u64 ib_address),
		TP_ARGS(ring_id, ring_offset, ib_address),
		TP_STRUCT__entry(
			__field(u32, ring_id)
			__field(u32, ring_offset)
			__field(u64, ib_address)
			),
		TP_fast_assign(
			__entry->ring_id = ring_id;
			__entry->ring_offset = ring_offset;
			__entry->ib_address = ib_address;
			),
		TP_printk("ib sumbmitted to ring %u (%x) %llx", __entry->ring_id, __entry->ring_offset, __entry->ib_address)
);

TRACE_EVENT(msm_gpu_write_ring,
		TP_PROTO(u32 ring_id, u32 word),
		TP_ARGS(ring_id, word),
		TP_STRUCT__entry(
			__field(u32, ring_id)
			__field(u32, word)
			),
		TP_fast_assign(
			__entry->ring_id = ring_id;
			__entry->word = word;
			),
		TP_printk("ring %i written %x", __entry->ring_id, __entry->word)
);

TRACE_EVENT(msm_gpu_wptr_update,
		TP_PROTO(struct msm_ringbuffer *ring, u32 old, u32 new),
		TP_ARGS(ring, old, new),
		TP_STRUCT__entry(
			__field(u32, ring)
			__field(u32, old)
			__field(u32, new)
			),
		TP_fast_assign(
			__entry->ring = ring->id;
			__entry->old = old;
			__entry->new = new;
			),
		TP_printk("ring %u wptr %x->%x", __entry->ring, __entry->old, __entry->new)
);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm/msm
#include <trace/define_trace.h>
