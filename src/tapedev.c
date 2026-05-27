#include <linux/blk-mq.h>
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

#include "asm-generic/errno-base.h"
#include "asm-generic/errno.h"
#include "linux/blk_types.h"
#include "linux/byteorder/generic.h"
#include "linux/device.h"
#include "linux/nodemask_types.h"
#include "tapedev.h"

#define DEBUG_ON

#ifdef DEBUG_ON
#define DEBUG(fmt, ...) printk(KERN_ALERT "[TAPEDEV]: " fmt, ##__VA_ARGS__)
#else
#define DEBUG(fmt, ...)
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

  // === Block Layer ===
  struct gendisk *gd;
  struct blk_mq_tag_set tag_set;
  /// The tape we're currently on. 0 means no tape.
  u32 current_tape;
  /// The position within the tape (in blocks).
  u64 current_pos;
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

/**
 * @brief Fill one page table entry
 * @p sec section
 * @p idx entry index (0...511)
 * @p addr DMA address if the buffer (512B-aligned)
 * @p block_count block size
 */
static void fill_pgtable_entry(struct tapedev_section *sec, int idx,
                               dma_addr_t addr, u32 block_count) {
  __le64 *pgt = sec->pgtable;
  u64 entry;

  WARN_ON(addr & 0x1ff);

  entry = ((u64)(addr >> 9) << 32) | (u64)block_count;
  pgt[idx] = cpu_to_le64(entry);
}

/**
 * @brief Clear the pgtable entries starting from `keep_n`
 */
static void clear_pgtable_after(struct tapedev_section *sec, int keep_n) {
  __le64 *pgt = sec->pgtable;
  int i;

  for (i = keep_n; i < TAPEDEV_BUF_PGTABLE_ENTRIES; i++)
    pgt[i] = 0;
}

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

static int tapedev_open(struct gendisk *disk, blk_mode_t mode) {
  // TODOP: Implement
  return 0;
}

static void tapedev_release(struct gendisk *disk) {
  // TODOP: Implement
  return;
}

/**
 * @brief Invoked  by the block layer for each request.
 */
static blk_status_t tapedev_queue_rq(struct blk_mq_hw_ctx *hctx,
                                     const struct blk_mq_queue_data *bd) {
  struct request *rq = bd->rq;
  struct tapedev_section *sec = rq->q->queuedata;

  blk_mq_start_request(rq);

  DEBUG("queue_rq sec=%d op=%u pos=%llu bytes=%u\n", sec->id, req_op(rq),
        (u64)blk_rq_pos(rq), blk_rq_bytes(rq));

  // TODOP: Do the work

  blk_mq_end_request(rq, BLK_STS_IOERR);
  return BLK_STS_OK;
}

static struct blk_mq_ops tapedev_mq_ops = {
    .queue_rq = tapedev_queue_rq,
};

static const struct block_device_operations tapedev_block_ops = {
    .owner = THIS_MODULE,
    .open = tapedev_open,
    .release = tapedev_release,
    // .ioctl = tapedev_ioclt,
};

// =================== SECTION INIT & DEINIT ===================
static int tapedev_init_section(struct tapedev_device *dev, int i) {
  struct tapedev_section *sec = &dev->sections[i];
  u64 total_sectors;
  int err;

  // Initialize general data.
  sec->device = dev;
  sec->id = i;
  mutex_init(&sec->cmd_mutex);
  init_waitqueue_head(&sec->cmd_wq);
  sec->cmd_done = false;
  sec->current_tape = 0;
  sec->current_pos = 0;

  sec->tapes_count = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPES_ADDR);
  sec->tape_type = TDEV_SECTION_IOR(dev, i, TAPEDEV_SECT_TAPE_SIZE_ADDR);
  sec->tape_size = TDEV_SECTION_SIZE(sec->tape_type);
  // TODOP: Make this configurable.
  sec->block_size = 4096;
  TDEV_SECTION_IOW(dev, i, TAPEDEV_SECT_TAPE_BLOCKSIZE_ADDR,
                   TAPEDEV_SECT_BLOCKSIZE_4096);

  // Initialize the pgtable.
  sec->pgtable = dma_alloc_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE,
                                    &sec->pgtable_dma, GFP_KERNEL);
  if (!sec->pgtable)
    return -ENOMEM;

  // Initialize the block layer.

  // Initialize tag set.
  memset(&sec->tag_set, 0, sizeof(sec->tag_set)); // TODOP: Needed?
  sec->tag_set.ops = &tapedev_mq_ops;
  sec->tag_set.nr_hw_queues = 1;
  sec->tag_set.queue_depth = 1; // TODOP: Validate
  sec->tag_set.numa_node = NUMA_NO_NODE;
  sec->tag_set.cmd_size = 0;
  sec->tag_set.flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_BLOCKING;

  err = blk_mq_alloc_tag_set(&sec->tag_set);
  if (err) {
    dev_err(&dev->pdev->dev, "blk_mq_alloc_tag_set failed: %d\n", err);
    goto out_pgtable;
  }

  // Initialize the gendisk
  // https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html
  sec->gd = blk_mq_alloc_disk(&sec->tag_set, sec, &sec->queue);
  if (IS_ERR(sec->gd)) {
    err = PTR_ERR(sec->gd);
    dev_err(&dev->pdev->dev, "blk_mq_alloc_disk failed: %d\n", err);
    sec->gd = NULL;
    goto out_tagset;
  }

  sec->gd->major = tapedev_major;
  sec->gd->first_minor = dev->idx * TAPEDEV_MAX_SECTIONS + i;
  sec->gd->minors = 1;
  sec->gd->fops = &tapedev_block_ops;
  sec->gd->private_data = sec;
  snprintf(sec->gd->disk_name, DISK_NAME_LEN, "tapedev%ds%d", dev->idx, i);

  // Queue limits.
  blk_queue_logical_block_size(sec->gd->queue, sec->block_size);
  blk_queue_physical_block_size(sec->gd->queue, sec->block_size);
  // Maximum number of segments per request - 512
  blk_queue_max_segments(sec->gd->queue, TAPEDEV_BUF_PGTABLE_ENTRIES);
  // Maximum size of a single request. 4096 sectors * 512.
  blk_queue_max_hw_sectors(sec->gd->queue, 4096);
  // DMA buffers have to be aligned to block_size.
  blk_queue_dma_alignment(sec->gd->queue, sec->block_size - 1);

  total_sectors = ((u64)sec->tapes_count * sec->tape_size) >> 9;
  set_capacity(sec->gd, total_sectors);

  // Add the disk.
  err = device_add_disk(&dev->pdev->dev, sec->gd, NULL);
  if (err) {
    dev_err(&dev->pdev->dev, "device_add_disk failed: %d\n", err);
    goto out_putdisk;
  }

  dev_info(&dev->pdev->dev,
           "section %d: %u tapes, type %u, %llu B per tape, /dev/%s (%llu "
           "sectors)\n",
           i, sec->tapes_count, sec->tape_type, sec->tape_size,
           sec->gd->disk_name, total_sectors);
  return 0;

out_putdisk:
  put_disk(sec->gd);
  sec->gd = NULL;
out_tagset:
  blk_mq_free_tag_set(&sec->tag_set);
out_pgtable:
  dma_free_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE, sec->pgtable,
                    sec->pgtable_dma);
  sec->pgtable = NULL;
  return err;
}

static void tapedev_free_section(struct tapedev_device *dev, int i) {
  struct tapedev_section *sec = &dev->sections[i];

  if (sec->gd) {
    del_gendisk(sec->gd);
    put_disk(sec->gd);
    sec->gd = NULL;
    blk_mq_free_tag_set(&sec->tag_set);
  }

  if (sec->pgtable) {
    dma_free_coherent(&dev->pdev->dev, TAPEDEV_BUF_PGTABLE_SIZE, sec->pgtable,
                      sec->pgtable_dma);
    sec->pgtable = NULL;
  }
}

// =================== COMMANDS ===================
static const char *tapedev_status_name(u32 status) {
  switch (status) {
  case TAPEDEV_SECT_STATUS_IDLE:
    return "IDLE";
  case TAPEDEV_SECT_STATUS_WORKING:
    return "WORKING";
  case TAPEDEV_SECT_STATUS_DONE:
    return "DONE";
  case TAPEDEV_SECT_STATUS_ERR_INVALID_CMD:
    return "CMD";
  case TAPEDEV_SECT_STATUS_ERR_TAPE_ACTIVE:
    return "ACTIVE";
  case TAPEDEV_SECT_STATUS_ERR_NO_TAPE:
    return "TAPE";
  case TAPEDEV_SECT_STATUS_ERR_RESET:
    return "RESET";
  case TAPEDEV_SECT_STATUS_ERR_INVALID_TAPE_NO:
    return "NO";
  case TAPEDEV_SECT_STATUS_ERR_INVALID_FFWD_POS:
    return "POS";
  case TAPEDEV_SECT_STATUS_ERR_READ_PAST_END:
    return "END";
  case TAPEDEV_SECT_STATUS_ERR_WRITE_PAST_END:
    return "END";
  case TAPEDEV_SECT_STATUS_ERR_IO:
    return "IO";
  case TAPEDEV_SECT_STATUS_ERR_PGTABLE:
    return "PGTABLE";
  default:
    return "UNKNOWN";
  }
}

/**
 * @brief Maps the device status to a correct errno.
 */
static int tapedev_status_to_errno(u32 status) {
  switch (status) {
  case TAPEDEV_SECT_STATUS_DONE:
    return 0;
  case TAPEDEV_SECT_STATUS_ERR_TAPE_ACTIVE:
    return -EBUSY;
  case TAPEDEV_SECT_STATUS_ERR_NO_TAPE:
    return -ENOMEDIUM;
  case TAPEDEV_SECT_STATUS_ERR_INVALID_TAPE_NO:
  case TAPEDEV_SECT_STATUS_ERR_INVALID_FFWD_POS:
  case TAPEDEV_SECT_STATUS_ERR_INVALID_CMD:
    return -EINVAL;
  case TAPEDEV_SECT_STATUS_ERR_READ_PAST_END:
  case TAPEDEV_SECT_STATUS_ERR_WRITE_PAST_END:
    return -ENOSPC;
  case TAPEDEV_SECT_STATUS_ERR_PGTABLE:
    return -EFAULT;
  case TAPEDEV_SECT_STATUS_ERR_IO:
  case TAPEDEV_SECT_STATUS_ERR_RESET:
  default:
    return -EIO;
  }
}

static int tapedev_send_cmd(struct tapedev_section *sec, u32 cmd,
                            dma_addr_t buf_dma) {
  struct tapedev_device *dev = sec->device;
  unsigned long flags;
  int ret;
  u32 status;

  // One command per section according to the task description.
  mutex_lock(&sec->cmd_mutex);

  // Set the command to not done under spin lock ao it doesn't race with the
  // interrupt.
  spin_lock_irqsave(&dev->slock, flags);
  sec->cmd_done = false;
  spin_unlock_irqrestore(&dev->slock, flags);

  // Set the buffer pointer if needed.
  if (buf_dma)
    TDEV_SECTION_IOW(dev, sec->id, TAPEDEV_SECT_BUFFER_PTR_ADDR,
                     (u32)(buf_dma >> 9));

  DEBUG("send_cmd sec=%d cmd=0x%08x\n", sec->id, cmd);

  // Send the command to the device.
  TDEV_SECTION_IOW(dev, sec->id, TAPEDEV_SECT_CMD_ADDR, cmd);

  // Wait for completion of the command.
  ret = wait_event_timeout(sec->cmd_wq, sec->cmd_done, 5 * HZ);
  if (ret == 0) {
    DEBUG("send_cmd sec=%d TIMEOUT\n", sec->id);
    mutex_unlock(&sec->cmd_mutex);
    return -ETIMEDOUT;
  }

  // Read the command status.
  spin_lock_irqsave(&dev->slock, flags);
  status = sec->cmd_status;
  spin_unlock_irqrestore(&dev->slock, flags);

  DEBUG("send_cmd sec=%d statuc=0x%02x (%s)\n", sec->id, status,
        tapedev_status_name(status));
  mutex_unlock(&sec->cmd_mutex);
  return tapedev_status_to_errno(status);
}

// =================== COMMAND WRAPPERS ===================
static int tapedev_take_tape(struct tapedev_section *sec, u32 tape_number) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_TAKE_TAPE | (tape_number << 8), 0);
}
static int tapedev_eject_tape(struct tapedev_section *sec) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_EJECT_TAPE, 0);
}
static int tapedev_rewind(struct tapedev_section *sec) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_REWIND, 0);
}
static int tapedev_fast_fwd(struct tapedev_section *sec, u32 block_count) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_FAST_FWD | (block_count << 8), 0);
}
static int tapedev_read(struct tapedev_section *sec, u32 block_count,
                        u32 offset) {
  u32 cmd = TAPEDEV_CMD_READ | (block_count << 8) | (offset << 23);
  return tapedev_send_cmd(sec, cmd, sec->pgtable_dma);
}
static int tapedev_write(struct tapedev_section *sec, u32 block_count,
                         u32 offset) {
  u32 cmd = TAPEDEV_CMD_WRITE | (block_count << 8) | (offset << 23);
  return tapedev_send_cmd(sec, cmd, sec->pgtable_dma);
}

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
  DEBUG("tapedev_remove\n");
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