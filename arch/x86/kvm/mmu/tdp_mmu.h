// SPDX-License-Identifier: GPL-2.0

#ifndef __KVM_X86_MMU_TDP_MMU_H
#define __KVM_X86_MMU_TDP_MMU_H

#include <linux/kvm_host.h>

#include "spte.h"

int kvm_mmu_init_tdp_mmu(struct kvm *kvm);
void kvm_mmu_uninit_tdp_mmu(struct kvm *kvm);

hpa_t kvm_tdp_mmu_get_vcpu_root_hpa(struct kvm_vcpu *vcpu, bool private);
hpa_t kvm_tdp_mmu_get_vcpu_root_hpa_no_alloc(struct kvm_vcpu *vcpu, bool private);
hpa_t kvm_tdp_mmu_move_private_pages_from(struct kvm_vcpu *vcpu,
					  struct kvm_vcpu *src_vcpu);

__must_check static inline bool kvm_tdp_mmu_get_root(struct kvm_mmu_page *root)
{
	return refcount_inc_not_zero(&root->tdp_mmu_root_count);
}

void kvm_tdp_mmu_put_root(struct kvm *kvm, struct kvm_mmu_page *root,
			  bool shared);

enum tdp_zap_private {
	ZAP_PRIVATE_SKIP = 0,
	ZAP_PRIVATE_BLOCK,
	ZAP_PRIVATE_REMOVE,
};

bool kvm_tdp_mmu_zap_leafs(struct kvm *kvm, int as_id, gfn_t start,
			   gfn_t end, bool can_yield, bool flush,
			   enum tdp_zap_private  zap_private);
bool kvm_tdp_mmu_zap_sp(struct kvm *kvm, struct kvm_mmu_page *sp);
void kvm_tdp_mmu_zap_all(struct kvm *kvm);
void kvm_tdp_mmu_invalidate_all_roots(struct kvm *kvm, bool skip_private);
void kvm_tdp_mmu_zap_invalidated_roots(struct kvm *kvm);

int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault);

bool kvm_tdp_mmu_unmap_gfn_range(struct kvm *kvm, struct kvm_gfn_range *range,
				 bool flush);
bool kvm_tdp_mmu_age_gfn_range(struct kvm *kvm, struct kvm_gfn_range *range);
bool kvm_tdp_mmu_test_age_gfn(struct kvm *kvm, struct kvm_gfn_range *range);
bool kvm_tdp_mmu_set_spte_gfn(struct kvm *kvm, struct kvm_gfn_range *range);

bool kvm_tdp_mmu_wrprot_slot(struct kvm *kvm,
			     const struct kvm_memory_slot *slot, int min_level);
bool kvm_tdp_mmu_clear_dirty_slot(struct kvm *kvm,
				  const struct kvm_memory_slot *slot);
void kvm_tdp_mmu_clear_dirty_pt_masked(struct kvm *kvm,
				       struct kvm_memory_slot *slot,
				       gfn_t gfn, unsigned long mask,
				       bool wrprot);
void kvm_tdp_mmu_zap_collapsible_sptes(struct kvm *kvm,
				       const struct kvm_memory_slot *slot);

bool kvm_tdp_mmu_write_protect_gfn(struct kvm *kvm,
				   struct kvm_memory_slot *slot, gfn_t gfn,
				   int min_level);

void kvm_tdp_mmu_try_split_huge_pages(struct kvm *kvm,
				      const struct kvm_memory_slot *slot,
				      gfn_t start, gfn_t end,
				      int target_level, bool shared);

int kvm_tdp_mmu_restore_private_pages(struct kvm *kvm);

static inline void kvm_tdp_mmu_walk_lockless_begin(void)
{
	rcu_read_lock();
}

static inline void kvm_tdp_mmu_walk_lockless_end(void)
{
	rcu_read_unlock();
}

int kvm_tdp_mmu_get_walk(struct kvm_vcpu *vcpu, u64 addr, u64 *sptes,
			 int *root_level);
u64 *kvm_tdp_mmu_fast_pf_get_last_sptep(struct kvm_vcpu *vcpu, u64 addr,
					u64 *spte);

int kvm_tdp_mmu_import_private_pages(struct kvm_vcpu *vcpu,
				     gfn_t *gfns,
				     uint64_t *sptes,
				     uint64_t npages,
				     void *first_time_import_bitmap,
				     void *opaque);

#ifdef CONFIG_X86_64
static inline bool is_tdp_mmu_page(struct kvm_mmu_page *sp) { return sp->tdp_mmu_page; }
#else
static inline bool is_tdp_mmu_page(struct kvm_mmu_page *sp) { return false; }
#endif

#ifdef CONFIG_INTEL_TDX_HOST
int kvm_tdp_mmu_is_page_private(struct kvm *kvm, struct kvm_memory_slot *memslot,
				gfn_t gfn, bool *is_private);
#else
static inline int kvm_tdp_mmu_is_page_private(struct kvm *kvm, struct kvm_memory_slot *memslot,
					      gfn_t gfn, bool *is_private)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __KVM_X86_MMU_TDP_MMU_H */
