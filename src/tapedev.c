#include <linux/blkdev.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "tapedev.h"

#define DEBUG_ON

#ifdef DEBUG_ON
#define DEBUG(TEXT) printk(KERN_ALERT "[TAPEDEV]: " TEXT);
#else
#define DEBUG(TEXT)
#endif

#define TAPEDEV_NAME "tapedev"
#ifndef KBUILD_MODNAME // TODOP: IS that needed?
#define KBUILD_MODNAME TAPEDEV_NAME
#endif
#define TAPEDEV_MAX_DEVICES 256
#define TAPEDEV_MAX_SECTIONS 8

// Sources:
// https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("tapedev device driver");
MODULE_AUTHOR("poleszc");

// =================== DEVICE STUCTURES ===================
// pci_dev (PCI card) -> tapedev_device (out device) ->
// tapedev_section[N] (single section in the device)

struct tapedev_device; // FD

/**
 * @brief Represents a single tapedev section.
 * Will be exported as `/dev/tapedevXsN`
 */
struct tapedev_section {
  /// Pointer to the tapedev_device this section belongs to.
  struct tapedev_device *device;
  /// 0...7, section N from `/dev/tapedevXsN`
  int id;

  // === Command synchronization ===
  struct mutex cmd_mutex;
  wait_queue_head_t cmd_wq;
  /// Set in the interrupt, zeroed before inserting a command
  bool cmd_done;
  /// SECT_STATUS_ADDR read in the interrupt.
  u32 cmd_status;

  // === Tape information ===
  /// Tape type 0...4
  u32 tape_type;
  /// Number of tapes in this section.
  u32 tapes_count;
  /// The size of a single tape in bytes.
  u64 tape_size;
  /// I/O block size of the device
  u32 block_size;

  // === DMA ===
  void *pgtable;
  dma_addr_t pgtable_dma;
};

struct tapedev_device {
  /// PCI object handling this device.
  struct pci_dev *pdev;
  /// Index of this device. X in `/dev/tapedevX`
  int idx;
  /// Mapped BAR0
  void __iomem *bar;

  /// For protecting the flags and states.
  spinlock_t slock;

  /// Handshake - the device is done with init.
  wait_queue_head_t init_wq;
  bool init_done;

  /// Number of sections in the device.
  u32 section_count;
  struct tapedev_section sections[TAPEDEV_MAX_SECTIONS];
};

// =================== GLOBAL STATE ===================
/// All active devices.
static struct tapedev_device *tapedev_devices[TAPEDEV_MAX_DEVICES];
/// Lock to protect the above.
static DEFINE_MUTEX(tapedev_devices_lock);
/// Major number assigned by register_blkdev.
static int tapedev_major;

// =================== HARDWARE HANDLING ===================
#define TDEV_IOR(TAPE_DEV, REG) ioread32(TAPE_DEV->bar + REG)
#define TDEV_IOW(TAPE_DEV, REG, VAL) iowrite32(VAL, TAPE_DEV->bar + REG)
/// Sections are stored under (id+1)*0x100 since HW starts numbering from 1.
#define TDEV_SECTION_OFFSET(SECT_ID, REG) ((SECT_ID + 1) * 0x100 + REG)
#define TDEV_SECTION_IOR(TAPE_DEV, SECT_ID, REG)                               \
  TDEV_IOR(TAPE_DEV, TDEV_SECTION_OFFSET(SECT_ID, REG))
#define TDEV_SECTION_IOW(TAPE_DEV, SECT_ID, REG, VAL)                          \
  TDEV_IOW(TAPE_DEV, TDEV_SECTION_OFFSET(SECT_ID, REG), VAL)
#define TDEV_SECTION_SIZE(TAPE_TYPE) ((32ULL) * BIT(TAPE_TYPE) * 8192u)

// =================== INTERRUPTS ===================
static irqreturn_t tapedev_isr(int irg, void *opaque) {
  DEBUG("ISR Enter\n");
  struct tapedev_device *dev = opaque;
  unsigned long flags;
  u32 istatus, handled = 0;
  int i;

  // Protect the access to the d->sections[*].cmd_done.
  spin_lock_irqsave(&dev->slock, flags);
  // Check what caused the interrupt.
  istatus = TDEV_IOR(dev, TAPEDEV_IRQ_STATUS_ADDR);
  if (!istatus) {
    spin_unlock_irqrestore(&dev->slock, flags);
    return IRQ_NONE;
  }

  // Init done - device has finished initing. Wake up someone waiting in
  // tapedev_enable_hw().
  if (istatus & BIT(TAPEDEV_IRQ_INIT_DONE)) {
    DEBUG("ISR Done\n");
    dev->init_done = true;
    wake_up(&dev->init_wq);
    handled |= BIT(TAPEDEV_IRQ_INIT_DONE);
  }

  // Hardware error.
  if (istatus & BIT(TAPEDEV_IRQ_HW_ERROR)) {
    DEBUG("ISR Error\n");
    dev_err(&dev->pdev->dev, "hardware error from device\n");
    handled |= BIT(TAPEDEV_IRQ_HW_ERROR);
  }

  // Check done/error for each section.
  for (i = 0; i < dev->section_count; i++) {
    if (istatus & (BIT(TAPEDEV_IRQ_SECT_0_DONE + i) |
                   BIT(TAPEDEV_IRQ_SECT_0_ERROR + i))) {
      DEBUG("ISR Section\n");
      struct tapedev_section *sec = &dev->sections[i];

      // Save the status for it to be interpreted in the consumer thread.
      sec->cmd_status = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_STATUS_ADDR);
      sec->cmd_done = true;
      wake_up(&sec->cmd_wq);

      handled |= (BIT(TAPEDEV_IRQ_SECT_0_DONE + i) |
                  BIT(TAPEDEV_IRQ_SECT_0_ERROR + i));
    }
  }

  // Clear the bits we have handled.
  TDEV_IOW(dev, TAPEDEV_IRQ_CLEAR_ADDR, handled);

  spin_unlock_irqrestore(&dev->slock, flags);
  return IRQ_HANDLED;
}

// =================== STARTING THE DEVICE ===================
static int tapedev_enable_hw(struct tapedev_device *dev) {
  u32 mask;
  int i;

  // Clear all interrupts.
  TDEV_IOW(dev, TAPEDEV_IRQ_CLEAR_ADDR, ~0u);

  // Enable TAPEDEV_IRQ_INIT_DONE and TAPEDEV_IRQ_HW_ERROR.
  mask = ~((u32)BIT(TAPEDEV_IRQ_INIT_DONE) | (u32)BIT(TAPEDEV_IRQ_HW_ERROR));
  TDEV_IOW(dev, TAPEDEV_IRQ_MASK_ADDR, mask);

  dev->init_done = false;

  // Do the thing.
  TDEV_IOW(dev, TAPEDEV_ENABLE_ADDR, 1);

  // Wait for the interrupt.
  if (!wait_event_timeout(dev->init_wq, dev->init_done, 5 * HZ)) {
    DEBUG("Init timeout\n");
    return -ETIMEDOUT;
  }

  dev->section_count = TDEV_IOR(dev, TAPEDEV_SECTIONS_ADDR);
  if (dev->section_count == 0 || dev->section_count > TAPEDEV_MAX_SECTIONS) {
    DEBUG("Invalid section count\n");
    return -EIO; // TODOP: Is tha so?
  }

  for (i = 0; i < dev->section_count; i++) {
    mask &= ~((u32)BIT(TAPEDEV_IRQ_SECT_0_DONE + i));
    mask &= ~((u32)BIT(TAPEDEV_IRQ_SECT_0_ERROR + i));
  }
  TDEV_IOW(dev, TAPEDEV_IRQ_MASK_ADDR, mask);
  return 0;
}

// =================== SECTION INIT & DEINIT ===================
static int tapedev_init_section(struct tapedev_device *dev, int i) {
  struct tapedev_section *sec = &dev->sections[i];

  sec->device = dev;
  sec->id = i;
  mutex_init(&sec->cmd_mutex);
  init_waitqueue_head(&sec->cmd_wq);
  sec->cmd_done = false;

  sec->tapes_count = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPES_ADDR);
  sec->tape_type = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPE_SIZE_ADDR);
  sec->tape_size = TDEV_SECTION_SIZE(sec->tape_type);
  // TODOP: Make this configurable.
  sec->block_size = 4096;
  TDEV_SECTION_IOW(dev, i, TAPEDEV_SECT_TAPE_BLOCKSIZE_ADDR,
                   TAPEDEV_SECT_BLOCKSIZE_4096);

  sec->pgtable = dma_alloc_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE,
                                    &sec->pgtable_dma, GFP_KERNEL);
  if (!sec->pgtable)
    return -ENOMEM;
  // TODOP: Change to debug.
  dev_info(&dev->pdev->dev, "section %d: %u tapes, type %u, %llu B per tape\n",
           i, sec->tapes_count, sec->tape_type, sec->tape_size);
  return 0;
}

static void tapedev_free_section(struct tapedev_device *dev, int i) {
  struct tapedev_section *sec = &dev->sections[i];
  if (sec->pgtable) {
    dma_free_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE, sec->pgtable,
                      sec->pgtable_dma);
    sec->pgtable = NULL;
  }
}

// =================== BLOCK DEVICE ===================
// static int tapedev_open(struct block_device *bdev, fmode_t mode) {
//   // TODOP: Implement
//   return 0;
// }

// static int tapedev_release(struct gendisk *gd, fmode_t mode) {
//   // TODOP: Implement
//   return 0;
// }

// static const struct block_device_operations tapedev_ops = {
//     .owner = THIS_MODULE,
//     .open = tapedev_open,
//     .release = tapedev_release,
//     // .ioctl = tapedev_ioclt,
// };

// static int create_block_device(struct tapedev_device *dev) {

//   // Initialize the gendisk structure.
//   dev->gd = alloc_disk(TAPEDEV_MINOR);
//   if (!dev->gd) {
//     printk(KERN_NOTICE "alloc_disk failure");
//     return -ENOMEM;
//   }

//   // Fill out the gendisk fields.
//   dev->gd->major = TAPEDEV_MAJOR;
//   dev->gd->first_minor = TAPEDEV_MINOR;
//   dev->gd->fops = &tapedev_ops;
//   dev->gd->queue = dev->queue;
//   dev->gd->private_data = dev;
//   snprintf(dev->gd->disk_name, 32, "tapedev");
//   set_capacity(dev->gd, NR_SECTORS);

//   // TODOP: Implement.
//   add_disk(dev->gd);
//   return 0;
// }

// static int delete_block_device(struct tapedev_device *dev) {
//   if (dev->gd)
//     del_gendisk(dev->gd);
//   // TODOP: Implement
// }

// =================== PCI DRIVER ===================

static int tapedev_probe(struct pci_dev *pdev,
                         const struct pci_device_id *pci_id) {
  // Strongly inspired by `adlerdev_probe`.
  int err, i;

  struct tapedev_device *dev = kzalloc(sizeof(*dev), GFP_KERNEL);
  if (!dev) {
    err = -ENOMEM;
    goto out_alloc;
  }
  // Save the PCI device.
  pci_set_drvdata(pdev, dev);
  dev->pdev = pdev;

  // Locks etc.
  spin_lock_init(&dev->slock);
  init_waitqueue_head(&dev->init_wq);

  // Allocate the free index.
  mutex_lock(&tapedev_devices_lock);
  for (i = 0; i < TAPEDEV_MAX_DEVICES; i++)
    if (!tapedev_devices[i])
      break;
  if (i == TAPEDEV_MAX_DEVICES) {
    err = -ENOSPC;
    mutex_unlock(&tapedev_devices_lock);
    goto out_slot;
  }
  tapedev_devices[i] = dev;
  dev->idx = i;
  mutex_unlock(&tapedev_devices_lock);

  // Enable PCI device.
  if ((err = pci_enable_device(pdev)))
    goto out_enable;

  // Set the DMA mask.
  if ((err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))))
    goto out_mask;

  // TODOP: What does this do?
  pci_set_master(pdev);

  if ((err = pci_request_regions(pdev, TAPEDEV_NAME)))
    goto out_regions;

  // Map the BAR.
  if (!(dev->bar = pci_iomap(pdev, 0, 0))) {
    err = -ENOMEM;
    goto out_bar;
  }

  // Connect the IRQ line.
  if ((err =
           request_irq(pdev->irq, tapedev_isr, IRQF_SHARED, TAPEDEV_NAME, dev)))
    goto out_irq;

  // Turn on the device and wait for INIT_DONE.
  if ((err = tapedev_enable_hw(dev)))
    goto out_enable_hw;

  // Initialize each section.
  for (i = 0; i < dev->section_count; i++) {
    if ((err = tapedev_init_section(dev, i))) {
      // Free all sections registered up to this point when an error occurs.
      while (--i >= 0)
        tapedev_free_section(dev, i);
      goto out_sections;
    }
  }

  // TODOP: Change to DEBUG;
  dev_info(&pdev->dev, "tapedev%d ready, %u sections\n", dev->idx,
           dev->section_count);
  return 0;

out_sections:
  // TODOP: Is that needed?
  // Mask all incoming interrupts for them not to come when cleaning up.
  TDEV_IOW(dev, TAPEDEV_IRQ_MASK_ADDR, ~0u);
  TDEV_IOW(dev, TAPEDEV_ENABLE_ADDR, 0);
out_enable_hw:
  free_irq(pdev->irq, dev);
out_irq:
  pci_iounmap(pdev, dev->bar);
out_bar:
  pci_release_regions(pdev);
out_regions:
out_mask:
  pci_disable_device(pdev);
out_enable:
  mutex_lock(&tapedev_devices_lock);
  tapedev_devices[dev->idx] = 0;
  mutex_unlock(&tapedev_devices_lock);
out_slot:
  kfree(dev);
out_alloc:
  return err;
}

static void tapedev_remove(struct pci_dev *pdev) {
  // Strongly inspired by `adlerdev_remove`.
  struct tapedev_device *dev = pci_get_drvdata(pdev);
  int i;

  // Mask all interrupts when disableing the device.
  TDEV_IOW(dev, TAPEDEV_IRQ_MASK_ADDR, ~0u);
  TDEV_IOW(dev, TAPEDEV_ENABLE_ADDR, 0);

  for (i = 0; i < dev->section_count; i++)
    tapedev_free_section(dev, i);

  // Cleanup PCI.
  free_irq(pdev->irq, dev);
  pci_iounmap(pdev, dev->bar);
  pci_release_regions(pdev);
  pci_disable_device(pdev);

  // Free the index.
  mutex_lock(&tapedev_devices_lock);
  tapedev_devices[dev->idx] = 0;
  mutex_unlock(&tapedev_devices_lock);

  kfree(dev);
}

static struct pci_device_id tapedev_pci_ids[] = {
    {PCI_DEVICE(PCI_VENDOR_ID_TAPEDEV, PCI_DEVICE_ID_TAPEDEV)},
    {0},
};

static struct pci_driver tapedev_pci_driver = {
    .name = TAPEDEV_NAME,
    .id_table = tapedev_pci_ids,
    .probe = tapedev_probe,
    .remove = tapedev_remove,
};

// =================== MODULE INIT & EXIT ===================
static int tapedev_init(void) {
  int err;

  // When passing 0, `register_blkdev()` will assign us the major.
  tapedev_major = register_blkdev(0, TAPEDEV_NAME);
  if (tapedev_major < 0) {
    DEBUG("Unable to register tapedev block device");
    return tapedev_major;
  }

  // Register PCI driver. Probe will be called automagically.
  if ((err = pci_register_driver(&tapedev_pci_driver))) {
    unregister_blkdev(tapedev_major, TAPEDEV_NAME);
    DEBUG("Unable to register PCI driver");
    return err;
  }
  return 0;
}

static void tapedev_exit(void) {
  pci_unregister_driver(&tapedev_pci_driver);
  unregister_blkdev(tapedev_major, TAPEDEV_NAME);
  return;
}

module_init(tapedev_init);
module_exit(tapedev_exit);