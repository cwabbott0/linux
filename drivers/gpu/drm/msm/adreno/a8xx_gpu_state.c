// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "msm_gem.h"
#include "a6xx_gpu.h"
#include "a6xx_gmu.h"
#include "adreno_snapshot.h"
#include "adreno_gen8_snapshot.h"

#define NUM_SLICES(gpu)  (hweight32((gpu)->slice_mask))

static inline int next_slice(int slice, u32 slice_mask)
{
	for (; slice < fls(slice_mask); slice++)
		if (BIT(slice) & slice_mask)
			return slice;

	return slice;
}

#define NEXT_SLICE(_slice, _region, _slice_mask) \
	((_region == UNSLICE) ? fls(_slice_mask) : next_slice(_slice, _slice_mask))

#define FIRST_SLICE(_region, _slice_mask) \
	((_region == UNSLICE) ? 0 : NEXT_SLICE(0, _region, _slice_mask))

#define FOR_EACH_SLICE(_slice, _region, _slice_mask) \
	for (_slice = FIRST_SLICE(_region, _slice_mask); \
	     _slice < fls(_slice_mask); \
	     _slice = NEXT_SLICE(++(_slice), _region, _slice_mask))

/* Use SLICE_ID to access the region (0 for unsliced) */
#define SLICE_ID(region, j) ((region == SLICE) ? j : 0)

/*
 * Use HEADER_SLICE_ID to specify the slice in the section header (UINT_MAX for unsliced).
 * This allows snapshot parsers to differentiate between slice ID 0 and unsliced regions.
 */
#define HEADER_SLICE_ID(region, j) ((region == SLICE) ? j : UINT_MAX)

#define GEN8_CP_APERTURE_REG_VAL_NS(_sliceid, _pipe, _cluster, _context) ( \
		A8XX_CP_APERTURE_CNTL_HOST_SLICEID(_sliceid) |		\
		A8XX_CP_APERTURE_CNTL_HOST_PIPEID(_pipe) |		\
		A8XX_CP_APERTURE_CNTL_HOST_CLUSTERID(_cluster) |	\
		A8XX_CP_APERTURE_CNTL_HOST_CONTEXTID3D(_context) |	\
		0)

#define GEN8_CP_APERTURE_REG_VAL(_sliceid, _pipe, _cluster, _context) (	\
		A8XX_CP_APERTURE_CNTL_HOST_USESLICEID |			\
		GEN8_CP_APERTURE_REG_VAL_NS(_sliceid, _pipe, _cluster, _context))

#define GEN8_SP_READ_SEL_VAL(_contextid, _sliceid, _location, _pipe, _statetype, _usptp, _sptp) ( \
		A7XX_SP_READ_SEL_CONTEXT(_contextid) |			\
		A7XX_SP_READ_SEL_SLICE(_sliceid) |			\
		A7XX_SP_READ_SEL_LOCATION(_location) |			\
		A7XX_SP_READ_SEL_PIPE(_pipe) |				\
		A7XX_SP_READ_SEL_STATETYPE(_statetype) |		\
		A7XX_SP_READ_SEL_USPTP(_usptp) |			\
		A7XX_SP_READ_SEL_SPTP(_sptp) |				\
		0)

/*
 * Allocate 4MB for the crashdumper scratch region - 2MB for the script and
 * the rest for the data.
 *
 * Note gen8_snapshot_indexed_registers() is worst case, needing separate
 * CD_WRITE + CD_READ per reg, 0xa000 times (so script is ~1.2MB)
 *
 * TODO we could perhaps be clever and fill in the out addrs in a 2nd pass
 * to accomodate invocations that need larger script and smaller data, like
 * indexed regs.
 */
#define A8XX_CD_SIZE        SZ_4M
#define A8XX_CD_DATA_OFFSET (2 * SZ_1M)
#define A8XX_CD_DATA_SIZE   (A8XX_CD_SIZE - A8XX_CD_DATA_OFFSET)

static int a8xx_crashdumper_init(struct msm_gpu *gpu,
				 struct adreno_crashdumper *dumper)
{
	dumper->ptr = msm_gem_kernel_new(gpu->dev,
		A8XX_CD_SIZE, MSM_BO_WC, gpu->vm,
		&dumper->bo, &dumper->iova);

	if (!IS_ERR(dumper->ptr)) {
		msm_gem_object_set_name(dumper->bo, "crashdump");
		dumper->data_offset = A8XX_CD_DATA_OFFSET;
	}

	return PTR_ERR_OR_ZERO(dumper->ptr);
}

static inline int __cd_run(struct msm_gpu *gpu, struct adreno_crashdumper *dumper)
{
	u32 val;
	int ret;

	if (IS_ERR_OR_NULL(dumper->ptr))
		return -EINVAL;

	/* Make sure all pending memory writes are posted */
	wmb();

	gpu_write64(gpu, REG_A8XX_CP_CRASH_DUMP_SCRIPT_BASE, dumper->iova);

	gpu_write(gpu, REG_A8XX_CP_CRASH_DUMP_CNTL, 1);

	/* NOTE: kgsl uses 500ms timeout */
	ret = gpu_poll_timeout(gpu, REG_A8XX_CP_CRASH_DUMP_STATUS, val,
			       val & 0x02, 100, 100000);

	gpu_write(gpu, REG_A8XX_CP_CRASH_DUMP_CNTL, 0);

	drm_WARN_ONCE(gpu->dev, ret, "crashdumper timed out: status=%08x (%d)\n", val, ret);

	return ret;
}

static inline bool __cd_check_overflow(struct cd_script *p)
{
	if (WARN_ON_ONCE(p->in > (u64 *)(p->dumper->ptr + A8XX_CD_DATA_OFFSET)))
		return false;
	if (WARN_ON_ONCE(p->out > p->dumper->iova + A8XX_CD_SIZE))
		return false;
	return true;
}

static const struct gen8_snapshot_block_list *
block_list(struct msm_gpu *gpu)
{
	return to_adreno_gpu(gpu)->info->snapshot_block_list;
}

/* The GMU is ioremapped separately, matching how it is described in dt.
 * However the snapshot tables generated from hw spec do not take into
 * account dt, and consider it as yet another sub-block of the GPU.  This
 * helper helps to bridge the worlds.
 */
static uint32_t reg_read(struct msm_gpu *gpu, uint32_t reg)
{
	struct a6xx_gmu *gmu = &to_a6xx_gpu(to_adreno_gpu(gpu))->gmu;
	if ((reg << 2) >= gmu->mmio_offset)
		return gmu_read(gmu, reg);
	return gpu_read(gpu, reg);
}

static void adreno_snapshot_registers_v2(struct msm_gpu *gpu,
					 struct adreno_gpu_state *state,
					 const u32 *ptr)
{
	int k, count;

	/* Figure out how many registers we are going to dump */
	count = adreno_snapshot_regs_count(ptr);

	snapshot_sect (state, REGS_V2, 4 * count, {}) {
		for (; ptr[0] != UINT_MAX; ptr += 2) {
			u32 cnt = REG_COUNT(ptr);

			if (cnt == 1) {
				snapshot_sect_u32(&sect, BIT(31) | ptr[0]);
			} else {
				snapshot_sect_u32(&sect, ptr[0]);
				snapshot_sect_u32(&sect, cnt);
			}

			for (k = ptr[0]; k <= ptr[1]; k++) {
				snapshot_sect_u32(&sect, reg_read(gpu, k));
			}
		}
	}
}

static void snapshot_mvc_regs_v3_subheader(struct adreno_snapshot_sect *sect, const u32 *ptr)
{
	/*
	 * Note this might look like adreno_snapshot_registers_v2(),
	 * but looks are deceiving.  The meaning of b31 is inverted
	 * and the 2nd dword is end-offset instead of count.
	 */
	if (REG_COUNT(ptr) == 1) {
		snapshot_sect_u32(sect, ptr[0]);
	} else {
		snapshot_sect_u32(sect, BIT(31) | ptr[0]);
		snapshot_sect_u32(sect, ptr[1]);
	}
}

static void gen8_legacy_snapshot_registers(struct msm_gpu *gpu,
					   struct adreno_gpu_state *state,
					   const struct gen8_reg_list *regs,
					   u32 slice_id)
{
	int count;

	/* Figure out how many registers we are going to dump */
	count = adreno_snapshot_regs_count(regs->regs);

	snapshot_sect (state, MVC_V3, 4 * count, {
		.ctxt_id = 0,
		.cluster_id = CLUSTER_NONE,
		.pipe_id = PIPE_NONE,
		.location_id = UINT_MAX,
		.sp_id = UINT_MAX,
		.usptp_id = UINT_MAX,
		.slice_id = HEADER_SLICE_ID(regs->slice_region, slice_id),
	}) {
		unsigned long flags;

		if (regs->sel)
			gpu_write(gpu, regs->sel->host_reg, regs->sel->val);

		if (regs->slice_region) {
			u32 val = GEN8_CP_APERTURE_REG_VAL(slice_id, 0, 0, 0);
			a8xx_aperture_acquire_raw(gpu, val, &flags);
		}

		/* Make sure the previous writes are posted before reading */
		mb();

		for (const u32 *ptr = regs->regs; ptr[0] != UINT_MAX; ptr += 2) {
			snapshot_mvc_regs_v3_subheader(&sect, ptr);

			for (int k = ptr[0]; k <= ptr[1]; k++) {
				snapshot_sect_u32(&sect, reg_read(gpu, k));
			}
		}

		if (regs->slice_region)
			a8xx_aperture_release(gpu, flags);
	}
}

static bool gen8_reglist_snapshot(struct msm_gpu *gpu, struct adreno_gpu_state *state,
				  struct adreno_crashdumper *dumper)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);
	u32 slice_mask = to_a6xx_gpu(to_adreno_gpu(gpu))->slice_mask;

	if (!dumper) {
		for (u32 i = 0; i < b->num_reg_list; i++) {
			const struct gen8_reg_list *regs = &b->reg_list[i];
			u32 j;

			FOR_EACH_SLICE (j, regs->slice_region, slice_mask) {
				u32 slice_id = SLICE_ID(regs->slice_region, j);
				gen8_legacy_snapshot_registers(gpu, state, regs, slice_id);
			}

			/*
			 * The downstream reglist contains registers in other memory regions
			 * (cx_misc/cx_mem and cx_dbgc) and we need to plumb through their
			 * offsets and map them to read them on the CPU. For now only read the
			 * first two regions.
			 */
			if (i == 1)
				break;
		}
		return true;
	}

	for (u32 i = 0; i < b->num_reg_list; i++) {
		const struct gen8_reg_list *regs = &b->reg_list[i];
		u32 j;

		FOR_EACH_SLICE (j, regs->slice_region, slice_mask) {
			u32 slice_id = SLICE_ID(regs->slice_region, j);
			struct cd_script p = CD_INIT(dumper);

			CD_WRITE(&p, REG_A8XX_CP_APERTURE_CNTL_CD,
				 GEN8_CP_APERTURE_REG_VAL(j, 0, 0, 0));

			/* Program the SEL_CNTL_CD register appropriately */
			if (regs->sel)
				CD_WRITE(&p, regs->sel->cd_reg, regs->sel->val);

			for (const u32 *ptr = regs->regs; ptr[0] != UINT_MAX; ptr += 2) {
				u32 count = REG_COUNT(ptr);

				CD_READ(&p, ptr[0], count);
			}

			/* Marker for end of script */
			CD_FINISH(&p);

			/* Try to run the crash dumper and bail if it times out */
			if (CD_RUN(&p, gpu))
				return false;

			u32 count = adreno_snapshot_regs_count(regs->regs);
			snapshot_sect (state, MVC_V3, 4 * count, {
				.ctxt_id = 0,
				.cluster_id = CLUSTER_NONE,
				.pipe_id = PIPE_NONE,
				.location_id = UINT_MAX,
				.sp_id = UINT_MAX,
				.usptp_id = UINT_MAX,
				.slice_id = HEADER_SLICE_ID(regs->slice_region, slice_id),
			}) {
				const u32 *src = dumper->ptr + A8XX_CD_DATA_OFFSET;

				for (const u32 *ptr = regs->regs; ptr[0] != UINT_MAX; ptr += 2) {
					u32 count = REG_COUNT(ptr);

					snapshot_mvc_regs_v3_subheader(&sect, ptr);
					snapshot_sect_data(&sect, src, count * sizeof(u32));

					src += count;
				}
			}
		}
	}

	return true;
}

static bool gen8_snapshot_indexed_registers(
	struct msm_gpu *gpu, struct adreno_gpu_state *state, struct adreno_crashdumper *dumper,
	u32 index, u32 data, u32 start, u32 count, u32 pipe_id, u32 slice_id)
{
	struct adreno_snapshot_indexed_regs_v2 hdr = {
		.index_reg = index,
		.data_reg = data,
		.count = count,
		.start = start,
		.pipe_id = pipe_id,
		.slice_id = slice_id,
	};
	u32 s = (slice_id == UINT_MAX) ? 0 : slice_id;

	if (!dumper) {
		snapshot_sect (state, INDEXED_REGS_V2, count * sizeof(u32), hdr) {
			unsigned long flags;

			u32 val = GEN8_CP_APERTURE_REG_VAL(s, pipe_id, 0, 0);
			a8xx_aperture_acquire_raw(gpu, val, &flags);

			for (int i = 0; i < count; i++) {
				/* Write the address register */
				gpu_write(gpu, index, start + i);

				/* Make sure the write finishes */
				wmb();

				snapshot_sect_u32(&sect, reg_read(gpu, data));

				/* Make sure the read finishes */
				rmb();
			}

			a8xx_aperture_release(gpu, flags);
		}

		return true;
	}

	struct cd_script p = CD_INIT(dumper);

	CD_WRITE(&p, REG_A8XX_CP_APERTURE_CNTL_CD,
		 GEN8_CP_APERTURE_REG_VAL(s, pipe_id, 0, 0));

	for (int i = 0; i < count; i++) {
		/* Write the address register */
		CD_WRITE(&p, index, start + i);
		CD_READ(&p, data, 1);
	}

	/* Marker for end of script */
	CD_FINISH(&p);

	/* Try to run the crash dumper and bail if it times out */
	if (CD_RUN(&p, gpu))
		return false;

	snapshot_sect (state, INDEXED_REGS_V2, count * sizeof(u32), hdr) {
		const u32 *src = dumper->ptr + A8XX_CD_DATA_OFFSET;
		snapshot_sect_data(&sect, src, count * sizeof(u32));
	}

	return true;
}

static bool gen8_snapshot_cp_indexed_regs(struct msm_gpu *gpu,
					  struct adreno_gpu_state *state,
					  struct adreno_crashdumper *dumper)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);

	for (int i = 0; i < b->index_registers_len; i++) {
		bool ret = gen8_snapshot_indexed_registers(gpu, state, dumper,
			b->index_registers[i].addr,
			b->index_registers[i].data, 0,
			b->index_registers[i].size,
			b->index_registers[i].pipe_id, UINT_MAX);
		if (!ret)
			return false;
	}

	return true;
}

static bool gen8_snapshot_mempool(struct msm_gpu *gpu,
				  struct adreno_gpu_state *state,
				  struct adreno_crashdumper *dumper)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(to_adreno_gpu(gpu));
	u32 slice_mask = a6xx_gpu->slice_mask;
	u32 first_slice = a8xx_get_first_slice(a6xx_gpu);
	bool ret = true;

	for (int i = 0; i < b->mempool_index_registers_len && ret; i++) {
		const struct gen8_cp_indexed_reg *cp_indexed_reg =
			&b->mempool_index_registers[i];
		unsigned long flags;
		u32 j;

		FOR_EACH_SLICE (j, cp_indexed_reg->slice_region, slice_mask) {
			u32 pipe_id = cp_indexed_reg->pipe_id;

			/* Set CP_CHICKEN_DBG[StabilizeMVC] to stabilize it while dumping */
			a8xx_aperture_acquire_raw(gpu,
				GEN8_CP_APERTURE_REG_VAL_NS(first_slice, pipe_id, 0, 0),
				&flags);
			gpu_rmw(gpu, REG_A8XX_CP_CHICKEN_DBG_PIPE, BIT(2), BIT(2));
			a8xx_aperture_release(gpu, flags);

			a8xx_aperture_acquire_raw(gpu,
				GEN8_CP_APERTURE_REG_VAL(j, pipe_id, 0, 0),
				&flags);
			gpu_rmw(gpu, REG_A8XX_CP_SLICE_CHICKEN_DBG_PIPE, BIT(2), BIT(2));
			a8xx_aperture_release(gpu, flags);

			ret = gen8_snapshot_indexed_registers(gpu, state, dumper,
				cp_indexed_reg->addr,
				cp_indexed_reg->data, 0,
				cp_indexed_reg->size,
				cp_indexed_reg->pipe_id,
				HEADER_SLICE_ID(cp_indexed_reg->slice_region, j));

			/* Reset CP_CHICKEN_DBG[StabilizeMVC] once we are done */
			a8xx_aperture_acquire_raw(gpu,
				GEN8_CP_APERTURE_REG_VAL_NS(first_slice, pipe_id, 0, 0),
				&flags);
			gpu_rmw(gpu, REG_A8XX_CP_CHICKEN_DBG_PIPE, BIT(2), 0);
			a8xx_aperture_release(gpu, flags);

			a8xx_aperture_acquire_raw(gpu,
				GEN8_CP_APERTURE_REG_VAL(j, pipe_id, 0, 0),
				&flags);
			gpu_rmw(gpu, REG_A8XX_CP_SLICE_CHICKEN_DBG_PIPE, BIT(2), 0);
			a8xx_aperture_release(gpu, flags);

			if (!ret)
				break;
		}
	}

	/* Clear aperture register */
	a8xx_aperture_clear(gpu);

	return ret;
}

static void gen8_legacy_snapshot_mvc(struct msm_gpu *gpu,
				     struct adreno_gpu_state *state,
				     const struct gen8_cluster_registers *cluster,
				     u32 slice_id)
{
	int count;

	/* Figure out how many registers we are going to dump */
	count = adreno_snapshot_regs_count(cluster->regs);

	snapshot_sect (state, MVC_V3, 4 * count, {
		.ctxt_id = (cluster->context_id == STATE_FORCE_CTXT_1) ? 1 : 0,
		.cluster_id = cluster->cluster_id,
		.pipe_id = cluster->pipe_id,
		.location_id = UINT_MAX,
		.sp_id = UINT_MAX,
		.usptp_id = UINT_MAX,
		.slice_id = HEADER_SLICE_ID(cluster->slice_region, slice_id),
	}) {
		unsigned long flags;

		/*
		 * Set the AHB control for the Host to read from the
		 * cluster/context for this iteration.
		 */
		u32 val = GEN8_CP_APERTURE_REG_VAL(
			slice_id, cluster->pipe_id,
			cluster->cluster_id, cluster->context_id);
		a8xx_aperture_acquire_raw(gpu, val, &flags);

		if (cluster->sel)
			gpu_write(gpu, cluster->sel->host_reg, cluster->sel->val);

		/* Make sure the previous writes are posted before reading */
		mb();

		for (const u32 *ptr = cluster->regs; ptr[0] != UINT_MAX; ptr += 2) {
			snapshot_mvc_regs_v3_subheader(&sect, ptr);

			for (int k = ptr[0]; k <= ptr[1]; k++) {
				snapshot_sect_u32(&sect, reg_read(gpu, k));
			}
		}

		a8xx_aperture_release(gpu, flags);
	}
}

static bool gen8_snapshot_mvc_regs(
	struct msm_gpu *gpu, struct adreno_gpu_state *state, struct adreno_crashdumper *dumper,
	const struct gen8_cluster_registers *clusters, size_t num_cluster)
{
	u32 slice_mask = to_a6xx_gpu(to_adreno_gpu(gpu))->slice_mask;

	if (!dumper) {
		for (int i = 0; i < num_cluster; i++) {
			const struct gen8_cluster_registers *cluster = &clusters[i];
			u32 j;

			FOR_EACH_SLICE (j, cluster->slice_region, slice_mask) {
				u32 slice_id = SLICE_ID(cluster->slice_region, j);
				gen8_legacy_snapshot_mvc(gpu, state, cluster, slice_id);
			}
		}
		return true;
	}

	for (int i = 0; i < num_cluster; i++) {
		const struct gen8_cluster_registers *cluster = &clusters[i];
		u32 j;

		FOR_EACH_SLICE (j, cluster->slice_region, slice_mask) {
			u32 slice_id = SLICE_ID(cluster->slice_region, j);
			struct cd_script p = CD_INIT(dumper);

			CD_WRITE(&p, REG_A8XX_CP_APERTURE_CNTL_CD,
				 GEN8_CP_APERTURE_REG_VAL(j, cluster->pipe_id,
							  cluster->cluster_id,
							  cluster->context_id));

			if (cluster->sel)
				CD_WRITE(&p, cluster->sel->cd_reg, cluster->sel->val);

			for (const u32 *ptr = cluster->regs; ptr[0] != UINT_MAX; ptr += 2) {
				CD_READ(&p, ptr[0], REG_COUNT(ptr));
			}

			/* Marker for end of script */
			CD_FINISH(&p);

			/* Try to run the crash dumper and bail if it times out */
			if (CD_RUN(&p, gpu))
				return false;

			u32 count = adreno_snapshot_regs_count(cluster->regs);

			snapshot_sect (state, MVC_V3, 4 * count, {
				.ctxt_id = (cluster->context_id == STATE_FORCE_CTXT_1) ? 1 : 0,
				.cluster_id = cluster->cluster_id,
				.pipe_id = cluster->pipe_id,
				.location_id = UINT_MAX,
				.sp_id = UINT_MAX,
				.usptp_id = UINT_MAX,
				.slice_id = HEADER_SLICE_ID(cluster->slice_region, slice_id),
			}) {
				const u32 *src = dumper->ptr + A8XX_CD_DATA_OFFSET;

				for (const u32 *ptr = cluster->regs; ptr[0] != UINT_MAX; ptr += 2) {
					u32 count = REG_COUNT(ptr);

					snapshot_mvc_regs_v3_subheader(&sect, ptr);
					snapshot_sect_data(&sect, src, count * sizeof(u32));

					src += count;
				}
			}
		}
	}
	return true;
}

static u32 gen8_read_dbgahb(struct msm_gpu *gpu, u32 regbase, u32 reg)
{
	return gpu_read(gpu, REG_A7XX_SP_AHB_READ_APERTURE + reg - regbase);
}

static void gen8_legacy_snapshot_cluster_dbgahb(
	struct msm_gpu *gpu, struct adreno_gpu_state *state,
	const struct gen8_sptp_cluster_registers *cluster,
	u32 sp, u32 usptp, u32 slice_id)
{
	int count;

	/* Figure out how many registers we are going to dump */
	count = adreno_snapshot_regs_count(cluster->regs);

	snapshot_sect (state, MVC_V3, 4 * count, {
		.ctxt_id = cluster->context_id,
		.cluster_id = cluster->cluster_id,
		.pipe_id = cluster->pipe_id,
		.location_id = cluster->location_id,
		.sp_id = sp,
		.usptp_id = usptp,
		.slice_id = HEADER_SLICE_ID(cluster->slice_region, slice_id),
	}) {
		u32 read_sel = GEN8_SP_READ_SEL_VAL(
			cluster->context_id, slice_id, cluster->location_id,
			cluster->pipe_id, cluster->statetype, usptp, sp);

		gpu_write(gpu, REG_A7XX_SP_READ_SEL, read_sel);

		/*
		 * An explicit barrier is needed so that reads do not happen before
		 * the register write.
		 */
		mb();

		for (const u32 *ptr = cluster->regs; ptr[0] != UINT_MAX; ptr += 2) {
			snapshot_mvc_regs_v3_subheader(&sect, ptr);

			for (int k = ptr[0]; k <= ptr[1]; k++) {
				u32 val = gen8_read_dbgahb(gpu, cluster->regbase, k);
				snapshot_sect_u32(&sect, val);
			}
		}
	}
}

static bool gen8_snapshot_dbgahb_regs(struct msm_gpu *gpu,
				      struct adreno_gpu_state *state,
				      struct adreno_crashdumper *dumper)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);
	u32 slice_mask = to_a6xx_gpu(to_adreno_gpu(gpu))->slice_mask;

	if (!dumper) {
		for (int i = 0; i < b->num_sptp_clusters; i++) {
			const struct gen8_sptp_cluster_registers *cluster =
				&b->sptp_clusters[i];

			for (u32 sp = 0; sp < cluster->num_sps; sp++) {
				for (u32 usptp = 0; usptp < cluster->num_usptps; usptp++) {
					u32 j;

					FOR_EACH_SLICE (j, cluster->slice_region, slice_mask) {
						u32 slice_id = SLICE_ID(cluster->slice_region, j);

						gen8_legacy_snapshot_cluster_dbgahb(
							gpu, state, cluster,
							sp, usptp, slice_id);
					}
				}
			}
		}

		return true;
	}

	for (int i = 0; i < b->num_sptp_clusters; i++) {
		const struct gen8_sptp_cluster_registers *cluster =
			&b->sptp_clusters[i];

		for (u32 sp = 0; sp < cluster->num_sps; sp++) {
			for (u32 usptp = 0; usptp < cluster->num_usptps; usptp++) {
				u32 j;

				FOR_EACH_SLICE (j, cluster->slice_region, slice_mask) {
					u32 slice_id = SLICE_ID(cluster->slice_region, j);

					/* Build the crash script */
					struct cd_script p = CD_INIT(dumper);

					u32 read_sel = GEN8_SP_READ_SEL_VAL(
						cluster->context_id, slice_id,
						cluster->location_id, cluster->pipe_id,
						cluster->statetype, usptp, sp);

					/* Program the aperture */
					CD_WRITE(&p, REG_A7XX_SP_READ_SEL, read_sel);

					for (const u32 *ptr = cluster->regs; ptr[0] != UINT_MAX; ptr += 2) {
						u32 count = REG_COUNT(ptr);
						u32 reg = REG_A7XX_SP_AHB_READ_APERTURE + ptr[0] - cluster->regbase;

						CD_READ(&p, reg, count);
					}

					/* Marker for end of script */
					CD_FINISH(&p);

					/* Try to run the crash dumper and bail if it times out */
					if (CD_RUN(&p, gpu))
						return false;

					u32 count = adreno_snapshot_regs_count(cluster->regs);
					snapshot_sect (state, MVC_V3, 4 * count, {
						.ctxt_id = cluster->context_id,
						.cluster_id = cluster->cluster_id,
						.pipe_id = cluster->pipe_id,
						.location_id = cluster->location_id,
						.sp_id = sp,
						.usptp_id = usptp,
						.slice_id = HEADER_SLICE_ID(cluster->slice_region, slice_id),
					}) {
						const u32 *src = dumper->ptr + A8XX_CD_DATA_OFFSET;

						for (const u32 *ptr = cluster->regs; ptr[0] != UINT_MAX; ptr += 2) {
							u32 count = REG_COUNT(ptr);

							snapshot_mvc_regs_v3_subheader(&sect, ptr);
							snapshot_sect_data(&sect, src, count * sizeof(u32));

							src += count;
						}
					}
				}
			}
		}
	}

	return true;
}

static void gen8_legacy_snapshot_shader(struct msm_gpu *gpu, struct adreno_gpu_state *state,
					const struct gen8_shader_block *block,
					u32 sp, u32 ctxt, u32 slice_id)
{
	snapshot_sect (state, SHADER_V3, block->size * sizeof(u32), {
		.type = block->statetype,
		.slice_id = HEADER_SLICE_ID(block->slice_region, slice_id),
		.sp_index = sp,
		.usptp = block->usptp_id,
		.pipe_id = block->pipeid,
		.location = block->location,
		.ctxt_id = ctxt,
		.size = block->size,
	}) {
		u32 read_sel = GEN8_SP_READ_SEL_VAL(
			ctxt, slice_id, block->location, block->pipeid,
			block->statetype, block->usptp_id, sp);

		gpu_write(gpu, REG_A7XX_SP_READ_SEL, read_sel);

		/*
		 * An explicit barrier is needed so that reads do not happen before
		 * the register write.
		 */
		mb();

		for (int i = 0; i < block->size; i++) {
			u32 val = gpu_read(gpu, REG_A7XX_SP_AHB_READ_APERTURE + i);
			snapshot_sect_u32(&sect, val);
		}
	}
}

static bool gen8_snapshot_shader(struct msm_gpu *gpu,
				 struct adreno_gpu_state *state,
				 struct adreno_crashdumper *dumper)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);
	u32 slice_mask = to_a6xx_gpu(to_adreno_gpu(gpu))->slice_mask;

	if (!dumper) {
		for (int i = 0; i < b->num_shader_blocks; i++) {
			const struct gen8_shader_block *block = &b->shader_blocks[i];
			u32 j;

			FOR_EACH_SLICE (j, block->slice_region, slice_mask) {
				u32 slice_id = SLICE_ID(block->slice_region, j);

				for (u32 sp = 0; sp < block->num_sps; sp++) {
					for (u32 ctxt = 0; ctxt < block->num_ctx; ctxt++) {
						gen8_legacy_snapshot_shader(
							gpu, state, block, sp, ctxt, slice_id);
					}
				}
			}
		}

		return true;
	}

	for (int i = 0; i < b->num_shader_blocks; i++) {
		const struct gen8_shader_block *block = &b->shader_blocks[i];
		u32 j;

		FOR_EACH_SLICE (j, block->slice_region, slice_mask) {
			u32 slice_id = SLICE_ID(block->slice_region, j);

			for (u32 sp = 0; sp < block->num_sps; sp++) {
				for (u32 ctxt = 0; ctxt < block->num_ctx; ctxt++) {
					/* Build the crash script */
					struct cd_script p = CD_INIT(dumper);

					u32 read_sel = GEN8_SP_READ_SEL_VAL(
						ctxt, slice_id, block->location, block->pipeid,
						block->statetype, block->usptp_id, sp);

					/* Program the aperture */
					CD_WRITE(&p, REG_A7XX_SP_READ_SEL, read_sel);

					/* Read all the data in one chunk */
					CD_READ(&p, REG_A7XX_SP_AHB_READ_APERTURE, block->size);

					/* Marker for end of script */
					CD_FINISH(&p);

					/* Try to run the crash dumper and bail if it times out */
					if (CD_RUN(&p, gpu))
						return false;

					snapshot_sect (state, SHADER_V3, block->size * sizeof(u32), {
						.type = block->statetype,
						.slice_id = HEADER_SLICE_ID(block->slice_region, slice_id),
						.sp_index = sp,
						.usptp = block->usptp_id,
						.pipe_id = block->pipeid,
						.location = block->location,
						.ctxt_id = ctxt,
						.size = block->size,
					}) {
						const u32 *src = dumper->ptr + A8XX_CD_DATA_OFFSET;
						snapshot_sect_data(&sect, src, block->size * sizeof(u32));
					}
				}
			}
		}
	}

	return true;
}

static void a8xx_snapshot_gmu_bo(struct adreno_gpu_state *state,
				 struct a6xx_gmu_bo *bo,
				 unsigned type)
{
	snapshot_sect (state, GMU_MEMORY, bo->size, {
		.type = type,
		.gmuaddr = bo->iova,
	}) {
		snapshot_sect_data(&sect, bo->virt, bo->size);
	}
}

static void a8xx_snapshot_gpu_bo(struct adreno_gpu_state *state,
				 struct drm_gem_object *bo,
				 uint64_t iova)
{
	if (IS_ERR_OR_NULL(bo))
		return;

	snapshot_sect (state, GPU_OBJECT_V2, bo->size, {
		.type = SNAPSHOT_GPU_OBJECT_GLOBAL,
		.gpuaddr = iova,
		.size = bo->size / 4,  /* dwords */
	}) {
		void *ptr = msm_gem_get_vaddr(bo);
		snapshot_sect_data(&sect, ptr, bo->size);
		msm_gem_put_vaddr(bo);
	}
}

static void a8xx_snapshot_gmu_hfi_history(struct msm_gpu *gpu,
					  struct adreno_gpu_state *state)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct a6xx_gmu *gmu = &a6xx_gpu->gmu;
	unsigned i, j;

	BUILD_BUG_ON(ARRAY_SIZE(gmu->queues) != ARRAY_SIZE(state->hfi_queue_history));

	for (i = 0; i < ARRAY_SIZE(gmu->queues); i++) {
		struct a6xx_hfi_queue *queue = &gmu->queues[i];
		for (j = 0; j < HFI_HISTORY_SZ; j++) {
			unsigned idx = (j + queue->history_idx) % HFI_HISTORY_SZ;
			state->hfi_queue_history[i][j] = queue->history[idx];
		}
	}
}

#define GEN8_DEBUGBUS_BLOCK_SIZE 0x100

#define GEN8_DEBUGBUS_BLOCK_IDX_1 128
#define GEN8_DEBUGBUS_BLOCK_IDX_2 192

static void gen8_dbgc_debugbus_select(struct msm_gpu *gpu, u32 block_id, u32 index)
{
	u32 reg;

	reg = A8XX_DBGC_CFG_DBGBUS_SEL_D_PING_INDEX(index) |
		A8XX_DBGC_CFG_DBGBUS_SEL_D_PING_BLK_SEL(block_id);

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_A, reg);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_B, reg);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_C, reg);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_SEL_D, reg);

	/*
	 * There needs to be a delay of 1 us to ensure enough time for correct
	 * data is funneled into the trace buffer
	 */
	udelay(1);
}

static void gen8_dbgc_side_debug_bus_read(struct msm_gpu *gpu,
	u32 block_id, u32 index, struct adreno_snapshot_sect *sect)
{
	u32 reg, side_data;

	gen8_dbgc_debugbus_select(gpu, block_id, index);

	reg = gpu_read(gpu, REG_A6XX_DBGC_CFG_DBGBUS_OVER);
	side_data = FIELD_GET(GENMASK(27, 24), reg);

	snapshot_sect_u32(sect, side_data);
}

static void gen8_snapshot_dbgc_side_debugbus_block(
	struct msm_gpu *gpu, uint32_t block_id, size_t count,
	struct adreno_snapshot_sect *sect)
{
	for (int i = 0; i < count; i++)
		gen8_dbgc_side_debug_bus_read(gpu, block_id, i, sect);
}

static void gen8_dbgc_debug_bus_read(struct msm_gpu *gpu,
	u32 block_id, u32 index, struct adreno_snapshot_sect *sect)
{
	gen8_dbgc_debugbus_select(gpu, block_id, index);

	snapshot_sect_u32(sect, gpu_read(gpu, REG_A6XX_DBGC_CFG_DBGBUS_TRACE_BUF2));
	snapshot_sect_u32(sect, gpu_read(gpu, REG_A6XX_DBGC_CFG_DBGBUS_TRACE_BUF1));
}

static void gen8_snapshot_dbgc_debugbus_block(
	struct msm_gpu *gpu, uint32_t block_id, size_t count,
	struct adreno_snapshot_sect *sect)
{
	for (int i = 0; i < count; i++)
		gen8_dbgc_debug_bus_read(gpu, block_id, i, sect);
}

static void gen8_snapshot_debugbus(struct msm_gpu *gpu, struct adreno_gpu_state *state)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);

	/* Set up the GX debug bus */

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_CNTLT,
		A6XX_DBGC_CFG_DBGBUS_CNTLT_SEGT(0xf));

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_CNTLM,
		A6XX_DBGC_CFG_DBGBUS_CNTLM_ENABLE(0xf));

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_0, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_1, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_2, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_IVTL_3, 0);

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_BYTEL_0, 0x76543210);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_BYTEL_1, 0xFEDCBA98);

	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_0, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_1, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_2, 0);
	gpu_write(gpu, REG_A6XX_DBGC_CFG_DBGBUS_MASKL_3, 0);

	/* Read the blocks: */

	for (int i = 0; i < b->debugbus_blocks_len; i++) {
		size_t count = GEN8_DEBUGBUS_BLOCK_SIZE;
		uint32_t block_id = b->debugbus_blocks[i];

		snapshot_sect (state, DEBUGBUS, 4 * 2 * count, {
			/* debugbus section hdr: */
			.id = block_id,
			.count = count * 2,
		}) {
			gen8_snapshot_dbgc_debugbus_block(gpu, block_id, count, &sect);
		}

		snapshot_sect (state, SIDE_DEBUGBUS, 4 * count, {
			/* side_debugbus section hdr: */
			.id = block_id,
			.size = count,
			.valid_data = 0x4,
		}) {
			gen8_snapshot_dbgc_side_debugbus_block(gpu, block_id, count, &sect);
		}
	}

	for (int i = 0; i < b->gbif_debugbus_blocks_len; i++) {
		size_t count = GEN8_DEBUGBUS_BLOCK_SIZE;
		uint32_t block_id = b->gbif_debugbus_blocks[i];

		snapshot_sect (state, DEBUGBUS, 4 * 2 * count, {
			/* debugbus section hdr: */
			.id = block_id,
			.count = count * 2,
		}) {
			gen8_snapshot_dbgc_debugbus_block(gpu, block_id, count, &sect);
		}

		snapshot_sect (state, SIDE_DEBUGBUS, 4 * count, {
			/* side_debugbus section hdr: */
			.id = block_id,
			.size = count,
			.valid_data = 0x4,
		}) {
			gen8_snapshot_dbgc_side_debugbus_block(gpu, block_id, count, &sect);
		}
	}
}

#define cxdbg_write(gpu, offset, val) \
	gpu_write((gpu), (offset) + 0x18400, (val))

#define cxdbg_read(gpu, offset) \
	gpu_read((gpu), (offset) + 0x18400)

static void gen8_cx_dbgc_debugbus_select(struct msm_gpu *gpu, u32 block_id, u32 index)
{
	u32 reg;

	reg = A8XX_CX_DBGC_CFG_DBGBUS_SEL_A_PING_INDEX(index) |
		A8XX_CX_DBGC_CFG_DBGBUS_SEL_A_PING_BLK_SEL(block_id);

	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_A, reg);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_B, reg);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_C, reg);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_SEL_D, reg);

	/*
	 * There needs to be a delay of 1 us to ensure enough time for correct
	 * data is funneled into the trace buffer
	 */
	udelay(1);
}

static void gen8_debugbus_internal_read_setup(struct msm_gpu *gpu)
{
	/* Clear the DBGC logic/events */
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_CNTLM, 0x70000000);

	/* Enable the internal buffer mode and program to tail mode */
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_OPL, 0x28000000);

	/* Program the logic unit 0 to high when the side data is 1 */
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_BYTEL_0, 0x0000000c);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_0, 0x000000f0);

	/* Enable the DBGC logic/events */
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_CNTLM, 0x07000000);
}

static void gen8_cx_gc_us_i_0_debugbus_read(
	struct msm_gpu *gpu, u32 block_id, u32 index, u32 data[2])
{
	gen8_cx_dbgc_debugbus_select(gpu, block_id, index);

	/*
	 * Select lower 32 bits of internal buffer data for internal mux[1:0]
	 * DBGC_CFG_DBGBUS_IDX.internalIndex0 = 24 (0x18)
	 * DBGC_CFG_DBGBUS_IDX.internalIndex1 = 25 (0x19)
	 */
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_IDX, 0x00001918);
	udelay(1);

	data[0] = cxdbg_read(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_TRACE_BUF3);

	/*
	 * Select upper 32 bits of internal buffer data for internal mux[1:0]
	 * DBGC_CFG_DBGBUS_IDX.internalIndex0 = 26 (0x1a)
	 * DBGC_CFG_DBGBUS_IDX.internalIndex1 = 27 (0x1b)
	 */
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_IDX, 0x00001a1b);
	udelay(1);

	data[1] = cxdbg_read(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_TRACE_BUF3);
}

/* gen8_cx_dbgc_debug_bus_read() - Read data from trace bus */
static void gen8_cx_debug_bus_read(
	struct msm_gpu *gpu, u32 block_id, u32 index, u32 data[2])
{
	gen8_cx_dbgc_debugbus_select(gpu, block_id, index);

	data[0] = cxdbg_read(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_TRACE_BUF2);
	data[1] = cxdbg_read(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_TRACE_BUF1);
}

static void gen8_snapshot_cx_dbgc_debugbus_block(
	struct msm_gpu *gpu, uint32_t block_id, size_t count,
	struct adreno_snapshot_sect *sect)
{
	/* This block requires an out of order access due to a HW limitation.
	 * (QCTDD12240539)
	 */
	if (block_id == DEBUGBUS_CX_GC_US_I_0) {
		static u32 data[GEN8_DEBUGBUS_BLOCK_SIZE * 2];
		gen8_debugbus_internal_read_setup(gpu);
		for (int i = 0; i < GEN8_DEBUGBUS_BLOCK_IDX_1; i++)
			gen8_cx_gc_us_i_0_debugbus_read(gpu, block_id, i, &data[i * 2]);
		for (int i = GEN8_DEBUGBUS_BLOCK_IDX_2; i < GEN8_DEBUGBUS_BLOCK_SIZE; i++)
			gen8_cx_gc_us_i_0_debugbus_read(gpu, block_id, i, &data[i * 2]);
		for (int i = GEN8_DEBUGBUS_BLOCK_IDX_1; i < GEN8_DEBUGBUS_BLOCK_IDX_2; i++)
			gen8_cx_gc_us_i_0_debugbus_read(gpu, block_id, i, &data[i * 2]);
		snapshot_sect_arr(sect, data);
	} else {
		for (int i = 0; i < count; i++) {
			u32 data[2];
			gen8_cx_debug_bus_read(gpu, block_id, i, data);
			snapshot_sect_arr(sect, data);
		}
	}
}


/* gen8_cx_side_dbgc_debug_bus_read() - Read data from trace bus */
static void gen8_cx_side_debug_bus_read(
	struct msm_gpu *gpu, u32 block_id, u32 index, struct adreno_snapshot_sect *sect)
{
	u32 reg, side_data;

	gen8_cx_dbgc_debugbus_select(gpu, block_id, index);

	reg = cxdbg_read(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_OVER);
	side_data = FIELD_GET(GENMASK(27, 24), reg);

	snapshot_sect_u32(sect, side_data);

}

static void gen8_snapshot_cx_side_dbgc_debugbus_block(
	struct msm_gpu *gpu, uint32_t block_id, size_t count,
	struct adreno_snapshot_sect *sect)
{
	for (int i = 0; i < count; i++)
		gen8_cx_side_debug_bus_read(gpu, block_id, i, sect);
}

static void gen8_snapshot_cx_debugbus(struct msm_gpu *gpu, struct adreno_gpu_state *state)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);

	/* Set up the CX debug bus */

	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_CNTLT,
		A6XX_DBGC_CFG_DBGBUS_CNTLT_SEGT(0xf));

	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_CNTLM,
		A6XX_DBGC_CFG_DBGBUS_CNTLM_ENABLE(0xf));

	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_0, 0);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_1, 0);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_2, 0);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_IVTL_3, 0);

	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_BYTEL_0, 0x76543210);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_BYTEL_1, 0xFEDCBA98);

	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_0, 0);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_1, 0);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_2, 0);
	cxdbg_write(gpu, REG_A6XX_CX_DBGC_CFG_DBGBUS_MASKL_3, 0);

	/* Read the blocks: */

	for (int i = 0; i < b->cx_debugbus_blocks_len; i++) {
		size_t count = GEN8_DEBUGBUS_BLOCK_SIZE;
		uint32_t block_id = b->cx_debugbus_blocks[i];

		snapshot_sect (state, DEBUGBUS, 4 * 2 * count, {
			/* debugbus section hdr: */
			.id = block_id,
			.count = count * 2,
		}) {
			gen8_snapshot_cx_dbgc_debugbus_block(gpu, block_id, count, &sect);
		}

		snapshot_sect (state, SIDE_DEBUGBUS, 4 * count, {
			/* side_debugbus section hdr: */
			.id = block_id,
			.size = count,
			.valid_data = 0x4,
		}) {
			gen8_snapshot_cx_side_dbgc_debugbus_block(gpu, block_id, count, &sect);
		}
	}
}

struct msm_gpu_state *a8xx_gpu_state_get(struct msm_gpu *gpu)
{
	const struct gen8_snapshot_block_list *b = block_list(gpu);
	struct adreno_crashdumper _dumper = { 0 }, *dumper = NULL;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct adreno_gpu_state *state;

	/*
	 * GPU state snapshot is inherently single threaded.  All paths
	 * are serialized on gpu->lock.  Make that clear, and prevent
	 * future bugs:
	 */
	WARN_ON_ONCE(!mutex_is_locked(&gpu->lock));

	if (!b)
		return NULL;

	state = kzalloc_obj(*state);
	if (!state)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&state->objs);

	/* Get the generic state from the adreno core */
	adreno_gpu_state_get(gpu, &state->base);

	adreno_snapshot_registers_v2(gpu, state, b->gmu_cx_unsliced_regs);

	a8xx_snapshot_gmu_bo(state, &a6xx_gpu->gmu.log, SNAPSHOT_GMU_MEM_LOG);
	a8xx_snapshot_gmu_bo(state, &a6xx_gpu->gmu.hfi, SNAPSHOT_GMU_MEM_HFI);
	a8xx_snapshot_gmu_bo(state, &a6xx_gpu->gmu.debug, SNAPSHOT_GMU_MEM_DEBUG);

	a8xx_snapshot_gmu_hfi_history(gpu, state);

	snapshot_sect (state, DEBUG, 4, {
		.type = SNAPSHOT_DEBUG_SLICE_MASK,
		.size = 1,
	}) {
		snapshot_sect_u32(&sect, a6xx_gpu->slice_mask);
	}

	if (snapshot_debugbus)
		gen8_snapshot_cx_debugbus(gpu, state);

	/* If GX isn't on the rest of the data isn't going to be accessible */
	if (!adreno_gpu->funcs->gx_is_on(adreno_gpu))
		return &state->base;

	if (snapshot_debugbus)
		gen8_snapshot_debugbus(gpu, state);

	/*
	 * Try to initialize the crashdumper, if we are not dumping state
	 * with the SMMU stalled.  The crashdumper needs memory access to
	 * write out GPU state, so we need to skip this when the SMMU is
	 * stalled in response to an iova fault
	 */
	bool stalled = !!(gpu_read(gpu, REG_A8XX_RBBM_MISC_STATUS) &
			A8XX_RBBM_MISC_STATUS_SMMU_STALLED_ON_FAULT);
	if (!stalled && !gpu->needs_hw_init &&
	    !a8xx_crashdumper_init(gpu, &_dumper)) {
		dumper = &_dumper;
	}

	/* Clear aperture register */
	a8xx_aperture_clear(gpu);

	/*
	 * Assert the isStatic bit before triggering snapshot.
	 * BIT(0): GPU activity during snapshot dump
	 * BIT(1): GPU UNSLICE activity during snapshot dump
	 * Similarly, BIT(4) for slice-0, BIT(5) for slice-1 and so on.
	 */
	gpu_write(gpu, REG_A8XX_RBBM_SNAPSHOT_STATUS, BIT(0) | BIT(1) |
			FIELD_PREP(GENMASK(7, 4), a6xx_gpu->slice_mask));

	/* Dump the registers which get affected by crash dumper trigger */
	for (int i = 0; i < b->num_pre_crashdumper_regs; i++) {
		const struct gen8_reg_list *regs = &b->pre_crashdumper_regs[i];
		u32 j;

		FOR_EACH_SLICE (j, regs->slice_region, a6xx_gpu->slice_mask) {
			u32 slice_id = SLICE_ID(regs->slice_region, j);
			gen8_legacy_snapshot_registers(gpu, state, regs, slice_id);
		}
	}

	/*
	 * If crashdumper timed out while dumping this section skip everything
	 * since even AHB acceses to the GPU might cause NoC errors.
	 */
	if (!gen8_reglist_snapshot(gpu, state, dumper))
		goto out;

	/* CP indexed regs data */
	if (!gen8_snapshot_cp_indexed_regs(gpu, state, dumper))
		goto out;

	/* Mempool debug data */
	if (!gen8_snapshot_mempool(gpu, state, dumper))
		goto out;

	/*
	 * CP MVC register section
	 * If crashdumper timed out while dumping any section below skip everything
	 * since even AHB acceses to the GPU might cause NoC errors.
	 */
	if (!gen8_snapshot_mvc_regs(gpu, state, dumper, b->cp_clusters, b->num_cp_clusters))
		goto out;

	/* MVC register section */
	if (!gen8_snapshot_mvc_regs(gpu, state, dumper, b->clusters, b->num_clusters))
		goto out;

	/* registers dumped through DBG AHB */
	if (!gen8_snapshot_dbgahb_regs(gpu, state, dumper))
		goto out;

	/* Shader memory */
	if (!gen8_snapshot_shader(gpu, state, dumper))
		goto out;

	/*
	 * Capture preempt records after cp_clusters, to make this easier
	 * to decode (ie. so we already know CP_CONTEXT_SWITCH_SMMU_INFO and
	 * CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR) when crashdec sees
	 * the logged GPU objs)
	 */
	if (gpu->nr_rings > 1) {
		for (unsigned i = 0; i < gpu->nr_rings; i++) {
			a8xx_snapshot_gpu_bo(state, a6xx_gpu->preempt_bo[i],
					     a6xx_gpu->preempt_iova[i]);
			a8xx_snapshot_gpu_bo(state, a6xx_gpu->preempt_smmu_bo[i],
					     a6xx_gpu->preempt_smmu_iova[i]);
		}
	}

out:
	gpu_write(gpu, REG_A8XX_RBBM_SNAPSHOT_STATUS, 0x0);

	/* Clear aperture register */
	a8xx_aperture_clear(gpu);

	if (dumper)
		msm_gem_kernel_put(dumper->bo, gpu->vm);

	state->gpu_initialized = !gpu->needs_hw_init;

	return  &state->base;
}

static void a8xx_gpu_state_destroy(struct kref *kref)
{
	struct adreno_state_memobj *obj, *tmp;
	struct msm_gpu_state *state = container_of(kref,
			struct msm_gpu_state, ref);
	struct adreno_gpu_state *adreno_state = container_of(state,
			struct adreno_gpu_state, base);

	list_for_each_entry_safe (obj, tmp, &adreno_state->objs, node) {
		list_del(&obj->node);
		kvfree(obj->ptr);
		kfree(obj);
	}

	adreno_gpu_state_destroy(state);
	kfree(adreno_state);
}

int a8xx_gpu_state_put(struct msm_gpu_state *state)
{
	if (IS_ERR_OR_NULL(state))
		return 1;

	return kref_put(&state->ref, a8xx_gpu_state_destroy);
}

void a8xx_show(struct msm_gpu *gpu, struct msm_gpu_state *state,
		struct drm_printer *p)
{
	struct adreno_gpu_state *adreno_state = container_of(state,
			struct adreno_gpu_state, base);
	struct adreno_state_memobj *obj;

	if (IS_ERR_OR_NULL(state))
		return;

	drm_printf(p, "gpu-initialized: %d\n", adreno_state->gpu_initialized);

	adreno_show(gpu, state, p);

	drm_puts(p, "gmu-hfi:\n");
	for (int i = 0; i < ARRAY_SIZE(adreno_state->hfi_queue_history); i++) {
		drm_printf(p, "    queue-history[%u]:", i);
		for (int j = 0; j < HFI_HISTORY_SZ; j++) {
			drm_printf(p, " %d", adreno_state->hfi_queue_history[i][j]);
		}
		drm_printf(p, "\n");
	}

	/*
	 * Parsing note: do not assume each snapshot obj is only a single
	 * section, assume sub-allocation.
	 */
	drm_puts(p, "snapshot:\n");
	list_for_each_entry (obj, &adreno_state->objs, node) {
		drm_printf(p, "    size: %zu\n", obj->size);
		adreno_show_object(p, &obj->ptr, obj->size, &obj->encoded);
	}
}
