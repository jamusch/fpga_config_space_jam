#ifndef PCIE_WB_DRIVER_H
#define PCIE_WB_DRIVER_H

#include "wishbone.h"

#define PCIE_WB "pcie_wb"
#define PCIE_WB_VERSION	"0.2"

#define PCIE_WB_VENDOR_ID	0x10dc
#define	PCIE_WB_DEVICE_ID	0x019a

#define CONTROL_REGISTER_HIGH	0
#define CONTROL_REGISTER_LOW	4
#define ERROR_FLAG_HIGH		8
#define ERROR_FLAG_LOW		12
#define WINDOW_OFFSET_HIGH	16
#define WINDOW_OFFSET_LOW	20
#define SDWB_ADDRESS_HIGH	24
#define SDWB_ADDRESS_LOW	28

#define MASTER_CTL_HIGH		64
#define MASTER_CTL_LOW		68
#define MASTER_ADR_HIGH		72
#define MASTER_ADR_LOW		76
#define MASTER_DAT_HIGH		80
#define MASTER_DAT_LOW		84

#define WINDOW_HIGH	0xFFFF0000UL
#define WINDOW_LOW	0x0000FFFCUL


/* new control register by Michael Reese 2017:*/
#define DIRECT_ACCESS_CONTROL 4

/** maximum number of devices controlled by this driver*/
#define PCIE_WB_MAXDEVS 4

/** eported name of device*/
#define PCIE_WB_NAME    "pcie_wb"

#define PCIE_WB_AUTHORS     "Stefan Rauch <s.rauch@gsi.de>, JAM <j.adamczewski@gsi.de>"
#define PCIE_WB_DESC        "GSI Altera-Wishbone bridge driver"




/* One per BAR */
struct pcie_wb_resource {
	unsigned long start;			/* start addr of BAR */
	unsigned long end;			/* end addr of BAR */
	unsigned long size;			/* size of BAR */
	void *addr;				/* remapped addr */
};

/* One per physical card - JAM this is the private data*/
struct pcie_wb_dev {
	struct pci_dev* pci_dev;
	struct pcie_wb_resource pci_res[2];
	int    msi;
	
	struct wishbone wb;
	unsigned int window_offset;
	unsigned int low_addr, width, shift;

	/** JAM2018 - required to export pcie_wb for direct TLU access*/
	 dev_t devno; /**< device number (major and minor) */
	 int devid; /**< local id (counter number) */
	 struct device *class_dev; /**< Class device */
	 struct cdev cdev; /**< char device struct */


};



int pcie_wb_open(struct inode *inode, struct file *filp);
int pcie_wb_release(struct inode *inode, struct file *filp);

int pcie_wb_mmap(struct file *filp, struct vm_area_struct *vma);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
/** export something to sysfs: */
ssize_t pcie_wb_sysfs_codeversion_show(struct device *dev,
        struct device_attribute *attr, char *buf);

ssize_t pcie_wb_sysfs_dactl_show (struct device *dev, struct device_attribute *attr, char *buf);

ssize_t pcie_wb_sysfs_dactl_store (struct device *dev, struct device_attribute *attr, const char *buf, size_t count);




#endif

#endif
