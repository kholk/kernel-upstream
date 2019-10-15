// SPDX-License-Identifier: GPL-2.0-only
/*
 * This kernel module validates architecture page table helpers &
 * accessors and helps in verifying their continued compliance with
 * generic MM semantics.
 *
 * Copyright (C) 2019 ARM Ltd.
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#define pr_fmt(fmt) "arch_pgtable_test: %s " fmt, __func__

#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/pfn_t.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/sched/mm.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>

/*
 * Basic operations
 *
 * mkold(entry)			= An old and not a young entry
 * mkyoung(entry)		= A young and not an old entry
 * mkdirty(entry)		= A dirty and not a clean entry
 * mkclean(entry)		= A clean and not a dirty entry
 * mkwrite(entry)		= A write and not a write protected entry
 * wrprotect(entry)		= A write protected and not a write entry
 * pxx_bad(entry)		= A mapped and non-table entry
 * pxx_same(entry1, entry2)	= Both entries hold the exact same value
 */
#define VMFLAGS	(VM_READ|VM_WRITE|VM_EXEC)

/*
 * On s390 platform, the lower 12 bits are used to identify given page table
 * entry type and for other arch specific requirements. But these bits might
 * affect the ability to clear entries with pxx_clear(). So while loading up
 * the entries skip all lower 12 bits in order to accommodate s390 platform.
 * It does not have affect any other platform.
 */
#define RANDOM_ORVALUE	(0xfffffffffffff000UL)
#define RANDOM_NZVALUE	(0xff)

static bool pud_aligned __initdata;
static bool pmd_aligned __initdata;

static void __init pte_basic_tests(struct page *page, pgprot_t prot)
{
	pte_t pte = mk_pte(page, prot);

	WARN_ON(!pte_same(pte, pte));
	WARN_ON(!pte_young(pte_mkyoung(pte)));
	WARN_ON(!pte_dirty(pte_mkdirty(pte)));
	WARN_ON(!pte_write(pte_mkwrite(pte)));
	WARN_ON(pte_young(pte_mkold(pte)));
	WARN_ON(pte_dirty(pte_mkclean(pte)));
	WARN_ON(pte_write(pte_wrprotect(pte)));
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE
static void __init pmd_basic_tests(struct page *page, pgprot_t prot)
{
	pmd_t pmd;

	/*
	 * Memory block here must be PMD_SIZE aligned. Abort this
	 * test in case we could not allocate such a memory block.
	 */
	if (!pmd_aligned) {
		pr_warn("Could not proceed with PMD tests\n");
		return;
	}

	pmd = mk_pmd(page, prot);
	WARN_ON(!pmd_same(pmd, pmd));
	WARN_ON(!pmd_young(pmd_mkyoung(pmd)));
	WARN_ON(!pmd_dirty(pmd_mkdirty(pmd)));
	WARN_ON(!pmd_write(pmd_mkwrite(pmd)));
	WARN_ON(pmd_young(pmd_mkold(pmd)));
	WARN_ON(pmd_dirty(pmd_mkclean(pmd)));
	WARN_ON(pmd_write(pmd_wrprotect(pmd)));
	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pmd_bad().
	 */
	WARN_ON(!pmd_bad(pmd_mkhuge(pmd)));
}
#else
static void __init pmd_basic_tests(struct page *page, pgprot_t prot) { }
#endif

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static void __init pud_basic_tests(struct page *page, pgprot_t prot)
{
	pud_t pud;

	/*
	 * Memory block here must be PUD_SIZE aligned. Abort this
	 * test in case we could not allocate such a memory block.
	 */
	if (!pud_aligned) {
		pr_warn("Could not proceed with PUD tests\n");
		return;
	}

	pud = pfn_pud(page_to_pfn(page), prot);
	WARN_ON(!pud_same(pud, pud));
	WARN_ON(!pud_young(pud_mkyoung(pud)));
	WARN_ON(!pud_write(pud_mkwrite(pud)));
	WARN_ON(pud_write(pud_wrprotect(pud)));
	WARN_ON(pud_young(pud_mkold(pud)));

	if (mm_pmd_folded(mm) || __is_defined(ARCH_HAS_4LEVEL_HACK))
		return;

	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pud_bad().
	 */
	WARN_ON(!pud_bad(pud_mkhuge(pud)));
}
#else
static void __init pud_basic_tests(struct page *page, pgprot_t prot) { }
#endif

static void __init p4d_basic_tests(struct page *page, pgprot_t prot)
{
	p4d_t p4d;

	memset(&p4d, RANDOM_NZVALUE, sizeof(p4d_t));
	WARN_ON(!p4d_same(p4d, p4d));
}

static void __init pgd_basic_tests(struct page *page, pgprot_t prot)
{
	pgd_t pgd;

	memset(&pgd, RANDOM_NZVALUE, sizeof(pgd_t));
	WARN_ON(!pgd_same(pgd, pgd));
}

#ifndef __ARCH_HAS_4LEVEL_HACK
static void __init pud_clear_tests(struct mm_struct *mm, pud_t *pudp)
{
	pud_t pud = READ_ONCE(*pudp);

	if (mm_pmd_folded(mm))
		return;

	pud = __pud(pud_val(pud) | RANDOM_ORVALUE);
	WRITE_ONCE(*pudp, pud);
	pud_clear(pudp);
	pud = READ_ONCE(*pudp);
	WARN_ON(!pud_none(pud));
}

static void __init pud_populate_tests(struct mm_struct *mm, pud_t *pudp,
				      pmd_t *pmdp)
{
	pud_t pud;

	if (mm_pmd_folded(mm))
		return;
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pud_bad().
	 */
	pmd_clear(pmdp);
	pud_clear(pudp);
	pud_populate(mm, pudp, pmdp);
	pud = READ_ONCE(*pudp);
	WARN_ON(pud_bad(pud));
}
#else
static void __init pud_clear_tests(struct mm_struct *mm, pud_t *pudp) { }
static void __init pud_populate_tests(struct mm_struct *mm, pud_t *pudp,
				      pmd_t *pmdp)
{
}
#endif

#ifndef __ARCH_HAS_5LEVEL_HACK
static void __init p4d_clear_tests(struct mm_struct *mm, p4d_t *p4dp)
{
	p4d_t p4d = READ_ONCE(*p4dp);

	if (mm_pud_folded(mm))
		return;

	p4d = __p4d(p4d_val(p4d) | RANDOM_ORVALUE);
	WRITE_ONCE(*p4dp, p4d);
	p4d_clear(p4dp);
	p4d = READ_ONCE(*p4dp);
	WARN_ON(!p4d_none(p4d));
}

static void __init p4d_populate_tests(struct mm_struct *mm, p4d_t *p4dp,
				      pud_t *pudp)
{
	p4d_t p4d;

	if (mm_pud_folded(mm))
		return;

	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as p4d_bad().
	 */
	pud_clear(pudp);
	p4d_clear(p4dp);
	p4d_populate(mm, p4dp, pudp);
	p4d = READ_ONCE(*p4dp);
	WARN_ON(p4d_bad(p4d));
}

static void __init pgd_clear_tests(struct mm_struct *mm, pgd_t *pgdp)
{
	pgd_t pgd = READ_ONCE(*pgdp);

	if (mm_p4d_folded(mm))
		return;

	pgd = __pgd(pgd_val(pgd) | RANDOM_ORVALUE);
	WRITE_ONCE(*pgdp, pgd);
	pgd_clear(pgdp);
	pgd = READ_ONCE(*pgdp);
	WARN_ON(!pgd_none(pgd));
}

static void __init pgd_populate_tests(struct mm_struct *mm, pgd_t *pgdp,
				      p4d_t *p4dp)
{
	pgd_t pgd;

	if (mm_p4d_folded(mm))
		return;

	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pgd_bad().
	 */
	p4d_clear(p4dp);
	pgd_clear(pgdp);
	pgd_populate(mm, pgdp, p4dp);
	pgd = READ_ONCE(*pgdp);
	WARN_ON(pgd_bad(pgd));
}
#else
static void __init p4d_clear_tests(struct mm_struct *mm, p4d_t *p4dp) { }
static void __init pgd_clear_tests(struct mm_struct *mm, pgd_t *pgdp) { }
static void __init p4d_populate_tests(struct mm_struct *mm, p4d_t *p4dp,
				      pud_t *pudp)
{
}
static void __init pgd_populate_tests(struct mm_struct *mm, pgd_t *pgdp,
				      p4d_t *p4dp)
{
}
#endif

static void __init pte_clear_tests(struct mm_struct *mm, pte_t *ptep)
{
	pte_t pte = READ_ONCE(*ptep);

	pte = __pte(pte_val(pte) | RANDOM_ORVALUE);
	WRITE_ONCE(*ptep, pte);
	pte_clear(mm, 0, ptep);
	pte = READ_ONCE(*ptep);
	WARN_ON(!pte_none(pte));
}

static void __init pmd_clear_tests(struct mm_struct *mm, pmd_t *pmdp)
{
	pmd_t pmd = READ_ONCE(*pmdp);

	pmd = __pmd(pmd_val(pmd) | RANDOM_ORVALUE);
	WRITE_ONCE(*pmdp, pmd);
	pmd_clear(pmdp);
	pmd = READ_ONCE(*pmdp);
	WARN_ON(!pmd_none(pmd));
}

static void __init pmd_populate_tests(struct mm_struct *mm, pmd_t *pmdp,
				      pgtable_t pgtable)
{
	pmd_t pmd;

	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pmd_bad().
	 */
	pmd_clear(pmdp);
	pmd_populate(mm, pmdp, pgtable);
	pmd = READ_ONCE(*pmdp);
	WARN_ON(pmd_bad(pmd));
}

static struct page * __init alloc_mapped_page(void)
{
	struct page *page;
	gfp_t gfp_mask = GFP_KERNEL | __GFP_ZERO;

	page = alloc_gigantic_page_order(get_order(PUD_SIZE), gfp_mask,
				first_memory_node, &node_states[N_MEMORY]);
	if (page) {
		pud_aligned = true;
		pmd_aligned = true;
		return page;
	}

	page = alloc_pages(gfp_mask, get_order(PMD_SIZE));
	if (page) {
		pmd_aligned = true;
		return page;
	}
	return alloc_page(gfp_mask);
}

static void __init free_mapped_page(struct page *page)
{
	if (pud_aligned) {
		unsigned long pfn = page_to_pfn(page);

		free_contig_range(pfn, 1ULL << get_order(PUD_SIZE));
		return;
	}

	if (pmd_aligned) {
		int order = get_order(PMD_SIZE);

		free_pages((unsigned long)page_address(page), order);
		return;
	}
	free_page((unsigned long)page_address(page));
}

static unsigned long __init get_random_vaddr(void)
{
	unsigned long random_vaddr, random_pages, total_user_pages;

	total_user_pages = (TASK_SIZE - FIRST_USER_ADDRESS) / PAGE_SIZE;

	random_pages = get_random_long() % total_user_pages;
	random_vaddr = FIRST_USER_ADDRESS + random_pages * PAGE_SIZE;

	WARN_ON(random_vaddr > TASK_SIZE);
	WARN_ON(random_vaddr < FIRST_USER_ADDRESS);
	return random_vaddr;
}

void __init debug_vm_pgtable(void)
{
	struct mm_struct *mm;
	struct page *page;
	pgd_t *pgdp;
	p4d_t *p4dp, *saved_p4dp;
	pud_t *pudp, *saved_pudp;
	pmd_t *pmdp, *saved_pmdp, pmd;
	pte_t *ptep;
	pgtable_t saved_ptep;
	pgprot_t prot;
	unsigned long vaddr;

	prot = vm_get_page_prot(VMFLAGS);
	vaddr = get_random_vaddr();
	mm = mm_alloc();
	if (!mm) {
		pr_err("mm_struct allocation failed\n");
		return;
	}

	page = alloc_mapped_page();
	if (!page) {
		pr_err("memory allocation failed\n");
		return;
	}

	pgdp = pgd_offset(mm, vaddr);
	p4dp = p4d_alloc(mm, pgdp, vaddr);
	pudp = pud_alloc(mm, p4dp, vaddr);
	pmdp = pmd_alloc(mm, pudp, vaddr);
	ptep = pte_alloc_map(mm, pmdp, vaddr);

	/*
	 * Save all the page table page addresses as the page table
	 * entries will be used for testing with random or garbage
	 * values. These saved addresses will be used for freeing
	 * page table pages.
	 */
	pmd = READ_ONCE(*pmdp);
	saved_p4dp = p4d_offset(pgdp, 0UL);
	saved_pudp = pud_offset(p4dp, 0UL);
	saved_pmdp = pmd_offset(pudp, 0UL);
	saved_ptep = pmd_pgtable(pmd);

	pte_basic_tests(page, prot);
	pmd_basic_tests(page, prot);
	pud_basic_tests(page, prot);
	p4d_basic_tests(page, prot);
	pgd_basic_tests(page, prot);

	pte_clear_tests(mm, ptep);
	pmd_clear_tests(mm, pmdp);
	pud_clear_tests(mm, pudp);
	p4d_clear_tests(mm, p4dp);
	pgd_clear_tests(mm, pgdp);

	pte_unmap(ptep);

	pmd_populate_tests(mm, pmdp, saved_ptep);
	pud_populate_tests(mm, pudp, saved_pmdp);
	p4d_populate_tests(mm, p4dp, saved_pudp);
	pgd_populate_tests(mm, pgdp, saved_p4dp);

	p4d_free(mm, saved_p4dp);
	pud_free(mm, saved_pudp);
	pmd_free(mm, saved_pmdp);
	pte_free(mm, saved_ptep);

	mm_dec_nr_puds(mm);
	mm_dec_nr_pmds(mm);
	mm_dec_nr_ptes(mm);
	__mmdrop(mm);

	free_mapped_page(page);
}
