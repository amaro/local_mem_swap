/*
 * A sample, extra-simple block driver. Updated for kernel 2.6.31.
 *
 * (C) 2003 Eklektix, Inc.
 * (C) 2010 Pat Patterson <pat at superpat dot com>
 * Redistributable under the terms of the GNU GPL.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/random.h>

#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

MODULE_LICENSE("Dual BSD/GPL");

static int major_num = 0;
module_param(major_num, int, 0);
 
static int npages = 2048 * 1024; 
module_param(npages, int, 0); 


spinlock_t rx_lock;
spinlock_t tx_lock;


#define KERNEL_SECTOR_SIZE 	512
#define SECTORS_PER_PAGE	(PAGE_SIZE / KERNEL_SECTOR_SIZE)
static struct request_queue *Queue;

static struct mem_swap_device {
	unsigned long size;
	spinlock_t lock;
	u8 **data;
	struct gendisk *gd;
} device;

static void mem_swap_transfer(struct mem_swap_device *dev, sector_t sector,
		unsigned long nsect, char *buffer, int write) 
{
	int i;
	int page;
	int npage;

	if (sector % SECTORS_PER_PAGE != 0 || nsect % SECTORS_PER_PAGE != 0) {
		pr_err("incorrect align: %lu %lu %d\n", sector, nsect, write);
		return;
	}

	page = sector / SECTORS_PER_PAGE;
	npage = nsect / SECTORS_PER_PAGE;

	if (page + npage - 1 >= npages) {
		printk (KERN_NOTICE "mem_swap: Beyond-end write (%d %d %d)\n", page, npage, npages);
		return;
	}

	if (write) {
		spin_lock(&tx_lock);
		for (i = 0; i < npage; i++)
			copy_page(dev->data[page + i], buffer + PAGE_SIZE * i);
		spin_unlock(&tx_lock);
	} else {
		spin_lock(&rx_lock);

		for (i = 0; i < npage; i++)
			copy_page(buffer + PAGE_SIZE * i, dev->data[page + i]);
		spin_unlock(&rx_lock);
	}
}



static void mem_swap_request(struct request_queue *q) 
{
	struct request *req;

	req = blk_fetch_request(q);
	if(req){
		while (req != NULL) {
			if (req == NULL || (req->cmd_type != REQ_TYPE_FS)) {
				printk (KERN_NOTICE "Skip non-CMD request\n");
				__blk_end_request_all(req, -EIO);
				continue;
			}
			mem_swap_transfer(&device, blk_rq_pos(req), blk_rq_cur_sectors(req),
					bio_data(req->bio), rq_data_dir(req));
			if ( ! __blk_end_request_cur(req, 0) ) {
				req = blk_fetch_request(q);
			}
		}
	}
}

int mem_swap_getgeo(struct block_device * block_device, struct hd_geometry * geo) {
	long size;

	size = device.size * (PAGE_SIZE / KERNEL_SECTOR_SIZE);
	geo->cylinders = (size & ~0x3f) >> 6;
	geo->heads = 4;
	geo->sectors = 16;
	geo->start = 0;
	return 0;
}

/*
 * The device operations structure.
 */
static struct block_device_operations mem_swap_ops = {
	.owner  = THIS_MODULE,
	.getgeo = mem_swap_getgeo
};



static int __init mem_swap_init(void) {
	int i;
	
	spin_lock_init(&rx_lock);
	spin_lock_init(&tx_lock);

	device.size = npages * PAGE_SIZE;
	spin_lock_init(&device.lock);

	device.data = vmalloc(npages * sizeof(u8 *));
	if (device.data == NULL)
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		device.data[i] = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (device.data[i] == NULL) {
			int j;
			for (j = 0; j < i - 1; j++)
				kfree(device.data[i]);
			vfree(device.data);
			return -ENOMEM;
		}

		memset(device.data[i], 0, PAGE_SIZE);
		if (i % 100000 == 0)
			pr_info("mem_swap: allocated %dth page\n", i);
	}

	Queue = blk_init_queue(mem_swap_request, &device.lock);
	if (Queue == NULL)
		goto out;
	blk_queue_physical_block_size(Queue, PAGE_SIZE);
	blk_queue_logical_block_size(Queue, PAGE_SIZE);
	blk_queue_io_min(Queue, PAGE_SIZE);
	blk_queue_io_opt(Queue, PAGE_SIZE * 4);

	major_num = register_blkdev(major_num, "mem_swap");
	if (major_num < 0) {
		printk(KERN_WARNING "unable to get major number\n");
		goto out;
	}
	device.gd = alloc_disk(16);
	if (!device.gd)
		goto out_unregister;
	device.gd->major = major_num;
	device.gd->first_minor = 0;
	device.gd->fops = &mem_swap_ops;
	device.gd->private_data = &device;
	strcpy(device.gd->disk_name, "mem_swap");
	set_capacity(device.gd, npages * SECTORS_PER_PAGE);
	device.gd->queue = Queue;
	add_disk(device.gd);


	return 0;

out_unregister:
	unregister_blkdev(major_num, "mem_swap");
out:
	for (i = 0; i < npages; i++)
		kfree(device.data[i]);
	vfree(device.data);
	return -ENOMEM;
}

static void __exit mem_swap_exit(void)
{
	int i;

	del_gendisk(device.gd);
	put_disk(device.gd);
	unregister_blkdev(major_num, "mem_swap");
	blk_cleanup_queue(Queue);

	for (i = 0; i < npages; i++)
		kfree(device.data[i]);

	vfree(device.data);

	pr_info("exit\n");
}

module_init(mem_swap_init);
module_exit(mem_swap_exit);
