#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/ptrace.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Provide access to all physical memory through /dev/allmem");

static const struct vm_operations_struct mmap_mem_ops = {
};

/* The following functions are copy paste from arch/x86/mm/ioremap.c */

/*
 * Convert a physical pointer to a virtual kernel pointer for /dev/mem
 * access
 */
void *xlate_dev_mem_ptr(phys_addr_t phys)
{
	unsigned long start  = phys &  PAGE_MASK;
	unsigned long offset = phys & ~PAGE_MASK;
	void *vaddr;

	/* If page is RAM, we can use __va. Otherwise ioremap and unmap. */
	if (page_is_ram(start >> PAGE_SHIFT))
		return __va(phys);

	vaddr = ioremap_cache(start, PAGE_SIZE);
	/* Only add the offset on success and return NULL if the ioremap() failed: */
	if (vaddr)
		vaddr += offset;

	return vaddr;
}

void unxlate_dev_mem_ptr(phys_addr_t phys, void *addr)
{
	if (page_is_ram(phys >> PAGE_SHIFT))
		return;

	iounmap((void __iomem *)((unsigned long)addr & PAGE_MASK));
}

/* The following functions are mostly copy paste from drivers/char/mem.c */

static inline int valid_phys_addr_range(phys_addr_t addr, size_t count)
{
	return addr + count <= __pa(high_memory);
}

static inline unsigned long size_inside_page(unsigned long start,
					     unsigned long size)
{
	unsigned long sz;

	sz = PAGE_SIZE - (start & (PAGE_SIZE - 1));

	return min(sz, size);
}

static ssize_t read_allmem(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	phys_addr_t p = *ppos;
	ssize_t read, sz;
	void *ptr;

	if (p != *ppos)
		return 0;

	if (!valid_phys_addr_range(p, count))
		return -EFAULT;
	read = 0;

	while (count > 0) {
		unsigned long remaining;

		sz = size_inside_page(p, count);

		/*
		 * On ia64 if a page has been mapped somewhere as uncached, then
		 * it must also be accessed uncached by the kernel or data
		 * corruption may occur.
		 */
		ptr = xlate_dev_mem_ptr(p);
		if (!ptr)
			return -EFAULT;

		remaining = copy_to_user(buf, ptr, sz);
		unxlate_dev_mem_ptr(p, ptr);
		if (remaining)
			return -EFAULT;

		buf += sz;
		p += sz;
		count -= sz;
		read += sz;
	}

	*ppos += read;
	return read;
}

/*
 * The memory devices use the full 32/64 bits of the offset, and so we cannot
 * check against negative addresses: they are ok. The return value is weird,
 * though, in that case (0).
 *
 * also note that seeking relative to the "end of file" isn't supported:
 * it has no meaning, so it returns -EINVAL.
 */
static loff_t lseek_allmem(struct file *file, loff_t offset, int orig)
{
	loff_t ret;

	mutex_lock(&file_inode(file)->i_mutex);
	switch (orig) {
	case SEEK_CUR:
		offset += file->f_pos;
	case SEEK_SET:
		/* to avoid userland mistaking f_pos=-9 as -EBADF=-9 */
		if (IS_ERR_VALUE((unsigned long long)offset)) {
			ret = -EOVERFLOW;
			break;
		}
		file->f_pos = offset;
		ret = file->f_pos;
		force_successful_syscall_return();
		break;
	case SEEK_END:
		offset += __pa(high_memory);
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&file_inode(file)->i_mutex);
	return ret;
}

static int mmap_allmem(struct file *file, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;

	vma->vm_page_prot = vma->vm_page_prot;

	vma->vm_ops = &mmap_mem_ops;

	/* Remap-pfn-range will mark the range VM_IO */
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    vma->vm_pgoff,
			    size,
			    vma->vm_page_prot)) {
		return -EAGAIN;
	}
	return 0;
}

static int open_allmem(struct inode *inode, struct file *file)
{
	i_size_write(inode, __pa(high_memory));
	return 0;
}

static const struct file_operations allmem_chardev_ops = {
	.open	   = open_allmem,
	.llseek	   = lseek_allmem,
	.read	   = read_allmem,
	.mmap	   = mmap_allmem,
};

static struct miscdevice allmem_dev = {
	MISC_DYNAMIC_MINOR,
	"allmem",
	&allmem_chardev_ops,
};

static int __init allmem_init(void)
{
	return misc_register(&allmem_dev);
}

static void __exit allmem_exit(void)
{
	misc_deregister(&allmem_dev);
}

module_init(allmem_init);
module_exit(allmem_exit);
