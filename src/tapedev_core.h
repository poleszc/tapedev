#ifndef TAPEDEV_CORE_H
#define TAPEDEV_CORE_H

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

#include "tapedev.h"
#include "tapedev_uapi.h"

// =================== DEBUG ===================
#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt, ...) printk(KERN_ALERT "[TAPEDEV]: " fmt, ##__VA_ARGS__)
#else
#define DEBUG(fmt, ...)
#endif

// =================== BASIC INFO ===================
#define TAPEDEV_NAME "tapedev"
#define TAPEDEV_MAX_DEVICES 256
#define TAPEDEV_MAX_SECTIONS 8
#define TAPEDEV_BUFFER_ALIGNEMENT 512

// =================== HARDWARE HANDLING ===================
#define TDEV_IOR(TAPE_DEV, REG) ioread32(TAPE_DEV->bar + REG)
#define TDEV_IOW(TAPE_DEV, REG, VAL) iowrite32(VAL, TAPE_DEV->bar + REG)
/// Sections are stored under (id+1)*0x100 since `tapedev` starts numbering
/// from 1.
#define TDEV_SECTION_OFFSET(SECT_ID, REG) ((SECT_ID + 1) * 0x100 + REG)
#define TDEV_SECTION_IOR(TAPE_DEV, SECT_ID, REG)                               \
  TDEV_IOR(TAPE_DEV, TDEV_SECTION_OFFSET(SECT_ID, REG))
#define TDEV_SECTION_IOW(TAPE_DEV, SECT_ID, REG, VAL)                          \
  TDEV_IOW(TAPE_DEV, TDEV_SECTION_OFFSET(SECT_ID, REG), VAL)
#define TDEV_SECTION_SIZE(TAPE_TYPE) ((32ULL) * BIT(TAPE_TYPE) * 8192u)

// =================== DEVICE STUCTURES ===================
// pci_dev (PCI card) -> tapedev_device (out device) ->
// tapedev_section[N] (single section in the device)

struct tapedev_device; // FD

/**
 * @brief Single tapedev section.
 */
struct tapedev_section {
  /// Pointer to the tapedev_device this section belongs to.
  struct tapedev_device *device;
  /// 0...7, section N from `/dev/tapedevXsN`
  int id;

  // === Command synchronization ===
  struct mutex cmd_mutex;
  // Mutex for performing ioctl calls and waiting for all reads/writes to end
  // before it.
  struct mutex io_mutex;
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

  // === Device state information ===
  /// The tape we're currently on. 0 means no tape.
  u32 current_tape;
  /// The position within the tape (in blocks).
  u64 current_pos;

  // === DMA ===
  void *pgtable;
  dma_addr_t pgtable_dma;

  // === Block Layer ===
  struct gendisk *gd;
  struct blk_mq_tag_set tag_set;
  struct scatterlist *sgl;
};

/**
 * @brief The whole device.
 */
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

// =================== SOME NEEDED EXPORTS ===================

/// Major number assigned by `register_blkdev()`
extern int tapedev_major;

int tapedev_init_section(struct tapedev_device *dev, int i);
void tapedev_free_section(struct tapedev_device *dev, int i);

void tapedev_fill_pgtable_from_sg(struct tapedev_section *sec, int *sg_idx,
                                  u32 *sg_offset, u32 target_bytes);
int tapedev_read(struct tapedev_section *sec, u32 block_count, u32 offset);
int tapedev_write(struct tapedev_section *sec, u32 block_count, u32 offset);
int tapedev_move_to(struct tapedev_section *sec, u32 tape, u64 pos);
int tapedev_eject_tape(struct tapedev_section *sec);

extern const struct attribute_group *tapedev_disk_groups[];
int tapedev_ioctl(struct block_device *bdev, blk_mode_t mode, unsigned int cmd,
                  unsigned long arg);

#endif