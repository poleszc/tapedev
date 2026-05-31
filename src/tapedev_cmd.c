#include "tapedev_core.h"

/**
 * @brief FIll the page table with a fragment of `sg_table`.
 * @p sec section
 * @p sg_idx index of the current segment in `sec->sgl`
 * @p sg_offset offset in bytes in the current segment
 * @p target_bytes how many bytes to consume from the `sg_table` (has to be
 * aligned to `block_size`)
 *
 * Starts from (*sg_idx, *sg_offset) and loops through segments filling the
 * `pgtable`. Updates the pointers for the next invokation of this function to
 * start where we have ended.
 *
 * This function can be invoked many times in a single request.
 */
void tapedev_fill_pgtable_from_sg(struct tapedev_section *sec, int *sg_idx,
                                  u32 *sg_offset, u32 target_bytes) {
  // DMA interprets it as little endian, so we ensure proper endianness here.
  // TODOP: This may not be needed since we're on x86_64.
  __le64 *pgt = sec->pgtable;

  int pgt_idx = 0;

  // Until we have bytes to consume and free space in the pg_table.
  while (target_bytes > 0 && pgt_idx < TAPEDEV_BUF_PGTABLE_ENTRIES) {
    struct scatterlist *sg = &sec->sgl[*sg_idx];

    // DMA address of this fragment. We add `sg_offset` since some part of it
    // may have been consumed by the previous iteration.
    dma_addr_t seg_addr = sg_dma_address(sg) + *sg_offset;
    // How much more bytes are left to consume in this segment.
    u32 seg_remaining = sg_dma_len(sg) - *sg_offset;

    // Bytes we are going to consume.
    u32 take = min(target_bytes, seg_remaining);
    u32 take_blocks = take / sec->block_size;

    u64 entry;

    // Sanity check.
    WARN_ON(!IS_ALIGNED(seg_addr, TAPEDEV_BUFFER_ALIGNEMENT));
    WARN_ON(take % sec->block_size);

    entry = ((u64)(seg_addr >> 9) << 32) | (u64)take_blocks;

    // Safe the entry in the page_table.
    pgt[pgt_idx++] = cpu_to_le64(entry);

    *sg_offset += take;
    target_bytes -= take;

    // If the whole segment was used, move to the next one.
    if (*sg_offset >= sg_dma_len(sg)) {
      (*sg_idx)++;
      *sg_offset = 0;
    }
  }
  WARN_ON(target_bytes != 0); // All should be read.
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

int tapedev_eject_tape(struct tapedev_section *sec) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_EJECT_TAPE, 0);
}

static int tapedev_rewind(struct tapedev_section *sec) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_REWIND, 0);
}

static int tapedev_fast_fwd(struct tapedev_section *sec, u32 block_count) {
  return tapedev_send_cmd(sec, TAPEDEV_CMD_FAST_FWD | (block_count << 8), 0);
}

int tapedev_read(struct tapedev_section *sec, u32 block_count, u32 offset) {
  u32 cmd = TAPEDEV_CMD_READ | (block_count << 8) | (offset << 23);
  return tapedev_send_cmd(sec, cmd, sec->pgtable_dma);
}

int tapedev_write(struct tapedev_section *sec, u32 block_count, u32 offset) {
  u32 cmd = TAPEDEV_CMD_WRITE | (block_count << 8) | (offset << 23);
  return tapedev_send_cmd(sec, cmd, sec->pgtable_dma);
}

/**
 * @brief Move the head to (tape, pos)
 * On success, `sec->current_tape == tape && sec->curreny_tape == tape`;
 * On fail, `sec->current_tape = 0`;
 */
int tapedev_move_to(struct tapedev_section *sec, u32 tape, u64 pos) {
  int err;

  // If needed, switch the tape.
  if (sec->current_tape != tape) {
    if (sec->current_tape != 0) {
      if ((err = tapedev_eject_tape(sec)))
        goto fail;
      sec->current_tape = 0;
    }

    if ((err = tapedev_take_tape(sec, tape)))
      goto fail;

    // Tape position may be from the previous eject. Rewind it.
    if ((err = tapedev_rewind(sec)))
      goto fail;

    sec->current_tape = tape;
    sec->current_pos = 0;
  }

  // Head is to far. Rewind it.
  if (sec->current_pos > pos) {
    if ((err = tapedev_rewind(sec)))
      goto fail;
    sec->current_pos = 0;
  }

  if (sec->current_pos < pos) {
    u64 diff = pos - sec->current_pos;
    if ((err = tapedev_fast_fwd(sec, diff)))
      goto fail;
    sec->current_pos = pos;
  }

  return 0;

fail:
  sec->current_tape = 0;
  sec->current_pos = 0;
  return err;
}
