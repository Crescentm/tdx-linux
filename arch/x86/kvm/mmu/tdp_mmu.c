// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "mmu.h"
#include "mmu_internal.h"
#include "mmutrace.h"
#include "tdp_iter.h"
#include "tdp_mmu.h"
#include "spte.h"

#include <asm/cmpxchg.h>
#include <trace/events/kvm.h>

/* Initializes the TDP MMU for the VM, if enabled. */
int kvm_mmu_init_tdp_mmu(struct kvm *kvm)
{
	struct workqueue_struct *wq;

	wq = alloc_workqueue("kvm", WQ_UNBOUND|WQ_MEM_RECLAIM|WQ_CPU_INTENSIVE, 0);
	if (!wq)
		return -ENOMEM;

	INIT_LIST_HEAD(&kvm->arch.tdp_mmu_roots);
	spin_lock_init(&kvm->arch.tdp_mmu_pages_lock);
	kvm->arch.tdp_mmu_zap_wq = wq;
#ifdef CONFIG_INTEL_TDX_HOST_DEBUG_MEMORY_CORRUPT
	mutex_init(&kvm->arch.private_spt_for_split_lock);
#endif
	return 1;
}

/* Arbitrarily returns true so that this may be used in if statements. */
static __always_inline bool kvm_lockdep_assert_mmu_lock_held(struct kvm *kvm,
							     bool shared)
{
	if (shared)
		lockdep_assert_held_read(&kvm->mmu_lock);
	else
		lockdep_assert_held_write(&kvm->mmu_lock);

	return true;
}

void kvm_mmu_uninit_tdp_mmu(struct kvm *kvm)
{
	/*
	 * Invalidate all roots, which besides the obvious, schedules all roots
	 * for zapping and thus puts the TDP MMU's reference to each root, i.e.
	 * ultimately frees all roots.
	 */
	kvm_tdp_mmu_invalidate_all_roots(kvm, false);

	/*
	 * Destroying a workqueue also first flushes the workqueue, i.e. no
	 * need to invoke kvm_tdp_mmu_zap_invalidated_roots().
	 */
	destroy_workqueue(kvm->arch.tdp_mmu_zap_wq);

	WARN_ON(atomic64_read(&kvm->arch.tdp_mmu_pages));
	WARN_ON(atomic64_read(&kvm->arch.tdp_private_mmu_pages));
	WARN_ON(!list_empty(&kvm->arch.tdp_mmu_roots));

	/*
	 * Ensure that all the outstanding RCU callbacks to free shadow pages
	 * can run before the VM is torn down.  Work items on tdp_mmu_zap_wq
	 * can call kvm_tdp_mmu_put_root and create new callbacks.
	 */
	rcu_barrier();
}

static void tdp_mmu_free_sp(struct kvm_mmu_page *sp)
{
	kvm_mmu_free_private_spt(sp);
	free_page((unsigned long)sp->spt);
	kmem_cache_free(mmu_page_header_cache, sp);
}

/*
 * This is called through call_rcu in order to free TDP page table memory
 * safely with respect to other kernel threads that may be operating on
 * the memory.
 * By only accessing TDP MMU page table memory in an RCU read critical
 * section, and freeing it after a grace period, lockless access to that
 * memory won't use it after it is freed.
 */
static void tdp_mmu_free_sp_rcu_callback(struct rcu_head *head)
{
	struct kvm_mmu_page *sp = container_of(head, struct kvm_mmu_page,
					       rcu_head);

	tdp_mmu_free_sp(sp);
}

static void tdp_mmu_zap_root(struct kvm *kvm, struct kvm_mmu_page *root,
			     bool shared);

static void tdp_mmu_zap_root_work(struct work_struct *work)
{
	struct kvm_mmu_page *root = container_of(work, struct kvm_mmu_page,
						 tdp_mmu_async_work);
	struct kvm *kvm = root->tdp_mmu_async_data;

	read_lock(&kvm->mmu_lock);

	/*
	 * A TLB flush is not necessary as KVM performs a local TLB flush when
	 * allocating a new root (see kvm_mmu_load()), and when migrating vCPU
	 * to a different pCPU.  Note, the local TLB flush on reuse also
	 * invalidates any paging-structure-cache entries, i.e. TLB entries for
	 * intermediate paging structures, that may be zapped, as such entries
	 * are associated with the ASID on both VMX and SVM.
	 */
	tdp_mmu_zap_root(kvm, root, true);

	/*
	 * Drop the refcount using kvm_tdp_mmu_put_root() to test its logic for
	 * avoiding an infinite loop.  By design, the root is reachable while
	 * it's being asynchronously zapped, thus a different task can put its
	 * last reference, i.e. flowing through kvm_tdp_mmu_put_root() for an
	 * asynchronously zapped root is unavoidable.
	 */
	kvm_tdp_mmu_put_root(kvm, root, true);

	read_unlock(&kvm->mmu_lock);
}

static void tdp_mmu_schedule_zap_root(struct kvm *kvm, struct kvm_mmu_page *root)
{
	root->tdp_mmu_async_data = kvm;
	INIT_WORK(&root->tdp_mmu_async_work, tdp_mmu_zap_root_work);
	queue_work(kvm->arch.tdp_mmu_zap_wq, &root->tdp_mmu_async_work);
}

void kvm_tdp_mmu_put_root(struct kvm *kvm, struct kvm_mmu_page *root,
			  bool shared)
{
	kvm_lockdep_assert_mmu_lock_held(kvm, shared);

	if (!refcount_dec_and_test(&root->tdp_mmu_root_count))
		return;

	/*
	 * The TDP MMU itself holds a reference to each root until the root is
	 * explicitly invalidated, i.e. the final reference should be never be
	 * put for a valid root.
	 */
	KVM_BUG_ON(!is_tdp_mmu_page(root) || !root->role.invalid, kvm);

	spin_lock(&kvm->arch.tdp_mmu_pages_lock);
	list_del_rcu(&root->link);
	spin_unlock(&kvm->arch.tdp_mmu_pages_lock);
	call_rcu(&root->rcu_head, tdp_mmu_free_sp_rcu_callback);
}

/*
 * Returns the next root after @prev_root (or the first root if @prev_root is
 * NULL).  A reference to the returned root is acquired, and the reference to
 * @prev_root is released (the caller obviously must hold a reference to
 * @prev_root if it's non-NULL).
 *
 * If @only_valid is true, invalid roots are skipped.
 *
 * Returns NULL if the end of tdp_mmu_roots was reached.
 */
static struct kvm_mmu_page *tdp_mmu_next_root(struct kvm *kvm,
					      struct kvm_mmu_page *prev_root,
					      bool shared, bool only_valid)
{
	struct kvm_mmu_page *next_root;

	rcu_read_lock();

	if (prev_root)
		next_root = list_next_or_null_rcu(&kvm->arch.tdp_mmu_roots,
						  &prev_root->link,
						  typeof(*prev_root), link);
	else
		next_root = list_first_or_null_rcu(&kvm->arch.tdp_mmu_roots,
						   typeof(*next_root), link);

	while (next_root) {
		if ((!only_valid || !next_root->role.invalid) &&
		    kvm_tdp_mmu_get_root(next_root))
			break;

		next_root = list_next_or_null_rcu(&kvm->arch.tdp_mmu_roots,
				&next_root->link, typeof(*next_root), link);
	}

	rcu_read_unlock();

	if (prev_root)
		kvm_tdp_mmu_put_root(kvm, prev_root, shared);

	return next_root;
}

/*
 * Note: this iterator gets and puts references to the roots it iterates over.
 * This makes it safe to release the MMU lock and yield within the loop, but
 * if exiting the loop early, the caller must drop the reference to the most
 * recent root. (Unless keeping a live reference is desirable.)
 *
 * If shared is set, this function is operating under the MMU lock in read
 * mode. In the unlikely event that this thread must free a root, the lock
 * will be temporarily dropped and reacquired in write mode.
 */
#define __for_each_tdp_mmu_root_yield_safe(_kvm, _root, _as_id, _shared, _only_valid)\
	for (_root = tdp_mmu_next_root(_kvm, NULL, _shared, _only_valid);	\
	     _root;								\
	     _root = tdp_mmu_next_root(_kvm, _root, _shared, _only_valid))	\
		if (kvm_lockdep_assert_mmu_lock_held(_kvm, _shared) &&		\
		    kvm_mmu_page_as_id(_root) != _as_id) {			\
		} else

#define for_each_valid_tdp_mmu_root_yield_safe(_kvm, _root, _as_id, _shared)	\
	__for_each_tdp_mmu_root_yield_safe(_kvm, _root, _as_id, _shared, true)

#define for_each_tdp_mmu_root_yield_safe(_kvm, _root, _as_id)			\
	__for_each_tdp_mmu_root_yield_safe(_kvm, _root, _as_id, false, false)

/*
 * Iterate over all TDP MMU roots.  Requires that mmu_lock be held for write,
 * the implication being that any flow that holds mmu_lock for read is
 * inherently yield-friendly and should use the yield-safe variant above.
 * Holding mmu_lock for write obviates the need for RCU protection as the list
 * is guaranteed to be stable.
 */
#define for_each_tdp_mmu_root(_kvm, _root, _as_id)			\
	list_for_each_entry(_root, &_kvm->arch.tdp_mmu_roots, link)	\
		if (kvm_lockdep_assert_mmu_lock_held(_kvm, false) &&	\
		    kvm_mmu_page_as_id(_root) != _as_id) {		\
		} else

static struct kvm_mmu_page *tdp_mmu_alloc_sp(struct kvm_vcpu *vcpu,
					     union kvm_mmu_page_role role)
{
	struct kvm_mmu_page *sp;

	sp = kvm_mmu_memory_cache_alloc(&vcpu->arch.mmu_page_header_cache);
	sp->spt = kvm_mmu_memory_cache_alloc(&vcpu->arch.mmu_shadow_page_cache);
	sp->role = role;

	if (kvm_mmu_page_role_is_private(role))
		kvm_mmu_alloc_private_spt(vcpu, sp);

	return sp;
}

static void tdp_mmu_init_sp(struct kvm_mmu_page *sp, tdp_ptep_t sptep,
			    gfn_t gfn)
{
	INIT_LIST_HEAD(&sp->possible_nx_huge_page_link);

	set_page_private(virt_to_page(sp->spt), (unsigned long)sp);

	/*
	 * role must be set before calling this function.  At least role.level
	 * is not 0 (PG_LEVEL_NONE).
	 */
	WARN_ON_ONCE(!sp->role.word);
	sp->gfn = gfn;
	sp->ptep = sptep;
	sp->tdp_mmu_page = true;

	trace_kvm_mmu_get_page(sp, true);
}

static struct kvm_mmu_page *
kvm_tdp_mmu_get_vcpu_root_no_alloc(struct kvm_vcpu *vcpu, union kvm_mmu_page_role role)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_mmu_page *root;

	lockdep_assert_held(&kvm->mmu_lock);

	for_each_tdp_mmu_root(kvm, root, kvm_mmu_role_as_id(role)) {
		if (root->role.word == role.word &&
		    kvm_tdp_mmu_get_root(root))
			return root;
	}

	return NULL;
}

static struct kvm_mmu_page *kvm_tdp_mmu_get_vcpu_root(struct kvm_vcpu *vcpu,
						      bool private)
{
	union kvm_mmu_page_role role = vcpu->arch.mmu->root_role;
	struct kvm *kvm = vcpu->kvm;
	struct kvm_mmu_page *root;

	lockdep_assert_held_write(&kvm->mmu_lock);

	/*
	 * Check for an existing root before allocating a new one.  Note, the
	 * role check prevents consuming an invalid root.
	 */
	if (private)
		kvm_mmu_page_role_set_private(&role);
	root = kvm_tdp_mmu_get_vcpu_root_no_alloc(vcpu, role);
	if (!!root)
		goto out;

	root = tdp_mmu_alloc_sp(vcpu, role);
	tdp_mmu_init_sp(root, NULL, 0);

	/*
	 * TDP MMU roots are kept until they are explicitly invalidated, either
	 * by a memslot update or by the destruction of the VM.  Initialize the
	 * refcount to two; one reference for the vCPU, and one reference for
	 * the TDP MMU itself, which is held until the root is invalidated and
	 * is ultimately put by tdp_mmu_zap_root_work().
	 */
	refcount_set(&root->tdp_mmu_root_count, 2);

	spin_lock(&kvm->arch.tdp_mmu_pages_lock);
	list_add_rcu(&root->link, &kvm->arch.tdp_mmu_roots);
	spin_unlock(&kvm->arch.tdp_mmu_pages_lock);

out:
	return root;
}

hpa_t kvm_tdp_mmu_move_private_pages_from(struct kvm_vcpu *vcpu,
					  struct kvm_vcpu *src_vcpu)
{
	union kvm_mmu_page_role role = vcpu->arch.mmu->root_role;
	struct kvm *kvm = vcpu->kvm;
	struct kvm *src_kvm = src_vcpu->kvm;
	struct kvm_mmu_page *private_root = NULL;
	struct kvm_mmu_page *root;
	s64 num_private_pages, old;

	lockdep_assert_held_write(&vcpu->kvm->mmu_lock);
	lockdep_assert_held_write(&src_vcpu->kvm->mmu_lock);

	/* Find the private root of the source. */
	kvm_mmu_page_role_set_private(&role);
	for_each_tdp_mmu_root(src_kvm, root, kvm_mmu_role_as_id(role)) {
		if (root->role.word == role.word) {
			private_root = root;
			break;
		}
	}
	if (!private_root)
		return INVALID_PAGE;

	/* Remove the private root from the src kvm and add it to dst kvm. */
	list_del_rcu(&private_root->link);
	list_add_rcu(&private_root->link, &kvm->arch.tdp_mmu_roots);

	num_private_pages = atomic64_read(&src_kvm->arch.tdp_private_mmu_pages);
	old = atomic64_cmpxchg(&kvm->arch.tdp_private_mmu_pages, 0,
			       num_private_pages);
	/* The destination VM should have no private pages at this point. */
	WARN_ON(old);
	atomic64_set(&src_kvm->arch.tdp_private_mmu_pages, 0);

	return __pa(private_root->spt);
}

hpa_t kvm_tdp_mmu_get_vcpu_root_hpa_no_alloc(struct kvm_vcpu *vcpu, bool private)
{
	struct kvm_mmu_page *root;
	union kvm_mmu_page_role role = vcpu->arch.mmu->root_role;

	if (private)
		kvm_mmu_page_role_set_private(&role);
	root = kvm_tdp_mmu_get_vcpu_root_no_alloc(vcpu, role);
	if (!root)
		return INVALID_PAGE;

	return __pa(root->spt);
}

hpa_t kvm_tdp_mmu_get_vcpu_root_hpa(struct kvm_vcpu *vcpu, bool private)
{
	return __pa(kvm_tdp_mmu_get_vcpu_root(vcpu, private)->spt);
}

static void handle_changed_spte(struct kvm *kvm, int as_id, gfn_t gfn,
				u64 old_spte, u64 new_spte,
				union kvm_mmu_page_role role, bool shared);

static void tdp_account_mmu_page(struct kvm *kvm, struct kvm_mmu_page *sp)
{
	kvm_account_pgtable_pages((void *)sp->spt, +1);
	if (is_private_sp(sp))
		atomic64_inc(&kvm->arch.tdp_private_mmu_pages);
	else
		atomic64_inc(&kvm->arch.tdp_mmu_pages);
}

static void tdp_unaccount_mmu_page(struct kvm *kvm, struct kvm_mmu_page *sp)
{
	kvm_account_pgtable_pages((void *)sp->spt, -1);
	if (is_private_sp(sp))
		atomic64_dec(&kvm->arch.tdp_private_mmu_pages);
	else
		atomic64_dec(&kvm->arch.tdp_mmu_pages);
}

/**
 * tdp_mmu_unlink_sp() - Remove a shadow page from the list of used pages
 *
 * @kvm: kvm instance
 * @sp: the page to be removed
 * @shared: This operation may not be running under the exclusive use of
 *	    the MMU lock and the operation must synchronize with other
 *	    threads that might be adding or removing pages.
 */
static void tdp_mmu_unlink_sp(struct kvm *kvm, struct kvm_mmu_page *sp,
			      bool shared)
{
	tdp_unaccount_mmu_page(kvm, sp);

	if (!sp->nx_huge_page_disallowed)
		return;

	if (shared)
		spin_lock(&kvm->arch.tdp_mmu_pages_lock);
	else
		lockdep_assert_held_write(&kvm->mmu_lock);

	sp->nx_huge_page_disallowed = false;
	untrack_possible_nx_huge_page(kvm, sp);

	if (shared)
		spin_unlock(&kvm->arch.tdp_mmu_pages_lock);
}

/**
 * handle_removed_pt() - handle a page table removed from the TDP structure
 *
 * @kvm: kvm instance
 * @pt: the page removed from the paging structure
 * @shared: This operation may not be running under the exclusive use
 *	    of the MMU lock and the operation must synchronize with other
 *	    threads that might be modifying SPTEs.
 *
 * Given a page table that has been removed from the TDP paging structure,
 * iterates through the page table to clear SPTEs and free child page tables.
 *
 * Note that pt is passed in as a tdp_ptep_t, but it does not need RCU
 * protection. Since this thread removed it from the paging structure,
 * this thread will be responsible for ensuring the page is freed. Hence the
 * early rcu_dereferences in the function.
 */
static void handle_removed_pt(struct kvm *kvm, tdp_ptep_t pt, bool shared)
{
	struct kvm_mmu_page *sp = sptep_to_sp(rcu_dereference(pt));
	int level = sp->role.level;
	gfn_t base_gfn = sp->gfn;
	int i;

	trace_kvm_mmu_prepare_zap_page(sp);

	tdp_mmu_unlink_sp(kvm, sp, shared);

	for (i = 0; i < SPTE_ENT_PER_PAGE; i++) {
		tdp_ptep_t sptep = pt + i;
		gfn_t gfn = base_gfn + i * KVM_PAGES_PER_HPAGE(level);
		u64 old_spte;

		if (shared) {
			/*
			 * Set the SPTE to a nonpresent value that other
			 * threads will not overwrite. If the SPTE was
			 * already marked as removed then another thread
			 * handling a page fault could overwrite it, so
			 * set the SPTE until it is set from some other
			 * value to the removed SPTE value.
			 */
			for (;;) {
				old_spte = kvm_tdp_mmu_write_spte_atomic(sptep, REMOVED_SPTE);
				if (!is_removed_spte(old_spte))
					break;
				cpu_relax();
			}
		} else {
			/*
			 * If the SPTE is not MMU-present, there is no backing
			 * page associated with the SPTE and so no side effects
			 * that need to be recorded, and exclusive ownership of
			 * mmu_lock ensures the SPTE can't be made present.
			 * Note, zapping MMIO SPTEs is also unnecessary as they
			 * are guarded by the memslots generation, not by being
			 * unreachable.
			 */
			old_spte = kvm_tdp_mmu_read_spte(sptep);
			/*
			 * It comes here when zapping all pages when destroying
			 * vm.  It means TLB shootdown optimization doesn't make
			 * sense.  Zap private_zapped entry.
			 */
			if (!is_shadow_present_pte(old_spte) &&
			    !is_private_zapped_spte(old_spte))
				continue;

			/*
			 * Use the common helper instead of a raw WRITE_ONCE as
			 * the SPTE needs to be updated atomically if it can be
			 * modified by a different vCPU outside of mmu_lock.
			 * Even though the parent SPTE is !PRESENT, the TLB
			 * hasn't yet been flushed, and both Intel and AMD
			 * document that A/D assists can use upper-level PxE
			 * entries that are cached in the TLB, i.e. the CPU can
			 * still access the page and mark it dirty.
			 *
			 * No retry is needed in the atomic update path as the
			 * sole concern is dropping a Dirty bit, i.e. no other
			 * task can zap/remove the SPTE as mmu_lock is held for
			 * write.  Marking the SPTE as a removed SPTE is not
			 * strictly necessary for the same reason, but using
			 * the remove SPTE value keeps the shared/exclusive
			 * paths consistent and allows the handle_changed_spte()
			 * call below to hardcode the new value to REMOVED_SPTE.
			 *
			 * Note, even though dropping a Dirty bit is the only
			 * scenario where a non-atomic update could result in a
			 * functional bug, simply checking the Dirty bit isn't
			 * sufficient as a fast page fault could read the upper
			 * level SPTE before it is zapped, and then make this
			 * target SPTE writable, resume the guest, and set the
			 * Dirty bit between reading the SPTE above and writing
			 * it here.
			 */
			old_spte = kvm_tdp_mmu_write_spte(sptep, old_spte,
							  REMOVED_SPTE, level);
		}
		handle_changed_spte(kvm, kvm_mmu_page_as_id(sp), gfn,
				    old_spte, REMOVED_SPTE, sp->role,
				    shared);
	}

	KVM_BUG_ON(is_private_sp(sp) && !kvm_mmu_private_spt(sp), kvm);
	if (is_private_sp(sp) &&
	    WARN_ON(static_call(kvm_x86_free_private_spt)(kvm, sp->gfn, sp->role.level,
							  kvm_mmu_private_spt(sp)))) {
		/*
		 * Failed to unlink Secure EPT page and there is nothing to do
		 * further.  Intentionally leak the page to prevent the kernel
		 * from accessing the encrypted page.
		 */
		kvm_mmu_init_private_spt(sp, NULL);
	}

	call_rcu(&sp->rcu_head, tdp_mmu_free_sp_rcu_callback);
}

static void *get_private_spt(gfn_t gfn, u64 new_spte, int level)
{
	if (is_shadow_present_pte(new_spte) && !is_last_spte(new_spte, level)) {
		struct kvm_mmu_page *sp = to_shadow_page(pfn_to_hpa(spte_to_pfn(new_spte)));
		void *private_spt = kvm_mmu_private_spt(sp);

		WARN_ON_ONCE(!private_spt);
		WARN_ON_ONCE(sp->role.level + 1 != level);
		WARN_ON_ONCE(sp->gfn != gfn);
		return private_spt;
	}

	return NULL;
}

static void handle_private_zapped_spte(struct kvm *kvm, gfn_t gfn,
				       u64 old_spte, u64 new_spte, int level)
{
	bool was_private_zapped = is_private_zapped_spte(old_spte);
	bool is_private_zapped = is_private_zapped_spte(new_spte);
	bool was_present = is_shadow_present_pte(old_spte);
	bool is_present = is_shadow_present_pte(new_spte);
	kvm_pfn_t old_pfn = spte_to_pfn(old_spte);
	int ret = 0;

	/* Temporarily blocked private SPTE can only be leaf. */
	KVM_BUG_ON(!is_last_spte(old_spte, level), kvm);
	KVM_BUG_ON(is_private_zapped, kvm);
	KVM_BUG_ON(was_present, kvm);
	KVM_BUG_ON(!was_private_zapped, kvm);
	KVM_BUG_ON(is_present, kvm);

	lockdep_assert_held_write(&kvm->mmu_lock);
	ret = static_call(kvm_x86_remove_private_spte)(kvm, gfn, level, old_pfn);
	KVM_BUG_ON(ret, kvm);
}

static void handle_removed_private_spte(struct kvm *kvm, gfn_t gfn,
					u64 old_spte, u64 new_spte,
					int level)
{
	bool was_present = is_shadow_present_pte(old_spte);
	bool is_present = is_shadow_present_pte(new_spte);
	bool was_leaf = was_present && is_last_spte(old_spte, level);
	bool is_leaf = is_present && is_last_spte(new_spte, level);
	bool was_private_zapped = is_private_zapped_spte(old_spte);
	bool is_private_zapped = is_private_zapped_spte(new_spte);
	kvm_pfn_t old_pfn = spte_to_pfn(old_spte);
	kvm_pfn_t new_pfn = spte_to_pfn(new_spte);
	int ret;

	/* Ignore change of software only bits. e.g. host_writable */
	if (was_leaf == is_leaf && was_present == is_present &&
	    was_private_zapped == is_private_zapped)
		return;

	/*
	 * Allow only leaf page to be zapped.  Reclaim Non-leaf page tables at
	 * destroying VM.
	 */
	KVM_BUG_ON(was_private_zapped && is_private_zapped, kvm);
	WARN_ON_ONCE(is_present);
	if (!was_leaf)
		return;

	/* Zapping leaf spte is allowed only when write lock is held. */
	lockdep_assert_held_write(&kvm->mmu_lock);
	ret = static_call(kvm_x86_zap_private_spte)(kvm, gfn, level);
	if (is_private_zapped) {
		/* page migration isn't supported yet. */
		KVM_BUG_ON(new_pfn != old_pfn, kvm);
		return;
	}
	/* Because write lock is held, operation should success. */
	if (KVM_BUG_ON(ret, kvm))
		return;

	/* non-present -> non-present doesn't make sense. */
	KVM_BUG_ON(!was_present, kvm);
	KVM_BUG_ON(new_pfn, kvm);
	ret = static_call(kvm_x86_remove_private_spte)(kvm, gfn, level, old_pfn);
	KVM_BUG_ON(ret, kvm);
}

/**
 * handle_changed_spte - handle bookkeeping associated with an SPTE change
 * @kvm: kvm instance
 * @as_id: the address space of the paging structure the SPTE was a part of
 * @gfn: the base GFN that was mapped by the SPTE
 * @old_spte: The value of the SPTE before the change
 * @new_spte: The value of the SPTE after the change
 * @role: the role of the PT the SPTE is part of in the paging structure
 * @shared: This operation may not be running under the exclusive use of
 *	    the MMU lock and the operation must synchronize with other
 *	    threads that might be modifying SPTEs.
 *
 * Handle bookkeeping that might result from the modification of a SPTE.  Note,
 * dirty logging updates are handled in common code, not here (see make_spte()
 * and fast_pf_fix_direct_spte()).
 */
static void handle_changed_spte(struct kvm *kvm, int as_id, gfn_t gfn,
				u64 old_spte, u64 new_spte,
				union kvm_mmu_page_role role, bool shared)
{
	bool is_private = kvm_mmu_page_role_is_private(role);
	int level = role.level;
	bool was_present = is_shadow_present_pte(old_spte);
	bool is_present = is_shadow_present_pte(new_spte);
	bool was_last = is_last_spte(old_spte, level);
	bool was_leaf = was_present && was_last;
	bool is_leaf = is_present && is_last_spte(new_spte, level);
	kvm_pfn_t old_pfn = spte_to_pfn(old_spte);
	kvm_pfn_t new_pfn = spte_to_pfn(new_spte);
	bool pfn_changed = old_pfn != new_pfn;
	bool was_private_zapped = is_private_zapped_spte(old_spte);

	WARN_ON_ONCE(level > PT64_ROOT_MAX_LEVEL);
	WARN_ON_ONCE(level < PG_LEVEL_4K);
	WARN_ON_ONCE(gfn & (KVM_PAGES_PER_HPAGE(level) - 1));
	KVM_BUG_ON(was_private_zapped && !is_private, kvm);
	KVM_BUG_ON(kvm_is_private_gpa(kvm, gfn_to_gpa(gfn)) != is_private, kvm);

	/*
	 * If this warning were to trigger it would indicate that there was a
	 * missing MMU notifier or a race with some notifier handler.
	 * A present, leaf SPTE should never be directly replaced with another
	 * present leaf SPTE pointing to a different PFN. A notifier handler
	 * should be zapping the SPTE before the main MM's page table is
	 * changed, or the SPTE should be zeroed, and the TLBs flushed by the
	 * thread before replacement.
	 */
	if (was_leaf && is_leaf && pfn_changed) {
		pr_err("Invalid SPTE change: cannot replace a present leaf\n"
		       "SPTE with another present leaf SPTE mapping a\n"
		       "different PFN!\n"
		       "as_id: %d gfn: %llx old_spte: %llx new_spte: %llx level: %d",
		       as_id, gfn, old_spte, new_spte, level);

		/*
		 * Crash the host to prevent error propagation and guest data
		 * corruption.
		 */
		BUG();
	}

	if (old_spte == new_spte)
		return;

	trace_kvm_tdp_mmu_spte_changed(as_id, gfn, level, old_spte, new_spte);

	if (is_leaf)
		check_spte_writable_invariants(new_spte);

	if (was_private_zapped && !is_present) {
		handle_private_zapped_spte(kvm, gfn, old_spte, new_spte, level);
		return;
	}

	/*
	 * The only times a SPTE should be changed from a non-present to
	 * non-present state is when an MMIO entry is installed/modified/
	 * removed. In that case, there is nothing to do here.
	 */
	if (!was_present && !is_present) {
		/*
		 * If this change does not involve a MMIO SPTE or removed SPTE,
		 * it is unexpected. Log the change, though it should not
		 * impact the guest since both the former and current SPTEs
		 * are nonpresent.
		 */
		if (WARN_ON_ONCE(!is_mmio_spte(kvm, old_spte) &&
				 !is_mmio_spte(kvm, new_spte) &&
				 !is_removed_spte(new_spte)))
			pr_err("Unexpected SPTE change! Nonpresent SPTEs\n"
			       "should not be replaced with another,\n"
			       "different nonpresent SPTE, unless one or both\n"
			       "are MMIO SPTEs, or the new SPTE is\n"
			       "a temporary removed SPTE.\n"
			       "as_id: %d gfn: %llx old_spte: %llx new_spte: %llx level: %d",
			       as_id, gfn, old_spte, new_spte, level);
		return;
	}

	if (is_leaf != was_leaf)
		kvm_update_page_stats(kvm, level, is_leaf ? 1 : -1);

	if (was_leaf && is_dirty_spte(old_spte) &&
	    (!is_present || !is_dirty_spte(new_spte) || pfn_changed))
		kvm_set_pfn_dirty(old_pfn);

	/*
	 * Recursively handle child PTs if the change removed a subtree from
	 * the paging structure.  Note the WARN on the PFN changing without the
	 * SPTE being converted to a hugepage (leaf) or being zapped.  Shadow
	 * pages are kernel allocations and should never be migrated.
	 */
	if (was_present && !was_last &&
	    (is_leaf || !is_present || WARN_ON_ONCE(pfn_changed))) {
		KVM_BUG_ON(is_private != is_private_sptep(spte_to_child_pt(old_spte, level)),
			   kvm);
		handle_removed_pt(kvm, spte_to_child_pt(old_spte, level), shared);
	}

	/*
	 * Secure-EPT requires to remove Secure-EPT tables after removing
	 * children.  hooks after handling lower page table by above
	 * handle_remove_pt().
	 */
	if (is_private && !is_present) {
		/*
		 * When write lock is held, leaf pte should be zapping or
		 * prohibiting.  Not directly was_present=1 -> zero EPT entry.
		 */
		KVM_BUG_ON(!shared && is_leaf && !is_private_zapped_spte(new_spte), kvm);
		handle_removed_private_spte(kvm, gfn, old_spte, new_spte, role.level);
	}

	if (was_leaf && is_accessed_spte(old_spte) &&
	    (!is_present || !is_accessed_spte(new_spte) || pfn_changed))
		kvm_set_pfn_accessed(spte_to_pfn(old_spte));
}

static int private_spte_change_flags(struct kvm *kvm, gfn_t gfn, u64 old_spte,
				     u64 new_spte, int level)
{
	bool was_writable = is_writable_pte(old_spte);
	bool is_writable = is_writable_pte(new_spte);

	/* Write block. TODO: Optimize with batching */
	if (was_writable && !is_writable) {
		/* Sanity check: should be 4KB only */
		KVM_BUG_ON(level != PG_LEVEL_4K, kvm);
		static_call(kvm_x86_write_block_private_pages)(kvm,
							(gfn_t *)&gfn, 1);
	}

	/* Write unblock should have been handled by the fast page fault */
	WARN_ON_ONCE(!was_writable && is_writable);

	return 0;
}

static int __must_check __set_private_spte_present(struct kvm *kvm, tdp_ptep_t sptep,
						   gfn_t gfn, u64 old_spte,
						   u64 new_spte, int level)
{
	bool was_private_zapped = is_private_zapped_spte(old_spte);
	bool is_private_zapped = is_private_zapped_spte(new_spte);
	bool was_last = is_last_spte(old_spte, level);
	bool is_last = is_last_spte(new_spte, level);
	bool was_present = is_shadow_present_pte(old_spte);
	bool is_present = is_shadow_present_pte(new_spte);
	bool was_leaf = was_present && is_last_spte(old_spte, level);
	bool is_leaf = is_present && is_last_spte(new_spte, level);
	kvm_pfn_t old_pfn = spte_to_pfn(old_spte);
	kvm_pfn_t new_pfn = spte_to_pfn(new_spte);
	void *private_spt;
	int ret = 0;

	lockdep_assert_held(&kvm->mmu_lock);

	/*
	 * Handle special case of old_spte being temporarily blocked private
	 * SPTE.  There are two cases: 1) Need to restore the original mapping
	 * (unblock) when guest accesses the private page; 2) Need to truly zap
	 * the SPTE because of zapping aliasing in fault handler, or when VM is
	 * being destroyed.
	 * See handel_private_zapped_spte() for zapping case.
	 */
	if (was_private_zapped) {
		/* Temporarily blocked private SPTE can only be leaf. */
		KVM_BUG_ON(!is_last_spte(old_spte, level), kvm);
		KVM_BUG_ON(!is_present, kvm);
		KVM_BUG_ON(is_private_zapped, kvm);
		KVM_BUG_ON(was_present, kvm);
		KVM_BUG_ON(!was_private_zapped, kvm);

		if (old_pfn == new_pfn) {
			ret = static_call(kvm_x86_unzap_private_spte)(kvm, gfn,
								      level);
		} else if (level > PG_LEVEL_4K && was_last && !is_last) {
			/*
			 * Splitting private_zapped large page doesn't happen.
			 * Unzap and then split.
			 */
			pr_err("gfn 0x%llx old_spte 0x%llx new_spte 0x%llx level %d\n",
			       gfn, old_spte, new_spte, level);
			WARN_ON(1);
		} else {
			/*
			 * Because page is pinned (refer to
			 * kvm_faultin_pfn_private()), page migration shouldn't
			 * be triggered for private page.  kvm private memory
			 * slot case should also prevent page migration.
			 */
			pr_err("gfn 0x%llx old_spte 0x%llx new_spte 0x%llx level %d\n",
			       gfn, old_spte, new_spte, level);
			WARN_ON(1);
		}

		return ret;
	}

	/*
	 * Use different call to either set up middle level
	 * private page table, or leaf.
	 */
	if (level > PG_LEVEL_4K && was_leaf && !is_leaf) {
		/*
		 * splitting large page into 4KB.
		 * tdp_mmu_split_huage_page() => tdp_mmu_link_sp()
		 */
		private_spt = get_private_spt(gfn, new_spte, level);
		KVM_BUG_ON(!private_spt, kvm);
		ret = static_call(kvm_x86_zap_private_spte)(kvm, gfn, level);
		kvm_flush_remote_tlbs(kvm);
		if (!ret)
			ret = static_call(kvm_x86_split_private_spt)(kvm, gfn,
								     level, private_spt);
	} else if (is_leaf) {
		if (was_present)
			return private_spte_change_flags(kvm, gfn, old_spte,
							 new_spte, level);

		ret = static_call(kvm_x86_set_private_spte)(kvm, gfn, level, new_pfn);
	} else {
		private_spt = get_private_spt(gfn, new_spte, level);
		KVM_BUG_ON(!private_spt, kvm);
		ret = static_call(kvm_x86_link_private_spt)(kvm, gfn, level, private_spt);
	}

	return ret;
}

static int __must_check set_private_spte_present(struct kvm *kvm, tdp_ptep_t sptep,
						 gfn_t gfn, u64 old_spte,
						 u64 new_spte, int level)
{
	int ret;

	/*
	 * For private page table, callbacks are needed to propagate SPTE
	 * change into the protected page table.  In order to atomically update
	 * both the SPTE and the protected page tables with callbacks, utilize
	 * freezing SPTE.
	 * - Freeze the SPTE. Set entry to REMOVED_SPTE.
	 * - Trigger callbacks for protected page tables.
	 * - Unfreeze the SPTE.  Set the entry to new_spte.
	 */
	lockdep_assert_held(&kvm->mmu_lock);
	if (!try_cmpxchg64(sptep, &old_spte, REMOVED_SPTE))
		return -EBUSY;

	ret = __set_private_spte_present(kvm, sptep, gfn, old_spte, new_spte, level);
	if (ret)
		__kvm_tdp_mmu_write_spte(sptep, old_spte);
	else
		__kvm_tdp_mmu_write_spte(sptep, new_spte);
	return ret;
}

/*
 * tdp_mmu_set_spte_atomic - Set a TDP MMU SPTE atomically
 * and handle the associated bookkeeping.  Do not mark the page dirty
 * in KVM's dirty bitmaps.
 *
 * If setting the SPTE fails because it has changed, iter->old_spte will be
 * refreshed to the current value of the spte.
 *
 * @kvm: kvm instance
 * @iter: a tdp_iter instance currently on the SPTE that should be set
 * @new_spte: The value the SPTE should be set to
 * Return:
 * * 0      - If the SPTE was set.
 * * -EBUSY - If the SPTE cannot be set. In this case this function will have
 *            no side-effects other than setting iter->old_spte to the last
 *            known value of the spte.
 */
static inline int __must_check tdp_mmu_set_spte_atomic(struct kvm *kvm,
						       struct tdp_iter *iter,
						       u64 new_spte)
{
	u64 *sptep = rcu_dereference(iter->sptep);
	bool freezed = false;

	KVM_BUG_ON(iter->yielded, kvm);

	/*
	 * The caller is responsible for ensuring the old SPTE is not a REMOVED
	 * SPTE.  KVM should never attempt to zap or manipulate a REMOVED SPTE,
	 * and pre-checking before inserting a new SPTE is advantageous as it
	 * avoids unnecessary work.
	 */
	WARN_ON_ONCE(iter->yielded || is_removed_spte(iter->old_spte));

	lockdep_assert_held_read(&kvm->mmu_lock);

	if (is_private_sptep(iter->sptep) && !is_removed_spte(new_spte)) {
		int ret;

		if (is_shadow_present_pte(new_spte)) {
			/*
			 * Populating case. handle_changed_spte() can
			 * process without freezing because it only updates
			 * stats.
			 */
			ret = set_private_spte_present(kvm, iter->sptep, iter->gfn,
						       iter->old_spte, new_spte, iter->level);
			if (ret)
				return ret;
		} else {
			/*
			 * Zapping case. handle_changed_spte() calls Secure-EPT
			 * blocking or removal.  Freeze the entry.
			 */
			if (!try_cmpxchg64(sptep, &iter->old_spte, REMOVED_SPTE))
				return -EBUSY;
			freezed = true;
		}
	} else {
		/*
		 * Note, fast_pf_fix_direct_spte() can also modify TDP MMU SPTEs
		 * and does not hold the mmu_lock.  On failure, i.e. if a
		 * different logical CPU modified the SPTE, try_cmpxchg64()
		 * updates iter->old_spte with the current value, so the caller
		 * operates on fresh data, e.g. if it retries
		 * tdp_mmu_set_spte_atomic()
		 */
		if (!try_cmpxchg64(sptep, &iter->old_spte, new_spte))
			return -EBUSY;
	}

	handle_changed_spte(kvm, iter->as_id, iter->gfn, iter->old_spte,
			    new_spte, sptep_to_sp(sptep)->role, true);
	if (freezed)
		__kvm_tdp_mmu_write_spte(sptep, new_spte);
	return 0;
}

static u64 __private_zapped_spte(u64 old_spte)
{
	return SHADOW_NONPRESENT_VALUE | SPTE_PRIVATE_ZAPPED |
		(spte_to_pfn(old_spte) << PAGE_SHIFT) |
		(is_large_pte(old_spte) ? PT_PAGE_SIZE_MASK : 0);
}

static u64 private_zapped_spte(struct kvm *kvm, const struct tdp_iter *iter)
{
	if (!kvm_gfn_shared_mask(kvm))
		return SHADOW_NONPRESENT_VALUE;

	if (!is_private_sptep(iter->sptep))
		return SHADOW_NONPRESENT_VALUE;

	return __private_zapped_spte(iter->old_spte);
}

static inline int __must_check tdp_mmu_zap_spte_atomic(struct kvm *kvm,
						       struct tdp_iter *iter)
{
	int ret;

	/*
	 * Freeze the SPTE by setting it to a special,
	 * non-present value. This will stop other threads from
	 * immediately installing a present entry in its place
	 * before the TLBs are flushed.
	 */
	ret = tdp_mmu_set_spte_atomic(kvm, iter, REMOVED_SPTE);
	if (ret)
		return ret;

	kvm_flush_remote_tlbs_gfn(kvm, iter->gfn, iter->level);

	/*
	 * No other thread can overwrite the removed SPTE as they must either
	 * wait on the MMU lock or use tdp_mmu_set_spte_atomic() which will not
	 * overwrite the special removed SPTE value. No bookkeeping is needed
	 * here since the SPTE is going from non-present to non-present.  Use
	 * the raw write helper to avoid an unnecessary check on volatile bits.
	 */
	__kvm_tdp_mmu_write_spte(iter->sptep, private_zapped_spte(kvm, iter));

	return 0;
}


/*
 * tdp_mmu_set_spte - Set a TDP MMU SPTE and handle the associated bookkeeping
 * @kvm:	      KVM instance
 * @as_id:	      Address space ID, i.e. regular vs. SMM
 * @sptep:	      Pointer to the SPTE
 * @old_spte:	      The current value of the SPTE
 * @new_spte:	      The new value that will be set for the SPTE
 * @gfn:	      The base GFN that was (or will be) mapped by the SPTE
 * @level:	      The level _containing_ the SPTE (its parent PT's level)
 *
 * Returns the old SPTE value, which _may_ be different than @old_spte if the
 * SPTE had voldatile bits.
 */
static u64 tdp_mmu_set_spte(struct kvm *kvm, int as_id, tdp_ptep_t sptep,
			    u64 old_spte, u64 new_spte, gfn_t gfn, int level)
{
	union kvm_mmu_page_role role;

	KVM_BUG_ON(is_private_sptep(sptep) != kvm_is_private_gpa(kvm, gfn_to_gpa(gfn)), kvm);
	lockdep_assert_held_write(&kvm->mmu_lock);

	/*
	 * No thread should be using this function to set SPTEs to or from the
	 * temporary removed SPTE value.
	 * If operating under the MMU lock in read mode, tdp_mmu_set_spte_atomic
	 * should be used. If operating under the MMU lock in write mode, the
	 * use of the removed SPTE should not be necessary.
	 */
	WARN_ON_ONCE(is_removed_spte(old_spte) || is_removed_spte(new_spte));

	old_spte = kvm_tdp_mmu_write_spte(sptep, old_spte, new_spte, level);
	if (is_private_sptep(sptep) && !is_removed_spte(new_spte) &&
	    is_shadow_present_pte(new_spte)) {
		lockdep_assert_held_write(&kvm->mmu_lock);
		/* Because write spin lock is held, no race.  It should success. */
		KVM_BUG_ON(__set_private_spte_present(kvm, sptep, gfn, old_spte,
						      new_spte, level), kvm);
	}

	role = sptep_to_sp(sptep)->role;
	role.level = level;
	handle_changed_spte(kvm, as_id, gfn, old_spte, new_spte, role, false);
	return old_spte;
}

static inline void tdp_mmu_iter_set_spte(struct kvm *kvm, struct tdp_iter *iter,
					 u64 new_spte)
{
	WARN_ON_ONCE(iter->yielded);
	iter->old_spte = tdp_mmu_set_spte(kvm, iter->as_id, iter->sptep,
					  iter->old_spte, new_spte,
					  iter->gfn, iter->level);
}

#define tdp_root_for_each_pte(_iter, _root, _start, _end) \
	for_each_tdp_pte(_iter, _root, _start, _end)

/*
 * Note temporarily blocked private SPTE is considered as valid leaf, although
 * !is_shadow_present_pte() returns true for it, since the target page (which
 * the mapping maps to ) is still there.
 */
#define tdp_root_for_each_leaf_pte(_iter, _root, _start, _end)		\
	tdp_root_for_each_pte(_iter, _root, _start, _end)		\
		if ((!is_shadow_present_pte(_iter.old_spte) &&		\
		     !is_private_zapped_spte(_iter.old_spte)) ||	\
		     !is_last_spte(_iter.old_spte, _iter.level)) {	\
			continue;					\
		} else

#define tdp_mmu_for_each_pte(_iter, _mmu, _private, _start, _end)	\
	for_each_tdp_pte(_iter,						\
		 root_to_sp((_private) ? _mmu->private_root_hpa :	\
				_mmu->root.hpa),			\
		_start, _end)

/*
 * Yield if the MMU lock is contended or this thread needs to return control
 * to the scheduler.
 *
 * If this function should yield and flush is set, it will perform a remote
 * TLB flush before yielding.
 *
 * If this function yields, iter->yielded is set and the caller must skip to
 * the next iteration, where tdp_iter_next() will reset the tdp_iter's walk
 * over the paging structures to allow the iterator to continue its traversal
 * from the paging structure root.
 *
 * Returns true if this function yielded.
 */
static inline bool __must_check tdp_mmu_iter_cond_resched(struct kvm *kvm,
							  struct tdp_iter *iter,
							  bool flush, bool shared)
{
	WARN_ON_ONCE(iter->yielded);

	/* Ensure forward progress has been made before yielding. */
	if (iter->next_last_level_gfn == iter->yielded_gfn)
		return false;

	if (need_resched() || rwlock_needbreak(&kvm->mmu_lock)) {
		if (flush)
			kvm_flush_remote_tlbs(kvm);

		rcu_read_unlock();

		if (shared)
			cond_resched_rwlock_read(&kvm->mmu_lock);
		else
			cond_resched_rwlock_write(&kvm->mmu_lock);

		rcu_read_lock();

		WARN_ON_ONCE(iter->gfn > iter->next_last_level_gfn);

		iter->yielded = true;
	}

	return iter->yielded;
}

static inline gfn_t tdp_mmu_max_gfn_exclusive(void)
{
	/*
	 * Bound TDP MMU walks at host.MAXPHYADDR.  KVM disallows memslots with
	 * a gpa range that would exceed the max gfn, and KVM does not create
	 * MMIO SPTEs for "impossible" gfns, instead sending such accesses down
	 * the slow emulation path every time.
	 */
	return kvm_mmu_max_gfn() + 1;
}

static void __tdp_mmu_zap_root(struct kvm *kvm, struct kvm_mmu_page *root,
			       bool shared, int zap_level)
{
	struct tdp_iter iter;

	gfn_t end = tdp_mmu_max_gfn_exclusive();
	gfn_t start = 0;

	for_each_tdp_pte_min_level(iter, root, zap_level, start, end) {
retry:
		if (tdp_mmu_iter_cond_resched(kvm, &iter, false, shared))
			continue;

		if (!is_shadow_present_pte(iter.old_spte))
			continue;

		if (iter.level > zap_level)
			continue;

		if (!shared)
			tdp_mmu_iter_set_spte(kvm, &iter, SHADOW_NONPRESENT_VALUE);
		else if (tdp_mmu_set_spte_atomic(kvm, &iter, SHADOW_NONPRESENT_VALUE))
			goto retry;
	}
}

static void tdp_mmu_zap_root(struct kvm *kvm, struct kvm_mmu_page *root,
			     bool shared)
{

	/*
	 * The root must have an elevated refcount so that it's reachable via
	 * mmu_notifier callbacks, which allows this path to yield and drop
	 * mmu_lock.  When handling an unmap/release mmu_notifier command, KVM
	 * must drop all references to relevant pages prior to completing the
	 * callback.  Dropping mmu_lock with an unreachable root would result
	 * in zapping SPTEs after a relevant mmu_notifier callback completes
	 * and lead to use-after-free as zapping a SPTE triggers "writeback" of
	 * dirty accessed bits to the SPTE's associated struct page.
	 */
	WARN_ON_ONCE(!refcount_read(&root->tdp_mmu_root_count));

	kvm_lockdep_assert_mmu_lock_held(kvm, shared);

	rcu_read_lock();

	/*
	 * To avoid RCU stalls due to recursively removing huge swaths of SPs,
	 * split the zap into two passes.  On the first pass, zap at the 1gb
	 * level, and then zap top-level SPs on the second pass.  "1gb" is not
	 * arbitrary, as KVM must be able to zap a 1gb shadow page without
	 * inducing a stall to allow in-place replacement with a 1gb hugepage.
	 *
	 * Because zapping a SP recurses on its children, stepping down to
	 * PG_LEVEL_4K in the iterator itself is unnecessary.
	 */
	__tdp_mmu_zap_root(kvm, root, shared, PG_LEVEL_1G);
	__tdp_mmu_zap_root(kvm, root, shared, root->role.level);

	rcu_read_unlock();
}

static int tdp_mmu_restore_private_page(struct kvm *kvm, gfn_t gfn,
					u64 *sptep, u64 old_spte,
					u64 new_spte, int level)
{
	int ret;

	kvm_tdp_mmu_write_spte(sptep, old_spte, new_spte, level);
	ret = static_call(kvm_x86_restore_private_page)(kvm, gfn);
	if (ret == -EPERM && is_shadow_present_pte(old_spte) &&
	    !is_writable_pte(old_spte)) {
		kvm_write_unblock_private_page(kvm, gfn, level);
		ret = 0;
	}

	return 0;
}

static int tdp_mmu_restore_private_pages(struct kvm *kvm,
					 struct kvm_mmu_page *root)
{
	struct tdp_iter iter;
	u64 old_spte, new_spte, *sptep;
	gfn_t end = tdp_mmu_max_gfn_exclusive();
	gfn_t start = 0;
	gfn_t gfn;
	int ret = 0;

	for_each_tdp_pte_min_level(iter, root, PG_LEVEL_4K, start, end) {
		if (tdp_mmu_iter_cond_resched(kvm, &iter, false, false))
			continue;

		if (iter.level > PG_LEVEL_4K)
			continue;

		old_spte = iter.old_spte;
		sptep = iter.sptep;
		gfn = iter.gfn;

		/* Restored sptes should always be writable */
		new_spte = old_spte | PT_WRITABLE_MASK;
		ret = tdp_mmu_restore_private_page(kvm, gfn, sptep,
						   old_spte, new_spte,
						   iter.level);
		if (ret)
			break;
	}

	return ret;
}

int kvm_tdp_mmu_restore_private_pages(struct kvm *kvm)
{
	struct kvm_mmu_page *root;
	int i, ret;

	write_lock(&kvm->mmu_lock);

	for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
		for_each_tdp_mmu_root_yield_safe(kvm, root, i) {
			if (!is_private_sp(root))
				continue;
			ret = tdp_mmu_restore_private_pages(kvm, root);
			if (ret)
				break;
		}
	}
	kvm_flush_remote_tlbs(kvm);

	write_unlock(&kvm->mmu_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kvm_tdp_mmu_restore_private_pages);

bool kvm_tdp_mmu_zap_sp(struct kvm *kvm, struct kvm_mmu_page *sp)
{
	u64 old_spte;
	u64 new_spte;

	/*
	 * This helper intentionally doesn't allow zapping a root shadow page,
	 * which doesn't have a parent page table and thus no associated entry.
	 */
	if (WARN_ON_ONCE(!sp->ptep))
		return false;

	old_spte = kvm_tdp_mmu_read_spte(sp->ptep);
	if (WARN_ON_ONCE(!is_shadow_present_pte(old_spte)))
		return false;

	if (kvm_gfn_shared_mask(kvm) && is_private_sp(sp))
		new_spte = __private_zapped_spte(old_spte);
	else
		new_spte = SHADOW_NONPRESENT_VALUE;

	tdp_mmu_set_spte(kvm, kvm_mmu_page_as_id(sp), sp->ptep, old_spte,
			 new_spte, sp->gfn, sp->role.level + 1);

	return true;
}


static struct kvm_mmu_page *tdp_mmu_alloc_sp_for_split(struct kvm *kvm,
						       struct tdp_iter *iter,
						       bool shared, bool can_yield);

static int tdp_mmu_split_huge_page(struct kvm *kvm, struct tdp_iter *iter,
				   struct kvm_mmu_page *sp, bool shared);

/*
 * If can_yield is true, will release the MMU lock and reschedule if the
 * scheduler needs the CPU or there is contention on the MMU lock. If this
 * function cannot yield, it will not release the MMU lock or reschedule and
 * the caller must ensure it does not supply too large a GFN range, or the
 * operation can cause a soft lockup.
 */
static bool tdp_mmu_zap_leafs(struct kvm *kvm, struct kvm_mmu_page *root,
			      gfn_t start, gfn_t end, bool can_yield, bool flush,
			      enum tdp_zap_private zap_private)
{
	bool is_private = is_private_sp(root);
	struct kvm_mmu_page *split_sp = NULL;
	struct tdp_iter iter;
	u64 new_spte;

	end = min(end, tdp_mmu_max_gfn_exclusive());

	lockdep_assert_held_write(&kvm->mmu_lock);
	WARN_ON_ONCE(zap_private != ZAP_PRIVATE_SKIP && !is_private);

	if (zap_private == ZAP_PRIVATE_SKIP && is_private)
		return flush;

	/*
	 * start and end doesn't have GFN shared bit.  This function zaps
	 * a region including alias.  Adjust shared bit of [start, end) if the
	 * root is shared.
	 */
	start = kvm_gfn_for_root(kvm, root, start);
	end = kvm_gfn_for_root(kvm, root, end);

	rcu_read_lock();

	for_each_tdp_pte_min_level(iter, root, PG_LEVEL_4K, start, end) {
		if (can_yield &&
		    tdp_mmu_iter_cond_resched(kvm, &iter, flush, false)) {
			flush = false;
			continue;
		}

		if (!is_last_spte(iter.old_spte, iter.level))
			continue;

		/*
		 * Skip non-present SPTE, with exception of temporarily
		 * blocked private SPTE, which also needs to be zapped.
		 */
		if (!is_shadow_present_pte(iter.old_spte) &&
		    !is_private_zapped_spte(iter.old_spte))
			continue;

		if (is_private && kvm_gfn_shared_mask(kvm) &&
		    is_large_pte(iter.old_spte)) {
			gfn_t gfn = iter.gfn & ~kvm_gfn_shared_mask(kvm);
			gfn_t mask = KVM_PAGES_PER_HPAGE(iter.level) - 1;
			struct kvm_memory_slot *slot;
			struct kvm_mmu_page *sp;

			slot = gfn_to_memslot(kvm, gfn);
			if (kvm_hugepage_test_mixed(slot, gfn, iter.level) ||
			    (gfn & mask) < start ||
			    end < (gfn & mask) + KVM_PAGES_PER_HPAGE(iter.level)) {
				WARN_ON_ONCE(!can_yield);
				if (split_sp) {
					sp = split_sp;
					split_sp = NULL;
					sp->role = tdp_iter_child_role(&iter);
				} else {
					WARN_ON(iter.yielded);
					if (flush && can_yield) {
						kvm_flush_remote_tlbs(kvm);
						flush = false;
					}
					sp = tdp_mmu_alloc_sp_for_split(kvm, &iter, false,
									can_yield);
					if (iter.yielded) {
						split_sp = sp;
						continue;
					}
				}
				KVM_BUG_ON(!sp, kvm);

				tdp_mmu_init_sp(sp, iter.sptep, iter.gfn);
				if (tdp_mmu_split_huge_page(kvm, &iter, sp, false)) {
					kvm_flush_remote_tlbs(kvm);
					flush = false;
					/* force retry on this gfn. */
					iter.yielded = true;
				} else
					flush = true;
				continue;
			}
		}

		if ((zap_private == ZAP_PRIVATE_SKIP ||
		     zap_private == ZAP_PRIVATE_BLOCK) &&
		    is_private_zapped_spte(iter.old_spte))
			continue;

		if (zap_private == ZAP_PRIVATE_REMOVE)
			new_spte = SHADOW_NONPRESENT_VALUE;
		else
			new_spte = private_zapped_spte(kvm, &iter);
		tdp_mmu_iter_set_spte(kvm, &iter, new_spte);
		flush = true;
	}

	rcu_read_unlock();

	if (split_sp) {
		WARN_ON(!can_yield);
		if (flush) {
			kvm_flush_remote_tlbs(kvm);
			flush = false;
		}

		write_unlock(&kvm->mmu_lock);
		tdp_mmu_free_sp(split_sp);
		write_lock(&kvm->mmu_lock);
	}

	/*
	 * Because this flow zaps _only_ leaf SPTEs, the caller doesn't need
	 * to provide RCU protection as no 'struct kvm_mmu_page' will be freed.
	 */
	return flush;
}

/*
 * Zap leaf SPTEs for the range of gfns, [start, end), for all roots. Returns
 * true if a TLB flush is needed before releasing the MMU lock, i.e. if one or
 * more SPTEs were zapped since the MMU lock was last acquired.
 */
bool kvm_tdp_mmu_zap_leafs(struct kvm *kvm, int as_id, gfn_t start, gfn_t end,
			   bool can_yield, bool flush, enum tdp_zap_private zap_private)
{
	struct kvm_mmu_page *root;

	for_each_tdp_mmu_root_yield_safe(kvm, root, as_id) {
		flush = tdp_mmu_zap_leafs(kvm, root, start, end, can_yield, flush,
					  is_private_sp(root) ? zap_private : ZAP_PRIVATE_SKIP);
	}

	return flush;
}

void kvm_tdp_mmu_zap_all(struct kvm *kvm)
{
	struct kvm_mmu_page *root;
	int i;

	/*
	 * Zap all roots, including invalid roots, as all SPTEs must be dropped
	 * before returning to the caller.  Zap directly even if the root is
	 * also being zapped by a worker.  Walking zapped top-level SPTEs isn't
	 * all that expensive and mmu_lock is already held, which means the
	 * worker has yielded, i.e. flushing the work instead of zapping here
	 * isn't guaranteed to be any faster.
	 *
	 * A TLB flush is unnecessary, KVM zaps everything if and only the VM
	 * is being destroyed or the userspace VMM has exited.  In both cases,
	 * KVM_RUN is unreachable, i.e. no vCPUs will ever service the request.
	 */
	for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
		for_each_tdp_mmu_root_yield_safe(kvm, root, i)
			tdp_mmu_zap_root(kvm, root, false);
	}
}

/*
 * Zap all invalidated roots to ensure all SPTEs are dropped before the "fast
 * zap" completes.
 */
void kvm_tdp_mmu_zap_invalidated_roots(struct kvm *kvm)
{
	flush_workqueue(kvm->arch.tdp_mmu_zap_wq);
}

/*
 * Mark each TDP MMU root as invalid to prevent vCPUs from reusing a root that
 * is about to be zapped, e.g. in response to a memslots update.  The actual
 * zapping is performed asynchronously.  Using a separate workqueue makes it
 * easy to ensure that the destruction is performed before the "fast zap"
 * completes, without keeping a separate list of invalidated roots; the list is
 * effectively the list of work items in the workqueue.
 *
 * Note, the asynchronous worker is gifted the TDP MMU's reference.
 * See kvm_tdp_mmu_get_vcpu_root_hpa().
 */
void kvm_tdp_mmu_invalidate_all_roots(struct kvm *kvm, bool skip_private)
{
	struct kvm_mmu_page *root;

	/*
	 * mmu_lock must be held for write to ensure that a root doesn't become
	 * invalid while there are active readers (invalidating a root while
	 * there are active readers may or may not be problematic in practice,
	 * but it's uncharted territory and not supported).
	 *
	 * Waive the assertion if there are no users of @kvm, i.e. the VM is
	 * being destroyed after all references have been put, or if no vCPUs
	 * have been created (which means there are no roots), i.e. the VM is
	 * being destroyed in an error path of KVM_CREATE_VM.
	 */
	if (IS_ENABLED(CONFIG_PROVE_LOCKING) &&
	    refcount_read(&kvm->users_count) && kvm->created_vcpus)
		lockdep_assert_held_write(&kvm->mmu_lock);

	/*
	 * As above, mmu_lock isn't held when destroying the VM!  There can't
	 * be other references to @kvm, i.e. nothing else can invalidate roots
	 * or be consuming roots, but walking the list of roots does need to be
	 * guarded against roots being deleted by the asynchronous zap worker.
	 */
	rcu_read_lock();

	list_for_each_entry_rcu(root, &kvm->arch.tdp_mmu_roots, link) {
		/*
		 * Skip private root since private page table
		 * is only torn down when VM is destroyed.
		 */
		if (skip_private && is_private_sp(root))
			continue;
		if (!root->role.invalid) {
			root->role.invalid = true;
			tdp_mmu_schedule_zap_root(kvm, root);
		}
	}

	rcu_read_unlock();
}

static int tdp_mmu_iter_step_side(int i, struct tdp_iter *iter)
{
	i++;

	/*
	 * if i = SPTE_ENT_PER_PAGE, tdp_iter_step_side() results
	 * in reading the entry beyond the last entry.
	 */
	if (i < SPTE_ENT_PER_PAGE)
		tdp_iter_step_side(iter);

	return i;
}

static int tdp_mmu_merge_private_spt(struct kvm_vcpu *vcpu,
				     struct kvm_page_fault *fault,
				     struct tdp_iter *iter, u64 new_spte)
{
	u64 *sptep = rcu_dereference(iter->sptep);
	u64 old_spte = iter->old_spte;
	struct kvm_mmu_page *child_sp;
	struct kvm *kvm = vcpu->kvm;
	struct tdp_iter child_iter;
	int level = iter->level;
	gfn_t gfn = iter->gfn;
	tdp_ptep_t child_pt;
	u64 child_spte;
	int ret = 0;
	int i;

	/*
	 * TDX KVM supports only 2MB large page.  It's not supported to merge
	 * 2MB pages into 1GB page at the moment.
	 */
	WARN_ON_ONCE(fault->goal_level != PG_LEVEL_2M);
	WARN_ON_ONCE(iter->level != PG_LEVEL_2M);
	WARN_ON_ONCE(!is_large_pte(new_spte));

	/* Freeze the spte to prevent other threads from working spte. */
	if (!try_cmpxchg64(sptep, &iter->old_spte, REMOVED_SPTE))
		return -EBUSY;

	/*
	 * Step down to the child spte.  Because tdp_iter_next() assumes the
	 * parent spte isn't freezed, do it manually.
	 */
	child_pt = spte_to_child_pt(iter->old_spte, iter->level);
	child_sp = sptep_to_sp(child_pt);
	WARN_ON_ONCE(child_sp->role.level != PG_LEVEL_4K);
	WARN_ON_ONCE(!kvm_mmu_page_role_is_private(child_sp->role));

	/* Don't modify iter as the caller will use iter after this function. */
	child_iter = *iter;
	/* Adjust the target gfn to the head gfn of the large page. */
	child_iter.next_last_level_gfn &= -KVM_PAGES_PER_HPAGE(level);
	tdp_iter_step_down(&child_iter, child_pt);

	/*
	 * All child pages are required to be populated for merging them into a
	 * large page.  Populate all child spte.
	 */
	for (i = 0; i < SPTE_ENT_PER_PAGE; i = tdp_mmu_iter_step_side(i, &child_iter)) {
		int tmp;

		WARN_ON_ONCE(child_iter.level != PG_LEVEL_4K);

		if (is_shadow_present_pte(child_iter.old_spte)) {
			/* TODO: relocate page for huge page. */
			if (WARN_ON_ONCE(spte_to_pfn(child_iter.old_spte) !=
					 spte_to_pfn(new_spte) + i)) {
				if (!ret)
					ret = -EAGAIN;
				continue;
			}
			/*
			 * When SEPT_VE_DISABLE=true and the page state is
			 * pending, this case can happen.  Just resume the vcpu
			 * again with the expectation for other vcpu to accept
			 * this page.
			 */
			if (child_iter.gfn == fault->gfn) {
				if (!ret)
					ret = -EAGAIN;
			}
			continue;
		}

		/* The blocked spte should have same PFN. */
		WARN_ON_ONCE(is_private_zapped_spte(old_spte) &&
			     spte_to_pfn(child_iter.old_spte) != spte_to_pfn(new_spte) + i);
		child_spte = make_huge_page_split_spte(kvm, new_spte, child_sp->role, i);
		/*
		 * Because other thread may have started to operate on this spte
		 * before freezing the parent spte,  Use atomic version to
		 * prevent race.
		 */
		tmp = tdp_mmu_set_spte_atomic(vcpu->kvm, &child_iter, child_spte);
		if (tmp == -EBUSY || tmp == -EAGAIN) {
			/*
			 * There was a race condition.  Populate remaining 4K
			 * spte to resolve fault->gfn to guarantee the forward
			 * progress.
			 */
			if (!ret)
				ret = tmp;
		} else if (tmp) {
			ret = tmp;
			goto out;
		}
	}
	if (ret)
		goto out;

	/* Prevent the Secure-EPT entry from being used. */
	ret = static_call(kvm_x86_zap_private_spte)(kvm, gfn, level);
	if (ret)
		goto out;
	kvm_flush_remote_tlbs_range(kvm, gfn & KVM_HPAGE_GFN_MASK(level),
				    KVM_PAGES_PER_HPAGE(level));

	/* Merge pages into a large page. */
	ret = static_call(kvm_x86_merge_private_spt)(kvm, gfn, level,
						     kvm_mmu_private_spt(child_sp));
	/*
	 * Failed to merge pages because some pages are accepted and some are
	 * pending.  Since the child page was mapped above, let vcpu run.
	 */
	if (ret) {
		if (static_call(kvm_x86_unzap_private_spte)(kvm, gfn, level))
			old_spte = __private_zapped_spte(old_spte);
		goto out;
	}

	/* Unfreeze spte. */
	iter->old_spte = new_spte;
	__kvm_tdp_mmu_write_spte(sptep, new_spte);

	/*
	 * Free unused child sp.  Secure-EPT page was already freed at TDX level
	 * by kvm_x86_merge_private_spt().
	 */
	tdp_unaccount_mmu_page(kvm, child_sp);
	tdp_mmu_free_sp(child_sp);
	return -EAGAIN;

out:
	iter->old_spte = old_spte;
	__kvm_tdp_mmu_write_spte(sptep, old_spte);
	return ret;
}

static int __tdp_mmu_map_handle_target_level(struct kvm_vcpu *vcpu,
					     struct kvm_page_fault *fault,
					     struct tdp_iter *iter, u64 new_spte)
{
	/*
	 * The private page has smaller-size pages.  For example, the child
	 * pages was converted from shared to page, and now it can be mapped as
	 * a large page.  Try to merge small pages into a large page.
	 */
	if (fault->slot &&
	    kvm_gfn_shared_mask(vcpu->kvm) &&
	    iter->level > PG_LEVEL_4K &&
	    kvm_is_private_gpa(vcpu->kvm, fault->addr) &&
	    is_shadow_present_pte(iter->old_spte) &&
	    !is_large_pte(iter->old_spte))
		return tdp_mmu_merge_private_spt(vcpu, fault, iter, new_spte);

	return tdp_mmu_set_spte_atomic(vcpu->kvm, iter, new_spte);
}

/*
 * Installs a last-level SPTE to handle a TDP page fault.
 * (NPT/EPT violation/misconfiguration)
 */
static int tdp_mmu_map_handle_target_level(struct kvm_vcpu *vcpu,
					  struct kvm_page_fault *fault,
					  struct tdp_iter *iter)
{
	struct kvm_mmu_page *sp = sptep_to_sp(rcu_dereference(iter->sptep));
	u64 new_spte;
	int ret = RET_PF_FIXED;
	bool wrprot = false;

	if (WARN_ON_ONCE(sp->role.level != fault->goal_level))
		return RET_PF_RETRY;

	if (unlikely(!fault->slot))
		new_spte = make_mmio_spte(vcpu, iter->gfn, ACC_ALL);
	else {
		unsigned long pte_access = ACC_ALL;
		gfn_t gfn = iter->gfn;

		if (kvm_gfn_shared_mask(vcpu->kvm)) {
			if (fault->is_private)
				gfn |= kvm_gfn_shared_mask(vcpu->kvm);
			else
				/*
				 * TDX shared GPAs are no executable, enforce
				 * this for the SDV.
				 */
				pte_access &= ~ACC_EXEC_MASK;
		}

		wrprot = make_spte(vcpu, sp, fault->slot, pte_access, gfn,
				   fault->pfn, iter->old_spte,
				   fault->prefetch, true, fault->map_writable,
				   &new_spte);
	}

	if (new_spte == iter->old_spte)
		ret = RET_PF_SPURIOUS;
	else if (__tdp_mmu_map_handle_target_level(vcpu, fault, iter, new_spte))
		return RET_PF_RETRY;
	else if (is_shadow_present_pte(iter->old_spte) &&
		 !is_last_spte(iter->old_spte, iter->level))
		kvm_flush_remote_tlbs_gfn(vcpu->kvm, iter->gfn, iter->level);

	/*
	 * If the page fault was caused by a write but the page is write
	 * protected, emulation is needed. If the emulation was skipped,
	 * the vCPU would have the same fault again.
	 */
	if (wrprot) {
		if (fault->write)
			ret = RET_PF_EMULATE;
	}

	/* If a MMIO SPTE is installed, the MMIO will need to be emulated. */
	if (unlikely(is_mmio_spte(vcpu->kvm, new_spte))) {
		vcpu->stat.pf_mmio_spte_created++;
		trace_mark_mmio_spte(rcu_dereference(iter->sptep), iter->gfn,
				     new_spte);
		ret = RET_PF_EMULATE;
	} else {
		trace_kvm_mmu_set_spte(iter->level, iter->gfn,
				       rcu_dereference(iter->sptep));
	}

	return ret;
}

/*
 * tdp_mmu_link_sp - Replace the given spte with an spte pointing to the
 * provided page table.
 *
 * @kvm: kvm instance
 * @iter: a tdp_iter instance currently on the SPTE that should be set
 * @sp: The new TDP page table to install.
 * @shared: This operation is running under the MMU lock in read mode.
 *
 * Returns: 0 if the new page table was installed. Non-0 if the page table
 *          could not be installed (e.g. the atomic compare-exchange failed).
 */
static int tdp_mmu_link_sp(struct kvm *kvm, struct tdp_iter *iter,
			   struct kvm_mmu_page *sp, bool shared)
{
	u64 spte = make_nonleaf_spte(sp->spt, !kvm_ad_enabled());
	int ret = 0;

	if (shared) {
		ret = tdp_mmu_set_spte_atomic(kvm, iter, spte);
		if (ret)
			return ret;
	} else {
		tdp_mmu_iter_set_spte(kvm, iter, spte);
	}

	tdp_account_mmu_page(kvm, sp);

	return 0;
}

static int tdp_mmu_split_huge_page(struct kvm *kvm, struct tdp_iter *iter,
				   struct kvm_mmu_page *sp, bool shared);

/*
 * unzap large page spte: shortened version of tdp_mmu_map_handle_target_level()
 */
static int tdp_mmu_unzap_large_spte(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault,
				    struct tdp_iter *iter)
{
	struct kvm_mmu_page *sp = sptep_to_sp(rcu_dereference(iter->sptep));
	kvm_pfn_t mask = KVM_HPAGE_GFN_MASK(iter->level);
	u64 new_spte;

	KVM_BUG_ON((fault->pfn & mask) != spte_to_pfn(iter->old_spte), vcpu->kvm);
	make_spte(vcpu, sp, fault->slot, ACC_ALL, gpa_to_gfn(fault->addr) & mask,
		  fault->pfn & mask, iter->old_spte, false, true,
		  fault->map_writable, &new_spte);

	if (new_spte == iter->old_spte)
		return RET_PF_SPURIOUS;

	if (tdp_mmu_set_spte_atomic(vcpu->kvm, iter, new_spte))
		return RET_PF_RETRY;
	trace_kvm_mmu_set_spte(iter->level, iter->gfn,
			       rcu_dereference(iter->sptep));
	iter->old_spte = new_spte;
	return RET_PF_CONTINUE;
}

/*
 * Handle a TDP page fault (NPT/EPT violation/misconfiguration) by installing
 * page tables and SPTEs to translate the faulting guest physical address.
 */
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
	struct kvm_mmu *mmu = vcpu->arch.mmu;
	struct kvm *kvm = vcpu->kvm;
	struct tdp_iter iter;
	struct kvm_mmu_page *sp;
	gfn_t raw_gfn;
	bool is_private = fault->is_private && kvm_gfn_shared_mask(kvm);
	int ret = RET_PF_RETRY;

	kvm_mmu_hugepage_adjust(vcpu, fault);

	trace_kvm_mmu_spte_requested(fault);

	rcu_read_lock();

	raw_gfn = gpa_to_gfn(fault->addr);

	if (!fault->nonleaf &&
	    (is_error_noslot_pfn(fault->pfn) ||
	    !kvm_pfn_to_refcounted_page(fault->pfn))) {
		if (is_private) {
			rcu_read_unlock();
			return -EFAULT;
		}
	}

	tdp_mmu_for_each_pte(iter, mmu, is_private, raw_gfn, raw_gfn + 1) {
		int r;

		KVM_BUG_ON(is_private_sptep(iter.sptep) != is_private, vcpu->kvm);
		if (fault->nx_huge_page_workaround_enabled ||
		    kvm_gfn_shared_mask(vcpu->kvm))
			disallowed_hugepage_adjust(fault, iter.old_spte, iter.level);

		/*
		 * If SPTE has been frozen by another thread, just give up and
		 * retry, avoiding unnecessary page table allocation and free.
		 */
		if (is_removed_spte(iter.old_spte))
			goto retry;

		if (iter.level == fault->goal_level)
			goto map_target_level;

		/* Step down into the lower level page table if it exists. */
		if (is_shadow_present_pte(iter.old_spte) &&
		    !is_large_pte(iter.old_spte))
			continue;

		if (is_private_zapped_spte(iter.old_spte) &&
		    is_large_pte(iter.old_spte)) {
			if (tdp_mmu_unzap_large_spte(vcpu, fault, &iter) !=
			    RET_PF_CONTINUE)
				break;
		}

		/*
		 * The SPTE is either non-present or points to a huge page that
		 * needs to be split.
		 */
		sp = tdp_mmu_alloc_sp(vcpu, tdp_iter_child_role(&iter));
		tdp_mmu_init_sp(sp, iter.sptep, iter.gfn);

		sp->nx_huge_page_disallowed = fault->huge_page_disallowed;

		if (is_shadow_present_pte(iter.old_spte))
			r = tdp_mmu_split_huge_page(kvm, &iter, sp, true);
		else
			r = tdp_mmu_link_sp(kvm, &iter, sp, true);

		/*
		 * Force the guest to retry if installing an upper level SPTE
		 * failed, e.g. because a different task modified the SPTE.
		 */
		if (r) {
			tdp_mmu_free_sp(sp);
			goto retry;
		}

		if (fault->huge_page_disallowed &&
		    fault->req_level >= iter.level) {
			spin_lock(&kvm->arch.tdp_mmu_pages_lock);
			if (sp->nx_huge_page_disallowed)
				track_possible_nx_huge_page(kvm, sp);
			spin_unlock(&kvm->arch.tdp_mmu_pages_lock);
		}
	}

	/*
	 * The walk aborted before reaching the target level, e.g. because the
	 * iterator detected an upper level SPTE was frozen during traversal.
	 */
	WARN_ON_ONCE(iter.level == fault->goal_level);
	goto retry;

map_target_level:
	if (!fault->nonleaf)
		ret = tdp_mmu_map_handle_target_level(vcpu, fault, &iter);
	else
		ret = RET_PF_FIXED;
retry:
	rcu_read_unlock();
	return ret;
}

/* Used by mmu notifier via kvm_unmap_gfn_range() */
bool kvm_tdp_mmu_unmap_gfn_range(struct kvm *kvm, struct kvm_gfn_range *range,
				 bool flush)
{
	enum tdp_zap_private zap_private = ZAP_PRIVATE_SKIP;

	if (kvm_gfn_shared_mask(kvm)) {
		if (!range->only_private && !range->only_shared) {
			/* memory attributes change */
			if (range->arg.attributes & KVM_MEMORY_ATTRIBUTE_PRIVATE)
				zap_private = ZAP_PRIVATE_SKIP;
			else
				zap_private = ZAP_PRIVATE_BLOCK;
		} else if (range->only_private && !range->only_shared) {
			/* gmem invalidation */
			zap_private = ZAP_PRIVATE_REMOVE;
		} else if (range->only_private && range->only_shared) {
			/* memory slot deletion or movement */
			zap_private = ZAP_PRIVATE_REMOVE;
		} else {
			/*
			 * !range->only_private && range->only_shared
			 * mmu notifier
			 */
			zap_private = ZAP_PRIVATE_SKIP;
		}
	}

	return kvm_tdp_mmu_zap_leafs(kvm, range->slot->as_id, range->start,
				     range->end, range->may_block, flush,
				     zap_private);
}

typedef bool (*tdp_handler_t)(struct kvm *kvm, struct tdp_iter *iter,
			      struct kvm_gfn_range *range);

static __always_inline bool kvm_tdp_mmu_handle_gfn(struct kvm *kvm,
						   struct kvm_gfn_range *range,
						   tdp_handler_t handler)
{
	struct kvm_mmu_page *root;
	struct tdp_iter iter;
	bool ret = false;

	/*
	 * Don't support rescheduling, none of the MMU notifiers that funnel
	 * into this helper allow blocking; it'd be dead, wasteful code.
	 */
	for_each_tdp_mmu_root(kvm, root, range->slot->as_id) {
		gfn_t start;
		gfn_t end;

		/*
		 * For now, operation on private GPA, e.g. dirty page logging,
		 * isn't supported yet.
		 */
		if (is_private_sp(root))
			continue;

		rcu_read_lock();

		/*
		 * For TDX shared mapping, set GFN shared bit to the range,
		 * so the handler() doesn't need to set it, to avoid duplicated
		 * code in multiple handler()s.
		 */
		start = kvm_gfn_to_shared(kvm, range->start);
		end = kvm_gfn_to_shared(kvm, range->end);

		tdp_root_for_each_leaf_pte(iter, root, start, end)
			ret |= handler(kvm, &iter, range);

		rcu_read_unlock();
	}

	return ret;
}

/*
 * Mark the SPTEs range of GFNs [start, end) unaccessed and return non-zero
 * if any of the GFNs in the range have been accessed.
 *
 * No need to mark the corresponding PFN as accessed as this call is coming
 * from the clear_young() or clear_flush_young() notifier, which uses the
 * return value to determine if the page has been accessed.
 */
static bool age_gfn_range(struct kvm *kvm, struct tdp_iter *iter,
			  struct kvm_gfn_range *range)
{
	u64 new_spte;

	/* If we have a non-accessed entry we don't need to change the pte. */
	if (!is_accessed_spte(iter->old_spte))
		return false;

	if (spte_ad_enabled(iter->old_spte)) {
		iter->old_spte = tdp_mmu_clear_spte_bits(iter->sptep,
							 iter->old_spte,
							 shadow_accessed_mask,
							 iter->level);
		new_spte = iter->old_spte & ~shadow_accessed_mask;
	} else {
		/*
		 * Capture the dirty status of the page, so that it doesn't get
		 * lost when the SPTE is marked for access tracking.
		 */
		if (is_writable_pte(iter->old_spte))
			kvm_set_pfn_dirty(spte_to_pfn(iter->old_spte));

		new_spte = mark_spte_for_access_track(iter->old_spte);
		iter->old_spte = kvm_tdp_mmu_write_spte(iter->sptep,
							iter->old_spte, new_spte,
							iter->level);
	}

	trace_kvm_tdp_mmu_spte_changed(iter->as_id, iter->gfn, iter->level,
				       iter->old_spte, new_spte);
	return true;
}

bool kvm_tdp_mmu_age_gfn_range(struct kvm *kvm, struct kvm_gfn_range *range)
{
	return kvm_tdp_mmu_handle_gfn(kvm, range, age_gfn_range);
}

static bool test_age_gfn(struct kvm *kvm, struct tdp_iter *iter,
			 struct kvm_gfn_range *range)
{
	return is_accessed_spte(iter->old_spte);
}

bool kvm_tdp_mmu_test_age_gfn(struct kvm *kvm, struct kvm_gfn_range *range)
{
	return kvm_tdp_mmu_handle_gfn(kvm, range, test_age_gfn);
}

static bool set_spte_gfn(struct kvm *kvm, struct tdp_iter *iter,
			 struct kvm_gfn_range *range)
{
	u64 new_spte;

	/* Huge pages aren't expected to be modified without first being zapped. */
	WARN_ON_ONCE(pte_huge(range->arg.pte) || range->start + 1 != range->end);

	if (iter->level != PG_LEVEL_4K ||
	    !is_shadow_present_pte(iter->old_spte))
		return false;

	/*
	 * Note, when changing a read-only SPTE, it's not strictly necessary to
	 * zero the SPTE before setting the new PFN, but doing so preserves the
	 * invariant that the PFN of a present * leaf SPTE can never change.
	 * See handle_changed_spte().
	 */
	tdp_mmu_iter_set_spte(kvm, iter, private_zapped_spte(kvm, iter));

	if (!pte_write(range->arg.pte)) {
		new_spte = kvm_mmu_changed_pte_notifier_make_spte(iter->old_spte,
								  pte_pfn(range->arg.pte));

		tdp_mmu_iter_set_spte(kvm, iter, new_spte);
	}

	return true;
}

/*
 * Handle the changed_pte MMU notifier for the TDP MMU.
 * data is a pointer to the new pte_t mapping the HVA specified by the MMU
 * notifier.
 * Returns non-zero if a flush is needed before releasing the MMU lock.
 */
bool kvm_tdp_mmu_set_spte_gfn(struct kvm *kvm, struct kvm_gfn_range *range)
{
	/*
	 * No need to handle the remote TLB flush under RCU protection, the
	 * target SPTE _must_ be a leaf SPTE, i.e. cannot result in freeing a
	 * shadow page. See the WARN on pfn_changed in handle_changed_spte().
	 */
	return kvm_tdp_mmu_handle_gfn(kvm, range, set_spte_gfn);
}

/*
 * Remove write access from all SPTEs at or above min_level that map GFNs
 * [start, end). Returns true if an SPTE has been changed and the TLBs need to
 * be flushed.
 */
static bool wrprot_gfn_range(struct kvm *kvm, struct kvm_mmu_page *root,
			     gfn_t start, gfn_t end, int min_level)
{
	struct tdp_iter iter;
	u64 new_spte;
	bool spte_set = false;

	rcu_read_lock();

	BUG_ON(min_level > KVM_MAX_HUGEPAGE_LEVEL);

	for_each_tdp_pte_min_level(iter, root, min_level, start, end) {
retry:
		if (tdp_mmu_iter_cond_resched(kvm, &iter, false, true))
			continue;

		if (!is_shadow_present_pte(iter.old_spte) ||
		    !is_last_spte(iter.old_spte, iter.level) ||
		    !(iter.old_spte & PT_WRITABLE_MASK))
			continue;

		new_spte = iter.old_spte & ~PT_WRITABLE_MASK;

		if (tdp_mmu_set_spte_atomic(kvm, &iter, new_spte))
			goto retry;

		spte_set = true;
	}

	rcu_read_unlock();
	return spte_set;
}

/*
 * Remove write access from all the SPTEs mapping GFNs in the memslot. Will
 * only affect leaf SPTEs down to min_level.
 * Returns true if an SPTE has been changed and the TLBs need to be flushed.
 */
bool kvm_tdp_mmu_wrprot_slot(struct kvm *kvm,
			     const struct kvm_memory_slot *slot, int min_level)
{
	struct kvm_mmu_page *root;
	bool spte_set = false;

	lockdep_assert_held_read(&kvm->mmu_lock);

	/*
	 * Because first TDX generation doesn't support write protecting private
	 * mappings and kvm_arch_dirty_log_supported(kvm) = false, it's a bug
	 * to reach here for guest TD.
	 */
	if (WARN_ON_ONCE(!kvm_arch_dirty_log_supported(kvm)))
		return false;

	for_each_valid_tdp_mmu_root_yield_safe(kvm, root, slot->as_id, true)
		spte_set |= wrprot_gfn_range(kvm, root, slot->base_gfn,
			     slot->base_gfn + slot->npages, min_level);

	return spte_set;
}

static struct kvm_mmu_page *__tdp_mmu_alloc_sp_for_split(struct kvm *kvm, gfp_t gfp,
							 union kvm_mmu_page_role role,
							 bool can_yield)
{
	struct kvm_mmu_page *sp;

	gfp |= __GFP_ZERO;

	sp = kmem_cache_alloc(mmu_page_header_cache, gfp);
	if (!sp)
		return NULL;

	sp->role = role;
	sp->spt = (void *)__get_free_page(gfp);
	if (kvm_mmu_page_role_is_private(role)) {
		if (kvm_alloc_private_spt_for_split(kvm, sp, gfp, can_yield)) {
			free_page((unsigned long)sp->spt);
			sp->spt = NULL;
		}
	}
	if (!sp->spt) {
		kmem_cache_free(mmu_page_header_cache, sp);
		return NULL;
	}

	return sp;
}

static struct kvm_mmu_page *tdp_mmu_alloc_sp_for_split(struct kvm *kvm,
						       struct tdp_iter *iter,
						       bool shared,
						       bool can_yield)
{
	union kvm_mmu_page_role role = tdp_iter_child_role(iter);
	struct kvm_mmu_page *sp;

	KVM_BUG_ON(kvm_mmu_page_role_is_private(role) !=
		   is_private_sptep(iter->sptep), kvm);

	/*
	 * Since we are allocating while under the MMU lock we have to be
	 * careful about GFP flags. Use GFP_NOWAIT to avoid blocking on direct
	 * reclaim and to avoid making any filesystem callbacks (which can end
	 * up invoking KVM MMU notifiers, resulting in a deadlock).
	 *
	 * If this allocation fails we drop the lock and retry with reclaim
	 * allowed.
	 */
	sp = __tdp_mmu_alloc_sp_for_split(kvm, GFP_NOWAIT | __GFP_ACCOUNT, role, can_yield);
	if (sp || !can_yield)
		return sp;

	rcu_read_unlock();

	if (shared)
		read_unlock(&kvm->mmu_lock);
	else
		write_unlock(&kvm->mmu_lock);

	iter->yielded = true;
	sp = __tdp_mmu_alloc_sp_for_split(kvm, GFP_KERNEL_ACCOUNT, role, can_yield);

	if (shared)
		read_lock(&kvm->mmu_lock);
	else
		write_lock(&kvm->mmu_lock);

	rcu_read_lock();

	return sp;
}

/* Note, the caller is responsible for initializing @sp. */
static int tdp_mmu_split_huge_page(struct kvm *kvm, struct tdp_iter *iter,
				   struct kvm_mmu_page *sp, bool shared)
{
	const u64 huge_spte = iter->old_spte;
	const int level = iter->level;
	int ret, i;

	/*
	 * No need for atomics when writing to sp->spt since the page table has
	 * not been linked in yet and thus is not reachable from any other CPU.
	 */
	for (i = 0; i < SPTE_ENT_PER_PAGE; i++)
		sp->spt[i] = make_huge_page_split_spte(kvm, huge_spte, sp->role, i);

	/*
	 * Replace the huge spte with a pointer to the populated lower level
	 * page table. Since we are making this change without a TLB flush vCPUs
	 * will see a mix of the split mappings and the original huge mapping,
	 * depending on what's currently in their TLB. This is fine from a
	 * correctness standpoint since the translation will be the same either
	 * way.
	 */
	ret = tdp_mmu_link_sp(kvm, iter, sp, shared);
	if (ret)
		goto out;

	/*
	 * tdp_mmu_link_sp_atomic() will handle subtracting the huge page we
	 * are overwriting from the page stats. But we have to manually update
	 * the page stats with the new present child pages.
	 */
	kvm_update_page_stats(kvm, level - 1, SPTE_ENT_PER_PAGE);

out:
	trace_kvm_mmu_split_huge_page(iter->gfn, huge_spte, level, ret);
	return ret;
}

static int tdp_mmu_split_huge_pages_root(struct kvm *kvm,
					 struct kvm_mmu_page *root,
					 gfn_t start, gfn_t end,
					 int target_level, bool shared)
{
	struct kvm_mmu_page *sp = NULL;
	struct tdp_iter iter;
	int ret = 0;

	rcu_read_lock();

	/*
	 * Traverse the page table splitting all huge pages above the target
	 * level into one lower level. For example, if we encounter a 1GB page
	 * we split it into 512 2MB pages.
	 *
	 * Since the TDP iterator uses a pre-order traversal, we are guaranteed
	 * to visit an SPTE before ever visiting its children, which means we
	 * will correctly recursively split huge pages that are more than one
	 * level above the target level (e.g. splitting a 1GB to 512 2MB pages,
	 * and then splitting each of those to 512 4KB pages).
	 */
	for_each_tdp_pte_min_level(iter, root, target_level + 1, start, end) {
retry:
		if (tdp_mmu_iter_cond_resched(kvm, &iter, false, shared))
			continue;

		if (!is_shadow_present_pte(iter.old_spte) || !is_large_pte(iter.old_spte))
			continue;

		if (!sp) {
			sp = tdp_mmu_alloc_sp_for_split(kvm, &iter, shared, true);
			if (!sp) {
				ret = -ENOMEM;
				trace_kvm_mmu_split_huge_page(iter.gfn,
							      iter.old_spte,
							      iter.level, ret);
				break;
			}

			if (iter.yielded)
				continue;
		}

		tdp_mmu_init_sp(sp, iter.sptep, iter.gfn);

		if (tdp_mmu_split_huge_page(kvm, &iter, sp, shared))
			goto retry;

		sp = NULL;
	}

	rcu_read_unlock();

	/*
	 * It's possible to exit the loop having never used the last sp if, for
	 * example, a vCPU doing HugePage NX splitting wins the race and
	 * installs its own sp in place of the last sp we tried to split.
	 */
	if (sp)
		tdp_mmu_free_sp(sp);

	return ret;
}


/*
 * Try to split all huge pages mapped by the TDP MMU down to the target level.
 */
void kvm_tdp_mmu_try_split_huge_pages(struct kvm *kvm,
				      const struct kvm_memory_slot *slot,
				      gfn_t start, gfn_t end,
				      int target_level, bool shared)
{
	struct kvm_mmu_page *root;
	int r = 0;

	kvm_lockdep_assert_mmu_lock_held(kvm, shared);

	for_each_valid_tdp_mmu_root_yield_safe(kvm, root, slot->as_id, shared) {
		r = tdp_mmu_split_huge_pages_root(kvm, root, start, end, target_level, shared);
		if (r) {
			kvm_tdp_mmu_put_root(kvm, root, shared);
			break;
		}
	}
}

/*
 * Clear the dirty status of all the SPTEs mapping GFNs in the memslot. If
 * AD bits are enabled, this will involve clearing the dirty bit on each SPTE.
 * If AD bits are not enabled, this will require clearing the writable bit on
 * each SPTE. Returns true if an SPTE has been changed and the TLBs need to
 * be flushed.
 */
static bool clear_dirty_gfn_range(struct kvm *kvm, struct kvm_mmu_page *root,
			   gfn_t start, gfn_t end)
{
	u64 dbit = kvm_ad_enabled() ? shadow_dirty_mask : PT_WRITABLE_MASK;
	struct tdp_iter iter;
	bool spte_set = false;

	rcu_read_lock();

	tdp_root_for_each_leaf_pte(iter, root, start, end) {
retry:
		if (tdp_mmu_iter_cond_resched(kvm, &iter, false, true))
			continue;

		if (!is_shadow_present_pte(iter.old_spte))
			continue;

		KVM_MMU_WARN_ON(kvm_ad_enabled() &&
				spte_ad_need_write_protect(iter.old_spte));

		if (!(iter.old_spte & dbit))
			continue;

		if (tdp_mmu_set_spte_atomic(kvm, &iter, iter.old_spte & ~dbit))
			goto retry;

		spte_set = true;
	}

	rcu_read_unlock();
	return spte_set;
}

/*
 * Clear the dirty status of all the SPTEs mapping GFNs in the memslot. If
 * AD bits are enabled, this will involve clearing the dirty bit on each SPTE.
 * If AD bits are not enabled, this will require clearing the writable bit on
 * each SPTE. Returns true if an SPTE has been changed and the TLBs need to
 * be flushed.
 */
bool kvm_tdp_mmu_clear_dirty_slot(struct kvm *kvm,
				  const struct kvm_memory_slot *slot)
{
	struct kvm_mmu_page *root;
	bool spte_set = false;

	lockdep_assert_held_read(&kvm->mmu_lock);

	/*
	 * First TDX generation doesn't support clearing dirty bit,
	 * since there's no secure EPT API to support it.  It is a
	 * bug to reach here for TDX guest.
	 */
	if (WARN_ON_ONCE(!kvm_arch_dirty_log_supported(kvm)))
		return false;

	for_each_valid_tdp_mmu_root_yield_safe(kvm, root, slot->as_id, true)
		spte_set |= clear_dirty_gfn_range(kvm, root, slot->base_gfn,
				slot->base_gfn + slot->npages);

	return spte_set;
}

/*
 * Clears the dirty status of all the 4k SPTEs mapping GFNs for which a bit is
 * set in mask, starting at gfn. The given memslot is expected to contain all
 * the GFNs represented by set bits in the mask. If AD bits are enabled,
 * clearing the dirty status will involve clearing the dirty bit on each SPTE
 * or, if AD bits are not enabled, clearing the writable bit on each SPTE.
 */
static void clear_dirty_pt_masked(struct kvm *kvm, struct kvm_mmu_page *root,
				  gfn_t gfn, unsigned long mask, bool wrprot)
{
	u64 dbit = (wrprot || !kvm_ad_enabled()) ? PT_WRITABLE_MASK :
						   shadow_dirty_mask;
	struct tdp_iter iter;

	lockdep_assert_held_write(&kvm->mmu_lock);

	rcu_read_lock();

	tdp_root_for_each_leaf_pte(iter, root, gfn + __ffs(mask),
				    gfn + BITS_PER_LONG) {
		if (!mask)
			break;

		KVM_MMU_WARN_ON(kvm_ad_enabled() &&
				spte_ad_need_write_protect(iter.old_spte));

		if (iter.level > PG_LEVEL_4K ||
		    !(mask & (1UL << (iter.gfn - gfn))))
			continue;

		mask &= ~(1UL << (iter.gfn - gfn));

		if (!(iter.old_spte & dbit))
			continue;

		if (is_private_sptep(iter.sptep))
			static_call(kvm_x86_write_block_private_pages)(kvm,
							(gfn_t *)&iter.gfn, 1);

		iter.old_spte = tdp_mmu_clear_spte_bits(iter.sptep,
							iter.old_spte, dbit,
							iter.level);

		trace_kvm_tdp_mmu_spte_changed(iter.as_id, iter.gfn, iter.level,
					       iter.old_spte,
					       iter.old_spte & ~dbit);
		kvm_set_pfn_dirty(spte_to_pfn(iter.old_spte));
	}

	rcu_read_unlock();
}

/*
 * Clears the dirty status of all the 4k SPTEs mapping GFNs for which a bit is
 * set in mask, starting at gfn. The given memslot is expected to contain all
 * the GFNs represented by set bits in the mask. If AD bits are enabled,
 * clearing the dirty status will involve clearing the dirty bit on each SPTE
 * or, if AD bits are not enabled, clearing the writable bit on each SPTE.
 */
void kvm_tdp_mmu_clear_dirty_pt_masked(struct kvm *kvm,
				       struct kvm_memory_slot *slot,
				       gfn_t gfn, unsigned long mask,
				       bool wrprot)
{
	struct kvm_mmu_page *root;

	/*
	 * First TDX generation doesn't support clearing dirty bit,
	 * since there's no secure EPT API to support it.  For now silently
	 * ignore KVM_CLEAR_DIRTY_LOG.
	 */
	if (!kvm_arch_dirty_log_supported(kvm))
		return;

	for_each_tdp_mmu_root(kvm, root, slot->as_id)
		clear_dirty_pt_masked(kvm, root, gfn, mask, wrprot);
}

static void zap_collapsible_spte_range(struct kvm *kvm,
				       struct kvm_mmu_page *root,
				       const struct kvm_memory_slot *slot)
{
	gfn_t start = slot->base_gfn;
	gfn_t end = start + slot->npages;
	struct tdp_iter iter;
	int max_mapping_level;

	rcu_read_lock();

	for_each_tdp_pte_min_level(iter, root, PG_LEVEL_2M, start, end) {
retry:
		if (tdp_mmu_iter_cond_resched(kvm, &iter, false, true))
			continue;

		if (iter.level > KVM_MAX_HUGEPAGE_LEVEL ||
		    !is_shadow_present_pte(iter.old_spte))
			continue;

		/*
		 * Don't zap leaf SPTEs, if a leaf SPTE could be replaced with
		 * a large page size, then its parent would have been zapped
		 * instead of stepping down.
		 */
		if (is_last_spte(iter.old_spte, iter.level))
			continue;

		/*
		 * If iter.gfn resides outside of the slot, i.e. the page for
		 * the current level overlaps but is not contained by the slot,
		 * then the SPTE can't be made huge.  More importantly, trying
		 * to query that info from slot->arch.lpage_info will cause an
		 * out-of-bounds access.
		 */
		if (iter.gfn < start || iter.gfn >= end)
			continue;

		max_mapping_level = kvm_mmu_max_mapping_level(kvm, slot,
							      iter.gfn, PG_LEVEL_NUM, false);
		if (max_mapping_level < iter.level)
			continue;

		/* Note, a successful atomic zap also does a remote TLB flush. */
		if (tdp_mmu_zap_spte_atomic(kvm, &iter))
			goto retry;
	}

	rcu_read_unlock();
}

/*
 * Zap non-leaf SPTEs (and free their associated page tables) which could
 * be replaced by huge pages, for GFNs within the slot.
 */
void kvm_tdp_mmu_zap_collapsible_sptes(struct kvm *kvm,
				       const struct kvm_memory_slot *slot)
{
	struct kvm_mmu_page *root;

	lockdep_assert_held_read(&kvm->mmu_lock);

	for_each_valid_tdp_mmu_root_yield_safe(kvm, root, slot->as_id, true)
		zap_collapsible_spte_range(kvm, root, slot);
}

/*
 * Removes write access on the last level SPTE mapping this GFN and unsets the
 * MMU-writable bit to ensure future writes continue to be intercepted.
 * Returns true if an SPTE was set and a TLB flush is needed.
 */
static bool write_protect_gfn(struct kvm *kvm, struct kvm_mmu_page *root,
			      gfn_t gfn, int min_level)
{
	struct tdp_iter iter;
	u64 new_spte;
	bool spte_set = false;

	BUG_ON(min_level > KVM_MAX_HUGEPAGE_LEVEL);

	rcu_read_lock();

	for_each_tdp_pte_min_level(iter, root, min_level, gfn, gfn + 1) {
		if (!is_shadow_present_pte(iter.old_spte) ||
		    !is_last_spte(iter.old_spte, iter.level))
			continue;

		new_spte = iter.old_spte &
			~(PT_WRITABLE_MASK | shadow_mmu_writable_mask);

		if (new_spte == iter.old_spte)
			break;

		tdp_mmu_iter_set_spte(kvm, &iter, new_spte);
		spte_set = true;
	}

	rcu_read_unlock();

	return spte_set;
}

/*
 * Removes write access on the last level SPTE mapping this GFN and unsets the
 * MMU-writable bit to ensure future writes continue to be intercepted.
 * Returns true if an SPTE was set and a TLB flush is needed.
 */
bool kvm_tdp_mmu_write_protect_gfn(struct kvm *kvm,
				   struct kvm_memory_slot *slot, gfn_t gfn,
				   int min_level)
{
	struct kvm_mmu_page *root;
	bool spte_set = false;

	lockdep_assert_held_write(&kvm->mmu_lock);

	/*
	 * First TDX generation doesn't support write protecting private
	 * mappings, silently ignore the request.  KVM_GET_DIRTY_LOG etc
	 * can reach here, no warning.
	 */
	if (!kvm_arch_dirty_log_supported(kvm))
		return false;

	for_each_tdp_mmu_root(kvm, root, slot->as_id)
		spte_set |= write_protect_gfn(kvm, root, gfn, min_level);

	return spte_set;
}

/*
 * Return the level of the lowest level SPTE added to sptes.
 * That SPTE may be non-present.
 *
 * Must be called between kvm_tdp_mmu_walk_lockless_{begin,end}.
 */
int kvm_tdp_mmu_get_walk(struct kvm_vcpu *vcpu, u64 addr, u64 *sptes,
			 int *root_level)
{
	struct tdp_iter iter;
	struct kvm_mmu *mmu = vcpu->arch.mmu;
	gfn_t gfn = addr >> PAGE_SHIFT;
	int leaf = -1;

	*root_level = vcpu->arch.mmu->root_role.level;

	/*
	 * mmio page fault isn't supported for protected guest because
	 * instructions in protected guest memory can't be parsed by VMM.
	 */
	if (WARN_ON_ONCE(kvm_gfn_shared_mask(vcpu->kvm)))
		return leaf;

	tdp_mmu_for_each_pte(iter, mmu, false, gfn, gfn + 1) {
		leaf = iter.level;
		sptes[leaf] = iter.old_spte;
	}

	return leaf;
}

/*
 * Returns the last level spte pointer of the shadow page walk for the given
 * gpa, and sets *spte to the spte value. This spte may be non-preset. If no
 * walk could be performed, returns NULL and *spte does not contain valid data.
 *
 * Contract:
 *  - Must be called between kvm_tdp_mmu_walk_lockless_{begin,end}.
 *  - The returned sptep must not be used after kvm_tdp_mmu_walk_lockless_end.
 *
 * WARNING: This function is only intended to be called during fast_page_fault.
 */

static u64 *kvm_tdp_mmu_fast_get_last_sptep(struct kvm_vcpu *vcpu,
					    bool is_private, gfn_t gfn,
					    u64 *spte)
{
	struct tdp_iter iter;
	struct kvm_mmu *mmu = vcpu->arch.mmu;
	tdp_ptep_t sptep = NULL;

	tdp_mmu_for_each_pte(iter, mmu, is_private, gfn, gfn + 1) {
		*spte = iter.old_spte;
		sptep = iter.sptep;
	}

	/*
	 * Perform the rcu_dereference to get the raw spte pointer value since
	 * we are passing it up to fast_page_fault, which is shared with the
	 * legacy MMU and thus does not retain the TDP MMU-specific __rcu
	 * annotation.
	 *
	 * This is safe since fast_page_fault obeys the contracts of this
	 * function as well as all TDP MMU contracts around modifying SPTEs
	 * outside of mmu_lock.
	 */
	return rcu_dereference(sptep);
}

u64 *kvm_tdp_mmu_fast_pf_get_last_sptep(struct kvm_vcpu *vcpu, u64 addr,
					u64 *spte)
{
	bool is_private = kvm_is_private_gpa(vcpu->kvm, addr);
	gfn_t gfn = gpa_to_gfn(addr) & ~kvm_gfn_shared_mask(vcpu->kvm);

	return kvm_tdp_mmu_fast_get_last_sptep(vcpu, is_private, gfn, spte);
}

int kvm_tdp_mmu_import_private_pages(struct kvm_vcpu *vcpu,
				     gfn_t *gfns,
				     uint64_t *sptes,
				     uint64_t npages,
				     void *first_time_import_bitmap,
				     void *opaque)
{
	uint64_t i, old_spte, new_spte;
	uint64_t *sptep = NULL;
	gfn_t gfn;
	int ret;

	bitmap_zero(first_time_import_bitmap, npages);

	kvm_tdp_mmu_walk_lockless_begin();
	/*
	 * Check if all the sptes are ready to be updated.If yes, frozen the
	 * spteps till import is done.
	 */
	for (i = 0; i < npages; i++) {
		if (gfns[i] == INVALID_GFN)
			continue;

		gfn = gfns[i];
		if (sptes[i])
			new_spte = sptes[i] | SPTE_MMU_PRESENT_MASK;
		else
			new_spte = 0;
		sptep = kvm_tdp_mmu_fast_get_last_sptep(vcpu, true, gfn, &old_spte);

		/* The spte has been freezon */
		if (is_removed_spte(old_spte)) {
			ret = -EBUSY;
			pr_err("%s: gfn=%llx is frozen\n", __func__, gfn);
			goto out;
		}

		if (!try_cmpxchg64(sptep, &old_spte, new_spte)) {
			ret = -EBUSY;
			pr_err("%s: septp of gfn=%llx is changed\n", __func__, gfn);
			goto out;
		}

		if (first_time_import_bitmap &&
		    !is_shadow_present_pte(old_spte))
			set_bit_le(i, first_time_import_bitmap);
	}

	ret = static_call(kvm_x86_import_private_pages)(vcpu->kvm,
							sptes, npages, opaque);
out:
	kvm_tdp_mmu_walk_lockless_end();
	return ret;
}

#ifdef CONFIG_INTEL_TDX_HOST
int kvm_tdp_mmu_is_page_private(struct kvm *kvm, struct kvm_memory_slot *memslot,
				gfn_t gfn, bool *is_private)
{
	int ret = -EINVAL;
	struct tdp_iter iter;
	struct kvm_mmu_page *root;

	rcu_read_lock();

	for_each_tdp_mmu_root(kvm, root, memslot->as_id) {
		if (root->role.invalid)
			continue;

		if (!refcount_read(&root->tdp_mmu_root_count))
			continue;

		if (root->role.guest_mode)
			continue;

		if (is_private_sp(root))
			gfn = kvm_gfn_to_private(kvm, gfn);
		else
			gfn = kvm_gfn_to_shared(kvm, gfn);

		tdp_root_for_each_pte(iter, root, gfn, gfn + 1) {
			if (!is_shadow_present_pte(iter.old_spte))
				continue;

			if (!is_last_spte(iter.old_spte, iter.level))
				continue;

			*is_private = is_private_sp(root);
			ret = 0;
			goto exit;
		}
	}
 exit:
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(kvm_tdp_mmu_is_page_private);
#endif
