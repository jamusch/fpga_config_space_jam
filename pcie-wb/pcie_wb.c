#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/poll.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/aer.h>
#include <linux/sched.h> 
#include <linux/version.h>
#include <linux/miscdevice.h>

#include <linux/mm.h>
#include <linux/sysfs.h>


#include <asm/io.h>
#include <asm/spinlock.h>
#include <asm/byteorder.h>

#include "pcie_wb.h"
#include "wishbone.h"

#if defined(__BIG_ENDIAN)
#define endian_addr(width, shift) (sizeof(wb_data_t)-width)-shift
#elif defined(__LITTLE_ENDIAN)
#define endian_addr(width, shift) shift
#else
#error "unknown machine byte order (endian)"
#endif

static unsigned int debug = 0;


#define pcie_wb_dbg( args... )                    \
  if(debug) printk( args );

#define pcie_wb_msg( args... )                    \
  printk( args );

/** hold full device number JAM2018 */



static dev_t pcie_wb_devt;
static atomic_t pcie_wb_numdevs = ATOMIC_INIT(0);
static int my_major_nr = 0;


//-----------------------------------------------------------------------------
struct file_operations pcie_wb_fops = {
    .owner = THIS_MODULE,
    .mmap = pcie_wb_mmap,
    .open = pcie_wb_open,
    .release = pcie_wb_release, };
//-----------------------------------------------------------------------------


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
static struct class* pcie_wb_class;

static DEVICE_ATTR(codeversion, S_IRUGO, pcie_wb_sysfs_codeversion_show, NULL);
static DEVICE_ATTR(dactl, S_IWUGO | S_IRUGO, pcie_wb_sysfs_dactl_show, pcie_wb_sysfs_dactl_store);


ssize_t pcie_wb_sysfs_codeversion_show (struct device *dev, struct device_attribute *attr, char *buf)
{
  ssize_t curs = 0;
  curs += snprintf (buf + curs, PAGE_SIZE, "*** This is %s, version %s build on %s at %s \n",
     PCIE_WB_DESC, PCIE_WB_VERSION, __DATE__, __TIME__);
  curs += snprintf (buf + curs, PAGE_SIZE, "\tmodule authors: %s \n", PCIE_WB_AUTHORS);
  curs += snprintf (buf + curs, PAGE_SIZE, "\tPrepared for direct TLU access for MBS DAQ \n");
  return curs;
}


ssize_t  pcie_wb_sysfs_dactl_show (struct device *dev, struct device_attribute *attr, char *buf)
{
  ssize_t curs = 0;
  u32* reg=0;
  unsigned int val=0;
  struct pcie_wb_dev *privdata;
  privdata = (struct pcie_wb_dev *) dev_get_drvdata (dev);

  reg = (u32*)(privdata->pci_res[0].addr + DIRECT_ACCESS_CONTROL);
  val=ioread32 (reg);
  pcie_wb_msg( KERN_NOTICE "PCIE_WB: read from dactl register the value 0x%x\n", val);
  curs += snprintf (buf + curs, PAGE_SIZE - curs, "%d\n", val);
  return curs;
}

ssize_t  pcie_wb_sysfs_dactl_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
  unsigned int val=0;
  u32* reg=0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
  int rev=0;
#else
  char* endp=0;
#endif
  struct pcie_wb_dev *privdata;
  privdata = (struct pcie_wb_dev*) dev_get_drvdata (dev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
  rev=kstrtouint(buf,0,&val); // this can handle both decimal, hex and octal formats if specified by prefix JAM
  if(rev!=0) return rev;
#else
  val=simple_strtoul(buf,&endp, 0);
  count= endp - buf; // do we need this?
#endif
   reg = (u32*)(privdata->pci_res[0].addr + DIRECT_ACCESS_CONTROL);
   iowrite32(val,reg);
   pcie_wb_msg( KERN_NOTICE "PCIE_WB: wrote to dactl register the value 0x%x\n", val);
  return count;
}




#endif



#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,28)

/* Missing in 2.6.28. Present in 2.6.29. */
static void compat_pci_clear_master(struct pci_dev *dev)
{
	u16 old_cmd, cmd;
	
	pci_read_config_word(dev, PCI_COMMAND, &old_cmd);
	cmd = old_cmd & ~PCI_COMMAND_MASTER;
	pci_write_config_word(dev, PCI_COMMAND, cmd);
	dev->is_busmaster = false;
}

/* Override with backwards compatible version */
#define pci_clear_master compat_pci_clear_master
#endif

static void pcie_int_enable(struct pcie_wb_dev *dev, int on)
{
	int enable = on && !dev->msi;
	iowrite32((enable?0x20000000UL:0) + 0x10000000UL, dev->pci_res[0].addr + CONTROL_REGISTER_HIGH);
}

static void wb_cycle(struct wishbone* wb, int on)
{
	struct pcie_wb_dev* dev;
	unsigned char* control;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	control = dev->pci_res[0].addr;
	
	if (unlikely(debug))
		printk(KERN_ALERT PCIE_WB ": cycle(%d)\n", on);
	
	iowrite32((on?0x80000000UL:0) + 0x40000000UL, control + CONTROL_REGISTER_HIGH);
}

static void wb_byteenable(struct wishbone* wb, unsigned char be)
{
	struct pcie_wb_dev* dev;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	
	switch (be) {
	case 0x1:
		dev->width = 1;
		dev->shift = 0;
		dev->low_addr = endian_addr(1, 0);
		break;
	case 0x2:
		dev->width = 1;
		dev->shift = 8;
		dev->low_addr = endian_addr(1, 1);
		break;
	case 0x4:
		dev->width = 1;
		dev->shift = 16;
		dev->low_addr = endian_addr(1, 2);
		break;
	case 0x8:
		dev->width = 1;
		dev->shift = 24;
		dev->low_addr = endian_addr(1, 3);
		break;
	case 0x3:
		dev->width = 2;
		dev->shift = 0;
		dev->low_addr = endian_addr(2, 0);
		break;
	case 0xC:
		dev->width = 2;
		dev->shift = 16;
		dev->low_addr = endian_addr(2, 2);
		break;
	case 0xF:
		dev->width = 4;
		dev->shift = 0;
		dev->low_addr = endian_addr(4, 0);
		break;
	default:
		/* noop -- ignore the strange bitmask */
		break;
	}
}

static void wb_write(struct wishbone* wb, wb_addr_t addr, wb_data_t data)
{
	struct pcie_wb_dev* dev;
	unsigned char* control;
	unsigned char* window;
	wb_addr_t window_offset;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	control = dev->pci_res[0].addr;
	window = dev->pci_res[1].addr;
	
	window_offset = addr & WINDOW_HIGH;
	if (unlikely(window_offset != dev->window_offset)) {
		iowrite32(window_offset, control + WINDOW_OFFSET_LOW);
		dev->window_offset = window_offset;
	}
	
	switch (dev->width) {
	case 4:	
		if (unlikely(debug)) printk(KERN_ALERT PCIE_WB ": iowrite32(0x%x, 0x%x)\n", data, addr & ~3);
		iowrite32(data, window + (addr & WINDOW_LOW)); 
		break;
	case 2: 
		if (unlikely(debug)) printk(KERN_ALERT PCIE_WB ": iowrite16(0x%x, 0x%x)\n", data >> dev->shift, (addr & ~3) + dev->low_addr);
		iowrite16(data >> dev->shift, window + (addr & WINDOW_LOW) + dev->low_addr); 
		break;
	case 1: 
		if (unlikely(debug)) printk(KERN_ALERT PCIE_WB ": iowrite8(0x%x, 0x%x)\n", data >> dev->shift, (addr & ~3) + dev->low_addr);
		iowrite8 (data >> dev->shift, window + (addr & WINDOW_LOW) + dev->low_addr); 
		break;
	}
}

static wb_data_t wb_read(struct wishbone* wb, wb_addr_t addr)
{
	wb_data_t out;
	struct pcie_wb_dev* dev;
	unsigned char* control;
	unsigned char* window;
	wb_addr_t window_offset;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	control = dev->pci_res[0].addr;
	window = dev->pci_res[1].addr;
	
	window_offset = addr & WINDOW_HIGH;
	if (unlikely(window_offset != dev->window_offset)) {
		iowrite32(window_offset, control + WINDOW_OFFSET_LOW);
		dev->window_offset = window_offset;
	}
	
	switch (dev->width) {
	case 4:	
		if (unlikely(debug)) printk(KERN_ALERT PCIE_WB ": ioread32(0x%x)\n", addr & ~3);
		out = ((wb_data_t)ioread32(window + (addr & WINDOW_LOW)));
		break;
	case 2: 
		if (unlikely(debug)) printk(KERN_ALERT PCIE_WB ": ioread16(0x%x)\n", (addr & ~3) + dev->low_addr);
		out = ((wb_data_t)ioread16(window + (addr & WINDOW_LOW) + dev->low_addr)) << dev->shift;
		break;
	case 1: 
		if (unlikely(debug)) printk(KERN_ALERT PCIE_WB ": ioread8(0x%x)\n", (addr & ~3) + dev->low_addr);
		out = ((wb_data_t)ioread8 (window + (addr & WINDOW_LOW) + dev->low_addr)) << dev->shift;
		break;
	default: /* technically should be unreachable */
		out = 0;
		break;
	}

	mb(); /* ensure serial ordering of non-posted operations for wishbone */
	
	return out;
}

static wb_data_t wb_read_cfg(struct wishbone *wb, wb_addr_t addr)
{
	wb_data_t out;
	struct pcie_wb_dev* dev;
	unsigned char* control;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	control = dev->pci_res[0].addr;
	
	switch (addr) {
	case 0:  out = ioread32(control + ERROR_FLAG_HIGH);   break;
	case 4:  out = ioread32(control + ERROR_FLAG_LOW);    break;
	case 8:  out = ioread32(control + SDWB_ADDRESS_HIGH); break;
	case 12: out = ioread32(control + SDWB_ADDRESS_LOW);  break;
	default: out = 0; break;
	}
	
	mb(); /* ensure serial ordering of non-posted operations for wishbone */
	
	return out;
}

static int wb_request(struct wishbone *wb, struct wishbone_request *req)
{
	struct pcie_wb_dev* dev;
	unsigned char* control;
	uint32_t ctl;
	int out;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	control = dev->pci_res[0].addr;
	
	ctl        = ioread32(control + MASTER_CTL_HIGH);
	req->addr  = ioread32(control + MASTER_ADR_LOW);
	req->data  = ioread32(control + MASTER_DAT_LOW);
	req->mask  = ctl & 0xf;
	req->write = (ctl & 0x40000000) != 0;
	
	out = (ctl & 0x80000000) != 0;
	
	if (out) iowrite32(1, control + MASTER_CTL_HIGH); /* dequeue operation */
	
	pcie_int_enable(dev, 1);
	
	return out;
}

static void wb_reply(struct wishbone *wb, int err, wb_data_t data)
{
	struct pcie_wb_dev* dev;
	unsigned char* control;
	
	dev = container_of(wb, struct pcie_wb_dev, wb);
	control = dev->pci_res[0].addr;
	
	iowrite32(data, control + MASTER_DAT_LOW);
	iowrite32(err+2, control + MASTER_CTL_HIGH);
}

static const struct wishbone_operations wb_ops = {
	.owner      = THIS_MODULE,
	.cycle      = wb_cycle,
	.byteenable = wb_byteenable,
	.write      = wb_write,
	.read       = wb_read,
	.read_cfg   = wb_read_cfg,
	.request    = wb_request,
	.reply      = wb_reply,
};

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	struct pcie_wb_dev *dev = dev_id;
	
	pcie_int_enable(dev, 0);
	wishbone_slave_ready(&dev->wb);
	
	return IRQ_HANDLED;
}

static int setup_bar(struct pci_dev* pdev, struct pcie_wb_resource* res, int bar)
{
	res->start = pci_resource_start(pdev, bar);
	res->end = pci_resource_end(pdev, bar);
	res->size = res->end - res->start + 1;
	
	if (debug)
		printk(KERN_ALERT PCIE_WB "/BAR%d  0x%lx - 0x%lx\n", bar, res->start, res->end);

	if ((pci_resource_flags(pdev, 0) & IORESOURCE_MEM) == 0) {
		printk(KERN_ALERT PCIE_WB "/BAR%d is not a memory resource\n", bar);
		return -ENOMEM;
	}

	if (!request_mem_region(res->start, res->size, PCIE_WB)) {
		printk(KERN_ALERT PCIE_WB "/BAR%d: request_mem_region failed\n", bar);
		return -ENOMEM;
	}
	
	res->addr = ioremap_nocache(res->start, res->size);
	if (debug)
		printk(KERN_ALERT PCIE_WB "/BAR%d: ioremap to %lx\n", bar, (unsigned long)res->addr);
	
	return 0;
}

static void destroy_bar(struct pcie_wb_resource* res)
{
	if (debug)
		printk(KERN_ALERT "released io 0x%lx\n", res->start);
		
	iounmap(res->addr);
	release_mem_region(res->start, res->size);
}


/** JAM2018 - new file operation to map bar adress to user space for direct access TLU*/


int pcie_wb_open (struct inode *inode, struct file *filp)
{
  struct pcie_wb_dev *dev;      // device information
  pcie_wb_dbg(KERN_INFO "\nBEGIN pcie_wb_open \n");
  dev = container_of(inode->i_cdev, struct pcie_wb_dev, cdev);
  filp->private_data = dev;    // for other methods
  pcie_wb_dbg(KERN_INFO "END   pcie_wb_open \n");
  return 0;                   // success
}
//-----------------------------------------------------------------------------
int pcie_wb_release (struct inode *inode, struct file *filp)
{
  pcie_wb_dbg(KERN_INFO "BEGIN pex_release \n");
  pcie_wb_dbg(KERN_INFO "END   pex_release \n");
  return 0;
}

int pcie_wb_mmap (struct file *filp, struct vm_area_struct *vma)
{
  /** JAM2018 : this function is a clone from the mmap of mbspex driver, adjusted to the case that
   * we want bar1 exported to user space for direct TLU access. The functionality to map any physical memory
   * is still provided, but not a use case for timing receiver*/


  struct pcie_wb_dev *privdata;
  int ret = 0;
  unsigned long bufsize, barsize;
  privdata = (struct pcie_wb_dev*) filp->private_data;
  pcie_wb_dbg(KERN_NOTICE "** starting pcie_wb_mmap for vm_start=0x%lx\n", vma->vm_start);
  if (!privdata)
    return -EFAULT;
  bufsize = (vma->vm_end - vma->vm_start);
  pcie_wb_dbg(KERN_NOTICE "** starting pcie_wb_mmap for size=%ld \n", bufsize);

  if (vma->vm_pgoff == 0)
  {
    /* user does not specify external physical address, we deliver mapping of bar 1 that will contain TLU register:*/
    pcie_wb_dbg(
        KERN_NOTICE "PCIe_Wishbone hardware is Mapping bar1 base address %lx / PFN %lx\n",  privdata->pci_res[1].start, privdata->pci_res[1].start >> PAGE_SHIFT);


    barsize = privdata->pci_res[1].size;
    if (bufsize > barsize)
    {
      pcie_wb_dbg(
          KERN_WARNING "Requested length %ld exceeds bar0 size, shrinking to %ld bytes\n", bufsize, barsize);
      bufsize = barsize;
    }

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,7,0)
    vma->vm_flags |= (VM_RESERVED);
#else
    vma->vm_flags |= (VM_DONTEXPAND | VM_DONTDUMP);
#endif


    ret = remap_pfn_range (vma, vma->vm_start, privdata->pci_res[1].start >> PAGE_SHIFT, bufsize, vma->vm_page_prot);
  }
  else
  {
    /* for external phys memory, use directly pfn*/
    pcie_wb_dbg(
        KERN_NOTICE "PCIe_Wishbone hardware is Mapping external address %lx / PFN %lx\n", (vma->vm_pgoff << PAGE_SHIFT ), vma->vm_pgoff);

    /* JAM tried to check via bios map if the requested region is usable or reserved
     * This will not work, since the e820map as present in Linux kernel was already cut above mem=1024M
     * So we would need to rescan the original bios entries, probably too much effort if standard MBS hardware is known
     * */
    /* phstart=  (u64) vma->vm_pgoff << PAGE_SHIFT;
     phend = phstart +  (u64) bufsize;
     phtype = E820_RAM;
     if(e820_any_mapped(phstart, phend, phtype)==0)
     {
     printk(KERN_ERR "PCIe_Wishbone hardware mmap: requested physical memory region  from %lx to %lx is not completely usable!\n", (long) phstart, (long) phend);
     return -EFAULT;
     }
     NOTE that e820_any_mapped only checks if _any_ memory inside region is mapped
     So it is the wrong method anyway?*/

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,7,0)
    vma->vm_flags |= (VM_RESERVED);
#else
    vma->vm_flags |= (VM_DONTEXPAND | VM_DONTDUMP);
#endif

    ret = remap_pfn_range (vma, vma->vm_start, vma->vm_pgoff, bufsize, vma->vm_page_prot);

  }

  if (ret)
  {
    pcie_wb_msg(
        KERN_ERR "PCIe_Wishbone hardware mmap: remap_pfn_range failed with %d\n", ret);
    return -EFAULT;
  }
  return ret;
}




static int probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	/* Do probing type stuff here.  
	 * Like calling request_region();
	 * reading BARs
	 * reading IRQ
	 * register char dev
	 */
	u8 revision;
	int err=0;
    char devname[64];
	struct pcie_wb_dev *dev;
	unsigned char* control;

	pci_read_config_byte(pdev, PCI_REVISION_ID, &revision);
	if (revision != 0x01) {
		printk(KERN_ALERT PCIE_WB ": revision ID wrong!\n");
		goto fail_out;
	}

	if (pci_enable_device(pdev) < 0) {
		printk(KERN_ALERT PCIE_WB ": could not enable device!\n");
		goto fail_out;
	}
	
	dev = kmalloc(sizeof(struct pcie_wb_dev), GFP_KERNEL);
	if (!dev) {
		printk(KERN_ALERT PCIE_WB ": could not allocate memory for pcie_wb_dev structure!\n");
		goto fail_disable;
	}
	
	/* Initialize structure */
	dev->pci_dev = pdev;
	dev->msi = 1;
	dev->wb.wops = &wb_ops;
	dev->wb.parent = &pdev->dev;
	dev->wb.mask = 0xffff;
	dev->window_offset = 0;
	dev->low_addr = 0;
	dev->width = 4;
	dev->shift = 0;
	pci_set_drvdata(pdev, dev);
	
	if (setup_bar(pdev, &dev->pci_res[0], 0) < 0) goto fail_free;
	if (setup_bar(pdev, &dev->pci_res[1], 1) < 0) goto fail_bar0;
	
	/* Initialize device registers */
	control = dev->pci_res[0].addr;
	iowrite32(0, control + WINDOW_OFFSET_LOW);
	iowrite32(0, control + CONTROL_REGISTER_HIGH);

	pci_set_master(pdev); /* enable bus mastering => needed for MSI */
	
	/*********************************************************************/
	/** now provide own device for direct TLU functionalities JAM2018 :*/
	 dev->devid = atomic_inc_return(&pcie_wb_numdevs) - 1;
	 if (dev->devid >= PCIE_WB_MAXDEVS)
	    {
	      pcie_wb_msg(KERN_ERR "Maximum number of devices reached! Increase PCIE_WB_MAXDEVS.\n");
	      goto fail_master;
	    }
	 dev->devno = MKDEV(MAJOR(pcie_wb_devt), MINOR(pcie_wb_devt) + dev->devid);


	  /* Register character device */
	 cdev_init (&(dev->cdev), &pcie_wb_fops);
	 dev->cdev.owner = THIS_MODULE;
	 dev->cdev.ops = &pcie_wb_fops;
	 err = cdev_add (&dev->cdev, dev->devno, 1);
	 if (err)
	    {
	      pcie_wb_msg( "Couldn't add character device.\n");
	      goto fail_cdev;
	    }

	 /* export special things to class in sysfs: */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
    if (!IS_ERR (pcie_wb_class))
    {
      /* driver init had successfully created class, now we create device:*/
      snprintf (devname, 64, "pcie_wb%d", MINOR(pcie_wb_devt) + dev->devid);
  #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
      dev->class_dev = device_create (pcie_wb_class, NULL, dev->devno, dev, devname);
  #else
      dev->class_dev = device_create(pcie_wb_class, NULL,
          dev->devno, devname);
  #endif
      dev_set_drvdata (dev->class_dev, dev);
      pcie_wb_msg (KERN_NOTICE "Added PCIE_WB device: %s", devname);

    // here the sysfs exports:

      if (device_create_file (dev->class_dev, &dev_attr_codeversion) != 0)
      {
        pcie_wb_msg (KERN_ERR "Could not add device file node for code version.\n");
        goto fail_sysfs;
      }
      if (device_create_file (dev->class_dev, &dev_attr_dactl) != 0)
       {
         pcie_wb_msg(KERN_ERR "Could not add device file node for direct access control register.\n");
         goto fail_sysfs;
       }

    }
    else
     {
       /* something was wrong at class creation, we skip sysfs device support here:*/
       pcie_wb_msg(KERN_ERR "Could not add PCIE_WB device node to /dev !");
       goto fail_sysfs;
     }

#endif

/* JAM2018 end special things for direct TLU export handles
**************************************************************************************/






	/* enable message signaled interrupts */
	if (pci_enable_msi(pdev) != 0) {
		/* resort to legacy interrupts */
		printk(KERN_ALERT PCIE_WB ": could not enable MSI interrupting (using legacy)\n");
		dev->msi = 0;
	}
	
	if (dev->msi) {
		/* disable legacy interrupts when using MSI */
		pci_intx(pdev, 0); 
	}
	
	if (wishbone_register(&dev->wb) < 0) {
		printk(KERN_ALERT PCIE_WB ": could not register wishbone bus\n");
		goto fail_msi;
	}
	
	if (request_irq(pdev->irq, irq_handler, IRQF_SHARED, "pcie_wb", dev) < 0) {
		printk(KERN_ALERT PCIE_WB ": could not register interrupt handler\n");
		goto fail_reg;
	}
	
	/* Enable classic interrupts */
	pcie_int_enable(dev, 1);

	return 0;

fail_reg:
	wishbone_unregister(&dev->wb);
fail_msi:	
	if (dev->msi) {
		pci_intx(pdev, 1);
		pci_disable_msi(pdev);
	}

fail_sysfs:
  if (dev->class_dev)
  {
    device_remove_file (dev->class_dev, &dev_attr_dactl);
    device_remove_file (dev->class_dev, &dev_attr_codeversion);
  }
  device_destroy (pcie_wb_class, dev->devno);


fail_cdev:
    cdev_del (&dev->cdev);
    atomic_dec (&pcie_wb_numdevs);

fail_master:
	pci_clear_master(pdev);

	/*fail_bar1:*/
	destroy_bar(&dev->pci_res[1]);
fail_bar0:
	destroy_bar(&dev->pci_res[0]);
fail_free:
	kfree(dev);


fail_disable:
	pci_disable_device(pdev);
fail_out:
	return -EIO;
}

static void remove(struct pci_dev *pdev)
{
	struct pcie_wb_dev *dev;
	
	dev = pci_get_drvdata(pdev);
	
	pcie_int_enable(dev, 0);
	free_irq(dev->pci_dev->irq, dev);
	wishbone_unregister(&dev->wb);
	if (dev->msi) {
		pci_intx(pdev, 1);
		pci_disable_msi(pdev);
	}

	/************
	 * clean up the sysfs stuff: */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
  /* sysfs device cleanup */
    device_remove_file (dev->class_dev, &dev_attr_dactl);
    device_remove_file (dev->class_dev, &dev_attr_codeversion);
    device_destroy (pcie_wb_class, dev->devno);
#endif

  /*** char device */

	pci_clear_master(pdev);
	destroy_bar(&dev->pci_res[1]);
	destroy_bar(&dev->pci_res[0]);
	kfree(dev);
	pci_disable_device(pdev);
}

static struct pci_device_id ids[] = {
	{ PCI_DEVICE(PCIE_WB_VENDOR_ID, PCIE_WB_DEVICE_ID), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, ids);

static struct pci_driver pcie_wb_driver = {
	.name = PCIE_WB,
	.id_table = ids,
	.probe = probe,
	.remove = remove,
};

static int __init pcie_wb_init(void)
{
  int result;
  pcie_wb_devt = MKDEV(my_major_nr, 0);

    /*
     * Register your major, and accept a dynamic number.
     */
    if (my_major_nr)
    {
      result = register_chrdev_region (pcie_wb_devt, PCIE_WB_MAXDEVS, PCIE_WB_NAME);
    }
    else
    {
      result = alloc_chrdev_region (&pcie_wb_devt, 0, PCIE_WB_MAXDEVS, PCIE_WB_NAME);
      my_major_nr = MAJOR(pcie_wb_devt);
    }
    if (result < 0)
    {
      pcie_wb_msg(
          KERN_ALERT "Could not alloc chrdev region for major: %d !\n", my_major_nr);
      return result;
    }

  #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
    pcie_wb_class = class_create (THIS_MODULE, PCIE_WB_NAME);
    if (IS_ERR (pcie_wb_class))
    {
      pcie_wb_msg(KERN_ALERT "Could not create class for sysfs support!\n");
    }
#endif
    if (pci_register_driver (&pcie_wb_driver) < 0)
     {
      pcie_wb_msg(KERN_ALERT "pci driver could not register!\n");
       unregister_chrdev_region (pcie_wb_devt, PCIE_WB_MAXDEVS);
       return -EIO;
     }
    pcie_wb_msg(
         KERN_NOTICE "\t\tdriver init with registration for major no %d done.\n", my_major_nr);
return 0;

}

static void __exit pcie_wb_exit(void)
{	

  unregister_chrdev_region (pcie_wb_devt, PCIE_WB_MAXDEVS);
  pci_unregister_driver(&pcie_wb_driver);

 #if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
   if (pcie_wb_class != NULL )
     class_destroy (pcie_wb_class);
 #endif






}

MODULE_AUTHOR(PCIE_WB_AUTHORS);
MODULE_DESCRIPTION(PCIE_WB_DESC);
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable debugging information");
MODULE_LICENSE("GPL");
MODULE_VERSION(PCIE_WB_VERSION);

module_init(pcie_wb_init);
module_exit(pcie_wb_exit);
