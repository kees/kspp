/*
 * Sleepable Read-Copy Update mechanism for mutual exclusion.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * Copyright (C) IBM Corporation, 2006
 * Copyright (C) Fujitsu, 2012
 *
 * Author: Paul McKenney <paulmck@us.ibm.com>
 *	   Lai Jiangshan <laijs@cn.fujitsu.com>
 *
 * For detailed explanation of Read-Copy Update mechanism see -
 *		Documentation/RCU/ *.txt
 *
 */

#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/rcupdate_wait.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/srcu.h>

#include <linux/rcu_node_tree.h>
#include "rcu.h"

static int init_srcu_struct_fields(struct srcu_struct *sp)
{
	sp->completed = 0;
	sp->srcu_gp_seq = 0;
	spin_lock_init(&sp->queue_lock);
	sp->srcu_state = SRCU_STATE_IDLE;
	rcu_segcblist_init(&sp->srcu_cblist);
	INIT_DELAYED_WORK(&sp->work, process_srcu);
	sp->per_cpu_ref = alloc_percpu(struct srcu_array);
	return sp->per_cpu_ref ? 0 : -ENOMEM;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC

int __init_srcu_struct(struct srcu_struct *sp, const char *name,
		       struct lock_class_key *key)
{
	/* Don't re-initialize a lock while it is held. */
	debug_check_no_locks_freed((void *)sp, sizeof(*sp));
	lockdep_init_map(&sp->dep_map, name, key, 0);
	return init_srcu_struct_fields(sp);
}
EXPORT_SYMBOL_GPL(__init_srcu_struct);

#else /* #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/**
 * init_srcu_struct - initialize a sleep-RCU structure
 * @sp: structure to initialize.
 *
 * Must invoke this on a given srcu_struct before passing that srcu_struct
 * to any other function.  Each srcu_struct represents a separate domain
 * of SRCU protection.
 */
int init_srcu_struct(struct srcu_struct *sp)
{
	return init_srcu_struct_fields(sp);
}
EXPORT_SYMBOL_GPL(init_srcu_struct);

#endif /* #else #ifdef CONFIG_DEBUG_LOCK_ALLOC */

/*
 * Returns approximate total of the readers' ->lock_count[] values for the
 * rank of per-CPU counters specified by idx.
 */
static unsigned long srcu_readers_lock_idx(struct srcu_struct *sp, int idx)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct srcu_array *cpuc = per_cpu_ptr(sp->per_cpu_ref, cpu);

		sum += READ_ONCE(cpuc->lock_count[idx]);
	}
	return sum;
}

/*
 * Returns approximate total of the readers' ->unlock_count[] values for the
 * rank of per-CPU counters specified by idx.
 */
static unsigned long srcu_readers_unlock_idx(struct srcu_struct *sp, int idx)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct srcu_array *cpuc = per_cpu_ptr(sp->per_cpu_ref, cpu);

		sum += READ_ONCE(cpuc->unlock_count[idx]);
	}
	return sum;
}

/*
 * Return true if the number of pre-existing readers is determined to
 * be zero.
 */
static bool srcu_readers_active_idx_check(struct srcu_struct *sp, int idx)
{
	unsigned long unlocks;

	unlocks = srcu_readers_unlock_idx(sp, idx);

	/*
	 * Make sure that a lock is always counted if the corresponding unlock
	 * is counted. Needs to be a smp_mb() as the read side may contain a
	 * read from a variable that is written to before the synchronize_srcu()
	 * in the write side. In this case smp_mb()s A and B act like the store
	 * buffering pattern.
	 *
	 * This smp_mb() also pairs with smp_mb() C to prevent accesses after the
	 * synchronize_srcu() from being executed before the grace period ends.
	 */
	smp_mb(); /* A */

	/*
	 * If the locks are the same as the unlocks, then there must have
	 * been no readers on this index at some time in between. This does not
	 * mean that there are no more readers, as one could have read the
	 * current index but not have incremented the lock counter yet.
	 *
	 * Possible bug: There is no guarantee that there haven't been ULONG_MAX
	 * increments of ->lock_count[] since the unlocks were counted, meaning
	 * that this could return true even if there are still active readers.
	 * Since there are no memory barriers around srcu_flip(), the CPU is not
	 * required to increment ->completed before running
	 * srcu_readers_unlock_idx(), which means that there could be an
	 * arbitrarily large number of critical sections that execute after
	 * srcu_readers_unlock_idx() but use the old value of ->completed.
	 */
	return srcu_readers_lock_idx(sp, idx) == unlocks;
}

/**
 * srcu_readers_active - returns true if there are readers. and false
 *                       otherwise
 * @sp: which srcu_struct to count active readers (holding srcu_read_lock).
 *
 * Note that this is not an atomic primitive, and can therefore suffer
 * severe errors when invoked on an active srcu_struct.  That said, it
 * can be useful as an error check at cleanup time.
 */
static bool srcu_readers_active(struct srcu_struct *sp)
{
	int cpu;
	unsigned long sum = 0;

	for_each_possible_cpu(cpu) {
		struct srcu_array *cpuc = per_cpu_ptr(sp->per_cpu_ref, cpu);

		sum += READ_ONCE(cpuc->lock_count[0]);
		sum += READ_ONCE(cpuc->lock_count[1]);
		sum -= READ_ONCE(cpuc->unlock_count[0]);
		sum -= READ_ONCE(cpuc->unlock_count[1]);
	}
	return sum;
}

/**
 * cleanup_srcu_struct - deconstruct a sleep-RCU structure
 * @sp: structure to clean up.
 *
 * Must invoke this after you are finished using a given srcu_struct that
 * was initialized via init_srcu_struct(), else you leak memory.
 */
void cleanup_srcu_struct(struct srcu_struct *sp)
{
	if (WARN_ON(srcu_readers_active(sp)))
		return; /* Leakage unless caller handles error. */
	if (WARN_ON(!rcu_segcblist_empty(&sp->srcu_cblist)))
		return; /* Leakage unless caller handles error. */
	flush_delayed_work(&sp->work);
	if (WARN_ON(READ_ONCE(sp->srcu_state) != SRCU_STATE_IDLE))
		return; /* Caller forgot to stop doing call_srcu()? */
	free_percpu(sp->per_cpu_ref);
	sp->per_cpu_ref = NULL;
}
EXPORT_SYMBOL_GPL(cleanup_srcu_struct);

/*
 * Counts the new reader in the appropriate per-CPU element of the
 * srcu_struct.  Must be called from process context.
 * Returns an index that must be passed to the matching srcu_read_unlock().
 */
int __srcu_read_lock(struct srcu_struct *sp)
{
	int idx;

	idx = READ_ONCE(sp->completed) & 0x1;
	__this_cpu_inc(sp->per_cpu_ref->lock_count[idx]);
	smp_mb(); /* B */  /* Avoid leaking the critical section. */
	return idx;
}
EXPORT_SYMBOL_GPL(__srcu_read_lock);

/*
 * Removes the count for the old reader from the appropriate per-CPU
 * element of the srcu_struct.  Note that this may well be a different
 * CPU than that which was incremented by the corresponding srcu_read_lock().
 * Must be called from process context.
 */
void __srcu_read_unlock(struct srcu_struct *sp, int idx)
{
	smp_mb(); /* C */  /* Avoid leaking the critical section. */
	this_cpu_inc(sp->per_cpu_ref->unlock_count[idx]);
}
EXPORT_SYMBOL_GPL(__srcu_read_unlock);

/*
 * We use an adaptive strategy for synchronize_srcu() and especially for
 * synchronize_srcu_expedited().  We spin for a fixed time period
 * (defined below) to allow SRCU readers to exit their read-side critical
 * sections.  If there are still some readers after 10 microseconds,
 * we repeatedly block for 1-millisecond time periods.  This approach
 * has done well in testing, so there is no need for a config parameter.
 */
#define SRCU_RETRY_CHECK_DELAY		5
#define SYNCHRONIZE_SRCU_TRYCOUNT	2
#define SYNCHRONIZE_SRCU_EXP_TRYCOUNT	12

/*
 * Start an SRCU grace period.
 */
static void srcu_gp_start(struct srcu_struct *sp)
{
	rcu_segcblist_accelerate(&sp->srcu_cblist,
				 rcu_seq_snap(&sp->srcu_gp_seq));
	WRITE_ONCE(sp->srcu_state, SRCU_STATE_SCAN1);
	rcu_seq_start(&sp->srcu_gp_seq);
}

/*
 * Wait until all readers counted by array index idx complete, but loop
 * a maximum of trycount times.  The caller must ensure that ->completed
 * is not changed while checking.
 */
static bool try_check_zero(struct srcu_struct *sp, int idx, int trycount)
{
	for (;;) {
		if (srcu_readers_active_idx_check(sp, idx))
			return true;
		if (--trycount <= 0)
			return false;
		udelay(SRCU_RETRY_CHECK_DELAY);
	}
}

/*
 * Increment the ->completed counter so that future SRCU readers will
 * use the other rank of the ->(un)lock_count[] arrays.  This allows
 * us to wait for pre-existing readers in a starvation-free manner.
 */
static void srcu_flip(struct srcu_struct *sp)
{
	WRITE_ONCE(sp->completed, sp->completed + 1);

	/*
	 * Ensure that if the updater misses an __srcu_read_unlock()
	 * increment, that task's next __srcu_read_lock() will see the
	 * above counter update.  Note that both this memory barrier
	 * and the one in srcu_readers_active_idx_check() provide the
	 * guarantee for __srcu_read_lock().
	 */
	smp_mb(); /* D */  /* Pairs with C. */
}

/*
 * End an SRCU grace period.
 */
static void srcu_gp_end(struct srcu_struct *sp)
{
	rcu_seq_end(&sp->srcu_gp_seq);
	WRITE_ONCE(sp->srcu_state, SRCU_STATE_DONE);

	spin_lock_irq(&sp->queue_lock);
	rcu_segcblist_advance(&sp->srcu_cblist,
			      rcu_seq_current(&sp->srcu_gp_seq));
	spin_unlock_irq(&sp->queue_lock);
}

/*
 * Enqueue an SRCU callback on the specified srcu_struct structure,
 * initiating grace-period processing if it is not already running.
 *
 * Note that all CPUs must agree that the grace period extended beyond
 * all pre-existing SRCU read-side critical section.  On systems with
 * more than one CPU, this means that when "func()" is invoked, each CPU
 * is guaranteed to have executed a full memory barrier since the end of
 * its last corresponding SRCU read-side critical section whose beginning
 * preceded the call to call_rcu().  It also means that each CPU executing
 * an SRCU read-side critical section that continues beyond the start of
 * "func()" must have executed a memory barrier after the call_rcu()
 * but before the beginning of that SRCU read-side critical section.
 * Note that these guarantees include CPUs that are offline, idle, or
 * executing in user mode, as well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked call_rcu() and CPU B invoked the
 * resulting SRCU callback function "func()", then both CPU A and CPU
 * B are guaranteed to execute a full memory barrier during the time
 * interval between the call to call_rcu() and the invocation of "func()".
 * This guarantee applies even if CPU A and CPU B are the same CPU (but
 * again only if the system has more than one CPU).
 *
 * Of course, these guarantees apply only for invocations of call_srcu(),
 * srcu_read_lock(), and srcu_read_unlock() that are all passed the same
 * srcu_struct structure.
 */
void call_srcu(struct srcu_struct *sp, struct rcu_head *head,
	       rcu_callback_t func)
{
	unsigned long flags;

	head->next = NULL;
	head->func = func;
	spin_lock_irqsave(&sp->queue_lock, flags);
	smp_mb__after_unlock_lock(); /* Caller's prior accesses before GP. */
	rcu_segcblist_enqueue(&sp->srcu_cblist, head, false);
	if (READ_ONCE(sp->srcu_state) == SRCU_STATE_IDLE) {
		srcu_gp_start(sp);
		queue_delayed_work(system_power_efficient_wq, &sp->work, 0);
	}
	spin_unlock_irqrestore(&sp->queue_lock, flags);
}
EXPORT_SYMBOL_GPL(call_srcu);

static void srcu_reschedule(struct srcu_struct *sp, unsigned long delay);

/*
 * Helper function for synchronize_srcu() and synchronize_srcu_expedited().
 */
static void __synchronize_srcu(struct srcu_struct *sp, int trycount)
{
	struct rcu_synchronize rcu;
	struct rcu_head *head = &rcu.head;

	RCU_LOCKDEP_WARN(lock_is_held(&sp->dep_map) ||
			 lock_is_held(&rcu_bh_lock_map) ||
			 lock_is_held(&rcu_lock_map) ||
			 lock_is_held(&rcu_sched_lock_map),
			 "Illegal synchronize_srcu() in same-type SRCU (or in RCU) read-side critical section");

	if (rcu_scheduler_active == RCU_SCHEDULER_INACTIVE)
		return;
	might_sleep();
	init_completion(&rcu.completion);

	head->next = NULL;
	head->func = wakeme_after_rcu;
	spin_lock_irq(&sp->queue_lock);
	smp_mb__after_unlock_lock(); /* Caller's prior accesses before GP. */
	if (READ_ONCE(sp->srcu_state) == SRCU_STATE_IDLE) {
		/* steal the processing owner */
		rcu_segcblist_enqueue(&sp->srcu_cblist, head, false);
		srcu_gp_start(sp);
		spin_unlock_irq(&sp->queue_lock);
		/* give the processing owner to work_struct */
		srcu_reschedule(sp, 0);
	} else {
		rcu_segcblist_enqueue(&sp->srcu_cblist, head, false);
		spin_unlock_irq(&sp->queue_lock);
	}

	wait_for_completion(&rcu.completion);
	smp_mb(); /* Caller's later accesses after GP. */
}

/**
 * synchronize_srcu - wait for prior SRCU read-side critical-section completion
 * @sp: srcu_struct with which to synchronize.
 *
 * Wait for the count to drain to zero of both indexes. To avoid the
 * possible starvation of synchronize_srcu(), it waits for the count of
 * the index=((->completed & 1) ^ 1) to drain to zero at first,
 * and then flip the completed and wait for the count of the other index.
 *
 * Can block; must be called from process context.
 *
 * Note that it is illegal to call synchronize_srcu() from the corresponding
 * SRCU read-side critical section; doing so will result in deadlock.
 * However, it is perfectly legal to call synchronize_srcu() on one
 * srcu_struct from some other srcu_struct's read-side critical section,
 * as long as the resulting graph of srcu_structs is acyclic.
 *
 * There are memory-ordering constraints implied by synchronize_srcu().
 * On systems with more than one CPU, when synchronize_srcu() returns,
 * each CPU is guaranteed to have executed a full memory barrier since
 * the end of its last corresponding SRCU-sched read-side critical section
 * whose beginning preceded the call to synchronize_srcu().  In addition,
 * each CPU having an SRCU read-side critical section that extends beyond
 * the return from synchronize_srcu() is guaranteed to have executed a
 * full memory barrier after the beginning of synchronize_srcu() and before
 * the beginning of that SRCU read-side critical section.  Note that these
 * guarantees include CPUs that are offline, idle, or executing in user mode,
 * as well as CPUs that are executing in the kernel.
 *
 * Furthermore, if CPU A invoked synchronize_srcu(), which returned
 * to its caller on CPU B, then both CPU A and CPU B are guaranteed
 * to have executed a full memory barrier during the execution of
 * synchronize_srcu().  This guarantee applies even if CPU A and CPU B
 * are the same CPU, but again only if the system has more than one CPU.
 *
 * Of course, these memory-ordering guarantees apply only when
 * synchronize_srcu(), srcu_read_lock(), and srcu_read_unlock() are
 * passed the same srcu_struct structure.
 */
void synchronize_srcu(struct srcu_struct *sp)
{
	__synchronize_srcu(sp, (rcu_gp_is_expedited() && !rcu_gp_is_normal())
			   ? SYNCHRONIZE_SRCU_EXP_TRYCOUNT
			   : SYNCHRONIZE_SRCU_TRYCOUNT);
}
EXPORT_SYMBOL_GPL(synchronize_srcu);

/**
 * synchronize_srcu_expedited - Brute-force SRCU grace period
 * @sp: srcu_struct with which to synchronize.
 *
 * Wait for an SRCU grace period to elapse, but be more aggressive about
 * spinning rather than blocking when waiting.
 *
 * Note that synchronize_srcu_expedited() has the same deadlock and
 * memory-ordering properties as does synchronize_srcu().
 */
void synchronize_srcu_expedited(struct srcu_struct *sp)
{
	__synchronize_srcu(sp, SYNCHRONIZE_SRCU_EXP_TRYCOUNT);
}
EXPORT_SYMBOL_GPL(synchronize_srcu_expedited);

/**
 * srcu_barrier - Wait until all in-flight call_srcu() callbacks complete.
 * @sp: srcu_struct on which to wait for in-flight callbacks.
 */
void srcu_barrier(struct srcu_struct *sp)
{
	synchronize_srcu(sp);
}
EXPORT_SYMBOL_GPL(srcu_barrier);

/**
 * srcu_batches_completed - return batches completed.
 * @sp: srcu_struct on which to report batch completion.
 *
 * Report the number of batches, correlated with, but not necessarily
 * precisely the same as, the number of grace periods that have elapsed.
 */
unsigned long srcu_batches_completed(struct srcu_struct *sp)
{
	return sp->completed;
}
EXPORT_SYMBOL_GPL(srcu_batches_completed);

#define SRCU_CALLBACK_BATCH	10
#define SRCU_INTERVAL		1

/*
 * Core SRCU state machine.  Advance callbacks from ->batch_check0 to
 * ->batch_check1 and then to ->batch_done as readers drain.
 */
static void srcu_advance_batches(struct srcu_struct *sp, int trycount)
{
	int idx;

	WARN_ON_ONCE(sp->srcu_state == SRCU_STATE_IDLE);

	/*
	 * Because readers might be delayed for an extended period after
	 * fetching ->completed for their index, at any point in time there
	 * might well be readers using both idx=0 and idx=1.  We therefore
	 * need to wait for readers to clear from both index values before
	 * invoking a callback.
	 */

	if (sp->srcu_state == SRCU_STATE_DONE)
		srcu_gp_start(sp);

	if (sp->srcu_state == SRCU_STATE_SCAN1) {
		idx = 1 ^ (sp->completed & 1);
		if (!try_check_zero(sp, idx, trycount))
			return; /* readers present, retry after SRCU_INTERVAL */
		srcu_flip(sp);
		WRITE_ONCE(sp->srcu_state, SRCU_STATE_SCAN2);
	}

	if (sp->srcu_state == SRCU_STATE_SCAN2) {

		/*
		 * SRCU read-side critical sections are normally short,
		 * so check at least twice in quick succession after a flip.
		 */
		idx = 1 ^ (sp->completed & 1);
		trycount = trycount < 2 ? 2 : trycount;
		if (!try_check_zero(sp, idx, trycount))
			return; /* readers present, retry after SRCU_INTERVAL */
		srcu_gp_end(sp);
	}
}

/*
 * Invoke a limited number of SRCU callbacks that have passed through
 * their grace period.  If there are more to do, SRCU will reschedule
 * the workqueue.  Note that needed memory barriers have been executed
 * in this task's context by srcu_readers_active_idx_check().
 */
static void srcu_invoke_callbacks(struct srcu_struct *sp)
{
	struct rcu_cblist ready_cbs;
	struct rcu_head *rhp;

	spin_lock_irq(&sp->queue_lock);
	if (!rcu_segcblist_ready_cbs(&sp->srcu_cblist)) {
		spin_unlock_irq(&sp->queue_lock);
		return;
	}
	rcu_cblist_init(&ready_cbs);
	rcu_segcblist_extract_done_cbs(&sp->srcu_cblist, &ready_cbs);
	spin_unlock_irq(&sp->queue_lock);
	rhp = rcu_cblist_dequeue(&ready_cbs);
	for (; rhp != NULL; rhp = rcu_cblist_dequeue(&ready_cbs)) {
		local_bh_disable();
		rhp->func(rhp);
		local_bh_enable();
	}
	spin_lock_irq(&sp->queue_lock);
	rcu_segcblist_insert_count(&sp->srcu_cblist, &ready_cbs);
	spin_unlock_irq(&sp->queue_lock);
}

/*
 * Finished one round of SRCU grace period.  Start another if there are
 * more SRCU callbacks queued, otherwise put SRCU into not-running state.
 */
static void srcu_reschedule(struct srcu_struct *sp, unsigned long delay)
{
	bool pending = true;

	if (rcu_segcblist_empty(&sp->srcu_cblist)) {
		spin_lock_irq(&sp->queue_lock);
		if (rcu_segcblist_empty(&sp->srcu_cblist) &&
		    READ_ONCE(sp->srcu_state) == SRCU_STATE_DONE) {
			WRITE_ONCE(sp->srcu_state, SRCU_STATE_IDLE);
			pending = false;
		}
		spin_unlock_irq(&sp->queue_lock);
	}

	if (pending)
		queue_delayed_work(system_power_efficient_wq, &sp->work, delay);
}

/*
 * This is the work-queue function that handles SRCU grace periods.
 */
void process_srcu(struct work_struct *work)
{
	struct srcu_struct *sp;

	sp = container_of(work, struct srcu_struct, work.work);

	srcu_advance_batches(sp, 1);
	srcu_invoke_callbacks(sp);
	srcu_reschedule(sp, SRCU_INTERVAL);
}
EXPORT_SYMBOL_GPL(process_srcu);
