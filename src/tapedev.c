#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "tapedev.h"

#define DRIVER_NAME "tapedev"
#define TAPEDEV_MAX_SECTIONS 8

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tapedev device driver");
MODULE_AUTHOR("poleszc");

// =================== DEVICE ===================
struct tapedev_section {
  struct tapedev_device *device;
  int id; ///< 0...7, section N from `/dev/tapedevXsN`

  u32 tape_type; ///> 0...4
  u32 tapes_count;
  u64 tape_size;  ///< bytes per tape
  u32 block_size; ///< bytes per device block

  void *pgtable;
  dma_addr_t pgtable_dma;
};

struct tapedev_device {
  struct pci_dev *pdev;
  void __iomem *bar;

  int device_numer; ///< X from `/dev/tapedevX`

  u32 section_count;
  struct tapedev_section sections[TAPEDEV_MAX_SECTIONS];
};

// =================== PCI DRIVER ===================

static int tapedev_probe(struct pci_dev *pdev,
                         const struct pci_device_id *pci_id) {
  // TODOP: Implement
  return 0;
}

static void tapedev_remove(struct pci_dev *pdev) {
  // TODOP: Implement
  return;
}

static struct pci_device_id tapedev_pci_ids[] = {
    {PCI_DEVICE(PCI_VENDOR_ID_TAPEDEV, PCI_DEVICE_ID_TAPEDEV)},
    {0},
};

static struct pci_driver tapedev_pci_driver = {
    .name = "tapedev",
    .id_table = tapedev_pci_ids,
    .probe = tapedev_probe,
    .remove = tapedev_remove,
};

// =================== INIT $ EXIT ===================
static void tapedev_init(void) {
  // TODOP: Implement
  return;
}

static void tapedev_exit(void) {
  // TODOP: Implement
  return;
}

module_init(tapedev_init);
module_exit(tapedev_exit);