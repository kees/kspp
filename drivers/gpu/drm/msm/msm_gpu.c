/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "msm_gpu.h"
#include "msm_gem.h"
#include "msm_mmu.h"
#include "msm_fence.h"


/*
 * Power Management:
 */

#ifdef DOWNSTREAM_CONFIG_MSM_BUS_SCALING
#include <mach/board.h>
static void bs_init(struct msm_gpu *gpu)
{
	if (gpu->bus_scale_table) {
		gpu->bsc = msm_bus_scale_register_client(gpu->bus_scale_table);
		DBG("bus scale client: %08x", gpu->bsc);
	}
}

static void bs_fini(struct msm_gpu *gpu)
{
	if (gpu->bsc) {
		msm_bus_scale_unregister_client(gpu->bsc);
		gpu->bsc = 0;
	}
}

static void bs_set(struct msm_gpu *gpu, int idx)
{
	if (gpu->bsc) {
		DBG("set bus scaling: %d", idx);
		msm_bus_scale_client_update_request(gpu->bsc, idx);
	}
}
#else
static void bs_init(struct msm_gpu *gpu) {}
static void bs_fini(struct msm_gpu *gpu) {}
static void bs_set(struct msm_gpu *gpu, int idx) {}
#endif

static int enable_pwrrail(struct msm_gpu *gpu)
{
	struct drm_device *dev = gpu->dev;
	int ret = 0;

	if (gpu->gpu_reg) {
		ret = regulator_enable(gpu->gpu_reg);
		if (ret) {
			dev_err(dev->dev, "failed to enable 'gpu_reg': %d\n", ret);
			return ret;
		}
	}

	if (gpu->gpu_cx) {
		ret = regulator_enable(gpu->gpu_cx);
		if (ret) {
			dev_err(dev->dev, "failed to enable 'gpu_cx': %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int disable_pwrrail(struct msm_gpu *gpu)
{
	if (gpu->gpu_cx)
		regulator_disable(gpu->gpu_cx);
	if (gpu->gpu_reg)
		regulator_disable(gpu->gpu_reg);
	return 0;
}

static int enable_clk(struct msm_gpu *gpu)
{
	int i;

	if (gpu->grp_clks[0] && gpu->fast_rate)
		clk_set_rate(gpu->grp_clks[0], gpu->fast_rate);

	/* Set the RBBM timer rate to 19.2Mhz */
	if (gpu->grp_clks[2])
		clk_set_rate(gpu->grp_clks[2], 19200000);

	for (i = ARRAY_SIZE(gpu->grp_clks) - 1; i >= 0; i--)
		if (gpu->grp_clks[i])
			clk_prepare(gpu->grp_clks[i]);

	for (i = ARRAY_SIZE(gpu->grp_clks) - 1; i >= 0; i--)
		if (gpu->grp_clks[i])
			clk_enable(gpu->grp_clks[i]);

	return 0;
}

static int disable_clk(struct msm_gpu *gpu)
{
	int i;

	for (i = ARRAY_SIZE(gpu->grp_clks) - 1; i >= 0; i--)
		if (gpu->grp_clks[i])
			clk_disable(gpu->grp_clks[i]);

	for (i = ARRAY_SIZE(gpu->grp_clks) - 1; i >= 0; i--)
		if (gpu->grp_clks[i])
			clk_unprepare(gpu->grp_clks[i]);

	if (gpu->grp_clks[0] && gpu->slow_rate)
		clk_set_rate(gpu->grp_clks[0], gpu->slow_rate);

	if (gpu->grp_clks[2])
		clk_set_rate(gpu->grp_clks[2], 0);

	return 0;
}

static int enable_axi(struct msm_gpu *gpu)
{
	if (gpu->ebi1_clk)
		clk_prepare_enable(gpu->ebi1_clk);
	if (gpu->bus_freq)
		bs_set(gpu, gpu->bus_freq);
	return 0;
}

static int disable_axi(struct msm_gpu *gpu)
{
	if (gpu->ebi1_clk)
		clk_disable_unprepare(gpu->ebi1_clk);
	if (gpu->bus_freq)
		bs_set(gpu, 0);
	return 0;
}

int msm_gpu_pm_resume(struct msm_gpu *gpu)
{
	int ret;

	DBG("%s", gpu->name);

	ret = enable_pwrrail(gpu);
	if (ret)
		return ret;

	ret = enable_clk(gpu);
	if (ret)
		return ret;

	ret = enable_axi(gpu);
	if (ret)
		return ret;

	gpu->needs_hw_init = true;

	return 0;
}

int msm_gpu_pm_suspend(struct msm_gpu *gpu)
{
	int ret;

	DBG("%s", gpu->name);

	ret = disable_axi(gpu);
	if (ret)
		return ret;

	ret = disable_clk(gpu);
	if (ret)
		return ret;

	ret = disable_pwrrail(gpu);
	if (ret)
		return ret;

	return 0;
}

int msm_gpu_hw_init(struct msm_gpu *gpu)
{
	int ret;

	if (!gpu->needs_hw_init)
		return 0;

	disable_irq(gpu->irq);
	ret = gpu->funcs->hw_init(gpu);
	if (!ret)
		gpu->needs_hw_init = false;
	enable_irq(gpu->irq);

	return ret;
}

/*
 * Hangcheck detection for locked gpu:
 */

static void retire_submits(struct msm_gpu *gpu);

static void recover_worker(struct work_struct *work)
{
	struct msm_gpu *gpu = container_of(work, struct msm_gpu, recover_work);
	struct drm_device *dev = gpu->dev;
	struct msm_gem_submit *submit;
	uint32_t fence = gpu->funcs->last_fence(gpu);

	msm_update_fence(gpu->fctx, fence + 1);

	mutex_lock(&dev->struct_mutex);

	dev_err(dev->dev, "%s: hangcheck recover!\n", gpu->name);
	list_for_each_entry(submit, &gpu->submit_list, node) {
		if (submit->fence->seqno == (fence + 1)) {
			struct task_struct *task;

			rcu_read_lock();
			task = pid_task(submit->pid, PIDTYPE_PID);
			if (task) {
				dev_err(dev->dev, "%s: offending task: %s\n",
						gpu->name, task->comm);
			}
			rcu_read_unlock();
			break;
		}
	}

	if (msm_gpu_active(gpu)) {
		/* retire completed submits, plus the one that hung: */
		retire_submits(gpu);

		pm_runtime_get_sync(&gpu->pdev->dev);
		gpu->funcs->recover(gpu);
		pm_runtime_put_sync(&gpu->pdev->dev);

		/* replay the remaining submits after the one that hung: */
		list_for_each_entry(submit, &gpu->submit_list, node) {
			gpu->funcs->submit(gpu, submit, NULL);
		}
	}

	mutex_unlock(&dev->struct_mutex);

	msm_gpu_retire(gpu);
}

static void hangcheck_timer_reset(struct msm_gpu *gpu)
{
	DBG("%s", gpu->name);
	mod_timer(&gpu->hangcheck_timer,
			round_jiffies_up(jiffies + DRM_MSM_HANGCHECK_JIFFIES));
}

static void hangcheck_handler(unsigned long data)
{
	struct msm_gpu *gpu = (struct msm_gpu *)data;
	struct drm_device *dev = gpu->dev;
	struct msm_drm_private *priv = dev->dev_private;
	uint32_t fence = gpu->funcs->last_fence(gpu);

	if (fence != gpu->hangcheck_fence) {
		/* some progress has been made.. ya! */
		gpu->hangcheck_fence = fence;
	} else if (fence < gpu->fctx->last_fence) {
		/* no progress and not done.. hung! */
		gpu->hangcheck_fence = fence;
		dev_err(dev->dev, "%s: hangcheck detected gpu lockup!\n",
				gpu->name);
		dev_err(dev->dev, "%s:     completed fence: %u\n",
				gpu->name, fence);
		dev_err(dev->dev, "%s:     submitted fence: %u\n",
				gpu->name, gpu->fctx->last_fence);
		queue_work(priv->wq, &gpu->recover_work);
	}

	/* if still more pending work, reset the hangcheck timer: */
	if (gpu->fctx->last_fence > gpu->hangcheck_fence)
		hangcheck_timer_reset(gpu);

	/* workaround for missing irq: */
	queue_work(priv->wq, &gpu->retire_work);
}

/*
 * Performance Counters:
 */

/* called under perf_lock */
static int update_hw_cntrs(struct msm_gpu *gpu, uint32_t ncntrs, uint32_t *cntrs)
{
	uint32_t current_cntrs[ARRAY_SIZE(gpu->last_cntrs)];
	int i, n = min(ncntrs, gpu->num_perfcntrs);

	/* read current values: */
	for (i = 0; i < gpu->num_perfcntrs; i++)
		current_cntrs[i] = gpu_read(gpu, gpu->perfcntrs[i].sample_reg);

	/* update cntrs: */
	for (i = 0; i < n; i++)
		cntrs[i] = current_cntrs[i] - gpu->last_cntrs[i];

	/* save current values: */
	for (i = 0; i < gpu->num_perfcntrs; i++)
		gpu->last_cntrs[i] = current_cntrs[i];

	return n;
}

static void update_sw_cntrs(struct msm_gpu *gpu)
{
	ktime_t time;
	uint32_t elapsed;
	unsigned long flags;

	spin_lock_irqsave(&gpu->perf_lock, flags);
	if (!gpu->perfcntr_active)
		goto out;

	time = ktime_get();
	elapsed = ktime_to_us(ktime_sub(time, gpu->last_sample.time));

	gpu->totaltime += elapsed;
	if (gpu->last_sample.active)
		gpu->activetime += elapsed;

	gpu->last_sample.active = msm_gpu_active(gpu);
	gpu->last_sample.time = time;

out:
	spin_unlock_irqrestore(&gpu->perf_lock, flags);
}

void msm_gpu_perfcntr_start(struct msm_gpu *gpu)
{
	unsigned long flags;

	pm_runtime_get_sync(&gpu->pdev->dev);

	spin_lock_irqsave(&gpu->perf_lock, flags);
	/* we could dynamically enable/disable perfcntr registers too.. */
	gpu->last_sample.active = msm_gpu_active(gpu);
	gpu->last_sample.time = ktime_get();
	gpu->activetime = gpu->totaltime = 0;
	gpu->perfcntr_active = true;
	update_hw_cntrs(gpu, 0, NULL);
	spin_unlock_irqrestore(&gpu->perf_lock, flags);
}

void msm_gpu_perfcntr_stop(struct msm_gpu *gpu)
{
	gpu->perfcntr_active = false;
	pm_runtime_put_sync(&gpu->pdev->dev);
}

/* returns -errno or # of cntrs sampled */
int msm_gpu_perfcntr_sample(struct msm_gpu *gpu, uint32_t *activetime,
		uint32_t *totaltime, uint32_t ncntrs, uint32_t *cntrs)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&gpu->perf_lock, flags);

	if (!gpu->perfcntr_active) {
		ret = -EINVAL;
		goto out;
	}

	*activetime = gpu->activetime;
	*totaltime = gpu->totaltime;

	gpu->activetime = gpu->totaltime = 0;

	ret = update_hw_cntrs(gpu, ncntrs, cntrs);

out:
	spin_unlock_irqrestore(&gpu->perf_lock, flags);

	return ret;
}

/*
 * Cmdstream submission/retirement:
 */

static void retire_submit(struct msm_gpu *gpu, struct msm_gem_submit *submit)
{
	int i;

	for (i = 0; i < submit->nr_bos; i++) {
		struct msm_gem_object *msm_obj = submit->bos[i].obj;
		/* move to inactive: */
		msm_gem_move_to_inactive(&msm_obj->base);
		msm_gem_put_iova(&msm_obj->base, gpu->id);
		drm_gem_object_unreference(&msm_obj->base);
	}

	pm_runtime_mark_last_busy(&gpu->pdev->dev);
	pm_runtime_put_autosuspend(&gpu->pdev->dev);
	msm_gem_submit_free(submit);
}

static void retire_submits(struct msm_gpu *gpu)
{
	struct drm_device *dev = gpu->dev;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	while (!list_empty(&gpu->submit_list)) {
		struct msm_gem_submit *submit;

		submit = list_first_entry(&gpu->submit_list,
				struct msm_gem_submit, node);

		if (dma_fence_is_signaled(submit->fence)) {
			retire_submit(gpu, submit);
		} else {
			break;
		}
	}
}

static void retire_worker(struct work_struct *work)
{
	struct msm_gpu *gpu = container_of(work, struct msm_gpu, retire_work);
	struct drm_device *dev = gpu->dev;
	uint32_t fence = gpu->funcs->last_fence(gpu);

	msm_update_fence(gpu->fctx, fence);

	mutex_lock(&dev->struct_mutex);
	retire_submits(gpu);
	mutex_unlock(&dev->struct_mutex);
}

/* call from irq handler to schedule work to retire bo's */
void msm_gpu_retire(struct msm_gpu *gpu)
{
	struct msm_drm_private *priv = gpu->dev->dev_private;
	queue_work(priv->wq, &gpu->retire_work);
	update_sw_cntrs(gpu);
}

/* add bo's to gpu's ring, and kick gpu: */
void msm_gpu_submit(struct msm_gpu *gpu, struct msm_gem_submit *submit,
		struct msm_file_private *ctx)
{
	struct drm_device *dev = gpu->dev;
	struct msm_drm_private *priv = dev->dev_private;
	int i;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	pm_runtime_get_sync(&gpu->pdev->dev);

	msm_gpu_hw_init(gpu);

	list_add_tail(&submit->node, &gpu->submit_list);

	msm_rd_dump_submit(submit);

	update_sw_cntrs(gpu);

	for (i = 0; i < submit->nr_bos; i++) {
		struct msm_gem_object *msm_obj = submit->bos[i].obj;
		uint64_t iova;

		/* can't happen yet.. but when we add 2d support we'll have
		 * to deal w/ cross-ring synchronization:
		 */
		WARN_ON(is_active(msm_obj) && (msm_obj->gpu != gpu));

		/* submit takes a reference to the bo and iova until retired: */
		drm_gem_object_reference(&msm_obj->base);
		msm_gem_get_iova_locked(&msm_obj->base,
				submit->gpu->id, &iova);

		if (submit->bos[i].flags & MSM_SUBMIT_BO_WRITE)
			msm_gem_move_to_active(&msm_obj->base, gpu, true, submit->fence);
		else if (submit->bos[i].flags & MSM_SUBMIT_BO_READ)
			msm_gem_move_to_active(&msm_obj->base, gpu, false, submit->fence);
	}

	gpu->funcs->submit(gpu, submit, ctx);
	priv->lastctx = ctx;

	hangcheck_timer_reset(gpu);
}

/*
 * Init/Cleanup:
 */

static irqreturn_t irq_handler(int irq, void *data)
{
	struct msm_gpu *gpu = data;
	return gpu->funcs->irq(gpu);
}

static const char *clk_names[] = {
	"core", "iface", "rbbmtimer", "mem", "mem_iface", "alt_mem_iface",
};

int msm_gpu_init(struct drm_device *drm, struct platform_device *pdev,
		struct msm_gpu *gpu, const struct msm_gpu_funcs *funcs,
		const char *name, const char *ioname, const char *irqname, int ringsz)
{
	struct iommu_domain *iommu;
	int i, ret;

	if (WARN_ON(gpu->num_perfcntrs > ARRAY_SIZE(gpu->last_cntrs)))
		gpu->num_perfcntrs = ARRAY_SIZE(gpu->last_cntrs);

	gpu->dev = drm;
	gpu->funcs = funcs;
	gpu->name = name;
	gpu->fctx = msm_fence_context_alloc(drm, name);
	if (IS_ERR(gpu->fctx)) {
		ret = PTR_ERR(gpu->fctx);
		gpu->fctx = NULL;
		goto fail;
	}

	INIT_LIST_HEAD(&gpu->active_list);
	INIT_WORK(&gpu->retire_work, retire_worker);
	INIT_WORK(&gpu->recover_work, recover_worker);

	INIT_LIST_HEAD(&gpu->submit_list);

	setup_timer(&gpu->hangcheck_timer, hangcheck_handler,
			(unsigned long)gpu);

	spin_lock_init(&gpu->perf_lock);

	BUG_ON(ARRAY_SIZE(clk_names) != ARRAY_SIZE(gpu->grp_clks));

	/* Map registers: */
	gpu->mmio = msm_ioremap(pdev, ioname, name);
	if (IS_ERR(gpu->mmio)) {
		ret = PTR_ERR(gpu->mmio);
		goto fail;
	}

	/* Get Interrupt: */
	gpu->irq = platform_get_irq_byname(pdev, irqname);
	if (gpu->irq < 0) {
		ret = gpu->irq;
		dev_err(drm->dev, "failed to get irq: %d\n", ret);
		goto fail;
	}

	ret = devm_request_irq(&pdev->dev, gpu->irq, irq_handler,
			IRQF_TRIGGER_HIGH, gpu->name, gpu);
	if (ret) {
		dev_err(drm->dev, "failed to request IRQ%u: %d\n", gpu->irq, ret);
		goto fail;
	}

	/* Acquire clocks: */
	for (i = 0; i < ARRAY_SIZE(clk_names); i++) {
		gpu->grp_clks[i] = msm_clk_get(pdev, clk_names[i]);
		DBG("grp_clks[%s]: %p", clk_names[i], gpu->grp_clks[i]);
		if (IS_ERR(gpu->grp_clks[i]))
			gpu->grp_clks[i] = NULL;
	}

	gpu->ebi1_clk = msm_clk_get(pdev, "bus");
	DBG("ebi1_clk: %p", gpu->ebi1_clk);
	if (IS_ERR(gpu->ebi1_clk))
		gpu->ebi1_clk = NULL;

	/* Acquire regulators: */
	gpu->gpu_reg = devm_regulator_get(&pdev->dev, "vdd");
	DBG("gpu_reg: %p", gpu->gpu_reg);
	if (IS_ERR(gpu->gpu_reg))
		gpu->gpu_reg = NULL;

	gpu->gpu_cx = devm_regulator_get(&pdev->dev, "vddcx");
	DBG("gpu_cx: %p", gpu->gpu_cx);
	if (IS_ERR(gpu->gpu_cx))
		gpu->gpu_cx = NULL;

	/* Setup IOMMU.. eventually we will (I think) do this once per context
	 * and have separate page tables per context.  For now, to keep things
	 * simple and to get something working, just use a single address space:
	 */
	iommu = iommu_domain_alloc(&platform_bus_type);
	if (iommu) {
		/* TODO 32b vs 64b address space.. */
		iommu->geometry.aperture_start = SZ_16M;
		iommu->geometry.aperture_end = 0xffffffff;

		dev_info(drm->dev, "%s: using IOMMU\n", name);
		gpu->aspace = msm_gem_address_space_create(&pdev->dev,
				iommu, "gpu");
		if (IS_ERR(gpu->aspace)) {
			ret = PTR_ERR(gpu->aspace);
			dev_err(drm->dev, "failed to init iommu: %d\n", ret);
			gpu->aspace = NULL;
			iommu_domain_free(iommu);
			goto fail;
		}

	} else {
		dev_info(drm->dev, "%s: no IOMMU, fallback to VRAM carveout!\n", name);
	}
	gpu->id = msm_register_address_space(drm, gpu->aspace);


	/* Create ringbuffer: */
	mutex_lock(&drm->struct_mutex);
	gpu->rb = msm_ringbuffer_new(gpu, ringsz);
	mutex_unlock(&drm->struct_mutex);
	if (IS_ERR(gpu->rb)) {
		ret = PTR_ERR(gpu->rb);
		gpu->rb = NULL;
		dev_err(drm->dev, "could not create ringbuffer: %d\n", ret);
		goto fail;
	}

	gpu->pdev = pdev;
	platform_set_drvdata(pdev, gpu);

	bs_init(gpu);

	return 0;

fail:
	return ret;
}

void msm_gpu_cleanup(struct msm_gpu *gpu)
{
	DBG("%s", gpu->name);

	WARN_ON(!list_empty(&gpu->active_list));

	bs_fini(gpu);

	if (gpu->rb) {
		if (gpu->rb_iova)
			msm_gem_put_iova(gpu->rb->bo, gpu->id);
		msm_ringbuffer_destroy(gpu->rb);
	}

	if (gpu->aspace)
		msm_gem_address_space_destroy(gpu->aspace);

	if (gpu->fctx)
		msm_fence_context_free(gpu->fctx);
}
