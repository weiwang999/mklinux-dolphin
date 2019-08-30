/**
 * @file page_server.c
 *
 * Popcorn Linux page server implementation
 * This work is an extension of Marina Sadini MS Thesis, plese refer to the
 * Thesis for further information about the algorithm.
 *
 * @author Vincent Legout, Antonio Barbalace, SSRG Virginia Tech 2016
 * @author Ajith Saya, Sharath Bhat, SSRG Virginia Tech 2015
 * @author Marina Sadini, Antonio Barbalace, SSRG Virginia Tech 2014
 * @author Marina Sadini, SSRG Virginia Tech 2013
 */
/*
 * THE FOLLOWINGS ARE THE MESSAGE WAITERS/RECEIVERS: (rendevouz marina style)
 * handle_mapping_response_void <<< called by the messaging layer, registered from the page_server_init function
 * handle_mapping_response <<< called by the messaging layer, registered from the page_server_init function
 * handle_ack <<< called by the messaging layer, registered from the page_server_init function ??? there is probably one also for the vma_server
 * END OF MESSAGE RECEIVERS
 *
 * REMOTE WORKERS == SERVERS
 * handle_invalid_request <<< called by the messaging layer, registered from the page_server_init
 * handle_mapping_request << called by the messaging layer, registered from the page_server_init
 *
 * process_invalid_request_for_2_kernels <<< called by handle_invalid_request
 *                                                     called by itself as a delayed work
 * process_mapping_request_for_2_kernels <<< called by handle_mapping_request
 * 														called by itself as a delayed work
 * END OF REMOTE WORKERS
 *
 * process_server_update_page <<< called by __do_page_fault, and do_page_fault
 * process_server_clean_page <<< called by get_page_from_freelist, and zap_pte_page
 *
 * LOCAL WORKERS == CLIENTS
 * do_remote_read_for_2_kernels <<< called by process_server_try_handle_mm_fault
 * do_remote_write_for_2_kernels <<< called by process_server_try_handle_mm_fault
 * do_remote_fetch_for_2_kernels <<< called by process_server_try_handle_mm_fault
 *
 * process_server_try_handle_mm_fault <<< called by __do_page_fault
 *                                                  __get_user_pages
 *                                                  fixup_user_fault
 * END LOCAL WORKERS
 *
 * page_server_init <<< called by process_server initialization function
 */

#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/mman.h>
#include <linux/highmem.h>
#include <linux/rmap.h>
#include <linux/memcontrol.h>
#include <linux/pagemap.h>
#include <linux/mmu_notifier.h>

#include <asm/traps.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/page.h>
#include <asm/mmu_context.h>
#include <asm/atomic.h>

#include <popcorn/init.h>
#include <popcorn/cpuinfo.h>
#include <process_server_arch.h>
#include <linux/process_server.h>
#include <popcorn/process_server.h>
#include "page_server.h"
#include <popcorn/page_server.h>
#include "vma_server.h"
#include <popcorn/vma_server.h>
#include "sched_server.h"
#include <popcorn/sched_server.h>
#include "internal.h"

#include <linux/popcorn_cpuinfo.h>

///////////////////////////////////////////////////////////////////////////////
// Working queues (servers)
///////////////////////////////////////////////////////////////////////////////
static struct workqueue_struct *message_request_wq;
static struct workqueue_struct *invalid_message_wq;

// wait lists
DECLARE_WAIT_QUEUE_HEAD( read_write_wait);

///////////////////////////////////////////////////////////////////////////////
// Mappings handling
///////////////////////////////////////////////////////////////////////////////
/* TODO the following two functions can be refactored into one */
static int handle_mapping_response_void(struct pcn_kmsg_message* inc_msg)
{
	data_void_response_for_2_kernels_t* response;
	mapping_answers_for_2_kernels_t* fetched_data;

	response = (data_void_response_for_2_kernels_t*) inc_msg;
	fetched_data = find_mapping_entry(response->tgroup_home_cpu,
					  response->tgroup_home_id, response->address);

#if STATISTICS
	answer_request_void++;
#endif
	PSPRINTK("%s: answer_request_void %i address %lx from cpu %i. This is a void response.\n", __func__, 0, response->address, inc_msg->hdr.from_cpu);
	PSMINPRINTK("answer_request_void address %lx from cpu %i. This is a void response.\n", response->address, inc_msg->hdr.from_cpu);

	if (fetched_data == NULL) {
		printk(KERN_ERR"%s: WARN: data not found in local list r%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
				__func__, response->fetching_read, response->fetching_write,
				response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);
		pcn_kmsg_free_msg(inc_msg);
		return -1;
	}

	if (response->owner == 1) {
		PSPRINTK("Response with ownership\n");
		fetched_data->owner = 1;
	}

	if (response->vma_present == 1) {
		if (response->header.from_cpu != response->tgroup_home_cpu)
			printk("%s: WARN: a kernel that is not the server is sending the mapping (cpu %d id %d address 0x%lx)\n",
					__func__, response->header.from_cpu, response->tgroup_home_cpu, response->address);

		PSPRINTK("%s: response->vma_pesent %d response->vaddr_start %lx response->vaddr_size %lx response->prot %lx response->vm_flags %lx response->pgoff %lx response->path %s response->fowner %d\n", __func__,
					 response->vma_present, response->vaddr_start , response->vaddr_size,response->prot, response->vm_flags , response->pgoff, response->path,response->futex_owner);

		if (fetched_data->vma_present == 0) {
			PSPRINTK("Set vma\n");
			fetched_data->vma_present = 1;
			fetched_data->vaddr_start = response->vaddr_start;
			fetched_data->vaddr_size = response->vaddr_size;
			fetched_data->prot = response->prot;
			fetched_data->pgoff = response->pgoff;
			fetched_data->vm_flags = response->vm_flags;
			strcpy(fetched_data->path, response->path);
		} else {
			printk("%s: WARN: received more than one mapping %d r%dw%d f%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
					__func__, fetched_data->vma_present, response->fetching_read, response->fetching_write,
					fetched_data->is_fetch, fetched_data->is_write,
					response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);
		}
	}

	if (fetched_data->arrived_response!=0)
		printk("%s: WARN: received more than one answer, arrived_response is %d r%dw%d f%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
				__func__, fetched_data->arrived_response, response->fetching_read, response->fetching_write,
				fetched_data->is_fetch, fetched_data->is_write,
				response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);

	fetched_data->arrived_response++;
	fetched_data->futex_owner = response->futex_owner;

	PSPRINTK("%s: wake up %d\n", __func__, fetched_data->waiting->pid);
	wake_up_process(fetched_data->waiting);

	pcn_kmsg_free_msg(inc_msg);
	return 1;
}

static int handle_mapping_response(struct pcn_kmsg_message* inc_msg)
{
	data_response_for_2_kernels_t* response;
	mapping_answers_for_2_kernels_t* fetched_data;
	int set = 0;

	response = (data_response_for_2_kernels_t*) inc_msg;
	fetched_data = find_mapping_entry(response->tgroup_home_cpu,
					  response->tgroup_home_id, response->address);

	//printk("sizeof(data_response_for_2_kernels_t) %d PAGE_SIZE %d response->data_size %d\n",sizeof(data_response_for_2_kernels_t),PAGE_SIZE,response->data_size);
#if STATISTICS
	answer_request++;
#endif
	PSPRINTK("%s: Answer_request %i address %lx from cpu %i\n", __func__, 0, response->address, inc_msg->hdr.from_cpu);
	PSMINPRINTK("Received answer for address %lx last write %d from cpu %i\n", response->address, response->last_write,inc_msg->hdr.from_cpu);

	if (fetched_data == NULL) {
		printk(KERN_ERR"%s: WARN: data not found in local list (cpu %d id %d address 0x%lx) 0x%lx\n",
				__func__, response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);
		pcn_kmsg_free_msg(inc_msg);
		return -1;
	}

	if (response->owner == 1) {
		PSPRINTK("Response with ownership\n");
		fetched_data->owner = 1;
	}

	if (response->vma_present == 1) {
		if (response->header.from_cpu != response->tgroup_home_cpu)
			printk("%s: WARN: a kernel that is not the server is sending the mapping (cpu %d id %d address 0x%lx)\n",
					__func__, response->header.from_cpu, response->tgroup_home_cpu, response->address);

		PSPRINTK("%s: response->vma_pesent %d response->vaddr_start %lx response->vaddr_size %lx response->prot %lx response->vm_flags %lx response->pgoff %lx response->path %s response->fowner %d\n", __func__,
			 response->vma_present, response->vaddr_start , response->vaddr_size,response->prot, response->vm_flags , response->pgoff, response->path,response->futex_owner);

		if (fetched_data->vma_present == 0) {
			PSPRINTK("Set vma\n");
			fetched_data->vma_present = 1;
			fetched_data->vaddr_start = response->vaddr_start;
			fetched_data->vaddr_size = response->vaddr_size;
			fetched_data->prot = response->prot;
			fetched_data->pgoff = response->pgoff;
			fetched_data->vm_flags = response->vm_flags;
			strcpy(fetched_data->path, response->path);
		} else {
			printk("%s: WARN: received more than one mapping %d f%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
					__func__, fetched_data->vma_present, fetched_data->is_fetch, fetched_data->is_write,
					response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);
		}
	}

	if (fetched_data->address_present == 1) {
		printk("%s: WARN: received more than one answer with a copy of the page from %d f%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
						__func__, response->header.from_cpu, fetched_data->is_fetch, fetched_data->is_write,
						response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);
	}
	else  {
		fetched_data->address_present= 1;
		fetched_data->data = response;
		fetched_data->last_write = response->last_write;
		set = 1;
	}

	if (fetched_data->arrived_response!=0)
		printk("%s: WARN: received more than one answer, arrived_response is %d f%dw%d (cpu %d id %d address 0x%lx) 0x%lx\n",
				__func__, fetched_data->arrived_response, fetched_data->is_fetch, fetched_data->is_write,
				response->tgroup_home_cpu, response->tgroup_home_id, response->address, (unsigned long)response);

	fetched_data->owners[inc_msg->hdr.from_cpu] = 1;
	fetched_data->arrived_response++;
	fetched_data->futex_owner = response->futex_owner;

	PSPRINTK("%s: wake up %d\n", __func__, fetched_data->waiting->pid);
	wake_up_process(fetched_data->waiting);

	if (set == 0)
		pcn_kmsg_free_msg(inc_msg);  // This is deleted by do_remote_*_for_2_kernels
	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// Handling acknowledges
///////////////////////////////////////////////////////////////////////////////
static int handle_ack(struct pcn_kmsg_message* inc_msg)
{
	ack_t* response;
	ack_answers_for_2_kernels_t* fetched_data;

	response = (ack_t*) inc_msg;
	fetched_data = find_ack_entry(response->tgroup_home_cpu,
				      response->tgroup_home_id, response->address);

#if STATISTICS
	ack++;
#endif
	PSPRINTK("Answer_invalid %i address %lx from cpu %i\n",
			ack, response->address, inc_msg->hdr.from_cpu);

	if (fetched_data == NULL) {
		goto out;
	}
	fetched_data->response_arrived++;

	if(fetched_data->response_arrived>1)
		printk("ERROR: received more than one ack\n");

	PSPRINTK("%s: wake up %d\n", __func__, fetched_data->waiting->pid);
			wake_up_process(fetched_data->waiting);

out:
	pcn_kmsg_free_msg(inc_msg);
	return 0;
}

static void process_invalid_request_for_2_kernels(struct work_struct* work)
{
	invalid_work_t* work_request = (invalid_work_t*) work;
	invalid_data_for_2_kernels_t* data = work_request->request;
	ack_t* response;
	memory_t* memory = NULL;
	struct mm_struct* mm = NULL;
	struct vm_area_struct* vma;
	unsigned long address = data->address & PAGE_MASK;
	int from_cpu = data->header.from_cpu;
	pgd_t* pgd;
	pud_t* pud;
	pmd_t* pmd;
	pte_t* pte;
	pte_t entry;
	struct page* page;
	spinlock_t *ptl;
	int lock = 0;

	//unsigned long long start,end;
	invalid_work_t *delay;

#if STATISTICS
	invalid++;
#endif

	PSPRINTK("Invalid %i address %lx from cpu %i\n", invalid, data->address, from_cpu);
	PSMINPRINTK("Invalid for address %lx from cpu %i\n",data->address, from_cpu);

	//start= native_read_tsc();
	response= (ack_t*) kmalloc(sizeof(ack_t), GFP_ATOMIC);
	if(response==NULL){
		pcn_kmsg_free_msg(data);
		kfree(work);
		return;
	}
	response->writing = 0;

	memory = find_memory_entry(data->tgroup_home_cpu, data->tgroup_home_id);
	if (memory != NULL) {
		if(memory->setting_up==1){
			goto out;
		}
		mm = memory->mm;
	} else {
		goto out;
	}

	down_read(&mm->mmap_sem);
	//check the vma era first -- check the comment about VMA era in this file
	//if delayed is not a problem (but if > it is maybe a problem)
	if (mm->vma_operation_index > data->vma_operation_index)
			printk("%s: WARN: different era invalid [mm %d > data %d] (cpu %d id %d)\n",
					__func__, mm->vma_operation_index, data->vma_operation_index,
					data->tgroup_home_cpu, data->tgroup_home_id);

	if (mm->vma_operation_index < data->vma_operation_index) {
		printk("%s: WARN: different era invalid [mm %d < data %d] (cpu %d id %d)\n",
				__func__, mm->vma_operation_index, data->vma_operation_index,
				data->tgroup_home_cpu, data->tgroup_home_id);
		delay = (invalid_work_t*) kmalloc(sizeof(invalid_work_t), GFP_ATOMIC);

		if (delay!=NULL) {
			delay->request = data;
			INIT_DELAYED_WORK( (struct delayed_work*)delay,
					   process_invalid_request_for_2_kernels);
			queue_delayed_work(invalid_message_wq,
					   (struct delayed_work*) delay, 10);
		}

		up_read(&mm->mmap_sem);
		kfree(work);
		return;
	}

	// check if there is a valid vma
	vma = find_vma(mm, address);
	if (!vma || address >= vma->vm_end || address < vma->vm_start) {
		vma = NULL;
		if (_cpu == data->tgroup_home_cpu)
					printk(KERN_ALERT"%s: vma NULL in cpu %d address 0x%lx\n",
							__func__, _cpu, address);
	} else {
		if (unlikely(is_vm_hugetlb_page(vma))
		    || unlikely(transparent_hugepage_enabled(vma))) {
			printk("%s: ERROR: Request for HUGE PAGE vma\n", __func__);
			up_read(&mm->mmap_sem);
			goto out;
		}
	}

	pgd = pgd_offset(mm, address);
	if (!pgd || pgd_none(*pgd)) {
		up_read(&mm->mmap_sem);
		goto out;
	}
	pud = pud_offset(pgd, address);
	if (!pud || pud_none(*pud)) {
		up_read(&mm->mmap_sem);
		goto out;
	}
	pmd = pmd_offset(pud, address);
	if (!pmd || pmd_none(*pmd) || pmd_trans_huge(*pmd)) {
		up_read(&mm->mmap_sem);
		goto out;
	}

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	/*PTE LOCKED*/
	lock = 1;

	//case pte not yet installed
        if (pte == NULL ||
#if defined(CONFIG_ARM64) 
		pte_none(*pte) ) {
#else
		pte_none(pte_clear_flags(*pte,_PAGE_UNUSED1)) ) {
#endif	
		PSPRINTK("pte not yet mapped\n");

		//If I receive an invalid while it is not mapped, I must be fetching the page.
		//Otherwise it is an error.
		//Delay the invalid while I install the page.

		//Check if I am concurrently fetching the page
		mapping_answers_for_2_kernels_t* fetched_data = find_mapping_entry(
			data->tgroup_home_cpu, data->tgroup_home_id, address);

		if (fetched_data != NULL) {
			PSPRINTK("Concurrently fetching the same address\n");

			if(fetched_data->is_fetch!=1)
				printk("%s: ERROR: invalid received for a not mapped pte that has a mapping_answer not in fetching\n",
						__func__);

			delay = (invalid_work_t*) kmalloc(sizeof(invalid_work_t), GFP_ATOMIC);
			if (delay!=NULL) {
				delay->request = data;
				INIT_DELAYED_WORK( (struct delayed_work*)delay,
						   process_invalid_request_for_2_kernels);
				queue_delayed_work(invalid_message_wq,
						   (struct delayed_work*) delay, 10);
			}
			spin_unlock(ptl);
			up_read(&mm->mmap_sem);
			kfree(work);
			return;
		}
		else
			printk("%s: ERROR: received an invalid for a not mapped pte not in fetching status (cpu %d id %d address 0x%lx)\n",
					__func__, data->tgroup_home_cpu, data->tgroup_home_id, address);

		goto out;
	} else {
		//the "standard" page fault releases the pte lock after that it installs the page
		//so before that I lock the pte again there is a moment in which is not null
		//but still fetching
		if (memory->alive != 0) {
			mapping_answers_for_2_kernels_t* fetched_data = find_mapping_entry(
				data->tgroup_home_cpu, data->tgroup_home_id, address);

			if(fetched_data!=NULL && fetched_data->is_fetch==1){
				delay = (invalid_work_t*) kmalloc(sizeof(invalid_work_t), GFP_ATOMIC);

				if (delay!=NULL) {
					delay->request = data;
					INIT_DELAYED_WORK( (struct delayed_work*)delay,
							   process_invalid_request_for_2_kernels);
					queue_delayed_work(invalid_message_wq,
							   (struct delayed_work*) delay, 10);
				}
				spin_unlock(ptl);
				up_read(&mm->mmap_sem);
				kfree(work);
				return;
			}
		}

		page = pte_page(*pte);
		if (page != vm_normal_page(vma, address, *pte)) {
			PSPRINTK("page different from vm_normal_page in request page\n");
		}
		if (page->replicated == 0 || page->status==REPLICATION_STATUS_NOT_REPLICATED) {
			printk("%s: ERROR: Invalid message in not replicated page cpu %d id %d address 0x%lx\n",
					__func__, data->tgroup_home_cpu, data->tgroup_home_id, address);
			goto out;
		}

		if (page->status == REPLICATION_STATUS_WRITTEN) {
			printk("%s: ERROR: invalid message in a written page cpu %d id %d address 0x%lx\n",
					__func__, data->tgroup_home_cpu, data->tgroup_home_id, address);
			goto out;
		}

		if (page->reading==1) {
			/*If I am reading my current status must be invalid and the one of the other kernel must be written.
			 *After that he sees my request of page, it mights want to write again and it sends me an invalid.
			 *So this request must be delayed.
			 */
			//printk("page reading when received invalid\n");

			if(page->status!=REPLICATION_STATUS_INVALID || page->last_write!=(data->last_write-1))
				printk("%s: WARN: Incorrect invalid received while reading address %lx, my status is %d, page last write %lx, invalid for version %lx (cpu %d id %d)\n",
						__func__, address,page->status,page->last_write,data->last_write,
						data->tgroup_home_cpu, data->tgroup_home_id);

			delay = (invalid_work_t*) kmalloc(sizeof(invalid_work_t), GFP_ATOMIC);

			if (delay!=NULL) {
				delay->request = data;
				INIT_DELAYED_WORK( (struct delayed_work*)delay,
						   process_invalid_request_for_2_kernels);
				queue_delayed_work(invalid_message_wq,
						   (struct delayed_work*) delay, 10);
			}
			spin_unlock(ptl);
			up_read(&mm->mmap_sem);
			kfree(work);
			return;
		}

		if (page->writing==1) {
			/*Concurrent write.
			 *To be correct I must be or in valid or invalid state and not owner.
			 *The kernel with the ownership always wins.
			 */
			response->writing=1;
			if(page->owner==1 || page->status==REPLICATION_STATUS_WRITTEN)
				printk("%s: WARN: Incorrect invalid received while writing address %lx, my status is %d, page last write %lx, invalid for version %lx page owner %d (cpu %d id %d)\n", __func__,
				       address,page->status,page->last_write,data->last_write,page->owner,
					   data->tgroup_home_cpu, data->tgroup_home_id);

			//printk("received invalid while writing\n");
		}

		if (page->last_write!= data->last_write)
			printk("%s: WARN: received an invalid for copy %lx but my copy is %lx (cpu %d id %d)\n",
					__func__, data->last_write,page->last_write,
					data->tgroup_home_cpu, data->tgroup_home_id);

		page->status = REPLICATION_STATUS_INVALID;
		page->owner = 0;

// TODO check the following
		flush_cache_page(vma, address, pte_pfn(*pte));
		entry = *pte;
		//the page is invalid so as not present
#if defined(CONFIG_ARM64)
		entry = pte_clear_valid_entry_flag(entry);
		entry = pte_mkyoung(entry);
#else
		entry = pte_clear_flags(entry, _PAGE_PRESENT);
		entry = pte_set_flags(entry, _PAGE_ACCESSED);
#endif

		ptep_clear_flush(vma, address, pte);
		set_pte_at_notify(mm, address, pte, entry);

		update_mmu_cache(vma, address, pte);
	    
		flush_tlb_page(vma, address);
		flush_tlb_fix_spurious_fault(vma, address);
	}

out: if (lock) {
		spin_unlock(ptl);
		up_read(&mm->mmap_sem);
	}

	response->header.type = PCN_KMSG_TYPE_PROC_SRV_ACK_DATA;
	response->header.prio = PCN_KMSG_PRIO_NORMAL;
	response->tgroup_home_cpu = data->tgroup_home_cpu;
	response->tgroup_home_id = data->tgroup_home_id;
	response->address = data->address;
	response->ack = 1;

	//pcn_kmsg_send(from_cpu, (struct pcn_kmsg_message*) (response));
	pcn_kmsg_send_long(from_cpu,(struct pcn_kmsg_long_message*) (response),sizeof(ack_t)-sizeof(struct pcn_kmsg_hdr));
	kfree(response);
	pcn_kmsg_free_msg(data);
	kfree(work);
}

static int handle_invalid_request(struct pcn_kmsg_message* inc_msg)
{
	invalid_work_t* request_work;
	invalid_data_for_2_kernels_t* data = (invalid_data_for_2_kernels_t*) inc_msg;

	request_work = (invalid_work_t*)kmalloc(sizeof(invalid_work_t), GFP_ATOMIC);

	if (request_work!=NULL) {
		request_work->request = data;
		INIT_WORK( (struct work_struct*)request_work, process_invalid_request_for_2_kernels);
		queue_work(invalid_message_wq, (struct work_struct*) request_work);
	}
	return 1;
}

static void process_mapping_request_for_2_kernels(struct work_struct* work)
{
	request_work_t* request_work = (request_work_t*) work;

	data_request_for_2_kernels_t* request = request_work->request;
	data_void_response_for_2_kernels_t * void_response = NULL;
	data_response_for_2_kernels_t * response = NULL;
	mapping_answers_for_2_kernels_t * fetched_data = NULL;

	memory_t * memory = NULL;
	struct mm_struct * mm = NULL;
	struct vm_area_struct * vma = NULL;
	struct page * page = NULL, * old_page = NULL;

	int owner= 0;
	char * plpath; char lpath[512];
	int from_cpu = request->header.from_cpu;
	unsigned long address = request->address & PAGE_MASK;
	pgd_t* pgd;
	pud_t* pud;
	pmd_t* pmd;
	pte_t* pte;
	pte_t entry;

	spinlock_t* ptl;
	request_work_t* delay;

	int lock =0;
	void *vfrom;

#if STATISTICS
	request_data++;
#endif
	PSPRINTK("%s: INFO: Request address 0x%lx is_fetch:%i is_write:%i idx:%i\n",
			__func__, request->address,
			((request->is_fetch==1)?1:0), ((request->is_write==1)?1:0), request->vma_operation_index);

	memory = find_memory_entry(request->tgroup_home_cpu, request->tgroup_home_id);
	if (memory != NULL) {
		if (memory->setting_up==1) {
			owner=1;
			goto out;
		}
		mm = memory->mm;
		if (memory->tgroup_home_cpu != request->tgroup_home_cpu
				|| memory->tgroup_home_id != request->tgroup_home_id)
			printk("%s: ERROR: tgroup_home_cpu and tgroup_home_id DON'T match", __func__);
	}
	else {
		owner=1;
		goto out;
	}
	PSPRINTK("%s: INFO: Request address 0x%lx is_fetch:%d is_write:%d idx:%d mm_idx:%d\n",
			__func__, request->address,
			((request->is_fetch==1)?1:0), ((request->is_write==1)?1:0),
			request->vma_operation_index, mm->vma_operation_index);

	down_read(&mm->mmap_sem);
	/*
	 * check the vma era first -- this is an error only if it keeps printing forever
	 * if it is still a problem do increment the delay
	 * In Marina's design we should only apply the same modifications at the same era
	 * Added the next check only to make sure that > is not a problem (but cannot be re-queued)
	 */
	if (mm->vma_operation_index > request->vma_operation_index)
		printk("%s: INFO: different era request [mm %d > request %d] (cpu %d id %d)\n",
				__func__, mm->vma_operation_index, request->vma_operation_index,
				request->tgroup_home_cpu, request->tgroup_home_id);

	if (mm->vma_operation_index < request->vma_operation_index) {
		printk("%s: WARN: different era request [mm %d < request %d] (cpu %d id %d)\n",
				__func__, mm->vma_operation_index, request->vma_operation_index,
				request->tgroup_home_cpu, request->tgroup_home_id);
		delay = (request_work_t*)kmalloc(sizeof(request_work_t), GFP_ATOMIC);
		if (delay) {
			delay->request = request;
			INIT_DELAYED_WORK( (struct delayed_work*)delay,
					   process_mapping_request_for_2_kernels);
			queue_delayed_work(message_request_wq,
					   (struct delayed_work*) delay, 10);
		}
		else {
			printk("%s: ERROR: cannot allocate memory to delay work 1 (cpu %d id %d)\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id);
		}

		up_read(&mm->mmap_sem);
		kfree(work);
		return;
	}

	// check if there is a valid vma
	vma = find_vma(mm, address);
	if (!vma || address >= vma->vm_end || address < vma->vm_start) {
		vma = NULL;
		if(_cpu == request->tgroup_home_cpu){
			printk(KERN_ALERT"%s: ERROR: vma NULL in cpu %d address 0x%lx\n",
					__func__, _cpu, address);
			up_read(&mm->mmap_sem);
			goto out;
		}
	} else {
		if (unlikely(is_vm_hugetlb_page(vma))
		    || unlikely(transparent_hugepage_enabled(vma))) {
			printk("%s: ERROR: Request for HUGE PAGE vma\n", __func__);
			up_read(&mm->mmap_sem);
			goto out;
		}
		PSPRINTK("Find vma from %s start %lx end %lx\n",
				((vma->vm_file!=NULL)?d_path(&vma->vm_file->f_path,lpath,512):"no file"), vma->vm_start, vma->vm_end);
	}
	PSPRINTK("In %s:%d vma_flags = %lx\n", __func__, __LINE__, vma->vm_flags);

	if (vma && vma->vm_flags & VM_FETCH_LOCAL) {
		PSPRINTK("%s: WARN: VM_FETCH_LOCAL flag set - Going to void response address %lx\n",
				__func__, address);
		up_read(&mm->mmap_sem);
		goto out;
	}

	if (_cpu != request->tgroup_home_cpu) {
		pgd = pgd_offset(mm, address);
		if (!pgd || pgd_none(*pgd)) {
			up_read(&mm->mmap_sem);
			goto out;
		}
		pud = pud_offset(pgd, address); //pud_alloc below
		if (!pud || pud_none(*pud)) {
			up_read(&mm->mmap_sem);
			goto out;
		}
		pmd = pmd_offset(pud, address); //pmd_alloc below
		if (!pmd || pmd_none(*pmd) || pmd_trans_huge(*pmd)) {
			up_read(&mm->mmap_sem);
			goto out;
		}
	}
	else {
		pgd = pgd_offset(mm, address);
		if (!pgd || pgd_none(*pgd)) {
			up_read(&mm->mmap_sem);
			goto out;
		}
		pud = pud_alloc(mm, pgd, address);
		if (!pud){
			up_read(&mm->mmap_sem);
			goto out;
		}
		pmd = pmd_alloc(mm, pud, address);
		if (!pmd){
			up_read(&mm->mmap_sem);
			goto out;
		}

		if (pmd_none(*pmd) && __pte_alloc(mm, vma, pmd, address)){
			up_read(&mm->mmap_sem);
			goto out;
		}
		if (unlikely(pmd_trans_huge(*pmd))) {
			printk("%s: ERROR: request for huge page\n", __func__);
			up_read(&mm->mmap_sem);
			goto out;
		}
	}
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	/*PTE LOCKED*/
	entry = *pte;
	lock= 1;
///////////////////////////////////////////////////////////////////////////////
// Got pte, now proceed with the rest

	if (pte == NULL ||
#if defined(CONFIG_ARM64)
		pte_none(entry)) {
#else
		pte_none(pte_clear_flags(entry, _PAGE_UNUSED1))) {
#endif
		PSPRINTK("pte not mapped\n");

		if ( !pte_none(entry) ) {
			if(_cpu!=request->tgroup_home_cpu || request->is_fetch==1){
				printk("%s: ERROR: incorrect request for marked page (cpu %d id %d address 0x%lx)\n",
						__func__, request->tgroup_home_cpu, request->tgroup_home_id, address);
				goto out;
			}
			else {
				PSPRINTK("request for a marked page\n");
			}
		}
		if ( (_cpu==request->tgroup_home_cpu) || memory->alive != 0) {
			fetched_data = find_mapping_entry(
				request->tgroup_home_cpu, request->tgroup_home_id, address);

			//case concurrent fetch
			if (fetched_data != NULL) {
fetch:
				PSPRINTK("concurrent request\n");
				/*Whit marked pages only two scenarios can happenn:
				 * Or I am the main and I an locally fetching=> delay this fetch
				 * Or I am not the main, but the main already answer to my fetch (otherwise it will not answer to me the page)
				 * so wait that the answer arrive before consuming the fetch.
				 * */
				if (fetched_data->is_fetch != 1)
					printk("%s: ERROR: find a mapping_answers_for_2_kernels_t not mapped and not fetch (cpu %d id %d address 0x%lx)\n",
							__func__, request->tgroup_home_cpu, request->tgroup_home_id, address);

				delay = (request_work_t*)kmalloc(sizeof(request_work_t), GFP_ATOMIC);
				if (delay) {
					delay->request = request;
					INIT_DELAYED_WORK(
						(struct delayed_work*)delay,
						process_mapping_request_for_2_kernels);
					queue_delayed_work(message_request_wq,
							   (struct delayed_work*) delay, 10);
				}
				else {
					printk("%s: ERROR: cannot allocate memory to delay work 2 (cpu %d id %d)\n",
							__func__, request->tgroup_home_cpu, request->tgroup_home_id);
				}

				spin_unlock(ptl);
				up_read(&mm->mmap_sem);
				kfree(work);
				return;
			}
			else {
				//mark the pte if main
				if (_cpu==request->tgroup_home_cpu) {
					PSPRINTK(KERN_ALERT"%s: marking a pte for address %lx\n",__func__,address);

#if defined(CONFIG_ARM64)
                                        //Ajith - Removing optimization used for local fetch - _PAGE_UNUSED1 case
                                        //entry = pte_set_flags(entry, _PAGE_UNUSED1);
#else
					entry = pte_set_flags(entry, _PAGE_UNUSED1);
#endif
					ptep_clear_flush(vma, address, pte);

					set_pte_at_notify(mm, address, pte, entry);
					//in x86 does nothing
					update_mmu_cache(vma, address, pte);

					flush_tlb_page(vma, address);
					flush_tlb_fix_spurious_fault(vma, address);
				}
			}
		}
		//pte not present
		owner= 1;
		goto out;
	}
	page = pte_page(entry);
	if (page != vm_normal_page(vma, address, entry)) {
		PSPRINTK("Page different from vm_normal_page in request page\n");
	}
	old_page = NULL;
///////////////////////////////////////////////////////////////////////////////
// Got page, proceed with the rest

	if (is_zero_page(pte_pfn(entry)) || !(page->replicated == 1)) {
		PSPRINTK("Page not replicated\n");

		/*There is the possibility that this request arrived while I am fetching, after that I installed the page
		 * but before calling the update page....
		 * */
		if (memory->alive != 0) {
			fetched_data = find_mapping_entry (
					request->tgroup_home_cpu, request->tgroup_home_id, address);

			if (fetched_data!=NULL) {
				goto fetch;
			}
		}

		//the request must be for a fetch
		if (request->is_fetch==0)
			printk("%s: WARN: received a request not fetch for a not replicated page (cpu %d id %d address 0x%lx)\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, address);

		if ((vma->vm_flags & VM_WRITE) ||
				(vma->vm_start <= mm->context.popcorn_vdso && mm->context.popcorn_vdso < vma->vm_end) ) {
			//if the page is writable but the pte has not the write flag set, it is a cow page
			if (!pte_write(entry) &&
					!(vma->vm_start <= mm->context.popcorn_vdso && mm->context.popcorn_vdso < vma->vm_end) ) {
retry_cow:
				PSPRINTK("COW page at %lx\n", address);

				int ret= do_wp_page_for_popcorn(mm, vma,address, pte,pmd,ptl, entry);
				if (ret & VM_FAULT_ERROR) {
					if (ret & VM_FAULT_OOM){
						printk("ERROR: %s VM_FAULT_OOM\n",__func__);
						up_read(&mm->mmap_sem);
						goto out;
					}
					if (ret & (VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE)){
						printk("ERROR: %s EHWPOISON\n",__func__);
						up_read(&mm->mmap_sem);
						goto out;
					}
					if (ret & VM_FAULT_SIGBUS){
						printk("ERROR: %s EFAULT\n",__func__);
						up_read(&mm->mmap_sem);
						goto out;
					}
					printk("%s: ERROR: bug from do_wp_page_for_popcorn (cpu %d id %d address 0x%lx)\n",
							__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address);
					up_read(&mm->mmap_sem);
					goto out;
				}
				spin_lock(ptl);
				/*PTE LOCKED*/
				lock = 1;
				entry = *pte;

				if (!pte_write(entry)) {
					printk("%s: WARN: page not writable after cow (cpu %d id %d address 0x%lx)\n",
							__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address);
					goto retry_cow;
				}
				page = pte_page(entry);
			}

			page->replicated = 1;
			flush_cache_page(vma, address, pte_pfn(*pte));
			entry = mk_pte(page, vma->vm_page_prot);

			if (request->is_write==0) {
				//case fetch for read
				page->status = REPLICATION_STATUS_VALID;
#if defined(CONFIG_ARM64)
				entry = pte_wrprotect(entry);
				entry = pte_set_valid_entry_flag(entry);
#else
				entry = pte_clear_flags(entry, _PAGE_RW);
				entry = pte_set_flags(entry, _PAGE_PRESENT);
#endif
				owner= 0;
				page->owner= 1;
			}
			else {
				//case fetch for write
				page->status = REPLICATION_STATUS_INVALID;
#if defined(CONFIG_ARM64)
				entry = pte_clear_valid_entry_flag(entry);
#else
				entry = pte_clear_flags(entry, _PAGE_PRESENT);
#endif
				owner= 1;
				page->owner= 0;
			}
			page->last_write= 1;
			//printk("%s: INFO putting page 0x%lx last_write to 1 @0x%lx VM_WRITE\n", __func__, (unsigned long)page, request->address);

#if defined(CONFIG_ARM64)
			entry = pte_set_user_access_flag(entry);
			entry = pte_mkyoung(entry);
#else
			entry = pte_set_flags(entry, _PAGE_USER);
			entry = pte_set_flags(entry, _PAGE_ACCESSED);
#endif

			ptep_clear_flush(vma, address, pte);
			set_pte_at_notify(mm, address, pte, entry);

			//in x86 does nothing
			update_mmu_cache(vma, address, pte);

			flush_tlb_page(vma, address);
			flush_tlb_fix_spurious_fault(vma, address);
			if (old_page != NULL) {
				page_remove_rmap(old_page);
			}
		}
		else {
			//read only vma
			page->replicated=0;
			page->status= REPLICATION_STATUS_NOT_REPLICATED;

			if (request->is_write==1) {
				printk("%s: ERROR: received a write in a read-only not replicated page(cpu %d id %d address 0x%lx)\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address);
			}
			page->owner= 1;
			owner= 0;
		}

		page->other_owners[_cpu]=1;
		page->other_owners[from_cpu]=1;

		goto resolved;
	}
	else {
		//replicated page case
		PSPRINTK("Page replicated...\n");

		if (request->is_fetch==1) {
			printk("%s: ERROR: received a fetch request in a replicated status (cpu %d id %d address 0x%lx) w:%dr:%ds:%s\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address,
					page->writing, page->reading,
					(page->status == REPLICATION_STATUS_INVALID) ? "I" :
							((page->status == REPLICATION_STATUS_VALID) ? "V" :
							((page->status == REPLICATION_STATUS_WRITTEN) ? "W" :
							((page->status == REPLICATION_STATUS_NOT_REPLICATED) ? "N" : "?"))) );
			//BUG(); // TODO removed for debugging but really needed here
		}
		if (page->writing==1) {
			PSPRINTK("Page currently in writing\n");

			delay = (request_work_t*)kmalloc(sizeof(request_work_t), GFP_ATOMIC);
			if (delay) {
				delay->request = request;
				INIT_DELAYED_WORK( (struct delayed_work*)delay,
						   process_mapping_request_for_2_kernels);
				queue_delayed_work(message_request_wq,
						   (struct delayed_work*) delay, 10);
			}
			else {
				printk("%s: ERROR: cannot allocate memory to delay work 3 (cpu %d id %d)\n",
						__func__, request->tgroup_home_cpu, request->tgroup_home_id);
			}

			spin_unlock(ptl);
			up_read(&mm->mmap_sem);
			kfree(work);
			return;
		}
		if (page->reading==1) {
			printk("%s: ERROR: page in reading but received a request (cpu %d id %d address 0x%lx) w:%dr:%ds:%s\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address,
					page->writing, page->reading,
					(page->status == REPLICATION_STATUS_INVALID) ? "I" :
							((page->status == REPLICATION_STATUS_VALID) ? "V" :
							((page->status == REPLICATION_STATUS_WRITTEN) ? "W" :
							((page->status == REPLICATION_STATUS_NOT_REPLICATED) ? "N" : "?"))) );
			//BUG(); // TODO removed for debugging but really needed here
			goto out;
		}

		//invalid page case
		if (page->status == REPLICATION_STATUS_INVALID) {
			printk("%s: ERROR: received a request in invalid status without reading or writing (cpu %d id %d address 0x%lx) w:%dr:%ds:%s\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address,
					page->writing, page->reading,
					(page->status == REPLICATION_STATUS_INVALID) ? "I" :
						((page->status == REPLICATION_STATUS_VALID) ? "V" :
						((page->status == REPLICATION_STATUS_WRITTEN) ? "W" :
						((page->status == REPLICATION_STATUS_NOT_REPLICATED) ? "N" : "?"))) );
			goto out;
		}

		//valid page case
		if (page->status == REPLICATION_STATUS_VALID) {
			PSPRINTK("Page requested valid\n");

			if (page->owner!=1) {
				printk("%s: ERROR: request in a not owner valid page (cpu %d id %d address 0x%lx)w:%dr:%ds:%s\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address,
					page->writing, page->reading,
					(page->status == REPLICATION_STATUS_INVALID) ? "I" :
							((page->status == REPLICATION_STATUS_VALID) ? "V" :
							((page->status == REPLICATION_STATUS_WRITTEN) ? "W" :
							((page->status == REPLICATION_STATUS_NOT_REPLICATED) ? "N" : "?"))) );
			}
			else {
				if (request->is_write) {
					if (page->last_write!= request->last_write)
						printk("%s: ERROR: received a write for copy %lx but my copy is %lx\n",
								__func__, request->last_write, page->last_write);

					page->status= REPLICATION_STATUS_INVALID;
					page->owner= 0;
					owner= 1;
					entry = *pte;
#if defined(CONFIG_ARM64)
					entry = pte_clear_valid_entry_flag(entry);
					entry = pte_mkyoung(entry);
#else
					entry = pte_clear_flags(entry, _PAGE_PRESENT);
					entry = pte_set_flags(entry, _PAGE_ACCESSED);
#endif

					ptep_clear_flush(vma, address, pte);
					set_pte_at_notify(mm, address, pte, entry);

					update_mmu_cache(vma, address, pte);
					flush_tlb_page(vma, address);
					flush_tlb_fix_spurious_fault(vma, address);
				}
				else {
					printk("%s: ERROR: received a read request in valid status (cpu %d id %d address 0x%lx)w:%dr:%ds:%s\n",
							__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address,
							page->writing, page->reading,
							(page->status == REPLICATION_STATUS_INVALID) ? "I" :
									((page->status == REPLICATION_STATUS_VALID) ? "V" :
											((page->status == REPLICATION_STATUS_WRITTEN) ? "W" :
													((page->status == REPLICATION_STATUS_NOT_REPLICATED) ? "N" : "?"))) );
				}
			}
			goto out;
		}

		if (page->status == REPLICATION_STATUS_WRITTEN) {
			PSPRINTK("Page requested in written status\n");

			if (page->owner!=1) {
				printk("%s: ERROR: page in written status without ownership (cpu %d id %d address 0x%lx) w:%dr:%ds:%s\n",
					__func__, request->tgroup_home_cpu, request->tgroup_home_id, request->address,
					page->writing, page->reading,
					(page->status == REPLICATION_STATUS_INVALID) ? "I" :
							((page->status == REPLICATION_STATUS_VALID) ? "V" :
							((page->status == REPLICATION_STATUS_WRITTEN) ? "W" :
							((page->status == REPLICATION_STATUS_NOT_REPLICATED) ? "N" : "?"))) );
			}
			else {
				if (request->is_write==1) {
					if (page->last_write!= (request->last_write+1))
						printk("%s: ERROR: received a write for copy %lx but my copy is %lx (cpu %d id %d address 0x%lx)\n",
								__func__, request->last_write,page->last_write,
								request->tgroup_home_cpu, request->tgroup_home_id, request->address );

					page->status= REPLICATION_STATUS_INVALID;
					page->owner= 0;
					owner= 1;
					entry = *pte;
#if defined(CONFIG_ARM64)
					entry = pte_clear_valid_entry_flag(entry);
					entry = pte_mkyoung(entry);
#else
					entry = pte_clear_flags(entry, _PAGE_PRESENT);
					entry = pte_set_flags(entry, _PAGE_ACCESSED);
#endif

					ptep_clear_flush(vma, address, pte);
					set_pte_at_notify(mm, address, pte, entry);
					update_mmu_cache(vma, address, pte);
					flush_tlb_page(vma, address);
					flush_tlb_fix_spurious_fault(vma, address);
				}
				else {
					if (page->last_write!= (request->last_write+1))
						printk("%s: ERROR: received an read for copy %lx but my copy is %lx (cpu %d id %d address 0x%lx)\n",
								__func__, request->last_write,page->last_write,
								request->tgroup_home_cpu, request->tgroup_home_id, request->address);

					page->status = REPLICATION_STATUS_VALID;
					page->owner= 1;
					owner= 0;
					entry = *pte;
#if defined(CONFIG_ARM64)
					entry = pte_set_valid_entry_flag(entry);
					entry = pte_mkyoung(entry);
					entry = pte_wrprotect(entry);
#else
					entry = pte_set_flags(entry, _PAGE_PRESENT);
					entry = pte_set_flags(entry, _PAGE_ACCESSED);
					entry = pte_clear_flags(entry, _PAGE_RW);
#endif
					ptep_clear_flush(vma, address, pte);
					set_pte_at_notify(mm, address, pte, entry);
					update_mmu_cache(vma, address, pte);

					flush_tlb_page(vma, address);
					flush_tlb_fix_spurious_fault(vma, address);
				}
			}
			goto resolved;
		}
	}
///////////////////////////////////////////////////////////////////////////////
// Everything resolved at this point, going to respond

resolved:
	PSPRINTK("Resolved Copy from %s\n",
			((vma->vm_file!=NULL)?d_path(&vma->vm_file->f_path,lpath,512):"no file"));
	PSPRINTK("Page read only?%i Page shared?%i\n",
			(vma->vm_flags & VM_WRITE)?0:1, (vma->vm_flags & VM_SHARED)?1:0);

	response = (data_response_for_2_kernels_t*) kmalloc(sizeof(data_response_for_2_kernels_t)+PAGE_SIZE, GFP_ATOMIC);
	if (response == NULL) {
		printk("%s: ERROR: Impossible to kmalloc in process mapping request.\n", __func__); //THIS CASE SHOULD KILL THE PROCESS
		spin_unlock(ptl);
		up_read(&mm->mmap_sem);
		pcn_kmsg_free_msg(request);
		kfree(work);
		return;
	}

	void* vto = &(response->data);
	// Ported to Linux 3.12
	//vfrom = kmap_atomic(page, KM_USER0);
	vfrom = kmap_atomic(page);

#if READ_PAGE
	int ct=0;
	unsigned long _buff[16];

	if (address == PAGE_ADDR) {
		for (ct=0;ct<8;ct++) {
			_buff[ct]=(unsigned long) *(((unsigned long *)vfrom) + ct);
		}
	}
#endif

	//printk("Copying page (address) : 0x%lx\n", address);
	copy_page(vto, vfrom);
	// Ported to Linux 3.12
	//kunmap_atomic(vfrom, KM_USER0);
	kunmap_atomic(vfrom);
	response->data_size= PAGE_SIZE;


#if READ_PAGE
	if (address == PAGE_ADDR) {
		for(ct=8;ct<16;ct++) {
			_buff[ct]=(unsigned long) *((unsigned long*)(&(response->data))+ct-8);
		}
		for(ct=0;ct<16;ct++){
			printk(KERN_ALERT"{%lx} ",_buff[ct]);
		}
	}
#endif

	flush_cache_page(vma, address, pte_pfn(*pte));
	response->last_write = page->last_write;
	response->header.type = PCN_KMSG_TYPE_PROC_SRV_MAPPING_RESPONSE;
	response->header.prio = PCN_KMSG_PRIO_NORMAL;
	response->tgroup_home_cpu = request->tgroup_home_cpu;
	response->tgroup_home_id = request->tgroup_home_id;
	response->address = request->address;
	response->owner= owner;

	response->futex_owner = (!page) ? 0 : page->futex_owner;//akshay

	if (_cpu == request->tgroup_home_cpu && vma != NULL) {
		//only the vmas SERVER sends the vma

		response->vma_present = 1;
		response->vaddr_start = vma->vm_start;
		response->vaddr_size = vma->vm_end - vma->vm_start;
		response->prot = vma->vm_page_prot;
		response->vm_flags = vma->vm_flags;
		response->pgoff = vma->vm_pgoff;
		if (vma->vm_file == NULL) {
			response->path[0] = '\0';
		}
		else {
			plpath = d_path(&vma->vm_file->f_path, lpath, 512);
			strcpy(response->path, plpath);
		}
		PSPRINTK("response->vma_present %d response->vaddr_start %lx response->vaddr_size %lx response->prot %lx response->vm_flags %lx response->pgoff %lx response->path %s response->futex_owner %d\n",
			 response->vma_present, response->vaddr_start , response->vaddr_size,response->prot, response->vm_flags , response->pgoff, response->path,response->futex_owner);
	}
	else {
		response->vma_present = 0;
	}
	spin_unlock(ptl);
	up_read(&mm->mmap_sem);

	// Send response
	pcn_kmsg_send_long(from_cpu, (struct pcn_kmsg_long_message*) (response),
			   sizeof(data_response_for_2_kernels_t) - sizeof(struct pcn_kmsg_hdr) + response->data_size);
	// Clean up incoming messages
	pcn_kmsg_free_msg(request);
	kfree(work);
	kfree(response);
	//end= native_read_tsc();
	PSPRINTK("Handle request end\n");
	return;

out:
	PSPRINTK("sending void answer\n");

/*	char tmpchar; // Marina and Vincent debugging
	char *addrp = (char *) (address & PAGE_MASK);
	int ii;
	for (ii = 1; ii < PAGE_SIZE; ii++) {
		copy_from_user(&tmpchar, addrp++, 1);
	}*/

#if 0
	void* vto2 = kmalloc(GFP_ATOMIC, PAGE_SIZE * 2);

	// Ported to Linux 3.12
	//vfrom = kmap_atomic(page, KM_USER0);
	vfrom = kmap_atomic(page);
	//printk("Copying page (address) : 0x%lx\n", address);
	copy_page(vto2, vfrom);
	// Ported to Linux 3.12
	//kunmap_atomic(vfrom, KM_USER0);
	kunmap_atomic(vfrom);
	kfree(vto2);
#endif

	void_response = (data_void_response_for_2_kernels_t*) kmalloc(
		sizeof(data_void_response_for_2_kernels_t), GFP_ATOMIC);
	if (void_response == NULL) {
		if (lock) {
			spin_unlock(ptl);
			up_read(&mm->mmap_sem);
		}
		printk("%s: ERROR: Impossible to kmalloc in process mapping request.\n", __func__);
		pcn_kmsg_free_msg(request);
		kfree(work);
		return;
	}

	void_response->header.type = PCN_KMSG_TYPE_PROC_SRV_MAPPING_RESPONSE_VOID;
	void_response->header.prio = PCN_KMSG_PRIO_NORMAL;
	void_response->tgroup_home_cpu = request->tgroup_home_cpu;
	void_response->tgroup_home_id = request->tgroup_home_id;
	void_response->address = request->address;
	void_response->owner=owner;
	void_response->futex_owner = 0;//TODO: page->futex_owner;//akshay

	if (_cpu == request->tgroup_home_cpu && vma != NULL) {
		void_response->vma_present = 1;
		void_response->vaddr_start = vma->vm_start;
		void_response->vaddr_size = vma->vm_end - vma->vm_start;
		void_response->prot = vma->vm_page_prot;
		void_response->vm_flags = vma->vm_flags;
		void_response->pgoff = vma->vm_pgoff;
		if (vma->vm_file == NULL) {
			void_response->path[0] = '\0';
		}
		else {
			plpath = d_path(&vma->vm_file->f_path, lpath, 512);
			strcpy(void_response->path, plpath);
		}
	}
	else {
		void_response->vma_present = 0;
	}

	if(lock) {
		spin_unlock(ptl);
		up_read(&mm->mmap_sem);
	}

	// Send response
	pcn_kmsg_send_long(from_cpu,
			   (struct pcn_kmsg_long_message*) (void_response),
			   sizeof(data_void_response_for_2_kernels_t) - sizeof(struct pcn_kmsg_hdr));
	// Clean up incoming messages
	pcn_kmsg_free_msg(request);
	kfree(void_response);
	kfree(work);
	//end= native_read_tsc();
	PSPRINTK("Handle request end\n");
}

static int handle_mapping_request(struct pcn_kmsg_message* inc_msg)
{
	request_work_t* request_work;

	data_request_for_2_kernels_t* request = (data_request_for_2_kernels_t*) inc_msg;

	request_work = kmalloc(sizeof(request_work_t), GFP_ATOMIC);

	if (request_work) {
		request_work->request = request;
		INIT_WORK( (struct work_struct*)request_work, process_mapping_request_for_2_kernels);
		queue_work(message_request_wq, (struct work_struct*) request_work);
	}

	return 1;
}

///////////////////////////////////////////////////////////////////////////////
// update page function
///////////////////////////////////////////////////////////////////////////////

/* No other kernels had the page during the remote fetch => a local fetch has been performed.
 * If during the local fetch a thread in another kernel asks for this page,
 * I would not set the page as replicated. This function check if the page
 * should be set as replicated.
 *
 * the mm->mmap_sem semaphore is already held in read
 * return types:
 * VM_FAULT_OOM, problem allocating memory.
 * VM_FAULT_VMA, error vma management.
 * VM_FAULT_REPLICATION_PROTOCOL, replication protocol error.
 * 0, updated;
 */
int page_server_update_page(struct task_struct * tsk, struct mm_struct *mm,
			       struct vm_area_struct *vma, unsigned long address_not_page, unsigned long page_fault_flags,
				   int retrying)
{
	unsigned long address;

	pgd_t* pgd;
	pud_t* pud;
	pmd_t* pmd;
	pte_t* pte;
	pte_t entry;
	spinlock_t* ptl = NULL;
	struct page* page;
	int ret = 0;

	mapping_answers_for_2_kernels_t* fetched_data;

	address = address_not_page & PAGE_MASK;

	if (!vma || address >= vma->vm_end || address < vma->vm_start) {
		printk("%s: ERROR: updating a page without valid vma (cpu %d id %d)\n",
				__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		ret = VM_FAULT_VMA;
		goto out_not_data;
	}

	if (unlikely(is_vm_hugetlb_page(vma))
	    || unlikely(transparent_hugepage_enabled(vma))) {
		printk("%s: ERROR: Installed a vma with HUGEPAGE (cpu %d id %d)\n",
				__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		ret = VM_FAULT_VMA;
		goto out_not_data;
	}

	fetched_data = find_mapping_entry(tsk->tgroup_home_cpu, tsk->tgroup_home_id,
					  address);

	if (fetched_data != NULL) {
		if (retrying == 1) {
			ret = 0;
			goto out_not_locked;
		}

		if(fetched_data->is_fetch!=1 ){
			printk("%s: ERROR: data structure is not for fetch (cpu %d id %d)\n",
					__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
			ret = VM_FAULT_REPLICATION_PROTOCOL;
			goto out_not_locked;
		}

		pgd = pgd_offset(mm, address);
		pud = pud_offset(pgd, address);
		if (!pud) {
			printk("%s: ERROR: no pud while trying to update a page locally fetched (cpu %d id %d)\n",
					__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
			ret = VM_FAULT_VMA;
			goto out_not_locked;
		}
		pmd = pmd_offset(pud, address);
		if (!pmd) {
			printk("%s: ERROR: no pmd while trying to update a page locally fetched (cpu %d id %d)\n",
					__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
			ret = VM_FAULT_VMA;
			goto out_not_locked;
		}
//retry:
		pte = pte_offset_map_lock(mm, pmd, address, &ptl);
		entry= *pte;
		page = pte_page(entry);

		//I replicate only if it is a writable page
		if (vma->vm_flags & VM_WRITE) {
			if (!pte_write(entry)) {
retry_cow:
				PSPRINTK("COW page at %lx\n", address);

				int cow_ret= do_wp_page_for_popcorn(mm, vma,address, pte,pmd,ptl, entry);
				if (cow_ret & VM_FAULT_ERROR) {
					if (cow_ret & VM_FAULT_OOM){
						printk("%s: ERROR: VM_FAULT_OOM\n",__func__);
						ret = VM_FAULT_OOM;
						goto out_not_locked;
					}
					if (cow_ret & (VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE)){
						printk("%s: ERROR: EHWPOISON\n",__func__);
						ret = VM_FAULT_OOM;
						goto out_not_locked;
					}
					if (cow_ret & VM_FAULT_SIGBUS){
						printk("%s: ERROR: EFAULT\n",__func__);
						ret = VM_FAULT_OOM;
						goto out_not_locked;
					}
					printk("%s: ERROR: bug from do_wp_page_for_popcorn\n",__func__);
					ret = VM_FAULT_OOM;
					goto out_not_locked;
				}

				spin_lock(ptl);
				/*PTE LOCKED*/
				entry = *pte;

				if(!pte_write(entry)){
					printk("%s: WARNING: page not writable after cow\n", __func__);
					goto retry_cow;
				}
				page = pte_page(entry);
			}

			page->replicated = 0;
			page->owner= 1;
			page->other_owners[_cpu] = 1;
		}
		else {
			page->replicated = 0;
			page->other_owners[_cpu] = 1;
			page->owner= 1;
		}
		PSPRINTK("%s: INFO current %p page %p r:0 o:1 (cpu %d id %d address 0x%lx)\n",
				__func__, current, page, tsk->tgroup_home_cpu, tsk->tgroup_home_id, address);
	}
	else {
		printk("%s: ERROR: impossible to find data to update (cpu %d id %d address 0x%lx)\n",
				__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id, address);
		ret = VM_FAULT_REPLICATION_PROTOCOL;
		goto out_not_data;
	}
	spin_unlock(ptl);

out_not_locked:
	remove_mapping_entry(fetched_data);
	kfree(fetched_data);
out_not_data:
	wake_up(&read_write_wait);

	return ret;
}

void page_server_clean_page(struct page* page)
{
	if (page == NULL) {
		return;
	}

	page->replicated = 0;
	page->status = REPLICATION_STATUS_NOT_REPLICATED;
	page->owner = 0;
	memset(page->other_owners, 0, MAX_KERNEL_IDS*sizeof(int));
	page->writing = 0;
	page->reading = 0;
}

/* Read on a REPLICATED page => ask a copy of the page at address "address" on the
 * virtual mapping of the process identified by "tgroup_home_cpu" and "tgroup_home_id".
 *
 * down_read(&mm->mmap_sem) must be held.
 * pte lock must be held.
 *
 *return types:
 *VM_FAULT_OOM, problem allocating memory.
 *VM_FAULT_VMA, error vma management.
 *VM_FAULT_REPLICATION_PROTOCOL, general error.
 *0, write succeeded;
 * */
static int do_remote_read_for_2_kernels(struct task_struct * tsk, int tgroup_home_cpu, int tgroup_home_id,
				 struct mm_struct *mm, struct vm_area_struct *vma,
				 unsigned long address, unsigned long page_fault_flags,
				 pmd_t* pmd, pte_t* pte,
				 spinlock_t* ptl, struct page* page)
{
	int i, ret=0;
	pte_t value_pte;
	page->reading= 1;

#if STATISTICS
	read++;
#endif
	PSPRINTK("%s Read for address %lx pid %d\n", __func__, address, tsk->pid);

	//message to ask for a copy
	data_request_for_2_kernels_t* read_message = (data_request_for_2_kernels_t*) kmalloc(sizeof(data_request_for_2_kernels_t),
											     GFP_ATOMIC);
	if (read_message == NULL) {
		ret = VM_FAULT_OOM;
		goto exit;
	}

	read_message->header.type = PCN_KMSG_TYPE_PROC_SRV_MAPPING_REQUEST;
	read_message->header.prio = PCN_KMSG_PRIO_NORMAL;
	read_message->address = address;
	read_message->tgroup_home_cpu = tgroup_home_cpu;
	read_message->tgroup_home_id = tgroup_home_id;
	read_message->is_fetch= 0;
	read_message->is_write= 0;
	read_message->last_write= page->last_write;
	read_message->vma_operation_index= mm->vma_operation_index;
	PSPRINTK("%s vma_operation_index %d\n", __func__, read_message->vma_operation_index);

	//object to held responses
	mapping_answers_for_2_kernels_t* reading_page = (mapping_answers_for_2_kernels_t*) kmalloc(sizeof(mapping_answers_for_2_kernels_t),
												   GFP_ATOMIC);
	if (reading_page == NULL) {
		ret = VM_FAULT_OOM;
		goto exit_read_message;
	}

	reading_page->tgroup_home_cpu= tgroup_home_cpu;
	reading_page->tgroup_home_id= tgroup_home_id;
	reading_page->address = address;
	reading_page->address_present= 0;
	reading_page->data= NULL;
	reading_page->is_fetch= 0;
	reading_page->is_write= 0;
	reading_page->last_write= page->last_write;
	reading_page->owner= 0;

	reading_page->vma_present = 0;
	reading_page->vaddr_start = 0;
	reading_page->vaddr_size = 0;
	reading_page->pgoff = 0;
	memset(reading_page->path,0,sizeof(char)*512);
	memset(&(reading_page->prot),0,sizeof(pgprot_t));
	reading_page->vm_flags = 0;
	reading_page->waiting = current;

	// Make data entry visible to handler.
	add_mapping_entry(reading_page);
	PSPRINTK("Sending a read message for address %lx\n ", address);
	spin_unlock(ptl);
	up_read(&mm->mmap_sem);
	/*PTE UNLOCKED*/
	int sent= 0;
	reading_page->arrived_response=0;

	// the list does not include the current processor group descirptor (TODO)
	struct list_head *iter, *tmp_iter;
	_remote_cpu_info_list_t *objPtr;

	list_for_each_safe(iter, tmp_iter, &rlist_head) {
		objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
		i = objPtr->_data._processor;

		if (!(pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) (read_message),
					 sizeof(data_request_for_2_kernels_t)-sizeof(struct pcn_kmsg_hdr)) == -1)) {
			// Message delivered
			sent++;
			if (sent>1)
				printk("%s: ERROR: using protocol optimized for 2 kernels but sending a read to more than one kernel",
						__func__);
		}
	}

	if (sent) {
		long counter = 0;
		while (reading_page->arrived_response == 0) {
			set_task_state(current, TASK_UNINTERRUPTIBLE); // TODO put it back
			if (reading_page->arrived_response == 0)
				schedule();
			set_task_state(current, TASK_RUNNING); // TODO put it back
			if (!(++counter % RATE_DO_REMOTE_OPERATION))
				printk("%s: WARN: reading_page->arrived_response 0 [%ld] (cpu %d id %d address 0x%lx)\n",
						__func__, counter, tgroup_home_cpu, tgroup_home_id, address);
		}
	}
	else {
		printk("%s: ERROR: impossible to send read message, no destination kernel (cpu %d id %d)\n",
				__func__, tgroup_home_cpu, tgroup_home_id);
		ret= VM_FAULT_REPLICATION_PROTOCOL;
		down_read(&mm->mmap_sem);
		spin_lock(ptl);
		goto exit_reading_page;
	}
///////////////////////////////////////////////////////////////////////////////
// request sent response received

	down_read(&mm->mmap_sem);
	spin_lock(ptl);
	/*PTE LOCKED*/

	vma = find_vma(mm, address);
	if (unlikely(!vma || address >= vma->vm_end || address < vma->vm_start)) {
		printk("%s: ERROR: vma not valid during read for write (cpu %d id %d address 0x%lx) fault_flag:0x%lx\n",
				__func__, tgroup_home_cpu, tgroup_home_id, address, page_fault_flags);
		ret = VM_FAULT_VMA;
		goto exit_reading_page;
	}

	if (reading_page->address_present==1) {
		if (reading_page->data->address != address) {
			printk("%s: ERROR: trying to copy wrong address! (cpu %d id %d address 0x%lx) fault:0x%lx r:%xs:%xo:%xw:%xr:%x\n",
				__func__, tgroup_home_cpu, tgroup_home_id, address, page_fault_flags,
				page->replicated, page->status, page->owner, page->writing, page->reading);
			pcn_kmsg_free_msg(reading_page->data);
			ret = VM_FAULT_REPLICATION_PROTOCOL;
			goto exit_reading_page;
		}

		if (reading_page->last_write != (page->last_write+1)) {
			printk("%s: ERROR: new copy received during a read but my last write is %lx and received last write is %lx (cpu %d id %d address 0x%lx) fault:0x%lx r:%xs:%xo:%xw:%xr:%x\n",
			       __func__, page->last_write,reading_page->last_write, tgroup_home_cpu, tgroup_home_id, address, page_fault_flags,
					page->replicated, page->status, page->owner, page->writing, page->reading);
			ret = VM_FAULT_REPLICATION_PROTOCOL;
			goto exit_reading_page;
		}
		else {
			page->last_write= reading_page->last_write;
		}

		if (reading_page->owner==1) {
			printk("%s: ERROR: ownership sent with read request (cpu %d id %d address 0x%lx) fault:0x%lx r:%xs:%xo:%xw:%xr:%x\n",
				__func__, tgroup_home_cpu, tgroup_home_id, address, page_fault_flags,
				page->replicated, page->status, page->owner, page->writing, page->reading);
			ret = VM_FAULT_REPLICATION_PROTOCOL;
			goto exit_reading_page;
		}

		void *vto;
		void *vfrom;
		// Ported to Linux 3.12
		//vto = kmap_atomic(page, KM_USER0);
		vto = kmap_atomic(page);
		vfrom = &(reading_page->data->data);

		copy_user_page(vto, vfrom, address, page);

		// Ported to Linux 3.12
		//kunmap_atomic(vto, KM_USER0);
		kunmap_atomic(vto);

		pcn_kmsg_free_msg(reading_page->data);

		page->status = REPLICATION_STATUS_VALID;
		page->owner = reading_page->owner;

#if STATISTICS
		if(page->last_write> most_written_page)
			most_written_page= page->last_write;
#endif

		flush_cache_page(vma, address, pte_pfn(*pte));
		//now the page can be written
		value_pte = *pte;
#if defined(CONFIG_ARM64)
		value_pte = pte_wrprotect(value_pte);
		value_pte = pte_set_valid_entry_flag(value_pte);
		value_pte = pte_mkyoung(value_pte);
#else
		value_pte = pte_clear_flags(value_pte, _PAGE_RW);
		value_pte = pte_set_flags(value_pte, _PAGE_PRESENT);
		value_pte = pte_set_flags(value_pte, _PAGE_ACCESSED);
#endif
		ptep_clear_flush(vma, address, pte);
		set_pte_at_notify(mm, address, pte, value_pte);
		update_mmu_cache(vma, address, pte);
		flush_tlb_page(vma, address);
		flush_tlb_fix_spurious_fault(vma, address);

		PSPRINTK("%s: Out read %i address %lx\n ", __func__, 0, address);
	}
	else {
		printk("%s: ERROR: no copy received for a read (cpu %d id %d address 0x%lx)\n",
				__func__, tgroup_home_cpu, tgroup_home_id, address);
		ret= VM_FAULT_REPLICATION_PROTOCOL;
		remove_mapping_entry(reading_page);
		kfree(reading_page);
		kfree(read_message);
		goto exit;

	}
exit_reading_page:
	remove_mapping_entry(reading_page);
	kfree(reading_page);

exit_read_message:
	kfree(read_message);

exit:
	page->reading = 0;
	return ret;
}

/* Write on a REPLICATED page => coordinate with other kernels to write on the page at address "address" on the
 * virtual mapping of the process identified by "tgroup_home_cpu" and "tgroup_home_id".
 *
 * down_read(&mm->mmap_sem) must be held.
 * pte lock must be held.
 *
 *return types:
 *VM_FAULT_OOM, problem allocating memory.
 *VM_FAULT_VMA, error vma management.
 *VM_FAULT_REPLICATION_PROTOCOL, general error.
 *0, write succeeded;
 * */
static int do_remote_write_for_2_kernels(struct tast_struct * tsk, int tgroup_home_cpu, int tgroup_home_id,
				  struct mm_struct *mm, struct vm_area_struct *vma, unsigned long address,
				  unsigned long page_fault_flags, pmd_t* pmd, pte_t* pte, spinlock_t* ptl,
				  struct page* page,int invalid)
{
	int i, ret= 0;
	pte_t value_pte;
	page->writing = 1;

#if STATISTICS
	write++;
#endif
	PSPRINTK("%s: Write %i address %lx pid %d\n", __func__, 0, address, tsk->pid);
	PSMINPRINTK("Write for address %lx owner %d pid %d\n", address, page->owner==1?1:0, tsk->pid);

	if (page->owner==1) {
		int sent= 0;

		//in this case I send and invalid message
		if(invalid) {
			printk("%s: ERROR: I am the owner of the page and it is invalid when going to write (cpu %d id %d address 0x%lx)\n",
					__func__, tgroup_home_cpu, tgroup_home_id, address);
			ret= VM_FAULT_REPLICATION_PROTOCOL;
			goto exit;
		}
		//object to store the acks (nacks) sent by other kernels
		ack_answers_for_2_kernels_t* answers = (ack_answers_for_2_kernels_t*) kmalloc(sizeof(ack_answers_for_2_kernels_t), GFP_ATOMIC);
		if (answers == NULL) {
			ret = VM_FAULT_OOM;
			goto exit;
		}
		answers->tgroup_home_cpu = tgroup_home_cpu;
		answers->tgroup_home_id = tgroup_home_id;
		answers->address = address;
		answers->waiting = current;

		//message to invalidate the other copies
		invalid_data_for_2_kernels_t* invalid_message = (invalid_data_for_2_kernels_t*) kmalloc(sizeof(invalid_data_for_2_kernels_t),
													GFP_ATOMIC);
		if (invalid_message == NULL) {
			ret = VM_FAULT_OOM;
			goto exit_answers;
		}
		invalid_message->header.type = PCN_KMSG_TYPE_PROC_SRV_INVALID_DATA;
		invalid_message->header.prio = PCN_KMSG_PRIO_NORMAL;
		invalid_message->tgroup_home_cpu = tgroup_home_cpu;
		invalid_message->tgroup_home_id = tgroup_home_id;
		invalid_message->address = address;
		invalid_message->vma_operation_index= mm->vma_operation_index;

		// Insert the object in the appropriate list.
		add_ack_entry(answers);

		invalid_message->last_write = page->last_write;
		answers->response_arrived= 0;

		spin_unlock(ptl);
		up_read(&mm->mmap_sem);
		/*PTE UNLOCKED*/
		// the list does not include the current processor group descirptor (TODO)
		struct list_head *iter, *tmp_iter;
		_remote_cpu_info_list_t *objPtr;
		list_for_each_safe(iter, tmp_iter, &rlist_head) {
			objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
			i = objPtr->_data._processor;
			if (page->other_owners[i] == 1) {
				if (!(pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) (invalid_message),sizeof(invalid_data_for_2_kernels_t)-sizeof(struct pcn_kmsg_hdr))
				      == -1)) {
					// Message delivered
					sent++;
					if (sent>1)
						printk("%s: ERROR: using protocol optimized for 2 kernels but sending an invalid to more than one kernel",
								__func__);
				}
			}
		}

		if (sent) {
			long counter = 0;
			while (answers->response_arrived==0) {
				set_task_state(current, TASK_INTERRUPTIBLE); // TODO put it back
				if (answers->response_arrived==0)
					schedule();
				set_task_state(current, TASK_RUNNING); // TODO put it back
				if (!(++counter % RATE_DO_REMOTE_OPERATION))
					printk("%s: WARN: writing_page->arrived_response 0 [%ld] (cpu %d id %d address 0x%lx)\n",
							__func__, counter, tgroup_home_cpu, tgroup_home_id, address);
			}
		}
		else {
			printk("%s: ERROR: impossible to send read message, no destination kernel (cpu %d id %d)n",
					__func__, tgroup_home_cpu, tgroup_home_id);
			ret= VM_FAULT_REPLICATION_PROTOCOL;
			down_read(&mm->mmap_sem);
			spin_lock(ptl);
			goto exit_invalid;
		}

		down_read(&mm->mmap_sem);
		spin_lock(ptl);

		/*PTE LOCKED*/
		vma = find_vma(mm, address);
		if (unlikely(!vma || address >= vma->vm_end || address < vma->vm_start)) {
			printk("%s: ERROR: vma not valid after waiting for ack to invalid (cpu %d id %d)\n",
					__func__, tgroup_home_cpu, tgroup_home_id);
			ret = VM_FAULT_VMA;
			goto exit_invalid;
		}
		PSPRINTK("%s: Received ack to invalid %i address %lx\n", __func__, 0, address);

exit_invalid:
		kfree(invalid_message);
		remove_ack_entry(answers);
exit_answers:
		kfree(answers);
		if(ret!=0)
			goto exit;
	}
	else {
		//in this case I send a mapping request with write flag set
		//message to ask for a copy
		data_request_for_2_kernels_t* write_message = (data_request_for_2_kernels_t*) kmalloc(sizeof(data_request_for_2_kernels_t),
												      GFP_ATOMIC);
		if (write_message == NULL) {
			ret = VM_FAULT_OOM;
			goto exit;
		}

		write_message->header.type = PCN_KMSG_TYPE_PROC_SRV_MAPPING_REQUEST;
		write_message->header.prio = PCN_KMSG_PRIO_NORMAL;
		write_message->address = address;
		write_message->tgroup_home_cpu = tgroup_home_cpu;
		write_message->tgroup_home_id = tgroup_home_id;
		write_message->is_fetch= 0;
		write_message->is_write= 1;
		write_message->last_write= page->last_write;
		write_message->vma_operation_index= mm->vma_operation_index;
		PSPRINTK("%s vma_operation_index %d\n", __func__, write_message->vma_operation_index);

		//object to held responses
		mapping_answers_for_2_kernels_t* writing_page = (mapping_answers_for_2_kernels_t*) kmalloc(sizeof(mapping_answers_for_2_kernels_t),
													   GFP_ATOMIC);
		if (writing_page == NULL) {
			ret = VM_FAULT_OOM;
			goto exit_write_message;

		}

		writing_page->tgroup_home_cpu= tgroup_home_cpu;
		writing_page->tgroup_home_id= tgroup_home_id;
		writing_page->address = address;
		writing_page->address_present= 0;
		writing_page->data= NULL;
		writing_page->is_fetch= 0;
		writing_page->is_write= 1;
		writing_page->last_write= page->last_write;
		writing_page->owner= 0;
		writing_page->vma_present = 0;
		writing_page->vaddr_start = 0;
		writing_page->vaddr_size = 0;
		writing_page->pgoff = 0;
		memset(writing_page->path,0,sizeof(char)*512);
		memset(&(writing_page->prot),0,sizeof(pgprot_t));
		writing_page->vm_flags = 0;
		writing_page->waiting = current;

		// Make data entry visible to handler.
		add_mapping_entry(writing_page);
		PSPRINTK("Sending a write message for address %lx\n ", address);

		spin_unlock(ptl);
		up_read(&mm->mmap_sem);
		/*PTE UNLOCKED*/
		int sent= 0;
		writing_page->arrived_response=0;

		// the list does not include the current processor group descriptor (TODO)
		struct list_head *iter, *tmp_iter;
		_remote_cpu_info_list_t *objPtr;
		list_for_each_safe(iter, tmp_iter, &rlist_head) {
			objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
			i = objPtr->_data._processor;
			if (page->other_owners[i] == 1) {
				if (!(pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) (write_message),sizeof(data_request_for_2_kernels_t)-sizeof(struct pcn_kmsg_hdr))
				      == -1)) {
					// Message delivered
					sent++;
					if(sent>1)
						printk("%s: ERROR: using protocol optimized for 2 kernels but sending a write to more than one kernel",
								__func__);
				}
			}
		}

		if (sent) {
			long counter =0;
			while (writing_page->arrived_response == 0) {
				set_task_state(current, TASK_UNINTERRUPTIBLE); // TODO put it back
				if (writing_page->arrived_response == 0)
					schedule();
				set_task_state(current, TASK_RUNNING); // TODO put it back
				if (!(++counter % RATE_DO_REMOTE_OPERATION))
					printk("%s: WARN: writing_page->arrived_response 0 [%ld] !owner (cpu %d id %d address 0x%lx)\n",
							__func__, counter, tgroup_home_cpu, tgroup_home_id, address);
			}
		}
		else {
			printk("%s: ERROR: impossible to send write message, no destination kernel (cpu %d id %d)\n",
					__func__, tgroup_home_cpu, tgroup_home_id);
			ret= VM_FAULT_REPLICATION_PROTOCOL;
			down_read(&mm->mmap_sem);
			spin_lock(ptl);
			goto exit_writing_page;
		}

		down_read(&mm->mmap_sem);
		spin_lock(ptl);
		/*PTE LOCKED*/

		vma = find_vma(mm, address);
		if (unlikely(!vma || address >= vma->vm_end || address < vma->vm_start)) {
			printk("ERROR: vma not valid during read for write\n");
			ret = VM_FAULT_VMA;
			goto exit_writing_page;
		}

		if(writing_page->owner!=1){
			printk("%s: ERROR: received answer to write without ownership (cpu %d id %d)\n",
					__func__, tgroup_home_cpu, tgroup_home_id);
			ret = VM_FAULT_REPLICATION_PROTOCOL;
			goto exit_writing_page;
		}
		if(writing_page->address_present==1){
			if (writing_page->data->address != address) {
				printk("%s: ERROR: trying to copy wrong address 0x%lx (cpu %d id %d)\n",
						__func__, address, tgroup_home_cpu, tgroup_home_id);
				pcn_kmsg_free_msg(writing_page->data);
				ret = VM_FAULT_REPLICATION_PROTOCOL;
				goto exit_writing_page;
			}
			//in this case I also received the new copy
			if (writing_page->last_write != (page->last_write+1)) {
				pcn_kmsg_free_msg(writing_page->data);
				printk(
					"%s: ERROR: new copy received during a write but my last write is %lx and received last write is %lx\n",
					__func__, page->last_write,writing_page->last_write);
				ret = VM_FAULT_REPLICATION_PROTOCOL;
				goto exit_writing_page;
			}
			else
				page->last_write= writing_page->last_write;

			void *vto;
			void *vfrom;
			// Ported to Linux 3.12
			//vto = kmap_atomic(page, KM_USER0);
			vto = kmap_atomic(page);
			vfrom = &(writing_page->data->data);

			copy_user_page(vto, vfrom, address, page);

			// Ported to Linux 3.12
			//kunmap_atomic(vto, KM_USER0);
			kunmap_atomic(vto);
			pcn_kmsg_free_msg(writing_page->data);

exit_writing_page:
			remove_mapping_entry(writing_page);
			kfree(writing_page);
exit_write_message:
			kfree(write_message);

			if(ret!=0)
				goto exit;
		}
		else{
			remove_mapping_entry(writing_page);
			kfree(writing_page);
			kfree(write_message);

			if(invalid){
				printk("%s: ERROR: writing an invalid page but not received a copy address 0x%lx (cpu %d id %d)\n",
						__func__, address, tgroup_home_cpu, tgroup_home_id);
				ret= VM_FAULT_REPLICATION_PROTOCOL;
				goto exit;
			}
		}
	}
	page->status = REPLICATION_STATUS_WRITTEN;
	page->owner = 1;
	(page->last_write)++;

#if STATISTICS
	if(page->last_write> most_written_page)
		most_written_page= page->last_write;
#endif

	flush_cache_page(vma, address, pte_pfn(*pte));

	//now the page can be written
	value_pte = *pte;
#if defined(CONFIG_ARM64)
	value_pte = pte_mkwrite(value_pte);
	/* in kernel - page is made dirty as soon as it is made writable (?) */
	value_pte = pte_mkdirty(value_pte);
	value_pte = pte_set_valid_entry_flag(value_pte);
	value_pte = pte_mkyoung(value_pte);
#else
	value_pte = pte_set_flags(value_pte, _PAGE_RW);
	value_pte = pte_set_flags(value_pte, _PAGE_PRESENT);
	//value_pte=pte_set_flags(value_pte,_PAGE_USER);
	value_pte = pte_set_flags(value_pte, _PAGE_ACCESSED);
	//value_pte=pte_set_flags(value_pte,_PAGE_DIRTY);
#endif
	ptep_clear_flush(vma, address, pte);
	set_pte_at_notify(mm, address, pte, value_pte);
	update_mmu_cache(vma, address, pte);

	flush_tlb_page(vma, address);
	flush_tlb_fix_spurious_fault(vma, address);

	PSPRINTK("%s: Out write %i address %lx last write is %lx\n ",
			__func__, 0, address,page->last_write);

exit:
	page->writing = 0;
	return ret;
}

/* Fetch a page from the system => ask other kernels if they have a copy of the page at address "address" on the
 * virtual mapping of the process identified by "tgroup_home_cpu" and "tgroup_home_id".
 *
 * down_read(&mm->mmap_sem) must be held.
 * pte lock must be held.
 *
 *return types:
 *VM_FAULT_OOM, problem allocating memory.
 *VM_FAULT_VMA, error vma management.
 *VM_FAULT_REPLICATION_PROTOCOL, general error.
 *VM_CONTINUE_WITH_CHECK, fetch the page locally.
 *0, remotely fetched;
 *-1, invalidated while fetching;
 * */
static int do_remote_fetch_for_2_kernels(struct task_struct *tsk, int tgroup_home_cpu, int tgroup_home_id,
				  struct mm_struct *mm, struct vm_area_struct *vma, unsigned long address,
				  unsigned long page_fault_flags, pmd_t* pmd, pte_t* pte, pte_t value_pte,
				  spinlock_t* ptl)
{

	mapping_answers_for_2_kernels_t* fetching_page;
	data_request_for_2_kernels_t* fetch_message;
	int ret= 0,i,reachable,other_cpu=-1;

	PSPRINTK("%s: Fetch for address %lx write %i pid %d is local?%d\n", __func__, address,((page_fault_flags & FAULT_FLAG_WRITE)?1:0),tsk->pid,pte_none(value_pte));
#if STATISTICS
	fetch++;
#endif

	fetching_page = (mapping_answers_for_2_kernels_t*) kmalloc(sizeof(mapping_answers_for_2_kernels_t),
								   GFP_ATOMIC);
	if (fetching_page == NULL) {
		ret = VM_FAULT_OOM;
		goto exit;
	}

	memset(fetching_page, 0, sizeof(mapping_answers_for_2_kernels_t));
	fetching_page->tgroup_home_cpu = tgroup_home_cpu;
	fetching_page->tgroup_home_id = tgroup_home_id;
	fetching_page->address = address;
	fetching_page->is_write = (page_fault_flags & FAULT_FLAG_WRITE) ? 1 : 0;
	fetching_page->is_fetch= 1;
	fetching_page->futex_owner = -1;//akshay
	fetching_page->waiting = current; // we are the waiters even if the message can be not for this process WARNING!

	add_mapping_entry(fetching_page);

	if (_cpu==tgroup_home_cpu) {
		if (pte_none(value_pte)) {
			//not marked pte
#if STATISTICS
			local_fetch++;
#endif
			PSPRINTK("Copy not present in the other kernel, local fetch %d of address %lx\n", local_fetch, address);
			ret = VM_CONTINUE_WITH_CHECK;
			goto exit;
		}
	}

	fetch_message = (data_request_for_2_kernels_t*) kmalloc(sizeof(data_request_for_2_kernels_t),
								GFP_ATOMIC);
	if (fetch_message == NULL) {
		ret = VM_FAULT_OOM;
		goto exit_fetching_page;
	}

	memset(fetch_message, 0, sizeof(data_request_for_2_kernels_t));
	fetch_message->header.type = PCN_KMSG_TYPE_PROC_SRV_MAPPING_REQUEST;
	fetch_message->header.prio = PCN_KMSG_PRIO_NORMAL;
	fetch_message->address = address;
	fetch_message->tgroup_home_cpu = tgroup_home_cpu;
	fetch_message->tgroup_home_id = tgroup_home_id;
	fetch_message->is_write = fetching_page->is_write;
	fetch_message->is_fetch = 1;
	fetch_message->vma_operation_index= tsk->mm->vma_operation_index;

	PSPRINTK("%s vma_operation_index %d\n", __func__, fetch_message->vma_operation_index);
	PSPRINTK("%s: Fetch %i address %lx\n", __func__, 0, address);
///////////////////////////////////////////////////////////////////////////////
// Ticket (Marina's data store) and message created

	spin_unlock(ptl);
	up_read(&mm->mmap_sem);
	/*PTE UNLOCKED*/

	fetching_page->arrived_response= 0;
	reachable= 0;

	memory_t* memory= find_memory_entry(tgroup_home_cpu, tgroup_home_id);
	if (!memory) {
		printk(KERN_ERR"%s: ERROR cannot find memory_t mapping (cpu %d id %d)\n",
				__func__, tgroup_home_cpu, tgroup_home_id);
		BUG();
		goto exit_fetch_message;
	}

	down_read(&memory->kernel_set_sem);
	// the list does not include the current processor group descirptor (TODO)
	struct list_head *iter, *tmp_iter;
	_remote_cpu_info_list_t *objPtr;
	list_for_each_safe(iter, tmp_iter, &rlist_head) {
		objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
		i = objPtr->_data._processor;

		if ((ret=pcn_kmsg_send_long(i, (struct pcn_kmsg_long_message*) (fetch_message),sizeof(data_request_for_2_kernels_t)-sizeof(struct pcn_kmsg_hdr)))
		    != -1) {
			// Message delivered
			reachable++;
			other_cpu= i;
			if(reachable>1)
				printk("%s: ERROR: using optimized algorithm for 2 kernels with more than two kernels\n",
						__func__);
		}
	}
	up_read(&memory->kernel_set_sem);

	if (reachable>0) {
		long counter = 0;
		while (fetching_page->arrived_response==0) {
			set_task_state(current, TASK_UNINTERRUPTIBLE); // as above,
			if (fetching_page->arrived_response==0) {
				schedule();
			}
			set_task_state(current, TASK_RUNNING);
			if (!(++counter % RATE_DO_REMOTE_OPERATION))
				printk("%s: WARN: fetching_page->arrived_response 0 [%ld] (cpu %d id %d address 0x%lx)\n",
						__func__, counter, tgroup_home_cpu, tgroup_home_id, address);
		}
	}
///////////////////////////////////////////////////////////////////////////////
// Response received, continuing

	down_read(&mm->mmap_sem);
	spin_lock(ptl);
	/*PTE LOCKED*/

	PSPRINTK("Out wait fetch %i address %lx\n", fetch, address);
	//only the client has to update the vma
	if (tgroup_home_cpu!=_cpu) {
		ret = vma_server_do_mapping_for_distributed_process(fetching_page, tsk, mm, address, ptl); // defined in vma_server.c
		if (ret != 0)
			goto exit_fetch_message;
		PSPRINTK("Mapping end\n");

		vma = find_vma(mm, address);
		if (!vma || address >= vma->vm_end || address < vma->vm_start) {
			vma = NULL;
		} else if (unlikely(is_vm_hugetlb_page(vma))
			   || unlikely(transparent_hugepage_enabled(vma))) {
			printk("%s: ERROR: Installed a vma with HUGEPAGE\n",
					__func__);
			ret = VM_FAULT_VMA;
			goto exit_fetch_message;
		}
		if (vma == NULL) {
			dump_stack();
			printk(KERN_ALERT"%s: ERROR: no vma for address %lx in the system {%d} (cpu %d id %d)\n",
					__func__, address, tsk->pid, tgroup_home_cpu, tgroup_home_id);
			ret = VM_FAULT_VMA;
			goto exit_fetch_message;
		}
	}
///////////////////////////////////////////////////////////////////////////////
// ?

	if (fetching_page->address_present == 1) { // OR > 0
		struct page* page;
		spin_unlock(ptl);
		/*PTE UNLOCKED*/

		if (unlikely(anon_vma_prepare(vma))) {
			printk("%s: ALERT: not implemented -- not anon_vma_prepare pid %d (cpu %d id %d address 0x%lx)\n",
					__func__, tsk->pid, tgroup_home_cpu, tgroup_home_id, address);

			spin_lock(ptl);
			/*PTE LOCKED*/
			if (fetching_page->data)
				pcn_kmsg_free_msg(fetching_page->data);
			ret = VM_FAULT_OOM;
			goto exit_fetch_message;
		}

		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
		if (!page) {
			printk("%s: ALERT: not implemented -- no page pid %d (cpu %d id %d address 0x%lx)\n",
					__func__, tsk->pid, tgroup_home_cpu, tgroup_home_id, address);

			spin_lock(ptl);
			/*PTE LOCKED*/
			if (fetching_page->data)
				pcn_kmsg_free_msg(fetching_page->data);
			ret = VM_FAULT_OOM;
			goto exit_fetch_message;
		}

		__SetPageUptodate(page);
		if (mem_cgroup_newpage_charge(page, mm, GFP_ATOMIC)) {
			printk("%s: ALERT: not implemented -- not mem_cgroup_newpage_charge %d (cpu %d id %d address 0x%lx)\n",
					__func__, tsk->pid, tgroup_home_cpu, tgroup_home_id, address);

			page_cache_release(page);
			spin_lock(ptl);
			/*PTE LOCKED*/
			if (fetching_page->data)
				pcn_kmsg_free_msg(fetching_page->data);
			ret = VM_FAULT_OOM;
			goto exit_fetch_message;
		}

#if STATISTICS
		pages_allocated++;
#endif
		spin_lock(ptl);
		/*PTE LOCKED*/

		int status;
		void *vto;
		void *vfrom;

		//if nobody changed the pte
		if (likely(pte_same(*pte, value_pte))) {
			if(fetching_page->is_write) { //if I am doing a write
				status= REPLICATION_STATUS_WRITTEN;
				if (fetching_page->owner==0) {
					printk("%s: ERROR: copy of a page sent to a write fetch request without ownership (cpu %d id %d)\n",
							__func__, tgroup_home_cpu, tgroup_home_id);
					pcn_kmsg_free_msg(fetching_page->data);
					ret = VM_FAULT_REPLICATION_PROTOCOL;
					goto exit_fetch_message;
				}
			}
			else {
				status= REPLICATION_STATUS_VALID;
				if (fetching_page->owner==1) {
					printk("%s: ERROR: copy of a page sent to a read fetch request with ownership (cpu %d id %d)\n",
							__func__, tgroup_home_cpu, tgroup_home_id);
					pcn_kmsg_free_msg(fetching_page->data);
					ret = VM_FAULT_REPLICATION_PROTOCOL;
					goto exit_fetch_message;
				}
			}
			if (fetching_page->data->address != address) {
				printk("%s: ERROR: trying to copy wrong address 0x%lx\n (cpu %d id %d)\n",
						__func__, address, tgroup_home_cpu, tgroup_home_id);
				pcn_kmsg_free_msg(fetching_page->data);
				ret = VM_FAULT_REPLICATION_PROTOCOL;
				goto exit_fetch_message;
			}

			// Ported to Linux 3.12
			//vto = kmap_atomic(page, KM_USER0);
			vto = kmap_atomic(page);
			vfrom = &(fetching_page->data->data);
			copy_user_page(vto, vfrom, address, page);
			// Ported to Linux 3.12
			//kunmap_atomic(vto, KM_USER0);
			kunmap_atomic(vto);
#if READ_PAGE
			int ct=0;
			if(address == PAGE_ADDR) {
				for(ct=0;ct<8;ct++){
					printk(KERN_ALERT"{%lx} ",(unsigned long) *(((unsigned long *)vfrom)+ct));
				}
			}
#endif

			pcn_kmsg_free_msg(fetching_page->data);
			pte_t entry = mk_pte(page, vma->vm_page_prot);

			//if the page is read only no need to keep replicas coherent
			// but this is not true for the VDSO (that can be read only)
			if (vma->vm_flags & VM_WRITE ||
					(vma->vm_start <= mm->context.popcorn_vdso && mm->context.popcorn_vdso < vma->vm_end) ) {
				page->replicated = 1;
				page->last_write = fetching_page->last_write + ((fetching_page->is_write) ? 1 : 0);
#if STATISTICS
				if(page->last_write> most_written_page)
					most_written_page= page->last_write;
#endif
				page->owner = fetching_page->owner;
				page->status = status;
#if defined(CONFIG_ARM64)
                                if (status == REPLICATION_STATUS_VALID) {
                                        entry =  pte_wrprotect(entry);
                                } else {
                                        entry =  pte_mkwrite(entry);
                                        entry =  pte_mkdirty(entry);
                                }
#else
				if (status == REPLICATION_STATUS_VALID)
					entry = pte_clear_flags(entry, _PAGE_RW);
				else
					entry = pte_set_flags(entry, _PAGE_RW);
#endif
			}
			else { /* !(vma->vm_flags & VM_WRITE) */
				if (fetching_page->is_write)
					printk("%s: ERROR: trying to write a read only page\n", __func__);

				if (fetching_page->owner==1)
					printk("%s: ERROR: received ownership with a copy of a read only page\n", __func__);

				page->replicated = 0;
				page->owner= 0;
				page->status= REPLICATION_STATUS_NOT_REPLICATED;
			}
#if defined(CONFIG_ARM64)
			entry = pte_set_valid_entry_flag(entry);
#else
			entry = pte_set_flags(entry, _PAGE_PRESENT);
#endif
			page->other_owners[_cpu]=1;
			page->other_owners[other_cpu]=1;
			page->futex_owner = fetching_page->futex_owner;//akshay

			flush_cache_page(vma, address, pte_pfn(*pte));
#if defined(CONFIG_ARM64)
			entry = pte_set_user_access_flag(entry);
			entry = pte_mkyoung(entry);
#else
			entry = pte_set_flags(entry, _PAGE_USER);
			entry = pte_set_flags(entry, _PAGE_ACCESSED);
#endif
			ptep_clear_flush(vma, address, pte);

			page_add_new_anon_rmap(page, vma, address);
			set_pte_at_notify(mm, address, pte, entry);

			update_mmu_cache(vma, address, pte);

			flush_tlb_page(vma, address);
			flush_tlb_fix_spurious_fault(vma, address);
		}
		else {
			printk("%s: WARN: pte changed while fetching pid %d (cpu %d id %d address 0x%lx)\n",
					__func__, (int)tsk->pid, tgroup_home_cpu, tgroup_home_id, address);
			status = REPLICATION_STATUS_INVALID;
			mem_cgroup_uncharge_page(page);
			page_cache_release(page);
			pcn_kmsg_free_msg(fetching_page->data);
		}

		PSPRINTK("End fetching address %lx\n", address);
		ret= 0;
		goto exit_fetch_message;
	}
	else if (fetching_page->address_present == 0) { // In both these cases we are not releasing the resources is it correct?
		if (_cpu==tgroup_home_cpu) {
			printk("%s: ERROR: No response for a marked page\n", __func__);
			// here is broken protocol all resources should be released in case
			ret = VM_FAULT_REPLICATION_PROTOCOL;
			goto exit_fetch_message;
		}
		else { /* copy not present on the other kernel, thus we have to allocate it locally, the last part of the page fault handler will take care of this, i.e. allocating and removing resources */
#if STATISTICS
		local_fetch++;
#endif
			PSPRINTK("%s: WARN: Copy not present in the other kernel, local fetch of address 0x%lx\n",
					__func__, address);
			kfree(fetch_message);
			ret = VM_CONTINUE_WITH_CHECK;
			goto exit;
		}
	}
	else { /* this can be a situation of broken protocol. For example if we have multiple answers the protocol is implemented for more than 2 kernels */
		printk("%s: ERROR: I do not know what to do at this point (address_present %d) Release resources?! 0x%lx\n",
				__func__, fetching_page->address_present, address);
	}

exit_fetch_message:
	kfree(fetch_message);
exit_fetching_page:
	remove_mapping_entry(fetching_page);
	kfree(fetching_page);
exit:
	return ret;
}

/**
 * down_read(&mm->mmap_sem) already held getting in
 *
 * return types:
 * VM_FAULT_OOM, problem allocating memory.
 * VM_FAULT_VMA, error vma management.
 * VM_FAULT_ACCESS_ERROR, access error;
 * VM_FAULT_REPLICATION_PROTOCOL, replication protocol error.
 * VM_CONTINUE_WITH_CHECK, fetch the page locally.
 * VM_CONTINUE, normal page_fault;
 * 0, remotely fetched;
 */
int page_server_try_handle_mm_fault(struct task_struct *tsk,
				       struct mm_struct *mm, struct vm_area_struct *vma,
				       unsigned long page_fault_address, unsigned long page_fault_flags,
				       unsigned long error_code)
{
	pgd_t* pgd; pud_t* pud; pmd_t* pmd; pte_t* pte;
	pte_t value_pte; spinlock_t *ptl;

	struct page* page;
	unsigned long address;

	int tgroup_home_cpu = tsk->tgroup_home_cpu;
	int tgroup_home_id = tsk->tgroup_home_id;
	int ret;

	address = page_fault_address & PAGE_MASK;

#if STATISTICS
	page_fault_mio++;
#endif
	PSPRINTK("%s: page fault for address %lx in page %lx task pid %d t_group_cpu %d t_group_id %d %s\n",
                 __func__, page_fault_address, address, tsk->pid,
                 tgroup_home_cpu, tgroup_home_id,
                 page_fault_flags & FAULT_FLAG_WRITE ? "WRITE" : "READ");

	if (address == 0) {
		printk("%s: ERROR: accessing page at address 0 pid %i %d %d\n",
				__func__, tsk->pid, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		dump_processor_regs(task_pt_regs(tsk));
		return VM_FAULT_ACCESS_ERROR | VM_FAULT_VMA;
	}
	if (vma && (address < vma->vm_end && address >= vma->vm_start)
	    && (unlikely(is_vm_hugetlb_page(vma))
		|| transparent_hugepage_enabled(vma))) {
		printk("%s: ERROR: page fault for huge page %d %d\n",
				__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		return VM_CONTINUE;
	}

///////////////////////////////////////////////////////////////////////////////
// find or allocate pte -- this is mm/memory.c: __handle_mm_fault() without pmd handling
	pgd = pgd_offset(mm, address);
	pud = pud_alloc(mm, pgd, address);
	if (!pud)
		return VM_FAULT_OOM;
	pmd = pmd_alloc(mm, pud, address);
	if (!pmd)
		return VM_FAULT_OOM;

// handling of pmd pages was here

	if (pmd_numa(*pmd)) {
		printk("%s: ERROR: page fault for numa page (cpu %d id %d)\n",
				__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		return VM_CONTINUE;
	}
	if (unlikely(pmd_none(*pmd)) &&
		unlikely(__pte_alloc(mm, vma, pmd, address)))
		return VM_FAULT_OOM;
	if (unlikely(pmd_trans_huge(*pmd))) {
		printk("%s: ERROR: page fault for huge page (cpu %d id %d)\n",
				__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
		return VM_CONTINUE;
	}

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
// end of the __handle_mm_fault() code here

	/*if (address == mm->context.popcorn_vdso)
		printk("%s: WARN: VDSO pte 0x%lx value 0x%lx PAGE_UNUSED1 %lx VM_WRITE %u\n",
				__func__, pte, (pte ? (unsigned long)*(unsigned long*)pte : -1lu), (unsigned long)_PAGE_UNUSED1,
				(unsigned int)(vma->vm_flags & VM_WRITE) );
	*/

///////////////////////////////////////////////////////////////////////////////
//  pte null or NONE handling
start:
	//printk("%s start\n", __func__);
	if (pte == NULL || 
#if defined(CONFIG_ARM64)
			pte_none(*pte)) {
#else
			pte_none(pte_clear_flags(*pte, _PAGE_UNUSED1))) {
#endif

		/* Check if other threads of my process are already
		 * fetching the same address on this kernel.
		 */
		if (find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address) != NULL) {
			//wait while the fetch is ended
			spin_unlock(ptl);
			up_read(&mm->mmap_sem);

			while ( find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address) != NULL ) {
				DEFINE_WAIT(wait);
				prepare_to_wait(&read_write_wait, &wait, TASK_UNINTERRUPTIBLE);
				if (find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address)!=NULL) {
					schedule();
				}
				finish_wait(&read_write_wait, &wait);
			}

			down_read(&mm->mmap_sem);
			spin_lock(ptl);

			vma = find_vma(mm, address);
			if (unlikely( !vma || address >= vma->vm_end || address < vma->vm_start)) {
				printk("%s: ERROR: vma not valid after waiting for another thread to fetch (cpu %d id %d)\n",
						__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
				spin_unlock(ptl);
				return VM_FAULT_VMA;
			}
			goto start;
		}

		if (pte) 	
			value_pte = *pte;

		if (!vma || address >= vma->vm_end || address < vma->vm_start)
			vma = NULL;

		ret = do_remote_fetch_for_2_kernels(tsk, tsk->tgroup_home_cpu, tsk->tgroup_home_id,
											mm, vma, address, page_fault_flags, pmd, pte, value_pte, ptl);

		spin_unlock(ptl);
		wake_up(&read_write_wait);

		return ret;
	}
	else { /* case pte MAPPED */

		/* There can be an unluckily case in which I am still fetching... is this handled?
		 */
		mapping_answers_for_2_kernels_t* fetch= find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address);
		if (fetch != NULL && fetch->is_fetch==1) {
			//wait while the fetch is ended
			spin_unlock(ptl);
			up_read(&mm->mmap_sem);

			fetch= find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address);
			while (fetch != NULL && fetch->is_fetch==1) {

				DEFINE_WAIT(wait);
				prepare_to_wait(&read_write_wait, &wait, TASK_UNINTERRUPTIBLE);

				fetch= find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address);
				if (fetch != NULL && fetch->is_fetch==1) {
					schedule();
				}

				finish_wait(&read_write_wait, &wait);

				fetch= find_mapping_entry(tgroup_home_cpu, tgroup_home_id, address);
			}

			down_read(&mm->mmap_sem);
			spin_lock(ptl);

			vma = find_vma(mm, address);
			if (unlikely(!vma || address >= vma->vm_end || address < vma->vm_start)) {
				printk("%s: ERROR: vma not valid after waiting for another thread to fetch (cpu %d id %d)\n",
					__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id);
				spin_unlock(ptl);
				return VM_FAULT_VMA;
			}
			goto start;
		}

		value_pte = *pte;
		/* The pte is mapped so the vma should be valid.
		 * Check if the access is within the limit.
		 */
		if (unlikely(!vma || address >= vma->vm_end || address < vma->vm_start)) {
			printk("%s: ERROR: no vma for address %lx in the system\n",
					__func__, address);
			spin_unlock(ptl);
			return VM_FAULT_VMA;
		}

		/*
		 * Check if the permission are ok.
		 */
		if (unlikely(access_error(error_code, vma))) {
			spin_unlock(ptl);
			printk("%s: WARN: access_error @ 0x%lx (cpu %d tgid %d)\n",
					__func__, page_fault_address, tgroup_home_cpu, tgroup_home_id);
			return VM_FAULT_ACCESS_ERROR;
		}

		page = pte_page(value_pte);
		if (page != vm_normal_page(vma, address, value_pte)) {
			PSPRINTK("page different from vm_normal_page\n");
		}
		PSPRINTK("%s page status %d\n", __func__, page->status);

		/* case page NOT REPLICATED */
		if (page->replicated == 0) {
			PSPRINTK("Page not replicated address %lx page %lx\n", address, (unsigned long)page);

			//check if it is a cow page...
			if ((vma->vm_flags & VM_WRITE) && !pte_write(value_pte)) {
retry_cow:
				PSPRINTK("COW page at %lx\n", address);

				int cow_ret= do_wp_page_for_popcorn(mm, vma,address, pte,pmd,ptl, value_pte);

				if (cow_ret & VM_FAULT_ERROR) {
					if (cow_ret & VM_FAULT_OOM){
						printk("%s: ERROR: VM_FAULT_OOM\n",__func__);
						return VM_FAULT_OOM;
					}
					if (cow_ret & (VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE)){
						printk("%s: ERROR: EHWPOISON\n",__func__);
						return VM_FAULT_OOM;
					}
					if (cow_ret & VM_FAULT_SIGBUS){
						printk("%s: ERROR: EFAULT\n",__func__);
						return VM_FAULT_OOM;
					}
					printk("%s: ERROR: bug from do_wp_page_for_popcorn\n",__func__);
					return VM_FAULT_OOM;
				}

				spin_lock(ptl);
				/*PTE LOCKED*/

				value_pte = *pte;

				if (!pte_write(value_pte)) {
					printk("%s: WARN: page not writable after cow (cpu %d id %d page 0x%lx)\n",
							__func__, tsk->tgroup_home_cpu, tsk->tgroup_home_id, (unsigned long)page);
					goto retry_cow;
				}

				page = pte_page(value_pte);
				page->replicated = 0;
				page->status= REPLICATION_STATUS_NOT_REPLICATED;
				page->owner= 1;
				page->other_owners[_cpu] = 1;
			} /* if cow page */

			spin_unlock(ptl);
			return 0;
		} /* if page->replicated == 0 */

check:
////////////////////////////////////////////////////// antoniob arrived here
		/* case REPLICATION_STATUS_VALID:
		 * the data of the page is up to date.
		 * reads can be performed locally.
		 * to write is needed to send an invalidation message to all the other copies.
		 * a write in REPLICATION_STATUS_VALID changes the status to REPLICATION_STATUS_WRITTEN
		 */
		if (page->status == REPLICATION_STATUS_VALID) {
			PSPRINTK("%s: Page status valid address %lx\n", __func__, address);

			/*read case
			 */
			if (!(page_fault_flags & FAULT_FLAG_WRITE)) {
				spin_unlock(ptl);

				return 0;
			}
			else { /* write case */

				/* If other threads of this process are writing or reading in this kernel, I wait.
				 *
				 * I wait for concurrent writes because after a write the status is updated to REPLICATION_STATUS_WRITTEN,
				 * so only the first write needs to send the invalidation messages.
				 *
				 * I wait for reads because if the invalidation message of the write is handled before the request of the read
				 * there could be the possibility that nobody answers to the read whit a copy.
				 *
				 */
				if (page->writing == 1 || page->reading == 1) {
					spin_unlock(ptl);
					up_read(&mm->mmap_sem);

					while (page->writing == 1 || page->reading == 1) {
						DEFINE_WAIT(wait);

						prepare_to_wait(&read_write_wait, &wait, TASK_UNINTERRUPTIBLE);
						if (page->writing == 1 || page->reading == 1)
							schedule();
						finish_wait(&read_write_wait, &wait);
					}
					down_read(&mm->mmap_sem);
					spin_lock(ptl);
					value_pte = *pte;

					vma = find_vma(mm, address);
					if (unlikely( !vma || address >= vma->vm_end || address < vma->vm_start)) {

						printk("%s: ERROR: vma not valid after waiting for another thread to fetch\n",
								__func__);
						spin_unlock(ptl);
						return VM_FAULT_VMA;
					}

					goto check;
				}

				ret = do_remote_write_for_2_kernels(tsk, tgroup_home_cpu, tgroup_home_id, mm, vma,
								    address, page_fault_flags, pmd, pte, ptl, page,0);

				spin_unlock(ptl);
				wake_up(&read_write_wait);
				return ret;
			}
		}
		else {
			/* case REPLICATION_STATUS_WRITTEN
			 * both read and write can be performed on this page.
			 * */
			if (page->status == REPLICATION_STATUS_WRITTEN) {
				printk("%s: WARN: Page status written address %lx\n", __func__, address);
				spin_unlock(ptl);
				return 0;
			}
			else {

				if (!(page->status == REPLICATION_STATUS_INVALID)) {
					printk("%s: ERROR: Page status not correct on address %lx\n",
					       __func__, address);
					spin_unlock(ptl);
					return VM_FAULT_REPLICATION_PROTOCOL;
				}
				PSPRINTK("%s: Page status invalid address %lx\n", __func__, address);

				/*If other threads are already reading or writing it wait,
				 * they will eventually read a valid copy
				 */
				if (page->writing == 1 || page->reading == 1) {
					spin_unlock(ptl);
					up_read(&mm->mmap_sem);

					while (page->writing == 1 || page->reading == 1) {
						DEFINE_WAIT(wait);
						prepare_to_wait(&read_write_wait, &wait,
								TASK_UNINTERRUPTIBLE);
						if (page->writing == 1 || page->reading == 1)
							schedule();
						finish_wait(&read_write_wait, &wait);
					}

					down_read(&mm->mmap_sem);
					spin_lock(ptl);
					value_pte = *pte;

					vma = find_vma(mm, address);
					if (unlikely(
						    !vma || address >= vma->vm_end
						    || address < vma->vm_start)) {

						printk(
							"%s: ERROR: vma not valid after waiting for another thread to fetch\n", __func__);
						spin_unlock(ptl);
						return VM_FAULT_VMA;
					}

					goto check;
				}
				if (page_fault_flags & FAULT_FLAG_WRITE)
					ret = do_remote_write_for_2_kernels(tsk, tgroup_home_cpu, tgroup_home_id, mm, vma,
									    address, page_fault_flags, pmd, pte, ptl, page,1);
				else
					ret = do_remote_read_for_2_kernels(tsk, tgroup_home_cpu, tgroup_home_id, mm, vma,
									   address, page_fault_flags, pmd, pte, ptl, page);
				spin_unlock(ptl);
				wake_up(&read_write_wait);

				return ret;
			}
		}
	}
}

int page_server_init (void)
{
	int ret;
	
	message_request_wq = create_workqueue("request_wq");
	if (!message_request_wq)
		return -ENOMEM;
	invalid_message_wq= create_workqueue("invalid_wq");
	if (!invalid_message_wq)
		return -ENOMEM;

	if ( ret = pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_MAPPING_REQUEST,
				   handle_mapping_request) )
		return ret;
	if ( ret = pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_MAPPING_RESPONSE,
				   handle_mapping_response) )
		return ret;
	if ( ret = pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_MAPPING_RESPONSE_VOID,
				   handle_mapping_response_void) )
		return ret;
	if ( ret = pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_INVALID_DATA,
				   handle_invalid_request) )
		return ret;
	if ( ret = pcn_kmsg_register_callback(PCN_KMSG_TYPE_PROC_SRV_ACK_DATA,
				   handle_ack) )
		return ret;

	return 0;
}
