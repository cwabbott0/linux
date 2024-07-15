// TODO those defines definitely don't belong here
#define GEN7_GMU_PWR_COL_PREEMPT_KEEPALIVE	0x1f8c4

#include "msm_gem.h"
#include "a6xx_gpu.h"
#include "a6xx_gmu.xml.h"
#include "msm_mmu.h"
#include "msm_gpu_trace.h"

#define FENCE_STATUS_WRITEDROPPED0_MASK 0x1
#define FENCE_STATUS_WRITEDROPPED1_MASK 0x2

/*
 * Try to transition the preemption state from old to new. Return
 * true on success or false if the original state wasn't 'old'
 */
static inline bool try_preempt_state(struct a6xx_gpu *a6xx_gpu,
		enum a6xx_preempt_state old, enum a6xx_preempt_state new)
{
	enum a6xx_preempt_state cur = atomic_cmpxchg(&a6xx_gpu->preempt_state,
		old, new);

	return (cur == old);
}

/*
 * Force the preemption state to the specified state.  This is used in cases
 * where the current state is known and won't change
 */
static inline void set_preempt_state(struct a6xx_gpu *gpu,
		enum a6xx_preempt_state new)
{
	/*
	 * preempt_state may be read by other cores trying to trigger a
	 * preemption or in the interrupt handler so barriers are needed
	 * before...
	 */
	smp_mb__before_atomic();
	atomic_set(&gpu->preempt_state, new);
	/* ... and after*/
	smp_mb__after_atomic();
}

/* Write the most recent wptr for the given ring into the hardware */
static inline void update_wptr(struct msm_gpu *gpu, struct msm_ringbuffer *ring)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	unsigned long flags;
	uint32_t wptr;

	if (!ring)
		return;

	spin_lock_irqsave(&ring->preempt_lock, flags);
	wptr = get_wptr(ring);
	spin_unlock_irqrestore(&ring->preempt_lock, flags);

	/* Make sure everything is posted before making a decision */
	mb();

	gpu_write(gpu, REG_A6XX_CP_RB_WPTR, wptr);
	/* a6xx_gmu_fenced_write(a6xx_gpu, */
	/* 	REG_A6XX_CP_RB_WPTR, */
	/* 	wptr, */
	/* 	FENCE_STATUS_WRITEDROPPED1_MASK); */
}

static void _power_collapse_set(struct a6xx_gpu *a6xx_gpu, bool val)
{
	/* gmu_write(&a6xx_gpu->gmu, REG_A6XX_GMU_GMU_PWR_COL_KEEPALIVE, (val ? 1 : 0)); */
	return;
	gmu_write(&a6xx_gpu->gmu,
              GEN7_GMU_PWR_COL_PREEMPT_KEEPALIVE, (val ? 1 : 0));
}

/* Return the highest priority ringbuffer with something in it */
static struct msm_ringbuffer *get_next_ring(struct msm_gpu *gpu)
{
	unsigned long flags;
	int i;

	for (i = 0; i < gpu->nr_rings; i++) {
		bool empty;
		struct msm_ringbuffer *ring = gpu->rb[i];

		spin_lock_irqsave(&ring->preempt_lock, flags);
		empty = (get_wptr(ring) == gpu->funcs->get_rptr(gpu, ring));
		spin_unlock_irqrestore(&ring->preempt_lock, flags);

		if (!empty)
			return ring;
	}

	return NULL;
}

static void a6xx_preempt_timer(struct timer_list *t)
{
	struct a6xx_gpu *a6xx_gpu = from_timer(a6xx_gpu, t, preempt_timer);
	struct msm_gpu *gpu = &a6xx_gpu->base.base;
	struct drm_device *dev = gpu->dev;

	if (!try_preempt_state(a6xx_gpu, PREEMPT_TRIGGERED, PREEMPT_FAULTED))
		return;

	dev_err(dev->dev, "%s: preemption timed out\n", gpu->name);
	kthread_queue_work(gpu->worker, &gpu->recover_work);
}

void a6xx_preempt_irq(struct msm_gpu *gpu)
{
	uint32_t status;
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	struct drm_device *dev = gpu->dev;
	struct msm_drm_private *priv = dev->dev_private;

	if (!try_preempt_state(a6xx_gpu, PREEMPT_TRIGGERED, PREEMPT_PENDING))
		return;

	/* Delete the preemption watchdog timer */
	del_timer(&a6xx_gpu->preempt_timer);

	/*
	 * The hardware should be setting the stop bit of CP_CONTEXT_SWITCH_CNTL
	 * to zero before firing the interrupt, but there is a non zero chance
	 * of a hardware condition or a software race that could set it again
	 * before we have a chance to finish. If that happens, log and go for
	 * recovery
	 */
	status = gpu_read(gpu, REG_A6XX_CP_CONTEXT_SWITCH_CNTL);
	if (unlikely(status & 0x1)) {
		DRM_DEV_ERROR(&gpu->pdev->dev,
					  "!!!!!!!!!!!!!!!! preemption faulted !!!!!!!!!!!!!! irq\n");
		set_preempt_state(a6xx_gpu, PREEMPT_FAULTED);
		dev_err(dev->dev, "%s: Preemption failed to complete\n",
			gpu->name);
        kthread_queue_work(gpu->worker, &gpu->recover_work);
		return;
	}

	/* DRM_DEV_ERROR(&gpu->pdev->dev, */
	/* 			  "!!!!!!!!!!!!!!!! premption irq !!!!!!!!!!!!!! new ring %i %i %i/%i\n", */
	/* 			  a6xx_gpu->cur_ring->id, */
	/* 			  a6xx_gpu->next_ring->id, */
	/* 			  gpu_read(gpu, REG_A6XX_CP_RB_RPTR), */
	/* 			  gpu_read(gpu, REG_A6XX_CP_RB_WPTR)); */

	a6xx_gpu->cur_ring = a6xx_gpu->next_ring;
	a6xx_gpu->next_ring = NULL;

	update_wptr(gpu, a6xx_gpu->cur_ring);

	trace_msm_gpu_preemption_irq(a6xx_gpu->cur_ring->id);
	set_preempt_state(a6xx_gpu, PREEMPT_NONE);
}

void a6xx_preempt_hw_init(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i;

	/* No preemption if we only have one ring */
	if (gpu->nr_rings == 1)
		return;

	for (i = 0; i < gpu->nr_rings; i++) {
		struct a6xx_preempt_record *record_ptr = a6xx_gpu->preempt[i] + PREEMPT_OFFSET_PRIV_NON_SECURE;
		record_ptr->wptr = 0;
		record_ptr->rptr = 0;
		record_ptr->info = 0;
		record_ptr->data = 0;
		record_ptr->rbase = gpu->rb[i]->iova;
	}

	/* Write a 0 to signal that we aren't switching pagetables */
	gpu_write64(gpu, REG_A6XX_CP_CONTEXT_SWITCH_SMMU_INFO, 0);

	/* Enable the GMEM save/restore feature for preemption */
	gpu_write(gpu, REG_A6XX_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE, 0x1);

	/* Reset the preemption state */
	set_preempt_state(a6xx_gpu, PREEMPT_NONE);

	/* Always come up on rb 0 */
	a6xx_gpu->cur_ring = gpu->rb[0];
}

void a6xx_preempt_trigger(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	unsigned long flags;
	struct msm_ringbuffer *ring;
	uint64_t user_ctx_iova;
	unsigned int cntl;

	if (gpu->nr_rings == 1)
		return;

	/*
	 * Try to start preemption by moving from NONE to START. If
	 * unsuccessful, a preemption is already in flight
	 */
	if (!try_preempt_state(a6xx_gpu, PREEMPT_NONE, PREEMPT_START))
		return;

	cntl = (((a6xx_gpu->preempt_level << 6) & 0xC0) |
		((a6xx_gpu->skip_save_restore << 9) & 0x200) |
		((a6xx_gpu->uses_gmem << 8) & 0x100) | 0x1);

	/* Get the next ring to preempt to */
	ring = get_next_ring(gpu);

	/*
	 * If no ring is populated or the highest priority ring is the current
	 * one do nothing except to update the wptr to the latest and greatest
	 */
	if (!ring || (a6xx_gpu->cur_ring == ring)) {
		set_preempt_state(a6xx_gpu, PREEMPT_ABORT);
		update_wptr(gpu, a6xx_gpu->cur_ring);
		set_preempt_state(a6xx_gpu, PREEMPT_NONE);
		return;
	}

	trace_msm_gpu_preemption_trigger(a6xx_gpu->cur_ring ? a6xx_gpu->cur_ring->id: 1000, ring ? ring->id: 1000);

	spin_lock_irqsave(&ring->preempt_lock, flags);

	struct gen7_cp_smmu_info *smmu_info_ptr = a6xx_gpu->preempt[ring->id] + PREEMPT_OFFSET_SMMU_INFO;
	struct a6xx_preempt_record *record_ptr = a6xx_gpu->preempt[ring->id] + PREEMPT_OFFSET_PRIV_NON_SECURE;
	u64 ttbr0 = ring->memptrs->ttbr0;
	u32 context_idr = ring->memptrs->context_idr;

	mb();

	smmu_info_ptr->ttbr0 = ttbr0;
	smmu_info_ptr->context_idr = context_idr;
	record_ptr->wptr = get_wptr(ring);
	mb();

	spin_unlock_irqrestore(&ring->preempt_lock, flags);

	/*
	 * The GPU power collapsing between the following preemption register
	 * writes can lead to a prolonged preemption trigger sequence, so we
	 * set a keepalive bit to make sure the GPU is not power collapsed by
	 * the GMU during this time. The first fenced write will make sure to
	 * wake up the GPU(if it was power collapsed) and from there on it is
	 * not going to be power collapsed until we close the keepalive window
	 * by resetting the keepalive bit.
	 */
	//a6xx gmu_rmw(&a6xx_gpu->gmu, REG_A6XX_GMU_AO_SPARE_CNTL, 0x0, 0x2);
	_power_collapse_set(a6xx_gpu, true);


	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_SMMU_INFO,
		lower_32_bits(a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_SMMU_INFO),
		FENCE_STATUS_WRITEDROPPED1_MASK);
	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_SMMU_INFO + 1,
		upper_32_bits(a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_SMMU_INFO),
		FENCE_STATUS_WRITEDROPPED1_MASK);

	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR,
		lower_32_bits(a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_PRIV_NON_SECURE),
		FENCE_STATUS_WRITEDROPPED1_MASK);
	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR + 1,
		upper_32_bits(a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_PRIV_NON_SECURE),
		FENCE_STATUS_WRITEDROPPED1_MASK);

	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_PRIV_SECURE_RESTORE_ADDR,
		lower_32_bits(a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_PRIV_SECURE),
		FENCE_STATUS_WRITEDROPPED1_MASK);
	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_PRIV_SECURE_RESTORE_ADDR + 1,
		upper_32_bits(a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_PRIV_SECURE),
		FENCE_STATUS_WRITEDROPPED1_MASK);

	/*
	 * Use the user context iova from the scratch memory that the CP may
	 * have written as part of the ring switch out.
	 */
	user_ctx_iova = *((uint64_t *)a6xx_gpu->scratch_ptr + ring->id);
	//user_ctx_iova = a6xx_gpu->preempt_iova[ring->id] + PREEMPT_OFFSET_NON_PRIV;

	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_NON_PRIV_RESTORE_ADDR,
		lower_32_bits(user_ctx_iova),
		FENCE_STATUS_WRITEDROPPED1_MASK);

	a6xx_gmu_fenced_write(a6xx_gpu,
		REG_A6XX_CP_CONTEXT_SWITCH_NON_PRIV_RESTORE_ADDR + 1,
		upper_32_bits(user_ctx_iova),
		FENCE_STATUS_WRITEDROPPED1_MASK);

	a6xx_gpu->next_ring = ring;

	/* Start a timer to catch a stuck preemption */
	mod_timer(&a6xx_gpu->preempt_timer, jiffies + msecs_to_jiffies(10000));

	/* Set the preemption state to triggered */
	set_preempt_state(a6xx_gpu, PREEMPT_TRIGGERED);

	/* Make sure everything is written before hitting the button */
	wmb();

	/* Trigger the preemption */
	a6xx_gmu_fenced_write(a6xx_gpu, REG_A6XX_CP_CONTEXT_SWITCH_CNTL, cntl,
		FENCE_STATUS_WRITEDROPPED1_MASK);

	/* Close the GPU keelaplive window */
	//gmu_rmw(&a6xx_gpu->gmu, REG_A6XX_GMU_AO_SPARE_CNTL, 0x2, 0x0);
	_power_collapse_set(a6xx_gpu, false);
}

static int preempt_init_ring(struct a6xx_gpu *a6xx_gpu,
		struct msm_ringbuffer *ring)
{
	struct adreno_gpu *adreno_gpu = &a6xx_gpu->base;
	struct msm_gpu *gpu = &adreno_gpu->base;
	void *ptr;
	struct drm_gem_object *bo = NULL;
	u64 iova = 0;

	ptr = msm_gem_kernel_new(gpu->dev,
		/*A6XX_PREEMPT_RECORD_SIZE + A6XX_PREEMPT_COUNTER_SIZE*/PREEMPT_SIZE,
		MSM_BO_WC/* | MSM_BO_MAP_PRIV*/, gpu->aspace, &bo, &iova);

	memset(ptr, 0, PREEMPT_SIZE);

	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	a6xx_gpu->preempt_bo[ring->id] = bo;
	a6xx_gpu->preempt_iova[ring->id] = iova;
	a6xx_gpu->preempt[ring->id] = ptr;

	struct gen7_cp_smmu_info *smmu_info_ptr = ptr + PREEMPT_OFFSET_SMMU_INFO;
	struct a6xx_preempt_record *record_ptr = ptr + PREEMPT_OFFSET_PRIV_NON_SECURE;

	phys_addr_t ttbr;
	int asid;
	msm_iommu_pagetable_params(gpu->aspace->mmu, &ttbr, &asid);

	smmu_info_ptr->magic = GEN7_CP_SMMU_INFO_MAGIC;
	smmu_info_ptr->ttbr0 = ttbr;
	smmu_info_ptr->asid = 0xdecafbad;
	smmu_info_ptr->context_idr = 0;

	DRM_DEV_ERROR(&gpu->pdev->dev,
				  "!!!!!!!!!!!!!!!! premption !!!!!!!!!!!!!! setting record vals\n");
	/* Set up the defaults on the preemption record */
	record_ptr->magic = A6XX_PREEMPT_RECORD_MAGIC;
	record_ptr->info = 0;
	record_ptr->data = 0;
	record_ptr->rptr = gpu->funcs->get_rptr(gpu, ring);
	record_ptr->wptr = 0;
	record_ptr->cntl = MSM_GPU_RB_CNTL_DEFAULT;
	record_ptr->rptr_addr = rbmemptr(ring, rptr);
	record_ptr->rbase = ring->iova;
	record_ptr->counter = 0;//iova + A6XX_PREEMPT_RECORD_SIZE;
	record_ptr->bv_rptr_addr = rbmemptr(ring, bv_fence);
	DRM_DEV_ERROR(&gpu->pdev->dev,
				  "ring %i base %llx\n", ring->id, ring->iova);
	DRM_DEV_ERROR(&gpu->pdev->dev,
				  "!!!!!!!!!!!!!!!! premption !!!!!!!!!!!!!! DONE setting record vals\n");

	return 0;
}

void a6xx_preempt_fini(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i;

	for (i = 0; i < gpu->nr_rings; i++)
		msm_gem_kernel_put(a6xx_gpu->preempt_bo[i], gpu->aspace);
}

void a6xx_preempt_init(struct msm_gpu *gpu)
{
	struct adreno_gpu *adreno_gpu = to_adreno_gpu(gpu);
	struct a6xx_gpu *a6xx_gpu = to_a6xx_gpu(adreno_gpu);
	int i;

	/* No preemption if we only have one ring */
	if (gpu->nr_rings <= 1)
		return;

	for (i = 0; i < gpu->nr_rings; i++) {
		if (preempt_init_ring(a6xx_gpu, gpu->rb[i]))
			goto fail;
	}

	/* TODO: make this configurable? */
	a6xx_gpu->preempt_level = 1;
	a6xx_gpu->uses_gmem = 1;
	a6xx_gpu->skip_save_restore = 0;

	a6xx_gpu->scratch_ptr  = msm_gem_kernel_new(gpu->dev,
			gpu->nr_rings * sizeof(uint64_t), MSM_BO_WC,
			gpu->aspace, &a6xx_gpu->scratch_bo,
			&a6xx_gpu->scratch_iova);

	if (IS_ERR(a6xx_gpu->scratch_ptr))
		goto fail;

	timer_setup(&a6xx_gpu->preempt_timer, a6xx_preempt_timer,
			(unsigned long) a6xx_gpu);

	return;
fail:
	/*
	 * On any failure our adventure is over. Clean up and
	 * set nr_rings to 1 to force preemption off
	 */
	a6xx_preempt_fini(gpu);
	gpu->nr_rings = 1;

	return;
}

void a6xx_preempt_submitqueue_close(struct msm_gpu *gpu,
		struct msm_gpu_submitqueue *queue)
{
	if (!queue->bo)
		return;

	msm_gem_kernel_put(queue->bo, gpu->aspace);
}

int a6xx_preempt_submitqueue_setup(struct msm_gpu *gpu,
		struct msm_gpu_submitqueue *queue)
{
	void *ptr;

	/*
	 * Create a per submitqueue buffer for the CP to save and restore user
	 * specific information such as the VPC streamout data.
	 */
	ptr = msm_gem_kernel_new(gpu->dev, A6XX_PREEMPT_USER_RECORD_SIZE,
			MSM_BO_WC, gpu->aspace, &queue->bo, &queue->bo_iova);

	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	return 0;
}
