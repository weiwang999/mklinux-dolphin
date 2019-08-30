
/*
 * Cpu namespaces
 *
 * (C) 2014 Antonio Barbalace, antoniob@vt.edu, SSRG VT
 */

#include <linux/cpu_namespace.h>
#include <linux/syscalls.h>
#include <linux/err.h>
#include <linux/acct.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/proc_ns.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/namei.h>

#include <popcorn/process_server.h>

static DEFINE_MUTEX(cpu_caches_mutex);
static struct kmem_cache *cpu_ns_cachep;

//TODO
static struct kmem_cache *create_cpu_cachep(int nr_ids)
{
	mutex_lock(&cpu_caches_mutex);
	//TODO  add code here
	mutex_unlock(&cpu_caches_mutex);
	return NULL;
}

static struct cpu_namespace *create_cpu_namespace(struct cpu_namespace *parent_cpu_ns)
{
	struct cpu_namespace *ns;
	unsigned int level = parent_cpu_ns->level +1; // ?
	int err = -ENOMEM;

	ns = kmem_cache_zalloc(cpu_ns_cachep, GFP_KERNEL);
	if (ns == NULL)
		goto out;

	//something

	ns->cpu_cachep = create_cpu_cachep(level +1);
	//        if (ns->cpu_cachep == NULL)
	//            goto out_free;

	kref_init(&ns->kref);
	ns->level = level;
	ns->parent = get_cpu_ns(parent_cpu_ns);
	ns->cpu_online_mask = ns->parent->cpu_online_mask;
	ns->nr_cpu_ids = ns->parent->nr_cpu_ids;
	ns->nr_cpus = ns->parent->nr_cpus;
	ns->cpumask_size = ns->parent->cpumask_size;	

	//the following is mounting the pid namespace, I think is the pid id in /proc
	/*        err = cpu_ns_prepare_proc(ns);
		  if (err)
		  goto out_put_parent_cpu_ns;
	 */
	return ns;

	put_cpu_ns(parent_cpu_ns);
	kmem_cache_free(cpu_ns_cachep, ns);
out:
	return ERR_PTR(err);
}

static void destroy_cpu_namespace(struct cpu_namespace *ns)
{
	//something
	kmem_cache_free(cpu_ns_cachep, ns);
}

struct cpu_namespace *copy_cpu_ns(unsigned long flags, struct cpu_namespace *old_ns)
{
	if (!(flags & CLONE_NEWCPU)) {
		printk("%s: cacca\n", __func__);
		return get_cpu_ns(old_ns);
	}
	if (flags & (CLONE_THREAD|CLONE_PARENT)) {
		return ERR_PTR(-EINVAL);
		printk("%s: grande cacca\n", __func__);
	}
	return create_cpu_namespace(old_ns);
}

void free_cpu_ns(struct kref *kref)
{
	struct cpu_namespace *ns, *parent;

	ns = container_of(kref, struct cpu_namespace, kref);

	parent = ns->parent;
	destroy_cpu_namespace(ns);

	if (parent != NULL)
		put_cpu_ns(parent);
}

// TODO
void zap_cpu_ns_processes(struct cpu_namespace *cpu_ns)
{

}

static void *cpuns_get(struct task_struct *task)
{
	struct cpu_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	rcu_read_lock();
	//nsproxy = get_pid_ns(task_active_pid_ns(task)); //from kernel/pid_namespace.c
	nsproxy = task_nsproxy(task);
	if (nsproxy)
		ns = get_cpu_ns(nsproxy->cpu_ns);
	rcu_read_unlock();

	return ns;
}

static void cpuns_put(void *ns)
{
	return put_cpu_ns(ns);
}

static int cpuns_install(struct nsproxy *nsproxy, void *ns)
{
	/* Ditch state from the old cpu namespace */
	//exit_sem(current); //from ipc/namespace.c

	// from kernel/cpu_namespace.c
	/*        struct cpu_namespace *active = task_active_pid_ns(current);//is ns_of_pid(task_pid(tsk))
		  struct cpu_namespace *ancestor, *new = ns;

		  if (!capable(CAP_SYS_ADMIN))
		  return -EPERM;

		  if (new->level < active->level)
		  return -EINVAL;

		  ancestor = new;
		  while (ancestor->level > active->level)
		  ancestor = ancestor->parent;
		  if (ancestor != active)
		  return -EINVAL;
	 */
	put_cpu_ns(nsproxy->cpu_ns);
	nsproxy->cpu_ns = get_cpu_ns(ns);
	return 0;
}

const struct proc_ns_operations cpuns_operations = {
	.name           = "cpu",
	.type           = CLONE_NEWCPU,
	.get            = cpuns_get,
	.put            = cpuns_put,
	.install        = cpuns_install,
};

/*****************************************************************************/


// INSTEAD of creating a file we will do attach to popcorn syscall or WRITE on /proc/popcorn
//Il seguente codice va probabilmente spostato in popcorn

struct cpu_namespace * popcorn_ns = 0;

char cpumask_buffer[1024];



//int read_notify_cpu_ns(char *page, char **start, off_t off, int count, int *eof, void *data)
int read_notify_cpu_ns(struct seq_file *file, void *ptr)
{
	struct cpu_namespace *ns = current->nsproxy->cpu_ns;

	memset(cpumask_buffer, 0,1024);

	if (ns->cpu_online_mask) 
		bitmap_scnprintf(cpumask_buffer, 1023, cpumask_bits(ns->cpu_online_mask), ns->_nr_cpumask_bits ); // TODO change with ns->nr_cpu_ids
	else
		printk("cpu_online_mask is  zero!?\n");

	return seq_printf(file, "task: %s(%p)\n"
			"cpu_ns %p level %d parent %p\n"
			"nr_cpus %d nr_cpu_ids %d nr_cpumask_bits %d cpumask_size %d\n"
			"cpumask %s\n"
			"popcorn ns %p\n",
			current->comm, current,
			ns, ns->level, ns->parent,
			ns->nr_cpus, ns->nr_cpu_ids, ns->_nr_cpumask_bits, ns->cpumask_size,
			cpumask_buffer,
			popcorn_ns);
}

// TODO move to popcorn.c/mklinux.c/kinit.c
// the following is temporary and should be updated in a final version, this should be known in the process_server.c --- this shuold be moved in something like kinit.c 
//struct cpu_namespace * popcorn_ns = 0;
extern unsigned int offset_cpus; //from kernel/smp.c
/*
 * This function should be called every time a new kernel will join the popcorn
 * namespace. Note that if there are applications using the popcorn namespace
 * it is not possible to modify the namespace. force will force to update the 
 * namespace data (not currently implemented).
 */
// NOTE this will modify the global variable popcorn_ns (so, no need to pass in anything)
int build_popcorn_ns( int force)
{
	int cnr_cpu_ids =0;

	struct list_head *iter;
	_remote_cpu_info_list_t *objPtr;
	struct cpumask *pcpum =0;
	unsigned long * summary, * tmp;
	int size, offset;
	int cpuid;

	//    if (kref says that no one is using it continue) // TODO
	//        return -EBUSY;

	// TODO lock the list of kernels

	//-------------------------------- SIZE --------------------------------
	/* calculate the minimum size of the bitmask */
	// current kernel
	cnr_cpu_ids += cpumask_weight(cpu_online_mask); // or nr_cpu_ids
	// other kernels  
	cpuid = -1;
	list_for_each(iter, &rlist_head) {
		objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
		pcpum = &(objPtr->_data.cpumask); //&(objPtr->_data._cpumask);
		cnr_cpu_ids += bitmap_weight(cpumask_bits(pcpum),
				(objPtr->_data.cpumask_size * BITS_PER_BYTE));//cpumask_weight(pcpum);
	}    

	//-------------------------------- GENERATE THE MASK --------------------------------
	size = max(cpumask_size(), (BITS_TO_LONGS(cnr_cpu_ids) * sizeof(long)));
	summary = kmalloc(size, GFP_KERNEL);
	tmp = kmalloc(size, GFP_KERNEL);
	if (!summary || !tmp) {
		printk(KERN_ERR"%s: kmalloc returned zero allocating summary (%p) or tmp (%p)\n",
				__func__, summary, tmp);
		return -ENOMEM;
	}
	//printk(KERN_ERR"%s: cnr_cpu_ids: %d size:%d summary %p tmp %p\n",
	//	__func__, cnr_cpu_ids, size, summary, tmp);
	// current kernel
	bitmap_zero(summary, size * BITS_PER_BYTE);
	bitmap_copy(summary, cpumask_bits(cpu_online_mask), nr_cpu_ids); // NOTE that the current cpumask is not included in the remote list
	bitmap_shift_left(summary, summary, offset_cpus, cnr_cpu_ids);
	// other kernels  
	cpuid =-1;
	list_for_each(iter, &rlist_head) {
		objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
		cpuid = objPtr->_data._processor;
		pcpum = &(objPtr->_data.cpumask);//&(objPtr->_data._cpumask);
		offset = objPtr->_data.cpumask_offset;

		//	printk("%s, cpumask_offset %d\n",__func__,offset);
		//TODO we should update kinit.c in order to support variable length cpumask

		bitmap_zero(tmp, size * BITS_PER_BYTE);
		bitmap_copy(tmp, cpumask_bits(pcpum), (objPtr->_data.cpumask_size *BITS_PER_BYTE));
		bitmap_shift_left(tmp, tmp, offset, cnr_cpu_ids);
		bitmap_or(summary, summary, tmp, cnr_cpu_ids);
	}

	//-------------------------------- GENERATE the namespace --------------------------------
	if (!popcorn_ns) {
		struct cpu_namespace * tmp_ns = create_cpu_namespace(&init_cpu_ns);
		if (IS_ERR(tmp_ns)) {
			return -ENODEV;
			//TODO release list lock
		}
		tmp_ns->cpu_online_mask = 0;

		//TODO lock the namespace
		popcorn_ns = tmp_ns;
	}
	if (popcorn_ns->cpu_online_mask) {
		if (popcorn_ns->cpu_online_mask != cpumask_bits(cpu_online_mask))
			kfree(popcorn_ns->cpu_online_mask);
		else
			printk(KERN_ERR"%s: there is something weird, popcorn was associated with cpu_online_mask\n", __func__);
	}
	popcorn_ns->cpu_online_mask= (struct cpumask *)summary;

	popcorn_ns->nr_cpus = cnr_cpu_ids; // the followings are intentional
	popcorn_ns->nr_cpu_ids = cnr_cpu_ids;
	popcorn_ns->_nr_cpumask_bits= cnr_cpu_ids;
	popcorn_ns->cpumask_size = BITS_TO_LONGS (cnr_cpu_ids) * sizeof(long);

	// TODO maybe the following should be different
	// the idea is that if does not have parent, parent should be init_cpu_ns)
	if (!popcorn_ns->parent) {
		popcorn_ns->parent = &init_cpu_ns;
		popcorn_ns->level = 1;   
	}

	//TODO unlock popcorn namespace
	//TODO unlock kernel list
	create_thread_pull();
	return 0;
}

/*
 * this function allow to 
 */
int associate_to_popcorn_ns(struct task_struct * tsk)
{

	//printk("%s entered pid %d \n",__func__,tsk->pid);
	if (tsk->nsproxy->cpu_ns != popcorn_ns) {
		printk("%s assumes the namespace is popcorn but is not\n", __func__);
		return -ENODEV;
	}

	if (tsk->cpus_allowed_map && (tsk->cpus_allowed_map->ns != tsk->nsproxy->cpu_ns)) {
		printk(KERN_ERR"%s: WARN tsk->cpus_allowed_map->ns (%p) != tsk->nsproxy->cpu_ns (%p)\n",
				__func__, tsk->cpus_allowed_map->ns, tsk->nsproxy->cpu_ns);

		// TODO recover from this situation
	}

	if (tsk->cpus_allowed_map == NULL)
	{

		//printk("%s, in task->cpus_allowed_map null\n",__func__);
		// in this case I have to convert allowed to global mask
		int size = CPUBITMAP_SIZE(popcorn_ns->nr_cpu_ids);
		struct cpubitmap * cbitm = kmalloc(size, GFP_ATOMIC);// here we should use  a cache instead of mkalloc
		if (!cbitm) {
			printk(KERN_ERR"%s: kmalloc allocation failed\n", __func__);
			return -ENOMEM;
		}
		cbitm->size = size-sizeof(struct cpubitmap);
		//printk("%s, cbitm->size %lu \n",__func__,cbitm->size);
		cbitm->ns = popcorn_ns; // add reference to it?! --> actually the task already did it!!! so not necessary

		// TODO we are always assuming that the previous namespace was init_cpu_ns but maybe is not correct
		//bitmap_fill(cbitm->bitmap, popcorn_ns->nr_cpu_ids);
		//bitmap_complement (cbitm->bitmap,  cpumask_bits(cpu_online_mask),nr_cpu_ids);
		//bitmap_or(cbitm->bitmap,cbitm->bitmap, cpumask_bits(&current->cpus_allowed), nr_cpu_ids);
		//printk("%s, cbitm->bitmap %lu, cbitm->bitmap %lu, offset_cpus %d, popcorn_ns->nr_cpu_ids %lu\n",__func__,cbitm->bitmap, cbitm->bitmap, offset_cpus, popcorn_ns->nr_cpu_ids);
		//bitmap_shift_left(cbitm->bitmap, cbitm->bitmap, offset_cpus, popcorn_ns->nr_cpu_ids);
		//if(!(offset_cpus==0))
		//bitmap_fill(cbitm->bitmap, offset_cpus);      
		//bitmap_or (cbitm->bitmap, cbitm->bitmap, tsk->nsproxy->cpu_ns->cpu_online_mask, popcorn_ns->nr_cpu_ids);
		//bitmap_complement (cbitm->bitmap, cbitm->bitmap, popcorn_ns->nr_cpu_ids);      
		//bitmap_xor (cbitm->bitmap, cbitm->bitmap, tsk->nsproxy->cpu_ns->cpu_online_mask, popcorn_ns->nr_cpu_ids);
		//bitmap_and (cbitm->bitmap, cbitm->bitmap, tsk->nsproxy->cpu_ns->cpu_online_mask, popcorn_ns->nr_cpu_ids);  
		//bitmap_complement (cbitm->bitmap, cbitm->bitmap, popcorn_ns->nr_cpu_ids);
		bitmap_copy (cbitm->bitmap, tsk->nsproxy->cpu_ns->cpu_online_mask, popcorn_ns->nr_cpu_ids);

		tsk->cpus_allowed_map = cbitm;
		//printk("%s, cbitm->size %lu \n",__func__,cbitm->size);
	}
	// NOTE the else case do not need to be handled, i.e. we are already linked and updated to popcorn

	return 0;
}


/* despite this is not the correct way to go, this is working in this way
 * every time we are writing something on this file (even NULL)
 * we are rebuilding a new popcorn namespace merging all the available kernels
 */
static ssize_t write_notify_cpu_ns(struct file *file, const char __user *buffer, unsigned long count, loff_t *data)
{

	int ret;

	printk("%s, entered in write proc popcorn\n",__func__);
	get_task_struct(current);

#undef USE_OLD_IMPLEM
#ifdef USE_OLD_IMPLEM
	int cnr_cpu_ids, cnr_cpus;
	struct cpu_namespace *ns;

	ns = current->nsproxy->cpu_ns;

	// TODO convert everything in bitmap (cpumask are fixed size bitmaps
	struct list_head *iter;
	_remote_cpu_info_list_t *objPtr;
	struct cpumask *pcpum =0;
	struct cpumask tmp; 
	struct cpumask * summary= kmalloc(cpumask_size(), GFP_KERNEL);
	if (!summary) {
		printk("kmalloc returned 0?!");
	}
	cnr_cpu_ids += cpumask_weight(cpu_online_mask); // or nr_cpu_ids
	cpumask_copy(summary, cpu_online_mask); // NOTE that the current cpumask is not included in the remote list

	int cpuid =-1;
	list_for_each(iter, &rlist_head) {
		objPtr = list_entry(iter, _remote_cpu_info_list_t, cpu_list_member);
		cpuid = objPtr->_data._processor;
		pcpum = &(objPtr->_data._cpumask);

		// TODO use offsetting with cpuid
		cpumask_copy(&tmp, summary);
		cpumask_or (summary, &tmp, pcpum);

		cnr_cpu_ids += cpumask_weight(pcpum);
	}
	printk("%s, after list for each of cpus\n",__func__);
	//associate the new cpu mask with the namespace
	ns->cpu_online_mask = summary;
	ns->nr_cpus = NR_CPUS;
	ns->nr_cpu_ids = cnr_cpu_ids;
#else
	// the new code builds a popcorn namespace and we should remove the ref count and destroy the current (if is not init) and attach to popcorn..

	/* if the namespace does not exist, create it */
	if (!popcorn_ns) {
		printk(KERN_ERR"%s: popcorn is null now!\n", __func__);
		if ((ret = build_popcorn_ns(0))) { 
			printk(KERN_ERR"%s: build_popcorn returned: %d\n", __func__, ret);
			return count;
		}
	}

	/* if we are already attached, let's skip the unlinking and linking */
	if (current->nsproxy->cpu_ns != popcorn_ns) { 
		put_cpu_ns(current->nsproxy->cpu_ns);
		current->nsproxy->cpu_ns = get_cpu_ns(popcorn_ns);
		printk(KERN_ERR"%s: cpu_ns %p\n", __func__, current->nsproxy->cpu_ns);
	} else
		printk(KERN_ERR"%s: already attached to popcorn(%p) current = %p\n",
				__func__, popcorn_ns, current->nsproxy->cpu_ns);

#endif

	// -------------- UPDATE cpus_allowed map -----------------
#ifdef USE_OLD_IMPLEM
	if (current->cpus_allowed_map == NULL) {// in this case I have to convert allowed to global mask
		int size = CPUBITMAP_SIZE(ns->nr_cpu_ids);
		struct cpubitmap * cbitm = kmalloc(size, GFP_KERNEL);// here we should use  a cache instead of mkalloc
		if (!cbitm)
			printk(KERN_ERR"%s: kmalloc allocation failed\n", __func__);
		cbitm->size = size;
		cbitm->ns = ns;
		bitmap_zero(cbitm->bitmap, ns->nr_cpu_ids);
		bitmap_copy(cbitm->bitmap, cpumask_bits(&current->cpus_allowed), cpumask_size());
		bitmap_shift_left(cbitm->bitmap, cbitm->bitmap, offset_cpus, ns->nr_cpu_ids);
		current->cpus_allowed_map = cbitm;
	}
	// TODO support the else case
#else
	if ((ret = associate_to_popcorn_ns(current)))  {
		printk(KERN_ERR"associate_to_popcorn_ns returned: %d\n", ret);
		return count;
	}
#endif

	printk("task %p %s associated with popcorn (local nr_cpu_ids %d NR_CPUS %d cpumask_bits %d OFFSET %d\n", current, current->comm, nr_cpu_ids, NR_CPUS, nr_cpumask_bits, offset_cpus);

	put_task_struct(current);

	printk("%s, after put_task_struct\n",__func__);
	return count;
}

int popcorn_ns_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, read_notify_cpu_ns, NULL);
}

static struct file_operations fops = {
	.open = popcorn_ns_proc_open,
	.read = seq_read,
	.write = write_notify_cpu_ns,
	.release = single_release,
};

int register_popcorn_ns(void)
{
	// if kernels > 1 then create /proc/popcorn
	struct proc_dir_entry *res;

	printk(KERN_INFO"Popcornlinux NS: Creating popcorn namespace entry in proc\n");

	/* Linux 3.2.14 */
	/*res = create_proc_entry("popcorn", S_IRUGO, NULL);
	if (!res) {
		printk(KERN_ALERT"%s: create_proc_entry failed (%p)\n", __func__, res);
		return -ENOMEM;
	}*/

	/* Linux 3.12 onwards */
	res = proc_create("popcorn", S_IRUGO, NULL, &fops);
	if(!res) {
		printk(KERN_ERR"Popcornlinux NS: (ERROR) Failed to create proc entry!");
		return -1;
	}
	return 0;
}

/*
 * TODO: Need to find an alternate implementation since macros like PROC_I
 * are no longer publically available in 3.12, the definitions are
 * moved to private header file inside proc/fs/internal.h
 */
#if 0

/// initial idea for attaching a process to popcorn namespace
/* This function should be called everytime a new kernel will be registered
 * in /kernel/kinit.c . When the number of kernels in the list is greater than
 * 1 the /proc/popcorn entry will be created.
 */
int notify_cpu_ns(void)
{
	// lock the cpu namespace

	// count the number of elements and calculate the cpumask/bitmap that is required to contain them
	// min size is cpumask

	// allocate cpumask and populate it

	// finish namespace intialization

	//unlock cpu namespace  
	/*
	// if kernels > 1 then create /proc/popcorn
	struct proc_dir_entry *res; // TODO mettiamola globale
	res = create_proc_entry("popcorn_ext", S_IRUGO, NULL);
	if (!res) {
	printk(KERN_ALERT"%s: create_proc_entry failed (%p)\n", __func__, res);
	return -ENOMEM;
	}
	res->read_proc = read_notify_cpu_ns;
	//	res->write_proc = write_notify_cpu_ns;
	res->proc_fops  = &ns_file_operations; // required by setns

	// alternative, when we open the file andiamo a settare i PROC_I elements
	 */
	// the following is probably not correct

	struct dentry *dentry = NULL;
	struct inode *dir = NULL;
	struct proc_inode *ei = NULL;
	struct path old_path; int err = 0;

	err = kern_path("/proc/", LOOKUP_FOLLOW, &old_path);
	if (err)
		return err;

	dentry = old_path.dentry;
	dir = dentry->d_inode * inode;

	// proc_pid_make_inode
	inode = new_inode(dir->i_sb);
	if (!inode)
		return err;

	//ei = PROC_I(inode);
	inode->i_ino = get_next_ino();
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_op = dir->i_op; //&proc_def_inode_operations;


	/*	ns = ns_ops->get(task);
		if (!ns)
		goto out_iput;*/

	//ei = PROC_I(inode);
	inode->i_mode = S_IFREG|S_IRUSR;

	extern const struct file_operations ns_file_operations;	
	inode->i_fop  = &ns_file_operations;
	ei->ns_ops    = &cpuns_operations;
	ei->ns	      = &popcorn_ns;

	//	d_set_d_op(dentry, &pid_dentry_operations); //this are already set in /proc
	d_add(dentry, inode);

	// TODO add the real dentry entry! with the name of the file!

	return 0;
}

#endif

static __init int cpu_namespaces_init(void)
{

	printk("Initializing cpu_namespace\n");
	cpu_ns_cachep = KMEM_CACHE(cpu_namespace, SLAB_PANIC);
	if (!cpu_ns_cachep)
		printk("%s: ERROR KMEM_CACHE\n", __func__);

	return register_popcorn_ns();
}

__initcall(cpu_namespaces_init);

/* the idea is indeed to do similarly to net, i.e. there will be a file
   somewhere and can be in /proc/popcorn or /var/run/cpuns/possible_configurations
   in which you can do setns and being part of the namespace */
