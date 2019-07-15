#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
#include <linux/pci-p2pdma.h>
#endif

#define DRV_NAME          "nettlp"
#define NETTLP_VERSION  "0.0.2"

struct mmio {
	uint8_t *virt;
	uint64_t start;
	uint64_t end;
	uint64_t flags;
	uint64_t len;
};


struct nettlp {
	struct mmio bar0;
	struct mmio bar2;
	struct mmio bar4;
};

struct nettlp_dev {
	struct nettlp dev;
};



static void nettlp_store_bar_info(struct pci_dev *pdev,
				  struct mmio *bar, int barn)
{
	bar->start = pci_resource_start(pdev, barn);
	bar->end   = pci_resource_end(pdev, barn);
	bar->flags = pci_resource_flags(pdev, barn);
	bar->len   = pci_resource_len(pdev, barn);

	pr_info("BAR%d start: %#llx\n", barn, bar->start);
	pr_info("BAR%d end  : %#llx\n", barn, bar->end);
	pr_info("BAR%d flags: 0x%llx\n", barn, bar->flags);
	pr_info("BAR%d len  : %llu\n", barn, bar->len);
}

static int nettlp_pci_init(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	int rc;
	struct nettlp_dev *nt;

	pr_info("%s: register nettlp device %s\n", __func__, pci_name(pdev));

	nt = kmalloc(sizeof(*nt), GFP_KERNEL);
	if (!nt)
		return -ENOMEM;

	pci_set_drvdata(pdev, nt);

	rc = pci_enable_device(pdev);
	if (rc)
		goto error;

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc)
		goto error;

	/* set BUS Master Mode */
	pci_set_master(pdev);

	/* BAR0 (pcie pio) */
	nettlp_store_bar_info(pdev, &nt->dev.bar0, 0);

	/* BAR2 (pcie DMA) */
	nettlp_store_bar_info(pdev, &nt->dev.bar2, 2);

	/* BAR4 (pseudo memory-dependent) */
	nettlp_store_bar_info(pdev, &nt->dev.bar4, 4);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	pr_info("Allocate BAR4 as p2pdma memory\n");
	rc = pci_p2pdma_add_resource(pdev, 4, nt->dev.bar4.len, 0);
	if (rc) {
		pr_err("failed to register BAR4 as p2pdma resource\n");
		goto error;
	}
	pci_p2pmem_publish(pdev, true);
#endif
	
	return 0;

error:
	pr_info("nettlp_pci_init error\n");
	pci_set_drvdata(pdev, NULL);
	kfree(nt);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	return -1;
}

static void nettlp_pci_remove(struct pci_dev *pdev)
{
	struct nettlp_dev *nt = pci_get_drvdata(pdev);

	pr_info("%s: remove nettlp device %s\n", __func__, pci_name(pdev));

	pci_set_drvdata(pdev, NULL);
	kfree(nt);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}


static const struct pci_device_id nettlp_pci_tbl[] = {
	{0x3776, 0x8022, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{0,}
};
MODULE_DEVICE_TABLE(pci, nettlp_pci_tbl);

struct pci_driver nettlp_pci_driver = {
	.name = DRV_NAME,
	.id_table = nettlp_pci_tbl,
	.probe = nettlp_pci_init,
	.remove = nettlp_pci_remove,
	//	.suspend = nettlp_suspend,
	//	.resume = nettlp_resume,
};

static int __init nt_init(void)
{
	pr_info("nettlp (v%s) is loaded\n", NETTLP_VERSION);

	return pci_register_driver(&nettlp_pci_driver);
}
module_init(nt_init);


static void __exit nt_release(void)
{
	pci_unregister_driver(&nettlp_pci_driver);
	pr_info("nettlp (v%s) is unloaded\n", NETTLP_VERSION);
	return;
}
module_exit(nt_release);


MODULE_AUTHOR("Yohei Kuga <sora@haeena.net>");
MODULE_DESCRIPTION("nettlp");
MODULE_LICENSE("GPL");
MODULE_VERSION(NETTLP_VERSION);

